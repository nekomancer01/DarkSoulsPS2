"""DSR's map lighting, fog colour and tone map, decoded from the game, for baking into PS2 vertex colours.

Sources (DarkSoulsRemastered.exe in Ghidra, shader HLSL embedded in shader/FRPG_*_DX11.shaderbnd):
- DrawParam colours (LightBank directional / hemisphere / envDif, FogBank, PointLightBank) become shader constants as
  pow(col / 255 x colA / 100, 2.2): the % multiplier is applied before the 2.2 power (FUN_140564510 / FUN_1405642f0
  lights, FUN_1401c8b00 fog colour), so colA 150 % is 1.5^2.2 = 2.44x in linear light.
- FogBank -> gFC_FogParam (begin, 1 / (end - begin), 0, degRotW / 100), gFC_FogCol (linear colour, degRotW / 100)
  (FUN_140565600 / FUN_1401c8650); shader: lerp(col, fogCol, saturate(max x saturate((view z - begin) x param.y)))
  on the linear HDR colour before the tone map (FRPG_Deferred_LightProbe.fpo).
- Diffuse light: dir lights + hemisphere lerp(down, up, n.y / 2 + 0.5) + envDif colour x the light probe's diffuse
  cube (map/mXX/GI_EnvM_mXX.tpfbhd: GI_EnvDif_mAA_BB_NNNN, BC6H 64 px cubes; NNNN = the collision part hNNNN).
- Tone map: Hable / Uncharted 2 with DSR's own constants (FUN_140458080: data 0x141a28d9c..b4): A 0.1, B 0.3, C 0.1,
  D 0.25, E 0.02, F 0.3, linear white 4.0; exposure grayKeyValue / adapted luminance clamped to [minAdaptedLum,
  maxAdapredLum] (FlverPBL ReverseToneMap is its inverse).
- ToneCorrectBank (brightness, contrast, saturation per area row) after the tone map. ASSUMED formula (the filter
  shader has no source): c = ((c - 0.5) x contrast + 0.5) x brightness, then lerp(luma, c, saturation), sRGB.
- Point lights (FRPG_Deferred_PointLight.fpo, linear falloff mode): saturate((end - d) / (end - begin)) x colour x N.L.
"""
import ctypes, json, math, os, struct

import souls
from geom_native import lib as geom_lib

HABLE = (0.1, 0.3, 0.1, 0.25, 0.02, 0.3)
WHITE = 4.0
LUMA = (0.299, 0.587, 0.114)
LUMA_TC = (0.3086, 0.6094, 0.0820)  # ToneCorrect's saturation weights (Haeberli), measured


def lin(c, a):
    """DrawParam colour component (0..255) and % multiplier -> linear shader constant."""
    return abs(c / 255.0 * a / 100.0) ** 2.2


def _u2(x):
    A, B, C, D, E, F = HABLE
    return (x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F


_U2W = _u2(WHITE)


class Tone:
    """One area's tone map (ToneMapBank row, exposure) + tone correction (ToneCorrectBank row): linear HDR -> sRGB."""

    def __init__(self, tm, tc, exposure=None):
        self.key = tm["grayKeyValue"]
        self.lo, self.hi = tm["minAdaptedLum"], tm["maxAdapredLum"]
        self.exposure = exposure if exposure else self.key / self.hi  # bright-scene clamp until adapt() is called
        self.tc = tc

    def adapt(self, scene_lum):
        """Auto exposure for an average scene luminance (linear HDR)."""
        self.exposure = self.key / min(self.hi, max(self.lo, scene_lum))

    def __call__(self, rgb):
        out = [max(0.0, min(1.0, _u2(max(0.0, x) * self.exposure) / _U2W)) ** (1 / 2.2) for x in rgb]
        tc = self.tc
        if tc:  # MEASURED (RenderDoc, FRPG_Fil_HDR_PBL_ColAdj c62-c64, Firelink row 20 and Burg row 11): saturation
            # around Haeberli luma, x contrast x brightness, + 0.5 (1 - contrast)
            y = sum(k * v for k, v in zip(LUMA_TC, out))
            out = [tc[f"brightness{c}"] * tc[f"contrast{c}"] * (tc["saturation"] * v + (1 - tc["saturation"]) * y)
                   + 0.5 * (1 - tc[f"contrast{c}"]) for v, c in zip(out, "RGB")]
        return [max(0.0, min(1.0, v)) for v in out]

    def many(self, X):
        """__call__ on the rows of an (n, 3) array, the same operations per element (min / max as Python's: the
        first argument unless the second is strictly smaller / larger; ** per element in Python)"""
        import numpy as np
        A, B, C, D, E, F = HABLE
        x = np.where(X > 0.0, X, 0.0) * self.exposure
        u = ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F) - E / F) / _U2W
        u = np.where(u < 1.0, u, 1.0)
        u = np.where(u > 0.0, u, 0.0)
        g = 1 / 2.2
        out = _pow_many(u, g, False)
        tc = self.tc
        if tc:
            y = sum3(LUMA_TC[0] * out[:, 0], LUMA_TC[1] * out[:, 1], LUMA_TC[2] * out[:, 2])
            sat = tc["saturation"]
            out = np.stack([tc[f"brightness{c}"] * tc[f"contrast{c}"] * (sat * out[:, j] + (1 - sat) * y)
                            + 0.5 * (1 - tc[f"contrast{c}"]) for j, c in enumerate("RGB")], 1)
        out = np.where(out < 1.0, out, 1.0)
        return np.where(out > 0.0, out, 0.0)


