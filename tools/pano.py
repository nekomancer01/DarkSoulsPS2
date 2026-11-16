"""Far panorama for map.c (render-distance variant A): the map beyond PANO_R, rendered offline, on a world-fixed cylinder.

  pano.py script <out.txt> X Y Z [shots] [--zoom Z]
      test script (src/testscript.c) for a MAP_PANO_R capture build: the player at (X, Y, Z), the camera turned through
      `shots` headings (default 16) in rows of pitch, one shot each (magenta clear: "option holes"). --zoom: for a build
      with -DPANO_ZOOM=Z (render.c focal length x Z): Z x the headings and pitch rows spaced to the narrower view.
  pano.py build <shot dir> <out pano.bin> X Y Z R [width] [--rows N] [--dsr <dir>]
      shots + their emulog.txt PANOCAM lines (camera basis) -> cylindrical panorama (width x N texels, default 128, uniform in
      azimuth and in tan(elevation)), cut into 128 x 128 tiles (texmap textures; empty ones left out) on a cylinder of radius R around
      (X, Y, Z): one chunk per tile. Writes <out>.png too (the panorama, magenta = transparent). --dsr: colours from
      DSR reference shots of the same headings (dsr-ref.sh cam --shot, JSON beside each), the mask from the capture.

pano.bin (little endian): u32 'PANO', chunks, chunk offset, textures, texture offset; chunks as map.bin's (64 B:
tex (pano texture index), flags, tris, pos / st / col offsets from pano.bin, min[3], max[3], origin[3], step); then
the texture section (texmap.write). Capture: the GS picture is 512 x 448, focal FX (PANOCAM) / 568.4, centre 256 / 224
(render.c); the window shot is cropped to the picture by its non-black bounding box (the magenta clear fills it)."""
import math
import os
import re
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import texmap  # noqa: E402

GS_W, GS_H, FY, FX0 = 512, 448, 568.4, 487.2  # FY / FX0: render.c focal lengths at PANO_ZOOM 1
ROWS = 128
TILE = 128  # texels: panorama tile (texture) size
SEG = 4  # cylinder segments per tile
PITCHES = (0.0, 0.45, -0.35)


def script(out, x, y, z, shots=16, zoom=1.0):
    L = ["option nodemo", "option noenemies", "option holes", f"spawn {x} {y} {z} 0", "cam 0 0", "wait 3",
         "freeze 1"]
    pitches = PITCHES
    if zoom != 1:  # rows 0.8 view heights apart from +0.5 down to -0.8 rad (tan -1.0..0.55 is about -0.79..0.5)
        step = 0.8 * 2 * math.atan(GS_H / 2 / (FY * zoom))
        pitches = [0.5 - i * step for i in range(int(math.ceil(1.3 / step)) + 1)]
        shots = int(round(shots * zoom))
    for r, pitch in enumerate(pitches):  # rows: level, looking down, looking up (the far map lies mostly below)
        for k in range(shots):
            yaw = 2 * math.pi * k / shots
            L += [f"cam {yaw:.4f} {pitch:.4f}", "wait 0.6", f"shot P{r}{k:02d}"]
    open(out, "w").write("\n".join(L + ["end"]) + "\n")


def cams(emulog):
    """{shot name: (eye, rt, up, fw, fx)} from the last PANOCAM line before each TEST shot"""
    out, last = {}, None
    for ln in open(emulog, errors="replace"):
        if "PANOCAM" in ln:
            v = [float(t) for t in ln.split("PANOCAM")[1].split()]
            last = (np.array(v[0:3]), np.array(v[3:6]), np.array(v[6:9]), np.array(v[9:12]), v[12])
        m = re.search(r"TEST shot (\S+)", ln)
        if m and last is not None:
            out[m.group(1)] = last
    return out


