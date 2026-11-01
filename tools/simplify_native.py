"""ctypes binding for tools/native/simplify.c: the same quadric edge collapse as tools/simplify.py's simplify(), in C.

  simplify(pos, wpos, tris, locked, scale, area_k=None) -> (T0, steps)   exactly as simplify.simplify returns them
  lib()   load (and build when missing or older than simplify.c) tools/native/libsimplify.so

The C code mirrors simplify.py operation for operation (same IEEE double arithmetic in the same order, no FMA, same
tie-breaking), so both return the same log, step for step. Check after any change to either file:
  simplify_native.py --test [n]      random meshes (seams, borders, locks, non-manifold edges) through both, compared
                                     exactly; also prints the speed-up"""
import ctypes, os, subprocess, sys, tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import simplify as pysimplify

SRC = os.path.join(HERE, "native", "simplify.c")
SO = os.path.join(HERE, "native", "libsimplify.so")
_L = None


class _Result(ctypes.Structure):
    _fields_ = [("nt", ctypes.c_int), ("t0", ctypes.POINTER(ctypes.c_int)), ("nsteps", ctypes.c_int),
                ("cost", ctypes.POINTER(ctypes.c_double)), ("nrem", ctypes.POINTER(ctypes.c_int)),
                ("ncha", ctypes.POINTER(ctypes.c_int)), ("rem", ctypes.POINTER(ctypes.c_int)),
                ("cha", ctypes.POINTER(ctypes.c_int)), ("nrem_all", ctypes.c_size_t), ("ncha_all", ctypes.c_size_t)]


def _build():
    cc = os.environ.get("CC", "cc")
    fd, tmp = tempfile.mkstemp(suffix=".so", dir=os.path.dirname(SO))
    os.close(fd)
    try:
        subprocess.run([cc, "-O2", "-ffp-contract=off", "-fno-fast-math", "-shared", "-fPIC", "-o", tmp, SRC, "-lm"],
                       check=True, capture_output=True, text=True)
        os.replace(tmp, SO)  # atomic: parallel exporters may build at once
    except subprocess.CalledProcessError as e:
        raise RuntimeError(f"building {SO} failed: {e.stderr.strip()[:400]}")
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def lib():
    global _L
    if _L is None:
        if not os.path.exists(SO) or os.path.getmtime(SO) < os.path.getmtime(SRC):
            _build()
        L = ctypes.CDLL(SO)
        dp, ip, bp = ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ubyte)
        L.simplify_run.restype = ctypes.POINTER(_Result)
        L.simplify_run.argtypes = [ctypes.c_int, dp, ctypes.c_int, ip, ctypes.c_int, ip, bp, dp] + [ctypes.c_double] * 5
        L.simplify_free.argtypes = [ctypes.POINTER(_Result)]
        _L = L
    return _L


def simplify(pos, wpos, tris, locked, scale, area_k=None):
    """simplify.simplify, in C"""
    L = lib()
    P = np.ascontiguousarray(np.asarray(pos, dtype=np.float64).reshape(-1, 3))
    W = np.ascontiguousarray(np.asarray(wpos, dtype=np.int32))
    T = np.ascontiguousarray(np.asarray(tris, dtype=np.int32).reshape(-1, 3))
    S = np.ascontiguousarray(np.asarray(scale, dtype=np.float64))
    K = None if locked is None else np.ascontiguousarray(np.asarray(locked, dtype=np.uint8))
    ak = pysimplify.AREA_K if area_k is None else area_k
    dp, ip, bp = ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ubyte)
    r = L.simplify_run(len(P), P.ctypes.data_as(dp), len(W), W.ctypes.data_as(ip), len(T), T.ctypes.data_as(ip),
                       K.ctypes.data_as(bp) if K is not None else None, S.ctypes.data_as(dp),
                       pysimplify.BORDER_W, pysimplify.SEAM_W, pysimplify.SEAM_K * pysimplify.SEAM_K, ak ** 2,
                       pysimplify.MIN_COS)
    try:
        R = r.contents
        # empty outputs may come back as NULL pointers (the C side allocates nothing for them): never wrap those
        def arr(ptr, n):
            return np.ctypeslib.as_array(ptr, (n,)).tolist() if n and ptr else []
        t0 = np.asarray(arr(R.t0, R.nt * 6), dtype=np.int64).reshape(-1, 6).tolist()
        T0 = [((t[0], t[1]), (t[2], t[3]), (t[4], t[5])) for t in t0]
        n = R.nsteps
        steps = []
        if n:
            cost, nrem, ncha = arr(R.cost, n), arr(R.nrem, n), arr(R.ncha, n)
            rem, cha = arr(R.rem, R.nrem_all), arr(R.cha, R.ncha_all)
            ri = ci = 0
            for c, nr, nc in zip(cost, nrem, ncha):
                ch = tuple(tuple(cha[ci + 4 * j:ci + 4 * j + 4]) for j in range(nc))
                steps.append((c, tuple(rem[ri:ri + nr]), ch))
                ri += nr
                ci += 4 * nc
        return T0, steps
    finally:
        L.simplify_free(r)


