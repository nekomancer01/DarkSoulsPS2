"""Normal-map relief for the PS2 map (CLUT dot3, TECH_DEBT map-23): bump.bin, a pack entry next to map.bin.

DSR lights map pieces per pixel with a tangent-space normal map (g_Bumpmap, BC5). FRPG_Phn_*Bmp*.fpo (RenderDoc,
Firelink) decodes x = 2r - 1, y = 2g - 1, z = sqrt(1 - x^2 - y^2) at strength p = gFC_LightProbeParam.z (1.0 on all
157 draws at spot A): n = normalize(N (p (z - 1) + 1) + B p x + T p y), T = the FLVER tangent (vertex semantic 6),
B = w (N x T) with w = the tangent's .w (FRPG_Phn_PINT_*.vpo). The GS has no per-pixel lighting, so the export does:
- textures: every bump texture in use -> an 8-bit index map into one area-wide palette of normals (index 0 = flat,
  1..255 = k-means over the texels in use). Mip 1 averages x, y (as the BC5 mips the GPU filters) and maps to the
  same palette, so distant relief flattens like DSR's.
- clusters: bumped triangles (MTD letter block with B: M[DB], M[DB][M]; lit, opaque, not cut-out / foliage) get a
  lighting response: DSR's light (the part's LightBank row: directional + hemisphere + GI_EnvDif probe, as
  dsr_light.light_fn) x the baked sun shadow, through the area's tone map, at RING tangent-space normals. An optional
  coarse k-means (BUMP_SPLIT groups, default off) keeps very different triangles out of one chunk (build_chunks); the
  chunks, by their mean response, go into BUMP_K clusters (one CLUT per chunk, so the chunk count does not grow).
- CLUTs: per cluster a CT32 CLUT, alpha = 0x80 x DSR's output for the palette normal / its output for the flat normal
  (what the vertex colour already holds), averaged over a sample of the cluster's triangles; rgb unused. Level 1 is the
  half-strength version (ratio^0.5) for the far band.
ps2/proto/src/bump.c draws the bumped chunks a second time with their index map and cluster CLUT: ALPHA Cd x As,
depth test GEQUAL on the depth the opaque pass wrote, no Z write, no fog.

bump.bin (little-endian, 16-byte aligned sections, pixels 128-byte aligned):
  header  u32 'DSB1', chunks (= map.bin's chunk count, 0 = no relief), textures, clusters, levels; u32 offsets:
          chunk table, texture table, CLUTs; u32 cap (relief batches per frame, 0 = none); pad to 48 bytes
  chunk table  per map.bin chunk: u16 texture (0xFFFF = not bumped), u8 cluster, u8 relief strength = std of the
               level 0 CLUT alpha over the texture's mip 0 texels (0x80 = x1; bump.c skips chunks below one 5-bit
               frame buffer step)
  textures     tools/texmap.py entries (32 B: psm PSMT8, clut psm CT32, clut offset 0 = none; pixel offsets from the
               start of bump.bin)
  CLUTs        [cluster][level] 16x16 CT32, CSM1-ordered (1 KB each)
  pixels       per texture mip 0, mip 1 (8-bit, linear rows)
"""
import math, os, re, struct

import numpy as np

import dsr_light
import souls
import texmap

BUMP = os.environ.get("BUMP", "1") == "1"
BUMP_MAX = int(os.environ.get("BUMP_MAX", 128))  # px, largest normal-map side (VRAM pool / RAM)
BUMP_K = int(os.environ.get("BUMP_K", 48))       # lighting clusters = CLUTs, one per chunk
# coarse per-triangle response groups that build_chunks keeps apart (a wall's sunlit and shadowed sides). Measured at
# Firelink (CLUT error = mean |triangle - CLUT| at the palette normals, map pass at spot A, PCSX2): per-triangle
# clusters (K 48) 11.2k chunks of 9 tris; 6 groups 5217 chunks, error 25 / 128, +1.85 ms; 1 (none) 5032 chunks
# (no relief: 5023), error 29 / 128, +0.79 ms. Default 1: the split cost more than the relief.
BUMP_SPLIT = int(os.environ.get("BUMP_SPLIT", 1))
# relief batches per frame, nearest first (0 = no cap): a per-area cost cap, set from the Makefile's MAPENV_<area>
# (Undead Burg: 32 keeps the southwest spot at 55 fps; uncapped 49, relief off 59)
BUMP_CAP = int(os.environ.get("BUMP_CAP", 0))
LEVELS = 2
STRENGTH = 1.0  # gFC_LightProbeParam.z, MEASURED (RenderDoc, Firelink spot A, every bump draw)
# tangent-space normals the clustering compares responses at: flat, then rings at 25 and 50 degrees
RING = [(0.0, 0.0)] + [(math.sin(t) * math.cos(a), math.sin(t) * math.sin(a))
                       for t in (math.radians(25), math.radians(50)) for a in np.arange(8) * math.pi / 4]
# output-space floor of the ratio: one step of the 5-bit frame buffer, so near-black texels (deep shadow, where the flat
# output is ~0) do not turn tiny absolute differences into huge ratios
EPS = 1 / 32
# split groups smaller than this (triangles in one texture + cell) join the largest bumped group of that texture + cell
# instead of becoming a partial chunk of their own
BUMP_MIN_GROUP = int(os.environ.get("BUMP_MIN_GROUP", 18))
CLUT_SAMPLE = 64  # triangles per cluster the CLUT is averaged over

MESH = []         # per registered mesh: dict(tex, light, alb, N, T, W, probe_of)
SRC = {}          # gather(): dds, probes, near (scattering d = 0 factor), smap (sun shadow map)
CHUNK_BUMP = []   # per build_chunks output chunk: (split key, [triangle numbers]) or None (filled by build_chunks)
_STATE = {}       # assign() -> write(): textures, palette, CLUTs, densities


