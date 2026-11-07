"""Baked texture pages for the horizon level (tools/horizon.py, HZN_TEX): the far mesh's colour came from its vertices
only (one colour every few metres: the coarse, blotchy look of Ame's report, 2026-11-07); a few 128 x 128 pages per
block now carry the source map's colour between the vertices.

  texture(groups, pages, bake) -> (groups with uv and a page per triangle, [texmap records], stats)

1. charts: region growing over shared edges from the largest triangle, a triangle joining while its normal is within
   60 degrees (COS) of the chart's axis (one of six); each chart is projected on its axis plane (a near-graph surface:
   no folds), cut into pieces of at most a page at the block's density;
2. one texel density for the block, the largest at which every chart over MIN_TEXELS texels fits `pages` pages (shelf
   packing, PAD texels around each); smaller charts stay untextured (their vertex colour, as before);
3. bake (tools/native/hzbake.c): each covered texel's point on the far mesh looks up the source surface along its
   normal (else the nearest within the error) and takes its texel x light, as the GS draws the source. The page stores
   that colour over the mesh's own interpolated vertex colour (x 128: GS modulate), so a missing page (VRAM) or the
   untextured fallback still shows the right average. Empty texels take their neighbours' (bilinear / mips);
4. pages go through texmap.encode (PSMT8 or PSMT4 + CLUT, mips 1-3), the format texcache.c streams."""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PAGE = 128  # texels (texcache.c: T8 wider than 128 is broken, TECH_DEBT tex-cache-1)
PAD = 2  # texels around each chart (bilinear and mips 1-2 stay inside)
LMAX = 400.0  # metres: longest chart piece side at most (else a page's width at the block's density)
MIN_TEXELS = 12.0  # charts covering fewer texels stay flat (ASSUMED)
COS = 0.5  # cos 60 degrees
RHO_MAX = 2.0  # texels a metre at most (ASSUMED: ~10 px a texel at 56 m already)
AXES = np.array([[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]], float)
DIMS = ((2, 1), (2, 1), (0, 2), (0, 2), (0, 1), (0, 1))  # plane coordinates per axis


def charts(V, T):
    """-> chart per triangle, axis per chart"""
    n = len(T)
    N = np.cross(V[T[:, 1]] - V[T[:, 0]], V[T[:, 2]] - V[T[:, 0]])
    area = 0.5 * np.linalg.norm(N, axis=1)
    dots = (N / np.maximum(2 * area, 1e-12)[:, None]) @ AXES.T
    e = np.sort(np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]]), 1)
    tid = np.tile(np.arange(n), 3)
    key = e[:, 0] * (int(T.max()) + 2) + e[:, 1]
    o = np.argsort(key, kind="stable")
    ks, ts = key[o], tid[o]
    nb = [[] for _ in range(n)]
    for i in np.flatnonzero(ks[1:] == ks[:-1]):
        a, b = int(ts[i]), int(ts[i + 1])
        nb[a].append(b)
        nb[b].append(a)
    cid = np.full(n, -1, np.int64)
    axes = []
    for seed in np.argsort(-area, kind="stable"):
        if cid[seed] >= 0:
            continue
        ax = int(np.argmax(dots[seed]))
        c = len(axes)
        axes.append(ax)
        cid[seed] = c
        stack = [int(seed)]
        while stack:
            t = stack.pop()
            for u in nb[t]:
                if cid[u] < 0 and dots[u, ax] >= COS:
                    cid[u] = c
                    stack.append(u)
    return cid, np.array(axes, np.int64)


def chart_list(groups):
    """every chart of every group: (group, triangle indices, plane coordinates (k, 3, 2) metres)"""
    out = []
    for gi, g in enumerate(groups):
        V, T = g[0], g[2]
        if not len(T) or g[4] & 512:  # CHUNK_PANO: the panorama's coarse stand-in stays untextured
            continue
        cid, axes = charts(V, T)
        order = np.argsort(cid, kind="stable")
        for tris in np.split(order, np.flatnonzero(np.diff(cid[order])) + 1):
            out.append((gi, tris, V[T[tris]][:, :, DIMS[axes[cid[tris[0]]]]]))
    return out


