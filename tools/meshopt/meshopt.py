"""ctypes binding for meshoptimizer (github.com/zeux/meshoptimizer v1.3, MIT), built as tools/meshopt/libmeshopt.so from
the source in tools/ext/meshoptimizer:
    g++ -O2 -shared -fPIC -o tools/meshopt/libmeshopt.so tools/ext/meshoptimizer/src/*.cpp
All arrays are numpy: positions float32 (n, 3), indices uint32 (m,) (triangle list, 3 per triangle).
    simplify(pos, idx, target_tris, error=1e9, lock_border=False, attrs=None, weights=None, lock=None, absolute=False,
             prune=False, options=0)   options: OR of LOCK_BORDER .. PERMISSIVE (meshopt_Simplify*)
        -> (idx, result_error)   quadric simplification; vertices with equal positions are treated as one for
           topology, so UV / material seams (split vertices) are kept and only slide along themselves
    stripify(idx, nverts, restart=0xFFFFFFFF) -> strip indices (restart index between strips)
    optimize_vertex_cache(idx, nverts) -> idx       optimize_vertex_fetch(idx, verts) -> (idx, verts)
    cache_stats(idx, nverts, cache=16) -> (acmr, atvr)
    point_mesh_distance(points, pos, idx) -> float32 distances (meshdist.c, uniform grid; for error measurements)
Test: meshopt.py --test"""
import ctypes, os, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "ext", "meshoptimizer", "src")
LOCK_BORDER, SPARSE, ERROR_ABSOLUTE, PRUNE, REGULARIZE, PERMISSIVE = 1, 2, 4, 8, 16, 32
_L = _D = None
f32p = np.ctypeslib.ndpointer(np.float32, flags="C")
u32p = np.ctypeslib.ndpointer(np.uint32, flags="C")
u8p = np.ctypeslib.ndpointer(np.uint8, flags="C")
sz = ctypes.c_size_t


class _Stats(ctypes.Structure):
    _fields_ = [("vertices_transformed", ctypes.c_uint), ("warps_executed", ctypes.c_uint),
                ("acmr", ctypes.c_float), ("atvr", ctypes.c_float)]


