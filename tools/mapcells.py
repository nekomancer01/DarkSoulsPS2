"""The map exporter's cell output: the PVS input, the streamed cell table and cells.bin (2026-11-12, code moved as it was; tools/export_map.py runs it)."""
import sys
import math, os, struct
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vu1layout import L as VU1L  # noqa: E402 (the VU1 memory contract, Polaris 1.5)
import mapcfg
import mapchunks
import mapsimp


def write_pvsin(path, all_chunks, nav_v, nav_t):
    """Input of the occlusion bake (tools/pvs_bake.c): per chunk in map.bin / dg.bin order its unit, flags and box,
    the units' cell boxes, the navmesh, then the chunks' triangles (world positions, float32). A unit is one MSB part
    within one CHUNK_GRID cell (huge terrain pieces so come in cell-sized clusters); sky chunks are unit 0xFFFFFFFF
    (always drawn). An empty file (header only, 0 chunks) when there is no PVS input: the bake then writes a pvs.bin
    that shows everything.
      header (32 B): magic, chunks, units, tris, nav verts, nav tris, cell table entries, f32 cell size
      per chunk (44 B): u32 unit, flags (bit 0 occluder, bit 1 near level, bit 2 coarse level, neither: kept /
                        always drawn; bits 8-15 map.c chunk flags), first triangle, triangles, cell table entry
                        (0xFFFFFFFF none: sky); f32 box min[3], max[3]
      per unit (40 B): i32 cell key[3], u32 part; f32 box of its cell's chunks min[3], max[3]
      parts: u32 count, NUL-terminated names, padded to 4 bytes
      nav: f32 x3 per vertex, u32 x3 per triangle
      tris: f32 x9 per triangle, in chunk order"""
    T = mapchunks.PVS_TRIS[0]
    if not mapcfg.PVS or T is None or len(mapchunks.PVS_ORDER) != len(all_chunks):
        if mapcfg.PVS:
            print("pvsin.bin: no PVS input (Python chunk path or no streaming): pvs.bin will show everything")
        with open(path, "wb") as f:
            f.write(struct.pack("<4s7I", mapsimp.PVSIN_MAGIC, 0, 0, 0, 0, 0, 0, 0))
        return
    units, parts, crec, cbox = {}, {}, [], {}
    tri_chunks, first = [], 0
    for j, (i, ck) in enumerate(mapchunks.PVS_ORDER):
        ch = all_chunks[j]
        part, occ = mapchunks.CHUNK_PART[i]
        sky = ch[1] & mapcfg.CHUNK_SKY
        if sky or ck is None:
            u = 0xFFFFFFFF
        else:
            key = tuple(ck[0])
            u = units.setdefault((parts.setdefault(part, len(parts)), key), len(units))
            lo, hi = cbox.get(key, ((1e9,) * 3, (-1e9,) * 3))
            cbox[key] = (tuple(min(a, b) for a, b in zip(lo, ch[6])), tuple(max(a, b) for a, b in zip(hi, ch[7])))
        src = np.asarray(mapchunks.CHUNK_SRC[i], np.int64)
        near = ck is not None and ck[1] == 1
        coarse = not near and mapchunks.CHUNK_TAG[i] is not None and mapchunks.CHUNK_TAG[i][0] == 0
        fl = (1 if occ and not ch[1] & (mapcfg.CHUNK_BLEND | mapchunks.CHUNK_GRASS | mapcfg.CHUNK_SKY) else 0) | (2 if near else 0) | \
             (4 if coarse else 0) | (ch[1] & 0xFF) << 8
        ent = CHUNK_ENTRY[j] if j < len(CHUNK_ENTRY) and CHUNK_ENTRY[j] is not None else 0xFFFFFFFF
        crec.append(struct.pack("<5I6f", u, fl, first, len(src), ent, *ch[6], *ch[7]))
        tri_chunks.append(src)
        first += len(src)
    urec = []
    for (pi, key), u in sorted(units.items(), key=lambda kv: kv[1]):
        lo, hi = cbox[key]
        urec.append(struct.pack("<3iI6f", *key, pi, *lo, *hi))
    pn = b"".join(n.encode() + b"\0" for n in parts)
    idx = np.concatenate(tri_chunks) if tri_chunks else np.zeros(0, np.int64)
    with open(path, "wb") as f:
        f.write(struct.pack("<4s6If", mapsimp.PVSIN_MAGIC, len(crec), len(urec), len(idx), len(nav_v), len(nav_t),
                            len(CELL_SIZES), mapcfg.CHUNK_GRID))
        f.write(b"".join(crec))
        f.write(b"".join(urec))
        f.write(struct.pack("<I", len(parts)) + pn + b"\0" * (-(len(pn) + 4) % 4))
        f.write(np.asarray(nav_v, np.float32).reshape(-1, 3).tobytes())
        f.write(np.asarray(nav_t, np.uint32).reshape(-1, 3).tobytes())
        f.write(np.ascontiguousarray(T[idx]).tobytes())
    print(f"pvsin.bin: {len(crec)} chunks, {len(urec)} units of {len(parts)} parts, {len(idx)} tris")