def bumped(mtd):
    """MTDs whose shader reads g_Bumpmap: a B in the letter block ([DB], [DSB], [DB][M]); cut-outs (_Edge), blends
    (_Alp / _Add) and foliage / trees are left out (a second pass over cut-out texels would darken what is behind)."""
    low = mtd.lower()
    return bool(re.search(r"\[[a-z]*b[a-z]*\]", low)) and not any(k in low for k in ("_edge", "_alp", "_add",
                                                                                    "foliage", "ivy", "tree"))


def register(m, V, fvw, wn, xn, light_row, alb, probe_of, wn_np=None):
    """gather() hook for one lit map mesh: keeps what the relief needs per wedge (normal, tangent, handedness) and
    returns an index for the mesh dict's "bump" key, or None when the mesh has no relief. wn_np: (wn, np.array(wn))
    when gather has that array."""
    if not BUMP or not m.get("bump") or not bumped(m["mtd"]) or len(V.get("tangent") or ()) != len(V["pos"]):
        return None
    nw = len(wn)
    T, W = np.zeros((nw, 3)), np.zeros(nw)
    if hasattr(xn, "many") and fvw:  # all vertices at once, summed per wedge in the same order (np.add.at)
        vs = np.fromiter(fvw.keys(), np.int64, len(fvw))
        ws = np.fromiter(fvw.values(), np.int64, len(fvw))
        Tv = V["tangent"].a[vs].astype(np.float64) if hasattr(V["tangent"], "a") else \
            np.array([V["tangent"][vi] for vi in vs.tolist()], np.float64).reshape(-1, 4)  # souls.Rows: its array
        np.add.at(T, ws, xn.many(np.ascontiguousarray(Tv[:, :3])))
        np.add.at(W, ws, Tv[:, 3])
    else:
        for vi, w in fvw.items():
            t = V["tangent"][vi]
            T[w] += xn(t[:3])
            W[w] += t[3]
    N = wn_np[1] if wn_np is not None and wn_np[0] is wn else np.array(wn, np.float64)
    MESH.append(dict(tex=m["bump"], light=light_row, alb=alb, N=N, T=T, W=np.where(W < 0, -1.0, 1.0),
                     probe_of=probe_of))
    return len(MESH) - 1


# ---- DSR's light, vectorised (mirrors dsr_light.light_fn / probe_sample; checked against them in assign) ----
def _light_terms(row):
    dirs = [(np.array(dsr_light.light_dir(row[f"degRotX_{i}"], row[f"degRotY_{i}"])),
             np.array([dsr_light.lin(row[f"col{c}_{i}"], row[f"colA_{i}"]) for c in "RGB"]))
            for i in "012" if row[f"colA_{i}"] > 0]
    up = np.array([dsr_light.lin(row[f"col{c}_u"], row["colA_u"]) for c in "RGB"])
    down = np.array([dsr_light.lin(row[f"col{c}_d"], row["colA_d"]) for c in "RGB"])
    env = np.array([dsr_light.lin(row[f"envDif_col{c}"], row["envDif_colA"]) for c in "RGB"])
    return dirs, up, down, env


def _probe(faces, n):
    R = dsr_light.PROBE_RES
    ax = np.argmax(np.abs(n), 1)
    rows = np.arange(len(n))
    m = np.abs(n[rows, ax])
    m = np.where(m > 0, m, 1.0)
    f = np.where(n[rows, ax] > 0, 0, 1) + 2 * ax
    out = np.zeros((len(n), 3))
    for fi, (_, _, (sa, ss), (ta, ts)) in enumerate(dsr_light._FACE):
        k = f == fi
        if not k.any():
            continue
        u = np.minimum(R - 1, ((ss * n[k, sa] / m[k] + 1) / 2 * R).astype(int))
        v = np.minimum(R - 1, ((ts * n[k, ta] / m[k] + 1) / 2 * R).astype(int))
        out[k] = faces[fi, v, u]
    return out


def _irradiance(terms, faces, n):
    """n (M, 3) unit -> (ambient, directional) linear HDR rgb (M, 3) each"""
    dirs, up, down, env = terms
    h = n[:, 1:2] * 0.5 + 0.5
    e = _probe(faces, n) if faces is not None else 0.0
    amb = down + (up - down) * h + env * e
    sun = np.zeros_like(amb)
    for d, col in dirs:
        sun += np.maximum(0.0, dsr_light.matmul(n, d))[:, None] * col
    return amb, sun


def _frames(N, T, W):
    """per-row DSR tangent frame: unit N, T, B = w (N x T)"""
    N = N / (np.linalg.norm(N, axis=1, keepdims=True) + 1e-12)
    T = T / (np.linalg.norm(T, axis=1, keepdims=True) + 1e-12)
    B = W[:, None] * np.cross(N, T)
    return N, T, B


def _perturb(N, T, B, xy):
    """tangent-space normals xy (J, 2) on every frame row -> world normals (rows, J, 3), DSR's formula"""
    x, y = STRENGTH * xy[:, 0], STRENGTH * xy[:, 1]
    z = STRENGTH * (np.sqrt(np.maximum(0.0, 1 - xy[:, 0] ** 2 - xy[:, 1] ** 2)) - 1) + 1
    n = N[:, None, :] * z[None, :, None] + B[:, None, :] * x[None, :, None] + T[:, None, :] * y[None, :, None]
    return n / (np.linalg.norm(n, axis=2, keepdims=True) + 1e-12)