def picture(png):
    """window screenshot -> GS picture (RGB array, GS_H x GS_W)"""
    im = np.asarray(Image.open(png).convert("RGB"))
    body = im[24:-24]
    lit = body.max(2) > 24
    rows, cols = np.where(lit.any(1))[0], np.where(lit.any(0))[0]
    pic = Image.fromarray(body[rows[0]:rows[-1] + 1, cols[0]:cols[-1] + 1])
    return np.asarray(pic.resize((GS_W, GS_H), Image.NEAREST)).astype(np.int32)  # no blend into the magenta clear


def dsr_colours(dsr_dir, d):
    """colour per texel direction d from the DSR reference shots (tools/dsr-ref.sh cam --shot P<row><k>.png, its JSON
    beside it: camera matrix rows right / up / forward / position, vertical fov, aspect; 1920 x 1080), or None"""
    import json
    best = np.full(d.shape[:2], 1e9)
    out = np.zeros(d.shape[:2] + (3,), np.int32)
    for f in sorted(os.listdir(dsr_dir)):
        if not f.endswith(".json"):
            continue
        txt = open(os.path.join(dsr_dir, f)).read()
        if "{" not in txt or not os.path.exists(os.path.join(dsr_dir, f[:-5] + ".png")):
            continue
        c = json.loads(txt[txt.index("{"):])
        rt, up, fw = (np.array(c["matrix_rows"][i][:3]) for i in range(3))
        ty = math.tan(c["fov_rad"] / 2)
        tx = ty * c["aspect"]
        im = np.asarray(Image.open(os.path.join(dsr_dir, f[:-5] + ".png")).convert("RGB")).astype(np.int32)
        h, w = im.shape[:2]
        z = d @ fw
        x = (1 + (d @ rt) / np.maximum(z, 1e-6) / tx) * w / 2
        y = (1 - (d @ up) / np.maximum(z, 1e-6) / ty) * h / 2
        ok = (z > 0.1) & (x >= 2) & (x < w - 2) & (y >= 2) & (y < h - 2)
        score = np.abs(x - w / 2) / w + np.abs(y - h / 2) / h
        use = ok & (score < best)
        out[use] = im[np.clip(y.astype(int), 0, h - 1)[use], np.clip(x.astype(int), 0, w - 1)[use]]
        best[use] = score[use]
    return out, best < 1e8