def lib():
    global _L
    if _L:
        return _L
    so = os.path.join(HERE, "libmeshopt.so")
    if not os.path.exists(so):
        if os.system(f"g++ -O2 -shared -fPIC -o '{so}' '{SRC}'/*.cpp"):
            sys.exit("meshopt.py: building libmeshopt.so failed (needs tools/ext/meshoptimizer)")
    L = ctypes.CDLL(so)
    L.meshopt_simplifyWithAttributes.restype = sz
    L.meshopt_simplifyWithAttributes.argtypes = [u32p, u32p, sz, f32p, sz, sz, ctypes.c_void_p, sz, ctypes.c_void_p,
                                                 sz, ctypes.c_void_p, sz, ctypes.c_float, ctypes.c_uint,
                                                 ctypes.POINTER(ctypes.c_float)]
    L.meshopt_stripify.restype = sz
    L.meshopt_stripify.argtypes = [u32p, u32p, sz, sz, ctypes.c_uint]
    L.meshopt_stripifyBound.restype = sz
    L.meshopt_stripifyBound.argtypes = [sz]
    L.meshopt_optimizeVertexCache.argtypes = [u32p, u32p, sz, sz]
    L.meshopt_optimizeVertexFetch.restype = sz
    L.meshopt_optimizeVertexFetch.argtypes = [ctypes.c_void_p, u32p, sz, ctypes.c_void_p, sz, sz]
    L.meshopt_analyzeVertexCache.restype = _Stats
    L.meshopt_analyzeVertexCache.argtypes = [u32p, sz, sz, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
    _L = L
    return L


def simplify(pos, idx, target_tris, error=1e9, lock_border=False, attrs=None, weights=None, lock=None,
             absolute=False, prune=False, options=0):
    pos = np.ascontiguousarray(pos, np.float32)
    idx = np.ascontiguousarray(idx, np.uint32)
    out = np.empty_like(idx)
    err = ctypes.c_float()
    opts = options | (LOCK_BORDER if lock_border else 0) | (ERROR_ABSOLUTE if absolute else 0) | (PRUNE if prune else 0)
    a = w = None
    na = 0
    if attrs is not None:
        attrs = np.ascontiguousarray(attrs, np.float32)
        weights = np.ascontiguousarray(weights if weights is not None else np.ones(attrs.shape[1]), np.float32)
        a, w, na = attrs.ctypes.data, weights.ctypes.data, attrs.shape[1]
    lk = np.ascontiguousarray(lock, np.uint8).ctypes.data if lock is not None else None
    n = lib().meshopt_simplifyWithAttributes(out, idx, len(idx), pos, len(pos), 12, a, 4 * na, w, na, lk,
                                             3 * int(target_tris), error, opts, ctypes.byref(err))
    return out[:n].copy(), err.value


def stripify(idx, nverts, restart=0xFFFFFFFF):
    idx = np.ascontiguousarray(idx, np.uint32)
    out = np.empty(lib().meshopt_stripifyBound(len(idx)), np.uint32)
    n = lib().meshopt_stripify(out, idx, len(idx), nverts, restart)
    return out[:n].copy()


def optimize_vertex_cache(idx, nverts):
    idx = np.ascontiguousarray(idx, np.uint32)
    out = np.empty_like(idx)
    lib().meshopt_optimizeVertexCache(out, idx, len(idx), nverts)
    return out


def optimize_vertex_fetch(idx, verts):
    idx = np.array(idx, np.uint32)
    verts = np.ascontiguousarray(verts)
    out = np.empty_like(verts)
    n = lib().meshopt_optimizeVertexFetch(out.ctypes.data, idx, len(idx), verts.ctypes.data, len(verts),
                                          verts.itemsize * int(np.prod(verts.shape[1:])))
    return idx, out[:n]


def cache_stats(idx, nverts, cache=16):
    idx = np.ascontiguousarray(idx, np.uint32)
    s = lib().meshopt_analyzeVertexCache(idx, len(idx), nverts, cache, 0, 0)
    return s.acmr, s.atvr


def point_mesh_distance(points, pos, idx):
    global _D
    if not _D:
        so, src = os.path.join(HERE, "libmeshdist.so"), os.path.join(HERE, "meshdist.c")
        if not os.path.exists(so) or os.path.getmtime(so) < os.path.getmtime(src):
            if os.system(f"gcc -O2 -shared -fPIC -o '{so}' '{src}' -lm"):
                sys.exit("meshopt.py: building libmeshdist.so failed")
        _D = ctypes.CDLL(so)
        _D.point_mesh_distance.argtypes = [f32p, sz, f32p, sz, u32p, sz, f32p]
    points = np.ascontiguousarray(points, np.float32)
    pos = np.ascontiguousarray(pos, np.float32)
    idx = np.ascontiguousarray(idx, np.uint32)
    out = np.empty(len(points), np.float32)
    _D.point_mesh_distance(points, len(points), pos, len(pos), idx, len(idx) // 3, out)
    return out


def _test():
    # 64x64 grid with a bump, a UV seam down the middle (split vertices)
    n = 64
    g = np.stack(np.meshgrid(np.arange(n), np.arange(n), indexing="ij"), -1).reshape(-1, 2).astype(np.float32)
    z = np.exp(-((g[:, 0] - 32) ** 2 + (g[:, 1] - 20) ** 2) / 50.0) * 8
    pos = np.column_stack([g[:, 0], z, g[:, 1]]).astype(np.float32)
    q = np.arange(n * n).reshape(n, n)
    tri = np.concatenate([np.stack([q[:-1, :-1], q[1:, :-1], q[1:, 1:]], -1).reshape(-1, 3),
                          np.stack([q[:-1, :-1], q[1:, 1:], q[:-1, 1:]], -1).reshape(-1, 3)])
    idx = tri.reshape(-1).astype(np.uint32)
    out, err = simplify(pos, idx, len(tri) // 10)
    d = point_mesh_distance(pos, pos, out)
    strip = stripify(optimize_vertex_cache(out, len(pos)), len(pos))
    print(f"grid {len(tri)} tris -> {len(out) // 3} (error {err:.4f}), max vertex distance {d.max():.3f}, "
          f"strip {len(strip)} indices for {len(out)} list indices, ACMR {cache_stats(out, len(pos))[0]:.2f} -> "
          f"{cache_stats(optimize_vertex_cache(out, len(pos)), len(pos))[0]:.2f}")
    assert len(out) // 3 <= len(tri) // 10 + 2 and d.max() < 1.0 and abs(point_mesh_distance(pos, pos, idx)).max() < 1e-5
    print("ok")


if __name__ == "__main__":
    _test()
