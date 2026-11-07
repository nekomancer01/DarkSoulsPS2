"""Far panorama of a world block for the horizon section (tools/horizon.py HZN_PANO): the block's backdrop beyond
PANO_MIN metres of every navmesh point, rendered offline from the block's centre onto a cylinder (tools/native/hzpano.c)
and drawn by map.c as a ring of textured strips while the camera is inside the block (its navmesh's reach). That far,
the view barely moves as the player walks the block (300 m away, 50 m of walking turns it under 10 degrees), so a
picture at full texture detail replaces thousands of triangles; other blocks' cameras still see the content through a
coarse horizon mesh (CHUNK_PANO, skipped while the panorama is drawn).

  render(P, sel, nav, src, eye) -> dict(tiles=[texmap records], chunks=[(tile, pos s16x3, st u16x2, org, step, mn,
                                       mx)], centre, valid_r, radius, stats) or None (too little far content)

Cylinder: W x H texels (azimuth uniform over 360 degrees, rows uniform in tan(elevation) between the content's
extremes), rendered 2 x 2 supersampled; a texel is opaque where at least half its samples hit (cut-out, texmap's
transparent index), its colour the mean of the hits. Cut into 128-wide tiles (texcache: T8 wider than 128 is broken,
TECH_DEBT tex-cache-1); empty tiles are left out. Each tile is SEG quads on the ring of radius `radius` (the content's
median distance, so the fog map.c applies at the ring is about the content's own)."""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PANO_MIN = float(os.environ.get("HZN_PANO_MIN", 300.0))  # metres from every navmesh point (ASSUMED)
W = int(os.environ.get("HZN_PANO_W", 1024))  # texels around (8 tiles: ~0.35 degrees a texel, ~3.5 px on screen)
H = 128
SEG = 8  # ring quads per tile
MIN_TRIS = 200  # less far content than this: no panorama
SPLIT_TEXELS = 6.0  # triangles spanning more texels than this are split (the projection is not affine)


def _split(P, UV, L):
    """each triangle into four (corner midpoints)"""
    def mid(A):
        m01, m12, m20 = (A[:, 0] + A[:, 1]) / 2, (A[:, 1] + A[:, 2]) / 2, (A[:, 2] + A[:, 0]) / 2
        return np.concatenate([np.stack([A[:, 0], m01, m20], 1), np.stack([m01, A[:, 1], m12], 1),
                               np.stack([m20, m12, A[:, 2]], 1), np.stack([m01, m12, m20], 1)])
    return mid(P), mid(UV), mid(L)


def _project(P, c, t_lo, t_hi, w, h):
    d = P - c
    hd = np.maximum(np.hypot(d[..., 0], d[..., 2]), 1e-3)
    u = (np.arctan2(d[..., 2], d[..., 0]) + math.pi) / (2 * math.pi) * w
    v = (t_hi - d[..., 1] / hd) / (t_hi - t_lo) * h
    wrap = (u.max(1) - u.min(1)) > w / 2  # over the seam: the left corners one turn on
    u = np.where(wrap[:, None] & (u < w / 2), u + w, u)
    return np.stack([u, v, np.linalg.norm(d, axis=-1)], -1)


