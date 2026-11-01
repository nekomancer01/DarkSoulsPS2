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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
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


def build(shot_dir, out, cx, cy, cz, R, width=2048, dsr_dir=None, rows=ROWS):
    img, t0, t1 = stitch(shot_dir, width, dsr_dir, rows)
    img.save(out + ".png")
    # 128 x 128 tiles, the largest map texture shape (texmap: at most 128 high; 256-wide 8-bit tiles drew garbage
    # through the texture cache)
    tiles, bands = width // TILE, max(1, rows // TILE)
    textures, chunks = [], []
    for k, b in ((k, b) for b in range(bands) for k in range(tiles)):
        tile = img.crop((TILE * k, TILE * b, TILE * (k + 1), TILE * (b + 1)))
        px = np.asarray(tile).copy()
        if not px[..., 3].any():
            continue  # nothing beyond the radius in this tile
        px[..., 3] = np.where(px[..., 3] > 0, 255, 0)  # texmap: alpha >= 128 opaque, else the transparent index
        tb0, tb1 = t0 + (t1 - t0) * b / bands, t0 + (t1 - t0) * (b + 1) / bands  # uniform in tan(elevation)
        k_tex = len(textures)
        textures.append(texmap.encode(TILE, TILE, px.tobytes(), name=f"pano{k_tex}"))
        pos, uv, P = bytearray(), [], []
        for s in range(SEG):
            u0, u1 = s / SEG, (s + 1) / SEG
            a0, a1 = 2 * math.pi * (k + u0) / tiles, 2 * math.pi * (k + u1) / tiles
            q = [(a0, tb0, u0, 0.0), (a1, tb0, u1, 0.0), (a1, tb1, u1, 1.0), (a0, tb1, u0, 1.0)]
            for i in (0, 1, 2, 0, 2, 3):
                a, tt, u, v = q[i]
                P.append((cx + R * math.sin(a), cy + R * tt, cz + R * math.cos(a)))
                uv.append((u, v))
        mn = [min(p[j] for p in P) for j in range(3)]
        mx = [max(p[j] for p in P) for j in range(3)]
        org = [float(round((mn[j] + mx[j]) / 2)) for j in range(3)]
        e = math.ceil(math.log2(max(max(abs(p[j] - org[j]) for p in P for j in range(3)), 1e-3) / 32767))
        for p in P:
            pos += struct.pack("<3h", *[int(round((p[j] - org[j]) / 2.0 ** e)) for j in range(3)])
        stb = bytearray()
        for u, v in uv:
            stb += struct.pack("<2H", min(65535, round(u * 4096)), min(65535, round(v * 4096)))
        col = bytes([0x80, 0x80, 0x80, 0x80]) * len(P)
        chunks.append((k_tex, len(P) // 3, bytes(pos), bytes(stb), col, mn, mx, org, 2.0 ** e))
    HDR = 32
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
    data = head + b"\0" * (HDR - len(head)) + bytes(blob)
    open(out, "wb").write(data)
    print(f"pano: {width} x {rows} texels, tan(elevation) {t1:.3f}..{t0:.3f}, {len(textures)} tiles, "
          f"{sum(c[1] for c in chunks)} tris, {len(data) >> 10} KB -> {out}")


if __name__ == "__main__":
    if sys.argv[1] == "script":
        zoom = float(sys.argv[sys.argv.index("--zoom") + 1]) if "--zoom" in sys.argv else 1.0
        args = [a for i, a in enumerate(sys.argv) if a != "--zoom" and sys.argv[i - 1] != "--zoom"]
        script(args[2], *map(float, args[3:6]), *(int(v) for v in args[6:7]), zoom=zoom)
    elif sys.argv[1] == "build":
        opt = {k: sys.argv[sys.argv.index(k) + 1] for k in ("--dsr", "--rows") if k in sys.argv}
        args = [a for i, a in enumerate(sys.argv) if a not in opt and (i == 0 or sys.argv[i - 1] not in opt)]
        build(args[2], args[3], *map(float, args[4:8]), *(int(v) for v in args[8:9]), dsr_dir=opt.get("--dsr"),
              rows=int(opt.get("--rows", ROWS)))
    else:
        raise SystemExit(__doc__)
