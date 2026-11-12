"""ctypes binding for tools/mapx (libmapx), the map exporter's C rewrite (see tools/mapx/mapx.h).

  lib()   tools/mapx/libmapx.so, built on first use from tools/mapx/*.c and rebuilt when any source or header is newer
          ($CC, temp file + os.replace: parallel exporters may build at once). None when MAPX=0 or when it cannot be
          built: export_map.py then runs its Python code, which writes the same bytes (the reference).
In the in-browser converter the library is a Pyodide side module (tools/native/build-wasm.sh, single-threaded).
MAPX_THREADS=n caps the threads (default: every CPU); the output never depends on it."""
import ctypes, glob, os

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mapx")
_L = []


def sources():
    """the files the library is built from (tools/mapx/simp.c includes tools/native/simplify.c)"""
    return sorted(glob.glob(os.path.join(HERE, "*.c")) + glob.glob(os.path.join(HERE, "*.h"))) + [
        os.path.join(os.path.dirname(HERE), "native", "simplify.c")]


def _build(so):
    import subprocess, tempfile
    fd, tmp = tempfile.mkstemp(suffix=".so", dir=HERE)
    os.close(fd)
    try:
        subprocess.run([os.environ.get("CC", "cc"), "-O2", "-ffp-contract=off", "-fno-fast-math", "-fvisibility=hidden",
                        "-shared", "-fPIC", "-pthread", "-o", tmp] + sorted(glob.glob(os.path.join(HERE, "*.c"))) + ["-lm"],
                       check=True, capture_output=True)
        os.replace(tmp, so)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def lib():
    if not _L:
        L = None
        if os.environ.get("MAPX", "1") != "0":
            import sys
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # run as a script with -I: not on the path
            import native_so
            so = native_so.so(os.path.join(HERE, "libmapx"))
            try:
                if not os.path.exists(so) or any(os.path.getmtime(so) < os.path.getmtime(s) for s in sources()):
                    _build(so)
                L = ctypes.CDLL(so)
                vp, i, ll, d = ctypes.c_void_p, ctypes.c_int, ctypes.c_longlong, ctypes.c_double
                L.mx_navgrid_new.argtypes = [ll, vp]
                L.mx_navgrid_new.restype = vp
                L.mx_navgrid_free.argtypes = [vp]
                L.mx_navgrid_free.restype = None
                L.mx_thin_foliage.argtypes = [i, vp, i, vp, vp, vp, vp, i, d, ctypes.c_char_p, i, i, d, d, d, vp, vp, vp,
                                              vp, vp]
                L.mx_thin_foliage.restype = i
                L.mx_soften_thin.argtypes = [i, vp, vp, vp, vp, vp, i, vp, d, d, d, d, d, d, vp, vp]
                L.mx_soften_thin.restype = ll
                L.mx_far_cells.argtypes = [ll, vp, ll, vp, d, d, vp, vp]
                L.mx_far_cells.restype = ll
                L.mx_chunks_st_align.argtypes = [i]
                L.mx_chunks_st_align.restype = None
                L.mx_simp_new.argtypes = [i, vp, vp, vp, vp, vp, vp, vp, vp, i, d, ll, vp, ll, vp, ll, vp, vp, d, d, i, vp,
                                          d, d, d, d, d, d, d, d, d, d, d, d]
                L.mx_simp_new.restype = vp
                for f, a, r in (("mx_simp_stats", [vp, vp], None), ("mx_simp_dup", [vp, vp], None),
                                ("mx_simp_jobs", [vp, vp, vp], None), ("mx_simp_count", [vp, d], ll),
                                ("mx_simp_replay", [vp, d, d, i], ll), ("mx_simp_refit", [vp, vp, vp, vp], None), ("mx_simp_envelope", [d], None), ("mx_simp_guard", [d, d], None), ("mx_simp_shape", [d], None), ("mx_simp_check", [vp, d, d, d, vp, vp, vp], ll), ("mx_simp_back_off", [vp, vp], ll), ("mx_simp_swap_jobs", [vp, vp, vp], ll), ("mx_simp_job_nout", [vp, vp], None), ("mx_simp_job_counts", [vp, d, vp], None),
                                ("mx_simp_replay_jobs", [vp, vp], ll), ("mx_simp_fetch", [vp, vp, vp, vp, vp, vp], None),
                                ("mx_simp_free", [vp], None),
                                ("mx_chunks_new", [i] + [vp] * 18 + [i, d, vp, d, d, i, i, d, ll, ll], vp),
                                ("mx_chunks_count", [vp, vp], i), ("mx_chunks_fetch", [vp] + [vp] * 10, None),
                                ("mx_chunks_free", [vp], None),
                                ("mx_first_rows", [ll, i, vp, vp, vp], ll), ("mx_any_within", [ll, vp, ll, vp, d], i),
                                ("mx_probe_new", [ll, vp, vp, d, ll], vp), ("mx_probe_query", [vp, ll, vp, vp], None),
                                ("mx_probe_free", [vp], None), ("mx_pow_many", [ll, vp, d, i, vp], None),
                                ("mx_faceset_tris", [ll, vp, i, vp], ll),
                                ("mx_skin", [ll, vp, vp, vp, vp, ll, vp, vp, vp], None),
                                ("mx_shadow_map", [ll, vp, vp, vp, d, i, i, vp], None),
                                ("mx_dedup_tris", [ll, vp, vp, ll, vp, vp], ll),
                                ("mx_weld", [ll, vp, vp, vp, vp, vp, vp, vp, vp, vp, vp], None),
                                ("mx_probe_faces", [vp, i, i, i, vp], None),
                                ("mx_wedge_light", [ll, vp, vp, vp, vp, vp, vp, i, i, vp, vp, vp, vp, vp], None),
                                ("mx_shadow_lit", [ll, vp, vp, vp, vp, d, vp, i, i, d, vp], None)):
                    getattr(L, f).argtypes, getattr(L, f).restype = a, r
            except Exception as e:
                print(f"mapx: C library unavailable ({str(e)[:300]}), using the Python export", flush=True)
                L = None
        _L.append(L)
    return _L[0]


