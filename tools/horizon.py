"""Resident far backdrop ("horizon" level) of a DSM5 map.bin for world streaming (src/map.c, src/mapstream.c):

  horizon.py <map.bin> <hzn.bin>

Burg's coarse level is ~1000 small streamed items per block (64 KB pages of cells). Read one seek each, the far ones
in view never kept up on the disc: holes and late pop-in on the ISO route (notes/threads/burg-iso-route.md). The
horizon is a third, much coarser level of every cell, small enough to stay loaded with its block (tools/mapsplit.py
appends it to maptab.bin, the block's lite set: one read when the block turns on, no seeks after that). map.c draws a
cell's horizon instead of its coarse level beyond HZN_R metres of the camera, and wherever the coarse level is not
in yet (a late read shows a blurrier cell instead of a hole); mapstream.c then reads coarse cells only within
MS_R_HZN of the player.

Built from the map's own resident level (the CHUNK_COARSE chunks, the opaque kept ones and the backdrop of every cell, as map.bin
stores them), so it needs no re-export:
  1. every triangle gets its colour baked: texel of the texture's mip 1 at the corner's UV x the corner's light (the
     GS's modulate, 0x80 = 1.0); cut-out triangles mostly transparent are dropped (a fence is no wall at 60 m);
  1b. triangles no view can see are dropped (HZN_VIS, tools/native/hzvis.c: cube maps at navmesh points and
     orthographic views from all round, conservative);
  2. the whole block is welded on a grid of WELD_FR x the error's pixels at each vertex's view distance (meshes, UV
     seams and materials stop mattering: one surface; HZN_SIMP=qem: the full error, the old path);
  3. one edge collapse over the whole block with each vertex judged at its view distance (the distance to the nearest
     navmesh vertex, at least HZN_D): meshoptimizer's attribute-aware collapse in distance bands (mo_simplify; qem:
     tools/simplify_native.py, the exporter's simplifier): every cell's horizon comes from the same mesh, so
     neighbouring cells meet without cracks;
  4. the collapse stops at PX pixels of screen error, or at the one larger error (the same for every draw-group mesh
     of the block) at which the block's horizon fits KB kilobytes (the backdrop BD_KB apart; budget: lite sets share
     the near-cell arena), never past PX_MAX;
  5. triangles go to the cell whose box holds their centroid (else the nearest), are sorted along a Z curve and cut
     into VU1 batches: indexed (HZN_IDX, the default: a breadth-first walk over shared vertices, at most IDX_TRIS
     triangles / IDX_NV vertices, each vertex stored once), else lists of CHUNK_TRIS.
Untextured: map.c draws it with TME off (vu1.c VU1_FLAT), vertex colour = final colour.

hzn.bin (little endian, 16-byte aligned throughout; stream offsets from the section start, so it can move):
  header (16 B): 'HZN1', chunks, cells (= the map's cell table entries), bytes
  per cell (32 B): f32 box min[3], max[3] (its horizon chunks); u16 first chunk, chunk count; u32 flags: bit 0 the
                   cell has chunks the horizon stands for (even if they simplified to nothing), bit 1 it has backdrop
                   chunks
  chunk records (64 B, map.c Chunk): tex -1, flags CHUNK_HZN (| CHUNK_BACKDROP: from the backdrop | draw-group set
                   << 16: dg.bin piece mask index, 0 = always drawn), tris, pos / st / col offsets, box, org, step
  streams: per chunk one VIF packet: indexed (tris | nv << 16, idx_streams): pos s16x3 x nv, colour u8x4 x nv, u8x4
           triangles (map.c CHUNK_IDX); lists (chunk_streams): pos s16x3, colour u8x4 per corner; no st
Each draw-group set of the cell's chunks (dg.bin, next to map.bin) is its own mesh, clustered and simplified apart, so
map.c draws only what the floor's groups show (HZN_DG=0: one mesh, set 0, the old file).
Env: HZN=0 writes an empty file (the old drawing); HZN_IDX=0 lists (the old batches); HZN_SIMP=qem the old simplifier;
HZN_VIS=0 no visibility cut; HZN_PX, HZN_KB, HZN_BD_KB, HZN_D, HZN_WELD, HZN_WELD_FR, HZN_CW, HZN_DG."""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