class _Response:
    """DSR output (tone mapped, gamma) / output for the flat normal, per triangle at given tangent-space normals."""

    def __init__(self, tone, near, shadow_amb):
        self.near, self.sa = np.array(near), shadow_amb
        y = np.logspace(-6, 3, 1024)
        self.ly = np.log(y)
        self.out = np.array([sum(k * v for k, v in zip(dsr_light.LUMA, tone([v, v, v]))) for v in y])
        self.terms, self.faces = {}, {}

    def tone(self, Y):
        return np.interp(_log(np.maximum(Y, 1e-6)), self.ly, self.out)

    def __call__(self, tri, xy, rows=32768):
        """tri: dict of arrays (N, T, B, k, alb (rgb linear), light id, probe id, rows / probes lookups) -> (n, J).
        In blocks of rows triangles: every value is per triangle, so the blocks give the same bits as one pass, and
        the (triangles, J, 3) normals and their temporaries stay small (one pass over Undead Burg's triangles took
        the in-browser converter's heap from 1.2 to 2 GB)"""
        nt = len(tri["N"])
        if nt > rows:
            return np.concatenate([self({k: v[i:i + rows] if isinstance(v, np.ndarray) else v for k, v in tri.items()},
                                        xy, rows) for i in range(0, nt, rows)])
        n = _perturb(tri["N"], tri["T"], tri["B"], np.vstack([[0.0, 0.0], xy]))  # column 0: flat
        J = n.shape[1]
        Y = np.zeros((nt, J))
        for key in set(zip(tri["lid"], tri["pid"])):
            sel = np.nonzero((tri["lid"] == key[0]) & (tri["pid"] == key[1]))[0]
            terms = self.terms.setdefault(key[0], _light_terms(tri["rows"][key[0]]))
            faces = tri["probes"].get(key[1])
            amb, sun = _irradiance(terms, faces, n[sel].reshape(-1, 3))
            k = np.repeat(tri["k"][sel], J)[:, None]
            e = amb * (1 - self.sa * (1 - k)) + sun * k
            alb = np.repeat(tri["alb"][sel], J, axis=0)
            Y[sel] = dsr_light.matmul(e * alb * self.near, np.array(dsr_light.LUMA)).reshape(len(sel), J)
        o = self.tone(Y) + EPS
        return np.clip(o[:, 1:] / o[:, :1], 0, 255 / 128)


_log = np.log  # tools/mapx/test_mapx.py swaps in libm's log (numpy's AVX-512 log differs by an ulp; libmapx calls libm)


# ---- libmapx (tools/mapx/bump.c, kmeans.c, rng.c): the same values as the numpy code below ----
def _mx():
    """libmapx with its export_normal entry points, or None (MAPX=0, no compiler, or a numpy whose x.dot(x) is
    neither a fused BLAS ddot nor a plain loop: then the numpy code runs)"""
    if "mx" not in _MX:
        import ctypes
        import mapx_native
        L = mapx_native.lib()
        if L is not None:
            vp, i, ll, d = ctypes.c_void_p, ctypes.c_int, ctypes.c_longlong, ctypes.c_double
            L.mx_dot3.argtypes = [vp, i]
            L.mx_dot3.restype = d
            L.mx_bump_frames.argtypes = [ll, vp, vp, vp, vp, i, vp, vp, vp]
            L.mx_bump_frames.restype = None
            L.mx_bump_response.argtypes = [ll, vp, vp, vp, vp, vp, vp, vp, vp, i, vp, vp, i, vp, vp, d, i, vp, vp, d, vp]
            L.mx_bump_response.restype = None
            L.mx_bc5_pow2.argtypes = [vp, ll, i, i, i, vp, vp, vp]
            L.mx_bc5_pow2.restype = i
            L.mx_kmeans.argtypes = [i, vp, ll, i, vp, i, i, vp, vp, vp]
            L.mx_kmeans.restype = i
            L.mx_nearest.argtypes = [i, vp, ll, i, i, vp, i, vp]
            L.mx_nearest.restype = None
            u64 = ctypes.c_uint64
            L.mx_rng_new.argtypes = [u64, u64, u64, u64, i, ctypes.c_uint32]
            L.mx_rng_new.restype = vp
            L.mx_rng_free.argtypes = [vp]
            L.mx_rng_free.restype = None
            V = np.random.default_rng(5).normal(size=(64, 3)) * 10
            ref = [float(v.dot(v)) for v in V]
            for f in (1, 0):
                if ref == [L.mx_dot3(np.ascontiguousarray(v).ctypes.data, f) for v in V]:
                    _MX["fma"] = f
                    break
            else:
                print("export_normal: numpy's dot is neither fused nor plain, relief in numpy", flush=True)
                L = None
        _MX["mx"] = L
    return _MX["mx"]


_MX = {}
_CHECK = os.environ.get("MAPX_CHECK") == "1"  # debugging: every C stage of the relief also run in numpy, compared


def _chk(what, a, b):
    """MAPX_CHECK: print whether the C result a equals the numpy result b (bit for bit), and where it first differs"""
    if isinstance(a, tuple):
        for i, (x, y) in enumerate(zip(a, b)):
            _chk(f"{what}[{i}]", x, y)
        return
    a, b = np.asarray(a), np.asarray(b)
    if a.shape != b.shape:
        print(f"mapx check: {what} DIFF shape {a.shape} vs {b.shape}", flush=True)
        return
    ne = (a != b) & ~((a != a) & (b != b))
    if not ne.any():
        print(f"mapx check: {what} same {a.shape} {a.dtype}/{b.dtype}", flush=True)
        return
    ix = np.argwhere(ne)
    i0 = tuple(ix[0].tolist())
    ad = np.abs(a.astype(np.float64) - b.astype(np.float64))[ne]
    print(f"mapx check: {what} DIFF {len(ix)} of {a.size} {a.dtype}/{b.dtype}, max |d| {ad.max():.3g}, first at {i0}: "
          f"{a[i0]!r} vs {b[i0]!r}", flush=True)


def _mx_rng(L, seed):
    st = np.random.default_rng(seed).bit_generator.state
    s, i, M = st["state"]["state"], st["state"]["inc"], (1 << 64) - 1
    return L.mx_rng_new(s >> 64, s & M, i >> 64, i & M, st["has_uint32"], st["uinteger"])