def stitch(shot_dir, width, dsr_dir=None, rows=ROWS):
    cs = cams(os.path.join(shot_dir, "emulog.txt"))
    pics = {n: picture(os.path.join(shot_dir, n + ".png")) for n in cs if os.path.exists(os.path.join(shot_dir, n + ".png"))}
    names = sorted(pics)
    # elevation range: tan from -0.45 to +0.45 (about +-24 degrees), the rows actually holding map are kept below
    H0 = max(1024, 3 * rows)
    t = np.linspace(0.7, -1.0, H0)
    a = np.arange(width) * 2 * math.pi / width
    A, T = np.meshgrid(a, t)
    d = np.stack([np.sin(A), T, np.cos(A)], -1).astype(np.float32)  # direction per texel (not normalised: projection is scale free)
    best = np.full(A.shape, 1e9)
    out = np.zeros(A.shape + (3,), np.int32)
    for n in names:
        eye, rt, up, fw, fx = cs[n]
        z = d @ fw
        x = GS_W / 2 + fx * (d @ rt) / np.maximum(z, 1e-6)
        y = GS_H / 2 - FY * fx / FX0 * (d @ up) / np.maximum(z, 1e-6)  # FY scales with PANO_ZOOM like fx
        ok = (z > 0.1) & (x >= 2) & (x < GS_W - 2) & (y >= 2) & (y < GS_H - 2)
        score = np.abs(x - GS_W / 2) / GS_W + np.abs(y - GS_H / 2) / GS_H
        use = ok & (score < best)
        xi, yi = np.clip(x.astype(int), 0, GS_W - 1), np.clip(y.astype(int), 0, GS_H - 1)
        out[use] = pics[n][yi[use], xi[use]]
        best[use] = score[use]
    # the magenta clear, and texels the window's scaling blended with it (purple fringes on silhouettes)
    r, g, b = out[..., 0], out[..., 1], out[..., 2]
    mag = (r > g + 70) & (b > g + 70) | (best > 1e8)
    if dsr_dir:  # DSR's own rendering (its fog, light, full detail) where the capture found map beyond the radius
        dc, have = dsr_colours(dsr_dir, d)
        out = np.where(have[..., None], dc, out)
    hit = np.where((~mag).any(1))[0]
    if not len(hit):
        raise SystemExit("pano: nothing beyond the radius in the shots")
    r0, r1 = max(0, hit[0] - 2), min(H0 - 1, hit[-1] + 2)
    t0, t1 = t[r0], t[r1]  # top / bottom tan(elevation)
    rgba = np.zeros((r1 - r0 + 1, width, 4), np.uint8)
    rgba[..., :3] = np.clip(out[r0:r1 + 1], 0, 255)
    rgba[..., 3] = np.where(mag[r0:r1 + 1], 0, 255)
    # colour box-filtered down to `rows` (the stitch grid is H0 rows) over the opaque texels only (no magenta
    # fringe), the cut-out mask nearest
    w = (rgba[..., 3] > 0).astype(np.float32)
    box = lambda a: np.asarray(Image.fromarray(a, "F").resize((width, rows), Image.BOX))
    ws = np.maximum(box(w), 1e-6)
    rgb = np.stack([box(rgba[..., i] * w) / ws for i in range(3)], -1)
    alpha = np.asarray(Image.fromarray(rgba[..., 3], "L").resize((width, rows), Image.NEAREST))
    img = Image.fromarray(np.dstack([np.clip(rgb + 0.5, 0, 255).astype(np.uint8), alpha]), "RGBA")
    return img, t0, t1


GRID = 8  # quads across a tile of a depth-mapped layer (16 texels, ~5.6 degrees at 1024 texels round)


def _grid_depth(D, x0, y0):
    """the (GRID + 1)^2 corner depths of the tile at texel (x0, y0) of depth map D (inf = empty; wraps in x): the
    median of the covered texels within half a patch of each corner, corners without any from their nearest neighbour
    with one (so neighbouring tiles agree on their shared corners: both read the same texels)"""
    rows, W = D.shape
    st = TILE // GRID
    out = np.full((GRID + 1, GRID + 1), np.nan)
    for gy in range(GRID + 1):
        for gx in range(GRID + 1):
            for grow in (1, 2, 4, 8, 16):  # the window grows until it holds something
                h = st * grow // 2
                ys = np.arange(max(0, y0 + gy * st - h), min(rows, y0 + gy * st + h))
                xs = np.arange(x0 + gx * st - h, x0 + gx * st + h) % W
                w = D[np.ix_(ys, xs)]
                w = w[np.isfinite(w)]
                if len(w):
                    out[gy, gx] = np.median(w)
                    break
    out[np.isnan(out)] = np.nanmedian(out) if np.isfinite(out).any() else 100.0
    return out