def srgb_lin(v):
    return max(0.0, v) ** 2.2


def srgb_lin_many(X):
    """srgb_lin per element of a float array (Python's ** per element: numpy's power may round differently)"""
    import numpy as np
    return _pow_many(np.asarray(X, np.float64), 2.2, True)


def _pow_many(X, e, pos):
    """Python's x ** e per element of a float64 array (pos: 0.0 where x <= 0), in C when libmapx is there (libm pow,
    what float.__pow__ calls)"""
    import numpy as np
    try:
        import mapx_native
        L = mapx_native.lib()
    except ImportError:
        L = None
    if L is not None:
        Xc = np.ascontiguousarray(X, np.float64)
        out = np.empty(Xc.shape)
        L.mx_pow_many(Xc.size, Xc.ctypes.data, e, 1 if pos else 0, out.ctypes.data)
        return out
    if pos:
        return np.array([x ** e if x > 0.0 else 0.0 for x in X.ravel().tolist()]).reshape(X.shape)
    return np.array([v ** e for v in X.ravel().tolist()]).reshape(X.shape)


def sum3(a, b, c):
    """Python's sum((a, b, c)) per element of float arrays, bit for bit: from Python 3.12 sum() of floats adds with
    Neumaier compensation (start 0, compensation added at the end when non-zero and finite), before that left to
    right. The vectorised bake / chunk code uses it wherever the per-element code summed three floats."""
    import sys
    import numpy as np
    f = 0.0 + a
    if sys.version_info < (3, 12):
        return (f + b) + c
    comp = np.zeros_like(f)
    for x in (b, c):
        t = f + x
        comp = comp + np.where(np.abs(f) >= np.abs(x), (f - t) + x, (x - t) + f)
        f = t
    return np.where((comp != 0) & np.isfinite(comp), f + comp, f)


def matmul(X, M):
    """X @ M without BLAS: the products summed left to right over the inner axis. Native numpy's @ goes through
    OpenBLAS and Pyodide's (the in-browser converter) does not, so their last bits differ (TECH_DEBT build-7); every
    product the exports keep goes through here instead, the same bits on every platform. M: (k,) or (k, m)."""
    import numpy as np
    X, M = np.asarray(X), np.asarray(M)
    x = (lambda k: X[..., k]) if M.ndim == 1 else (lambda k: X[..., k, None])
    out = x(0) * M[0]
    for k in range(1, M.shape[0]):
        out = out + x(k) * M[k]
    return out


# ---- light probes ----------------------------------------------------------------------------------------------
_FACE = [  # D3D cube faces +X -X +Y -Y +Z -Z: (major axis, sign, s axis/sign, t axis/sign)
    (0, 1, (2, -1), (1, -1)), (0, -1, (2, 1), (1, -1)), (1, 1, (0, 1), (2, 1)),
    (1, -1, (0, 1), (2, -1)), (2, 1, (0, 1), (1, -1)), (2, -1, (0, -1), (1, -1))]
PROBE_RES = 8


