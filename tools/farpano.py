"""Far panorama of a world block: the other blocks of its area as a coarse coloured mesh of what can be seen of them
from this block (src/world.c, src/mapblock.c mp_draw_far).

  farpano.py <far.bin> <this block's gen dir> <gen dir of world block 0> <gen dir of block 1> ...

(the gen dirs of every block of the world in world.bin's order, the boot map first, this block's among them)

DSR keeps every map of an area load list loaded and draws all of them (Firelink Shrine's great tree and cliffs from
the Undead Burg, the Burg's towers from Firelink). The port has room for the blocks around the player only: a block
that is not loaded showed nothing, and the boot block (Firelink) stayed loaded for good so that its horizon could be
drawn from the Burg (1.4 MB of map tables and up to 17k triangles a frame, 2026-11-13). The far panorama stands in for
both: with it world.c lets go of the boot block's map tables like any other block's, and a block that is off is
still seen.

Built from the other blocks' horizons (hzn.bin, tools/horizon.py: untextured, colours baked), so it needs no export:
  1. peers: the blocks whose dg.bin names the same area group as this block's (tools/drawgroups.py: the maps of one
     area load list share a draw-group bit space); a block without one (the Undead Asylum) has no panorama. A block of
     the same MSB counts only when part of this block's ground lies beyond the distance world.bin keeps it loaded;
  2. each peer's horizon pieces are grouped by how this block's collision parts show them (DSR's rule: a piece is
     drawn while it shares a draw group with the collision part under the player): pieces no part of this block shows
     are left out, and so are another map's pieces inside this map's blocks and any peer surface within FAR_STANDIN
     of this block's own horizon (stand-ins: the same place twice; a peer mostly made of them is left out whole:
     Firelink has the game's own low-detail Burg); the rest become at most FAR_SETS meshes of pieces shown
     from about the same floors, each with the union of its pieces' draw groups, which the runtime tests against the
     part under the player;
  3. each such group is rendered from one eye (the middle of the ground the peer is seen from) into a panorama of
     FAR_DEG degrees a cell (tools/mapview.py's software renderer, six views), and every cell that shows something
     becomes a quad at the depth it was seen at, in the colour seen there: a surface of what can be seen from here,
     as coarse as it looks from here, with parallax right to first order as the player walks. Neighbouring cells
     share corners unless their depths differ by more than FAR_JUMP (a silhouette against something far behind: the
     gap shows the sky from other spots rather than a stretched sheet);
  4. an edge collapse (horizon.py mo_simplify) at FAR_PX pixels seen from the eye merges the flat stretches; coarser
     cells until the file fits FAR_KB (never past FAR_DEG_MAX);
  5. indexed VU1 batches as the horizon's (horizon.py idx_batches / idx_streams), sorted along a Z curve.

far.bin (little endian, 16-byte aligned; stream offsets from the file start):
  header (16 B): 'FARP', sets, chunks, bytes
  per set (32 B): u32 world block it shows, first chunk, chunks, u32 draw groups[4] (all ones: always), u32 0
  chunk records (64 B, map.c Chunk as hzn.bin's: tex -1, flags CHUNK_HZN | CHUNK_BACKDROP)
  streams
Env: FARPANO=0 writes an empty file (no panorama: world.c then keeps the boot block's tables, the old path);
FAR_DEG, FAR_DEG_MAX, FAR_EL, FAR_JUMP, FAR_PX, FAR_KB, FAR_D, FAR_SETS, FAR_FAR, FAR_NEAR, FAR_STANDIN."""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import horizon  # noqa: E402