class Tris:
    """Triangles as corner arrays, the form the C stages pass along (simplify -> soften / bump / chunks): P (n, 3, 3)
    float64 positions, W (n, 3) int32 wedges. len / iteration / indexing give the list it stands for, triangles
    ((x, y, z), w) * 3 of Python floats and ints (the values np.array took from that list); an index array or mask
    gives a Tris."""
    __slots__ = ("P", "W")

    def __init__(self, P, W):
        self.P, self.W = P, W

    def __len__(self):
        return len(self.W)

    def __iter__(self):
        for p, w in zip(self.P.tolist(), self.W.tolist()):
            yield (tuple(p[0]), w[0]), (tuple(p[1]), w[1]), (tuple(p[2]), w[2])

    def __getitem__(self, k):
        import numpy as np
        if isinstance(k, (int, np.integer)):
            p, w = self.P[k].tolist(), self.W[k].tolist()
            return (tuple(p[0]), w[0]), (tuple(p[1]), w[1]), (tuple(p[2]), w[2])
        return Tris(np.ascontiguousarray(self.P[k]), np.ascontiguousarray(self.W[k]))


def tri_arrays(tl):
    """(P (n, 3, 3) float64, W (n, 3) int32) of a Tris or of a list of corner triples ((x, y, z), w)"""
    if isinstance(tl, Tris):
        return tl.P, tl.W
    import numpy as np
    return (np.array([[c[0] for c in t] for t in tl], np.float64).reshape(-1, 3, 3),
            np.array([[c[1] for c in t] for t in tl], np.int32).reshape(-1, 3))