HDR = 80
CHUNK_CULL, CHUNK_BLEND, CHUNK_SKY, CHUNK_GRASS, CHUNK_ST1K, CHUNK_COARSE, CHUNK_BACKDROP = 1, 2, 8, 16, 32, 64, 128
CHUNK_HZN = 256  # map.c: a horizon chunk (in hzn.bin only)
CHUNK_TRIS = 54  # export_map.CHUNK_TRIS (VU1 buffer)
POS_MAX = 32000
PSMT8, PSMT4, CT16 = 0x13, 0x14, 0x02
FOCAL = 568.0  # export_map: screen errors in pixels of a 568-pixel focal length ((px / 568)^2 = eps)
ON = os.environ.get("HZN", "1") == "1"
# Screen error at the nearest the horizon is drawn (map.c HZN_R 56 m - hysteresis). The coarse level's own cap is
# 24-36 px at 32 m (export_map WS_PX1), 14-20 px at 56 m. Lists at 12 px went past the KB cap in every block, so all of
# them sat at PX_MAX (32 px: the coarse, spiky far shapes of Ame's report, 2026-11-07); indexed, 8 px fits every Burg
# block in about the bytes 32 px took (Burg west 26.8k tris 455 KB, was 13.6k 455 KB) and Firelink reaches 12.7 px.
PX = float(os.environ.get("HZN_PX", 8.0))
HZN_D = float(os.environ.get("HZN_D", 54.0))  # metres: nearest view distance the error is judged from
# cap per block (ASSUMED; 320 with lists, which no block met: the PX_MAX floor won): lite sets share the near-cell arena with the near cells, relief and enemy models. At ~0.6 MB
# a block (1 MB for Firelink with its m8000 backdrop) mapstream_alloc held 4.4 MB of the 8 MB arena in the Burg and
# the near level waited up to 28 s (ISO route, 2026-11-06)
KB = float(os.environ.get("HZN_KB", 480))
PX_MAX = float(os.environ.get("HZN_PX_MAX", 32.0))  # the budget never pushes the error past this (ASSUMED)
# the backdrop (Firelink's m8000 view of the Burg and the valley, 8-14k triangles; a few hundred elsewhere) has its own
# budget: as a share of KB it took the map's triangles in Firelink, the block with the most to show far off
BD_KB = float(os.environ.get("HZN_BD_KB", 288))
WELD = float(os.environ.get("HZN_WELD", 0.35))  # metres: the finest clustering grid (ASSUMED)
ST = os.environ.get("HZN_ST", "0") == "1"  # write the (zero) st stream too (A/B)
# indexed batches (HZN_IDX=0: lists, the old file): each distinct vertex once (6 + 4 bytes) plus u8x4 per triangle,
# ~18-19 bytes a triangle against ~36 (horizon meshes share fewer vertices than closed ones), about twice the triangles
IDX = os.environ.get("HZN_IDX", "1") == "1"
MAPI_HDR, CLIPI_MAX = 8, 336  # export_map / tools/vu1/gen.py
IDX_TRIS = 64  # vu1.h VU1_IDX_TRIS
IDX_NV = 84  # vertices per batch at most (with 64 triangles: 8 + 252 + 64 = 324 <= CLIPI_MAX)
# bytes per triangle: 3 x (6 + 4) stream bytes, + a record, VIF codes and padding per chunk (measured ~36); indexed
# ~18-19 (measured 2026-11-07: Firelink 19.2, Burg west 17.7 at 12 px)
BPT = 19 if IDX else 36
# simplifier (HZN_SIMP): "meshopt", the default: a light weld (WELD_FR of the error's grid), then meshoptimizer's
# attribute-aware edge collapse (tools/native/meshopt, MIT) in distance bands (mo_simplify); "qem": the old path, the
# weld at the full error then tools/native/simplify.c. Grid clustering at the full error welded unrelated surfaces into
# slivers and the collapse kept long thin fans: the spiky far shapes of Ame's report (2026-11-07)
SIMP = os.environ.get("HZN_SIMP", "meshopt")
WELD_FR = float(os.environ.get("HZN_WELD_FR", 0.25))  # weld grid / error grid (ASSUMED: closes cracks under 1/4 the error)
# colour weight: a baked colour step of 1/CW of full range counts as much as the band's allowed position error (ASSUMED)
CW = float(os.environ.get("HZN_CW", 4.0))
# visibility (HZN_VIS, tools/native/hzvis.c): triangles no view can see are dropped before simplifying (interiors,
# undersides, faces pressed against other faces); views = cube maps at navmesh points EYE metres up (Y up) and
# orthographic views from VIS_DIRS directions (cameras outside the block). Conservative: a doubtful triangle stays
VIS = os.environ.get("HZN_VIS", "1") == "1"
VIS_DIRS = int(os.environ.get("HZN_VIS_DIRS", 64))
VIS_N = int(os.environ.get("HZN_VIS_N", 96))  # navmesh eye points at most
VIS_RES_O, VIS_RES_P = 512, 256  # pixels: orthographic view, cube face
EYE = 1.6  # metres above the navmesh (ASSUMED: DSR's camera sits ~1.5-2 m up)


def _swz(i):
    return (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)


