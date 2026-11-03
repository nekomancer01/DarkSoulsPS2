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


def same(a, b, what):
    if a != b:
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