def _mx_kmeans(L, x, k, wts=None, iters=20, seed=1):
    """_kmeans in C (the same centres and labels)"""
    x = np.ascontiguousarray(x)
    assert x.dtype in (np.float32, np.float64) and x.ndim == 2
    w = None if wts is None else np.ascontiguousarray(wts, np.float64)
    c = np.empty((min(k, len(x)), x.shape[1]), x.dtype)
    lab = np.empty(len(x), np.int64)
    r = _mx_rng(L, seed)
    L.mx_kmeans(x.dtype.itemsize, x.ctypes.data, len(x), x.shape[1], None if w is None else w.ctypes.data, k, iters, r,
                c.ctypes.data, lab.ctypes.data)
    L.mx_rng_free(r)
    return c, lab


def _mx_nearest(L, x, c):
    x, c = np.ascontiguousarray(x), np.ascontiguousarray(c)
    out = np.empty(len(x), np.int64)
    L.mx_nearest(x.dtype.itemsize, x.ctypes.data, len(x), x.shape[1], c.dtype.itemsize, c.ctypes.data, len(c),
                 out.ctypes.data)
    return out


def _mx_response(L, resp, tri, xy, rows=None):
    """_Response.__call__(tri, xy) in C; rows: the triangles to evaluate (default all)"""
    if "mx" not in tri:  # light rows and probe cubes as flat tables, once per tri dict
        nl = max(tri["lid"].max(initial=0) + 1, 1)
        tab = np.zeros((nl, 31))
        for l in range(nl):
            if l not in tri["rows"]:
                continue
            dirs, up, down, env = resp.terms.setdefault(l, _light_terms(tri["rows"][l]))
            tab[l, 0] = len(dirs)
            for j, (d, col) in enumerate(dirs):
                tab[l, 1 + 3 * j:4 + 3 * j] = d
                tab[l, 10 + 3 * j:13 + 3 * j] = col
            tab[l, 19:22], tab[l, 22:25], tab[l, 25:28] = up, down, env
        slots = sorted(tri["probes"])
        faces = np.ascontiguousarray(np.array([tri["probes"][p] for p in slots], np.float64).reshape(-1)
                                     if slots else np.zeros(1))
        sl = {p: i for i, p in enumerate(slots)}
        pslot = np.array([sl.get(p, -1) for p in tri["pid"].tolist()], np.int32)
        cp = lambda a, t=np.float64: np.ascontiguousarray(a, t)
        tri["mx"] = dict(tab=cp(tab), faces=faces, pslot=pslot, N=cp(tri["N"]), T=cp(tri["T"]), B=cp(tri["B"]),
                         k=cp(tri["k"]), alb=cp(tri["alb"]), lid=cp(tri["lid"], np.int32), nl=nl,
                         ly=cp(resp.ly), out=cp(resp.out), near=cp(resp.near))
    m = tri["mx"]
    xy = np.ascontiguousarray(xy, np.float64)
    n = len(m["N"]) if rows is None else len(rows)
    rw = None if rows is None else np.ascontiguousarray(rows, np.int64)
    R = np.empty((n, len(xy)))
    L.mx_bump_response(n, None if rw is None else rw.ctypes.data, m["N"].ctypes.data, m["T"].ctypes.data,
                       m["B"].ctypes.data, m["k"].ctypes.data, m["alb"].ctypes.data, m["lid"].ctypes.data,
                       m["pslot"].ctypes.data, m["nl"], m["tab"].ctypes.data, m["faces"].ctypes.data, len(xy),
                       xy.ctypes.data, m["near"].ctypes.data, float(resp.sa), len(m["ly"]), m["ly"].ctypes.data,
                       m["out"].ctypes.data, EPS, R.ctypes.data)
    return R


def _mx_xy_pow2(L, d, max_dim, cap):
    """xy_pow2 in C for BC5 maps (float32, as numpy's BC5 path); other formats: None (numpy decodes them)"""
    import ctypes
    lv = _bc5_level(d, max_dim)
    if not lv:
        return None
    off, w, h = lv
    out = np.empty(cap * cap * 2, np.float32)
    th, tw = ctypes.c_int(), ctypes.c_int()
    buf = np.frombuffer(d, np.uint8)
    L.mx_bc5_pow2(buf.ctypes.data, off, w, h, cap, out.ctypes.data, ctypes.byref(th), ctypes.byref(tw))
    return out[:th.value * tw.value * 2].reshape(th.value, tw.value, 2).copy()


# ---- normal maps ----
_BC4_PAL = []  # (65536, 8) float32: the palette of every (a0, a1) pair, built once with _bc4_pal


def _bc4_pal(a0, a1):
    a0, a1 = a0.astype(np.float32), a1.astype(np.float32)
    pal = np.zeros((len(a0), 8), np.float32)
    pal[:, 0], pal[:, 1] = a0, a1
    big = a0 > a1
    for i in range(1, 7):
        pal[:, 1 + i] = np.where(big, ((7 - i) * a0 + i * a1) / 7, ((5 - i) * a0 + i * a1) / 5 if i < 5 else 0)
    pal[~big, 6], pal[~big, 7] = 0, 255
    return pal


def _bc4(blk):
    """(n, 8) BC4 blocks -> (n, 16) values 0..1"""
    if not _BC4_PAL:
        ab = np.arange(65536)
        _BC4_PAL.append(_bc4_pal(ab >> 8, ab & 255))
    pal = _BC4_PAL[0][blk[:, 0].astype(np.int64) << 8 | blk[:, 1]]
    # 16 3-bit indices, little-endian over bytes 2..7
    b = np.unpackbits(np.ascontiguousarray(blk[:, 2:8]), axis=1, bitorder="little").reshape(-1, 16, 3)
    idx = b[:, :, 0] | b[:, :, 1] << 1 | b[:, :, 2] << 2
    return np.take_along_axis(pal, idx.astype(np.intp), 1) / 255.0