def texture_rgba(b, tex_off, t):
    """(h, w, 4) uint8 of texture t's mip 1 (mip 0 if it has one level); alpha 255 opaque / 0 cut out"""
    w, h, psm, cpsm, levels, _fl, _d, clut, p0, p1 = struct.unpack_from("<2H4BfIII", b, tex_off + 32 * t)
    if levels > 1 and p1:
        w, h, pix = w // 2, h // 2, p1
    else:
        pix = p0
    n = 16 if psm == PSMT4 else 256
    pal = np.zeros((n, 4), np.uint8)
    for i in range(n):
        j = _swz(i) if n == 256 else i
        if cpsm == CT16:
            v = struct.unpack_from("<H", b, clut + 2 * j)[0]
            pal[i] = ((v & 31) << 3, (v >> 5 & 31) << 3, (v >> 10 & 31) << 3, 255 if v & 0x8000 else 0)
        else:
            r, g, bb, a = b[clut + 4 * j:clut + 4 * j + 4]
            pal[i] = (r, g, bb, 255 if a >= 0x40 else 0)
    if psm == PSMT4:
        raw = np.frombuffer(b, np.uint8, w * h // 2, pix)
        idx = np.stack([raw & 15, raw >> 4], 1).reshape(-1)
    else:
        idx = np.frombuffer(b, np.uint8, w * h, pix)
    return pal[idx].reshape(h, w, 4)


def coarse_tris(b):
    """-> P (n, 3, 3) float64 corners, RGB (n, 3, 3) float baked colour (0..255), keep (n,) bool, backdrop (n,) bool
    (from CHUNK_BACKDROP chunks), the map's cells, the set of cells with covered chunks"""
    nch = struct.unpack_from("<I", b, 4)[0]
    chunk_off, tex_off = struct.unpack_from("<2I", b, 36)
    cell_off = struct.unpack_from("<I", b, 76)[0]
    ncell = struct.unpack_from("<I", b, cell_off)[0]
    cells = [struct.unpack_from("<6f6I", b, cell_off + 16 + 48 * i) for i in range(ncell)]
    tex = {}
    Ps, Cs, Ks, Bs, Ds, cov = [], [], [], [], [], set()
    cdg = chunk_dg(b, nch)
    for ci, ce in enumerate(cells):
        if ce[8] & 0x80000000:  # a later near part (no resident chunks)
            continue
        for j in range(ce[9], ce[9] + ce[10]):
            ti, fl, ntri, po, so, co = struct.unpack_from("<iII3I", b, chunk_off + 64 * j)
            if fl & (CHUNK_BLEND | CHUNK_SKY | CHUNK_GRASS):
                continue  # covered: coarse chunks, opaque kept ones and the backdrop (map.c hz_covers)
            cov.add(ci)
            mn_mx_org_step = struct.unpack_from("<10f", b, chunk_off + 64 * j + 24)
            org, step = np.array(mn_mx_org_step[6:9]), mn_mx_org_step[9]
            nv, ntri = ntri >> 16, ntri & 0xFFFF  # tris | nv << 16 (export_map.chunk_packet)
            n = 3 * ntri
            # an indexed chunk (nv > 0) stores nv distinct corners, then a u8 x4 triangle stream after its own VIF code
            # qword (map.c CHUNK_IDX); expand it to one entry per corner like an unindexed chunk
            corner = np.frombuffer(b, np.uint8, 4 * ntri, co + ((4 * nv + 15) & ~15) + 16).reshape(-1, 4)[:, :3] \
                .ravel().astype(np.int64) if nv else slice(None)
            m = nv or n
            p = np.frombuffer(b, np.int16, 3 * m, po).reshape(-1, 3)[corner].astype(np.float64) * step + org
            st = np.frombuffer(b, np.uint16, 2 * m, so).reshape(-1, 2)[corner].astype(np.float64) / \
                (1024.0 if fl & CHUNK_ST1K else 4096.0)
            col = np.frombuffer(b, np.uint8, 4 * m, co).reshape(-1, 4)[corner][:, :3].astype(np.float64)
            if ti not in tex:
                tex[ti] = texture_rgba(b, tex_off, ti) if ti >= 0 else None
            img = tex[ti]
            if img is None:
                rgb, a = np.full((n, 3), 128.0), np.ones(n, bool)
                cen_a = np.ones(ntri, bool)
            else:
                h, w = img.shape[:2]

                def sample(uv):
                    x = (np.floor(uv[:, 0] * w).astype(np.int64)) % w
                    y = (np.floor(uv[:, 1] * h).astype(np.int64)) % h
                    return img[y, x]
                s = sample(st)
                rgb, a = s[:, :3].astype(np.float64), s[:, 3] > 0
                # each corner's colour: the mean of the opaque texels from the corner a third and two thirds of the way
                # to the centroid (the coarse level is seen through mips 2-3 at this distance: an average, not the
                # texel under the corner; one texel made the horizon blotchy against the level it stands for)
                cst = np.repeat(st.reshape(-1, 3, 2).mean(1), 3, 0)
                acc, cnt3 = np.where(a[:, None], rgb, 0.0), a.astype(np.float64)
                for u in (1.0 / 3.0, 2.0 / 3.0):
                    sm = sample(st + (cst - st) * u)
                    ok = sm[:, 3] > 0
                    acc += np.where(ok[:, None], sm[:, :3], 0.0)
                    cnt3 += ok
                rgb = np.where(cnt3[:, None] > 0, acc / np.maximum(cnt3, 1)[:, None], rgb)
                cen = sample(st.reshape(-1, 3, 2).mean(1))
                cen_a = cen[:, 3] > 0
                # transparent corners take the triangle's mean opaque texel (cut-out edges keep the leaf colour)
                tri_rgb = np.where(a[:, None], rgb, 0).reshape(-1, 3, 3).sum(1)
                cnt = a.reshape(-1, 3).sum(1)
                mean = np.where(cnt[:, None] > 0, tri_rgb / np.maximum(cnt, 1)[:, None], cen[:, :3])
                rgb = np.where(a[:, None], rgb, np.repeat(mean, 3, 0))
            opaque = a.reshape(-1, 3).sum(1) + cen_a >= 2  # at least half of 4 samples
            Ps.append(p.reshape(-1, 3, 3))
            Cs.append(np.minimum(255.0, rgb * col / 128.0).reshape(-1, 3, 3))
            Ks.append(opaque)
            Bs.append(np.full(ntri, bool(fl & CHUNK_BACKDROP)))
            Ds.append(np.full(ntri, cdg[j] if j < len(cdg) else 0, np.int64))
    if not Ps:
        z = np.zeros(0, bool)
        return np.zeros((0, 3, 3)), np.zeros((0, 3, 3)), z, z, np.zeros(0, np.int64), cells, cov
    return (np.concatenate(Ps), np.concatenate(Cs), np.concatenate(Ks), np.concatenate(Bs), np.concatenate(Ds), cells,
            cov)


def chunk_dg(b, nch):
    """draw-group set per resident chunk (dg.bin next to map.bin, written by the same export: piece mask index per
    chunk in bump.bin's order, resident chunks first); all 0 (always shown) without one or with HZN_DG=0"""
    path = os.path.join(os.path.dirname(os.path.abspath(SRC[0])), "dg.bin") if SRC else ""
    if os.environ.get("HZN_DG", "1") != "1" or not path or not os.path.exists(path):
        return np.zeros(nch, np.int64)
    d = open(path, "rb").read()
    _mg, P, C, N = struct.unpack_from("<4I", d, 0)
    if N < nch:
        return np.zeros(nch, np.int64)
    return np.frombuffer(d, np.uint8, nch, 32 + 16 * (P + C)).astype(np.int64)


def nav_points(b):
    nv = struct.unpack_from("<I", b, 12)[0]
    navv_off = struct.unpack_from("<I", b, 44)[0]
    return np.frombuffer(b, np.float32, 3 * nv, navv_off).reshape(-1, 3).astype(np.float64)


_SO = {}


def _native(name, srcs, setup, cxx=False):
    """tools/native/<name>.so from srcs (paths under tools/native), built on first use or when a source is newer (an
    atomic replace: parallel exports may build at once); setup(lib) sets the argtypes. None without a compiler, with
    HZN_NATIVE=0, or on a failed build (the caller falls back)."""
    if name not in _SO:
        L = None
        if os.environ.get("HZN_NATIVE", "1") != "0":
            import ctypes
            native = os.path.join(HERE, "native")
            src = [os.path.join(native, s) for s in srcs]
            so = os.path.join(native, name + ".so")
            try:
                if not os.path.exists(so) or os.path.getmtime(so) < max(os.path.getmtime(s) for s in src):
                    import subprocess, tempfile
                    fd, tmp = tempfile.mkstemp(suffix=".so", dir=native)
                    os.close(fd)
                    try:
                        cc = os.environ.get("CXX", "c++") if cxx else os.environ.get("CC", "cc")
                        subprocess.run([cc, "-O2", "-ffp-contract=off", "-fno-fast-math", "-shared", "-fPIC", "-o", tmp]
                                       + src, check=True, capture_output=True)
                        os.replace(tmp, so)
                    finally:
                        if os.path.exists(tmp):
                            os.remove(tmp)
                L = ctypes.CDLL(so)
                setup(L)
            except Exception as e:
                print(f"horizon: {name} unavailable ({str(e)[:200]})", flush=True)
                L = None
        _SO[name] = L
    return _SO[name]


def _nearest_lib():
    """tools/native/libnearest.so (min_d2); None without a compiler (numpy path, same results) or with HZN_NATIVE=0"""
    def setup(L):
        import ctypes
        vp, i = ctypes.c_void_p, ctypes.c_int
        L.min_d2.argtypes = [i, vp, i, vp, ctypes.c_double, vp]
        L.min_d2.restype = None
    return _native("libnearest", ["nearest.c"], setup)


def view_d(X, nav):
    """view distance of points X: to the nearest of at most 4000 navmesh vertices (brute force in blocks: no scipy in
    the build venv), at least HZN_D (the horizon is never drawn nearer)"""
    if not len(nav) or not len(X):
        return np.full(len(X), HZN_D)
    ns = nav[::max(1, len(nav) // 4000)]
    L = _nearest_lib()
    if L is not None:  # the same minimum in C (tools/native/nearest.c): 93 % of a Burg block's 150 s before
        Q, S, d2 = np.ascontiguousarray(X, np.float64), np.ascontiguousarray(ns, np.float64), np.empty(len(X))
        L.min_d2(len(Q), Q.ctypes.data, len(S), S.ctypes.data, HZN_D * HZN_D, d2.ctypes.data)
        return np.maximum(np.sqrt(d2), HZN_D)
    d = np.empty(len(X))
    for s in range(0, len(X), 2048):
        v = X[s:s + 2048]
        d[s:s + 2048] = np.sqrt(((v[:, None, :] - ns[None]) ** 2).sum(2).min(1))
    return np.maximum(d, HZN_D)


def cluster(P, C, nav, px):
    """vertex clustering on a grid of px pixels at each corner's view distance, in octaves (grid x 2 per doubling of
    the distance, so a band is one consistent grid) -> (V positions (cluster means), VC colours (area weighted),
    T vertex triples); degenerate and duplicate triangles (either winding) dropped. Burg's coarse level is a soup of
    open pieces (a quarter of its edges are borders): an edge collapse alone stops at ~half, clustering welds them."""
    X = P.reshape(-1, 3)
    d = view_d(X, nav)
    lv = np.floor(np.log2(d / HZN_D)).astype(np.int64)
    g = np.maximum(HZN_D * px / FOCAL * 2.0 ** lv, WELD)
    q = np.floor(X / g[:, None]).astype(np.int64)
    _, inv = np.unique(np.concatenate([lv[:, None], q], 1), axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    nv = int(inv.max()) + 1 if len(inv) else 0
    area = 0.5 * np.linalg.norm(np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]), axis=1) + 1e-4
    wgt = np.repeat(area, 3)
    V = np.zeros((nv, 3))
    np.add.at(V, inv, X)
    V /= np.bincount(inv, minlength=nv).astype(np.float64)[:, None]
    VC = np.zeros((nv, 3))
    np.add.at(VC, inv, C.reshape(-1, 3) * wgt[:, None])
    VC /= np.bincount(inv, weights=wgt, minlength=nv)[:, None]
    T = inv.reshape(-1, 3)
    ok = (T[:, 0] != T[:, 1]) & (T[:, 1] != T[:, 2]) & (T[:, 0] != T[:, 2])
    T = T[ok]
    _, first = np.unique(np.sort(T, 1), axis=0, return_index=True)
    return V, VC, T[np.sort(first)]


def _meshopt():
    def setup(L):
        import ctypes
        vp, sz = ctypes.c_void_p, ctypes.c_size_t
        L.meshopt_simplifyWithAttributes.argtypes = [vp, vp, sz, vp, sz, sz, vp, sz, vp, sz, vp, sz, ctypes.c_float,
                                                     ctypes.c_uint, vp]
        L.meshopt_simplifyWithAttributes.restype = sz
    return _native("libmeshopt", ["meshopt/simplifier.cpp", "meshopt/allocator.cpp"], setup, cxx=True)


MO_ABS, MO_PRUNE, MO_LOCK = 1 << 2, 1 << 3, 1  # meshoptimizer.h: SimplifyErrorAbsolute, SimplifyPrune, Vertex_Lock


def mo_simplify(V, VC, T, d, px):
    """meshoptimizer's collapse with the error judged at each vertex's view distance d: passes over distance bands
    (octaves from HZN_D), pass k at the error px pixels make at the band's near edge, the vertices nearer than the band
    locked (they keep the finer error of the passes before). Colour is an attribute (CW), so collapses keep colour
    edges too. Prune drops pieces smaller than the error. -> surviving triangles (vertex triples into V)"""
    import ctypes
    L = _meshopt()
    if not len(T):
        return np.zeros((0, 3), np.int64)
    idx = np.ascontiguousarray(T.reshape(-1), np.uint32)
    pos = np.ascontiguousarray(V, np.float32)
    att = np.ascontiguousarray(VC / 255.0, np.float32)
    dst = np.empty_like(idx)
    k = 0
    while True:
        r0 = HZN_D * 2.0 ** k
        if k and not (d >= r0).any():
            break
        e = px / FOCAL * r0
        lock = np.ascontiguousarray(np.where(d < r0, MO_LOCK, 0), np.uint8)
        w = np.full(3, e * CW, np.float32)
        n = L.meshopt_simplifyWithAttributes(dst.ctypes.data, idx.ctypes.data, len(idx), pos.ctypes.data, len(pos), 12,
                                             att.ctypes.data, 12, w.ctypes.data, 3, lock.ctypes.data, 0,
                                             ctypes.c_float(e), MO_ABS | MO_PRUNE, None)
        idx = dst[:n].copy()
        k += 1
    return idx.reshape(-1, 3).astype(np.int64)


def visible_tris(P, key, nav):
    """(n,) bool: triangles some view may see (tools/native/hzvis.c); all True without the library"""
    def setup(L):
        import ctypes
        vp, i = ctypes.c_void_p, ctypes.c_int
        L.hz_vis.argtypes = [i, vp, vp, i, vp, i, i, i, vp]
        L.hz_vis.restype = None
    L = _native("libhzvis", ["hzvis.c"], setup)
    if L is None or not len(P):
        return np.ones(len(P), bool)
    eyes = nav[np.linspace(0, len(nav) - 1, min(VIS_N, len(nav))).astype(np.int64)] if len(nav) else np.zeros((0, 3))
    eyes = np.ascontiguousarray(eyes + np.array([0.0, EYE, 0.0]), np.float32)
    tri = np.ascontiguousarray(P.reshape(-1, 9), np.float32)
    ks = np.ascontiguousarray(key, np.int32)
    out = np.zeros(len(P), np.uint8)
    L.hz_vis(len(P), tri.ctypes.data, ks.ctypes.data, len(eyes), eyes.ctypes.data, VIS_DIRS, VIS_RES_O, VIS_RES_P,
             out.ctypes.data)
    return out.astype(bool)


def qlog(V, T, nav):
    """quadric collapse log of a clustered mesh (flat stretches the grid cut up), each vertex weighted by its view
    distance: costs are screen errors^2 at a 1-unit focal length; None without triangles"""
    import simplify_native
    if not len(T):
        return None
    d = view_d(V, nav)
    return simplify_native.simplify_log(V, np.arange(len(V), dtype=np.int32), T.astype(np.int32), None, 1.0 / (d * d))


def alive(log, eps):
    """triangles a log leaves at error eps"""
    if log is None:
        return 0
    k = int(np.searchsorted(log[1], eps, side="right"))
    return len(log[0]) - (int(np.cumsum(log[2])[k - 1]) if k else 0)


def block_eps(logs, cap):
    """the smallest error >= PX (as eps) whose triangles over all logs fit cap, at most PX_MAX (the budget gives way)"""
    e0, e1 = (PX / FOCAL) ** 2, (PX_MAX / FOCAL) ** 2
    if sum(alive(L, e0) for L in logs) <= cap:
        return e0
    cand = np.unique(np.concatenate([L[1][(L[1] > e0) & (L[1] <= e1)] for L in logs if L is not None] + [[e1]]))
    lo_i, hi_i = 0, len(cand) - 1
    if sum(alive(L, cand[hi_i]) for L in logs) > cap:
        return float(cand[hi_i])
    while lo_i < hi_i:  # alive is non-increasing in eps
        m = (lo_i + hi_i) // 2
        if sum(alive(L, cand[m]) for L in logs) <= cap:
            hi_i = m
        else:
            lo_i = m + 1
    return float(cand[lo_i])


def replay(log, eps):
    """surviving triangles (vertex triples) of a log at error eps"""
    import simplify_native
    if log is None:
        return np.zeros((0, 3), np.int64)
    return simplify_native.pysimplify.replay_log(log, eps)[:, 0::2]


def zkey(c, lo, step):
    q = np.clip(((c - lo) / step).astype(np.int64), 0, 1023)
    k = np.zeros(len(c), np.int64)
    for bit in range(10):
        for a in range(3):
            k |= ((q[:, a] >> bit) & 1) << (3 * bit + a)
    return k


def chunk_streams(ntri, pos, col):
    """export_map.chunk_streams without the st stream (untextured: VU1 reads whatever its st slots hold, the GS
    ignores ST with TME off): {NOP, NOP, STCYCL 3,1, UNPACK V3-16 n -> 7} pos, {NOP x 3, UNPACK V4-8 n -> 9} colour.
    The st offset points at the positions (vu1.c's split path still sends an st stream: harmless bytes)."""
    n = 3 * ntri
    if ST:  # with a zero st stream, as export_map.chunk_streams
        codes = ((0, 0, 0x01000103, 0x69008007 | n << 16), (0, 0, 0, 0x6500C008 | n << 16),
                 (0, 0, 0, 0x6E00C009 | n << 16))
        arrs = (pos, bytes(4 * n), col)
    else:
        codes = ((0, 0, 0x01000103, 0x69008007 | n << 16), (0, 0, 0, 0x6E00C009 | n << 16))
        arrs = (pos, col)
    out, offs = bytearray(), []
    for arr, cw in zip(arrs, codes):
        out += struct.pack("<4I", *cw)
        offs.append(len(out))
        out += arr
        out.extend(b"\0" * (-len(out) % 16))
    return out, offs if ST else [offs[0], offs[0], offs[1]]


def idx_streams(ntri, nv, pos, col, idx):
    """an indexed horizon batch (map.c CHUNK_IDX, vu1.c vu1_add_idx): {code} pos s16x3 x nv, {code} colour u8x4 x nv,
    {code} triangles u8x4; no st stream (the st offset points at the colours: vu1mapi reads them, the GS ignores ST
    with TME off). The code qwords are export_map.chunk_streams' (vu1.c REFs the streams one by one, never VU1_PKT)."""
    codes = ((0, 0, 0x01000103, 0x69008000 | MAPI_HDR | nv << 16), (0, 0, 0, 0x6E00C000 | (MAPI_HDR + 2) | nv << 16),
             (0, 0, 0x01000101, 0x6E00C000 | (MAPI_HDR + 3 * nv) | ntri << 16))
    out, offs = bytearray(), []
    for arr, cw in zip((pos, col, idx), codes):
        out += struct.pack("<4I", *cw)
        offs.append(len(out))
        out += arr
        out.extend(b"\0" * (-len(out) % 16))
    return out, [offs[0], offs[1], offs[1]]


def idx_batches(T):
    """a cell's triangles (vertex triples) -> batches for vu1mapi that share as many vertices as they can: triangles
    in breadth-first order over shared vertices (a surface is walked as a growing patch, not in Z-curve stripes), cut
    where the next one would pass IDX_TRIS triangles or IDX_NV vertices (vu1clip's indexed entry: MAPI_HDR + 3 nv +
    tris <= CLIPI_MAX, so a batch at the near plane is clipped on VU1)"""
    n = len(T)
    if not n:
        return []
    flat = T.reshape(-1)
    order_v = np.argsort(flat, kind="stable")
    vs, starts = np.unique(flat[order_v], return_index=True)
    ends = np.append(starts[1:], len(flat))
    pos_of = {int(v): (int(a), int(b)) for v, a, b in zip(vs, starts, ends)}
    done = np.zeros(n, bool)
    seq = []
    for seed in range(n):  # Z-curve order (the caller's) picks the seeds
        if done[seed]:
            continue
        done[seed] = True
        q, qi = [seed], 0
        while qi < len(q):
            t = q[qi]
            qi += 1
            seq.append(t)
            for v in T[t]:
                a, b = pos_of[int(v)]
                for k in order_v[a:b] // 3:
                    if not done[k]:
                        done[k] = True
                        q.append(int(k))
    out, cur, vset = [], [], set()
    for t in seq:
        new = vset | set(int(v) for v in T[t])
        nv = -(-len(new) // 3) * 3
        if cur and (len(cur) + 1 > IDX_TRIS or nv > IDX_NV or MAPI_HDR + 3 * nv + len(cur) + 1 > CLIPI_MAX):
            out.append(T[cur])
            cur, new = [], set(int(v) for v in T[t])
        cur.append(t)
        vset = new
    out.append(T[cur])
    return out


def _place(V, VC, T, lo, hi, cfl):
    """triangles to cells (the box holding the centroid, else the nearest box), Z-curve order inside a cell"""
    cen = V[T].mean(1) if len(T) else np.zeros((0, 3))
    tc = np.zeros(len(T), np.int64)
    for s in range(0, len(T), 4096):
        c = cen[s:s + 4096, None, :]
        dd = np.maximum(0, np.maximum(lo[None] - c, c - hi[None]))
        tc[s:s + 4096] = (dd * dd).sum(2).argmin(1)
    glo = cen.min(0) if len(cen) else np.zeros(3)
    order = np.lexsort((zkey(cen, glo, 4.0), tc)) if len(T) else np.zeros(0, np.int64)
    return V, np.clip(np.rint(VC), 0, 255).astype(np.uint8), T[order], tc[order], cfl


SRC = []


def build(b):
    """-> (hzn.bin bytes, stats dict)"""
    assert b[:4] == b"DSM5", "horizon: not a DSM5 map.bin"
    P, C, keep, bd, dg, cells, cov = coarse_tris(b)
    ncell = len(cells)
    n_in = len(P)
    nav = nav_points(b)
    lo = np.array([c[0:3] for c in cells]).reshape(-1, 3)
    hi = np.array([c[3:6] for c in cells]).reshape(-1, 3)
    part = np.array([bool(c[8] & 0x80000000) for c in cells], bool)
    lo[part], hi[part] = 1e9, 1e9 + 1  # later near parts hold no resident chunks
    # two meshes: the map, and the far backdrop (export_map BACKDROP: map.c never distance culls it, so its horizon
    # chunks keep CHUNK_BACKDROP); the backdrop is small and already coarse, so only the map counts against KB
    # one mesh per draw-group set (dg.bin: map.c draws a horizon chunk only while the floor's groups show its set):
    # merged into one surface, a cell's stand-ins and the detail they stand in for, Firelink's views of the Burg and the
    # Burg itself, drew all at once, spiky and purple, wherever the horizon was used (Ame's ISO report, 2026-11-07)
    groups, stats = [], dict(opaque=0, clustered=0, px=PX, hidden=0)
    # one screen error for the whole block (every set at once): the smallest at or above PX whose triangles fit the
    # budget. Shared by triangle count, each set hit its own cap apart: small sets stayed at PX while the big ones went
    # to PX_MAX (Firelink: every block at 32 px, 2026-11-07)
    if VIS and len(P):  # visibility key: draw-group set and backdrop apart (map.c may skip either: never occluders)
        vis = visible_tris(P[keep], dg[keep] * 2 + bd[keep], nav)
        stats["hidden"] = int((~vis).sum())
        keep = keep.copy()
        keep[np.flatnonzero(keep)[~vis]] = False
    mo = SIMP == "meshopt" and _meshopt() is not None
    for g in (1, 0):  # the backdrop (BD_KB), then the map (KB)
        selg = keep & (bd == bool(g))
        budget = BD_KB if g else KB
        if mo:
            raw = [(dset, selg & (dg == dset)) for dset in np.unique(dg[selg])]
            cap = int(budget * 1024 / BPT)

            def run(px):
                out = []
                for dset, sel in raw:
                    V, VC, T = cluster(P[sel], C[sel], nav, px * WELD_FR)
                    out.append((dset, V, VC, len(T), mo_simplify(V, VC, T, view_d(V, nav), px)))
                return out

            def total(r):
                return sum(len(x[4]) for x in r)
            px, res = PX, run(PX)
            if total(res) > cap:  # the smallest error that fits (geometric search, ~2 % apart at the end)
                plo, phi, res = PX, PX_MAX, run(PX_MAX)
                if total(res) <= cap:
                    for _ in range(7):
                        mid = (plo * phi) ** 0.5
                        r = run(mid)
                        if total(r) <= cap:
                            phi, res = mid, r
                        else:
                            plo = mid
                px = phi
            for dset, V, VC, ncl, T in res:
                stats["opaque"] += int((selg & (dg == dset)).sum())
                stats["clustered"] += ncl
                groups.append(_place(V, VC, T, lo, hi, CHUNK_HZN | (CHUNK_BACKDROP if g else 0) | int(dset) << 16))
            if not g:
                stats["px"] = float(px)
            continue
        sets = []
        for dset in np.unique(dg[selg]):
            sel = selg & (dg == dset)
            V, VC, T = cluster(P[sel], C[sel], nav, PX)
            stats["opaque"] += int(sel.sum())
            stats["clustered"] += len(T)
            sets.append((dset, V, VC, qlog(V, T, nav)))
        eps = block_eps([x[3] for x in sets], int(budget * 1024 / BPT))
        for dset, V, VC, log in sets:
            T = replay(log, eps)
            groups.append(_place(V, VC, T, lo, hi, CHUNK_HZN | (CHUNK_BACKDROP if g else 0) | int(dset) << 16))
        if not g:
            stats["px"] = float(np.sqrt(eps) * FOCAL)
    recs, streams, ctab = [], bytearray(), []
    first = 0
    for ci in range(ncell):
        nchk, flags = 0, (1 if ci in cov else 0)
        cmn, cmx = np.full(3, 1e9), np.full(3, -1e9)
        for V, col, T, tc, cfl in groups:
            sel = np.flatnonzero(tc == ci)
            for t in (idx_batches(T[sel]) if IDX else [T[sel[s:s + CHUNK_TRIS]] for s in range(0, len(sel), CHUNK_TRIS)]):
                if IDX:  # the batch's distinct vertices in first-use order, padded to a multiple of 3 (vu1mapi)
                    u, first_use, inv = np.unique(t.reshape(-1), return_index=True, return_inverse=True)
                    order = np.argsort(first_use, kind="stable")
                    rank = np.empty(len(u), np.int64)
                    rank[order] = np.arange(len(u))
                    vs = u[order]
                    nv = -(-len(vs) // 3) * 3
                    vs = np.concatenate([vs, np.repeat(vs[-1:], nv - len(vs))])
                    tri = rank[inv.reshape(-1)].reshape(-1, 3)
                else:
                    vs = t.reshape(-1)
                p = V[vs]
                mn, mx = p.min(0), p.max(0)
                org = (mn + mx) / 2
                ext = np.abs(p - org).max()
                e = int(np.ceil(np.log2(max(ext, 1e-3) / POS_MAX)))
                step = 2.0 ** e
                qp = np.clip(np.rint((p - org) / step), -32767, 32767).astype(np.int16)
                pq = qp.astype(np.float64) * step + org  # the box of what is drawn
                mn, mx = pq.min(0), pq.max(0)
                cmn, cmx = np.minimum(cmn, mn), np.maximum(cmx, mx)
                c4 = np.concatenate([col[vs], np.full((len(p), 1), 0x80, np.uint8)], 1)
                if IDX:
                    ix = np.zeros((len(t), 4), np.uint8)
                    ix[:, :3] = tri
                    pk, offs = idx_streams(len(t), nv, qp.tobytes(), c4.tobytes(), ix.tobytes())
                    ntw = len(t) | nv << 16
                else:
                    pk, offs = chunk_streams(len(t), qp.tobytes(), c4.tobytes())
                    ntw = len(t)
                recs.append((cfl, ntw, len(streams), offs, mn, mx, org, step))
                streams += pk
                nchk += 1
                flags |= 2 if cfl & CHUNK_BACKDROP else 0
        ctab.append((cmn if nchk else np.zeros(3), cmx if nchk else np.zeros(3), first, nchk, flags))
        first += nchk
    nchunks = first
    assert nchunks < 65536
    s0 = 16 + 32 * ncell + 64 * nchunks
    out = bytearray(struct.pack("<4s3I", b"HZN1", nchunks, ncell, 0))
    for mn, mx, f, n, fl in ctab:
        out += struct.pack("<6f2HI", *mn, *mx, f, n, fl)
    for cfl, ntri, so, offs, mn, mx, org, step in recs:
        out += struct.pack("<iII3I6f4f", -1, cfl, ntri, *(s0 + so + o for o in offs), *mn, *mx, *org, step)
    assert len(out) == s0
    out += streams
    out.extend(b"\0" * (-len(out) % 16))
    struct.pack_into("<I", out, 12, len(out))
    st = dict(cells=ncell, tris_in=n_in, tris_opaque=stats["opaque"], tris_welded=stats["clustered"],
              tris=sum(len(g[2]) for g in groups), bd_tris=sum(len(g[2]) for g in groups if g[4] & CHUNK_BACKDROP),
              sets=len(groups), chunks=nchunks, bytes=len(out),
              px=stats["px"], covered=len(cov), hidden=stats["hidden"], simp="meshopt" if mo else "qem")
    return bytes(out), st


def main():
    src, dst = sys.argv[1:3]
    b = open(src, "rb").read()
    if not ON or b[:4] != b"DSM5":
        open(dst, "wb").close()
        print(f"horizon: {src}: none ({'HZN=0' if not ON else 'not DSM5'})")
        return
    SRC.append(src)
    out, st = build(b)
    with open(dst, "wb") as f:
        f.write(out)
    print(f"horizon: {os.path.basename(os.path.dirname(os.path.abspath(src)))}: {st['tris_in']} coarse tris "
          f"({st['tris_opaque']} opaque, {st['hidden']} unseen, {st['tris_welded']} welded, {st['simp']}) -> {st['tris']} tris in {st['chunks']} chunks over "
          f"{st['cells']} cells ({st['covered']} covered, {st['bd_tris']} backdrop tris, {st['sets']} draw-group meshes), {st['bytes'] >> 10} KB, {st['px']:.1f} px at {HZN_D:.0f} m")


if __name__ == "__main__":
    main()
