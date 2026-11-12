"""Offline renders of an exported map.bin (export_map.py --render) (2026-11-12, code moved as it was; tools/export_map.py runs it)."""
import os, sys
import math, struct
import texmap
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import export_map
import mapcfg
import mapchunks


def load_map_bin(path):
    """map.bin -> (triangles: pos (N,3,3), st (N,3,2), rgba (N,3,4), texture (N), flags (N), foliage rank (N, 0 for
    other triangles: the tuft's rank estimated from its place in the chunk), chunk box (N,6);
    textures [(w, h, palette (256,4) rgb + alpha 0..0x80, indices (h,w))]; spawn (x, y, z, yaw); fog (begin, end, max
    density, rgb, sky rgb, sky density 0..1))."""
    import numpy as np
    b = open(path, "rb").read()
    _, nch, ntex = struct.unpack_from("<4s2I", b, 0)
    spawn = struct.unpack_from("<4f", b, 20)
    och, otex = struct.unpack_from("<2I", b, 36)
    P, ST, C, TI, FL, R, BX = [], [], [], [], [], [], []
    for i in range(nch):
        ti, fl, n, po, so, co = struct.unpack_from("<iII3I", b, och + 64 * i)
        n, nv = n & 0xFFFF, n >> 16
        BX.append(np.tile(np.array(struct.unpack_from("<6f", b, och + 64 * i + 24)), (n, 1)))
        org, step = np.array(struct.unpack_from("<3f", b, och + 64 * i + 48)), struct.unpack_from("<f", b, och + 64 * i + 60)[0]
        if fl & mapchunks.CHUNK_GRASS:
            per = max(1, (fl >> 8) & 255)
            lo, hi = (fl >> 16 & 255) / 255, (fl >> 24) / 255
            R.append(lo + (hi - lo) * (np.arange(n) // per + 0.5) / max(1, n // per))
        else:
            R.append(np.zeros(n))
        if nv:  # indexed chunk: shared vertices, then the triangle stream (u8 x4) after the colours' code qword
            ix = np.frombuffer(b, np.uint8, n * 4, co + -(-4 * nv // 16) * 16 + 16).reshape(n, 4)[:, :3]
            pv = np.frombuffer(b, np.int16, nv * 3, po).reshape(nv, 3)[ix]
            sv = np.frombuffer(b, np.uint16, nv * 2, so).reshape(nv, 2)[ix]
            cv = np.frombuffer(b, np.uint8, nv * 4, co).reshape(nv, 4)[ix]
        else:
            pv = np.frombuffer(b, np.int16, n * 9, po).reshape(n, 3, 3)
            sv = np.frombuffer(b, np.uint16, n * 6, so).reshape(n, 3, 2)
            cv = np.frombuffer(b, np.uint8, n * 12, co).reshape(n, 3, 4)
        P.append(org + step * pv.astype(np.float32))
        ST.append(sv / (1024.0 if fl & mapcfg.CHUNK_ST1K else 4096.0))
        C.append(cv)
        TI.append(np.full(n, ti))
        FL.append(np.full(n, fl))
    tex = texmap.load(b, ntex, otex)
    fb, fe, fm = struct.unpack_from("<3f", b, 52)
    fc, sc = struct.unpack_from("<4B", b, 64), struct.unpack_from("<4B", b, 68)
    fog = (fb, fe, fm, fc[:3], sc[:3], sc[3] / 255.0)
    return (np.concatenate(P), np.concatenate(ST), np.concatenate(C), np.concatenate(TI), np.concatenate(FL),
            np.concatenate(R), np.concatenate(BX)), \
        tex, spawn, fog


def render_view(path, geo, tex, eye, target, W=960, H=540, fov_y=43.0, fog=None, zout=None):
    """Z-buffered, perspective-correct textured software render like the PS2: CLUT textures x vertex light, alpha
    test on opaque chunks, back faces culled where the chunk flag says so, near plane 0.1 m; blended chunks after the
    opaque ones, far to near per pixel (alpha or additive, Z tested, not written); fog from load_map_bin's fog (sky
    chunks: the sky's constant fog)."""
    import numpy as np
    from PIL import Image
    P, ST, C, TI, FL, R, BX = geo
    eye = np.array(eye, np.float64)
    # foliage density by chunk distance, like map.c
    d = np.sqrt((np.maximum(0, np.maximum(BX[:, :3] - eye, eye - BX[:, 3:])) ** 2).sum(1))
    show = ~(FL & mapchunks.CHUNK_GRASS).astype(bool) | (R < np.minimum(1, (mapchunks.GRASS_D0 / np.maximum(d, 1e-3)) ** 2))
    P, ST, C, TI, FL = P[show], ST[show], C[show], TI[show], FL[show]
    fwd = np.array(target, np.float64) - eye
    fwd /= np.linalg.norm(fwd)
    right = np.array([fwd[2], 0.0, -fwd[0]])
    right /= np.linalg.norm(right)
    up = np.cross(fwd, right)
    f = H / 2 / math.tan(math.radians(fov_y) / 2)
    d = P.astype(np.float64) - eye
    V = np.stack([d @ right, d @ up, d @ fwd], -1)  # (N,3,3) camera space
    near = 0.1
    UV = ST.astype(np.float64)
    COL = C.astype(np.float64) / 128
    front = (V[..., 2] > near)
    allin = front.all(1)
    # triangles crossing the near plane: clip (Sutherland-Hodgman) and fan
    ext_v, ext_uv, ext_c, ext_i = [], [], [], []
    for i in np.where(front.any(1) & ~allin)[0]:
        poly = []
        for k in range(3):
            a, b = k, (k + 1) % 3
            za, zb = V[i, a, 2], V[i, b, 2]
            if za > near:
                poly.append((V[i, a], UV[i, a], COL[i, a]))
            if (za > near) != (zb > near):
                t = (near - za) / (zb - za)
                poly.append(tuple(x[a] + t * (x[b] - x[a]) for x in (V[i], UV[i], COL[i])))
        for k in range(1, len(poly) - 1):
            tri = (poly[0], poly[k], poly[k + 1])
            ext_v.append([x[0] for x in tri])
            ext_uv.append([x[1] for x in tri])
            ext_c.append([x[2] for x in tri])
            ext_i.append(i)
    idx = np.concatenate([np.where(allin)[0], np.array(ext_i, dtype=np.int64)])
    V = np.concatenate([V[allin], np.array(ext_v).reshape(-1, 3, 3)])
    UV = np.concatenate([UV[allin], np.array(ext_uv).reshape(-1, 3, 2)])
    COL = np.concatenate([COL[allin], np.array(ext_c).reshape(-1, 3, 4)])
    TI, FL = TI[idx], FL[idx]
    sx = W / 2 + f * V[..., 0] / V[..., 2]
    sy = H / 2 - f * V[..., 1] / V[..., 2]
    cr = (sx[:, 1] - sx[:, 0]) * (sy[:, 2] - sy[:, 0]) - (sy[:, 1] - sy[:, 0]) * (sx[:, 2] - sx[:, 0])
    keep = (np.abs(cr) > 1e-9) & ~((FL & 1).astype(bool) & (cr > 0))  # negative screen cross = front (DS winding)
    x0, x1 = np.floor(sx.min(1)), np.ceil(sx.max(1))
    y0, y1 = np.floor(sy.min(1)), np.ceil(sy.max(1))
    keep &= (x1 >= 0) & (x0 < W) & (y1 >= 0) & (y0 < H)
    x0, y0 = np.maximum(x0, 0), np.maximum(y0, 0)
    x1, y1 = np.minimum(x1, W), np.minimum(y1, H)
    size = np.maximum(x1 - x0, y1 - y0)
    # textures in one flat index array + palettes (rgb, alpha 0..0x80)
    offs, flat, pal = [], [], np.zeros((len(tex) + 1, 256, 4), np.float32)
    o = 0
    for k, (w, h, p, ix) in enumerate(tex):
        offs.append(o)
        flat.append(ix.ravel())
        o += w * h
        pal[k] = p
    flat = np.concatenate(flat) if flat else np.zeros(1, np.uint8)
    tw = np.array([t[0] for t in tex] + [1])
    th = np.array([t[1] for t in tex] + [1])
    offs = np.array(offs + [0])

    def fragments(mask):
        """(pixel, z, rgb, alpha 0..1, triangle flags) for the triangles in mask, in size buckets"""
        s = 1
        while True:
            sel = np.where(mask & (size <= s) & ((size > s // 2) if s > 1 else True))[0]
            if len(sel):
                oy, ox = np.mgrid[0:s, 0:s]
                ox, oy = ox.ravel(), oy.ravel()
                step = max(1, 2_000_000 // (s * s))
                for c0 in range(0, len(sel), step):
                    t = sel[c0:c0 + step]
                    px = (x0[t, None] + ox[None, :])
                    py = (y0[t, None] + oy[None, :])
                    cx, cy = px + 0.5, py + 0.5
                    ax, ay = sx[t], sy[t]
                    l0 = ((ax[:, 1, None] - cx) * (ay[:, 2, None] - cy) - (ay[:, 1, None] - cy) * (ax[:, 2, None] - cx)) / cr[t, None]
                    l1 = ((ax[:, 2, None] - cx) * (ay[:, 0, None] - cy) - (ay[:, 2, None] - cy) * (ax[:, 0, None] - cx)) / cr[t, None]
                    l2 = 1 - l0 - l1
                    m = (l0 >= 0) & (l1 >= 0) & (l2 >= 0) & (px < x1[t, None]) & (py < y1[t, None])
                    ti_, fi = np.nonzero(m)
                    if not len(ti_):
                        continue
                    tt = t[ti_]
                    b0, b1, b2 = l0[ti_, fi], l1[ti_, fi], l2[ti_, fi]
                    iz = V[tt, 0, 2] ** -1 * b0 + V[tt, 1, 2] ** -1 * b1 + V[tt, 2, 2] ** -1 * b2
                    w0, w1, w2 = b0 / V[tt, 0, 2] / iz, b1 / V[tt, 1, 2] / iz, b2 / V[tt, 2, 2] / iz
                    u = UV[tt, 0, 0] * w0 + UV[tt, 1, 0] * w1 + UV[tt, 2, 0] * w2
                    v = UV[tt, 0, 1] * w0 + UV[tt, 1, 1] * w1 + UV[tt, 2, 1] * w2
                    k = TI[tt]
                    k = np.where(k < 0, len(tex), k)
                    tx = np.floor(u * tw[k]).astype(np.int64) % tw[k]
                    ty = np.floor(v * th[k]).astype(np.int64) % th[k]
                    texel = np.where(k < len(tex), flat[np.minimum(offs[k] + ty * tw[k] + tx, len(flat) - 1)], 0)
                    fl = FL[tt]
                    textured = k < len(tex)
                    tc = np.where(textured[:, None], pal[k, texel], np.array([150, 150, 150, 128.0]))
                    vc = COL[tt, 0] * b0[:, None] + COL[tt, 1] * b1[:, None] + COL[tt, 2] * b2[:, None]
                    col = tc[:, :3] * vc[:, :3]
                    alpha = tc[:, 3] / 128 * vc[:, 3]
                    ok = (fl & 2).astype(bool) | (texel != 255) | ~textured  # alpha test on opaque cut-outs
                    z = 1 / iz
                    if fog:
                        fb, fe, fm, fc, sc, sd = fog
                        dens = np.where(fl & 8, sd, fm * np.clip((z - fb) / max(fe - fb, 1e-6), 0, 1))
                        fcol = np.where((fl & 8)[:, None].astype(bool), np.array(sc, float), np.array(fc, float))
                        col = col * (1 - dens[:, None]) + fcol * dens[:, None]
                    pix = (py[ti_, fi] * W + px[ti_, fi]).astype(np.int64)
                    yield pix[ok], z[ok], col[ok], alpha[ok], fl[ok]
            if s >= max(W, H):
                break
            s *= 2

    zbuf = np.full(W * H, np.inf)
    cbuf = np.zeros((W * H, 3), np.float32)
    blend = (FL & 2).astype(bool)
    for pix, z, col, _, _ in fragments(keep & ~blend):
        if not len(pix):
            continue
        o = np.lexsort((z, pix))
        pix, z, col = pix[o], z[o], col[o]
        first = np.r_[True, pix[1:] != pix[:-1]]
        pix, z, col = pix[first], z[first], col[first]
        upd = z < zbuf[pix]
        zbuf[pix[upd]] = z[upd]
        cbuf[pix[upd]] = col[upd]
    cbuf[np.isinf(zbuf)] = (190, 200, 210)
    if zout is not None:  # opaque depth per pixel (sky > SKY_DIST), for hole searches
        zout.append(zbuf.reshape(H, W).copy())
    # blended: Z tested against the opaque result, applied far to near per pixel in layers
    fr = [x for x in fragments(keep & blend)]
    if fr:
        pix, z, col, alpha, fl = (np.concatenate([x[i] for x in fr]) for i in range(5))
        vis = z < zbuf[pix]
        pix, z, col, alpha, fl = pix[vis], z[vis], col[vis], alpha[vis], fl[vis]
        o = np.lexsort((-z, pix))
        pix, col, alpha, fl = pix[o], col[o], alpha[o], fl[o]
        start = np.r_[True, pix[1:] != pix[:-1]]
        grp = np.cumsum(start) - 1
        rank = np.arange(len(pix)) - np.flatnonzero(start)[grp]
        for r in range(int(rank.max()) + 1 if len(rank) else 0):
            sel = rank == r
            p, c, a, add = pix[sel], col[sel], np.minimum(alpha[sel], 2)[:, None], (fl[sel] & 4).astype(bool)[:, None]
            cbuf[p] = np.where(add, cbuf[p] + c * a, cbuf[p] + (c - cbuf[p]) * a)
    Image.fromarray(np.clip(cbuf, 0, 255).astype(np.uint8).reshape(H, W, 3)).save(path)


def render_preview(prefix, bin_path):
    """<prefix>_spawn.png, <prefix>_court.png, <prefix>_overlook.png from a map.bin."""
    geo, tex, (x, y, z, yaw), fog = load_map_bin(bin_path)
    piv = (x, y + 1.42, z)
    eye = (x - math.sin(yaw) * 4.0, y + 1.42 + 4.0 * math.sin(0.25), z - math.cos(yaw) * 4.0)
    views = dict(spawn=(eye, (piv[0] + math.sin(yaw), piv[1], piv[2] + math.cos(yaw))), **export_map.VIEWS)
    for name, (e, t) in views.items():
        render_view(f"{prefix}_{name}.png", geo, tex, e, t, fog=fog)
