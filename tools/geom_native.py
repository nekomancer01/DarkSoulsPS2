"""ctypes binding for tools/native/geom.c, the map export's C helpers (each caller keeps a numpy path with the same
results, used when the library cannot be built, e.g. no compiler):
  dsr_light.ShadowMap    shadow_raster / shadow_lit: the sun shadow depth map and its 3x3 PCF lookup
  export_map.view_distance  nearest_d2: distance to the nearest navmesh sample (k-d tree)
  texmap._nearest        nearest_rgb: nearest palette entry (integer RGB)
In the in-browser converter the library is a Pyodide side module (tools/native/build-wasm.sh)."""
import ctypes, os

_GEOM = []


def lib():
    """tools/native/libgeom.so, built on first use like libsimplify.so; None when it cannot be built (the callers'
    numpy paths give the same results). GEOM_NATIVE=0: numpy."""
    if not _GEOM:
        L = None
        if os.environ.get("GEOM_NATIVE", "1") != "0":
            native = os.path.join(os.path.dirname(os.path.abspath(__file__)), "native")
            src, so = os.path.join(native, "geom.c"), os.path.join(native, "libgeom.so")
            try:
                if not os.path.exists(so) or os.path.getmtime(so) < os.path.getmtime(src):
                    import subprocess, tempfile
                    fd, tmp = tempfile.mkstemp(suffix=".so", dir=native)
                    os.close(fd)
                    try:
                        subprocess.run([os.environ.get("CC", "cc"), "-O2", "-ffp-contract=off", "-fno-fast-math",
                                        "-shared", "-fPIC", "-o", tmp, src], check=True, capture_output=True)
                        os.replace(tmp, so)  # atomic: parallel exporters may build at once
                    finally:
                        if os.path.exists(tmp):
                            os.remove(tmp)
                L = ctypes.CDLL(so)
                vp, i = ctypes.c_void_p, ctypes.c_int
                L.shadow_raster.argtypes = [i, vp, vp, vp, vp, vp, vp, i, i, vp]
                L.shadow_raster.restype = None
                L.shadow_lit.argtypes = [i, vp, vp, i, i, ctypes.c_double, vp]
                L.shadow_lit.restype = None
                L.nearest_d2.argtypes = [i, vp, i, vp, vp]
                L.nearest_d2.restype = i
                L.nearest_rgb.argtypes = [i, vp, i, vp, vp]
                L.nearest_rgb.restype = None
            except Exception as e:  # no compiler: numpy paths, slower
                print(f"geom: C helpers unavailable ({str(e)[:200]}), using numpy", flush=True)
                L = None
        _GEOM.append(L)
    return _GEOM[0]
