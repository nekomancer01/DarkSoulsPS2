"""The map exporter's VU1 chunks: streams, indexed batches, VIF packets, chunk building, foliage thinning (2026-11-12, code moved as it was; tools/export_map.py runs it)."""
import math, os, struct, sys
import souls
import texmap
import export_normal
import dsr_light
import drawgroups
import mapx_native
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapcfg
import mapgather


# (map.c culls by 32 m cells too), then cut into full, compact batches (split_batch)


def chunk_streams(ntri, pos, st, col, nv=0, idx=None):
    """A chunk's vertex streams as one VIF packet (render rebuild PR 1): a code qword in front of each stream
    {NOP, NOP, STCYCL 3,1, UNPACK V3-16 n -> TOPS+7} / {NOP x 3, UNPACK V2-16 unsigned n -> TOPS+8} / {NOP x 3,
    UNPACK V4-8 unsigned n -> TOPS+9}, then the stream zero-padded to 16 bytes (zeros = VIF NOPs). vu1.c sends the whole
    packet with one DMA REF (VU1_PKT); the stream offsets point past each code qword, so batches that send part of a
    chunk (foliage prefixes, clip splits) still REF the streams one by one. +48 bytes per chunk.
    Indexed chunks (nv vertices, idx: index_chunk): n = nv, the streams go to TOPS+8 / 9 / 10 (vu1mapi.vsm's header is
    8 qwords), then a fourth code qword {NOP x 2, STCYCL 1,1, UNPACK V4-8 unsigned ntri -> TOPS+8+3 nv} and the
    triangle stream (u8 x4 per triangle). +64 bytes per chunk.
    Returns (bytes, [pos, st, col offsets in it])."""
    n, a = (nv, MAPI_HDR) if nv else (3 * ntri, 7)
    assert 0 < n < 256, n
    codes = ((0, 0, 0x01000103, 0x69008000 | a | n << 16), (0, 0, 0, 0x6500C000 | (a + 1) | n << 16),
             (0, 0, 0, 0x6E00C000 | (a + 2) | n << 16))
    b, offs = bytearray(), []
    for arr, cw in zip((pos, st, col), codes):
        b += struct.pack("<4I", *cw)
        offs.append(len(b))
        b += arr
        b.extend(b"\0" * (-len(b) % 16))
    if nv:
        assert MAPI_HDR + 3 * nv + ntri <= MAPI_IN_QW
        b += struct.pack("<4I", 0, 0, 0x01000101, 0x6E00C000 | (MAPI_HDR + 3 * nv) | ntri << 16)
        b += idx
        b.extend(b"\0" * (-len(b) % 16))
    return b, offs


# Indexed chunks (vu1mapi.vsm, vu1.c vu1_add_idx): every chunk but foliage stores each distinct corner (position, st,
# colour) once plus a triangle stream of u8 x4 {v0, v1, v2, 0} (vertex numbers), like the shared vertices of Sony's HiG and
# other indexed PS2 renderers. Undead Burg west: 1.61 distinct corners per triangle, 45 -> ~28 bytes per triangle.
# INDEXED=0: lists as before (A/B)
INDEXED = os.environ.get("INDEXED", "1") == "1"
MAPI_HDR, MAPI_IN_QW = 8, 493  # tools/vu1/gen.py MAPI_HDR, MAP_IN_QW
CLIPI_MAX = 336  # tools/vu1/gen.py CLIPI_MAX: the most an indexed batch may take to be clipped on VU1 (vu1clip's indexed
                 # entry); bigger ones would be split on the EE near the camera (~1 ms a frame at the crossing), so they
                 # stay unindexed (little sharing anyway: about 2 vertices per triangle)