def render(P, sel, nav, src, eye, view_d, pano_lib):
    """P: the block's coarse triangles; sel: the panorama's (backdrop, opaque, beyond PANO_MIN); src: horizon.SRC_TEX;
    view_d: horizon.view_d (nearest navmesh distance); pano_lib: libhzpano"""
    import ctypes
    import texmap
    if sel.sum() < MIN_TRIS or not len(nav) or pano_lib is None:
        return None
    c = np.array([nav[:, 0].mean(), nav[:, 1].mean() + eye, nav[:, 2].mean()])
    valid_r = float(np.hypot(nav[:, 0] - c[0], nav[:, 2] - c[2]).max()) + 10.0
    Pp = P[sel]
    d = Pp - c
    t = d[..., 1] / np.maximum(np.hypot(d[..., 0], d[..., 2]), 1e-3)
    t_lo, t_hi = float(np.clip(t.min(), -1.5, -0.02)), float(np.clip(t.max(), 0.02, 1.5))
    radius = float(np.median(np.linalg.norm(d, axis=-1)))
    UV, L = src["uv"][sel], src["light"][sel]
    ids = sorted(src["img"])
    remap = {k: i for i, k in enumerate(ids)}
    tid = np.array([remap.get(int(k), -1) for k in src["tid"][sel]], np.int64)
    w2, h2 = 2 * W, 2 * H
    # split the triangles that span many texels, a few rounds (the cylinder is curved, the raster affine)
    for _ in range(6):
        S = _project(Pp, c, t_lo, t_hi, w2, h2)
        ext = np.maximum(S[..., 0].max(1) - S[..., 0].min(1), S[..., 1].max(1) - S[..., 1].min(1))
        big = ext > 2 * SPLIT_TEXELS
        if not big.any() or len(Pp) > 400000:
            break
        a, b, l = _split(Pp[big], UV[big], L[big])
        Pp, UV, L = np.concatenate([Pp[~big], a]), np.concatenate([UV[~big], b]), np.concatenate([L[~big], l])
        tid = np.concatenate([tid[~big], np.tile(tid[big], 4)])
    S = np.ascontiguousarray(_project(Pp, c, t_lo, t_hi, w2, h2).reshape(-1, 9), np.float32)
    imgs = [np.ascontiguousarray(src["img"][k], np.uint8) for k in ids]
    pix = np.ascontiguousarray(np.concatenate([im.reshape(-1) for im in imgs]) if imgs else np.zeros(4, np.uint8))
    toff = np.ascontiguousarray(np.cumsum([0] + [im.size for im in imgs])[:len(imgs)] if imgs else [0], np.int64)
    tw = np.ascontiguousarray([im.shape[1] for im in imgs] or [1], np.int32)
    th = np.ascontiguousarray([im.shape[0] for im in imgs] or [1], np.int32)
    uv = np.ascontiguousarray(UV.reshape(-1, 6), np.float32)
    li = np.ascontiguousarray(L.reshape(-1, 9), np.float32)
    ti = np.ascontiguousarray(tid, np.int32)
    rgba = np.zeros((h2, w2, 4), np.float32)
    zb = np.full((h2, w2), 1e30, np.float32)
    pano_lib.hz_pano(len(S), S.ctypes.data, uv.ctypes.data, ti.ctypes.data, li.ctypes.data, len(imgs), pix.ctypes.data,
                     toff.ctypes.data, tw.ctypes.data, th.ctypes.data, w2, h2, rgba.ctypes.data, zb.ctypes.data)
    # 2 x 2 down: opaque where half the samples hit, colour = mean of the hits
    q = rgba.reshape(H, 2, W, 2, 4)
    hits = q[..., 3].sum((1, 3))
    col = (q[..., :3] * q[..., 3:4]).sum((1, 3)) / np.maximum(hits, 1)[..., None]
    opaque = hits >= 2
    filled = hits > 0
    for _ in range(4):  # colour into the empty texels next to drawn ones (bilinear fringes take it, not black)
        acc, n = np.zeros_like(col), np.zeros(filled.shape)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            acc += np.roll(np.roll(col * filled[..., None], dy, 0), dx, 1)
            n += np.roll(np.roll(filled, dy, 0), dx, 1)
        grow = ~filled & (n > 0)
        col[grow] = acc[grow] / n[grow][:, None]
        filled |= grow
    tiles, chunks = [], []
    for k in range(W // 128):
        sl = slice(128 * k, 128 * (k + 1))
        if not opaque[:, sl].any():
            continue
        rgba8 = np.concatenate([np.clip(np.rint(col[:, sl]), 0, 255), np.where(opaque[:, sl], 255, 0)[..., None]],
                               2).astype(np.uint8)
        tiles.append(texmap.encode(128, 128, rgba8.tobytes(), name=f"pano{k}"))
        # the tile's ring strip: SEG quads, azimuth from u (texel 0 at -pi), rows from t_hi (v = 0) down to t_lo
        az = (128 * k + np.arange(SEG + 1) * 128.0 / SEG) / W * 2 * math.pi - math.pi
        top = np.stack([c[0] + radius * np.cos(az), np.full(SEG + 1, c[1] + radius * t_hi), c[2] + radius * np.sin(az)], 1)
        bot = np.stack([c[0] + radius * np.cos(az), np.full(SEG + 1, c[1] + radius * t_lo), c[2] + radius * np.sin(az)], 1)
        su = np.arange(SEG + 1) / SEG
        tri_p, tri_s = [], []
        for j in range(SEG):
            a, b, cc, dd = top[j], top[j + 1], bot[j + 1], bot[j]
            sa, sb, sc, sd = (su[j], 0.0), (su[j + 1], 0.0), (su[j + 1], 1.0), (su[j], 1.0)
            tri_p += [a, b, cc, a, cc, dd]
            tri_s += [sa, sb, sc, sa, sc, sd]
        tri_p, tri_s = np.array(tri_p), np.array(tri_s)
        mn, mx = tri_p.min(0), tri_p.max(0)
        org = (mn + mx) / 2
        step = 2.0 ** int(np.ceil(np.log2(max(np.abs(tri_p - org).max(), 1e-3) / 32000)))
        qp = np.clip(np.rint((tri_p - org) / step), -32767, 32767).astype(np.int16)
        st = np.clip(np.rint(tri_s * 4096.0), 0, 65535).astype(np.uint16)
        chunks.append((len(tiles) - 1, qp, st, org, step, qp.astype(np.float64).min(0) * step + org,
                       qp.astype(np.float64).max(0) * step + org))
    if not tiles:
        return None
    return dict(tiles=tiles, chunks=chunks, centre=c, valid_r=valid_r, radius=radius,
                stats=dict(tris=int(sel.sum()), tiles=len(tiles), radius=radius, t=(t_lo, t_hi),
                           cover=float(opaque.mean())))
