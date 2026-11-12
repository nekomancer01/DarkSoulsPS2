"""The map exporter's simplification: view distances, thin-face softening, cell cuts, the spot budget solve (2026-11-12, code moved as it was; tools/export_map.py runs it)."""
import sys
import math, os, time
import souls
import export_normal
import dsr_light
import area as area_mod
import mapx_native
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapcells
import mapcfg
import mapchunks
import mapgather


def view_distance(points, spawn, nav_v, nav_t, raw=False, floor=None):
    """Per point (lists of positions): the distance it is judged at, max(NEAR, distance to the walkable navmesh,
    SPAWN_K x distance to the spawn). Near anything walkable it stays detailed, background gets coarse. raw: the
    distance to the navmesh alone."""
    import numpy as np
    nv = np.array(nav_v, dtype=np.float64)
    nt = np.array(nav_t, dtype=np.int64)
    a, b, c = nv[nt[:, 0]], nv[nt[:, 1]], nv[nt[:, 2]]
    samples = [nv, (a + b + c) / 3]
    for s in (1, 2, 3):  # points inside the navmesh triangles, so large polygons count too
        for u, v in ((s / 4, 0), (0, s / 4), (s / 4, (4 - s) / 4 * 0.5)):
            samples.append(a + u * (b - a) + v * (c - a))
    samples.append((a + b) / 2)
    samples.append((b + c) / 2)
    samples.append((a + c) / 2)
    S = np.unique(np.round(np.concatenate(samples) * 2) / 2, axis=0)  # 0.5 m
    allp = np.array([p for ps in points for p in ps], dtype=np.float64)
    cells, inv = np.unique(np.round(allp / 2) * 2, axis=0, return_inverse=True)  # 2 m: only needed coarsely
    dn = np.empty(len(cells))
    lib = dsr_light.geom_lib()
    if lib is not None and len(S):  # k-d tree (tools/native/geom.c): Burg / Firelink 103 s of brute force -> < 1 s
        # cells (2 m) and samples (0.5 m) lie on lattices, so both paths' squared distances are exact: same result
        Sc, Qc, d2 = np.array(S, np.float64, order="C"), np.ascontiguousarray(cells, np.float64), np.empty(len(cells))
        if lib.nearest_d2(len(Qc), Qc.ctypes.data, len(Sc), Sc.ctypes.data, d2.ctypes.data):
            raise MemoryError("nearest_d2")
        dn[:] = np.sqrt(np.maximum(d2, 0))
    else:
        s2 = (S ** 2).sum(1)
        for i in range(0, len(cells), 4096):
            c = cells[i:i + 4096]
            d2 = (c ** 2).sum(1)[:, None] + s2[None, :] - 2 * c @ S.T
            dn[i:i + 4096] = np.sqrt(np.maximum(d2.min(1), 0))
    dn = dn[inv.ravel()]
    if raw:
        return [dn[k:k + len(ps)].tolist() for k, ps in zip(np.cumsum([0] + [len(ps) for ps in points]), points)]
    ds = np.sqrt(((allp - np.array(spawn)) ** 2).sum(1))
    d = np.maximum(floor or mapcfg.NEAR, np.maximum(dn, mapcfg.SPAWN_K * ds))
    for kp in mapcfg.KEEP_POS:  # ground under kept objects (KEEP_R)
        near_k = (allp[:, 0] - kp[0]) ** 2 + (allp[:, 2] - kp[2]) ** 2 < mapcfg.KEEP_R ** 2
        d[near_k] = np.minimum(d[near_k], mapcfg.KEEP_D)
    out, k = [], 0
    for ps in points:
        out.append(d[k:k + len(ps)].tolist())
        k += len(ps)
    return out


def prune_foliage(meshes, nav_v, nav_t):
    """Drop the kept foliage tufts that map.c could never draw: those whose rank stays above the density at the
    nearest the camera gets (distance to the navmesh - CAM_REACH; the chunk box can be a GRASS_GRID cell closer)."""
    ms = [m for m in meshes if m["grass"] and m["tris"]]
    pts = [[m["pos"][m["wpos"][t[0]]] for t in m["tris"]] for m in ms]
    before = after = 0
    for m, d in zip(ms, view_distance(pts, None, nav_v, nav_t, raw=True)):
        near = {}
        for (r, tuft, _), x in zip(m["fol"], d):
            near[tuft] = min(near.get(tuft, x), x)
        keep = [r < (mapchunks.GRASS_D0 / max(mapchunks.GRASS_D0, near[tuft] - mapchunks.CAM_REACH - 2 * mapchunks.GRASS_GRID)) ** 2 for r, tuft, _ in m["fol"]]
        before += len(keep)
        m["tris"] = [t for t, k in zip(m["tris"], keep) if k]
        m["fol"] = [f for f, k in zip(m["fol"], keep) if k]
        after += len(m["tris"])
    print(f"foliage: {before} tris kept by GRASS_KEEP, {after} within the camera's reach")


BACKDROP_STATS = [0, 0]


def _take(tl, idx):
    """the triangles idx (ascending) of tl (a list or mapx_native.Tris)"""
    if isinstance(tl, mapx_native.Tris):
        return tl[np.asarray(idx, np.intp)] if idx else []
    return [tl[i] for i in idx]


def backdrop_visible(m, tl, nav_s):
    """Backdrop triangles of mesh m minus the one-sided ones (CHUNK_CULL) that face away from every navmesh sample point:
    never seen from anywhere the player can stand."""
    BACKDROP_STATS[0] += len(tl)
    if not BACKDROP_SIDE or not m["flags"] & 1 or not nav_s:
        BACKDROP_STATS[1] += len(tl)
        return tl
    import numpy as np
    if not tl:
        return tl
    P = mapx_native.tri_arrays(tl)[0]  # (n, 3 corners, xyz)
    n = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
    S = np.array(nav_s, dtype=np.float64)
    # facing some sample point: n . (s - p0) > 0 (the winding / cull sense as map.c draws it is checked by the A/B)
    vis = ((S[None, :, :] - P[:, None, 0, :]) * n[:, None, :]).sum(2)
    keep = (vis * BACKDROP_SIDE > 0).any(1)
    out = _take(tl, np.flatnonzero(keep).tolist())
    BACKDROP_STATS[1] += len(out)
    return out


SOFT_STATS = [0, 0]  # coarse / backdrop triangles seen, softened