def _chunk(k_tex, P, uv):
    """triangle list P (corner positions) with uv -> a chunk tuple (texture, tris, pos, st, col, min, max, org, step)"""
    mn = [min(p[j] for p in P) for j in range(3)]
    mx = [max(p[j] for p in P) for j in range(3)]
    org = [float(round((mn[j] + mx[j]) / 2)) for j in range(3)]
    e = math.ceil(math.log2(max(max(abs(p[j] - org[j]) for p in P for j in range(3)), 1e-3) / 32767))
    pos = bytearray()
    for p in P:
        pos += struct.pack("<3h", *[int(round((p[j] - org[j]) / 2.0 ** e)) for j in range(3)])
    stb = bytearray()
    for u, v in uv:
        stb += struct.pack("<2H", min(65535, round(u * 4096)), min(65535, round(v * 4096)))
    return (k_tex, len(P) // 3, bytes(pos), bytes(stb), bytes([0x80, 0x80, 0x80, 0x80]) * len(P), mn, mx, org, 2.0 ** e)


def write(out, layers, shown=b""):
    """layers: [(RGBA image (width x rows texels, uniform in azimuth and tan(elevation)), tan range (top, bottom),
    centre (x, y, z), radius)], far to near: 128 x 128 tiles (texmap textures; empty ones left out) on world-fixed
    cylinders, one chunk per tile -> pano.bin. shown: the draw-group sets the picture stands for (a bit per dg.bin
    piece mask index, 32 bytes at byte 32 of the header; map.c leaves their horizon chunks out while it is drawn)."""
    # 128 x 128 tiles, the largest map texture shape (texmap: at most 128 high; 256-wide 8-bit tiles drew garbage
    # through the texture cache)
    textures, chunks = [], []
    for img, (t0, t1), (cx, cy, cz), R in layers:
        width, rows = img.size
        tiles, bands = width // TILE, max(1, rows // TILE)
        for k, b in ((k, b) for b in range(bands) for k in range(tiles)):
            tile = img.crop((TILE * k, TILE * b, TILE * (k + 1), TILE * (b + 1)))
            px = np.asarray(tile).copy()
            if not px[..., 3].any():
                continue  # nothing in this tile
            px[..., 3] = np.where(px[..., 3] > 0, 255, 0)  # texmap: alpha >= 128 opaque, else the transparent index
            tb0, tb1 = t0 + (t1 - t0) * b / bands, t0 + (t1 - t0) * (b + 1) / bands  # uniform in tan(elevation)
            k_tex = len(textures)
            textures.append(texmap.encode(TILE, TILE, px.tobytes(), name=f"pano{k_tex}"))
            if not np.isscalar(R):  # a depth map: the tile as a GRID x GRID sheet at the depth seen, in VU1 batches
                dv = _grid_depth(R, TILE * k, TILE * b)
                quads = []
                for gy in range(GRID):
                    for gx in range(GRID):
                        if not px[gy * TILE // GRID:(gy + 1) * TILE // GRID, gx * TILE // GRID:(gx + 1) * TILE // GRID, 3].any():
                            continue  # (an empty patch of the tile: nothing to draw)
                        quads.append([(gx + dx, gy + dy) for dx, dy in ((0, 0), (1, 0), (1, 1), (0, 0), (1, 1), (0, 1))])
                for q0 in range(0, len(quads), 24):  # 48 triangles a chunk (map.c CHUNK_HZN_TRIS / the VU1 buffer: 54)
                    P, uv = [], []
                    for q in quads[q0:q0 + 24]:
                        for gx, gy in q:
                            a = 2 * math.pi * (k + gx / GRID) / tiles
                            tt = tb0 + (tb1 - tb0) * gy / GRID
                            r = float(dv[gy, gx])
                            P.append((cx + r * math.sin(a), cy + r * tt, cz + r * math.cos(a)))
                            uv.append((gx / GRID, gy / GRID))
                    chunks.append(_chunk(k_tex, P, uv))
                continue
            uv, P = [], []
            for s in range(SEG):
                u0, u1 = s / SEG, (s + 1) / SEG
                a0, a1 = 2 * math.pi * (k + u0) / tiles, 2 * math.pi * (k + u1) / tiles
                q = [(a0, tb0, u0, 0.0), (a1, tb0, u1, 0.0), (a1, tb1, u1, 1.0), (a0, tb1, u0, 1.0)]
                for i in (0, 1, 2, 0, 2, 3):
                    a, tt, u, v = q[i]
                    P.append((cx + R * math.sin(a), cy + R * tt, cz + R * math.cos(a)))
                    uv.append((u, v))
            chunks.append(_chunk(k_tex, P, uv))
    HDR = 64
    blob = bytearray()
    table = HDR
    blob += b"\0" * (64 * len(chunks))
    recs = []
    for k, n, pos, st, col, mn, mx, org, step in chunks:
        offs = []
        for arr in (pos, st, col):
            blob.extend(b"\0" * ((-(len(blob) + HDR)) % 16))
            offs.append(len(blob) + HDR)
            blob += arr + b"\0" * (-len(arr) % 16)
        recs.append(struct.pack("<iII3I6f4f", k, 0, n, *offs, *mn, *mx, *org, step))
    blob[0:64 * len(recs)] = b"".join(recs)
    # texmap.write: density per texture from the chunks (texture, ..., min, max) like map.bin's
    tchunks = [(k, 0, n, pos, st, col, mn, mx, org, step, 4096) for k, n, pos, st, col, mn, mx, org, step in chunks]
    tex_off = texmap.write(blob, HDR, textures, tchunks)
    head = b"PANO" + struct.pack("<4I", len(chunks), table, len(textures), tex_off)
    data = head + b"\0" * (32 - len(head)) + bytes(shown).ljust(32, b"\0")[:32] + bytes(blob)
    open(out, "wb").write(data)
    return len(textures), sum(c[1] for c in chunks), len(data)


def build(shot_dir, out, cx, cy, cz, R, width=2048, dsr_dir=None, rows=ROWS):
    img, t0, t1 = stitch(shot_dir, width, dsr_dir, rows)
    img.save(out + ".png")
    nt, tris, size = write(out, [(img, (t0, t1), (cx, cy, cz), R)])
    print(f"pano: {width} x {rows} texels, tan(elevation) {t1:.3f}..{t0:.3f}, {nt} tiles, "
          f"{tris} tris, {size >> 10} KB -> {out}")


# ---- bake: a block's far backdrop as picture layers, rendered offline (no capture build) ----
BAKE_W = int(os.environ.get("PANO_W", 1024))          # texels round the cylinder (0.35 degrees each, ~3 screen pixels)
BAKE_LAYERS = int(os.environ.get("PANO_LAYERS", 2))   # depth layers at most
BAKE_SPLIT = float(os.environ.get("PANO_SPLIT", 1.4))   # depth ratio between two layers at least
BAKE_TAN = 1.0                                          # 45 degrees up and down at most


def _render(chunks, tex, eye, W, tans):
    """chunks (mapshot tuples) textured, unfogged, from eye -> (rgba uint8 (rows, W, 4), horizontal distance (rows, W))
    on a cylinder uniform in azimuth and tan(elevation) from tans[0] (top) to tans[1]"""
    import tempfile
    import mapview
    n = sum(len(c[0]) for c in chunks)
    geo = (np.concatenate([c[0] for c in chunks]), np.concatenate([c[1] for c in chunks]),
           np.concatenate([c[2] for c in chunks]), np.concatenate([np.full(len(c[0]), c[3]) for c in chunks]),
           np.concatenate([np.full(len(c[0]), c[4] & ~8) for c in chunks]), np.zeros(n),
           np.concatenate([np.tile(c[5], (len(c[0]), 1)) for c in chunks]))
    NV, S, FOV, K = 6, 1536, 100.0, 2
    f = S / 2 / math.tan(math.radians(FOV) / 2)
    rows = int(math.ceil((tans[0] - tans[1]) / (2 * math.pi / W) / TILE)) * TILE
    t1 = tans[0] - rows * (2 * math.pi / W)  # (square texels at the horizon; the bottom rounded down to whole tiles)
    az = (np.arange(W * K) + 0.5) / (W * K) * 2 * math.pi
    tn = tans[0] + (np.arange(rows * K) + 0.5) / (rows * K) * (t1 - tans[0])
    A, T = np.meshgrid(az, tn)
    D = np.stack([np.sin(A), T, np.cos(A)], -1)  # (a ray at horizontal distance 1)
    rgb, dist = np.zeros(A.shape + (3,), np.float32), np.full(A.shape, np.inf, np.float32)
    vi = np.rint(A / (2 * math.pi / NV)).astype(np.int64) % NV
    with tempfile.TemporaryDirectory() as tmp:
        for k in range(NV):
            yaw = 2 * math.pi * k / NV
            fwd = np.array([math.sin(yaw), 0.0, math.cos(yaw)])
            z = []
            mapview.render_view(os.path.join(tmp, "v.png"), geo, tex, tuple(eye), tuple(eye + fwd), W=S, H=S, fov_y=FOV, zout=z)
            img = np.asarray(Image.open(os.path.join(tmp, "v.png")).convert("RGB"), np.float32)
            m = vi == k
            right = np.array([fwd[2], 0.0, -fwd[0]])
            dz = D[m] @ fwd
            x = np.clip(np.floor(S / 2 + f * (D[m] @ right) / dz).astype(np.int64), 0, S - 1)
            y = np.clip(np.floor(S / 2 - f * D[m][:, 1] / dz).astype(np.int64), 0, S - 1)
            dist[m] = z[0][y, x] / dz  # (depth along the view axis -> horizontal distance)
            rgb[m] = img[y, x]
    ok = np.isfinite(dist)
    blk = lambda a: a.reshape(rows, K, W, K, *a.shape[2:]).swapaxes(1, 2).reshape(rows, W, K * K, *a.shape[2:])
    okc, cc = blk(ok), blk(np.where(ok[..., None], rgb, 0.0))
    cnt = okc.sum(2)
    out = np.zeros((rows, W, 4), np.uint8)
    out[..., :3] = np.clip(np.rint(cc.sum(2) / np.maximum(cnt, 1)[..., None]), 0, 255)
    out[..., 3] = np.where(cnt * 2 >= K * K, 255, 0)
    import warnings
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)  # (texels that show nothing: all NaN)
        dep = np.nanmedian(blk(np.where(ok, dist, np.nan)), 2)
    return out, (tans[0], t1), np.where(out[..., 3] > 0, dep, np.inf)


def bake(gen, out):
    """<gen dir>/map.bin's far backdrop (CHUNK_BACKDROP: the game's own distant pieces, never reached) -> pano.bin:
    DSR draws them textured at full detail. The port never loads them (1.6 MB in Firelink Shrine: the horizon's
    untextured stand-in is drawn, the ring walls from the graveyard as flat facets, Ame 2026-11-14). Baked instead:
    the triangles horizon.py leaves out of the horizon for it (pano_tris: far from the walkable ground, in draw-group
    sets most of the block's floor shows), in up to PANO_LAYERS depth layers, each rendered with its textures and baked light from the middle of the walkable
    ground onto a world-fixed cylinder at the layer's distance (the layers shift against each other as the player
    walks). An empty file when there is nothing to bake."""
    sys.path.insert(0, HERE)
    import horizon
    import mapchunks
    import mapcfg
    import mapshot
    b = mapshot.Block(gen)
    src = os.path.join(gen, "map.bin")
    nav = horizon.own_nav(open(src, "rb").read(), src)
    use, n_bd = [], 0
    for i, c in enumerate(b.res):
        if c[4] & mapcfg.CHUNK_BACKDROP and not c[4] & (mapcfg.CHUNK_SKY | mapcfg.CHUNK_BLEND | mapchunks.CHUNK_GRASS) and len(c[0]):
            n_bd += len(c[0])
            # horizon.py's rule, triangle by triangle: these leave the horizon, the rest stay its geometry
            m = horizon.pano_tris(c[0].mean(1).astype(np.float64), np.full(len(c[0]), b.chunk_dg(i)), np.ones(len(c[0]), bool), nav, src)
            if m.any():
                use.append((c[0][m], c[1][m], c[2][m], c[3], c[4], c[5]))
    if not use or not len(nav) or os.environ.get("HZN_PANO", "1") != "1":
        write(out, [])
        print(f"pano: {os.path.basename(os.path.normpath(gen))}: no far backdrop to bake")
        return
    eye = nav.mean(0) + np.array([0.0, 1.5, 0.0])
    cen = np.concatenate([c[0].mean(1) for c in use])
    hd = np.sqrt((cen[:, 0] - eye[0]) ** 2 + (cen[:, 2] - eye[2]) ** 2)
    tn = (np.concatenate([c[0].reshape(-1, 3) for c in use])[:, 1] - eye[1]) / \
        np.maximum(np.sqrt(((np.concatenate([c[0].reshape(-1, 3) for c in use])[:, [0, 2]] - eye[[0, 2]]) ** 2).sum(1)), 1.0)
    tans = (min(BAKE_TAN, float(tn.max()) + 0.02), max(-BAKE_TAN, float(tn.min()) - 0.02))
    # layers: chunks by the distance of their middle, split where the sorted distances jump most (in ratio)
    cd = np.array([math.sqrt((c[0].mean((0, 1))[0] - eye[0]) ** 2 + (c[0].mean((0, 1))[2] - eye[2]) ** 2) for c in use])
    cuts = []
    for _ in range(max(0, BAKE_LAYERS - 1)):
        lo = np.sort(cd)
        w = np.array([len(c[0]) for c in use])[np.argsort(cd)].cumsum() / sum(len(c[0]) for c in use)
        best = None
        for q in np.linspace(0.2, 0.8, 25):
            d0 = lo[np.searchsorted(w, q)]
            if any(abs(math.log(d0 / c)) < math.log(BAKE_SPLIT) for c in cuts):
                continue
            near_d, far_d = np.median(cd[cd < d0]) if (cd < d0).any() else 0, np.median(cd[cd >= d0])
            if near_d and far_d / near_d >= BAKE_SPLIT and (best is None or far_d / near_d > best[0]):
                best = (far_d / near_d, d0)
        if best:
            cuts.append(best[1])
    edges = [0.0] + sorted(cuts) + [1e9]
    layers, radii = [], []
    for a, e in reversed(list(zip(edges[:-1], edges[1:]))):  # far to near
        cs = [c for c, d in zip(use, cd) if a <= d < e]
        if not cs:
            continue
        px, tr, dep = _render(cs, b.tex, eye, BAKE_W, tans)
        layers.append((Image.fromarray(px, "RGBA"), tr, tuple(eye), dep))
        radii.append(float(np.median(dep[np.isfinite(dep)])) if np.isfinite(dep).any() else 0.0)
    nt, tris, size = write(out, layers)
    print(f"pano: {os.path.basename(os.path.normpath(gen))}: {sum(len(c[0]) for c in use)} backdrop tris of "
          f"{n_bd} beyond {horizon.PANO_D:.0f} m -> {len(layers)} layers at {', '.join(f'{r:.0f} m' for r in radii)}, {nt} tiles of {TILE} px "
          f"({nt * 17} KB), {tris} tris, {size >> 10} KB")


if __name__ == "__main__":
    if sys.argv[1] == "script":
        zoom = float(sys.argv[sys.argv.index("--zoom") + 1]) if "--zoom" in sys.argv else 1.0
        args = [a for i, a in enumerate(sys.argv) if a != "--zoom" and sys.argv[i - 1] != "--zoom"]
        script(args[2], *map(float, args[3:6]), *(int(v) for v in args[6:7]), zoom=zoom)
    elif sys.argv[1] == "bake":
        bake(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == "build":
        opt = {k: sys.argv[sys.argv.index(k) + 1] for k in ("--dsr", "--rows") if k in sys.argv}
        args = [a for i, a in enumerate(sys.argv) if a not in opt and (i == 0 or sys.argv[i - 1] not in opt)]
        build(args[2], args[3], *map(float, args[4:8]), *(int(v) for v in args[8:9]), dsr_dir=opt.get("--dsr"),
              rows=int(opt.get("--rows", ROWS)))
    else:
        raise SystemExit(__doc__)