def stream_cells(chunks, spots=None):
    """STREAM: [(cell key, resident chunk indices (coarse level + kept meshes whose centre lies in the cell, by
    texture), near chunk indices (by texture))] in the order of the disc: trace_order over the player spots when given
    (TRACE_ORDER, default on), else Z order of the key (cell_zorder); sky chunks belong to no cell."""
    cells = {}
    for i, ch in enumerate(chunks):
        if ch[1] & mapcfg.CHUNK_SKY:
            continue
        t = mapchunks.CHUNK_TAG[i]
        key = tuple(t[1]) if t else tuple(int(math.floor((ch[6][a] + ch[7][a]) / 2 / mapcfg.CHUNK_GRID)) for a in range(3))
        cells.setdefault(key, ([], []))[1 if t and t[0] == 1 else 0].append(i)
    def lev(i):  # kept chunks, then the coarse ones, each by texture: map.c cuts a cell's items by level (streaming v2)
        t = mapchunks.CHUNK_TAG[i]
        return (1 if t and t[0] == 0 else 0, chunks[i][0])
    keys = sorted(cells, key=cell_zorder)
    if TRACE_ORDER and spots is not None and len(spots) and len(keys) > 2:
        keys = trace_order(keys, cells, chunks, spots)
    return [(k, sorted(cells[k][0], key=lev), sorted(cells[k][1], key=lambda i: chunks[i][0])) for k in keys]


# TRACE_ORDER (default 1; TRACE_ORDER=0: Z order as before): the cells' order on the disc (cell table, mapres.bin,
# cells.bin) from the reads a player makes, not from the grid alone. The map research's idea 10 (2026-11-17), after
# Wassenberg ("Optimizing File Accesses via Ordering and Caching", 2006): record which files are read back to back,
# then chain the most frequent pairs greedily (an approximate travelling salesman). The traces here are simulated at
# export, one per player spot (navmesh vertices, as the spot budgets use): the cells of the spot's hard tier
# (residency.h RES_PIN_R) in the order mapstream's hard tier reads them, nearest first. Every spot counts the same
# (weighted by spot, not by a test route: no route's path is baked in). Pairs read back to back within the near
# level's radius (RES_NEAR_R) count TRACE_NEAR_W: those reads wait at the player's feet. Z order put a block's cells
# in quadrants of 2^n cells: a spot on a quadrant edge read cells 1-2 MB apart (B8b's runs log: ~8 seeks a second).
TRACE_ORDER = os.environ.get("TRACE_ORDER", "1") != "0"
TRACE_SPOTS = 3000  # spots at most (every n-th navmesh vertex)
TRACE_NEAR_W = 2


