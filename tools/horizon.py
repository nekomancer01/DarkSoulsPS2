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
  2. the whole block is clustered on a grid of PX pixels at each vertex's view distance (meshes, UV seams and
     materials stop mattering: one surface);
  3. one quadric edge collapse over the whole block (tools/simplify_native.py, the exporter's simplifier) with each
     vertex weighted by its view distance (the distance to the nearest navmesh vertex, at least HZN_D): every cell's
     horizon comes from the same mesh, so neighbouring cells meet without cracks;
  4. the collapse stops at PX pixels of screen error, or earlier if the block's horizon would pass KB kilobytes
     (budget: lite sets share the near-cell arena);
  5. triangles go to the cell whose box holds their centroid (else the nearest), are sorted along a Z curve and cut
     into VU1 batches of CHUNK_TRIS.
Untextured: map.c draws it with TME off (vu1.c VU1_FLAT), vertex colour = final colour.

hzn.bin (little endian, 16-byte aligned throughout; stream offsets from the section start, so it can move):
  header (16 B): 'HZN1', chunks, cells (= the map's cell table entries), bytes
  per cell (32 B): f32 box min[3], max[3] (its horizon chunks); u16 first chunk, chunk count; u32 flags: bit 0 the
                   cell has chunks the horizon stands for (even if they simplified to nothing), bit 1 it has backdrop
                   chunks
  chunk records (64 B, map.c Chunk): tex -1, flags CHUNK_HZN (| CHUNK_BACKDROP: from the backdrop | draw-group set
                   << 16: dg.bin piece mask index, 0 = always drawn), tris, pos / st / col offsets, box, org, step
  streams: per chunk one VIF packet (chunk_streams: pos s16x3, colour u8x4; no st)
Each draw-group set of the cell's chunks (dg.bin, next to map.bin) is its own mesh, clustered and simplified apart, so
map.c draws only what the floor's groups show (HZN_DG=0: one mesh, set 0, the old file).
Env: HZN=0 writes an empty file (the old drawing); HZN_PX, HZN_KB, HZN_D, HZN_WELD, HZN_DG."""
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
# Screen error at the nearest the horizon is drawn (map.c HZN_R 56 m - hysteresis). ASSUMED 12 px: the coarse level's
# own cap is 24-36 px at 32 m (export_map WS_PX1), 14-20 px at 56 m; the horizon stays a little under that (8 px:
# Burg 19.6k tris a block, 12 px 15.8k, 16 px 13.5k).
PX = float(os.environ.get("HZN_PX", 12.0))
HZN_D = float(os.environ.get("HZN_D", 54.0))  # metres: nearest view distance the error is judged from
# cap per block (ASSUMED): lite sets share the near-cell arena with the near cells, relief and enemy models. At ~0.6 MB
# a block (1 MB for Firelink with its m8000 backdrop) mapstream_alloc held 4.4 MB of the 8 MB arena in the Burg and
# the near level waited up to 28 s (ISO route, 2026-11-06)
KB = float(os.environ.get("HZN_KB", 320))
PX_MAX = float(os.environ.get("HZN_PX_MAX", 32.0))  # the budget never pushes the error past this (ASSUMED)
BD_SHARE = 0.3  # of KB for the backdrop mesh at most
WELD = float(os.environ.get("HZN_WELD", 0.35))  # metres: the finest clustering grid (ASSUMED)
ST = os.environ.get("HZN_ST", "0") == "1"  # write the (zero) st stream too (A/B)
BPT = 36  # bytes per triangle: 3 x (6 + 4) stream bytes, + a record, VIF codes and padding per chunk (measured ~36)


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


_NEAREST = []


def _nearest_lib():
    """tools/native/libnearest.so (min_d2), built on first use like libgeom.so; None without a compiler (numpy path,
    same results) or with HZN_NATIVE=0"""
    if not _NEAREST:
        L = None
        if os.environ.get("HZN_NATIVE", "1") != "0":
            import ctypes
            native = os.path.join(HERE, "native")
            src, so = os.path.join(native, "nearest.c"), os.path.join(native, "libnearest.so")
            try:
                if not os.path.exists(so) or os.path.getmtime(so) < os.path.getmtime(src):
                    import subprocess, tempfile
                    fd, tmp = tempfile.mkstemp(suffix=".so", dir=native)
                    os.close(fd)
                    try:
                        subprocess.run([os.environ.get("CC", "cc"), "-O2", "-ffp-contract=off", "-fno-fast-math",
                                        "-shared", "-fPIC", "-o", tmp, src], check=True, capture_output=True)
                        os.replace(tmp, so)  # atomic: parallel exports may build at once
                    finally:
                        if os.path.exists(tmp):
                            os.remove(tmp)
                L = ctypes.CDLL(so)
                vp, i = ctypes.c_void_p, ctypes.c_int
                L.min_d2.argtypes = [i, vp, i, vp, ctypes.c_double, vp]
                L.min_d2.restype = None
            except Exception as e:
                print(f"horizon: C nearest distance unavailable ({str(e)[:200]}), using numpy", flush=True)
                L = None
        _NEAREST.append(L)
    return _NEAREST[0]


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


def simplify(V, T, nav, px, kb):
    """quadric collapse of the clustered block (flat stretches the grid cut up) -> surviving triangles (vertex
    triples), the screen error used in pixels: px, or more where the block would pass kb"""
    import simplify_native
    if not len(T):
        return T, px
    d = view_d(V, nav)
    log = simplify_native.simplify_log(V, np.arange(len(V), dtype=np.int32), T.astype(np.int32), None, 1.0 / (d * d))
    t0, cost, nrem = log[0], log[1], log[2]
    eps = (px / FOCAL) ** 2
    alive = len(t0) - np.cumsum(nrem)
    cap = int(kb * 1024 / BPT)
    k = int(np.searchsorted(cost, eps, side="right"))
    if (alive[k - 1] if k else len(t0)) > cap:
        under = np.flatnonzero(alive <= cap)
        eps = float(cost[under[0]]) if len(under) else float(cost[-1]) if len(cost) else eps
        eps = min(eps, (PX_MAX / FOCAL) ** 2)  # never coarser than PX_MAX: the budget gives way
    out = simplify_native.pysimplify.replay_log(log, eps)
    return out[:, 0::2], float(np.sqrt(eps) * FOCAL)


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
    groups, stats = [], dict(opaque=0, clustered=0, px=PX)
    left = KB
    for g in (1, 0):  # the backdrop first, at most BD_SHARE of the budget; the map gets the rest
        selg = keep & (bd == bool(g))
        budget = left * BD_SHARE if g else left
        tot = max(1, int(selg.sum()))
        pxs = []
        for dset in np.unique(dg[selg]):
            sel = selg & (dg == dset)
            V, VC, T = cluster(P[sel], C[sel], nav, PX)
            stats["opaque"] += int(sel.sum())
            stats["clustered"] += len(T)
            T, px = simplify(V, T, nav, PX, budget * sel.sum() / tot)  # the budget shared by triangle count
            left -= len(T) * BPT / 1024.0
            pxs.append(px)
            groups.append(_place(V, VC, T, lo, hi, CHUNK_HZN | (CHUNK_BACKDROP if g else 0) | int(dset) << 16))
        if not g and pxs:
            stats["px"] = max(pxs)
    recs, streams, ctab = [], bytearray(), []
    first = 0
    for ci in range(ncell):
        nchk, flags = 0, (1 if ci in cov else 0)
        cmn, cmx = np.full(3, 1e9), np.full(3, -1e9)
        for V, col, T, tc, cfl in groups:
            sel = np.flatnonzero(tc == ci)
            for s in range(0, len(sel), CHUNK_TRIS):
                t = T[sel[s:s + CHUNK_TRIS]]
                p = V[t].reshape(-1, 3)
                mn, mx = p.min(0), p.max(0)
                org = (mn + mx) / 2
                ext = np.abs(p - org).max()
                e = int(np.ceil(np.log2(max(ext, 1e-3) / POS_MAX)))
                step = 2.0 ** e
                qp = np.clip(np.rint((p - org) / step), -32767, 32767).astype(np.int16)
                pq = qp.astype(np.float64) * step + org  # the box of what is drawn
                mn, mx = pq.min(0), pq.max(0)
                cmn, cmx = np.minimum(cmn, mn), np.maximum(cmx, mx)
                c4 = np.concatenate([col[t.reshape(-1)], np.full((len(p), 1), 0x80, np.uint8)], 1)
                pk, offs = chunk_streams(len(t), qp.tobytes(), c4.tobytes())
                recs.append((cfl, len(t), len(streams), offs, mn, mx, org, step))
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
              px=stats["px"], covered=len(cov))
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
          f"({st['tris_opaque']} opaque, {st['tris_welded']} clustered) -> {st['tris']} tris in {st['chunks']} chunks over "
          f"{st['cells']} cells ({st['covered']} covered, {st['bd_tris']} backdrop tris, {st['sets']} draw-group meshes), {st['bytes'] >> 10} KB, {st['px']:.1f} px at {HZN_D:.0f} m")


if __name__ == "__main__":
    main()