def pieces(cl, L):
    """charts cut on a grid of L metres (by triangle centroid) -> (group, triangles, plane coordinates, min, size)"""
    out = []
    for gi, tris, uv in cl:
        lo = uv.reshape(-1, 2).min(0)
        sub = np.floor((uv.mean(1) - lo) / L).astype(np.int64)
        sk = sub[:, 0] * 100000 + sub[:, 1]
        so = np.argsort(sk, kind="stable")
        for part in np.split(so, np.flatnonzero(np.diff(sk[so])) + 1):
            q = uv[part]
            mn = q.reshape(-1, 2).min(0)
            out.append((gi, tris[part], q, mn, q.reshape(-1, 2).max(0) - mn))
    return out


def pack(sizes, rho, pages):
    """shelf packing of the pieces at rho texels a metre (pieces too big for a page are scaled down to fit) ->
    [(page, x, y, scale)] per piece (None: below MIN_TEXELS), or None when they need more than `pages`"""
    inner = PAGE - 2 * PAD
    dims = []
    for sz in sizes:
        w, h = sz * rho
        if w * h < MIN_TEXELS and max(w, h) < 4:
            dims.append(None)
            continue
        k = min(1.0, (inner - 1) / max(w, h, 1e-9))
        dims.append((int(np.ceil(w * k)) + 1 + 2 * PAD, int(np.ceil(h * k)) + 1 + 2 * PAD, rho * k))
    order = sorted((i for i, d in enumerate(dims) if d), key=lambda i: -dims[i][1])
    out = [None] * len(sizes)
    page, x, y, shelf = 0, 0, 0, 0
    for i in order:
        w, h, r = dims[i]
        if x + w > PAGE:
            x, y, shelf = 0, y + shelf, 0
        if y + h > PAGE:
            page, x, y, shelf = page + 1, 0, 0, 0
            if page >= pages:
                return None
        out[i] = (page, x, y, r)
        x += w
        shelf = max(shelf, h)
    return out


def raster(uvt):
    """texel centres covered by the triangle uvt (3, 2) in texels -> (ix, iy, barycentrics (k, 3))"""
    lo = np.floor(uvt.min(0)).astype(int)
    hi = np.ceil(uvt.max(0)).astype(int)
    xs, ys = np.meshgrid(np.arange(lo[0], hi[0] + 1), np.arange(lo[1], hi[1] + 1))
    px, py = xs.ravel() + 0.5, ys.ravel() + 0.5
    a, b, c = uvt
    den = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    if abs(den) < 1e-12:
        return xs.ravel()[:0], ys.ravel()[:0], np.zeros((0, 3))
    w0 = ((b[0] - px) * (c[1] - py) - (b[1] - py) * (c[0] - px)) / den
    w1 = ((c[0] - px) * (a[1] - py) - (c[1] - py) * (a[0] - px)) / den
    w2 = 1 - w0 - w1
    tol = -1e-4
    ok = (w0 >= tol) & (w1 >= tol) & (w2 >= tol)
    return xs.ravel()[ok], ys.ravel()[ok], np.stack([w0[ok], w1[ok], w2[ok]], 1)


