"""Plank shells (TECH_DEBT world-49): the loose pieces of a map that are too dense to fit the arena (Blighttown's planks
and scaffolding: thousands of separate closed boxes the simplifier cannot collapse into each other, so half of them were
left at 24 px) become closed shells the simplifier can reduce, drawn untextured with their colour per vertex.

  apply(meshes, map_id) -> meshes      (tools/mapgather.py gather, after the light and tone map)

Per map in PLANK_SHELL_MAPS (env, comma list; default "m14_00_00_00", Blighttown; PLANK_SHELL=0: none, the old export):
1. Loose pieces: per mesh, triangles joined through shared positions, at most DIAG m across (mapcfg.WS_DROP_DIAG, the
   same rule as mapsimp.loose_pieces), of opaque, textured, lit, static map meshes (no blend, sky, foliage, grass,
   cut-out cards, switched entities, KEEP_MODELS). They leave their meshes.
2. Per BIN m cube of their centroids and per draw-group set: the unsigned distance to them on a grid of spacing VOX
   (tools/mapx/shell.c), its iso surface at ISO x VOX by surface nets (one vertex per grid cube the surface crosses, at
   the mean of its edge crossings; a quad per crossed grid edge), then meshoptimizer (geometry only, absolute error
   PRE_ERR) so the exporter's own simplifier starts from a few thousand triangles a cube, not a voxel surface. Pieces
   closer than twice the offset fuse. Drawn two-sided (the surface nets winding is not checked).
3. Colour per shell vertex: its nearest source triangle's (the grid point's nearest triangle): the mean of that
   triangle's wedge light x the texture's mean colour, the colour the textured triangle shows on average (vertex colour
   x texture mean / 0x80, gather's tone map). Untextured (ti -1): the GS draws the colour as it is. The wood grain under
   ~VOX is lost (the research thread's UV transfer onto shells broke on half the triangles: TECH_DEBT world-49).
Measured offline on Blighttown block d (reports/cixwrt/plank, the research thread's results.md): voxel shells at 0.1 m
keep ~32 % of the triangles at a 0.2 m error, against ~52 % for today's pieces at the 36 px caps."""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapcfg  # noqa: E402
import souls  # noqa: E402

ON = os.environ.get("PLANK_SHELL", "0") != "0"
MAPS = set(m for m in os.environ.get("PLANK_SHELL_MAPS", "m14_00_00_00").split(",") if m)
VOX = float(os.environ.get("PLANK_SHELL_VOX", 0.1))   # grid spacing (m)
ISO = 0.75                                             # iso offset in grid steps (the research thread's choice)
BIN = 8.0                                              # m: one grid per cube of loose-triangle centroids
PRE_ERR = float(os.environ.get("PLANK_SHELL_ERR", 0.04))  # m: meshoptimizer's pass before the exporter's simplifier
ALB = {}  # id(mesh wrgb list) -> the texture's mean colour (gather's tone map fills it)


def _pieces(T, npos):
    """position triangles (n, 3) -> component label per triangle (union-find over shared positions)"""
    parent = np.arange(npos)

    def find(a):
        r = a
        while parent[r] != r:
            r = parent[r]
        while parent[a] != r:
            parent[a], a = r, parent[a]
        return r
    for a, b, c in T.tolist():
        ra, rb, rc = find(a), find(b), find(c)
        if rb != ra:
            parent[rb] = ra
        if rc != ra and rc != rb:
            parent[rc] = ra
    roots = np.fromiter((find(i) for i in range(npos)), np.int64, npos)
    return roots[T[:, 0]]


def _eligible(m):
    return (m["ti"] >= 0 and not m["keep"] and not m["grass"] and not m.get("fol") and not m.get("cutout")
            and not m["ent"] and not m["flags"] & (mapcfg.CHUNK_BLEND | mapcfg.CHUNK_SKY | mapcfg.CHUNK_ADD)
            and len(m["tris"]) and len(m["wrgb"]) and id(m["wrgb"]) in ALB)