def load_probes(game, map_id):
    """{probe number: [6 faces][PROBE_RES][PROBE_RES] linear rgb} for the map's GI_EnvDif cubes."""
    lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), "native", "libbcdecode.so"))
    area = map_id[:3]
    d = f"{game}/map/{area}/GI_EnvM_{area}"
    pre = f"GI_EnvDif_{map_id[:6]}_"
    buf = (ctypes.c_float * 48)()
    out = {}
    for _, _, tb in souls.bxf3(d + ".tpfbhd", d + ".tpfbdt"):
        for name, dds in souls.tpf(tb).items():
            if not name.startswith(pre):
                continue
            h, w = struct.unpack_from("<2I", dds, 12)
            if struct.unpack_from("<I", dds, 128)[0] != 95:  # BC6H_UF16 only
                continue
            off, faces, step = 148, [], w // PROBE_RES
            for _f in range(6):
                acc = [[[0.0, 0.0, 0.0] for _ in range(PROBE_RES)] for _ in range(PROBE_RES)]
                for by in range(h // 4):
                    for bx in range(w // 4):
                        lib.bcdec_bc6h_float(ctypes.c_char_p(dds[off:off + 16]), buf, 12, 0)
                        off += 16
                        for i in range(16):
                            y, x = by * 4 + i // 4, bx * 4 + i % 4
                            cell = acc[y // step][x // step]
                            for c in range(3):
                                cell[c] += buf[i * 3 + c]
                n = step * step
                faces.append([[[v / n for v in cell] for cell in row] for row in acc])
            out[int(name[len(pre):])] = faces
    return out


def load_spec_probes(game, map_id, mip=5):
    """{probe number: [6 face means of linear rgb]} of the GI_EnvSpc cubes (BC6H, 256 px, 8 mips, face-major) at a
    blurred mip: the rough (r 0.7-0.85) DSR materials reflect about this much of the sky / ground per direction."""
    lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), "native", "libbcdecode.so"))
    area = map_id[:3]
    d = f"{game}/map/{area}/GI_EnvM_{area}"
    pre = f"GI_EnvSpc_{map_id[:6]}_"
    buf = (ctypes.c_float * 48)()
    out = {}
    for _, _, tb in souls.bxf3(d + ".tpfbhd", d + ".tpfbdt"):
        for name, dds in souls.tpf(tb).items():
            if not name.startswith(pre) or struct.unpack_from("<I", dds, 128)[0] != 95:
                continue
            h, w = struct.unpack_from("<2I", dds, 12)
            mips = max(1, struct.unpack_from("<I", dds, 28)[0])
            sizes = [max(1, (w >> i) // 4) ** 2 for i in range(mips)]
            faces = []
            for f in range(6):
                off = 148 + 16 * (f * sum(sizes) + sum(sizes[:mip]))
                acc = [0.0, 0.0, 0.0]
                for b in range(sizes[mip]):
                    lib.bcdec_bc6h_float(ctypes.c_char_p(dds[off + 16 * b:off + 16 * b + 16]), buf, 12, 0)
                    for i in range(16):
                        for c in range(3):
                            acc[c] += buf[i * 3 + c]
                faces.append([a / (16 * sizes[mip]) for a in acc])
            out[int(name[len(pre):].split("_")[0])] = faces
    return out


def env_hemispheres(faces):
    """(up, down) reflection colours from 6 face means (+x -x +y -y +z -z): half the pole face, half the sides"""
    side = [sum(faces[i][c] for i in (0, 1, 4, 5)) / 4 for c in range(3)]
    return [0.5 * faces[2][c] + 0.5 * side[c] for c in range(3)], [0.5 * faces[3][c] + 0.5 * side[c] for c in range(3)]


def probe_sample(faces, n):
    ax = max(range(3), key=lambda k: abs(n[k]))
    f = (0 if n[ax] > 0 else 1) + 2 * ax
    _, _, (sa, ss), (ta, ts) = _FACE[f]
    m = abs(n[ax]) or 1.0
    u = min(PROBE_RES - 1, int((ss * n[sa] / m + 1) / 2 * PROBE_RES))
    v = min(PROBE_RES - 1, int((ts * n[ta] / m + 1) / 2 * PROBE_RES))
    return faces[f][v][u]


_FACES_NP = {}  # id(probe faces) -> (faces, (6, R, R, 3) array)


def probe_sample_many(faces, N):
    """probe_sample per row of (n, 3) normals N with per-row probe faces (list; None = no probe: zeros)"""
    import numpy as np
    out = np.zeros((len(N), 3))
    if not len(N):
        return out
    ax = np.argmax(np.abs(N), 1)  # the first of equal magnitudes, as max(range(3), key=...)
    rows = np.arange(len(N))
    na = N[rows, ax]
    m = np.abs(na)
    m = np.where(m != 0, m, 1.0)  # abs(n[ax]) or 1.0
    f = np.where(na > 0, 0, 1) + 2 * ax
    u, v = np.zeros(len(N), np.int64), np.zeros(len(N), np.int64)
    for fi, (_, _, (sa, ss), (ta, ts)) in enumerate(_FACE):
        k = f == fi
        u[k] = np.minimum(PROBE_RES - 1, np.trunc((ss * N[k, sa] / m[k] + 1) / 2 * PROBE_RES).astype(np.int64))
        v[k] = np.minimum(PROBE_RES - 1, np.trunc((ts * N[k, ta] / m[k] + 1) / 2 * PROBE_RES).astype(np.int64))
    groups = {}
    for i, fc in enumerate(faces):
        if fc is not None:
            groups.setdefault(id(fc), (fc, []))[1].append(i)
    for fc, sel in groups.values():
        e = _FACES_NP.get(id(fc))
        if e is None or e[0] is not fc:
            e = _FACES_NP[id(fc)] = (fc, np.array(fc, np.float64))
        sel = np.array(sel, np.int64)
        out[sel] = e[1][f[sel], v[sel], u[sel]]
    return out


class ProbeIndex:
    """World position -> light probe: the probe of the nearest collision triangle (4 m grid of centroids) whose part
    has a GI_EnvDif cube (part_probe: collision part name -> probe number, from its MSB env_event; DSR lights a map
    piece from the probe of the collision part it stands on); far from any collision: the most used probe."""

    def __init__(self, subs, probes, part_probe, cell=4.0):
        self.cell, self.grid, self.probes = cell, {}, probes
        self.mx = None
        try:
            import mapx_native
            L = mapx_native.lib()
        except ImportError:
            L = None
        if L is not None:  # tools/mapx/probe.c: the same centroids, search, cache
            self._init_c(L, subs, probes, part_probe)
            return
        use = {}
        for s in subs:
            p = part_probe.get(s["part"], -1)
            if p not in probes:
                continue
            for t in s["tris"]:
                c = [sum(v[k] for v in t) / 3 for k in range(3)]
                self.grid.setdefault(self._key(c), []).append((c, p))
                use[p] = use.get(p, 0) + 1
        self.default = max(use, key=use.get) if use else None
        self.cache = {}

    def _init_c(self, L, subs, probes, part_probe):
        import numpy as np
        use, T, pr, ids = {}, [], [], {}
        for s in subs:
            p = part_probe.get(s["part"], -1)
            if p not in probes or not s["tris"]:
                continue
            T.append(np.array(s["tris"], np.float64).reshape(-1, 9))
            pr.append(np.full(len(s["tris"]), ids.setdefault(p, len(ids)), np.int64))
            use[p] = use.get(p, 0) + len(s["tris"])
        self.default = max(use, key=use.get) if use else None
        self.cache = {}
        self.vals = list(ids)
        T = np.ascontiguousarray(np.concatenate(T) if T else np.zeros((0, 9)))
        pr = np.ascontiguousarray(np.concatenate(pr) if pr else np.zeros(0, np.int64))
        self.L = L
        self.mx = L.mx_probe_new(len(T), T.ctypes.data, pr.ctypes.data, self.cell,
                                 ids[self.default] if self.default is not None else -1)

    def __del__(self):
        if getattr(self, "mx", None):
            self.L.mx_probe_free(self.mx)
            self.mx = None

    def _key(self, p):
        return tuple(int(math.floor(x / self.cell)) for x in p)

    def __call__(self, p):
        if self.mx:
            return self.many([p])[0]
        k = self._key(p)
        if k in self.cache:
            return self.cache[k]
        best, bd = self.default, 1e18
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for c, pr in self.grid.get((k[0] + dx, k[1] + dy, k[2] + dz), ()):
                        d = (c[0] - p[0]) ** 2 + (c[1] - p[1]) ** 2 + (c[2] - p[2]) ** 2
                        if d < bd:
                            best, bd = pr, d
        self.cache[k] = best
        return best

    def many(self, P):
        """__call__ for the rows of an (n, 3) array, in row order (the cache keeps the first query's answer per
        cell, so the order matters)"""
        import numpy as np
        P = np.asarray(P, np.float64).reshape(-1, 3)
        if self.mx:
            P = np.ascontiguousarray(P)
            r = np.empty(len(P), np.int64)
            self.L.mx_probe_query(self.mx, len(P), P.ctypes.data, r.ctypes.data)
            vals = self.vals
            return [vals[i] if i >= 0 else None for i in r.tolist()]
        cache, miss, out = self.cache, object(), []
        for k, p in zip(np.floor(P / self.cell).astype(np.int64).tolist(), P.tolist()):
            v = cache.get(tuple(k), miss)
            out.append(self(tuple(p)) if v is miss else v)
        return out


# ---- LightBank row -> HDR irradiance -------------------------------------------------------------------------------
def light_dir(rx, ry):
    """DrawParam degRotX (elevation) / degRotY (azimuth) -> unit vector towards the light. Sign MEASURED (RenderDoc,
    Firelink): ShadowBank 50/160 gives gFC_ShadowLightDir (0.220, -0.766, -0.604) and LightScattering 70/150 gives
    gFC_LsLightDir (0.171, -0.940, -0.296), both pointing from the light, so towards it is (-x, +y, -z) of the
    plain spherical form."""
    rx, ry = math.radians(rx), math.radians(ry)
    return (-math.cos(rx) * math.sin(ry), math.sin(rx), -math.cos(rx) * math.cos(ry))


def light_fn(row, probes=None):
    """LightBank row -> f(normal, probe faces or None) = linear HDR irradiance (r, g, b) without the specular terms."""
    dirs = []
    for i in "012":
        if row[f"colA_{i}"] > 0:
            dirs.append((light_dir(row[f"degRotX_{i}"], row[f"degRotY_{i}"]),
                         [lin(row[f"col{c}_{i}"], row[f"colA_{i}"]) for c in "RGB"]))
    up = [lin(row[f"col{c}_u"], row["colA_u"]) for c in "RGB"]
    down = [lin(row[f"col{c}_d"], row["colA_d"]) for c in "RGB"]
    env = [lin(row[f"envDif_col{c}"], row["envDif_colA"]) for c in "RGB"]

    def f(n, faces=None, split=False):
        """split: ((hemisphere + probe), directional) apart, for the shadow bake"""
        h = n[1] * 0.5 + 0.5
        e = probe_sample(faces, n) if faces else (0.0, 0.0, 0.0)
        amb = [down[c] + (up[c] - down[c]) * h + env[c] * e[c] for c in range(3)]
        sun = [0.0, 0.0, 0.0]
        for d, col in dirs:
            k = max(0.0, n[0] * d[0] + n[1] * d[1] + n[2] * d[2])
            for c in range(3):
                sun[c] += col[c] * k
        return (amb, sun) if split else [a + b for a, b in zip(amb, sun)]

    def many(N, faces):
        """f(n, faces, split=True) for the rows of (n, 3) unit normals N, per-row probe faces (list, None = none) ->
        (amb, sun) (n, 3) arrays, the same operations per element"""
        import numpy as np
        h = N[:, 1] * 0.5 + 0.5
        e = probe_sample_many(faces, N)
        amb = np.stack([down[c] + (up[c] - down[c]) * h + env[c] * e[:, c] for c in range(3)], 1)
        sun = np.zeros((len(N), 3))
        for d, col in dirs:
            k = N[:, 0] * d[0] + N[:, 1] * d[1] + N[:, 2] * d[2]
            k = np.where(k > 0.0, k, 0.0)
            for c in range(3):
                sun[:, c] += col[c] * k
        return amb, sun
    f.many = many
    return f


def point_light(pos, row):
    """PointLightBank row placed at pos -> f(position, normal) = HDR irradiance (linear falloff mode)."""
    col = [lin(row[f"col{c}"], row["colA"]) for c in "RGB"]
    b, e = row["dwindleBegin"], row["dwindleEnd"]

    def f(p, n):
        v = [pos[k] - p[k] for k in range(3)]
        d = math.sqrt(sum(x * x for x in v)) or 1e-6
        att = max(0.0, min(1.0, (e - d) / max(1e-6, e - b)))
        if att <= 0:
            return (0.0, 0.0, 0.0)
        k = att * max(0.0, sum(n[i] * v[i] for i in range(3)) / d) if n else att
        return [c * k for c in col]
    return f


def draw_banks(game, area):
    """All DrawParam banks of an area: {"Light" / "Fog" / "ToneMap" / "ToneCorrect" / "PointLight" / ...: {row: fields}}."""
    defs = {}
    for _, n, d in souls.bnd3(souls.read(os.path.join(game, "paramdef", "paramdef.paramdefbnd.dcx"))):
        t, f = souls.paramdef(d)
        defs[t] = f
    out = {}
    for _, n, d in souls.bnd3(souls.read(os.path.join(game, "param", "DrawParam", f"a{area[1:3]}_DrawParam.parambnd.dcx"))):
        b = souls.basename(n)
        if not b.startswith(area + "_"):
            continue
        kind = b[len(area) + 1:-len("Bank.param")]
        t = "".join("_" + ch if ch.isupper() and i else ch for i, ch in enumerate(kind)).upper() + "_BANK"
        if t in defs:
            out[kind] = {r: v for r, (_, v) in souls.param(d, defs[t]).items()}
    return out


class ShadowMap:
    """Static sun shadow: an orthographic depth map of the opaque map along the ShadowBank light direction (DSR draws
    a shadow map along gFC_ShadowLightDir), rasterised in C (tools/native/geom.c) or numpy (same depth map);
    lit(p, n) = 0 (shadowed) .. 1 (lit), 3x3 PCF; lit_many for arrays of points."""

    def __init__(self, tris, to_light, focus, texel=0.25, bias=0.25, max_side=4096, batch=2_000_000):
        """tris: (n, 3, 3) array-like of world corners. focus: world points the shadows matter around (navmesh
        vertices); the map covers their light-space box + 60 m and grows its texel to stay <= max_side; triangles are
        clipped to it and (numpy path) rasterised in batches of at most `batch` samples, so memory stays ~0.5 GB
        whatever the area's size (a km-wide background triangle once took an unclipped export to 24 GB)."""
        import numpy as np
        self.np = np
        L = np.array(to_light, np.float64)
        n3 = lambda v: math.sqrt((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2])  # not np.linalg.norm: a BLAS dot natively
        L /= n3(L)
        u = np.cross(L, [0.0, 1.0, 0.0] if abs(L[1]) < 0.99 else [1.0, 0.0, 0.0])
        u /= n3(u)
        v = np.cross(L, u)
        self.B = np.stack([u, v, L])  # rows: u, v, depth towards the light
        F = matmul(np.asarray(focus, np.float64), self.B.T)
        self.lo = F[:, :2].min(0) - 60
        hi = F[:, :2].max(0) + 60
        texel = max(texel, float((hi - self.lo).max()) / max_side)
        self.texel, self.bias = texel, bias
        W, H = (np.ceil((hi - self.lo) / texel)).astype(int) + 1
        self.W, self.H = W, H
        D = np.full(W * H, -1e9, np.float32)
        T = matmul(np.asarray(tris, np.float32).reshape(-1, 3), self.B.T.astype(np.float32)).reshape(-1, 3, 3)
        P = (T[..., :2] - self.lo.astype(np.float32)) / np.float32(texel)  # texel coordinates
        x0 = np.clip(np.floor(P[..., 0].min(1)), 0, W - 1).astype(np.int64)
        y0 = np.clip(np.floor(P[..., 1].min(1)), 0, H - 1).astype(np.int64)
        x1 = np.clip(np.ceil(P[..., 0].max(1)), 0, W - 1).astype(np.int64)
        y1 = np.clip(np.ceil(P[..., 1].max(1)), 0, H - 1).astype(np.int64)
        inside = (P[..., 0].max(1) >= 0) & (P[..., 1].max(1) >= 0) & (P[..., 0].min(1) < W) & (P[..., 1].min(1) < H)
        sw, sh = x1 - x0 + 1, y1 - y0 + 1
        a, b, c = P[:, 0], P[:, 1], P[:, 2]
        den = (b[:, 1] - c[:, 1]) * (a[:, 0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (a[:, 1] - c[:, 1])
        ok = inside & (np.abs(den) > 1e-9)
        size = np.maximum(sw, sh)
        lib = geom_lib()
        if lib is not None:
            # box side: the power of two the numpy batches below give each triangle (size in (s / 2, s])
            side = np.ones_like(size)
            while (side < size).any():
                side = np.where(side < size, side * 2, side)
            side = np.where(ok, side, 0)
            cp = lambda x, t: np.ascontiguousarray(x, t)
            Pc, Zc, dc = cp(P, np.float32), cp(T[..., 2], np.float32), cp(den, np.float32)
            xc, yc, sc = cp(x0, np.int32), cp(y0, np.int32), cp(side, np.int32)
            lib.shadow_raster(len(Pc), Pc.ctypes.data, Zc.ctypes.data, dc.ctypes.data, xc.ctypes.data, yc.ctypes.data,
                              sc.ctypes.data, int(W), int(H), D.ctypes.data)
            self.D = D.reshape(H, W)
            return
        s = 1
        smax = int(size[ok].max(initial=1))
        while True:
            sel = np.where(ok & (size <= s) & ((size > s // 2) if s > 1 else True))[0]
            rows = max(1, min(s, batch // s))  # row strips of the s x s box: big triangles go a strip at a time
            for r0 in range(0, s, rows):
                oy, ox = np.mgrid[r0:min(s, r0 + rows), 0:s]
                ox, oy = ox.ravel(), oy.ravel()
                step = max(1, batch // len(ox))
                for k in range(0, len(sel), step):
                    i = sel[k:k + step]
                    px = (x0[i, None] + ox[None]).astype(np.float32) + 0.5
                    py = (y0[i, None] + oy[None]).astype(np.float32) + 0.5
                    dx, dy = px - c[i, 0][:, None], py - c[i, 1][:, None]
                    w0 = ((b[i, 1] - c[i, 1])[:, None] * dx + (c[i, 0] - b[i, 0])[:, None] * dy) / den[i, None]
                    w1 = ((c[i, 1] - a[i, 1])[:, None] * dx + (a[i, 0] - c[i, 0])[:, None] * dy) / den[i, None]
                    w2 = 1 - w0 - w1
                    m = (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4) & (px < W) & (py < H)
                    del dx, dy
                    z = w0 * T[i, 0, 2][:, None] + w1 * T[i, 1, 2][:, None] + w2 * T[i, 2, 2][:, None]
                    idx = (py.astype(np.int64) * W + px.astype(np.int64))[m]
                    np.maximum.at(D, idx, z[m])
            if s >= smax:
                break
            s *= 2
        self.D = D.reshape(H, W)

    def lit(self, p, n):
        np = self.np
        q = matmul(self.B, np.asarray(p, np.float64) + 0.1 * np.asarray(n, np.float64))
        x, y = (q[:2] - self.lo) / self.texel
        xi, yi = int(x), int(y)
        tot = 0
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                X, Y = xi + dx, yi + dy
                if 0 <= X < self.W and 0 <= Y < self.H and self.D[Y, X] > q[2] + self.bias:
                    tot += 1
        return 1 - tot / 9

    def lit_many(self, P, N):
        """lit() for (n, 3) points and normals at once -> (n,) float64"""
        np = self.np
        P, N = np.asarray(P, np.float64).reshape(-1, 3), np.asarray(N, np.float64).reshape(-1, 3)
        if not len(P):
            return np.zeros(0)
        q = matmul(P + 0.1 * N, self.B.T)
        Q = np.empty_like(q)
        Q[:, :2] = (q[:, :2] - self.lo) / self.texel
        Q[:, 2] = q[:, 2]
        lib = geom_lib()
        if lib is not None:
            Q = np.ascontiguousarray(Q)
            out = np.empty(len(Q))
            D = np.ascontiguousarray(self.D, np.float32)
            lib.shadow_lit(len(Q), Q.ctypes.data, D.ctypes.data, int(self.W), int(self.H), float(self.bias),
                           out.ctypes.data)
            return out
        xi, yi = Q[:, 0].astype(np.int64), Q[:, 1].astype(np.int64)  # int(): towards zero, as lit()
        d = Q[:, 2] + self.bias
        tot = np.zeros(len(Q), np.int64)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                X, Y = xi + dx, yi + dy
                ok = (X >= 0) & (X < self.W) & (Y >= 0) & (Y < self.H)
                v = np.full(len(Q), -np.inf)
                v[ok] = self.D[Y[ok], X[ok]]
                tot += v > d
        return 1 - tot / 9


class Scatter:
    """DSR's light scattering (LightScatteringBank), applied by the map shaders after the fog in gamma space
    (FRPG_Phn_*.fpo, decompiled from a RenderDoc capture at Firelink; constants measured there):
      ext = exp2(-d x beta x distanceMul x 2.0814)            d = camera distance (m)
      mie = (1 - g^2) x (1 + g - 2g cos)^-1.5                   cos = view direction . gFC_LsLightDir
      ins = (betaDash1 (1 + cos^2) + mie betaDash2) / beta x (1 - ext) x reflectance.a x sun
      out = lerp(c, c x ext x reflectance + ins, blendCoef)
    Row -> constants (MEASURED, RenderDoc at Firelink and two Undead Burg spots): sun = sunRGB / 255 x sunA %,
    reflectance = reflectanceRGB / 255 (.a = reflectanceA %), blend = blendCoef %, distanceMul %, g = lsHGg, and
    beta = lsBetaRay x RAY + lsBetaMie x MIE, betaDash1 = lsBetaRay x DASH1, betaDash2 = lsBetaMie x DASH2 per
    channel: solved from Firelink row 20 (0.1 / 0.6) and Burg row 10 (7 / 0.7), it predicts Burg row 11 (10 / 1)
    to 6 digits."""
    RAY = (0.00069709, 0.00117884, 0.00244458)
    MIE = (0.00574048, 0.00740019, 0.01051424)
    DASH1 = (4.15493e-05, 7.04225e-05, 1.459155e-04)
    DASH2 = (0.00133385, 0.00173462, 0.00249769)

    def __init__(self, row, area=None, row_id=None):
        self.sun = [row[f"sun{c}"] / 255 * row["sunA"] / 100 for c in "RGB"]
        self.refl = [row[f"reflectance{c}"] / 255 for c in "RGB"]
        self.refl_a = row["reflectanceA"] / 100
        self.blend = row["blendCoef"] / 100
        self.dmul = row["distanceMul"] / 100
        self.g = row["lsHGg"]
        ray, mie = row["lsBetaRay"], row["lsBetaMie"]
        self.beta = [ray * r + mie * m for r, m in zip(self.RAY, self.MIE)]
        self.dash1 = [ray * x for x in self.DASH1]
        self.dash2 = [mie * x for x in self.DASH2]
        self.to_sun = light_dir(row["sunRotX"], row["sunRotY"])

    def gamma(self, c, d=0.0, cos=0.0):
        """Gamma-space colour c (3) seen at distance d with view . light-direction cos -> scattered colour"""
        g = self.g
        mie = (1 - g * g) * max(1e-3, 1 + g - 2 * g * cos) ** -1.5
        out = []
        for i in range(3):
            if d <= 0:
                ext, ins = 1.0, 0.0
            else:
                ext = 2 ** (-d * self.beta[i] * self.dmul * 2.0814)
                ins = (self.dash1[i] * (1 + cos * cos) + mie * self.dash2[i]) / self.beta[i] * (1 - ext) * self.refl_a * self.sun[i]
            out.append(c[i] + (c[i] * ext * self.refl[i] + ins - c[i]) * self.blend)
        return out

    def near_factor(self):
        """Linear-light multiplier of the d = 0 term (lerp(1, reflectance, blend) in gamma space), for the bake"""
        return [(1 - self.blend + self.blend * r) ** 2.2 for r in self.refl]


# ---- point lights and the map bake's tone map (export_map.py + export_effects.py) ----
def cubic_light(pos, begin, end, rgb):
    """DSR point light (FRPG_Phn_* loop): rgb x saturate(((end - d) / (end - begin))^3) x N.L -> f(p, n)."""
    k = 1.0 / max(1e-6, end - begin)

    def f(p, n):
        v = [pos[i] - p[i] for i in range(3)]
        d = math.sqrt(sum(x * x for x in v)) or 1e-6
        if d >= end:
            return (0.0, 0.0, 0.0)
        att = min(1.0, (k * (end - d)) ** 3)
        nl = max(0.0, sum(n[i] * v[i] for i in range(3)) / d) if n else 1.0
        return [c * att * nl for c in rgb]

    def many(P, N):
        """f per row of (n, 3) positions and normals -> (n, 3), the same operations per element"""
        import numpy as np
        V = np.asarray(pos, np.float64) - P
        d = np.sqrt(sum3(V[:, 0] * V[:, 0], V[:, 1] * V[:, 1], V[:, 2] * V[:, 2]))
        d[d == 0] = 1e-6
        out = np.zeros((len(P), 3))
        sel = np.nonzero(~(d >= end))[0]
        if len(sel):
            ds = d[sel]
            att = np.array([x ** 3 for x in (k * (end - ds)).tolist()])
            att = np.where(att < 1.0, att, 1.0)
            Ns, Vs = N[sel], V[sel]
            nl = sum3(Ns[:, 0] * Vs[:, 0], Ns[:, 1] * Vs[:, 1], Ns[:, 2] * Vs[:, 2]) / ds
            nl = np.where(nl > 0.0, nl, 0.0)
            out[sel] = np.stack([c * att * nl for c in rgb], 1)
        return out
    f.many = many
    return f


def write_tone(path, tone):
    """export_map.py hook: the lit map's adapted tone map for the effect colours."""
    if tone is None:
        return
    json.dump({"key": tone.key, "lo": tone.lo, "hi": tone.hi, "exposure": tone.exposure, "tc": tone.tc},
              open(path, "w"))


def load_tone(path):
    if path and os.path.exists(path):
        j = json.load(open(path))
        t = Tone({"grayKeyValue": j["key"], "minAdaptedLum": j["lo"], "maxAdapredLum": j["hi"]}, j["tc"],
                           j["exposure"])
        return t
    print("export_effects: no fx_tone.json, effect colours use the bright-scene exposure (re-export map.bin)")
    return None
