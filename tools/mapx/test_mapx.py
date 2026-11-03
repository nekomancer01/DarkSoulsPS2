"""Check libmapx (tools/mapx) against export_map.py's Python code on random data: every ported stage must give the
same values to the bit. Usage: python3 tools/mapx/test_mapx.py [seed count]   (also run by CI's offline checks)"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import numpy as np
import export_map as em
import mapx_native

L = mapx_native.lib()
assert L is not None, "libmapx did not build"


def _canon(x):
    if isinstance(x, (list, tuple)):
        return type(x)(_canon(v) for v in x)
    if isinstance(x, dict):
        return {k: _canon(v) for k, v in x.items()}
    if isinstance(x, np.generic):
        return x.item()
    if isinstance(x, np.ndarray):
        return _canon(x.tolist())
    return x


def same(a, b, what):
    if a != b or repr(_canon(a)) != repr(_canon(b)):  # repr: the sign of zeros too (0.0 == -0.0)
        raise SystemExit(f"FAIL {what}: C and Python differ")


def foliage_mesh(rng, ncomp):
    pos, tl = [], []
    for _ in range(ncomp):
        c = rng.uniform(-60, 60, 3) * [1, 0.2, 1]
        k = int(rng.integers(1, 6))
        base = len(pos)
        for v in range(k + 2):
            pos.append(tuple((c + rng.normal(0, 0.4, 3)).tolist()))
        for t in range(k):
            tl.append((base + t, base + t + 1, base + t + 2))
        if rng.random() < 0.3:  # a duplicate position joining two cards
            pos.append(pos[base])
            tl.append((len(pos) - 1, base + 1, base + 2))
    return tl, pos


def check_foliage(rng):
    tl, pos = foliage_mesh(rng, int(rng.integers(1, 120)))
    nav = [tuple(x) for x in (rng.uniform(-70, 70, (400, 3)) * [1, 0.05, 1]).tolist()]
    grid = {}
    for p in nav:
        grid.setdefault((int(p[0] // 2), int(p[2] // 2)), []).append(p[1])
    q = (0.1, 0.2, -0.3, 0.927)
    nq = sum(x * x for x in q) ** 0.5
    q = tuple(x / nq for x in q)
    xf, _ = em.place(dict(rot=(10.0, 33.0, -4.0), scale=(1.1, 0.9, 1.0), pos=(3.0, -2.5, 7.25)))

    def is_canopy(ps):
        base = min(p[1] for p in ps)
        cx, cz = sum(p[0] for p in ps) / len(ps), sum(p[2] for p in ps) / len(ps)
        for r in (2, 6):
            ys = [y for i in range(int(cx // 2) - r, int(cx // 2) + r + 1) for j in range(int(cz // 2) - r, int(cz // 2) + r + 1)
                  for y in grid.get((i, j), ()) if y <= base + 0.5]
            if ys:
                return base - max(ys) > em.CANOPY_H
        return True
    for ivy in (False, True):
        em.CANOPY_STATS[0] = 0
        ref = em.thin_foliage(tl, pos, "m1234B1_0000/17", ivy, canopy=lambda vs: is_canopy([xf(pos[i]) for i in vs]))
        cs = em.CANOPY_STATS[0]
        em.CANOPY_STATS[0] = 0
        npts = np.ascontiguousarray(nav, np.float64)
        g = L.mx_navgrid_new(len(npts), npts.ctypes.data)
        got = em.thin_foliage_c(L, tl, pos, "m1234B1_0000/17", ivy, xf.many(np.array(pos)), g)
        L.mx_navgrid_free(g)
        same(ref, got, "thin_foliage")
        same(cs, em.CANOPY_STATS[0], "thin_foliage canopy count")


def check_soften(rng):
    S = rng.uniform(-50, 50, (int(rng.integers(5, 300)), 3)).tolist()
    ents = []
    for _ in range(int(rng.integers(1, 6))):
        npos = int(rng.integers(3, 200))
        P = [tuple(x) for x in rng.uniform(-80, 80, (npos, 3)).tolist()]
        nw = int(rng.integers(3, 300))
        wrgb = rng.integers(0, 256, (nw, 3)).tolist()
        nt = int(rng.integers(1, 400))
        tris = []
        for _ in range(nt):
            ps = rng.integers(0, npos, 3)
            if rng.random() < 0.5:  # thin sliver
                ps[2] = ps[1]
                tris.append(tuple((tuple((np.array(P[ps[0]]) + rng.normal(0, 0.01, 3)).tolist()) if j == 2 else P[ps[j]],
                                   int(rng.integers(0, nw))) for j in range(3)))
            else:
                tris.append(tuple((P[ps[j]], int(rng.integers(0, nw))) for j in range(3)))
        ents.append(dict(tris=tris, wrgb=wrgb))
    a = [dict(e) for e in ents]
    b = [dict(e) for e in ents]
    em.SOFT_STATS[:] = [0, 0]
    L_ = mapx_native._L[0]
    mapx_native._L[0] = None  # Python path
    em.soften_thin(a, S, 24.0)
    mapx_native._L[0] = L_
    sa = list(em.SOFT_STATS)
    em.SOFT_STATS[:] = [0, 0]
    em.soften_thin(b, S, 24.0)
    same([e["wrgb"] for e in a], [e["wrgb"] for e in b], "soften_thin colours")
    same(sa, em.SOFT_STATS, "soften_thin stats")


def check_far(rng):
    ms = []
    for _ in range(int(rng.integers(1, 8))):
        npos = int(rng.integers(3, 300))
        pos = [tuple(x) for x in (rng.uniform(-300, 300, (npos, 3)) + rng.integers(-3, 3, 3) * 64).tolist()]
        wpos = rng.integers(0, npos, npos * 2).tolist()
        tris = [tuple(rng.integers(0, len(wpos), 3).tolist()) for _ in range(int(rng.integers(0, 500)))]
        ms.append(dict(pos=pos, wpos=wpos, tris=tris, keep=bool(rng.random() < 0.2)))
    nav = rng.uniform(-100, 100, (int(rng.integers(1, 500)), 3)).tolist()
    L_ = mapx_native._L[0]
    mapx_native._L[0] = None
    ref = em.far_cells(ms, nav)
    mapx_native._L[0] = L_
    same(ref, em.far_cells(ms, nav), "far_cells")


if __name__ == "__main__":
    import contextlib, io
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    for k in range(seed, seed + count):
        rng = np.random.default_rng(k)
        with contextlib.redirect_stdout(io.StringIO()):
            check_foliage(rng)
            check_soften(rng)
            check_far(rng)
    print(f"mapx: thin_foliage, soften_thin, far_cells identical to the Python code on {count} random cases")


# ---- stage 2: export_normal (relief) ----
import math
import export_normal as en

if os.environ.get("MAPX_TEST_LIBM_LOG", "1") == "1":  # numpy AVX-512 log differs from libm by an ulp
    en._log = lambda a: np.array([math.log(v) for v in np.asarray(a, np.float64).ravel().tolist()]).reshape(np.shape(a))


def bc5_dds(rng, w, h, mips=1):
    import struct
    hdr = bytearray(148)
    hdr[0:4] = b"DDS "
    struct.pack_into("<iii", hdr, 12, h, w, mips)
    hdr[84:88] = b"DX10"
    struct.pack_into("<I", hdr, 128, 83)
    n = 0
    ww, hh = w, h
    for _ in range(mips):
        n += max(1, (ww + 3) // 4) * max(1, (hh + 3) // 4) * 16
        ww, hh = max(1, ww // 2), max(1, hh // 2)
    return bytes(hdr) + rng.integers(0, 256, n, dtype=np.uint8).tobytes()


def check_bump(rng):
    Lx = en._mx()
    assert Lx is not None
    # (512, 256, 1) and (1024, 1024, 1) average 4 x 2 and 8 x 8 texels: the summation order of both offsets matters
    for w, h, mips in ((16, 16, 1), (256, 128, 2), (64, 512, 3), (8, 4, 1), (12, 20, 1), (512, 256, 1), (1024, 1024, 1),
                       (300, 1000, 2)):
        d = bc5_dds(rng, w, h, mips)
        same(en.xy_pow2(d, max(en.BUMP_MAX, 4), en.BUMP_MAX).tolist(), en._mx_xy_pow2(Lx, d, max(en.BUMP_MAX, 4), en.BUMP_MAX).tolist(),
             f"xy_pow2 {w}x{h}")
    nt = int(rng.integers(1, 3000))
    PS = rng.normal(size=(nt, 3, 3)) * 3
    N, T = rng.normal(size=(nt, 3)), rng.normal(size=(nt, 3))
    W = rng.normal(size=nt)
    area = np.zeros(nt)
    Nn, B = np.empty((nt, 3)), np.empty((nt, 3))
    Nc, Tc = N.copy(), T.copy()
    Lx.mx_bump_frames(nt, np.ascontiguousarray(PS).ctypes.data, Nc.ctypes.data, Tc.ctypes.data, W.ctypes.data, en._MX["fma"],
                      area.ctypes.data, Nn.ctypes.data, B.ctypes.data)
    C = np.cross(PS[:, 1] - PS[:, 0], PS[:, 2] - PS[:, 0])
    same([0.5 * np.linalg.norm(c) for c in C], area.tolist(), "bump area")
    same((N / (np.array([np.linalg.norm(v) for v in N]).reshape(-1, 1) + 1e-12)).tolist(), Nn.tolist(), "bump Nn")
    fN, fT, fB = en._frames(N, T, np.where(W < 0, -1.0, 1.0))
    same([fN.tolist(), fT.tolist(), fB.tolist()], [Nc.tolist(), Tc.tolist(), B.tolist()], "bump frames")
    # response: two light rows, a probe and no probe
    def row(seed):
        r = np.random.default_rng(seed)
        out = {}
        for i in "012":
            out[f"colA_{i}"] = float(r.choice([0, 80, 150]))
            out[f"degRotX_{i}"], out[f"degRotY_{i}"] = float(r.uniform(-90, 90)), float(r.uniform(0, 360))
            for c in "RGB":
                out[f"col{c}_{i}"] = float(r.integers(0, 256))
        for s in "ud":
            out[f"colA_{s}"] = float(r.uniform(20, 200))
            for c in "RGB":
                out[f"col{c}_{s}"] = float(r.integers(0, 256))
        out["envDif_colA"] = float(r.uniform(20, 200))
        for c in "RGB":
            out[f"envDif_col{c}"] = float(r.integers(0, 256))
        return out
    probes = {3: np.abs(rng.normal(size=(6, 8, 8, 3)))}
    tri = dict(N=fN, T=fT, B=fB, k=rng.random(nt), alb=rng.random((nt, 3)), lid=rng.integers(0, 2, nt),
               pid=rng.choice([3, -1, 7], nt), rows={0: row(1), 1: row(2)}, probes=probes)
    tone = lambda rgb: [min(1.0, max(0.0, x / (1 + x))) ** (1 / 2.2) for x in rgb]
    resp = en._Response(tone, [0.9, 1.0, 1.1], 0.6)
    xy = np.array(en.RING[1:])
    same(resp(tri, xy).tolist(), en._mx_response(Lx, en._Response(tone, [0.9, 1.0, 1.1], 0.6), dict(tri), xy).tolist(),
         "bump response")
    rows = rng.choice(nt, min(nt, 64), replace=False)
    pal = np.vstack([[0.0, 0.0], rng.uniform(-1, 1, (255, 2))])
    sub = {k: (v[rows] if isinstance(v, np.ndarray) else v) for k, v in tri.items()}
    same(resp(sub, pal).tolist(), en._mx_response(Lx, resp, dict(tri), pal, rows=rows).tolist(), "bump response rows")
    # k-means: float32 palette, float64 weighted clusters, nearest with float64 centres
    for dt, d, k, n, w in ((np.float32, 2, 255, int(rng.integers(300, 4000)), False), (np.float64, 2, 9, 500, False),
                           (np.float64, 16, 48, int(rng.integers(10, 900)), True), (np.float64, 16, 1, 300, True)):
        x = (rng.normal(size=(n, d)) * 0.4).astype(dt)
        wts = rng.random(n) + 1e-6 if w else None
        ref, got = en._kmeans(x, k, wts, iters=7), en._mx_kmeans(Lx, x, k, wts, iters=7)
        same([ref[0].tolist(), ref[1].tolist()], [got[0].tolist(), got[1].tolist()], f"kmeans {dt.__name__} {k}")
        if d == 2:
            p = np.vstack([[0.0, 0.0], ref[0].astype(np.float64)])
            same(en._nearest(x, p).tolist(), en._mx_nearest(Lx, x, p).tolist(), "nearest")


class _Probes:
    def __init__(self, ids):
        self.ids = ids

    def __call__(self, p):
        return self.ids[int(abs(p[0]) * 7) % len(self.ids)]

    def many(self, P):
        return [self(tuple(p)) for p in np.asarray(P).tolist()]


class _Shadow:
    def lit_many(self, P, N):
        return (np.floor(np.abs(np.asarray(P)[:, 0] * 3 + np.asarray(N)[:, 1])) % 3) / 2


def check_assign(rng):
    """export_normal.assign + _finish end to end, numpy vs libmapx"""
    import contextlib, io
    names = [f"n{i}" for i in range(int(rng.integers(1, 5)))]
    dds = {n: bc5_dds(rng, int(rng.choice([32, 64, 256])), int(rng.choice([32, 128, 256])), 2) for n in names}
    probes = {3: np.abs(rng.normal(size=(6, 8, 8, 3))).tolist(), 5: np.abs(rng.normal(size=(6, 8, 8, 3))).tolist()}
    tone = lambda rgb: [min(1.0, max(0.0, x / (1 + x))) ** (1 / 2.2) for x in rgb]
    def row(seed):
        r = np.random.default_rng(seed)
        out = {}
        for i in "012":
            out[f"colA_{i}"] = float(r.choice([0, 80, 150]))
            out[f"degRotX_{i}"], out[f"degRotY_{i}"] = float(r.uniform(-90, 90)), float(r.uniform(0, 360))
            for c in "RGB":
                out[f"col{c}_{i}"] = float(r.integers(0, 256))
        for s_ in "ud":
            out[f"colA_{s_}"] = float(r.uniform(20, 200))
            for c in "RGB":
                out[f"col{c}_{s_}"] = float(r.integers(0, 256))
        out["envDif_colA"] = float(r.uniform(20, 200))
        for c in "RGB":
            out[f"envDif_col{c}"] = float(r.integers(0, 256))
        return out
    rows = [row(1), row(2)]
    pof = _Probes([3, 5, None, 9])
    MESH, meshes = [], []
    for mi in range(int(rng.integers(1, 8))):
        nw = int(rng.integers(3, 200))
        MESH.append(dict(tex=names[mi % len(names)] if rng.random() < 0.9 else "missing", light=rows[mi % 2],
                         alb=tuple(rng.uniform(0.05, 0.9, 3).tolist()), N=rng.normal(size=(nw, 3)),
                         T=rng.normal(size=(nw, 3)), W=np.where(rng.random(nw) < 0.5, -1.0, 1.0), probe_of=pof))
        P = rng.normal(size=(nw, 3)) * 5
        tris = [tuple((tuple(P[w].tolist()), int(w)) for w in rng.integers(0, nw, 3)) for _ in range(int(rng.integers(1, 400)))]
        meshes.append(dict(tris=tris, bump=len(MESH) - 1 if rng.random() < 0.8 else None,
                           wuv=rng.uniform(-2, 3, (nw, 2)).tolist()))
    def run(use_c):
        en.MESH[:] = MESH
        en._MX.clear()
        L_ = mapx_native._L[0]
        if not use_c:
            mapx_native._L[0] = None
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                out = en.assign(meshes, dds, _Shadow() if rng.random() < 2 else None, tone, [0.9, 1.0, 1.1], 0.6, probes)
                ids = sorted(out)
                r = np.random.default_rng(11)
                en.CHUNK_BUMP[:] = [((out[ids[0]][0],), sorted(r.choice(ids, min(len(ids), int(r.integers(1, 60))), replace=False).tolist()))
                                    for _ in range(int(r.integers(1, 80)))] if ids else []
                fin = en._finish() if ids else None
        finally:
            mapx_native._L[0] = L_
            en._MX.clear()
        st = en._STATE
        return out, st.get("density"), [(t["name"], t["w"], t["h"], t["mips"]) for t in st.get("texs", [])], \
            st["pal"].tolist() if "pal" in st else None, st["R"].tolist() if "R" in st else None, \
            None if fin is None else (fin[0], fin[1].tolist())
    a, b = run(False), run(True)
    for x, y, what in zip(a, b, ("assign map", "density", "textures", "palette", "response", "_finish")):
        same(x, y, what)


if __name__ == "__main__":
    for k in range(seed, seed + max(1, count // 4)):
        check_assign(np.random.default_rng(k))
        check_bump(np.random.default_rng(k))
    print(f"mapx: relief (BC5 decode, frames, response, k-means) identical to export_normal's numpy code")


# ---- stage 3: simplify_meshes ----
import contextlib, io
def simp_scene(rng):
    """bumpy lattice patches (neighbours share their edge positions, so they weld), UV seams, exact copies, kept and
    foliage meshes, a navmesh"""
    ms, step = [], float(rng.choice([1.0, 1.5, 2.5]))
    for _ in range(int(rng.integers(1, 9))):
        g = int(rng.integers(2, 24))
        ox, oz = (rng.integers(-3, 3, 2) * (g - 1)).tolist()
        y0 = float(rng.integers(-2, 3)) * 8
        pos, idx = [], {}
        for i in range(g):
            for k in range(g):
                x, z = (ox + i) * step, (oz + k) * step
                y = y0 + round(float(np.sin(x * 0.3) + np.cos(z * 0.2)) * (rng.random() < 0.7), 3)
                idx[i, k] = len(pos)
                pos.append((x, y, z))
        wpos = list(range(len(pos))) + [int(p) for p in rng.integers(0, len(pos), len(pos) // 4)]
        seam = {p: len(pos) + n for n, p in enumerate(wpos[len(pos):])}
        tris = []
        for i in range(g - 1):
            for k in range(g - 1):
                a, b, c, d = idx[i, k], idx[i + 1, k], idx[i + 1, k + 1], idx[i, k + 1]
                if rng.random() < 0.05:
                    continue
                w = lambda p: seam.get(p, p) if (i + k) % 3 == 0 else p
                tris.append((w(a), w(b), w(c)))
                tris.append((w(a), w(c), w(d)))
        m = dict(pos=pos, wpos=wpos, tris=tris, keep=bool(rng.random() < 0.12), grass=bool(rng.random() < 0.1),
                 ti=int(rng.integers(0, 3)))
        ms.append(m)
        if rng.random() < 0.25:
            ms.append(dict(m, tris=list(reversed(m["tris"])), keep=False))
    nav_v = [(float(x), float(y), float(z)) for x, y, z in rng.uniform(-30, 30, (int(rng.integers(3, 40)), 3))]
    nav_t = [tuple(int(v) for v in rng.integers(0, len(nav_v), 3)) for _ in range(int(rng.integers(1, 30)))]
    return ms, nav_v, nav_t


def check_simplify(rng):
    import copy
    ms, nav_v, nav_t = simp_scene(rng)
    ms[0]["keep"] = False  # the Python code needs one live mesh (view_distance of no points fails)
    spawn = tuple(rng.uniform(-20, 20, 3).tolist())
    cells = bool(rng.random() < 0.6)
    keys = sorted({(int(np.floor(p[0] / em.CHUNK_GRID)), int(np.floor(p[1] / em.CHUNK_GRID)), int(np.floor(p[2] / em.CHUNK_GRID)))
                   for m in ms for p in m["pos"]})
    far = frozenset(k for k in keys if rng.random() < 0.3)
    kw = dict(src="tris_in", out="tris_far", cells=cells, far=far if cells else frozenset(),
              eps_fixed=None if rng.random() < 0.5 else float(rng.choice([1e-6, 1e-4, 1e-3])),
              floor=None if rng.random() < 0.5 else 2.0, area_k=None if rng.random() < 0.5 else 0.0,
              eps_far=None if rng.random() < 0.5 else 3e-4)
    em.KEEP_POS[:] = [tuple(rng.uniform(-20, 20, 3).tolist())] if rng.random() < 0.5 else []
    em.BUDGET = int(rng.integers(10, 3000))
    for m in ms:
        m["tris_in"] = m["tris"]

    def run(use_c):
        M = copy.deepcopy(ms)
        L_ = mapx_native._L[0]
        if not use_c:
            mapx_native._L[0] = None
        f = io.StringIO()
        try:
            with contextlib.redirect_stdout(f):
                eps = em.simplify_meshes(M, spawn, nav_v, nav_t, **kw)
        finally:
            mapx_native._L[0] = L_
        log = [ln for ln in f.getvalue().splitlines() if not ln.startswith("simplify: ") or " jobs, " not in ln]
        return eps, log, [(m.get("tris_in"), m.get("tris_far"), m.get("tris_far_cell")) for m in M]
    a, b = run(False), run(True)
    same(a[0], b[0], "simplify eps")
    if a[1] != b[1]:
        print("\n".join(a[1]), "\n----\n" + "\n".join(b[1]), file=sys.stderr)
    same(a[1], b[1], "simplify log")
    same(a[2], b[2], "simplify output")
    return sum(len(x[1] or []) for x in a[2])


if __name__ == "__main__":
    os.environ["SIMPLIFY_PROCS"], os.environ["SIMPLIFY_CACHE"] = "1", "0"
    tot = 0
    for k in range(seed, seed + count):
        tot += check_simplify(np.random.default_rng(k))
    print(f"mapx: simplify_meshes identical to the Python code on {count} random scenes ({tot} output triangles)")


# ---- stage 3b: flatten + build_chunks ----
def chunk_scene(rng):
    textures = [(int(rng.choice([32, 64, 128, 256])), int(rng.choice([32, 64, 128]))) for _ in range(4)]
    entries = []
    shared = [tuple(float(x) for x in p) for p in rng.integers(-40, 40, (60, 3)) * 0.5]
    shared += [(0.0, -0.0, 1.0), (-0.0, 0.0, 1.0), (0.0, 0.0, -0.0)]
    for _ in range(int(rng.integers(1, 12))):
        far, zeros = rng.random() < 0.3, rng.random() < 0.15
        nw = int(rng.integers(3, 60))
        wrgb = [tuple(int(v) for v in rng.integers(0, 256, 3)) for _ in range(nw)]
        wa = [int(v) for v in rng.integers(0, 256, nw)]
        wuv = [tuple(float(v) for v in rng.uniform(-3, 3, 2)) for _ in range(nw)]
        n = int(rng.integers(1, 300))
        ctr = rng.uniform(-1, 1, 3) * (rng.choice([10, 200, 1500]) if far else 30)
        tris = []
        for _ in range(n):
            t = []
            for _ in range(3):
                if zeros:  # signed zeros: which equal corner a Python set keeps shows in the boxes
                    p = tuple(float(rng.choice([0.0, -0.0, 0.5, -0.5])) for _ in range(3))
                elif rng.random() < 0.3:
                    p = shared[int(rng.integers(len(shared)))]
                else:
                    p = tuple(float(x) for x in ctr + np.round(rng.normal(0, 3, 3), 2))
                t.append((p, int(rng.integers(nw))))
            tris.append(tuple(t))
        grass = rng.random() < 0.2
        fol = None
        if grass:
            nt_ = int(rng.integers(1, 4))
            fol = [(float(rng.random()), (len(entries), i // nt_), nt_) for i in range(n)]
            if rng.random() < 0.5:  # tuft centres on GRASS_GRID lines
                tris = [tuple(((float(round(p[0] / 2) * 2), p[1], float(round(p[2] / 2) * 2)), w) for p, w in t) for t in tris]
        tc = None
        if rng.random() < 0.5:
            tc = [None if rng.random() < 0.2 else (int(rng.integers(0, 2)), tuple(int(v) for v in rng.integers(-2, 2, 3)))
                  for _ in range(n)]
        flags = int(rng.choice([0, 1])) | (em.CHUNK_SKY if rng.random() < 0.1 else 0)
        entries.append(dict(tris=tris if rng.random() < 0.95 else [], wrgb=wrgb, wa=wa, wuv=wuv,
                            ti=int(rng.integers(-1, len(textures))), flags=flags, fol=fol, tcell=tc,
                            dg=int(rng.integers(0, 3)), ent=int(rng.choice([0, 0, 7]))))
        if rng.random() < 0.2:  # a second entry of the same mesh (same wedge tables)
            entries.append(dict(entries[-1], tris=tris[: n // 2], fol=fol[: n // 2] if fol else None,
                                tcell=tc[: n // 2] if tc else None))
    ntot = sum(len(m["tris"]) for m in entries)
    bkey = {k: (int(rng.integers(0, 3)), int(rng.integers(0, 4))) for k in range(ntot) if rng.random() < 0.4} \
        if rng.random() < 0.7 else None
    return entries, textures, tuple(float(x) for x in rng.uniform(-20, 20, 3)), bkey


def check_chunks(rng):
    entries, textures, centre, bkey = chunk_scene(rng)
    stream, dgon = bool(rng.random() < 0.6), bool(rng.random() < 0.5)
    import export_normal

    def lists():
        return [list(export_normal.CHUNK_BUMP), list(em.CHUNK_TAG), list(em.CHUNK_DG), list(em.CHUNK_ENT)]
    with contextlib.redirect_stdout(io.StringIO()) as f1:
        meshes, verts, tris, uvs, fol, tcells, dgs, ents = em.flatten(entries, textures)
        ref = em.build_chunks(meshes, verts, tris, uvs, textures, centre, fol, bkey, tcells if stream else None,
                              dgs if dgon else None, ents)
    ref_l = lists()
    with contextlib.redirect_stdout(io.StringIO()) as f2:
        got, nm, nt = em.build_chunks_c(L, entries, textures, centre, bkey, stream, dgon)
    same(f1.getvalue(), f2.getvalue(), "chunks log")
    same((len(meshes), len(tris)), (nm, nt), "chunks counts")
    for a, b in zip(ref, got):
        same(a, b, "chunk")
    same(len(ref), len(got), "chunk count")
    same(ref_l, lists(), "CHUNK_BUMP / TAG / DG / ENT")
    return len(ref)


if __name__ == "__main__":
    tot = 0
    for k in range(seed, seed + count):
        tot += check_chunks(np.random.default_rng(k))
    print(f"mapx: flatten + build_chunks identical to the Python code on {count} random scenes ({tot} chunks)")


# ---- stage 4: gather helpers ----
def check_gather_helpers(rng):
    def both(f):
        L_ = mapx_native._L[0]
        mapx_native._L[0] = None
        try:
            a = f()
        finally:
            mapx_native._L[0] = L_
        return a, f()
    K = rng.integers(0, int(rng.integers(1, 6)), (int(rng.integers(1, 3000)), int(rng.integers(1, 16))))
    a, b = both(lambda: em._first_rows(K))
    same([x.tolist() for x in a], [x.tolist() for x in b], "_first_rows")
    nav = np.unique(np.round(rng.uniform(-900, 900, (int(rng.integers(1, 400)), 3)) / 10), axis=0) * 10
    for _ in range(5):
        P = rng.uniform(-1, 1, (int(rng.integers(1, 5000)), 3)) * rng.choice([100, 600, 1500]) + rng.uniform(-900, 900, 3)
        a, b = both(lambda: em.is_sky("m_wall", P, nav))
        same(a, b, "is_sky")
    V = dict(pos=[tuple(p) for p in (rng.integers(0, 4, (60, 3)) * 0.25).tolist()],
             uv=[tuple(p) for p in (rng.integers(0, 3, (60, 2)) / 4).tolist()] if rng.random() < 0.7 else [])
    tl = [tuple(int(v) for v in rng.integers(0, 60, 3)) for _ in range(int(rng.integers(1, 800)))]
    tl += [t[1:] + t[:1] for t in tl[:50]]
    em._VERT_NP.clear()
    a, b = both(lambda: em.dedup_tris(tl, V))
    same(a, b, "dedup_tris")
    import dsr_light
    subs = [dict(part=f"h{i}", tris=[tuple(tuple(c) for c in t) for t in
                                     (rng.uniform(-30, 30, (int(rng.integers(0, 200)), 3, 3)) +
                                      rng.uniform(-20, 20, 3)).round(int(rng.integers(1, 4))).tolist()])
            for i in range(int(rng.integers(1, 8)))]
    probes = {p: [] for p in rng.choice(10, 5, replace=False).tolist()}
    part_probe = {s["part"]: int(rng.integers(0, 12)) for s in subs}
    Q = (rng.uniform(-60, 60, (int(rng.integers(1, 3000)), 3))).round(int(rng.integers(0, 3)))

    def run():
        ix = dsr_light.ProbeIndex(subs, probes, part_probe)
        return ix.default, ix.many(Q[: len(Q) // 2]), [ix(tuple(q)) for q in Q[len(Q) // 2:].tolist()], ix.many(Q)
    a, b = both(run)
    same(a, b, "ProbeIndex")
    X = np.concatenate([rng.uniform(-1, 3, 3000), [0.0, -0.0, 1.0, np.inf, 1e-300]]).reshape(-1, 1)
    a, b = both(lambda: (dsr_light.srgb_lin_many(X).tolist(), dsr_light._pow_many(np.abs(X), 1 / 2.2, False).tolist()))
    same(a, b, "srgb_lin_many / pow")
    import havok_collision as hc, struct
    nv = int(rng.integers(0, 300))
    raw = rng.uniform(-50, 50, (nv, 4)).astype("<f4").tobytes()
    qs = tuple(struct.unpack("<12f", rng.uniform(-2, 2, 12).astype("<f4").tobytes()))
    for q in (qs, (0, 0, 0) + qs[3:4] + (0, 0, 0, 1) + (1, 1, 1) + qs[11:]):
        ref = [struct.unpack_from("<3f", raw, 16 * i) for i in range(nv)]
        if q[:3] != (0, 0, 0) or q[4:8] != (0, 0, 0, 1) or q[8:11] != (1, 1, 1):
            ref = [hc._qs_apply(q, v) for v in ref]
        same(ref, list(map(tuple, hc._verts_np(raw, 0, nv, q).tolist())), "hkx vertices")
        part = dict(scale=tuple(rng.uniform(0.5, 2, 3).tolist()), rot=tuple(rng.uniform(-180, 180, 3).tolist()),
                    pos=tuple(rng.uniform(-100, 100, 3).tolist()))
        same([hc._place(part, v) for v in ref], list(map(tuple, hc._place_np(part, np.array(ref).reshape(-1, 3)).tolist())),
             "collision placement")


if __name__ == "__main__":
    for k in range(seed, seed + count):
        check_gather_helpers(np.random.default_rng(k))
    print(f"mapx: _first_rows, is_sky, dedup_tris, ProbeIndex identical to the Python code on {count} random cases")