def _surface_nets(f, origin, v):
    """iso surface f = 0 of a grid (nx, ny, nz order z, y, x in memory: f[z, y, x]) -> (vertices (n, 3), triangles
    (m, 3), the grid point of each vertex's cube nearest the surface (for its colour))"""
    nz, ny, nx = f.shape
    inside = f < 0
    # cubes (z, y, x) with corners of both signs
    c = np.zeros((nz - 1, ny - 1, nx - 1), np.int8)
    acc = np.zeros((nz - 1, ny - 1, nx - 1, 3))
    cnt = np.zeros((nz - 1, ny - 1, nx - 1))
    # edges along x, y, z: crossing points accumulate into the 4 cubes around each edge
    for axis in range(3):
        sl0 = [slice(None)] * 3
        sl1 = [slice(None)] * 3
        sl0[2 - axis] = slice(0, -1)
        sl1[2 - axis] = slice(1, None)
        a, b = f[tuple(sl0)], f[tuple(sl1)]
        cross = (a < 0) != (b < 0)
        t = np.where(cross, a / np.where(a - b == 0, 1, a - b), 0)
        idx = np.argwhere(cross)  # (z, y, x) of the edge's first point
        if not len(idx):
            continue
        tt = t[cross]
        pt = idx[:, ::-1].astype(np.float64)  # x, y, z
        pt[:, axis] += tt
        # the cubes sharing this edge: offsets in the two other axes of -1 / 0
        o = [k for k in range(3) if k != axis]
        for d0 in (-1, 0):
            for d1 in (-1, 0):
                q = idx[:, ::-1].copy()
                q[:, o[0]] += d0
                q[:, o[1]] += d1
                ok = (q >= 0).all(1) & (q[:, 0] < nx - 1) & (q[:, 1] < ny - 1) & (q[:, 2] < nz - 1)
                qq = q[ok]
                np.add.at(acc, (qq[:, 2], qq[:, 1], qq[:, 0]), pt[ok])
                np.add.at(cnt, (qq[:, 2], qq[:, 1], qq[:, 0]), 1)
    has = cnt > 0
    vid = np.full(cnt.shape, -1, np.int64)
    vid[has] = np.arange(int(has.sum()))
    V = acc[has] / cnt[has][:, None]
    cube = np.argwhere(has)  # z, y, x
    tris = []
    for axis in range(3):
        sl0 = [slice(None)] * 3
        sl1 = [slice(None)] * 3
        sl0[2 - axis] = slice(0, -1)
        sl1[2 - axis] = slice(1, None)
        a, b = inside[tuple(sl0)], inside[tuple(sl1)]
        cross = a != b
        idx = np.argwhere(cross)
        if not len(idx):
            continue
        flip = a[cross]  # inside -> outside along +axis
        o = [k for k in range(3) if k != axis]
        quads = []
        for d0, d1 in ((-1, -1), (0, -1), (0, 0), (-1, 0)):
            q = idx[:, ::-1].copy()
            q[:, o[0]] += d0
            q[:, o[1]] += d1
            quads.append(q)
        q = np.stack(quads, 1)  # (n, 4, xyz)
        ok = ((q >= 0).all((1, 2)) & (q[:, :, 0] < nx - 1).all(1) & (q[:, :, 1] < ny - 1).all(1)
              & (q[:, :, 2] < nz - 1).all(1))
        q, flip = q[ok], flip[ok]
        ids = vid[q[:, :, 2], q[:, :, 1], q[:, :, 0]]
        good = (ids >= 0).all(1)
        ids, flip = ids[good], flip[good]
        # winding: outward normal (from inside to outside); swap for the edges going the other way and for the
        # handedness of the (o0, o1) pair
        t1 = np.stack([ids[:, 0], ids[:, 1], ids[:, 2]], 1)
        t2 = np.stack([ids[:, 0], ids[:, 2], ids[:, 3]], 1)
        rev = flip if axis == 1 else ~flip
        t1[rev] = t1[rev][:, ::-1]
        t2[rev] = t2[rev][:, ::-1]
        tris += [t1, t2]
    T = np.concatenate(tris) if tris else np.zeros((0, 3), np.int64)
    return origin + V * v, T, cube