def trace_order(keys, cells, chunks, spots):
    """keys (Z order) -> keys in the order of chained back-to-back reads (deterministic for the same input)"""
    n = len(keys)
    lo = np.full((n, 3), np.inf)
    hi = np.full((n, 3), -np.inf)
    for c, k in enumerate(keys):  # the cell's box: its resident and near chunks (map.c / mapstream box_d2)
        for i in cells[k][0] + cells[k][1]:
            lo[c] = np.minimum(lo[c], chunks[i][6])
            hi[c] = np.maximum(hi[c], chunks[i][7])
    P = np.asarray(spots, np.float64).reshape(-1, 3)
    P = P[::max(1, len(P) // TRACE_SPOTS)]
    pin_r, near_r = mapcfg.WS_PIN_R, mapcfg.WS_NEAR_R
    w, sets = {}, []
    for p in P:
        d = np.sqrt((np.maximum(0, np.maximum(lo - p, p - hi)) ** 2).sum(1))
        idx = np.nonzero(d < pin_r)[0]
        if len(idx) < 2:
            continue
        sets.append(idx)
        idx = idx[np.lexsort((idx, d[idx]))]  # nearest first, ties by Z order
        for a, b in zip(idx[:-1], idx[1:]):
            e = (min(a, b), max(a, b))
            w[e] = w.get(e, 0) + (TRACE_NEAR_W if d[b] < near_r else 1)
    if not w:
        print(f"cells: trace order: no spot within {pin_r:.0f} m of two cells, Z order kept")
        return keys
    # greedy edge chaining: heaviest pairs first, joined while both ends are chain ends of different chains
    nxt = [[] for _ in range(n)]
    root = list(range(n))

    def find(a):
        while root[a] != a:
            root[a] = root[root[a]]
            a = root[a]
        return a
    for (a, b), _ in sorted(w.items(), key=lambda kv: (-kv[1], kv[0])):
        if len(nxt[a]) < 2 and len(nxt[b]) < 2 and find(a) != find(b):
            nxt[a].append(b), nxt[b].append(a)
            root[find(a)] = find(b)
    chains, seen = [], [False] * n
    for s in range(n):  # chains from their lower (Z order) end; lone cells are chains of one
        if seen[s] or len(nxt[s]) == 2:
            continue
        ch, prev, cur = [], -1, s
        while cur >= 0:
            seen[cur] = True
            ch.append(cur)
            nx = [x for x in nxt[cur] if x != prev and not seen[x]]
            prev, cur = cur, (nx[0] if nx else -1)
        chains.append(ch)
    # chains one after another: from the one holding the first cell in Z order, each time the chain with an end
    # nearest (cell box centres) to where the last one ended, turned to start at that end
    ctr = np.where(np.isfinite(lo), (lo + hi) / 2, 0)
    order, left = list(chains[0]), chains[1:]
    while left:
        e = ctr[order[-1]]
        best = min(range(len(left)), key=lambda j: (min(((ctr[left[j][0]] - e) ** 2).sum(), ((ctr[left[j][-1]] - e) ** 2).sum()), j))
        ch = left.pop(best)
        if ((ctr[ch[-1]] - e) ** 2).sum() < ((ctr[ch[0]] - e) ** 2).sum():
            ch = ch[::-1]
        order += ch
    assert sorted(order) == list(range(n))
    pos = np.empty(n, np.int64)

    def runs(o):  # contiguous runs on the disc per spot's hard tier: the seeks one spot's reads cost at least
        pos[o] = np.arange(n)
        return sum(1 + int((np.diff(np.sort(pos[s])) != 1).sum()) for s in sets) / len(sets)
    rz, rt = runs(list(range(n))), runs(order)
    print(f"cells: trace order: {len(sets)} spots, {n} cells in {len(chains)} chains; a spot's hard tier in "
          f"{rz:.2f} runs on the disc (Z order) -> {rt:.2f}{'' if rt < rz else ', Z order kept'}")
    return [keys[c] for c in order] if rt < rz else keys


def cell_zorder(key):
    """Z order (Morton) of a cell key: the bits of x, z and y interleaved, so cells next to each other on the ground
    sit next to each other in the cell table, mapres.bin and cells.bin (the order of the disc). mapstream reads
    neighbouring resident items in one request (MS_RUN) and a walk reads nearby sectors; sorted by (x, y, z) as before,
    the cells of one x column came first and a neighbour in x was a whole column away (plan step 5, 2026-11-06)."""
    k = 0
    for b in range(10):
        for a in (0, 2, 1):
            k |= ((key[a] + 512) >> b & 1) << (3 * b + (0, 2, 1).index(a))
    return k


CELL_SIZES = []
RES_SIZE = []  # bytes of each resident chunk in map.bin (its packet + record)
CELL_BUDGET = []  # write_cells: per cell [key x, y, z, box min, max, near, coarse, kept, backdrop bytes, near box min, max] (map.bin.cells.json)
CHUNK_ENTRY = []  # write_cells: per chunk (map.bin / dg.bin order) its cell's first cell table entry, else None
CELLS_VIFPKT = 1  # cell table word 1, format flags: bit 0 = chunk streams are VIF packets (chunk_streams)
# bit 1: per entry s16 x, y, z, 0 of its CHUNK_GRID cell follow the entries. NEAR_CUBE (default on; 0 = none, the
# runtime then measures the near level to the box as before): mapstream.c / map.c measure the near level's distance to
# the cell's box clipped to that cube (mapstream.h cell_near_d2, MS_NEAR_CUBE): triangles are grouped by centroid, not
# cut, and the Undead Burg's near cells reach 12 m past their cube at the median (big wall and ground triangles), so a
# near level was wanted ~12 m early (2026-11-08, TECH_DEBT world-10). The runtime's CELL_GRID is 32 m.
CELLS_KEYS = 2
NEAR_CUBE = os.environ.get("NEAR_CUBE", "1") == "1" and mapcfg.CHUNK_GRID == 32.0
# Near cell blobs over NEAR_PART bytes are written as several parts (consecutive cell table entries, the later ones
# marked CELL_PART in their near count): each part is a mapstream item of its own, so no near cell needs one big hole
# in the arena (Burg near cells of 0.6-2.2 MB found none with 1.2-1.6 MB free: 115 refusals on the ISO route,
# TECH_DEBT world-9). map.c draws a cell's near level once every part is in. Plan step 5, 2026-11-06.
# One part is at most one page of mapstream's arena (MS_PAGE, plan step 6): 256 KB parts (step 5) still found no hole
# of their size with ~450 KB free in the arena; a part of a page fits any free page. A single chunk over a page makes
# a part of its own (it starts on a page).
NEAR_PART = 64 * 1024
NEAR_PARTS_MAX = 64  # map.c NEAR_PARTS
# NEAR_CLASSES (default on; NEAR_CLASSES=0 = the greedy cut before 2026-11-17): a cell's parts are cut so each is either
# "full" (NEAR_FULL_MIN up to a page) or a "tail" of at most NEAR_TAIL, so a page holds one full part and one tail and
# every hole a dropped part leaves fits another part of its class. The greedy cut filled each part until the next chunk
# did not fit: almost every part but a cell's last was 40-63 KB and needed a whole free page, while holes of 20-45 KB
# took none of them (world-14: "page-sized groups (40-63 KB) with 0.3-1.8 MB free in holes"; streaming research idea
# 3(b)). The parts stay runs of consecutive chunks (map.c / pvs.c read a part's chunks as one number range), so the
# cut is a shortest path over the cut points: fewest parts in neither class, then fewest parts. A chunk over NEAR_TAIL
# that cannot sit in a full part is a part of its own whatever its size.
NEAR_CLASSES = os.environ.get("NEAR_CLASSES", "1") == "1"
NEAR_TAIL = 16 * 1024
NEAR_FULL_MIN = NEAR_PART - NEAR_TAIL


def near_cut(sizes):
    """Parts (lists of chunk indices, in order) for chunks of these blob sizes: NEAR_CLASSES' classed cut, else greedy."""
    n = len(sizes)
    if not NEAR_CLASSES or sum(sizes) + 63 <= NEAR_PART:
        parts, cur, cb = [], [], 0
        for j in range(n):
            if cur and cb + sizes[j] + 63 > NEAR_PART:
                parts.append(cur)
                cur, cb = [], 0
            cur.append(j)
            cb += sizes[j]
        parts.append(cur)
        return parts
    INF = (1 << 30, 0)
    best = [INF] * (n + 1)  # best[i]: (mid parts, parts) for chunks [0, i)
    prev = [0] * (n + 1)
    best[0] = (0, 0)
    for i in range(n):
        if best[i] == INF:
            continue
        b = 0
        for e in range(i + 1, n + 1):
            b += sizes[e - 1]
            if e - 1 > i and b + 63 > NEAR_PART:
                break
            t = b + (-b % 64)
            mid = 0 if (t <= NEAR_TAIL or t >= NEAR_FULL_MIN or e - i == 1) else 1
            c = (best[i][0] + mid, best[i][1] + 1)
            if c < best[e]:
                best[e], prev[e] = c, i
    parts, e = [], n
    while e > 0:
        parts.append(list(range(prev[e], e)))
        e = prev[e]
    return parts[::-1]
CELL_PART = 0x80000000


def near_hist(hist, sizes, parts):
    """Adds a cell's near parts to the export's size print, for the cut used and the greedy one (NEAR_CLASSES A/B)."""
    global NEAR_CLASSES
    cuts = [("classed" if NEAR_CLASSES else "greedy", parts)]
    if NEAR_CLASSES:
        NEAR_CLASSES = False
        cuts.append(("greedy", near_cut(sizes)))
        NEAR_CLASSES = True
    for cut, ps in cuts:
        h = hist.setdefault(cut, [0, 0, 0, 0, 0])
        for part in ps:
            b = sum(sizes[j] for j in part)
            b += -b % 64
            h[0 if b <= NEAR_TAIL else 2 if b >= NEAR_FULL_MIN else 1] += 1
        h[3] += len(ps)
        h[4] = max(h[4], len(ps))


def write_cells(cells, res, all_chunks, nres):
    """DSM5 cell table + cells.bin. Table: u32 cells, flags (CELLS_VIFPKT), 0, 0; per cell (48 B) f32 min[3], max[3] (resident + near
    chunks), u32 blob offset, blob bytes (in cells.bin), near chunks, first resident chunk, resident chunks, number in
    bump.bin of the cell's first near chunk.
    cells.bin: per cell one blob (64-byte aligned): near chunk records (as map.bin's, stream offsets relative to the
    blob, flags bits 8-31 = the chunk's number in bump.bin; foliage (CHUNK_GRASS) keeps its tuft bits there, its
    number is the table's first near number + its index in the blob), then the streams (16-byte aligned).
    A cell whose blob would pass NEAR_PART bytes is split into parts of whole chunks (in their order): the first part
    in the cell's own entry, the others in the entries after it with near | CELL_PART, 0 resident chunks and the same
    box; each part is a blob of its own."""
    # word 1 bits 8-31: the VU1 layout the packets were written for (tools/vu1/layout.py layout_hash; mapblock.c checks)
    tab, cbin = bytearray(struct.pack("<4I", len(cells), CELLS_VIFPKT | (VU1L.layout_hash() & 0xFFFFFF) << 8, 0, 0)), bytearray()
    CELL_SIZES.clear()
    CELL_BUDGET.clear()
    CHUNK_ENTRY[:] = [None] * len(all_chunks)
    first, gid = 0, nres
    hist = {}  # cut -> [tail, mid, full, parts, most parts in a cell] (the near part size print below)
    for key, r, n in cells:
        boxes = [all_chunks[i] for i in range(first, first + len(r))] + \
                [all_chunks[gid + j] for j in range(len(n))]
        mn = [min(b[6][a] for b in boxes) for a in range(3)]
        mx = [max(b[7][a] for b in boxes) for a in range(3)]
        pkts = [mapchunks.chunk_packet(*all_chunks[gid + j][:6]) for j in range(len(n))]

        def size(j):  # bytes chunk gid + j adds to a blob: its record + its packet (VIF code qwords, streams)
            return 64 + len(pkts[j][0])
        parts = near_cut([size(j) for j in range(len(n))])
        if n:
            near_hist(hist, [size(j) for j in range(len(n))], parts)
        assert len(parts) <= NEAR_PARTS_MAX, f"cell {key}: {len(parts)} near parts"
        for i in list(range(first, first + len(r))) + [gid + j for j in range(len(n))]:
            CHUNK_ENTRY[i] = len(CELL_SIZES)  # the cell's own (first) entry, also for its later near parts
        for p, part in enumerate(parts):
            b = bytearray(b"\0" * (64 * len(part)))
            for i, j in enumerate(part):
                ti, fl, ntri, pos, st, col, lo, hi, org, step, sts = all_chunks[gid + j]
                pk, offs, nw = pkts[j]
                offs = [len(b) + o for o in offs]
                b += pk
                assert gid + j < 1 << 24
                b[64 * i:64 * i + 64] = struct.pack("<iII3I6f4f", ti, fl | (mapcfg.CHUNK_ST1K if sts == 1024 else 0) |
                                                    (0 if fl & mapchunks.CHUNK_GRASS else (gid + j) << 8), nw, *offs, *lo, *hi,
                                                    *org, step)
            b.extend(b"\0" * (-len(b) % 64))
            off = len(cbin) if part else 0
            cbin += b if part else b""
            CELL_SIZES.append((key, len(r) if not p else 0, len(part), len(b) if part else 0))
            tab += struct.pack("<6f6I", *mn, *mx, off, len(b) if part else 0, len(part) | (CELL_PART if p else 0),
                               first if not p else first + len(r), len(r) if not p else 0, gid + (part[0] if part else 0))
        lv = [0, 0, 0]  # coarse, kept, backdrop bytes of the cell's resident items
        for i in range(first, first + len(r)):
            lv[0 if res[i][1] & mapcfg.CHUNK_COARSE else 2 if res[i][1] & mapcfg.CHUNK_BACKDROP else 1] += \
                RES_SIZE[i] if i < len(RES_SIZE) else 0
        nbx = [all_chunks[gid + j] for j in range(len(n))] or boxes
        CELL_BUDGET.append([*key, *mn, *mx, sum(c[3] for c in CELL_SIZES[-len(parts):]), *lv,
                            *(min(b[6][a] for b in nbx) for a in range(3)), *(max(b[7][a] for b in nbx) for a in range(3))])
        first += len(r)
        gid += len(n)
    for cut, h in sorted(hist.items()):
        print(f"  near parts ({cut}): {h[3]} = tail {h[0]} / mid {h[1]} / full {h[2]}, most in a cell {h[4]} "
              f"(max {NEAR_PARTS_MAX})")
    tab[0:4] = struct.pack("<I", len(CELL_SIZES))
    if NEAR_CUBE:  # the cell keys after the entries (mapstream.h CELLS_KEYS / cell_near_d2)
        tab[4:8] = struct.pack("<I", CELLS_VIFPKT | CELLS_KEYS | (VU1L.layout_hash() & 0xFFFFFF) << 8)
        tab += b"".join(struct.pack("<4h", *key, 0) for key, *_ in CELL_SIZES)
    return bytes(tab), bytes(cbin)