def _test_mesh(rng, n):
    """a bumpy grid with UV seams, holes, a duplicated (non-manifold) strip and some locked vertices"""
    g = int(n ** 0.5) + 2
    xs, zs = np.meshgrid(np.arange(g, dtype=float), np.arange(g, dtype=float))
    ys = 0.05 * rng.standard_normal(xs.shape) + 0.3 * np.sin(xs * 0.7) * (rng.random() < 0.7)
    pos = [tuple(map(float, p)) for p in np.stack([xs, ys, zs], -1).reshape(-1, 3)]
    wpos, tris, aid = [], [], {}
    seam = rng.integers(1, g - 1)

    def w(p, side):
        k = (p, side)
        if k not in aid:
            aid[k] = len(wpos)
            wpos.append(p)
        return aid[k]
    for i in range(g - 1):
        for j in range(g - 1):
            if rng.random() < 0.03:
                continue  # hole: border loop
            a, b, c, d = i * g + j, i * g + j + 1, (i + 1) * g + j, (i + 1) * g + j + 1
            side = int(j >= seam)
            if rng.random() < 0.5:
                tris += [(w(a, side), w(c, side), w(b, side)), (w(b, side), w(c, side), w(d, side))]
            else:
                tris += [(w(a, side), w(d, side), w(b, side)), (w(a, side), w(c, side), w(d, side))]
    for j in range(0, g - 1, 3):  # a fin sharing edges: non-manifold
        top = len(pos)
        pos.append((float(j), 1.0, 0.5))
        tris.append((w(j, 9), w(j + 1, 9), w(top, 9)))
    locked = [bool(rng.random() < 0.02) for _ in pos]
    scale = [float(1.0 / (1.0 + 0.1 * p[0] + rng.random())) ** 2 for p in pos]
    return pos, wpos, tris, locked, scale


def _edge_cases():
    """meshes whose outputs are empty: no triangles, only degenerate ones, one triangle, two (a step with no changes)"""
    q = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0), (1.0, 0.0, 1.0)]
    yield [], [], [], [], []
    yield q[:3], [0, 1, 2], [(0, 0, 1)], [False] * 3, [1.0] * 3
    yield q[:3], [0, 1, 2], [(0, 1, 2)], [False] * 3, [1.0] * 3
    yield q, [0, 1, 2, 3], [(0, 2, 1), (1, 2, 3)], [False] * 4, [1.0] * 4
    yield q, [0, 1, 2, 3], [(0, 2, 1), (1, 2, 3)], [True] * 4, [1.0] * 4


def _test(count=40):
    import time
    for k, job in enumerate(_edge_cases()):
        for ak in (None, 0.0):
            a, b = pysimplify.simplify(*job, area_k=ak), simplify(*job, area_k=ak)
            if a != b:
                print(f"MISMATCH edge case {k}: {a} vs {b}")
                return 1
    rng = np.random.default_rng(1)
    tp = tn = 0.0
    for k in range(count):
        job = _test_mesh(rng, int(rng.integers(30, 2500)))
        ak = [None, 0.0, 2.5][k % 3]
        t = time.time()
        a = pysimplify.simplify(*job, area_k=ak)
        tp += time.time() - t
        t = time.time()
        b = simplify(*job, area_k=ak)
        tn += time.time() - t
        if a != b:
            na, nb = len(a[1]), len(b[1])
            first = next((i for i, (x, y) in enumerate(zip(a[1], b[1])) if x != y), min(na, nb))
            print(f"MISMATCH mesh {k}: {len(job[2])} tris, steps {na} vs {nb}, first difference at step {first}:",
                  a[1][first:first + 1], b[1][first:first + 1])
            return 1
    print(f"5 edge cases and {count} meshes identical; simplify.py {tp:.2f} s, C {tn:.2f} s ({tp / max(tn, 1e-9):.0f}x)")
    return 0


if __name__ == "__main__":
    if sys.argv[1:2] == ["--test"]:
        sys.exit(_test(int(sys.argv[2]) if len(sys.argv) > 2 else 40))
    print(__doc__)