def apply(meshes, map_id, log=print):
    if not ON or map_id not in MAPS:
        return meshes
    import mapx_native
    MX = mapx_native.lib()
    if MX is None:
        log("plank shells: libmapx missing, none")
        return meshes
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "meshopt"))
    import meshopt
    # 1. loose pieces
    soup, col, keys = [], [], []
    n_src = n_loose = 0
    for m in meshes:
        if not _eligible(m):
            continue
        P = np.asarray(m["_np"]["pos"][1] if "pos" in m.get("_np", {}) else m["pos"], np.float64).reshape(-1, 3)
        W = np.asarray(m["_np"]["wpos"][1] if "wpos" in m.get("_np", {}) else m["wpos"], np.int64).reshape(-1)
        Tw = np.asarray(m["_np"]["tris"][1], np.int64).reshape(-1, 3)
        Tp = W[Tw]
        n_src += len(Tw)
        lab = _pieces(Tp, len(P))
        lo = np.full((len(P), 3), np.inf)
        hi = np.full((len(P), 3), -np.inf)
        tp = P[Tp]  # (n, 3, 3)
        np.minimum.at(lo, lab, tp.min(1))
        np.maximum.at(hi, lab, tp.max(1))
        diag = np.linalg.norm(hi - lo, axis=1)
        loose = diag[lab] <= mapcfg.WS_DROP_DIAG
        if not loose.any():
            continue
        n_loose += int(loose.sum())
        alb = np.asarray(ALB[id(m["wrgb"])], np.float64)
        rgb = np.asarray(m["_np"]["wrgb"][1] if "wrgb" in m.get("_np", {}) else m["wrgb"], np.float64).reshape(-1, 3)
        c = rgb[Tw[loose]].mean(1) * alb * 255.0 / 0x80
        soup.append(tp[loose])
        col.append(np.clip(c, 0, 255))
        keys.append(np.full(int(loose.sum()), m["dg"], np.int64))
        keep = Tw[~loose]
        m["tris"] = souls.Rows(np.ascontiguousarray(keep.astype(np.int32)))
        m["_np"]["tris"] = (m["tris"], m["tris"].a)
    if not soup:
        log(f"plank shells: {map_id}: no loose pieces")
        return meshes
    soup, col, keys = np.concatenate(soup), np.concatenate(col), np.concatenate(keys)
    cen = soup.mean(1)
    binkey = np.floor(cen / BIN).astype(np.int64)
    groups = {}
    for i, k in enumerate(zip(keys.tolist(), *binkey.T.tolist())):
        groups.setdefault(k, []).append(i)
    band = (ISO + 1.5) * VOX
    n_out = 0
    for (dg, *bkey), idx in sorted(groups.items()):
        idx = np.asarray(idx)
        t = np.ascontiguousarray(soup[idx], np.float64)
        lo = t.reshape(-1, 3).min(0) - band - VOX
        hi = t.reshape(-1, 3).max(0) + band + VOX
        dims = np.ceil((hi - lo) / VOX).astype(int) + 1
        dist = np.full(int(np.prod(dims)), band, np.float32)
        near = np.full(len(dist), -1, np.int32)
        MX.mx_shell_field(len(t), t.ctypes.data, *lo.tolist(), VOX, int(dims[0]), int(dims[1]), int(dims[2]), band,
                          dist.ctypes.data, near.ctypes.data)
        f = (dist - ISO * VOX).reshape(dims[2], dims[1], dims[0])
        V, T, cube = _surface_nets(f, lo, VOX)
        if not len(T):
            continue
        # colour: the nearest source triangle of the vertex's cube corner nearest the surface
        nr = near.reshape(dims[2], dims[1], dims[0])
        cz, cy, cx = cube[:, 0], cube[:, 1], cube[:, 2]
        best = np.full(len(V), -1)
        bd = np.full(len(V), np.inf)
        for dz in (0, 1):
            for dy in (0, 1):
                for dx in (0, 1):
                    d = np.abs(f[cz + dz, cy + dy, cx + dx])
                    n_ = nr[cz + dz, cy + dy, cx + dx]
                    take = (n_ >= 0) & (d < bd)
                    best[take], bd[take] = n_[take], d[take]
        best[best < 0] = 0
        vc = col[idx][best]
        # meshoptimizer: geometry only, absolute error (the exporter's own simplifier takes it from there)
        Vf = np.ascontiguousarray(V, np.float32)
        out, _ = meshopt.simplify(Vf, np.ascontiguousarray(T.reshape(-1), np.uint32), 0, error=PRE_ERR, absolute=True)
        if not len(out):
            continue
        used, inv = np.unique(out, return_inverse=True)
        Pn = V[used]
        Tn = inv.reshape(-1, 3).astype(np.int32)
        Cn = np.trunc(vc[used] + 0.5).astype(np.int64)
        pos = [tuple(p) for p in Pn.tolist()]
        wpos = list(range(len(pos)))
        wrgb = Cn.tolist()
        wa = [0x80] * len(pos)
        tris = souls.Rows(np.ascontiguousarray(Tn))
        npd = {"pos": (pos, Pn), "wpos": (wpos, np.arange(len(pos), dtype=np.int32)), "tris": (tris, tris.a),
               "wrgb": (wrgb, Cn), "wa": (wa, np.full(len(pos), 0x80, np.int64))}
        meshes.append(dict(pos=pos, wpos=wpos, _np=npd, wuv=[], wrgb=wrgb, wa=wa, tris=tris, bump=None, ti=-1,
                           flags=0, grass=False, fol=None, keep=False,
                           name=f"plankshell/{'_'.join(map(str, bkey))}", dg=dg, ent=0, piece=f"plankshell_{dg}",
                           cutout=False, occ=1))
        n_out += len(Tn)
    log(f"plank shells: {map_id}: {n_loose} of {n_src} triangles in loose pieces -> {n_out} shell triangles in "
        f"{len(groups)} cubes (grid {VOX} m, offset {ISO * VOX:.3f} m, pre-simplify {PRE_ERR} m)")
    return meshes