def soften_thin(entries, view_s, floor):
    """Thin faces of the coarse / backdrop levels (render-12): a face drawn narrower than SOFT_PX pixels covers a pixel
    centre in one frame and misses it in the next, so the narrow lit pillar fronts of Firelink's ring wall (0.3 m, ~2 px
    from the graveyard) flicker as the camera moves (512 px frame, no anti-aliasing; DSR draws them 4-6 px wide). The
    width is the triangle's narrowest extent projected toward the viewpoints view_s (navmesh samples), the median over
    those within SOFT_REACH metres of the nearest one, at their distance - CAM_REACH (at least floor). Its corners'
    colours are pulled by 1 - width / SOFT_PX toward the mean colour of every coarse / backdrop triangle within SOFT_R_PX
    pixels of it at that distance (weighted by area seen from the viewpoints, any mesh): a pre-filter like a texture
    mip, for geometry. entries: the level entries (dicts with tris, wrgb); each softened one gets its own wrgb copy."""
    import numpy as np
    if not mapcfg.SOFT_PX or not entries or not len(view_s):
        return
    L = mapx_native.lib()
    if L is not None and dsr_light.geom_lib() is not None:
        return soften_thin_c(L, entries, view_s, floor)
    S = np.asarray(view_s, dtype=np.float64)
    rows = []  # per entry: corners P (n, 3, xyz), wedges W, colour per triangle, area seen, px, footprint (m / px)
    for e in entries:
        tl = e["tris"]
        SOFT_STATS[0] += len(tl)
        P = np.array([[c[0] for c in t] for t in tl], dtype=np.float64)
        W = np.array([[c[1] for c in t] for t in tl], dtype=np.int64)
        E = np.stack([P[:, 1] - P[:, 0], P[:, 2] - P[:, 1], P[:, 0] - P[:, 2]], 1)
        N = np.cross(E[:, 0], -E[:, 2])
        area2 = np.linalg.norm(N, axis=1)
        N /= np.maximum(area2, 1e-12)[:, None]
        height = area2 / np.maximum(np.linalg.norm(E, axis=2).max(1), 1e-6)  # narrowest extent
        px, mpp, seen = np.empty(len(tl)), np.empty(len(tl)), np.empty(len(tl))
        for i in range(0, len(tl), 2048):
            V = S[None, :, :] - P[i:i + 2048].mean(1)[:, None, :]
            d = np.linalg.norm(V, axis=2)
            cos = np.abs((V * N[i:i + 2048, None, :]).sum(2)) / np.maximum(d, 1e-6)
            dd = np.maximum(d - mapchunks.CAM_REACH, floor)
            far = d > d.min(1, keepdims=True) + mapcfg.SOFT_REACH
            w = height[i:i + 2048, None] * cos * mapcfg.SOFT_FOCAL / dd
            w[far], dd[far], cos[far] = np.nan, np.nan, np.nan
            px[i:i + 2048] = np.nanmedian(w, 1)
            mpp[i:i + 2048] = np.nanmedian(dd, 1) / mapcfg.SOFT_FOCAL
            seen[i:i + 2048] = np.nanmean(cos, 1)
        C = np.array(e["wrgb"], dtype=np.float64)[W].mean(1) if len(tl) else np.zeros((0, 3))
        rows.append((P, W, C, area2 * 0.5 * seen, px, mpp))
    cen = np.concatenate([r[0].mean(1) for r in rows if len(r[0])])
    col = np.concatenate([r[2] for r in rows if len(r[0])])
    wt = np.concatenate([r[3] for r in rows if len(r[0])])
    G = 2.0  # m: grid of triangle centroids for the footprint search
    gk = np.floor(cen / G).astype(np.int64)
    lib = dsr_light.geom_lib()
    if lib is None:  # the numpy path's cell lists (a Python dict over every centroid: only built when it is used)
        grid = {}
        for i, k in enumerate(map(tuple, gk)):
            grid.setdefault(k, []).append(i)
        grid = {k: np.array(v) for k, v in grid.items()}
    else:  # the cells sorted by key, members ascending (as the grid lists), for geom.c soft_targets
        order = np.lexsort((gk[:, 2], gk[:, 1], gk[:, 0]))
        sk = gk[order]
        new = np.ones(len(sk), bool)
        new[1:] = (sk[1:] != sk[:-1]).any(1)
        ckey = np.ascontiguousarray(sk[new])
        cstart = np.append(np.nonzero(new)[0], len(sk)).astype(np.int64)
        cidx = np.ascontiguousarray(order, np.int64)
        cen_c, col_c, wt_c = (np.ascontiguousarray(a, np.float64) for a in (cen, col, wt))
    for e, (P, W, C, _, px, mpp) in zip(entries, rows):
        k = np.clip(1 - px / mapcfg.SOFT_PX, 0, 1) if len(P) else px
        if not len(P) or not k.any():
            continue
        tgt = C.copy()
        if lib is not None:
            q = np.nonzero(k > 0)[0]
            Cq = np.ascontiguousarray(((P[q, 0] + P[q, 1]) + P[q, 2]) / 3)  # P[i].mean(0)
            Rq = np.ascontiguousarray(mapcfg.SOFT_R_PX * mpp[q])
            out, ok = np.empty((len(q), 3)), np.zeros(len(q), np.uint8)
            if lib.soft_targets(len(q), Cq.ctypes.data, Rq.ctypes.data, len(cen_c), cen_c.ctypes.data,
                                col_c.ctypes.data, wt_c.ctypes.data, len(ckey), ckey.ctypes.data, cstart.ctypes.data,
                                cidx.ctypes.data, G, out.ctypes.data, ok.ctypes.data) < 0:
                raise MemoryError("soft_targets")
            tgt[q[ok == 1]] = out[ok == 1]
        else:  # numpy path (no C helpers)
            for i in np.nonzero(k > 0)[0]:
                c, r = P[i].mean(0), mapcfg.SOFT_R_PX * mpp[i]
                lo, hi = np.floor((c - r) / G).astype(int), np.floor((c + r) / G).astype(int)
                idx = [grid[q] for q in ((x, y, z) for x in range(lo[0], hi[0] + 1) for y in range(lo[1], hi[1] + 1)
                                         for z in range(lo[2], hi[2] + 1)) if q in grid]
                if not idx:
                    continue
                idx = np.concatenate(idx)
                idx = idx[((cen[idx] - c) ** 2).sum(1) <= r * r]
                if len(idx) and wt[idx].sum() > 0:
                    tgt[i] = (col[idx] * wt[idx, None]).sum(0) / wt[idx].sum()
        # per wedge: the area-weighted mean of its triangles' k and targets
        ta = np.linalg.norm(np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]), axis=1)
        nw = len(e["wrgb"])
        kw, ww, tw = np.zeros(nw), np.zeros(nw), np.zeros((nw, 3))
        for j in range(3):
            np.add.at(kw, W[:, j], k * ta)
            np.add.at(ww, W[:, j], ta)
            np.add.at(tw, W[:, j], tgt * ta[:, None])
        out = list(e["wrgb"])
        for w in np.nonzero((ww > 0) & (kw > 0))[0]:
            f, t = kw[w] / ww[w], tw[w] / ww[w]
            out[w] = [min(255, int(out[w][j] + f * (t[j] - out[w][j]) + 0.5)) for j in range(3)]
        e["wrgb"] = out
        SOFT_STATS[1] += int((k > 0).sum())


def soften_thin_c(L, entries, view_s, floor):
    """soften_thin in C (tools/mapx/soften.c, libmapx): the same widths, footprint colours and wedge colours as the
    numpy code with geom.c's soft_targets."""
    S = np.ascontiguousarray(view_s, dtype=np.float64)
    toff, woff, Ps, Ws, Cs = [0], [0], [], [], []
    for e in entries:
        tl = e["tris"]
        SOFT_STATS[0] += len(tl)
        P, W = mapx_native.tri_arrays(tl)
        Ps.append(P.reshape(-1, 9))
        Ws.append(W)
        Cs.append(np.asarray(e["wrgb"], dtype=np.int32).reshape(-1, 3))
        toff.append(toff[-1] + len(tl))
        woff.append(woff[-1] + len(Cs[-1]))
    P, W, C = (np.ascontiguousarray(np.concatenate(x)) for x in (Ps, Ws, Cs))
    toff, woff = np.array(toff, np.int64), np.array(woff, np.int64)
    out, changed = np.empty_like(C), np.zeros(len(entries), np.uint8)
    SOFT_STATS[1] += L.mx_soften_thin(len(entries), toff.ctypes.data, P.ctypes.data, W.ctypes.data, woff.ctypes.data,
                                      C.ctypes.data, len(S), S.ctypes.data, floor, mapcfg.SOFT_PX, mapcfg.SOFT_FOCAL, mapcfg.SOFT_REACH,
                                      mapcfg.SOFT_R_PX, mapchunks.CAM_REACH, out.ctypes.data, changed.ctypes.data)
    for i in np.nonzero(changed)[0].tolist():
        entries[i]["wrgb"] = out[woff[i]:woff[i + 1]]  # (n, 3) int32: build_chunks_c takes it as is


# far panorama (render-distance variant A): PANO_R metres from PANO_C ("x,y,z", default the spawn); PANO_ONLY=1 keeps
# only the triangles beyond it at full detail (the capture export for tools/pano.py), else they are dropped
PANO_R = float(os.environ.get("PANO_R", 0))
PANO_C = [float(v) for v in os.environ["PANO_C"].split(",")] if os.environ.get("PANO_C") else None
PANO_ONLY = os.environ.get("PANO_ONLY") == "1"
BACKDROP = os.environ.get("BACKDROP", "1") == "1"  # far-only cells as the backdrop (render-distance pick A2); BACKDROP=0: old cut
BACKDROP_SIDE = int(os.environ.get("BACKDROP_SIDE", 0))  # 0: no back-face pruning (A/B); 1 / -1: front = n.(s-p) > 0 / < 0


def far_cells(meshes, nav_v):
    """STREAM: the set of CHUNK_GRID cells (of triangle centroids) whose centre lies beyond STREAM_FAR of the navmesh."""
    import numpy as np
    L = mapx_native.lib()
    if L is not None:
        return far_cells_c(L, meshes, nav_v)
    keys = set()
    for m in meshes:
        if m["keep"]:
            continue
        for t in m.get("tris_in", m["tris"]):
            ps = [m["pos"][m["wpos"][w]] for w in t]
            keys.add(tuple(int(math.floor(sum(q[a] for q in ps) / 3 / mapcfg.CHUNK_GRID)) for a in range(3)))
    keys = sorted(keys)
    if not keys or not nav_v:
        return set()
    c = (np.array(keys, dtype=np.float64) + 0.5) * mapcfg.CHUNK_GRID
    nv = np.array(nav_v, dtype=np.float64)
    d2 = np.concatenate([((c[i:i + 256, None, :] - nv[None, :, :]) ** 2).sum(2).min(1) for i in range(0, len(c), 256)])
    far = {k for k, d in zip(keys, d2) if d > mapcfg.STREAM_FAR * mapcfg.STREAM_FAR}
    print(f"stream: {len(far)} of {len(keys)} cells beyond {mapcfg.STREAM_FAR:.0f} m of the navmesh (one resident level)")
    return far


def far_cells_c(L, meshes, nav_v):
    """far_cells in C (tools/mapx/cells.c, libmapx)"""
    import ctypes
    Ts = [np.zeros((0, 9))]
    for m in meshes:
        tl = m.get("tris_in", m["tris"])
        if m["keep"] or not tl:
            continue
        P, Wp = mapgather._mesh_np(m)
        Ts.append(P[Wp[mapgather._np_list(m, "tris", tl, np.intp, 3)]].reshape(-1, 9))
    T = np.ascontiguousarray(np.concatenate(Ts))
    if not len(T) or not nav_v:
        return set()
    nv = np.ascontiguousarray(nav_v, np.float64)
    out, nc = np.empty((len(T), 3), np.int64), ctypes.c_longlong()
    nf = L.mx_far_cells(len(T), T.ctypes.data, len(nv), nv.ctypes.data, mapcfg.CHUNK_GRID, mapcfg.STREAM_FAR, out.ctypes.data,
                        ctypes.byref(nc))
    far = set(map(tuple, out[:nf].tolist()))
    print(f"stream: {len(far)} of {nc.value} cells beyond {mapcfg.STREAM_FAR:.0f} m of the navmesh (one resident level)")
    return far


def struct_sat(eps_fixed, budget):
    """The structure weight's saturation (simplify.simplify sat) for a level: the largest threshold the spot budget
    solve reaches under its px cap (solve_budget's steps eps x 1.5^k), so a cell the solve takes to its cap simplifies
    exactly as without the weight and the weight never costs memory the budget needs; below the cap crease / corner
    collapses wait for the plain ones. No solve (budget None): no saturation (a global threshold fits the triangle
    count anyway). Threshold searched instead of fixed: the cap itself, rounded down to a 1.5 step (ASSUMED close)."""
    if not budget:
        return math.inf
    px_cap = budget[4]
    eps = eps_fixed if eps_fixed is not None else mapcfg.EPS
    if eps is None:
        return (px_cap / 568) ** 2 / 1.5
    K = 0
    while K < 12 and math.sqrt(eps * 1.5 ** (K + 1)) * 568 <= px_cap + 1e-6:  # solve_budget's K
        K += 1
    return eps * 1.5 ** K