def texture(groups, pages, bake):
    """groups: horizon.py's (V, colours u8, T, cell per triangle, flags) after placement; bake: f(points, normals) ->
    (rgb, found) (horizon.bake_fn). -> (groups as (V, col, T, tc, flags, st u16 per vertex, page per triangle (-1:
    flat)), texmap records, stats), or None (nothing to texture)"""
    import texmap
    cl = chart_list(groups)
    inner = PAGE - 2 * PAD - 1

    def attempt(rho):  # pieces at most a page wide at rho (a big chart fills whole pages), packed
        ps = pieces(cl, min(LMAX, inner / rho * 0.98))
        return ps, pack([p[4] for p in ps], rho, pages)
    lo, hi = 0.02, RHO_MAX
    ps, place = attempt(hi)
    rho = hi
    if place is None:
        ps, place = attempt(lo)
        if place is None:
            return None
        for _ in range(12):
            mid = (lo * hi) ** 0.5
            p2, pl2 = attempt(mid)
            if pl2 is not None:
                lo, ps, place = mid, p2, pl2
            else:
                hi = mid
        rho = lo
    # texel coordinates of each placed piece's corners, and the texels each triangle covers
    corner_uv = [np.full((len(g[2]), 3, 2), np.nan) for g in groups]
    tpage = [np.full(len(g[2]), -1, np.int64) for g in groups]
    qp, qn, qc, qpix = [], [], [], []
    for (gi, tris, q, mn, _sz), pl in zip(ps, place):
        if pl is None:
            continue
        page, x, y, r = pl
        uvt = (q - mn) * r + np.array([x + PAD + 0.5, y + PAD + 0.5])
        corner_uv[gi][tris] = uvt
        tpage[gi][tris] = page
        V, col, T = groups[gi][0], groups[gi][1], groups[gi][2]
        for k, t in enumerate(tris):
            ix, iy, w = raster(uvt[k])
            if not len(ix):
                continue
            tri = V[T[t]]
            n = np.cross(tri[1] - tri[0], tri[2] - tri[0])
            n /= max(np.linalg.norm(n), 1e-12)
            qp.append(w @ tri)
            qn.append(np.repeat(n[None], len(ix), 0))
            qc.append(w @ col[T[t]].astype(np.float64))
            qpix.append(np.stack([np.full(len(ix), page), iy, ix], 1))
    if not qp:
        return None
    qp, qn, qc, qpix = np.concatenate(qp), np.concatenate(qn), np.concatenate(qc), np.concatenate(qpix)
    rgb, found = bake(qp, qn)
    rgb = np.where(found[:, None], rgb, qc)
    ratio = np.clip(np.rint(rgb * 128.0 / np.maximum(qc, 8.0)), 0, 255)
    img = np.zeros((pages, PAGE, PAGE, 3))
    cnt = np.zeros((pages, PAGE, PAGE))
    np.add.at(img, (qpix[:, 0], qpix[:, 1], qpix[:, 2]), ratio)
    np.add.at(cnt, (qpix[:, 0], qpix[:, 1], qpix[:, 2]), 1)
    used = int(qpix[:, 0].max()) + 1
    img, cnt = img[:used], cnt[:used]
    filled = cnt > 0
    img[filled] /= cnt[filled][:, None]
    for _ in range(PAD + 2):  # empty texels take their filled neighbours' mean
        acc = np.zeros_like(img)
        n = np.zeros_like(cnt)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            acc += np.roll(np.roll(img * filled[..., None], dy, 1), dx, 2)
            n += np.roll(np.roll(filled, dy, 1), dx, 2)
        grow = ~filled & (n > 0)
        img[grow] = acc[grow] / n[grow][:, None]
        filled = filled | grow
    img[~filled] = 128
    recs = []
    for p in range(used):
        rgba = np.concatenate([img[p], np.full((PAGE, PAGE, 1), 255.0)], 2).astype(np.uint8)
        recs.append(texmap.encode(PAGE, PAGE, rgba.tobytes(), name=f"hzn{p}"))
    # vertices split per (vertex, piece) where textured: each corner its own st
    out = []
    for gi, g in enumerate(groups):
        V, col, T, tc, cfl = g[:5]
        uv = corner_uv[gi].reshape(-1, 2)
        tex = np.repeat(tpage[gi] >= 0, 3)
        vid = T.reshape(-1)
        u16 = np.where(tex[:, None], np.clip(np.rint(uv / PAGE * 4096.0), 0, 65535), 0).astype(np.int64)
        key = np.stack([vid, np.repeat(tpage[gi], 3), u16[:, 0], u16[:, 1]], 1)
        uk, inv = np.unique(key, axis=0, return_inverse=True)
        inv = inv.reshape(-1)
        out.append((V[uk[:, 0]], col[uk[:, 0]], inv.reshape(-1, 3), tc, cfl, uk[:, 2:4].astype(np.uint16), tpage[gi]))
    st = dict(pages=used, rho=float(rho), texels=int(len(qpix)), found=float(found.mean()),
              charts=sum(1 for p in place if p is not None), flat=sum(1 for p in place if p is None))
    return out, recs, st
