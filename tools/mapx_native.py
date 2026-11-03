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
            so = os.path.join(HERE, "libmapx.so")
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
                L.mx_simp_new.argtypes = [i, vp, vp, vp, vp, vp, vp, vp, vp, i, d, ll, vp, ll, vp, ll, vp, vp, d, d, i, vp,
                                          d, d, d, d, d, d, d]
                L.mx_simp_new.restype = vp
                for f, a, r in (("mx_simp_stats", [vp, vp], None), ("mx_simp_dup", [vp, vp], None),
                                ("mx_simp_jobs", [vp, vp, vp], None), ("mx_simp_count", [vp, d], ll),
                                ("mx_simp_replay", [vp, d, d, i], ll), ("mx_simp_fetch", [vp, vp, vp, vp, vp, vp], None),
                                ("mx_simp_free", [vp], None),
                                ("mx_chunks_new", [i] + [vp] * 18 + [i, d, vp, d, d, i, i, d, ll, ll], vp),
                                ("mx_chunks_count", [vp, vp], i), ("mx_chunks_fetch", [vp] + [vp] * 10, None),
                                ("mx_chunks_free", [vp], None),
                                ("mx_first_rows", [ll, i, vp, vp, vp], ll), ("mx_any_within", [ll, vp, ll, vp, d], i),
                                ("mx_probe_new", [ll, vp, vp, d, ll], vp), ("mx_probe_query", [vp, ll, vp, vp], None),
                                ("mx_probe_free", [vp], None), ("mx_pow_many", [ll, vp, d, i, vp], None)):
                    getattr(L, f).argtypes, getattr(L, f).restype = a, r
            except Exception as e:
                print(f"mapx: C library unavailable ({str(e)[:300]}), using the Python export", flush=True)
                L = None
        _L.append(L)
    return _L[0]