def simplify_meshes(meshes, spawn, nav_v, nav_t, src="tris", out="tris", eps_fixed=None, floor=None, cells=False,
                    far=frozenset(), area_k=None, eps_far=None, budget=None, nav_s=None, env_pad=None, far_shape=False):
    """Quadric edge collapse (tools/native/simplify.c, the source of truth; tools/simplify.py is its frozen reference and
    the fallback below) of every non-grass mesh. Meshes that share vertex positions (2 mm)
    are welded into one group and simplified together, so seams between neighbouring meshes are ordinary edges
    whose vertices move for both sides at once (no cracks); every triangle keeps its own mesh's UVs / colours.
    One process per group. Cost = error^2 / view distance^2 (angular error^2); one global threshold is searched to
    fit BUDGET triangles (foliage not counted: map.c thins it by distance), or EPS (eps_fixed) if set. Each mesh's
    src triangles (wedge triples) become out = corners (position, wedge). cells (STREAM): each group is cut into
    CHUNK_GRID cells by triangle centroid, positions on the cuts are locked, and out + "_cell" gets each triangle's
    cell; floor: the view distance floor (NEAR); eps_far: the threshold of the far cells' jobs (else the same)."""
    import numpy as np
    import simplify
    import simplify_pool
    MX = mapx_native.lib()
    sat = struct_sat(eps_fixed, budget)
    if MX is not None and os.environ.get("SIMPLIFY_NATIVE") != "0" and nav_t:
        env_pad = mapcfg.SIMPLIFY_ENV_NEAR if env_pad is None else env_pad
        MX.mx_simp_envelope(env_pad)  # for the jobs mx_simp_new runs
        if mapcfg.SIMPLIFY_GUARD:
            MX.mx_simp_guard(mapcfg.SIMPLIFY_QMIN, math.cos(math.radians(mapcfg.SIMPLIFY_FLIP_DEG)))
            MX.mx_simp_shape(mapcfg.SIMPLIFY_VALIDATE_TOL or env_pad or 0.25)
        try:
            return simplify_meshes_c(MX, meshes, spawn, nav_v, nav_t, src, out, eps_fixed, floor, cells, far, area_k,
                                     eps_far, budget, nav_s, sat=sat, env_pad=env_pad, far_shape=far_shape)
        finally:
            MX.mx_simp_envelope(0.0)
            MX.mx_simp_guard(0.0, -2.0)
            MX.mx_simp_shape(0.0)
    if budget:
        print(f"simplify: no budget solve without the native simplifier ({budget[3]})")
    if mapcfg.SIMPLIFY_GUARD or env_pad or mapcfg.SIMPLIFY_ENV_NEAR:  # the fallback is simplify.py's reference collapse only
        why = "no navmesh" if MX is not None and os.environ.get("SIMPLIFY_NATIVE") != "0" else "libmapx off or missing"
        print(f"simplify: WARNING: Python fallback ({why}): no shape envelope, no triangle guard, unlike the C path")

    def key(p):
        return (round(p[0] * 500), round(p[1] * 500), round(p[2] * 500))  # 2 mm
    # key() of every position, per mesh at once (numpy rint rounds half to even like round())
    pkeys = {}

    def keys_of(mi):
        if mi not in pkeys:
            P = np.array(meshes[mi]["pos"], np.float64).reshape(-1, 3)
            pkeys[mi] = list(map(tuple, np.rint(P * 500).astype(np.int64).tolist()))
        return pkeys[mi]
    # identical copies (same texture, same triangles; Firelink has some far _L walls twice) only draw once
    seen = set()
    for mi, m in enumerate(meshes):
        if m["keep"]:
            continue
        kp, wp = keys_of(mi), m["wpos"]
        k = (m["ti"], frozenset(frozenset(kp[wp[w]] for w in t) for t in m[src]))
        if k in seen:
            m[src] = []
        seen.add(k)
    live = [mi for mi, m in enumerate(meshes) if not m["keep"] and m[src]]
    par = {mi: mi for mi in live}

    def root(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x
    first = {}
    for mi in live:
        for k in keys_of(mi):
            if k in first:
                par[root(mi)] = root(first[k])
            else:
                first[k] = mi
    groups = {}
    for mi in live:
        groups.setdefault(root(mi), []).append(mi)
    groups = list(groups.values())
    # per group: welded positions, attributes = (mesh, wedge), triangles in attribute indices
    jobs, attrs, jcell, jkeys = [], [], [], []
    for g in groups:
        percell = {}
        for mi in g:
            m = meshes[mi]
            if cells and m[src]:  # CHUNK_GRID cell of each triangle's centroid (sum() of the corners, as before)
                Pm = np.array(m["pos"], np.float64).reshape(-1, 3)[np.array(m["wpos"], np.int64)[np.array(m[src], np.int64)]]
                cc = np.floor(mapchunks._py_sum3(Pm[:, 0], Pm[:, 1], Pm[:, 2]) / 3 / mapcfg.CHUNK_GRID).astype(np.int64).tolist()
            for ti_, t in enumerate(m[src]):
                c = None
                if cells:
                    c = tuple(cc[ti_])
                    c = "far" if c in far else c  # far cells: one uncut job per welded group
                percell.setdefault(c, []).append((mi, t))
        for c, items in percell.items():
            pid, aid, pos, wpos, attr, tris, jk = {}, {}, [], [], [], [], []
            for mi, t in items:
                m = meshes[mi]
                kp = keys_of(mi)
                tt = []
                for w in t:
                    if (mi, w) not in aid:
                        k = kp[m["wpos"][w]]
                        if k not in pid:
                            pid[k] = len(pos)
                            pos.append(m["pos"][m["wpos"][w]])
                            jk.append(k)
                        aid[(mi, w)] = len(wpos)
                        wpos.append(pid[k])
                        attr.append((mi, w))
                    tt.append(aid[(mi, w)])
                tris.append(tuple(tt))
            jobs.append([pos, wpos, tris, None, None])
            attrs.append(attr)
            jcell.append(c)
            jkeys.append(jk)
    njobs = {}  # positions on a cell cut (in several jobs of one welded group) never move: both levels keep the cut
    for jk in jkeys:
        for k in jk:
            njobs[k] = njobs.get(k, 0) + 1
    for j, jk in zip(jobs, jkeys):
        j[3] = [njobs[k] > 1 for k in jk]
    dist = view_distance([j[0] for j in jobs], spawn, nav_v, nav_t, floor=floor)
    for j, d in zip(jobs, dist):
        j[4] = [1.0 / (x * x) for x in d]
        if simplify.STRUCT_W > 1.0 or simplify.STRUCT_FLAT_W != 1.0:  # crease / corner vertices first (struct_sat)
            j += [simplify.structure_weight(j[0], j[1], j[2]), sat]
    # cached by job content, run on forkserver workers that are retried when one dies (tools/simplify_pool.py;
    # SIMPLIFY_PROCS / SIMPLIFY_CACHE / SIMPLIFY_NATIVE)
    logs = simplify_pool.run_jobs(jobs, area_k=area_k)
    grass = sum(len(m[src]) for m in meshes if m["keep"] and not m["grass"])
    full = sum(len(lg[0]) for lg in logs)
    # every step of every group as (cost, triangles removed): the count at a threshold is one masked sum
    cost = np.concatenate([lg[1] for lg in logs] + [np.zeros(0)])
    gone = np.concatenate([lg[2] for lg in logs] + [np.zeros(0, np.int32)]).astype(np.int64)

    def count(eps):
        return grass + full - int(gone[cost <= eps].sum())
    print("simplify: tris at an error of " + ", ".join(f"{px} px {count((px / 568) ** 2)}" for px in (1, 2, 3, 4, 6, 8, 12, 24)))
    if eps_fixed is not None or mapcfg.EPS is not None:
        eps = eps_fixed if eps_fixed is not None else mapcfg.EPS
    else:
        lo, hi = 1e-16, 1.0
        while count(hi) > mapcfg.BUDGET and hi < 1e6:
            hi *= 100
        for _ in range(40):
            mid = math.sqrt(lo * hi)
            lo, hi = (lo, mid) if count(mid) <= mapcfg.BUDGET else (mid, hi)
        eps = hi
        if eps > mapcfg.EPS_MAX:  # BUDGET unreachable (Undead Burg: lightmapped pieces full of attribute seams / non-manifold
            # edges QEM must keep): the search ran to its cap and would collapse everything collapsible into spikes
            print(f"simplify: BUDGET {mapcfg.BUDGET} not reachable ({count(eps)} tris at any error); error capped at "
                  f"{math.sqrt(mapcfg.EPS_MAX) * 568:.0f} px -> {count(mapcfg.EPS_MAX)} tris")
            eps = mapcfg.EPS_MAX
    for mi in live:
        meshes[mi][out] = []
        meshes[mi][out + "_cell"] = []
    for (pos, *_), attr, lg, c in zip(jobs, attrs, logs, jcell):
        for p0, a0, p1, a1, p2, a2 in simplify.replay_log(lg, eps_far if c == "far" and eps_far is not None else eps).tolist():
            mi = attr[a0][0]
            meshes[mi][out].append(((pos[p0], attr[a0][1]), (pos[p1], attr[a1][1]), (pos[p2], attr[a2][1])))
            meshes[mi][out + "_cell"].append(c)
    for m in meshes:
        if m["keep"]:
            m[out] = [tuple((m["pos"][m["wpos"][w]], w) for w in t) for t in m[src]]
        elif not m[src]:
            m[out] = []
    print(f"simplify: {full} -> {count(eps)} tris ({grass} kept: effects, sky; foliage extra) in {len(groups)} welded "
          f"groups / {len(jobs)} jobs, eps {eps:.3g} "
          f"(error <= {math.sqrt(max(eps, 0)) * 568:.1f} px at 640x448)")
    return eps


# CUT_CELLS (Burg plan B3, default on; CUT_CELLS=0 = triangles grouped by centroid only, as before): before the two
# simplify passes, every simplified mesh's triangles are cut at the CHUNK_GRID planes, so each piece lies inside one
# cell's cube and a cell's box is its cube (the Undead Burg's near cells reached 12 m past it at the median, up to 74 m:
# big wall / ground triangles; mapstream.c / map.c then wanted and drew near levels ~1.3x farther than the export
# budgets assumed, TECH_DEBT world-10). The cut points are new vertices on the plane (shared by both sides, so the
# simplifier's cell cuts lock them: no cracks), with UV, colour, alpha and relief frame interpolated along the edge.
CUT_CELLS = os.environ.get("CUT_CELLS", "1") == "1"
CUT_EPS = 1e-4  # m: a corner this close to a plane counts as on it (not cut again)
# only triangles reaching more than CUT_MIN m past their centroid's cube are cut: every cut locks the cut points in both
# simplify passes, and cutting every crossing triangle of the Undead Burg's tower block made its coarse level 25 % and
# its near level 6 % bigger (2026-11-08); a cell's box then reaches at most CUT_MIN past its cube
CUT_MIN = float(os.environ.get("CUT_MIN", 4.0))
CUT_STATS = [0, 0, 0]  # meshes cut, triangles cut, pieces they became


def cut_cells(meshes):
    """CUT_CELLS: cut the triangles of every non-kept mesh at the CHUNK_GRID planes (x, then y, then z), in place:
    m["pos"], m["wpos"], m["tris"], m["wuv"], m["wrgb"], m["wa"] and the relief frames (export_normal.MESH) grow by the
    cut points; a triangle's pieces take its place in the list, in its winding."""
    import numpy as np
    G = mapcfg.CHUNK_GRID
    for m in meshes:
        if m["keep"] or not len(m["tris"]):
            continue
        arr = lambda x, w, dt=np.float64: np.asarray(x.a if isinstance(x, souls.Rows) else x, dt).reshape(-1, w) \
            if len(x) else np.zeros((0, w), dt)
        P = arr(m["pos"], 3)
        WP = arr(m["wpos"], 1, np.int64).reshape(-1)
        T = arr(m["tris"], 3, np.int64)
        X = P[WP[T]]  # (tris, corners, xyz)
        c0 = np.floor(X.sum(1) / 3 / G) * G  # the cube of each centroid (where the simplifier groups it)
        over = np.maximum(c0 - X.min(1), X.max(1) - (c0 + G)).max(1)
        span = np.flatnonzero(over > CUT_MIN)
        if not len(span):
            continue
        nw0 = len(WP)
        UV = arr(m["wuv"], 2)
        RGB = arr(m["wrgb"], 3) if len(m["wrgb"]) else np.zeros((nw0, 3))
        A = np.asarray(m["wa"], np.float64).reshape(-1) if len(m["wa"]) else np.zeros(nw0)
        B = export_normal.MESH[m["bump"]] if m.get("bump") is not None else None
        P_l, WP_l, UV_l, RGB_l, A_l = list(map(tuple, P.tolist())), WP.tolist(), UV.tolist(), RGB.tolist(), A.tolist()
        BN_l = B["N"].tolist() if B is not None else None
        BT_l = B["T"].tolist() if B is not None else None
        BW_l = B["W"].tolist() if B is not None else None
        pkey, wkey = {}, {}

        def point(wi, wj, a, v):
            """the cut point of edge (wi, wj) on plane a = v: one position per position pair, one wedge per wedge pair"""
            pi_, pj_ = WP_l[wi], WP_l[wj]
            if pi_ > pj_ or (pi_ == pj_ and wi > wj):
                wi, wj, pi_, pj_ = wj, wi, pj_, pi_
            k = (pi_, pj_, a, v)
            p = pkey.get(k)
            qa, qb = P_l[pi_], P_l[pj_]
            t = (v - qa[a]) / (qb[a] - qa[a])
            if p is None:
                q = [qa[c] + t * (qb[c] - qa[c]) for c in range(3)]
                q[a] = v
                p = pkey[k] = len(P_l)
                P_l.append(tuple(q))
            kw = (wi, wj, a, v)
            w = wkey.get(kw)
            if w is None:
                w = wkey[kw] = len(WP_l)
                WP_l.append(p)
                lerp = lambda L: [x + t * (y - x) for x, y in zip(L[wi], L[wj])]
                UV_l.append(lerp(UV_l))
                RGB_l.append(lerp(RGB_l))
                A_l.append(A_l[wi] + t * (A_l[wj] - A_l[wi]))
                if BN_l is not None:
                    for L in (BN_l, BT_l):
                        n = lerp(L)
                        ln = math.sqrt(sum(c * c for c in n)) or 1.0
                        L.append([c / ln for c in n])
                    BW_l.append(BW_l[wi])
            return w

        def split(tri, a, v):
            s = [P_l[WP_l[w]][a] - v for w in tri]
            side = [0 if abs(x) <= CUT_EPS else (1 if x > 0 else -1) for x in s]
            if min(side) >= 0 or max(side) <= 0:
                return [tri]
            if 0 in side:  # one corner on the plane, the others on either side: two pieces
                r = side.index(0)
                A_, B_, C_ = tri[r], tri[(r + 1) % 3], tri[(r + 2) % 3]
                p = point(B_, C_, a, v)
                return [(A_, B_, p), (A_, p, C_)]
            r = next(i for i in range(3) if side[i] != side[(i + 1) % 3] and side[i] != side[(i + 2) % 3])
            A_, B_, C_ = tri[r], tri[(r + 1) % 3], tri[(r + 2) % 3]  # A alone on its side
            pab, pac = point(A_, B_, a, v), point(A_, C_, a, v)
            return [(A_, pab, pac), (pab, B_, C_), (pab, C_, pac)]

        def cut(tri, a):
            xs = [P_l[WP_l[w]][a] for w in tri]
            v = (math.floor((min(xs) + CUT_EPS) / G) + 1) * G
            if v >= max(xs) - CUT_EPS:
                return [tri]
            out = []
            for piece in split(tri, a, v):
                out += [piece] if piece == tri else cut(piece, a)
            return out
        Tl = T.tolist()
        rep = {}
        for i in span.tolist():
            q = [tuple(Tl[i])]
            for a in range(3):
                q = [r for t in q for r in cut(t, a)]
            rep[i] = q
        pieces = sum(len(q) for q in rep.values())
        Tn = [t for i, x in enumerate(Tl) for t in rep.get(i, (x,))]
        CUT_STATS[0] += 1
        CUT_STATS[1] += len(span)
        CUT_STATS[2] += pieces
        m["pos"] = souls.Rows(np.ascontiguousarray(np.array(P_l, np.float64)))
        m["wpos"] = WP_l
        m["tris"] = souls.Rows(np.ascontiguousarray(np.array(Tn, np.int32)))
        m["wuv"] = souls.Rows(np.ascontiguousarray(np.array(UV_l, np.float64)))
        m["wrgb"] = [[int(round(c)) for c in x] for x in RGB_l]
        m["wa"] = [int(round(x)) for x in A_l]
        m.pop("_np", None)  # gather's arrays of the old lists
        if B is not None:
            B["N"], B["T"], B["W"] = np.array(BN_l, np.float64), np.array(BT_l, np.float64), np.array(BW_l, np.float64)
    if CUT_STATS[0]:
        print(f"cut cells: {CUT_STATS[1]} triangles of {CUT_STATS[0]} meshes crossed a {G:.0f} m plane -> "
              f"{CUT_STATS[2]} pieces")


def cell_extents(P, poff, W, woff, T, toff):
    """{CHUNK_GRID cell key: (min xyz, max xyz)} of the input triangles grouped by centroid (simplify_meshes_c's
    flat arrays): the box mapstream.c and map.c measure a cell's distance by (its chunks' box) is this, not the cell's
    cube. Triangles are grouped by centroid, not cut, so the Undead Burg's near cells reach 12 m (median, up to 74 m)
    past their cube; a spot budget over cubes saw a near level within ~26 + 12 m as if within 26 m (2026-11-08)."""
    import numpy as np
    if not len(T):
        return {}
    wm = np.repeat(np.arange(len(woff) - 1), np.diff(woff))
    tm = np.repeat(np.arange(len(toff) - 1), np.diff(toff))
    wg = W.reshape(-1).astype(np.int64) + poff[wm]
    V = P[wg[T.astype(np.int64) + woff[tm][:, None]]]  # (tris, 3 corners, xyz)
    lo, hi = V.min(1), V.max(1)
    key = np.floor((V[:, 0] + V[:, 1] + V[:, 2]) / 3 / mapcfg.CHUNK_GRID).astype(np.int64)
    u, inv = np.unique(key, axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    mn = np.full((len(u), 3), np.inf)
    mx = np.full((len(u), 3), -np.inf)
    np.minimum.at(mn, inv, lo)
    np.maximum.at(mx, inv, hi)
    return {tuple(k): (a, b) for k, a, b in zip(u.tolist(), mn, mx)}


_NAV_VOX = {}  # map id -> set of dilated 8 m voxels of its navmesh (ws_shares)


def _nav_voxels(game, map_id, rects=None):
    """the 8 m voxels within one voxel of map_id's navmesh vertices (inside rects, xz metres, if given)"""
    k = (map_id, tuple(rects or ()))
    if k not in _NAV_VOX:
        M = souls.msb1(souls.read(f"{game}/map/MapStudio/{map_id}.msb"))
        pts = np.asarray(mapgather.navmesh_points(game, map_id, M, lambda *q: True) or np.zeros((0, 3)), np.float64).reshape(-1, 3)
        if rects:
            ok = np.zeros(len(pts), bool)
            for x0, z0, x1, z1 in rects:
                ok |= (pts[:, 0] >= x0) & (pts[:, 0] < x1) & (pts[:, 2] >= z0) & (pts[:, 2] < z1)
            pts = pts[ok]
        v = set(map(tuple, np.floor(pts / 8.0).astype(np.int64).tolist()))
        _NAV_VOX[k] = {(x + a, y + b, z + c) for x, y, z in v for a in (-1, 0, 1) for b in (-1, 0, 1) for c in (-1, 0, 1)}
    return _NAV_VOX[k]


def ws_foreign_spots(game, map_id, nav_v, reach=64.0, most=2000):
    """WS_UNION: navmesh vertices of the other MSBs of this export's world (Firelink next to the Burg) within reach
    metres of this export's own navmesh: a seam's spots are solved from both sides"""
    b = area_mod.block()
    name, cfg = b if b else (map_id, {"map": map_id})
    world = next((w for w in area_mod.WORLDS.values() if name in w), [name])
    own = np.asarray(nav_v, np.float64).reshape(-1, 3)
    if not len(own):
        return []
    vox = set(map(tuple, np.floor(own / reach).astype(np.int64).tolist()))
    near = {(x + a, y + c, z + d) for x, y, z in vox for a in (-1, 0, 1) for c in (-1, 0, 1) for d in (-1, 0, 1)}
    out = []
    for omap in sorted({(area_mod.BLOCKS[o]["map"] if o in area_mod.BLOCKS else o) for o in world} - {cfg["map"]}):
        M = souls.msb1(souls.read(f"{game}/map/MapStudio/{omap}.msb"))
        pts = np.asarray(mapgather.navmesh_points(game, omap, M, lambda *q: True) or np.zeros((0, 3)), np.float64).reshape(-1, 3)
        pts = pts[[tuple(v) in near for v in np.floor(pts / reach).astype(np.int64).tolist()]] if len(pts) else pts
        if len(pts):
            d = np.full(len(pts), np.inf)
            for a in range(0, len(own), 1024):
                d = np.minimum(d, np.sqrt(((pts[:, None] - own[None, a:a + 1024]) ** 2).sum(2)).min(1))
            pts = pts[d <= reach]
        out += [tuple(p) for p in pts[::max(1, len(pts) // most)].tolist()]
    if out:
        print(f"budget: {len(out)} spots of other maps within {reach:.0f} m share the room (WS_UNION)")
    return out


def ws_shares(P, r, game=None, map_id=None, nav_v=None):
    """WS_UNION: per spot (P: (m, 3)) the share of the disc of radius r (xz at the spot's height, sampled every 4 m)
    that this export covers, of what it and the other blocks of its world (tools/area.py WORLDS) cover together. A
    block of the same MSB with cells covers its cells (tools/area.py BLOCKS); another MSB (Firelink, the Asylum) or a
    block without cells covers the 8 m voxels around its navmesh. 1 where nothing covers the disc."""
    b = area_mod.block()
    name, cfg = b if b else (map_id, {"map": map_id})
    world = next((w for w in area_mod.WORLDS.values() if name in w), [name])
    g = np.arange(-r, r + 1e-6, 4.0)
    gx, gz = np.meshgrid(g, g)
    m = gx * gx + gz * gz <= r * r
    X = P[:, None, 0] + gx[m][None]
    Y = np.broadcast_to(P[:, None, 1], X.shape)
    Z = P[:, None, 2] + gz[m][None]
    C = area_mod.CELL
    rects = lambda c: [tuple(c)] if isinstance(c[0], (int, float)) else [tuple(x) for x in c]
    to_m = lambda rs: [(x0 * C, z0 * C, (x1 + 1) * C, (z1 + 1) * C) for x0, z0, x1, z1 in rs]

    def by_rects(rs):
        out = np.zeros(X.shape, bool)
        for x0, z0, x1, z1 in to_m(rs):
            out |= (X >= x0) & (X < x1) & (Z >= z0) & (Z < z1)
        return out

    def by_nav(vox):
        V = np.floor(np.stack([X, Y, Z], -1) / 8.0).astype(np.int64)
        flat = V.reshape(-1, 3)
        return np.fromiter(((x, y, z) in vox for x, y, z in flat.tolist()), bool, len(flat)).reshape(X.shape)
    if cfg.get("cells"):
        own = by_rects(rects(cfg["cells"]))
    elif nav_v is not None and len(nav_v):
        pts = np.asarray(nav_v, np.float64).reshape(-1, 3)
        v = set(map(tuple, np.floor(pts / 8.0).astype(np.int64).tolist()))
        own = by_nav({(x + a, y + c, z + d) for x, y, z in v for a in (-1, 0, 1) for c in (-1, 0, 1) for d in (-1, 0, 1)})
    else:
        return np.ones(len(P))
    oth = np.zeros(X.shape, bool)
    for o in world:
        if o == name:
            continue
        ob = area_mod.BLOCKS.get(o)
        omap = ob["map"] if ob else o
        if ob and ob.get("cells") and omap == cfg["map"] and cfg.get("cells"):
            oth |= by_rects(rects(ob["cells"]))
        elif game:
            oth |= by_nav(_nav_voxels(game, omap, to_m(rects(ob["cells"])) if ob and ob.get("cells") else None))
    no, nt = own.sum(1), (own | oth).sum(1)
    if mapcfg.WS_SHARE_TRIS and cfg.get("cells") and mapcfg.WS_SRC_TRIS:
        # Shares by demand (2026-11-12). A cell is in a spot's tier with its whole bytes as soon as its box comes
        # within r, and the Burg's detail is not spread evenly: one cell by the bonfire holds 628 KB of near level
        # where its neighbours hold 2-130 KB. By the area of the disc, a spot 15 m inside the next block gave this
        # block 1-17 % of the room for that cell: the spot stayed over at any error, and the solve took the cells at
        # every seam to the allowance (31 of 85 near cells of the Burg west block past 12 px, up to 68 px) while
        # 6-25 % of the room stayed unused at the tightest spot each of them sees (world_budget.py's sums). The
        # blocks of this MSB now split the room by the source triangles of their cells whose 32 m cube lies within r;
        # another MSB's part (Firelink, the Asylum) is taken off first, by area as before.
        own_rs = rects(cfg["cells"])
        all_rs = list(own_rs)
        msb = own.copy()
        for o in world:
            ob = area_mod.BLOCKS.get(o)
            if o != name and ob and ob.get("cells") and ob["map"] == cfg["map"]:
                all_rs += rects(ob["cells"])
                msb |= by_rects(rects(ob["cells"]))
        inr = lambda rs, cx, cz: any(x0 <= cx <= x1 and z0 <= cz <= z1 for x0, z0, x1, z1 in rs)
        to, ta = np.zeros(len(P)), np.zeros(len(P))
        for (cx, cy, cz), n in mapcfg.WS_SRC_TRIS.items():
            if not inr(all_rs, cx, cz):
                continue
            lo_ = np.array([cx, cy, cz], np.float64) * C
            dd = np.maximum(0, np.maximum(lo_[None] - P, P - (lo_ + C)[None]))
            m_ = (dd * dd).sum(1) < r * r
            ta += m_ * n
            if inr(own_rs, cx, cz):
                to += m_ * n
        part = np.where(nt > 0, msb.sum(1) / np.maximum(nt, 1), 1.0)  # this MSB's part of the room, by area
        sh = np.where(ta > 0, to / np.maximum(ta, 1.0), np.where(nt > 0, no / np.maximum(nt, 1), 1.0))
        # (for solve_budget's own count, ws_reach_shares: the neighbours' triangles in reach and the MSB's part)
        WS_REACH[float(r)] = dict(n=len(P), oth=ta - to, part=part, area=np.where(nt > 0, no / np.maximum(nt, 1), 1.0))
        return np.minimum(1.0, sh * part * mapcfg.WS_SHARE_K)
    return np.minimum(1.0, np.where(nt > 0, no / np.maximum(nt, 1), 1.0) * mapcfg.WS_SHARE_K)


WS_REACH = {}  # ws_shares(r): per spot the neighbour blocks' source triangles in reach, this MSB's part of the room


def ws_reach_shares(M, cells, r):
    """WS_SHARE_TRIS: the shares of ws_shares(r) with this block's side counted as the solve reaches its cells (M:
    spots x cells, by their real boxes: a coarse cell's box overhangs its 32 m cube, and by the cube alone a spot that
    holds the cell had no share for it); None when ws_shares(r) did not run by triangles"""
    import numpy as np
    w = WS_REACH.get(float(r))
    if w is None or w["n"] != M.shape[0]:
        return None
    n = np.array([mapcfg.WS_SRC_TRIS.get(tuple(int(v) for v in c), 0) for c in cells], np.float64)
    to = M.astype(np.float64) @ n
    ta = to + w["oth"]
    return np.minimum(1.0, np.where(ta > 0, to / np.maximum(ta, 1.0), w["area"]) * w["part"] * mapcfg.WS_SHARE_K)


def solve_budget(MX, h, jc, eps, eps_far, budget, samples, extents=None, info=None, drop=None):
    """World streaming v2: per-job thresholds so that at every sample point (navmesh) the bytes of the level's cells
    within [r_in, r) (r None: every cell) stay under nbytes. Each cell's threshold is eps x 1.5^k; while a point is over,
    the heaviest cell around it that can still be raised goes one step up. Jobs outside cells (no cell cut, far-only
    cells) keep eps / eps_far. budget = (r_in, r, nbytes, name). Returns the per-job thresholds (float64) or None.
    drop (WS_DROP, loose_pieces): a cell's loose pieces whose error is within its threshold are left out, and past the
    cap the steps raise that piece threshold alone (to WS_DROP_PX) before the seam allowance raises the collapse's;
    drop["cell_de"] gets each cell's piece threshold, drop["jcell"] each job's cell."""
    import numpy as np
    r_in, r, nbytes, name, px_cap = budget
    njobs = len(jc)
    cells = sorted({c for c in jc if isinstance(c, tuple)})
    if not cells or not len(samples):
        return None
    cid = {c: i for i, c in enumerate(cells)}
    jcell = np.array([cid[c] if isinstance(c, tuple) else -1 for c in jc], np.int64)
    # at most px_cap px of error: past it the spot stays over budget (mapstream drops its farthest near cells first).
    # Without a cap the near level of dense Burg spots went to 130x the threshold (68 px) and still did not fit: the
    # simplifier keeps open borders, UV seams and small parts, so a spot's bytes stop falling (first export 2026-11-06)
    K = 0
    while K < 12 and math.sqrt(eps * 1.5 ** (K + 1)) * 568 <= px_cap + 1e-6:
        K += 1
    KA = K  # the seam allowance's cap (WS_UNION: WS_ALLOW_PX)
    while mapcfg.WS_UNION and KA < 16 and math.sqrt(eps * 1.5 ** (KA + 1)) * 568 <= max(px_cap, mapcfg.WS_ALLOW_PX) + 1e-6:
        KA += 1
    # the ladder: per step the collapse threshold ce and the piece threshold de. To the cap both rise together, then
    # (KD steps) the pieces' alone, then the seam allowance's collapse steps
    KD = 0
    while drop and KD < 16 and math.sqrt(eps * 1.5 ** (K + KD + 1)) * 568 <= max(px_cap, mapcfg.WS_DROP_PX) + 1e-6:
        KD += 1
    ce = [eps * 1.5 ** k for k in range(K + 1)] + [eps * 1.5 ** K] * KD + [eps * 1.5 ** k for k in range(K + 1, KA + 1)]
    de = [eps * 1.5 ** k for k in range(K + KD + 1)] + [eps * 1.5 ** max(K + KD, k) for k in range(K + 1, KA + 1)]
    KC, K, KA = K, K + KD, KA + KD  # KC: the collapse cap's step; K / KA from here on: the last step before / of the allowance
    steps = ce
    cnt = np.zeros(max(njobs, 1), np.int64)
    by = np.zeros((len(cells), KA + 1))  # bytes of cell c at step k
    sel = jcell >= 0
    if drop:  # per collapse threshold: the surviving triangles' cells and pieces, counted without the pieces within de
        for e in sorted(set(ce)):
            tj, g0 = _fetch_first(MX, h, int(MX.mx_simp_replay(h, e, e, 0)), drop["poff"])
            c, cost = jcell[tj], drop["cost"][drop["pp"][g0]]
            for k in [k for k in range(KA + 1) if ce[k] == e]:
                m = (c >= 0) & (cost > de[k])
                by[:, k] = np.bincount(c[m], minlength=len(cells)) * mapcfg.WS_BPT
    for k, e in enumerate(steps if not drop else ()):
        MX.mx_simp_job_counts(h, e, cnt.ctypes.data)
        by[:, k] = np.bincount(jcell[sel], weights=cnt[:njobs][sel], minlength=len(cells)) * mapcfg.WS_BPT
    lo = np.array(cells, np.float64) * mapcfg.CHUNK_GRID
    hi = lo + mapcfg.CHUNK_GRID
    if extents and mapcfg.WS_EXTENTS:  # the boxes the runtime measures (cell_extents): the near level's clipped to its cube
        near = r_in == 0.0  # (NEAR_CUBE, mapstream.h cell_near_d2), the coarse level's whole
        for i, c in enumerate(cells):
            e = extents.get(c)
            if e is None:
                continue
            if near and mapcells.NEAR_CUBE:
                lo[i], hi[i] = np.maximum(lo[i], e[0]), np.minimum(hi[i], e[1])
                hi[i] = np.maximum(hi[i], lo[i])
            else:
                lo[i], hi[i] = np.minimum(lo[i], e[0]), np.maximum(hi[i], e[1])
    P = np.asarray(samples, np.float64).reshape(-1, 3)
    d = np.sqrt((np.maximum(0, np.maximum(lo[None] - P[:, None], P[:, None] - hi[None])) ** 2).sum(2))
    M = (d >= r_in) & (d < r) if r else np.ones(d.shape, bool)
    if callable(nbytes):  # (WS_SHARE_TRIS: the spot's budget from the block's share by its cells in reach)
        nbytes = nbytes(ws_reach_shares(M | (d < r), cells, r) if r else None)
    nb = np.broadcast_to(np.asarray(nbytes, np.float64), (len(P),))  # per spot (WS_UNION) or one for all
    Mf = M.astype(np.float64)
    step = np.zeros(len(cells), np.int64)
    cur = by[np.arange(len(cells)), step]
    W = Mf @ cur
    w0, raised, stuck = W.max(), 0, False
    # spots whose cells are all at the cap: left over budget, and the solve goes on with the next worst spot. It used
    # to stop at the first such spot, so every other spot over budget kept its cells at the threshold (Undead Burg
    # 2026-11-08: 14 of 60 near cells raised in the Burg block, 25 % of its spots over budget -> 41 cells, 14 %)
    capped = np.zeros(len(W), bool)
    while raised < 100000:
        p = int(np.where(capped, -np.inf, W - nb).argmax())  # the spot most over its budget
        if capped[p] or W[p] <= nb[p]:
            break
        cand = np.flatnonzero(M[p] & (step < K) & (cur > 0))
        if not len(cand):
            stuck = capped[p] = True
            continue
        # the least simplified cells of the spot go up together: the spot's error evens out (one cell taken to 130x
        # the threshold while its neighbours kept theirs looked broken next to them; first export 2026-11-06)
        cand = cand[step[cand] == step[cand].min()]
        for c in cand.tolist():
            step[c] += 1
            new = by[c, step[c]]
            W -= Mf[:, c] * (cur[c] - new)
            cur[c] = new
        raised += 1
    allow = np.zeros(len(W), bool)  # spots that took the seam allowance (past the cap)
    while KA > K and raised < 200000:
        p = int(np.where(allow | ~capped, -np.inf, W - nb).argmax())
        if not capped[p] or allow[p] or W[p] <= nb[p]:
            break
        cand = np.flatnonzero(M[p] & (step < KA) & (cur > 0))
        # (WS_DROP: a spot that would stay over with every cell at the allowance's end bends nothing. The Undead Burg's
        # seam spots with a small share of the room took 29 near cells to 68 px for budgets no step could meet)
        if not len(cand) or (drop and Mf[p] @ np.where(step < KA, by[:, KA], cur) > nb[p]):
            allow[p] = True  # (no step left, or none that would do: stays over)
            continue
        cand = cand[step[cand] == step[cand].min()]
        for c in cand.tolist():
            step[c] += 1
            new = by[c, step[c]]
            W -= Mf[:, c] * (cur[c] - new)
            cur[c] = new
        raised += 1
        if W[p] <= nb[p]:
            allow[p] = True
    mine = nb >= nb.max() * 0.999 if len(nb) else nb > 0  # the block's own spots (its whole share of the room)
    allow_pct = float(allow[mine].mean() * 100) if mine.any() else 0.0
    own_over = float((W > nb)[mine].mean() * 100) if mine.any() else 0.0  # still over after the solve
    if allow.any():
        hot = sorted({cells[c] for c in np.flatnonzero(step > K).tolist()})
        print(f"budget {name}: seam allowance (past {px_cap:.0f} px, up to {mapcfg.WS_ALLOW_PX:.0f} px) at "
              f"{allow_pct:.1f} % of its own spots ({allow.mean() * 100:.1f} % of all), cells {hot}")
    job_eps = np.array([steps[step[jcell[i]]] if jcell[i] >= 0 else (eps_far if jc[i] == "far" and eps_far is not None
                                                                       else eps) for i in range(njobs)], np.float64)
    if info is not None:  # per cell: in reach of a spot the solve left over budget; the spots' reach and budgets
        overc = M[W > nb].any(0) if (W > nb).any() else np.zeros(len(cells), bool)
        info.update(over=overc, M=M, nb=nb, jcell=jcell)
    if drop:
        drop["cell_de"], drop["jcell"] = np.array(de)[step], jcell
        print(f"budget {name}: loose pieces: {int((step > KC).sum())} of {len(cells)} cells past the {px_cap:.0f} px cap for "
              f"pieces only (up to {math.sqrt(de[min(int(step.max()), K)]) * 568:.0f} px), {int((step > K).sum())} on the seam allowance")
    nr = int((step > 0).sum())
    over = float((W > nb).mean() * 100)
    nbytes = float(np.median(nb))
    print(f"budget {name}: worst spot {w0 / 1024:.0f} -> {W.max() / 1024:.0f} KB (budget {nbytes / 1024:.0f} KB"
          f"{' median' if nb.min() < nb.max() else ''}, "
          f"{len(P)} navmesh points); {nr} of {len(cells)} cells coarser (up to {steps[step.max()] / eps:.1f}x the threshold, "
          f"{math.sqrt(steps[step.max()]) * 568:.1f} px)" +
          (f"; {over:.1f} % of the spots over budget at the {px_cap:.0f} px cap" if stuck else ""))
    BUDGET_REPORT.append(dict(level=name, allow_pct=round(allow_pct, 2),
                              allow_spots=[[round(float(v), 1) for v in P[i]] + [round(float(W[i]) / 1024)]
                                           for i in np.flatnonzero(allow & mine)[:500].tolist()],
                              allow_cells=[list(cells[c]) for c in np.flatnonzero(step > K).tolist()],
                              worst_kb=round(w0 / 1024), after_kb=round(W.max() / 1024),
                              budget_kb=round(nbytes / 1024), cells=len(cells), coarser=nr, max_x=steps[int(step.max())] / eps,
                              met=not stuck, spots_over_pct=round(over, 1), own_over_pct=round(own_over, 2),
                              own_spots=int(mine.sum()),
                              p90_kb=round(float(np.percentile(W, 90)) / 1024)))
    return job_eps


def _fetch_first(MX, h, n, poff):
    """the last replay's n triangles: each one's job and the global position index of its first corner"""
    tj, tm = np.zeros(max(n, 1), np.int32), np.zeros(max(n, 1), np.int32)
    cm, cp, cw = (np.zeros(max(3 * n, 1), np.int32) for _ in range(3))
    MX.mx_simp_fetch(h, tj.ctypes.data, tm.ctypes.data, cm.ctypes.data, cp.ctypes.data, cw.ctypes.data)
    return tj[:n], (poff[cm[:3 * n:3]] + cp[:3 * n:3])


def loose_pieces(MX, meshes, P, poff, W, woff, T, toff, keep, nav_v, nav_t, spawn, floor):
    """WS_DROP: the loose pieces of the simplifier's input (tools/mapx/pieces.c) -> dict(pp: piece per position,
    cost: per piece the squared angular error of leaving its cluster out (inf: structure), owner: per piece the
    CHUNK_GRID cell key of its cluster's centre). The view distance is the simplifier's (view_distance), the nearest over the cluster."""
    t0 = time.time()
    a = lambda x: x.ctypes.data
    nt = max(len(T), 1)
    pp, pd, pb, pr = np.zeros(max(len(P), 1), np.int32), np.zeros(nt), np.zeros((nt, 6)), np.zeros(nt)
    pcl = np.zeros(nt, np.int32)
    solo = np.array([1 if m.get("cutout") else 0 for m in meshes] or [0], np.uint8)
    n = int(MX.mx_pieces(len(poff) - 1, a(P), a(poff), a(W), a(woff), a(T), a(toff), a(keep), a(solo), mapcfg.WS_DROP_DIAG,
                         mapcfg.WS_DROP_CAP, mapcfg.WS_DROP_GAP, a(pp), a(pd), a(pb), a(pcl), a(pr)))
    pd, pb, pr, pcl = pd[:n], pb[:n], pr[:n], pcl[:n]
    live = np.flatnonzero(pp[:len(P)] >= 0)
    d = np.asarray(view_distance([P[live].tolist()], spawn, nav_v, nav_t, floor=floor)[0]) if len(live) else np.zeros(0)
    dist = np.full(n, np.inf)
    np.minimum.at(dist, pcl[pp[live]], d)
    lo, hi, prc = np.full((n, 3), np.inf), np.full((n, 3), -np.inf), np.full(n, -1.0)
    np.minimum.at(lo, pcl, pb[:, :3])
    np.maximum.at(hi, pcl, pb[:, 3:])
    np.maximum.at(prc, pcl, pr)
    size = np.sqrt(((hi - lo) ** 2).sum(1))
    with np.errstate(invalid="ignore", divide="ignore"):
        err = np.sqrt(np.maximum(prc, mapcfg.WS_DROP_FLUSH * size) * size)  # metres, per cluster (at its first piece)
        cost = np.where(pr >= 0, ((err / dist) ** 2)[pcl], np.inf)
    owner = np.floor(((lo + hi) / 2)[pcl] / mapcfg.CHUNK_GRID).astype(np.int64)
    print(f"pieces: {n} ({int((pr >= 0).sum())} loose, at most {mapcfg.WS_DROP_DIAG:g} m across, in "
          f"{len(np.unique(pcl[pr >= 0]))} clusters), {time.time() - t0:.1f} s", flush=True)
    return dict(pp=pp, cost=cost, owner=owner, poff=poff)


BUDGET_REPORT = []  # solve_budget results, written next to map.bin (map.bin.budget.json)


def simplify_meshes_c(MX, meshes, spawn, nav_v, nav_t, src, out, eps_fixed, floor, cells, far, area_k, eps_far,
                      budget=None, nav_s=None, sat=math.inf, env_pad=0.0, far_shape=False):
    """simplify_meshes in C (tools/mapx/simpmesh.c): the same groups, jobs, view distances, threshold and output, the
    jobs run on threads in process (no job cache: tools/native/simplify.c is the simplifier either way)"""
    import ctypes
    import numpy as np
    import simplify
    i32, i64, f64 = np.int32, np.int64, np.float64

    def flat(rows, dt, w):
        return np.ascontiguousarray(np.array(rows, dt).reshape(-1, w)) if rows else np.zeros((0, w), dt)
    nm = len(meshes)
    PW = [mapgather._mesh_np(m) for m in meshes]
    Ps, Ws = [p for p, _ in PW], [w for _, w in PW]
    Ts = [mapgather._np_list(m, "tris", m[src], i32, 3) for m in meshes]
    off = lambda xs: np.concatenate([[0], np.cumsum([len(x) for x in xs])]).astype(i64)
    cat = lambda xs, dt, w: np.ascontiguousarray(np.concatenate(xs).astype(dt)) if xs else np.zeros((0, w), dt)
    P, poff, W, woff, T, toff = cat(Ps, f64, 3), off(Ps), cat(Ws, i32, 1), off(Ws), cat(Ts, i32, 3), off(Ts)
    tids = {}
    ti = np.array([tids.setdefault(m["ti"], len(tids)) for m in meshes] or [0], i32)
    keep = np.array([1 if m["keep"] else 0 for m in meshes] or [0], np.uint8)
    F = flat(sorted(far), i64, 3)
    nv, nt = flat(nav_v, f64, 3), flat(nav_t, i32, 3)
    sp = np.ascontiguousarray(np.array(spawn, f64).reshape(3))
    kp = flat([tuple(k[:3]) for k in mapcfg.KEEP_POS], f64, 3)
    ak = simplify.AREA_K if area_k is None else area_k
    a = lambda x: x.ctypes.data
    if os.environ.get("SIMP_DUMP"):  # debugging: the simplifier's whole input, to rerun / analyse it without the game
        np.savez_compressed(f"{os.environ['SIMP_DUMP']}.{out}.npz", P=P, poff=poff, W=W, woff=woff, T=T, toff=toff,
                            keep=keep, ti=ti, F=F, nv=nv, nt=nt, sp=sp, kp=kp, cells=int(bool(cells)),
                            floor=floor or mapcfg.NEAR, ak=ak, eps=-1.0 if eps_fixed is None else eps_fixed,
                            eps_far=-1.0 if eps_far is None else eps_far, names=np.array([m["name"] for m in meshes]),
                            budget=np.array(budget[:2] if budget else (), f64),
                            flags=np.array([m["flags"] for m in meshes] or [0], i64),
                            nav_s=np.array(nav_s if nav_s is not None else np.zeros((0, 3)), f64))
    t0 = time.time()
    def new_handle():
        h = MX.mx_simp_new(nm, a(P), a(poff), a(W), a(woff), a(T), a(toff), a(keep), a(ti), 1 if cells else 0, mapcfg.CHUNK_GRID,
                           len(F), a(F), len(nv), a(nv), len(nt), a(nt), a(sp), floor or mapcfg.NEAR, mapcfg.SPAWN_K, len(kp), a(kp),
                           mapcfg.KEEP_R ** 2, mapcfg.KEEP_D, simplify.BORDER_W, simplify.SEAM_W, simplify.SEAM_K * simplify.SEAM_K,
                           ak ** 2, simplify.MIN_COS, simplify.STRUCT_W, simplify.STRUCT_FLAT_W, sat,
                           simplify.STRUCT_CREASE_COS, simplify.STRUCT_CORNER_COS)
        if not h:
            raise MemoryError("mx_simp_new")
        return h
    drop = None
    if budget and cells and nav_s is not None and len(nav_s) and mapcfg.WS_DROP and (mapcfg.SIMPLIFY_ENV_STRICT or not env_pad):
        # (WS_FAR_SHAPE's coarse pass: clutter as seen from where the coarse level starts, WS_FAR_DROP_K x sooner)
        drop = loose_pieces(MX, meshes, P, poff, W, woff, T, toff, keep, nav_v, nav_t, spawn,
                            mapcfg.WS_NEAR_R if far_shape else floor or mapcfg.NEAR)
        if far_shape:
            drop["cost"] = drop["cost"] / mapcfg.WS_FAR_DROP_K ** 2
    h = new_handle()
    try:
        st = np.zeros(3, i64)
        MX.mx_simp_stats(h, a(st))
        full, ngroups, njobs = (int(x) for x in st)
        print(f"simplify: {njobs} jobs, run in C in {time.time() - t0:.1f} s", flush=True)
        dup = np.zeros(max(nm, 1), np.uint8)
        MX.mx_simp_dup(h, a(dup))
        for mi in np.flatnonzero(dup[:nm]).tolist():
            meshes[mi][src] = []
        grass = sum(len(m[src]) for m in meshes if m["keep"] and not m["grass"])

        def count(eps):
            return grass + int(MX.mx_simp_count(h, eps))
        print("simplify: tris at an error of " + ", ".join(f"{px} px {count((px / 568) ** 2)}" for px in (1, 2, 3, 4, 6, 8, 12, 24)))
        if eps_fixed is not None or mapcfg.EPS is not None:
            eps = eps_fixed if eps_fixed is not None else mapcfg.EPS
        else:
            lo, hi = 1e-16, 1.0
            while count(hi) > mapcfg.BUDGET and hi < 1e6:
                hi *= 100
            for _ in range(40):
                mid = math.sqrt(lo * hi)
                lo, hi = (lo, mid) if count(mid) <= mapcfg.BUDGET else (mid, hi)
            eps = hi
            if eps > mapcfg.EPS_MAX:  # see simplify_meshes
                print(f"simplify: BUDGET {mapcfg.BUDGET} not reachable ({count(eps)} tris at any error); error capped at "
                      f"{math.sqrt(mapcfg.EPS_MAX) * 568:.0f} px -> {count(mapcfg.EPS_MAX)} tris")
                eps = mapcfg.EPS_MAX
        kind, ck = np.zeros(max(njobs, 1), i32), np.zeros((max(njobs, 1), 3), i64)
        MX.mx_simp_jobs(h, a(kind), a(ck))
        jc = [None if k == 0 else "far" if k == 1 else tuple(c) for k, c in zip(kind[:njobs].tolist(), ck.tolist())]
        info, ext = {}, cell_extents(P, poff, W, woff, T, toff) if budget and cells else None
        job_eps = solve_budget(MX, h, jc, eps, eps_far, budget, nav_s, ext, info, drop) if budget and cells else None
        if job_eps is not None and env_pad and info["over"].any() and not mapcfg.SIMPLIFY_ENV_STRICT:
            # The shape envelope costs triangles. Where the solve cannot pay for them (cells in reach of a spot it
            # left over budget) the cells take the run without the envelope as its own solve leaves them, and then
            # any cell whose spot would so hold more than its budget and more than without the envelope, until none
            # does: no spot is worse off than with the envelope off
            MX.mx_simp_envelope(0.0)
            h0 = new_handle()
            try:
                del BUDGET_REPORT[-1]  # (the solve below reports this level)
                eps0 = solve_budget(MX, h0, jc, eps, eps_far, budget, nav_s, ext)
                MX.mx_simp_replay_jobs(h, job_eps.ctypes.data)
                MX.mx_simp_replay_jobs(h0, eps0.ctypes.data)
                jcell, Mf, nb, over = info["jcell"], info["M"].astype(np.float64), info["nb"], info["over"].copy()
                ce, c0 = np.zeros(max(njobs, 1), i64), np.zeros(max(njobs, 1), i64)
                MX.mx_simp_job_nout(h, a(ce))
                MX.mx_simp_job_nout(h0, a(c0))
                sel = jcell >= 0
                be, b0 = (np.bincount(jcell[sel], weights=c[:njobs][sel], minlength=len(over)) * mapcfg.WS_BPT for c in (ce, c0))
                lim = np.maximum(nb, Mf @ b0) + 1.0
                while True:
                    bad = Mf @ np.where(over, b0, be) > lim
                    more = info["M"][bad].any(0) & ~over if bad.any() else np.zeros(len(over), bool)
                    if not more.any():
                        break
                    over |= more
                jover = np.array([jcell[i] >= 0 and bool(over[jcell[i]]) for i in range(njobs)], np.uint8)
                n = int(MX.mx_simp_swap_jobs(h, h0, a(jover)))
            finally:
                MX.mx_simp_free(h0)
            print(f"envelope ({env_pad} m): kept in {int((~over).sum())} of {len(over)} cells; {int(over.sum())} cells in "
                  f"reach of spots over budget take the run without it")
        elif job_eps is not None:
            n = int(MX.mx_simp_replay_jobs(h, job_eps.ctypes.data))
        else:
            n = int(MX.mx_simp_replay(h, eps, eps_far if eps_far is not None else 0.0, 1 if eps_far is not None else 0))
        if mapcfg.SIMPLIFY_VALIDATE and njobs:
            n = validate_jobs(MX, h, njobs, n, mapcfg.SIMPLIFY_VALIDATE_TOL or env_pad or 0.25)
            final_n = n
        else:
            final_n = None
        tj, tm = np.zeros(max(n, 1), i32), np.zeros(max(n, 1), i32)
        cm, cp, cw = (np.zeros(max(3 * n, 1), i32) for _ in range(3))
        MX.mx_simp_fetch(h, a(tj), a(tm), a(cm), a(cp), a(cw))
        if drop and job_eps is not None and n:  # the loose pieces within their cell's piece threshold are left out,
            # whole: a piece over a cell cut goes by the cell of its centre (when the solve has that cell)
            pc_ = drop["pp"][poff[cm[:3 * n:3]] + cp[:3 * n:3]]
            cid = {c: i for i, c in enumerate(sorted({c for c in jc if isinstance(c, tuple)}))}
            own = np.array([cid.get(tuple(k), -1) for k in drop["owner"].tolist()], np.int64)
            c_ = np.where(own[pc_] >= 0, own[pc_], drop["jcell"][tj[:n]])
            gone = (c_ >= 0) & (drop["cost"][pc_] <= drop["cell_de"][np.maximum(c_, 0)])
            keep_t = np.flatnonzero(~gone)
            print(f"pieces: {int(gone.sum())} of {n} triangles in {len(np.unique(pc_[gone]))} loose pieces left out "
                  f"(error within their cell's threshold)")
            tj, tm = tj[keep_t], tm[keep_t]
            k3 = (keep_t[:, None] * 3 + np.arange(3)).reshape(-1)
            cm, cp, cw = cm[k3], cp[k3], cw[k3]
            n = len(keep_t)
            final_n = n
        refit = None
        if mapcfg.SIMPLIFY_REFIT and n:  # surviving wedges take the mean colour of what collapsed into them (simpmesh.c)
            has = [len(m.get("wrgb", ())) == len(w) for m, w in zip(meshes, Ws)]  # (meshes without colours: as they are)
            RGB = cat([mapgather._np_list(m, "wrgb", m["wrgb"], i32, 3) if k else np.zeros((len(w), 3), i32)
                       for m, w, k in zip(meshes, Ws, has)], i32, 3)
            acc = np.zeros((len(RGB), 4))
            MX.mx_simp_refit(h, a(woff), a(RGB), a(acc))
            hit = acc[:, 3] > 0
            refit = RGB.copy()
            refit[hit] = np.clip(np.rint(acc[hit, :3] / acc[hit, 3:4]), 0, 255).astype(i32)
            d = np.abs(refit[hit] - RGB[hit]).max(1)
            print(f"refit: {int(hit.sum())} surviving wedge colours averaged over their collapsed regions "
                  f"({int((d > 8).sum())} moved by more than 8 / 255, {int((d > 32).sum())} by more than 32)")
        final = grass + n if job_eps is not None or final_n is not None else count(eps)
    finally:
        MX.mx_simp_free(h)
    # the output as Tris (corner arrays), per mesh in replay order: positions from the group mesh cm, vertex cp
    CP = P[poff[cm[:3 * n]] + cp[:3 * n]].reshape(-1, 3, 3)
    CW = cw[:3 * n].reshape(-1, 3)
    tm, tj = tm[:n], tj[:n]
    order = np.argsort(tm, kind="stable")
    ends = np.cumsum(np.bincount(tm, minlength=nm)).tolist() if n else [0] * nm
    tjl = tj[order].tolist()
    for mi, m in enumerate(meshes):
        if m["keep"]:
            if m[src]:
                m[out] = mapx_native.Tris(np.ascontiguousarray(Ps[mi][Ws[mi][Ts[mi]]]), np.ascontiguousarray(Ts[mi]))
            else:
                m[out] = []
        elif not m[src]:
            m[out] = []
        else:
            a, b = ends[mi - 1] if mi else 0, ends[mi]
            sel = order[a:b]
            m[out] = mapx_native.Tris(np.ascontiguousarray(CP[sel]), np.ascontiguousarray(CW[sel])) if b > a else []
            m[out + "_cell"] = [jc[j] for j in tjl[a:b]]
            if refit is not None and b > a and has[mi]:
                m[out + "_wrgb"] = np.ascontiguousarray(refit[woff[mi]:woff[mi + 1]])
    print(f"simplify: {full} -> {final} tris ({grass} kept: effects, sky; foliage extra) in {ngroups} welded "
          f"groups / {njobs} jobs, eps {eps:.3g} "
          f"(error <= {math.sqrt(max(eps, 0)) * 568:.1f} px at 640x448)")
    return eps


def validate_jobs(MX, h, njobs, n, tol):
    """SIMPLIFY_VALIDATE: check every replayed triangle against its source (simpmesh.c mx_simp_check) and send the
    jobs with a failing one back to half their collapse steps until none fails -> the surviving triangle count"""
    import numpy as np
    t0 = time.time()
    fcos = math.cos(math.radians(mapcfg.SIMPLIFY_FLIP_DEG))
    bad, cnt = np.zeros(njobs, np.uint8), np.zeros((njobs, 5), np.int64)
    nb = int(MX.mx_simp_check(h, tol, mapcfg.SIMPLIFY_QMIN, fcos, None, bad.ctypes.data, cnt.ctypes.data))
    c0, worst, n0, rounds, jobs = cnt[:, :4].sum(0), int(cnt[:, 4].max()), n, 0, set()
    while nb and rounds < 100000:  # each round takes at least one step off every failing job: ends at the source
        jobs.update(np.flatnonzero(bad).tolist())
        n = int(MX.mx_simp_back_off(h, bad.ctypes.data))
        only = bad.copy()
        nb = int(MX.mx_simp_check(h, tol, mapcfg.SIMPLIFY_QMIN, fcos, only.ctypes.data, bad.ctypes.data, cnt.ctypes.data))
        rounds += 1
    if nb:
        raise RuntimeError(f"validate: {nb} jobs still fail after {rounds} rounds")
    print(f"validate (tol {tol} m, quality {mapcfg.SIMPLIFY_QMIN}, flip {mapcfg.SIMPLIFY_FLIP_DEG:g} deg): {int(c0[0])} degenerate, "
          f"{int(c0[1])} thin, {int(c0[2])} flipped, {int(c0[3])} off-shape triangles (worst {worst} mm) in "
          f"{len(jobs)} of {njobs} jobs; stepped back to before their last change in {rounds} rounds: {n0} -> {n} tris "
          f"(+{n - n0}), {time.time() - t0:.1f} s", flush=True)
    return n


PVSIN_MAGIC = b"PVI1"