def _bc5_level(d, max_dim):
    """BC5 normal-map DDS -> (data offset, w, h) of its largest mip <= max_dim, or None for other formats"""
    h, w, mips = struct.unpack_from("<iii", d, 12)
    mips = max(1, mips)
    if d[84:88] != b"DX10" or struct.unpack_from("<I", d, 128)[0] not in (83, 84):  # BC5_UNORM / SNORM (DSR: 83)
        return None
    off = 148
    while mips > 1 and max(w, h) > max_dim:
        off += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16
        w, h, mips = max(1, w // 2), max(1, h // 2), mips - 1
    return off, w, h


def _bc5_rows(d, off, w, by0, by1):
    """block rows by0..by1 of a BC5 level -> (4 (by1 - by0), 4 bw, 2) x, y in 0..1"""
    bw = max(1, (w + 3) // 4)
    blk = np.frombuffer(d, np.uint8, bw * (by1 - by0) * 16, off + by0 * bw * 16).reshape(-1, 16)
    ch = [_bc4(blk[:, :8]), _bc4(blk[:, 8:])]
    nb = by1 - by0
    return np.stack([c.reshape(nb, bw, 4, 4).transpose(0, 2, 1, 3).reshape(nb * 4, bw * 4) for c in ch], 2)


def decode_xy(d, max_dim):
    """normal-map DDS (BC5, or DXT1 / BC7 via souls) -> (h, w, 2) tangent x, y in -1..1, largest mip <= max_dim"""
    lv = _bc5_level(d, max_dim)
    if lv:
        off, w, h = lv
        img = _bc5_rows(d, off, w, 0, max(1, (h + 3) // 4))[:h, :w]
    else:
        dec = souls.dds_decode_rgba(d, max_dim)
        if not dec:
            return None
        w, h = dec[0], dec[1]
        img = np.frombuffer(dec[2], np.uint8).reshape(h, w, 4)[:, :, :2] / 255.0
    return img * 2 - 1


def _pow2_index(h, w, cap):
    """_pow2's power-of-two size and its source rows / columns per texel"""
    tw, th = 1, 1
    while tw * 2 <= min(w, cap):
        tw *= 2
    while th * 2 <= min(h, cap):
        th *= 2
    ys = (np.arange(th)[:, None] * h // th + np.arange(max(1, h // th))[None, :]).clip(0, h - 1)
    xs = (np.arange(tw)[:, None] * w // tw + np.arange(max(1, w // tw))[None, :]).clip(0, w - 1)
    return ys, xs


def xy_pow2(d, max_dim, cap, band=8):
    """_pow2(decode_xy(d, max_dim), cap), for BC5 band output rows at a time from just the block rows they read: the
    same values (the decode is per block, the average per texel), without the whole level as float64 (Undead Burg's
    4096 px m10_cliff_01_n took the in-browser converter's heap from 1.3 to 1.7 GB)"""
    lv = _bc5_level(d, max_dim)
    if not lv:
        xy = decode_xy(d, max_dim)
        return None if xy is None else _pow2(xy, cap)
    off, w, h = lv
    ys, xs = _pow2_index(h, w, cap)
    out = []
    for i in range(0, len(ys), band):
        y = ys[i:i + band]
        by0, by1 = int(y.min()) // 4, int(y.max()) // 4 + 1
        img = _bc5_rows(d, off, w, by0, by1)[:, :w] * 2 - 1
        out.append(img[y - 4 * by0][:, :, xs].mean(axis=(1, 3)))
    return np.concatenate(out)


def _pow2(img, cap):
    """resample to power-of-two sides <= cap (box average down, nearest up)"""
    ys, xs = _pow2_index(*img.shape[:2], cap)
    return img[ys][:, :, xs].mean(axis=(1, 3))


def _kmeans(x, k, wts=None, iters=20, seed=1):
    """weighted Lloyd k-means, k-means++ start; returns (centres, labels)"""
    rng = np.random.default_rng(seed)
    wts = np.ones(len(x)) if wts is None else wts
    k = min(k, len(x))
    c = [x[rng.choice(len(x), p=wts / wts.sum())]]
    d2 = ((x - c[0]) ** 2).sum(1)
    for _ in range(1, k):
        p = d2 * wts
        c.append(x[rng.choice(len(x), p=p / p.sum())] if p.sum() > 0 else x[rng.integers(len(x))])
        d2 = np.minimum(d2, ((x - c[-1]) ** 2).sum(1))
    c = np.array(c)
    for _ in range(iters):
        lab = _nearest(x, c)
        # members of each cluster in index order (a stable sort by label: the same rows as the mask lab == j, without
        # k passes over x)
        order = np.argsort(lab, kind="stable")
        bounds = np.searchsorted(lab[order], np.arange(k + 1)).tolist()
        for j in range(k):
            if bounds[j] < bounds[j + 1]:
                s = order[bounds[j]:bounds[j + 1]]
                c[j] = (x[s] * wts[s, None]).sum(0) / wts[s].sum()
    return c, _nearest(x, c)


def _nearest(x, c, step=4096):
    """index of the nearest centre per row of x, step rows at a time (each row on its own: any step gives the same
    labels; 65536 rows x 255 palette entries made dsr_light.matmul's temporaries 0.5 GB of the browser's heap)"""
    out = np.empty(len(x), int)
    cc = (c ** 2).sum(1)
    for i in range(0, len(x), step):
        d = dsr_light.matmul(2 * x[i:i + step], c.T)
        np.subtract(cc, d, out=d)  # cc - 2 x.c in place: the same values without a second (step, k) array
        out[i:i + step] = d.argmin(1)
    return out


# ---- export ----
def merge_small(groups):
    """build_chunks hook: groups {(texture, flags, cell, split key or None): [(fv, k)]} -> the same with bumped groups
    under BUMP_MIN_GROUP triangles merged into the largest bumped group of the same (texture, flags, cell)."""
    best = {}
    for key, ks in groups.items():
        if key[3] is not None and len(ks) > len(groups.get(best.get(key[:3]), ())):
            best[key[:3]] = key
    out = {}
    for key, ks in groups.items():
        dst = best[key[:3]] if key[3] is not None and len(ks) < BUMP_MIN_GROUP else key
        out.setdefault(dst, []).extend(ks)
    return out



def assign(meshes, dds, smap, tone, near, shadow_amb, probes):
    """After simplification (mesh "tris" = corners (position, wedge)): cluster the bumped triangles and build the
    textures and CLUTs. Returns {triangle number in main()'s order: (texture name, cluster)} for build_chunks."""
    import mapx_native
    _STATE.clear()
    segs = []  # (global number of the first triangle, mesh entry, triangles (corners)), per bumped mesh in order
    k = 0
    for m in meshes:
        tl = m["tris"]
        if not tl:
            continue
        e = MESH[m["bump"]] if m.get("bump") is not None else None
        if e is not None and e["tex"] in dds:
            segs.append((k, e, tl))
        k += len(tl)
    if not segs:
        print("bump: no bumped triangles")
        return {}
    sl = [len(tl) for _, _, tl in segs]
    # per triangle: DSR frame (mean of its corners), sun visibility (baked shadow at the corners), light row, probe
    nt = sum(sl)
    N, T, W = np.zeros((nt, 3)), np.zeros((nt, 3)), np.zeros(nt)
    ks, area, lid, pid, alb = np.ones(nt), np.zeros(nt), np.zeros(nt, int), np.zeros(nt, int), np.zeros((nt, 3))
    rows, rid, faces = {}, {}, {}
    # vectorised per mesh entry (was one numpy call chain per triangle: 68 s of Firelink's export); every element is
    # computed with the same operations in the same order as before (row sums ((a + b) + c), np.cross, the vector norm
    # per row), so the clusters and textures are unchanged
    arr = [mapx_native.tri_arrays(tl) for _, _, tl in segs]
    WS = np.concatenate([w for _, w in arr]).astype(np.int64)
    PS = np.concatenate([p for p, _ in arr])
    del arr
    ent = {}
    eix = np.repeat(np.array([ent.setdefault(id(e), (len(ent), e))[0] for _, e, _ in segs], np.int64), sl)
    for k, e in ent.values():
        sel = np.nonzero(eix == k)[0]
        w = WS[sel]
        N[sel] = e["N"][w[:, 0]] + e["N"][w[:, 1]] + e["N"][w[:, 2]]
        T[sel] = e["T"][w[:, 0]] + e["T"][w[:, 1]] + e["T"][w[:, 2]]
        W[sel] = e["W"][w[:, 0]] + e["W"][w[:, 1]] + e["W"][w[:, 2]]
        lid[sel] = rid.setdefault(id(e["light"]), len(rid))
        rows[lid[sel[0]]] = e["light"]
        alb[sel] = [dsr_light.srgb_lin(a) for a in e["alb"]]
    MX = _mx()
    if MX is not None:  # area, Nn and (below) _frames in C (tools/mapx/bump.c)
        N0, T0 = (N.copy(), T.copy()) if _CHECK else (N, T)  # mx_bump_frames overwrites N and T
        PSc, Nc, Tc, Wc = (np.ascontiguousarray(a, np.float64) for a in (PS, N, T, W))
        Nn, Bc = np.empty((nt, 3)), np.empty((nt, 3))
        MX.mx_bump_frames(nt, PSc.ctypes.data, Nc.ctypes.data, Tc.ctypes.data, Wc.ctypes.data, _MX["fma"],
                          area.ctypes.data, Nn.ctypes.data, Bc.ctypes.data)
        if _CHECK:
            C = np.cross(PS[:, 1] - PS[:, 0], PS[:, 2] - PS[:, 0])
            _chk("area", area, np.array([0.5 * np.linalg.norm(c) for c in C]))
            _chk("Nn", Nn, N0 / (np.array([np.linalg.norm(v) for v in N0]).reshape(-1, 1) + 1e-12))
            _chk("frames", (Nc, Tc, Bc), _frames(N0, T0, np.where(W < 0, -1.0, 1.0)))
    else:
        C = np.cross(PS[:, 1] - PS[:, 0], PS[:, 2] - PS[:, 0])
        norm = np.linalg.norm  # per row: np.linalg.norm(x, axis=1) sums differently (BLAS dot vs reduce)
        area[:] = [0.5 * norm(c) for c in C]
        Nn = N / (np.array([norm(v) for v in N]).reshape(-1, 1) + 1e-12)
    if smap is not None:
        k3 = smap.lit_many(PS.reshape(-1, 3), np.repeat(Nn, 3, axis=0)).reshape(-1, 3)
        ks[:] = (k3[:, 0] + k3[:, 1] + k3[:, 2]) / 3
    cen = (PS[:, 0] + PS[:, 1] + PS[:, 2]) / 3
    if all(hasattr(e["probe_of"], "many") for _, e in ent.values()) and len({id(e["probe_of"]) for _, e in ent.values()}) == 1:
        pof = segs[0][1]["probe_of"].many(cen)  # one ProbeIndex, queried in the same order
    else:
        pof = [e["probe_of"](tuple(c)) for e, c in zip((e for _, e, tl in segs for _ in range(len(tl))), cen.tolist())]
    for i, p in enumerate(pof):
        pid[i] = p if p is not None else -1
        if pid[i] not in faces and pid[i] in probes:
            faces[pid[i]] = np.array(probes[pid[i]], np.float64)
    N, T, B = (Nc, Tc, Bc) if MX is not None else _frames(N, T, np.where(W < 0, -1.0, 1.0))
    tri = dict(N=N, T=T, B=B, k=ks, alb=alb, lid=lid, pid=pid, rows=rows, probes=faces)
    # check the vectorised light against dsr_light (same row / probe, random normals)
    rn = np.random.default_rng(0).normal(size=(16, 3))
    rn /= np.linalg.norm(rn, axis=1, keepdims=True)
    f = dsr_light.light_fn(rows[0])
    pf = faces.get(pid[0])
    a, s = _irradiance(_light_terms(rows[0]), pf, rn)
    ref = [f(tuple(v), probes.get(pid[0]), split=True) for v in rn]
    err = max(np.abs(a - np.array([r[0] for r in ref])).max(), np.abs(s - np.array([r[1] for r in ref])).max())
    assert err < 1e-6, f"vectorised light differs from dsr_light by {err}"
    resp = _Response(tone, near, shadow_amb)
    R = resp(tri, np.array(RING[1:])) if MX is None else _mx_response(MX, resp, tri, np.array(RING[1:]))
    _, lab = (_kmeans if MX is None else lambda *a: _mx_kmeans(MX, *a))(R, BUMP_SPLIT, np.sqrt(area + 1e-6))
    if _CHECK and MX is not None:
        _chk("response", R, resp(tri, np.array(RING[1:])))
        _chk("split kmeans", _mx_kmeans(MX, R, BUMP_SPLIT, np.sqrt(area + 1e-6)), _kmeans(R, BUMP_SPLIT, np.sqrt(area + 1e-6)))
    # textures in use, the palette, index maps
    names = sorted({e["tex"] for _, e, _ in segs})
    imgs = {}
    for nme in names:
        xy = _mx_xy_pow2(MX, dds[nme], max(BUMP_MAX, 4), BUMP_MAX) if MX is not None else None
        if _CHECK and xy is not None:
            _chk(f"xy_pow2 {nme}", xy, xy_pow2(dds[nme], max(BUMP_MAX, 4), BUMP_MAX))
        if xy is None:
            xy = xy_pow2(dds[nme], max(BUMP_MAX, 4), BUMP_MAX)
        if xy is not None:
            imgs[nme] = xy
    allxy = np.concatenate([v.reshape(-1, 2) for v in imgs.values()])
    sub = allxy[np.random.default_rng(2).choice(len(allxy), min(len(allxy), 150000), replace=False)]
    pal, _ = _kmeans(sub, 255, iters=15) if MX is None else _mx_kmeans(MX, sub, 255, iters=15)
    if _CHECK and MX is not None:
        print(f"mapx check: palette input {sub.shape} {sub.dtype}, {len(imgs)} of {len(names)} textures decoded in C "
              f"where BC5", flush=True)
        _chk("palette kmeans", pal, _kmeans(sub, 255, iters=15)[0])
    pal = np.vstack([[0.0, 0.0], pal])
    texs, err_px = [], []
    for nme in names:
        if nme not in imgs:
            continue
        x0 = imgs[nme]
        h, w = x0.shape[:2]
        x1 = x0.reshape(h // 2, 2, w // 2, 2, 2).mean(axis=(1, 3)) if h > 1 and w > 1 else x0
        mips = [(_nearest(x.reshape(-1, 2), pal) if MX is None else _mx_nearest(MX, x.reshape(-1, 2), pal)).astype(np.uint8)
                for x in (x0, x1)]
        if _CHECK and MX is not None:
            _chk(f"mips {nme}", tuple(mips), tuple(_nearest(x.reshape(-1, 2), pal).astype(np.uint8) for x in (x0, x1)))
        err_px.append(np.linalg.norm(pal[mips[0]] - x0.reshape(-1, 2), axis=1).mean())
        texs.append(dict(name=nme, w=w, h=h, mips=[mm.tobytes() for mm in mips]))
    tex_of = {t["name"]: i for i, t in enumerate(texs)}
    # density (texels per metre, area-weighted) per texture for texcache's mip choice
    uva, wa = np.zeros(len(texs)), np.zeros(len(texs))
    out, local = {}, {}
    labl, areal, i = lab.tolist(), area.tolist(), 0
    for g, e, tl in segs:
        n = len(tl)
        if e["tex"] in tex_of:
            ti = tex_of[e["tex"]]
            out.update(zip(range(g, g + n), ((ti, x) for x in labl[i:i + n])))
            local.update(zip(range(g, g + n), range(i, i + n)))
            s = float(wa[ti])
            for x in areal[i:i + n]:  # summed in triangle order, as before
                s += x
            wa[ti] = s
        i += n
    for m in meshes:
        e = MESH[m["bump"]] if m.get("bump") is not None and m["tris"] else None
        if e is None or e["tex"] not in tex_of:
            continue
        ti = tex_of[e["tex"]]
        # all triangles at once, added to uva in triangle order (np.add.at) as the per-triangle loop did
        c = m.get("_np", {}).get("wuv")  # gather's array of this list (export_map._weld)
        wuv = c[1] if c is not None and c[0] is m["wuv"] else np.array(m["wuv"], np.float64).reshape(-1, 2)
        uv = wuv[mapx_native.tri_arrays(m["tris"])[1]] * np.array([texs[ti]["w"], texs[ti]["h"]])
        d1, d2 = uv[:, 1] - uv[:, 0], uv[:, 2] - uv[:, 0]
        np.add.at(uva, np.full(len(uv), ti), 0.5 * np.abs(d1[:, 0] * d2[:, 1] - d1[:, 1] * d2[:, 0]))
    _STATE["density"] = [float(math.sqrt(u / a)) if a > 0 else 0.0 for u, a in zip(uva, wa)]
    _STATE.update(texs=texs, tri=tri, R=R, area=area, local=local, resp=resp, pal=pal)
    print(f"bump: {nt} triangles ({100 * nt / max(1, k):.0f}%), {len(texs)} textures (<= {BUMP_MAX} px), "
          f"palette error {np.mean(err_px):.3f} (xy)")
    return out


def _finish():
    """chunks (CHUNK_BUMP) -> BUMP_K clusters by area-weighted mean response; CLUT per cluster = the mean response at
    every palette normal over a sample of its triangles. Returns (per chunk (texture, cluster) or None, CLUTs)."""
    R, area, local, tri, resp = (_STATE[k] for k in ("R", "area", "local", "tri", "resp"))
    ids = [i for i, b in enumerate(CHUNK_BUMP) if b]
    mem = [np.array([local[g] for g in CHUNK_BUMP[i][1] if g in local]) for i in ids]
    F = np.array([(R[m] * area[m, None]).sum(0) / max(1e-9, area[m].sum()) for m in mem])
    MX = _mx()
    cen, lab = (_kmeans if MX is None else lambda *a: _mx_kmeans(MX, *a))(F, BUMP_K, np.array([area[m].sum() for m in mem]) + 1e-6)
    if _CHECK and MX is not None:
        _chk("finish kmeans", (cen, lab), _kmeans(F, BUMP_K, np.array([area[m].sum() for m in mem]) + 1e-6))
    rng = np.random.default_rng(3)
    cluts, cerr, terr = [], [], []
    for c in range(len(cen)):
        tm = np.concatenate([mem[j] for j in np.nonzero(lab == c)[0]] or [np.zeros(0, int)])
        if not len(tm):
            cluts.append(np.ones(256))
            continue
        smp = tm if len(tm) <= CLUT_SAMPLE else rng.choice(tm, CLUT_SAMPLE, replace=False)
        r = resp({key: (v[smp] if isinstance(v, np.ndarray) else v) for key, v in tri.items()}, _STATE["pal"]) \
            if MX is None else _mx_response(MX, resp, tri, _STATE["pal"], rows=smp)
        if _CHECK and MX is not None and c < 4:
            _chk(f"clut response {c}", r, resp({key: (v[smp] if isinstance(v, np.ndarray) else v) for key, v in tri.items()}, _STATE["pal"]))
        cluts.append(r.mean(0))
        cerr.append(np.abs(r - r.mean(0)).mean() * 128)
        terr.append(np.abs(R[tm] - cen[c]).mean() * 128)
    per = [None] * len(CHUNK_BUMP)
    for j, i in enumerate(ids):
        per[i] = (CHUNK_BUMP[i][0][0], int(lab[j]))
    cl = np.clip(np.array(cluts), 0, 255 / 128)
    print(f"bump: {len(ids)} chunks in {len(cen)} clusters, |triangle - CLUT| {np.mean(cerr):.1f} / 128 over the "
          f"palette, {np.mean(terr):.1f} / 128 at the ring normals; ratio {cl.min():.2f}..{cl.max():.2f}")
    return per, cl


def _blocks8(w, h):
    """VRAM blocks of a PSMT8 level: texmap.blocks for <= 128 px wide (buffer width 128); wider ones use buffer width w
    (vu1.c TBW, the upload's), whole 128 x 64 pages of 32 blocks, a power of two"""
    if w <= 128:
        return texmap.blocks(texmap.PSMT8, w, h)
    n, top = 1, -(-w // 128) * -(-h // 64) * 32
    while n < top:
        n *= 2
    return n


def write(path, nchunks):
    """bump.bin for the chunks build_chunks made (CHUNK_BUMP); an empty one (chunks 0) when there is no relief."""
    texs = _STATE.get("texs")
    HDR = 48
    if not texs or len(CHUNK_BUMP) != nchunks or not any(CHUNK_BUMP):
        with open(path, "wb") as f:
            f.write(struct.pack("<4s7I", b"DSB1", 0, 0, 0, 0, 0, 0, 0) + b"\0" * (HDR - 32))
        return
    per, cl = _finish()
    blob = bytearray()

    def align(n):
        blob.extend(b"\0" * ((-(len(blob) + HDR)) % n))
    ctab = HDR + len(blob)
    pix0 = [np.frombuffer(t["mips"][0], np.uint8) for t in texs]
    spread = {}
    for b in per:
        if b and b not in spread:
            spread[b] = min(255, int(round(float((128 * cl[b[1]][pix0[b[0]]]).std()))))
        blob += struct.pack("<HBB", *(b if b else (0xFFFF, 0)), spread[b] if b else 0)
    align(16)
    ttab = HDR + len(blob)
    tpos = len(blob)
    blob += b"\0" * (texmap.ENTRY * len(texs))
    align(16)
    coff = HDR + len(blob)
    sw = [texmap._swz(i) for i in range(256)]
    for c in range(len(cl)):
        for lv in range(LEVELS):
            a = np.clip(np.round(128 * cl[c] ** (1.0 / (1 << lv))), 0, 255).astype(int)
            e = bytearray(1024)
            for i in range(256):
                e[4 * sw[i]:4 * sw[i] + 4] = bytes((0x80, 0x80, 0x80, a[i]))
            blob += e
    for k, t in enumerate(texs):
        offs = []
        for d in t["mips"]:
            align(128)
            offs.append(HDR + len(blob))
            blob += d
        b0, b1 = _blocks8(t["w"], t["h"]), _blocks8(t["w"] // 2, t["h"] // 2)
        struct.pack_into("<2H4BfIIIHHI", blob, tpos + texmap.ENTRY * k, t["w"], t["h"], texmap.PSMT8, texmap.CT32,
                         2, 0, _STATE["density"][k], 0, offs[0], offs[1], b0, b1, 0)
    hdr = struct.pack("<4s8I", b"DSB1", nchunks, len(texs), len(cl), LEVELS, ctab, ttab, coff, BUMP_CAP)
    with open(path, "wb") as f:
        f.write(hdr + b"\0" * (HDR - len(hdr)) + blob)
    nb = sum(1 for b in CHUNK_BUMP if b)
    sp = np.array([spread[b] for b in per if b])
    print(f"bump.bin: relief strength (std / 128) median {np.median(sp):.0f}, < 4: {100 * (sp < 4).mean():.0f}% of chunks")
    print(f"bump.bin: {nb} of {nchunks} chunks bumped, {len(texs)} textures, {len(cl)} x {LEVELS} CLUTs, "
          f"{HDR + len(blob)} bytes")