ON = os.environ.get("FARPANO", "1") == "1"
# The panorama's cell. ASSUMED 0.75 degrees (7 pixels of the port's 43 degree view): Ame, 2026-11-14: "it doesn't
# need to match 1 to 1, it can be lower quality"
DEG = float(os.environ.get("FAR_DEG", 0.75))
DEG_MAX = float(os.environ.get("FAR_DEG_MAX", 3.0))
EL = float(os.environ.get("FAR_EL", 45.0))     # degrees above and below the horizontal
JUMP = float(os.environ.get("FAR_JUMP", 1.5))  # depth ratio between neighbouring cells that still share a corner
MIN = 0.5                                      # share of a cell's samples that must show something
FAR = float(os.environ.get("FAR_FAR", 800.0))  # metres: nothing beyond
NEAR = float(os.environ.get("FAR_NEAR", 24.0))  # nor nearer (ASSUMED: a block that is off is a 32 m cell away at least)
PX = float(os.environ.get("FAR_PX", 8.0))      # the collapse's screen error (about one cell)
# The file's ceiling. It lives in the heap beside the enemy models while its block is the player's (world.c), in the
# room the boot block's tables leave (1.4 MB): ASSUMED 192 KB
KB = float(os.environ.get("FAR_KB", 192))
D = float(os.environ.get("FAR_D", 46.0))       # metres: nearest distance the collapse's error is judged from
# metres: a peer's triangle this near to one of this block's own horizon is the same place drawn twice (ASSUMED: the
# game's stand-ins lie within a few metres of the detailed pieces; 0: off)
STANDIN = float(os.environ.get("FAR_STANDIN", 6.0))
STANDIN_ALL = 0.5  # share of a peer on this block's surfaces from which the block is taken to stand in for all of it
SETS = int(os.environ.get("FAR_SETS", 3))      # draw-group meshes per peer at most
ALL = (0xFFFFFFFF,) * 4
CHUNK_BACKDROP, CHUNK_HZN = 128, 256


