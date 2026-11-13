"""The map exporter's cell output: the PVS input, the streamed cell table and cells.bin (2026-11-12, code moved as it was; tools/export_map.py runs it)."""
import sys
import math, os, struct
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
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


def stream_cells(chunks):
    """STREAM: [(cell key, resident chunk indices (coarse level + kept meshes whose centre lies in the cell, by
    texture), near chunk indices (by texture))] in Z order of the key (cell_zorder); sky chunks belong to no cell."""
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
    return [(k, sorted(r, key=lev), sorted(n, key=lambda i: chunks[i][0]))
            for k, (r, n) in sorted(cells.items(), key=lambda kv: cell_zorder(kv[0]))]


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
CELL_PART = 0x80000000


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
    tab, cbin = bytearray(struct.pack("<4I", len(cells), CELLS_VIFPKT, 0, 0)), bytearray()
    CELL_SIZES.clear()
    CELL_BUDGET.clear()
    CHUNK_ENTRY[:] = [None] * len(all_chunks)
    first, gid = 0, nres
    for key, r, n in cells:
        boxes = [all_chunks[i] for i in range(first, first + len(r))] + \
                [all_chunks[gid + j] for j in range(len(n))]
        mn = [min(b[6][a] for b in boxes) for a in range(3)]
        mx = [max(b[7][a] for b in boxes) for a in range(3)]
        pkts = [mapchunks.chunk_packet(*all_chunks[gid + j][:6]) for j in range(len(n))]

        def size(j):  # bytes chunk gid + j adds to a blob: its record + its packet (VIF code qwords, streams)
            return 64 + len(pkts[j][0])
        parts, cur, cb = [], [], 0
        for j in range(len(n)):
            if cur and cb + size(j) + 63 > NEAR_PART:
                parts.append(cur)
                cur, cb = [], 0
            cur.append(j)
            cb += size(j)
        parts.append(cur)
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
    tab[0:4] = struct.pack("<I", len(CELL_SIZES))
    if NEAR_CUBE:  # the cell keys after the entries (mapstream.h CELLS_KEYS / cell_near_d2)
        tab[4:8] = struct.pack("<I", CELLS_VIFPKT | CELLS_KEYS)
        tab += b"".join(struct.pack("<4h", *key, 0) for key, *_ in CELL_SIZES)
    return bytes(tab), bytes(cbin)