def index_chunk(ntri, pos, st, col):
    """Unindexed corner streams -> (nv, pos, st, col, idx) with the distinct corners in first-use order (the count padded
    to a multiple of 3 with copies of the last one: vu1mapi transforms three per loop pass), or None when the chunk
    does not fit vu1clip's indexed entry (CLIPI_MAX) or is no smaller."""
    n = 3 * ntri
    P = np.frombuffer(bytes(pos[:6 * n]), np.uint8).reshape(n, 6)
    S = np.frombuffer(bytes(st[:4 * n]), np.uint8).reshape(n, 4)
    C = np.frombuffer(bytes(col[:4 * n]), np.uint8).reshape(n, 4)
    K = np.ascontiguousarray(np.concatenate([P, S, C], 1)).view("V14").ravel()
    _, first, inv = np.unique(K, return_index=True, return_inverse=True)
    order = np.argsort(first, kind="stable")  # distinct corners in first-use order
    rank = np.empty(len(order), np.int64)
    rank[order] = np.arange(len(order))
    src = first[order]
    nv = -(-len(src) // 3) * 3
    if nv > 255 or MAPI_HDR + 3 * nv + ntri > CLIPI_MAX:
        return None
    src = np.concatenate([src, np.repeat(src[-1:], nv - len(src))])
    tri = rank[inv.ravel()].reshape(ntri, 3)
    idx = np.zeros((ntri, 4), np.uint8)
    idx[:, :3] = tri
    pad = lambda k: -(-k // 16) * 16
    if pad(6 * nv) + 2 * pad(4 * nv) + pad(4 * ntri) + 16 >= pad(6 * n) + 2 * pad(4 * n):
        return None
    return nv, P[src].tobytes(), S[src].tobytes(), C[src].tobytes(), idx.tobytes()



def chunk_packet(ti, fl, ntri, pos, st, col):
    """chunk_streams of one chunk tuple, indexed when it pays (index_chunk; never foliage) ->
    (bytes, offsets, ntri word for the chunk record: ntri | nv << 16)"""
    ix = index_chunk(ntri, pos, st, col) if INDEXED and not fl & CHUNK_GRASS else None
    if os.environ.get("CHUNK_STATS"):  # debugging: per chunk a line "texture flags tris P PS PC PSC nv bytes" (distinct
        n = 3 * ntri                   # corners by position, + st, + colour, + both; indexed vertices; packet bytes)
        P, S, C = (np.frombuffer(bytes(x[:w * n]), np.uint8).reshape(n, w) for x, w in ((pos, 6), (st, 4), (col, 4)))
        u = lambda *a: len(np.unique(np.ascontiguousarray(np.concatenate(a, 1)), axis=0))
        nb = len(chunk_streams(ntri, pos, st, col)[0] if ix is None else chunk_streams(ntri, *ix[1:4], ix[0], ix[4])[0])
        with open(os.environ["CHUNK_STATS"], "a") as f:
            f.write(f"{ti} {fl} {ntri} {u(P)} {u(P, S)} {u(P, C)} {u(P, S, C)} {0 if ix is None else ix[0]} {nb}\n")
    if ix is None:
        pk, offs = chunk_streams(ntri, pos, st, col)
        return pk, offs, ntri
    nv, p2, s2, c2, idx = ix
    pk, offs = chunk_streams(ntri, p2, s2, c2, nv, idx)
    return pk, offs, ntri | nv << 16


POS_MAX = 32000  # s16 position range used before snapping (leaves room for split_batch / snapping)
GRASS_D0 = float(os.environ.get("GRASS_D0", 10.0))
# STREAM: ground foliage tufts ranked above the density map.c draws at GRASS_NEAR_D (map.c NEAR_DRAW_R - NEAR_HYST, where
# a cell's near blob is drawn at the latest) go into their cell's near blob, the rest (and canopy, rank 0) stay resident
GRASS_NEAR_D = float(os.environ.get("GRASS_NEAR_D", 22.0))  # metres: full exported foliage density up to here, then (D0 / d)^2 (map.c)
CAM_REACH = 6.0  # metres: how far the camera gets from the walkable navmesh (4 m arm, pivot 1.4 m up, falls)
GRASS_GRID = 2.0  # metres: foliage cell (by tuft centre); map.c picks the density per chunk, so cells stay small
CHUNK_GRASS = 16  # foliage chunk: flags bits 8-15 triangles per tuft, 16-23 / 24-31 lowest / highest rank x 255


def rss(stage):
    """Peak / current RSS at an export stage (the 24 GB Undead Burg run, 2026-10-31: find what grows)"""
    if sys.platform == "emscripten":  # in-browser converter: the wasm heap (it never shrinks, 4 GB at most)
        import js
        if hasattr(js, "heapMB"):
            print(f"mem {stage}: wasm heap {js.heapMB():.0f} MB", flush=True)
            return
    st = dict(l.split(":", 1) for l in open("/proc/self/status") if l.startswith(("VmRSS", "VmHWM")))
    print(f"mem {stage}: rss {int(st['VmRSS'].split()[0]) >> 10} MB, peak {int(st['VmHWM'].split()[0]) >> 10} MB",
          flush=True)


# near texture level (texmap.add_near, texcache level 2): twice mip 0's size for the textures named in TEX_NEAR
# (comma list, "all" = every opaque texture), drawn on near chunks; TECH_DEBT map-2 / texstream-5
TEX_NEAR = set(os.environ.get("TEX_NEAR", "").split(",")) - {""}


def near_level(t, dds, name):
    """Add the near level to texture record t from its DDS when TEX_NEAR asks for it."""
    if "all" in TEX_NEAR or name in TEX_NEAR:
        dec = souls.dds_decode_rgba(dds, 2 * mapcfg.TEX_MAX)
        if dec and dec[0] > t["w"]:  # only when the source has more detail than mip 0
            texmap.add_near(t, *dec)


def split_batch(ks, cents):
    """Triangles of one (texture, flags, cell) group -> ceil(n / CHUNK_TRIS) batches of nearly equal size, by recursive
    median cuts along the longest axis of the centroids, so every batch is full and its box compact (F2).
    ks: triangle numbers (int array), cents: centroid per triangle number (array (n, 3))."""
    k = -(-len(ks) // mapcfg.CHUNK_TRIS)
    if k <= 1:
        return [ks]
    c = cents[ks]
    ext = c.max(0) - c.min(0)
    ax = int(np.argmax(ext))  # first longest axis
    ks = ks[np.argsort(c[:, ax], kind="stable")]
    k1 = k // 2
    cut = max(len(ks) - (k - k1) * mapcfg.CHUNK_TRIS, min(k1 * mapcfg.CHUNK_TRIS, round(len(ks) * k1 / k)))
    return split_batch(ks[:cut], cents) + split_batch(ks[cut:], cents)


def pos_scale(lo, hi, org):
    """Smallest power-of-two step (<= 1 m) that puts every offset from the chunk origin in s16 range."""
    m = max(max(abs(hi[a] - org[a]), abs(lo[a] - org[a])) for a in range(3))
    e = -16
    while m / 2.0 ** e > POS_MAX and e < 0:
        e += 1
    return e


CHUNK_TAG = []  # build_chunks: (level, cell) per chunk (STREAM), else None
CHUNK_DG = []  # build_chunks: draw-group set index per chunk (tools/drawgroups.py, dg.bin)
CHUNK_PART = []  # build_chunks_c (PVS): (MSB part name, occluder) per chunk
CHUNK_SRC = []  # build_chunks_c (PVS): per chunk the indices of its triangles in PVS_TRIS
PVS_TRIS = [None]  # build_chunks_c (PVS): float32 (triangles, 3, 3) world positions of every input triangle
PVS_ORDER = []  # STREAM + PVS: per chunk in map.bin / dg.bin order (index in build order, (cell key, 0 resident / 1 near) or None)
CHUNK_ENT = []  # build_chunks: MSB entity id per chunk (0 = none): the part an event switches on / off (dg.bin DGM3)
# TECH_DEBT event-5: pieces / objects whose entity id the map's events switch ((De)activate Object, Activate Map Part,
# tools/emevd.switched_entities) never share a chunk with anything else, so src/drawgrp.c can hide exactly them
EVENT_PARTS = os.environ.get("EVENT_PARTS", "1") == "1"
DG = drawgroups.Table()  # the area's piece draw-group sets (gather: one per mesh)


_py_sum3 = dsr_light.sum3  # Python's sum() of three floats, per element


def _first_ext(v, X, cs):
    """min / max per batch from ufunc.reduceat (v) with the sign of a zero result taken from the first zero in the
    batch, as Python's min() / max() keep the first of equal values"""
    z = np.nonzero(v == 0)
    if len(z[0]):
        v = v.copy()
        for b, a in zip(*(i.tolist() for i in z)):
            s = cs[b]
            e = cs[b + 1] if b + 1 < len(cs) else len(X)
            col = X[s:e, a]
            v[b, a] = col[np.argmax(col == 0)]
    return v


def build_chunks(meshes, verts, tris, uvs, textures, centre, fol, bkey=None, tcell=None, dgk=None, entk=None):
    """Triangles -> VU1 batches: (texture, flags, tri count, pos bytes, st bytes, colour bytes, min, max, origin, pos
    step, st scale), grouped across meshes by (texture, cull flag, CHUNK_GRID cell of the centroid) and cut into full,
    compact batches (split_batch). Foliage (fol[k] = (rank, tuft, tuft size), else None): whole tufts grouped by
    (texture, flags, GRASS_GRID cell of the tuft centre, tuft size), sorted by rank, so a chunk is one density layer of
    its cell and map.c draws a prefix.
    Unindexed quantised streams (R3): positions s16 x3 (V3-16) in steps of 2^e m from an integer-metre chunk origin;
    a vertex shared by several chunks is first snapped to the coarsest of their steps, so every chunk decodes it to the
    same point (no cracks); texture coords u16 x2 (V2-16) in units of 1 / st scale (4096, or 1024 when a chunk has
    coordinates >= 16); colour u8 x4 (V4-8). bkey: {triangle: normal-map key} (export_normal.assign): triangles
    with different keys never share a chunk; export_normal.CHUNK_BUMP gets each chunk's key. tcell (STREAM): per
    triangle (level, cell) or None: such triangles are grouped by that CHUNK_GRID cell (no coarser far cells) and
    level; CHUNK_TAG gets each chunk's (level, cell) or None. dgk / entk: per triangle draw-group set / switched
    entity id (CHUNK_DG / CHUNK_ENT per chunk); triangles with different ones never share a chunk."""
    # vectorised (was 26 s of pure Python per corner at Firelink): every value is computed with the same float
    # operations in the same order as the per-corner code was, so map.bin is unchanged (see _py_sum3, _first_ext)
    nt = len(tris)
    T = np.asarray(tris, np.int64).reshape(-1, 3)
    fvt = np.zeros(nt, np.int64)
    for ti, fv, vc, ft, tc, fl, lo, hi in meshes:
        fvt[ft:ft + tc] = fv
    G = T + fvt[:, None]  # global vertex per corner
    P = np.array([v[0] for v in verts], np.float64).reshape(-1, 3)
    exact = all(type(x) is float for v in verts for x in v[0])
    if exact:
        X = P[G]
        C = _py_sum3(X[:, 0], X[:, 1], X[:, 2]) / 3
    else:  # positions that are not plain floats: Python's own sum (other summation rules)
        C = np.array([[sum(verts[fvt[k] + i][0][a] for i in tris[k]) / 3 for a in range(3)] for k in range(nt)],
                     np.float64).reshape(-1, 3)
    cl = C.tolist()
    # distance-scaled CHUNK_GRID cell of each centroid: floor(log2(max(1, dist / 4 grid))) from the float's exponent;
    # values within 1e-9 of a power of two are redone with math.log2 as before (its rounding decides there)
    D = C - np.asarray(centre, np.float64)
    r = np.sqrt(_py_sum3(D[:, 0] ** 2, D[:, 1] ** 2, D[:, 2] ** 2)) / (4 * mapcfg.CHUNK_GRID)
    mant, ex = np.frexp(r)
    lvl = np.where(r >= 1.0, ex - 1, 0)
    for k in np.nonzero((r >= 1.0) & ((np.abs(mant - 0.5) < 1e-9) | (np.abs(mant - 1.0) < 1e-9)))[0].tolist():
        c = cl[k]
        dist = math.sqrt(sum((c[a] - centre[a]) ** 2 for a in range(3)))
        lvl[k] = max(0, int(math.log2(max(1.0, dist / (4 * mapcfg.CHUNK_GRID)))))
    gq = (mapcfg.CHUNK_GRID * np.ldexp(1.0, lvl)).tolist()
    fq = np.floor(C / np.asarray(gq)[:, None]).astype(np.int64).tolist()
    groups, tufts = {}, {}
    for ti, fv, vc, ft, tc, fl, lo, hi in meshes:
        for k in range(ft, ft + tc):
            c = cl[k]
            if fol[k]:
                t = tufts.setdefault(fol[k][1], [ti, fl, fol[k][0], fol[k][2], [0.0] * 3, [], dgk[k] if dgk else 0,
                                                 entk[k] if entk else 0])
                t[4] = [t[4][a] + c[a] for a in range(3)]
                t[5].append((fv, k))
                continue
            if fl & mapcfg.CHUNK_SKY:  # sky: 8 azimuth sectors x above / below the horizon, so batches stay full
                cell = ("sky", int((math.atan2(c[0] - centre[0], c[2] - centre[2]) + math.pi) / (math.pi / 4)) % 8,
                        c[1] > centre[1])
            elif tcell and tcell[k] is not None:
                cell = ("s", tcell[k][0]) + tuple(tcell[k][1])
            else:  # coarser far away
                cell = (gq[k],) + tuple(fq[k])
            # dgk: pieces with different draw groups never share a chunk (src/drawgrp.c hides chunks per group)
            cell += (dgk[k] if dgk else 0, entk[k] if entk else 0)
            # the coarse level (STREAM level 0) is drawn beyond the relief range: no normal-map split, fuller batches
            bk = bkey.get(k) if bkey and not (tcell and tcell[k] is not None and tcell[k][0] != 1) else None
            groups.setdefault((ti, fl, cell, bk), []).append((fv, k))
    groups = export_normal.merge_small(groups)
    batches = []
    export_normal.CHUNK_BUMP.clear()
    CHUNK_TAG.clear()
    CHUNK_DG.clear()
    CHUNK_ENT.clear()
    for (ti, fl, cell, bk), ks in groups.items():
        for part in split_batch(np.array([k for _, k in ks], np.int64), C):
            part = part.tolist()
            batches.append((ti, fl, part))
            export_normal.CHUNK_BUMP.append((bk, part) if bk else None)
            CHUNK_TAG.append((cell[1], cell[2:-2]) if cell[0] == "s" else None)
            CHUNK_DG.append(cell[-2])
            CHUNK_ENT.append(cell[-1])
    fgroups = {}
    near_r = min(1.0, (GRASS_D0 / GRASS_NEAR_D) ** 2)
    for ti, fl, r, n, c, ks, dg, ent in tufts.values():
        cell = tuple(int(math.floor(c[a] / len(ks) / GRASS_GRID)) for a in range(3)) + (dg, ent)
        # STREAM: (CHUNK_GRID cell of the tuft centre) for tufts only drawn nearer than GRASS_NEAR_D, else resident
        sc = tuple(int(math.floor(c[a] / len(ks) / mapcfg.CHUNK_GRID)) for a in range(3)) if tcell and r >= near_r else None
        fgroups.setdefault((ti, fl, n, cell, sc), []).append((r, ks))
    for (ti, fl, n, cell, sc), ts in fgroups.items():
        ts.sort(key=lambda t: t[0])
        per = max(1, mapcfg.CHUNK_TRIS // n)
        for i in range(0, len(ts), per):
            part = ts[i:i + per]
            lo, hi = int(part[0][0] * 255), min(255, math.ceil(part[-1][0] * 255))
            batches.append((ti, fl | CHUNK_GRASS | min(n, 255) << 8 | lo << 16 | hi << 24,
                            [k for _, ks in part for _, k in ks]))
            export_normal.CHUNK_BUMP.append(None)
            CHUNK_TAG.append((1, sc) if sc else None)
            CHUNK_DG.append(cell[-2])
            CHUNK_ENT.append(cell[-1])
    # every corner of every batch, in batch order: triangle, global vertex, position
    nb = len(batches)
    tb = [len(part) for _, _, part in batches]
    K = np.fromiter((k for _, _, part in batches for k in part), np.int64, sum(tb))
    # np.intp: repeat / reduceat take counts and indices only as the platform's index type (int32 in the in-browser
    # converter's 32-bit numpy, which refuses int64 there; int64 natively, so the same arrays)
    cb = np.repeat(np.arange(nb), np.asarray(tb, np.intp) * 3)  # batch of each corner
    cs = np.concatenate([[0], np.cumsum(tb)[:-1]]).astype(np.intp) * 3  # first corner of each batch
    CG = G[K].reshape(-1)
    PC = P[CG]
    # chunk boxes (the first of equal values wins, as min() / max() did: the sign of a zero), origins and steps
    MN = _first_ext(np.minimum.reduceat(PC, cs, 0), PC, cs)
    MX = _first_ext(np.maximum.reduceat(PC, cs, 0), PC, cs)
    ORG = np.rint((MN + MX) / 2) + 0.0  # float(round(x)): no negative zero
    mnl, mxl, orgl = MN.tolist(), MX.tolist(), ORG.tolist()
    E = np.array([pos_scale(mnl[b], mxl[b], orgl[b]) for b in range(nb)], np.int64)
    # then the step of each position = the coarsest among the chunks using it (positions keyed by value, as before)
    pk = {}
    pid = np.fromiter((pk.setdefault(v[0], len(pk)) for v in verts), np.int64, len(verts))
    cp = pid[CG]
    while True:
        S = np.full(len(pk), -99, np.int64)
        np.maximum.at(S, cp, E[cb])
        sv = np.ldexp(1.0, S[cp])[:, None]
        Q = np.rint(PC / sv) * sv
        m = np.maximum.reduceat(np.abs(Q - ORG[cb]).max(1), cs) if nb else np.zeros(0)
        grow = m / np.ldexp(1.0, E) > 32767  # snapping moved a vertex out of range: coarser step for this chunk
        if not grow.any():
            break
        E[grow] += 1
    sc = np.ldexp(1.0, E)[cb][:, None]
    NQ = np.rint((Q - ORG[cb]) / sc)
    err = float(np.abs(ORG[cb] + NQ * sc - PC).max()) if len(PC) else 0.0
    assert len(NQ) == 0 or (NQ.min() >= -32768 and NQ.max() <= 32767), "chunk position out of s16 range"
    pos16 = NQ.astype("<i2")
    # texture coordinates: 1/4096 units, or 1/1024 when a chunk has coordinates >= 65535 / 4096
    tws = np.array([(textures[ti][0], textures[ti][1]) if ti >= 0 else (1, 1) for ti, _, _ in batches],
                   np.float64).reshape(-1, 2)
    UV = np.array(uvs, np.float64).reshape(-1, 3, 2)[K].reshape(-1, 2) / tws[cb]
    smax = np.maximum.reduceat(UV.max(1), cs) if nb else np.zeros(0)
    sts = np.where(smax < 65535 / 4096, 4096, 1024)
    ST = np.minimum(65535, np.rint(UV * sts[cb][:, None]))
    assert len(ST) == 0 or ST.min() >= 0, "negative texture coordinate"
    st16 = ST.astype("<u2")
    rgb = np.array([v[1] for v in verts], np.int64).reshape(-1, 3)
    al = np.fromiter((v[2] for v in verts), np.int64, len(verts))
    col = np.concatenate([rgb, al[:, None]], 1)[CG]
    assert len(col) == 0 or (col.min() >= 0 and col.max() <= 255), "vertex colour out of byte range"
    col8 = col.astype(np.uint8)
    out = []
    pad = lambda b: b + b"\0" * (-len(b) % 16)
    ce = np.cumsum(np.asarray(tb, np.int64) * 3).tolist()
    for b, (ti, fl, part) in enumerate(batches):
        s, e = cs[b], ce[b]
        out.append((ti, fl, len(part), pad(pos16[s:e].tobytes()), pad(st16[s:e].tobytes()), pad(col8[s:e].tobytes()),
                    mnl[b], mxl[b], orgl[b], 2.0 ** int(E[b]), int(sts[b])))
    print(f"chunks: {len(out)}, {sum(o[2] for o in out) / max(1, len(out)):.1f} tris / chunk, largest position "
          f"error {err * 1000:.2f} mm")
    return out


def flatten(entries, textures):
    """the drawn entries -> build_chunks' inputs: per entry (texture, first vertex, vertices, first triangle, triangles,
    flags, box), vertices (position, rgb, alpha) of its distinct corners (position, wedge) in sorted order, triangles
    in those, texture coordinates per corner (in texels, from the integer below the triangle's smallest), and per
    triangle foliage, stream cell, draw group, entity"""
    meshes, verts, tris, uvs, fol, tcells, dgs, ents = [], [], [], [], [], [], [], []
    for m in entries:
        tl = m["tris"]
        if not tl:
            continue
        tl = list(tl)  # a mapx_native.Tris: its corner triples
        ti = m["ti"]
        used = sorted({c for t in tl for c in t})  # corners (position, wedge)
        remap = {c: k for k, c in enumerate(used)}
        base = len(verts)
        lo, hi = [1e9] * 3, [-1e9] * 3
        wrgb = m["wrgb"].tolist() if isinstance(m["wrgb"], np.ndarray) else m["wrgb"]  # soften_thin_c's array
        for p, a in used:
            verts.append((p, wrgb[a], m["wa"][a]))
            for j in range(3):
                lo[j], hi[j] = min(lo[j], p[j]), max(hi[j], p[j])
        first_tri = len(tris)
        fol += m["fol"] if m["fol"] else [None] * len(tl)
        tcells += m["tcell"] if m["tcell"] else [None] * len(tl)
        dgs += [m["dg"]] * len(tl)
        ents += [m["ent"]] * len(tl)
        for t in tl:
            tris.append(tuple(remap[c] for c in t))
            if ti >= 0:
                tw, th = textures[ti][0], textures[ti][1]
                uv = [m["wuv"][a] for _, a in t]
                du, dv = math.floor(min(u for u, _ in uv)), math.floor(min(v for _, v in uv))
                uvs.append([((u - du) * tw, (v - dv) * th) for u, v in uv])
            else:
                uvs.append([(0, 0)] * 3)
        meshes.append((ti, base, len(used), first_tri, len(tl), m["flags"], lo, hi))
    return meshes, verts, tris, uvs, fol, tcells, dgs, ents


def build_chunks_c(MX, entries, textures, centre, bkey, stream, draw_groups):
    """flatten + build_chunks in C (tools/mapx/chunks.c): the same chunks, CHUNK_BUMP / TAG / DG / ENT; also returns
    the number of drawn entries and triangles; None when a corner position is not a plain float (build_chunks then
    sums them with Python's rules)"""
    import ctypes
    es = [m for m in entries if m["tris"]]
    i32, i64, f64 = np.int32, np.int64, np.float64
    ne = len(es)
    nts = [len(m["tris"]) for m in es]
    toff = np.concatenate([[0], np.cumsum(nts)]).astype(i64) if ne else np.zeros(1, i64)
    nt = int(toff[-1])
    cps, cws = [np.zeros(0)], [np.zeros(0, i32)]
    for m in es:
        tl = m["tris"]
        if isinstance(tl, mapx_native.Tris):  # float64 from simplify_meshes_c
            cps.append(tl.P.reshape(-1))
            cws.append(tl.W.reshape(-1))
            continue
        cp = [x for t in tl for c in t for x in c[0]]
        if any(t is not float for t in set(map(type, cp))):
            return None
        cps.append(np.array(cp, f64))
        cws.append(np.array([c[1] for t in tl for c in t], i32))
    cpos, cw = np.concatenate(cps), np.concatenate(cws).astype(i32)
    wt, woff, wrgb, wa, wuv, w0 = {}, [], [], [], [], 0
    for m in es:  # wedge tables, once per mesh (its entries share them)
        k = (id(m["wrgb"]), id(m["wa"]), id(m["wuv"]))
        if k not in wt:
            wt[k] = w0
            wrgb.append(mapgather._np_list(m, "wrgb", m["wrgb"], i64, 3))
            wa.append(mapgather._np_list(m, "wa", m["wa"], i64, 1))
            wuv.append(mapgather._np_list(m, "wuv", m["wuv"], f64, 2))
            w0 += len(m["wa"])
        woff.append(wt[k])
    woff = np.array(woff + [w0], i64)
    cat = lambda xs, dt, w: np.ascontiguousarray(np.concatenate(xs).astype(dt)) if xs else np.zeros((0, w), dt)
    wrgb, wa, wuv = cat(wrgb, i32, 3), cat(wa, i32, 1), cat(wuv, f64, 2)
    if (wrgb.size and (wrgb.min() < 0 or wrgb.max() > 255)) or (wa.size and (wa.min() < 0 or wa.max() > 255)):
        raise AssertionError("vertex colour out of byte range")
    tex = np.array([(t[0], t[1]) for t in textures] or [(1, 1)], i32)
    tuft, frank, fsize = np.full(nt, -1, i64), np.zeros(nt), np.zeros(nt, i32)
    tlev, tkey = np.full(nt, -1, i32), np.zeros((nt, 3), i64)
    dgv, entv, bkv = np.zeros(nt, i32), np.zeros(nt, i32), np.full(nt, -1, i32)
    tids, dids, eids, bids = {}, {}, {}, {}
    tl_ = toff.tolist()
    for e, m in enumerate(es):  # per triangle values set a mesh entry at a time
        a, b = tl_[e], tl_[e + 1]
        if m["fol"]:
            fs = [(k, f) for k, f in zip(range(a, b), m["fol"]) if f]
            if fs:
                sel = np.array([k for k, _ in fs], np.intp)
                tuft[sel] = [tids.setdefault(f[1], len(tids)) for _, f in fs]
                frank[sel] = [f[0] for _, f in fs]
                fsize[sel] = [f[2] for _, f in fs]
        if stream and m["tcell"]:
            cs = [(k, c) for k, c in zip(range(a, b), m["tcell"]) if c is not None]
            if cs:
                sel = np.array([k for k, _ in cs], np.intp)
                tlev[sel] = [c[0] for _, c in cs]
                tkey[sel] = [c[1] for _, c in cs]
        dgv[a:b] = dids.setdefault((m["dg"] if draw_groups else 0, m.get("piece", "") if mapcfg.PVS else "",
                                    m.get("occ", 0) if mapcfg.PVS else 0), len(dids))
        entv[a:b] = eids.setdefault(m["ent"], len(eids))
    if isinstance(bkey, export_normal.BumpKeys) and bkey.unique():  # the same numbering from the arrays
        sel = bkey.G < nt
        if sel.any():
            ids, first = mapgather._first_rows(np.stack([bkey.TI[sel], bkey.LAB[sel]], 1))
            bkv[bkey.G[sel]] = ids
            bids = dict.fromkeys(zip(bkey.TI[sel][first].tolist(), bkey.LAB[sel][first].tolist()))
    elif bkey:
        bs = [(k, bids.setdefault(v, len(bids))) for k, v in bkey.items() if k < nt]
        if bs:
            bkv[np.array([k for k, _ in bs], np.intp)] = [i for _, i in bs]
    ti = np.array([m["ti"] for m in es] or [0], i32)
    fl = np.array([m["flags"] for m in es] or [0], i64)
    ce = np.ascontiguousarray(np.array(centre, f64).reshape(3))
    arrs = [ti, fl, toff, woff, cpos, cw, wrgb, wa, wuv, tex, tuft, frank, fsize, tlev, tkey, dgv, entv, bkv, ce]
    arrs = [np.ascontiguousarray(x) for x in arrs]
    MX.mx_chunks_st_align(1 if mapcfg.ST_ALIGN else 0)
    h = MX.mx_chunks_new(ne, *[x.ctypes.data for x in arrs[:15]], arrs[15].ctypes.data, arrs[16].ctypes.data,
                         arrs[17].ctypes.data, 1 if stream else 0, mapcfg.CHUNK_GRID, arrs[18].ctypes.data, GRASS_GRID,
                         min(1.0, (GRASS_D0 / GRASS_NEAR_D) ** 2), mapcfg.CHUNK_TRIS, export_normal.BUMP_MIN_GROUP, POS_MAX,
                         mapcfg.CHUNK_SKY, CHUNK_GRASS)
    if not h:
        raise MemoryError("mx_chunks_new")
    try:
        cnt = np.zeros(4, i64)
        nb = MX.mx_chunks_count(h, cnt.ctypes.data)
        if nb < 0:
            return None  # a value out of range: the Python code asserts
        iv, flv, tag = np.zeros((max(nb, 1), 9), i32), np.zeros(max(nb, 1), i64), np.zeros((max(nb, 1), 4), i64)
        box, offs, bt = np.zeros((max(nb, 1), 9)), np.zeros((nb + 1, 3), i64), np.zeros(max(nt, 1), i64)
        pos, st, col = (np.zeros(max(int(n), 1), np.uint8) for n in cnt[1:])
        err = ctypes.c_double()
        MX.mx_chunks_fetch(h, iv.ctypes.data, flv.ctypes.data, tag.ctypes.data, box.ctypes.data, offs.ctypes.data,
                           bt.ctypes.data, pos.ctypes.data, st.ctypes.data, col.ctypes.data, ctypes.byref(err))
    finally:
        MX.mx_chunks_free(h)
    dgl, entl, bkl = list(dids), list(eids), list(bids)
    pos, st, col = pos.tobytes(), st.tobytes(), col.tobytes()
    out = []
    export_normal.CHUNK_BUMP.clear()
    CHUNK_TAG.clear()
    CHUNK_DG.clear()
    CHUNK_ENT.clear()
    CHUNK_PART.clear()
    CHUNK_SRC.clear()
    PVS_TRIS[0] = cpos.astype(np.float32).reshape(-1, 3, 3) if mapcfg.PVS and len(cpos) == 9 * nt else None
    btl = bt.tolist()
    for b, (v, f, tg, bx, o0, o1) in enumerate(zip(iv.tolist(), flv.tolist(), tag.tolist(), box.tolist(),
                                                    offs[:-1].tolist(), offs[1:].tolist())):
        t_i, ntri, kind, dg, ent, bk, e, sts, t0 = v
        out.append((t_i, f, ntri, pos[o0[0]:o1[0]], st[o0[1]:o1[1]], col[o0[2]:o1[2]], bx[0:3], bx[3:6], bx[6:9],
                    2.0 ** e, sts))
        export_normal.CHUNK_BUMP.append((bkl[bk], btl[t0:t0 + ntri]) if bk >= 0 else None)
        CHUNK_TAG.append((tg[0], tuple(tg[1:])) if kind in (1, 2) else None)
        CHUNK_DG.append(dgl[dg][0])
        CHUNK_ENT.append(entl[ent])
        CHUNK_PART.append(dgl[dg][1:])
        CHUNK_SRC.append(bt[t0:t0 + ntri])
    print(f"chunks: {len(out)}, {sum(o[2] for o in out) / max(1, len(out)):.1f} tris / chunk, largest position "
          f"error {err.value * 1000:.2f} mm")
    return out, ne, nt


def name_is_grass(name):
    # m10_01_grass_ground_* is solid ground (Burg floors, ~1k tris): thinned as tufts it left holes under the player
    # once the draw-group filter hid the stand-in floor beneath (m2250B1 over m2420B1, autotest point 25)
    return bool(name) and ("grass" in name or "fern" in name) and "ground" not in name


CANOPY_H = float(os.environ.get("CANOPY_H", 2.5))  # metres above the walkable ground: foliage up there is canopy / ivy


def thin_foliage_c(L, tl, pos, seed, ivy, world, grid):
    """thin_foliage in C (tools/mapx/foliage.c, libmapx): the same triangles, ranks, tuft names and positions. The
    canopy test is gather's is_canopy on world (the mesh's vertices placed with xf.many, the same values as xf) against
    the navmesh grid (mx_navgrid; None when the area has no navmesh points: always canopy)."""
    import ctypes
    T = np.ascontiguousarray(np.asarray(tl, np.int32).reshape(-1, 3))
    P = np.array(mapgather._vert_np(pos), np.float64).reshape(-1, 3)  # a copy: moved positions are written into it
    Wc = np.ascontiguousarray(world, np.float64)
    nt, nv = len(T), len(P)
    moved = np.zeros(nv, np.uint8)
    out_t, root, size = (np.empty(max(1, nt), np.int32) for _ in range(3))
    rank = np.empty(max(1, nt))
    cn = ctypes.c_int()
    n = L.mx_thin_foliage(nt, T.ctypes.data, nv, P.ctypes.data, moved.ctypes.data, Wc.ctypes.data, grid, 1, CANOPY_H,
                          seed.encode(), int(bool(ivy)), mapcfg.GRASS_KEEP, mapcfg.FOL_XZ, mapcfg.FOL_Y, mapcfg.FOL_IVY, out_t.ctypes.data,
                          rank.ctypes.data, root.ctypes.data, size.ctypes.data, ctypes.byref(cn))
    CANOPY_STATS[0] += cn.value
    out = [tl[k] for k in out_t[:n].tolist()]
    fol = [(r, f"{seed}/{c}", z) for r, c, z in zip(rank[:n].tolist(), root[:n].tolist(), size[:n].tolist())]
    pos = list(pos)
    for i in np.nonzero(moved)[0].tolist():
        pos[i] = tuple(P[i].tolist())
    return out, fol, pos


def thin_foliage(tl, pos, seed, ivy, canopy=None):
    """Foliage cards / tufts (triangles connected by shared positions; 2-32 triangles, often back-to-back pairs) ->
    (kept triangles, [(rank, component, size)] per kept triangle, positions). Each component gets a stable random u in
    [0, 1); it is kept if u < 1 / GRASS_KEEP and its rank is u * GRASS_KEEP (uniform in [0, 1)). The PS2 draws the
    components whose rank is below the density for their distance (map.c), so thinning never splits a tuft or a
    two-sided card. Kept tufts are widened to make up for the missing ones (FOL_XZ sideways and FOL_Y up about the
    tuft's base), so the ground stays about as covered as in DSR. Ivy leaves (single quads on walls) are thinned half
    as much (keep 1 in GRASS_KEEP / 2) and scaled FOL_IVY about their centre, as bigger leaves stand out.
    canopy(vertex indices) -> True for tree canopy / wall foliage (above the walkable ground, render-distance): every
    such component is kept, unscaled, with rank 0, so map.c never thins it with distance (a thinned canopy showed two
    leaf clusters on Firelink's big tree)."""
    import hashlib
    key = [(round(p[0] * 1e4), round(p[1] * 1e4), round(p[2] * 1e4)) for p in pos]
    par = list(range(len(tl)))

    def root(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x
    first = {}
    for k, t in enumerate(tl):
        for i in t:
            if key[i] in first:
                par[root(k)] = root(first[key[i]])
            else:
                first[key[i]] = k
    comps = {}
    for k in range(len(tl)):
        comps.setdefault(root(k), []).append(k)
    out, fol, pos = [], [], list(pos)
    keep = max(1, (mapcfg.GRASS_KEEP + 1) // 2) if ivy else mapcfg.GRASS_KEEP
    sxz, sy = (mapcfg.FOL_IVY, mapcfg.FOL_IVY) if ivy else (mapcfg.FOL_XZ, mapcfg.FOL_Y)
    for c, ks in comps.items():
        u = int.from_bytes(hashlib.md5(f"{seed}/{c}".encode()).digest()[:4], "little") / 2 ** 32
        vs = sorted({i for k in ks for i in tl[k]})
        if canopy and canopy(vs):
            CANOPY_STATS[0] += len(ks)
            out.extend(tl[k] for k in ks)
            fol.extend((0.0, f"{seed}/{c}", len(ks)) for _ in ks)
            continue
        if u * keep >= 1:
            continue
        o = [sum(pos[i][a] for i in vs) / len(vs) for a in range(3)]
        if not ivy:
            o[1] = min(pos[i][1] for i in vs)
        for i in vs:
            p = pos[i]
            pos[i] = (o[0] + (p[0] - o[0]) * sxz, o[1] + (p[1] - o[1]) * sy, o[2] + (p[2] - o[2]) * sxz)
        for k in ks:
            out.append(tl[k])
            fol.append((u * keep, f"{seed}/{c}", len(ks)))
    return out, fol, pos


CANOPY_STATS = [0]