def hzn_tris(path):
    """hzn.bin -> (P (n, 3, 3) corners, C (n, 3, 3) colours 0..255, set (n,) draw-group piece mask index)"""
    b = open(path, "rb").read() if os.path.exists(path) else b""
    if b[:4] != b"HZN1":
        return np.zeros((0, 3, 3)), np.zeros((0, 3, 3)), np.zeros(0, np.int64)
    nch, ncell = struct.unpack_from("<2I", b, 4)
    nch &= 0xFFFF
    off = 16 + 32 * ncell
    Ps, Cs, Ss = [], [], []
    for i in range(nch):
        _ti, fl, nw, po, _so, co = struct.unpack_from("<iII3I", b, off + 64 * i)
        t, nv = nw & 0xFFFF, nw >> 16
        org, step = np.array(struct.unpack_from("<3f", b, off + 64 * i + 48)), struct.unpack_from("<f", b, off + 64 * i + 60)[0]
        if nv:
            ix = np.frombuffer(b, np.uint8, t * 4, co + -(-4 * nv // 16) * 16 + 16).reshape(t, 4)[:, :3]
            pv = np.frombuffer(b, np.int16, nv * 3, po).reshape(nv, 3)[ix]
            cv = np.frombuffer(b, np.uint8, nv * 4, co).reshape(nv, 4)[ix]
        else:
            pv = np.frombuffer(b, np.int16, t * 9, po).reshape(t, 3, 3)
            cv = np.frombuffer(b, np.uint8, t * 12, co).reshape(t, 3, 4)
        Ps.append(org + step * pv.astype(np.float64)), Cs.append(cv[..., :3].astype(np.float64))
        Ss.append(np.full(t, fl >> 16 & 0xFF, np.int64))
    if not Ps:
        return np.zeros((0, 3, 3)), np.zeros((0, 3, 3)), np.zeros(0, np.int64)
    return np.concatenate(Ps), np.concatenate(Cs), np.concatenate(Ss)


def dg_masks(gen):
    """dg.bin -> (piece masks [(4 x u32)], the collision masks this block's own collision triangles use, each one's
    share of those triangles, area group); ([], [], [], 0) without one"""
    p = os.path.join(gen, "dg.bin")
    if not os.path.exists(p):
        return [], [], [], 0
    d = open(p, "rb").read()
    _mg, P, C, N, T, _E, _S, area = struct.unpack_from("<8I", d, 0)
    pm = [struct.unpack_from("<4I", d, 32 + 16 * i) for i in range(P)]
    cnt = np.bincount(np.frombuffer(d, np.uint8, T, 32 + 16 * (P + C) + N), minlength=C)[:C]  # (the table holds the whole area's)
    used = np.flatnonzero(cnt)
    cm = [struct.unpack_from("<4I", d, 32 + 16 * (P + int(i))) for i in used]
    return pm, cm, cnt[used] / max(1, cnt.sum()), area


def own_nav(gen):
    """this block's walkable ground: the navmesh vertices of its map.bin inside its own cells (a block's map.bin
    carries the whole MSB's navmesh; tools/area.py BLOCKS)"""
    import area
    nav = horizon.nav_points(open(os.path.join(gen, "map.bin"), "rb").read())
    cs = area.BLOCKS.get(os.path.basename(os.path.normpath(gen)), {}).get("cells")
    if cs and len(nav):
        keep = np.zeros(len(nav), bool)
        for x0, z0, x1, z1 in area.cell_rects(cs):
            keep |= (nav[:, 0] >= x0 * area.CELL) & (nav[:, 0] < (x1 + 1) * area.CELL) & \
                (nav[:, 2] >= z0 * area.CELL) & (nav[:, 2] < (z1 + 1) * area.CELL)
        nav = nav[keep] if keep.any() else nav
    return nav


def pano_mesh(P, C, eye, deg):
    """triangles P (n, 3, 3) with corner colours C (n, 3, 3) as seen from eye -> (V, VC, T): the panorama's quads"""
    import math
    import tempfile
    import mapview
    from PIL import Image
    d = np.sqrt(((P.mean(1) - eye) ** 2).sum(1))
    P, C = P[d < FAR], C[d < FAR]
    if not len(P):
        return np.zeros((0, 3)), np.zeros((0, 3)), np.zeros((0, 3), np.int64)
    n = len(P)
    col = np.concatenate([C * (128.0 / 150.0), np.full((n, 3, 1), 128.0)], 2)  # (render_view: 150 grey x colour / 128)
    geo = (P.astype(np.float32), np.zeros((n, 3, 2)), col, np.full(n, -1), np.zeros(n, np.int64), np.zeros(n),
           np.concatenate([P.min(1), P.max(1)], 1))
    NV, S, FOV = 6, 640, 100.0
    f = S / 2 / math.tan(math.radians(FOV) / 2)
    views = []
    with tempfile.TemporaryDirectory() as tmp:
        for k in range(NV):
            yaw = 2 * math.pi * k / NV
            fwd = np.array([math.sin(yaw), 0.0, math.cos(yaw)])
            z = []
            mapview.render_view(os.path.join(tmp, "v.png"), geo, [], tuple(eye), tuple(eye + fwd), W=S, H=S, fov_y=FOV, zout=z)
            views.append((np.asarray(Image.open(os.path.join(tmp, "v.png")).convert("RGB"), np.float64), z[0], fwd))
    # the panorama's cells: azimuth x elevation, K x K samples each, taken from the view that faces them most
    K = 3
    naz, nel = int(round(360.0 / deg)), int(round(2 * EL / deg))
    az = (np.arange(naz * K) + 0.5) / (naz * K) * 2 * math.pi
    el = ((np.arange(nel * K) + 0.5) / (nel * K) * 2 - 1) * math.radians(EL)
    A, E = np.meshgrid(az, el)  # (nel K, naz K)
    Dr = np.stack([np.sin(A) * np.cos(E), np.sin(E), np.cos(A) * np.cos(E)], -1)
    dist = np.full(A.shape, np.inf)
    rgb = np.zeros(A.shape + (3,))
    vi = np.rint(A / (2 * math.pi / NV)).astype(np.int64) % NV
    for k, (img, zb, fwd) in enumerate(views):
        m = vi == k
        right = np.array([fwd[2], 0.0, -fwd[0]])
        dz = Dr[m] @ fwd
        x = np.clip(np.floor(S / 2 + f * (Dr[m] @ right) / dz).astype(np.int64), 0, S - 1)
        y = np.clip(np.floor(S / 2 - f * Dr[m][:, 1] / dz).astype(np.int64), 0, S - 1)
        dist[m] = zb[y, x] / dz  # (depth along the view axis -> distance along the ray)
        rgb[m] = img[y, x]
    ok = np.isfinite(dist) & (dist < FAR) & (dist >= NEAR)
    blk = lambda a: a.reshape(nel, K, naz, K, *a.shape[2:]).swapaxes(1, 2).reshape(nel, naz, K * K, *a.shape[2:])
    okc, dc, cc = blk(ok), blk(np.where(ok, dist, np.nan)), blk(np.where(ok[..., None], rgb, 0.0))
    cnt = okc.sum(2)
    show = cnt >= MIN * K * K
    import warnings
    with np.errstate(all="ignore"), warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)  # (cells that show nothing: all NaN)
        cd = np.where(show, np.nanmedian(dc, 2), np.nan)                 # the cell's depth
        ccol = np.where(show[..., None], cc.sum(2) / np.maximum(cnt, 1)[..., None], 0.0)
    # corners: mean depth / colour of the cells around them within JUMP of the cell asking (a corner shared by a near
    # and a far cell gets one position for each side)
    V, VC, T, vid = [], [], [], {}
    azc = np.arange(naz + 1) / naz * 2 * math.pi
    elc = (np.arange(nel + 1) / nel * 2 - 1) * math.radians(EL)
    for i, j in zip(*np.nonzero(show)):
        quad = []
        for di, dj in ((0, 0), (0, 1), (1, 1), (1, 0)):
            ds, cs = [], []
            for ii in (i + di - 1, i + di):
                for jj in ((j + dj - 1) % naz, (j + dj) % naz):
                    if 0 <= ii < nel and show[ii, jj] and max(cd[ii, jj], cd[i, j]) <= JUMP * min(cd[ii, jj], cd[i, j]):
                        ds.append(cd[ii, jj]), cs.append(ccol[ii, jj])
            r, c = float(np.mean(ds)), np.mean(cs, 0)
            a, e = azc[j + dj], elc[i + di]
            key = (i + di, (j + dj) % naz, round(r, 2))
            if key not in vid:
                vid[key] = len(V)
                V.append(eye + r * np.array([math.sin(a) * math.cos(e), math.sin(e), math.cos(a) * math.cos(e)]))
                VC.append(c)
            quad.append(vid[key])
        T += [(quad[0], quad[1], quad[2]), (quad[0], quad[2], quad[3])]
    return np.array(V).reshape(-1, 3), np.array(VC).reshape(-1, 3), np.array(T, np.int64).reshape(-1, 3)


def groups(S, pm, coll, wgt):
    """a peer's horizon triangles' piece masks S -> [(draw groups (4 x u32), member mask indices)]: the pieces some
    collision part of this block shows, as at most SETS groups of pieces shown from about the same floors (the two
    groups whose shown floors differ least, by collision triangles, are joined until SETS are left); a group's draw
    groups are the union of its members', so it is drawn whenever one of them would be"""
    cls = {}
    for s in np.unique(S):
        m = pm[s] if 0 < s < len(pm) else ALL
        sg = tuple(m == ALL or any(a & b for a, b in zip(m, c)) for c in coll)
        if any(sg):
            g = cls.setdefault(sg, [np.array(sg), [0, 0, 0, 0], [], 0])
            g[1] = [a | b for a, b in zip(g[1], m)]
            g[2].append(int(s))
            g[3] += int((S == s).sum())
    cls = list(cls.values())
    while len(cls) > max(1, SETS):
        best = None
        for i in range(len(cls)):
            for j in range(i + 1, len(cls)):
                # the floors only one of the two shows, weighted by the smaller group's triangles
                c = float(wgt[cls[i][0] != cls[j][0]].sum()) * min(cls[i][3], cls[j][3])
                if best is None or c < best[0]:
                    best = (c, i, j)
        _c, i, j = best
        a, b = cls[i], cls.pop(j)
        cls[i] = [a[0] | b[0], [x | y for x, y in zip(a[1], b[1])], a[2] + b[2], a[3] + b[3]]
    return [(tuple(g[1]) if not g[0].all() else ALL, g[2]) for g in cls]


def write(res):
    """[(key (block, mask), V, VC, T)] -> far.bin bytes"""
    sets, recs, streams = [], [], bytearray()
    for (blk, mask), V, col, T in res:
        if not len(T):
            continue
        cen = V[T].mean(1)
        T = T[np.argsort(horizon.zkey(cen, cen.min(0), 4.0), kind="stable")]
        first = len(recs)
        for t in horizon.idx_batches(T):
            u, first_use, inv = np.unique(t.reshape(-1), return_index=True, return_inverse=True)
            order = np.argsort(first_use, kind="stable")
            rank = np.empty(len(u), np.int64)
            rank[order] = np.arange(len(u))
            vs = u[order]
            nv = -(-len(vs) // 3) * 3
            vs = np.concatenate([vs, np.repeat(vs[-1:], nv - len(vs))])
            tri = rank[inv.reshape(-1)].reshape(-1, 3)
            p = V[vs]
            org = (p.min(0) + p.max(0)) / 2
            step = 2.0 ** int(np.ceil(np.log2(max(np.abs(p - org).max(), 1e-3) / horizon.POS_MAX)))
            qp = np.clip(np.rint((p - org) / step), -32767, 32767).astype(np.int16)
            pq = qp.astype(np.float64) * step + org
            c4 = np.concatenate([col[vs], np.full((len(p), 1), 0x80, np.uint8)], 1)
            ix = np.zeros((len(t), 4), np.uint8)
            ix[:, :3] = tri
            pk, offs = horizon.idx_streams(len(t), nv, qp.tobytes(), c4.tobytes(), ix.tobytes())
            recs.append((len(t) | nv << 16, len(streams), offs, pq.min(0), pq.max(0), org, step))
            streams += pk
        sets.append((blk, first, len(recs) - first, mask))
    s0 = 16 + 32 * len(sets) + 64 * len(recs)
    out = bytearray(struct.pack("<4s3I", b"FARP", len(sets), len(recs), 0))
    for blk, first, n, mask in sets:
        out += struct.pack("<8I", blk, first, n, *mask, 0)
    for ntw, so, offs, mn, mx, org, step in recs:
        out += struct.pack("<iII3I6f4f", -1, CHUNK_HZN | CHUNK_BACKDROP, ntw, *(s0 + so + o for o in offs), *mn, *mx, *org, step)
    out += streams
    out.extend(b"\0" * (-len(out) % 16))
    struct.pack_into("<I", out, 12, len(out))
    return bytes(out)


def peer_eyes(nav, own_gen, peer_gen):
    """the spots of this block the peer block is seen from while it is off: all of its walkable ground for a block of
    another map (world.bin loads it on DSR's ConnectCollision parts only); for a block of the same MSB those farther
    than the link rule keeps it loaded (tools/export_world.py KEEP_CELLS cells from its cell rectangles)"""
    import area
    import export_world
    a, b = (area.BLOCKS.get(os.path.basename(os.path.normpath(g)), {}) for g in (own_gen, peer_gen))
    if not a or not b or a.get("map") != b.get("map") or not b.get("cells"):
        return nav
    d = np.full(len(nav), np.inf)
    for x0, z0, x1, z1 in area.cell_rects(b["cells"]):
        dx = np.maximum(0, np.maximum(x0 * area.CELL - nav[:, 0], nav[:, 0] - (x1 + 1) * area.CELL))
        dz = np.maximum(0, np.maximum(z0 * area.CELL - nav[:, 2], nav[:, 2] - (z1 + 1) * area.CELL))
        d = np.minimum(d, np.maximum(dx, dz))
    return nav[d > export_world.KEEP_CELLS * area.CELL]


def in_map(X, map_id):
    """points (n, 3) inside the cells of a block of map_id (tools/area.py BLOCKS, 32 m xz cells)"""
    import area
    out = np.zeros(len(X), bool)
    for o in area.BLOCKS.values():
        if o["map"] == map_id and o.get("cells"):
            for x0, z0, x1, z1 in area.cell_rects(o["cells"]):
                out |= (X[:, 0] >= x0 * area.CELL) & (X[:, 0] < (x1 + 1) * area.CELL) & \
                    (X[:, 2] >= z0 * area.CELL) & (X[:, 2] < (z1 + 1) * area.CELL)
    return out


def build(own, gens):
    """-> (far.bin bytes, log line)"""
    name = os.path.basename(os.path.normpath(gens[own]))
    _pm, coll, wgt, area_grp = dg_masks(gens[own])
    nav = own_nav(gens[own])
    if not area_grp or not len(nav):
        return write([]), f"farpano: {name}: none (no area group or no walkable ground)"
    horizon.HZN_D = D  # (horizon.py's distance bands start at its module global)
    import area
    own_map = area.BLOCKS.get(name, {}).get("map")
    src, n_in, n_hidden, n_standin, peers = [], 0, 0, 0, set()
    own_tris = hzn_tris(os.path.join(gens[own], "hzn.bin"))[0] if horizon._err_lib() is not None else np.zeros((0, 3, 3))
    for bi, g in enumerate(gens):
        if bi == own:
            continue
        pm, _c, _w, grp = dg_masks(g)
        eyes = peer_eyes(nav, gens[own], g) if grp == area_grp else []
        if not len(eyes):
            continue
        P, C, S = hzn_tris(os.path.join(g, "hzn.bin"))
        n_in += len(P)
        peers.add(bi)
        if own_map and area.BLOCKS.get(os.path.basename(os.path.normpath(g)), {}).get("map") != own_map:
            # another map's pieces inside this map's blocks are its stand-ins for this map (Firelink carries a
            # low-detail Undead Burg): the real blocks, or their own part of the panorama, stand there
            out = ~in_map(P.mean(1), own_map)
            n_standin += int((~out).sum())
            P, C, S = P[out], C[out], S[out]
        if STANDIN > 0 and len(own_tris) and len(P):
            # and a peer's surfaces that lie on this block's own (this block's stand-ins for the peer, or the peer's
            # for this block: Firelink's low-detail Burg and the Burg itself, drawn twice from Firelink's stairs)
            cen = P.mean(1)
            on = horizon._dist(own_tris, cen, np.full(len(cen), STANDIN)) < STANDIN
            n_standin += int(on.sum())
            if on.mean() > STANDIN_ALL:  # this block's map has its own far view of that peer: none of it twice
                n_standin += int((~on).sum())
                continue
            P, C, S = P[~on], C[~on], S[~on]
        eye = eyes.mean(0) + np.array([0.0, 1.5, 0.0])
        gs = groups(S, pm, coll, wgt)
        n_hidden += int((~np.isin(S, [m for _mask, mem in gs for m in mem])).sum())
        for mask, members in gs:
            sel = np.isin(S, members)
            src.append(((bi, mask), P[sel], C[sel], eye))
    deg = DEG
    while True:
        res = []
        for key, P, C, eye in src:
            V, VC, T = pano_mesh(P, C, eye, deg)
            n0 = len(T)
            if len(T) and horizon._meshopt() is not None:
                T = horizon.mo_simplify(V, VC, T, np.maximum(np.sqrt(((V - eye) ** 2).sum(1)), D), PX * deg / DEG)
            if os.environ.get("FAR_LOG"):
                print(f"farpano:   block {key[0]} groups {key[1][0]:08x}..: {len(P)} tris -> {n0} panorama -> {len(T)}")
            res.append((key, V, np.clip(np.rint(VC), 0, 255).astype(np.uint8), T))
        out = write(res)
        if len(out) <= KB * 1024 or deg >= DEG_MAX:
            break
        deg = min(DEG_MAX, deg * 1.25)
    nt = sum(len(r[3]) for r in res)
    return out, (f"farpano: {name}: {n_in} horizon tris of {len(peers)} blocks, {len(set(k[0][0] for k in src))} kept ({n_hidden} not shown from "
                 f"here, {n_standin} stand-ins) -> {nt} tris in {sum(1 for r in res if len(r[3]))} draw-group meshes, {len(out) >> 10} KB, {deg:.2f} degree cells")


def main():
    dst, gens = sys.argv[1], [os.path.abspath(g) for g in sys.argv[3:]]
    own = gens.index(os.path.abspath(sys.argv[2])) if os.path.abspath(sys.argv[2]) in gens else -1
    if own < 0:
        out, line = write([]), "farpano: none (not a block of this world)"
    elif not ON:
        out, line = write([]), "farpano: none (FARPANO=0)"
    else:
        out, line = build(own, gens)
    with open(dst, "wb") as f:
        f.write(out)
    print(line)


if __name__ == "__main__":
    main()
