"""World block table for the PS2 block manager (src/world.c, TECH_DEBT build-5): which block packs exist, where each
one lies, which ones neighbour it and which of its collision triangles load which neighbour. Blocks are defined in
tools/area.py (WORLDS / BLOCKS); this script reads their exported collision.bin files for the bounds, the same
collision crops (export_collision.area_collision, collision.bin's triangle order) for the links, and the game's MSB
ConnectCollision parts (tools/worldgraph.py) for the links between maps.

  export_world.py <game dir> <out world.bin> <boot map> <gen dir>

world.bin: a chunk file (tools/chunkfile.py, file id 'WRLD'; src/world.c reads it in place), little-endian. The link
and reach masks are block-local (Polaris 4 finish): bit k of a block's mask = its NBRS entry nbr[k], so a mask names
any of up to 64 blocks (world_i.h WMask) in 16 bits:
  BLKS v2  per block (64 B) char pack[16] (pack name under PACK_ROOT without '.pak', NUL padded), char map[16] (MSB
           id); f32 min x, min z, max x, max z (xz bounds of the block's collision, world frame = the maps' shared
           frame); f32 min y, max y (its collision's height range: the Asylum lies over Firelink and the Burg in x / z);
           u32 flags (bit 0: boot block, already loaded by main.c), u32 neighbour count (its NBRS count; v1 until
           2026-11-27: a u32 neighbour mask, bit n = block n)
  NBRS v1  per block (16 B) u8 count, u8 nbr[15]: the block's neighbours (global block numbers, ascending, 0 padded):
           every block in a load, keep or reach mask of its triangles plus its LINK_GAP / ConnectCollision neighbours,
           itself excluded
  LRNL v1  per block: u32 first link, u32 links (into LNKL)
  LNKL v1  per link (12 B) u32 first triangle, u16 triangles, u16 load mask, u16 keep mask, u16 0: while the player
           stands on triangles [first, first + n) of this block's collision.bin, the blocks in `load` are wanted (read
           in the background) and the blocks in `keep` stay loaded; a block in neither is unloaded. Sorted by first
           triangle; triangles with no link record want nothing.
  RCHL v1  (tools/worldreach.py; world.c WORLD_REACH wants a block once the player could reach its seam before it turns
           on) u32 reach runs (total), u32 reach entries (0: none), f32 metres per distance unit; per block u32 first
           reach run, u32 reach runs; u16 first triangle per run, then u8 reach entry per run (1-based, 0 none), padded
           to 4 bytes: a run covers its block's triangles from its first to the next run's first (or the last), sorted
           by first triangle; per entry (8 B) u16 block mask, u8 distance[6]: walking distance in worldreach.REACH_STEP
           units (rounded down) from the run's triangles to the seam of each block in the mask, low bit first
  A block with more than NBR_MAX neighbours switches all three to the wide layout: NBRS v2 (32 B: u8 count,
  u8 nbr[31]), LNKL v2 (16 B: u32 first, u16 n, u16 0, u32 load, u32 keep), RCHL v2 (12 B entries: u32 mask, u8
  distance[6], 2 B 0). The runtime reads v1 only (it refuses v2).
  (until 2026-11-27 also LRNG v1 / LINK v2 / REAC v2: the same with global u32 masks, WORLD_MAX 32; before Polaris
  3.3: one 'DSW1' blob, version 5, of the same sections behind a 16-byte header)
Links (TECH_DEBT world-3):
- between two MSBs, DSR's own rule: a ConnectCollision part (MSB type 11) names a collision part of map A and a map B;
  DSR loads B while the player stands on that part. Every triangle of such a part in block i links the block of map B
  holding most of the part's triangle centroids (its cells / bounds), load and keep alike.
- inside one MSB DSR has no rule (the whole MSB stays resident: m10.arealoadlist); the PS2 blocks are cut on the
  32 m cells export_map.py groups chunks by, so a triangle loads a neighbour within LOAD_CELLS cells of its cells and
  keeps it within KEEP_CELLS (ASSUMED lead: one cut cell gives ~6 s at dash speed for a ~1 MB lite set, host: 0.7 s,
  cdrom0: 2.9 s; the second cell is hysteresis against loading / unloading at the edge)."""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import area, chunkfile as chunk, souls, worldlinks, worldreach

LINK_GAP = 20.0  # metres between two blocks' bounds that still count as touching (ASSUMED: one load radius apart)
LOAD_CELLS, KEEP_CELLS = 1.0, 2.0  # in area.CELL units, see above
# REAC (world.c WORLD_REACH): the reach table, walking distances to the seams (tools/worldreach.py). WORLD_REACH=0
# (env): no reach entries (the links alone)
REACH = os.environ.get("WORLD_REACH", "1") != "0"
NBR_MAX = 15  # neighbours per block in NBRS v1 (u16 local masks: LNKL v1, RCHL v1; world_i.h)
NBR_WIDE = 31  # the wide layout's (NBRS / LNKL / RCHL v2, u32 local masks)


def col_bounds(path):
    """xz bounds of a collision.bin (tools/export_collision.py header: grid origin x0, z0, cell, gw, gd)"""
    with open(path, "rb") as f:
        h = f.read(64)
    assert h[:4] == b"DCOL", path
    x0, z0, cell = struct.unpack_from("<3f", h, 32)
    gw, gd = struct.unpack_from("<2I", h, 44)
    if struct.unpack_from("<I", h, 4)[0] == 2:  # v2 (export_collision.write_v2): the 8 m top grid overshoots; v1's 2 m
        return _col_bounds_v2(path)            # grid from the same origin, as v1 wrote it
    return x0, z0, x0 + gw * cell, z0 + gd * cell


def _col_bounds_v2(path):
    with open(path, "rb") as f:
        b = f.read()
    ox, oy, oz, sc, x0, z0 = struct.unpack_from("<6f", b, 16)
    off, nfrag = struct.unpack_from("<2I", b, 52)
    fcell = struct.unpack_from("<f", b, 92)[0]
    hx = max(struct.unpack_from("<3h", b, off + 32 * i + 6)[0] for i in range(nfrag)) * sc + ox
    hz = max(struct.unpack_from("<3h", b, off + 32 * i + 6)[2] for i in range(nfrag)) * sc + oz
    gw, gd = int((hx - x0) / fcell) + 1, int((hz - z0) / fcell) + 1
    return x0, z0, x0 + gw * fcell, z0 + gd * fcell


def col_height(path):
    """(min y, max y) of a collision.bin's vertices"""
    with open(path, "rb") as f:
        b = f.read()
    if struct.unpack_from("<I", b, 4)[0] == 2:  # v2 header: vertex height range
        return struct.unpack_from("<2f", b, 84)
    nv = struct.unpack_from("<I", b, 8)[0]
    oy, scale = struct.unpack_from("<f", b, 20)[0], struct.unpack_from("<f", b, 28)[0]
    ov = struct.unpack_from("<I", b, 52)[0]
    ys = [oy + v[1] * scale for v in struct.iter_unpack("<3h", b[ov:ov + 6 * nv])]
    return min(ys), max(ys)


msb, connect_parts = worldlinks.msb, worldlinks.connect_parts


def crop(game, block, m):
    """the block's collision crop in collision.bin order: [(part, triangles)]"""
    import export_collision
    if block == m:
        os.environ.pop("AREA_BLOCK", None)
    else:
        os.environ["AREA_BLOCK"] = block
    try:
        subs, _, _ = export_collision.area_collision(game, m)
    finally:
        os.environ.pop("AREA_BLOCK", None)
    return [(s["part"], s["tris"]) for s in subs]


def rect_d(rs, x, z):
    """xz distance from (x, z) to the nearest of rectangles [(x0, z0, x1, z1)]"""
    d = 1e30
    for r in rs:
        dx = max(r[0] - x, 0.0, x - r[2])
        dz = max(r[1] - z, 0.0, z - r[3])
        d = min(d, (dx * dx + dz * dz) ** 0.5)
    return d


def block_masks(game, names, nbr, blocks, i):
    """block i's [load, keep] masks per collision triangle (collision.bin order) and its reach per triangle:
    worldreach.encode's (mask, distances), (0, ()) for none, None for a wall (no floor anyone stands on: takes the
    reach of the run it falls in)"""
    b, m, bb, rect = blocks[i]
    masks = []  # per triangle [load, keep]
    sub = crop(game, b, m)
    for part, tris in sub:
        for t in tris:
            load = keep = 0
            for j, (_, mj, bj, rj) in enumerate(blocks):
                if nbr[i] >> j & 1 and mj == m and rj and rect:
                    d = min(rect_d(rj, p[0], p[2]) for p in t)
                    load |= (d <= LOAD_CELLS * area.CELL) << j
                    keep |= (d <= KEEP_CELLS * area.CELL) << j
            masks.append([load, keep])
    # between MSBs: DSR's ConnectCollision parts (tools/worldlinks.py, shared with the map exporters' spot budgets)
    cross = worldlinks.cross_masks(game, names, nbr, i, sub)
    for t, mk in enumerate(cross):
        masks[t][0] |= mk
        masks[t][1] |= mk
    reach = [(0, ())] * len(masks)
    if REACH:  # walking distances to the neighbours' seams (tools/worldreach.py)
        rects = [bj[3] for bj in blocks]
        maps = [bj[1] for bj in blocks]
        walk, dists = worldreach.block_reach(sub, i, nbr, rects, maps, cross)
        reach = [worldreach.encode(d) if d else (0, ()) if w else None for w, d in zip(walk, dists)]
    return masks, reach


def pack_world(blocks, heights, nbr, per):
    """world.bin's bytes from blocks [(name, map, xz bounds, cell rects)], heights [(min y, max y)], neighbour masks
    and per block block_masks' (masks, reach) (the LNKL / RCHL runs are cut here; tools/ci/test_worldbin.py reads
    them back). Prints the per-block link summary and the totals."""
    n = len(blocks)
    # the neighbour lists first: their largest count picks the layout (NBR_MAX)
    nbrs = [worldlinks.neighbours(i, nbr[i], *per[i]) for i in range(n)]
    top = max(map(len, nbrs), default=0)
    print(f"world.bin: neighbours max {top}")
    assert top <= NBR_WIDE, f"world.bin: {top} neighbours, NBRS holds {NBR_WIDE}"
    reach, reach_ix, rruns, rranges = [], {}, [], []
    for i, (b, m, bb, rect) in enumerate(blocks):
        masks, rc = per[i]
        # reach runs (global masks; RCHL turns each entry local): a wall triangle (None) joins the run it lies in
        first = len(rruns)
        t = 0
        while t < len(rc):
            e, cur = t, rc[t]
            while e < len(rc) and (rc[e] is None or cur is None or rc[e] == cur):
                cur = cur if cur is not None else rc[e]
                e += 1
            ri = 0
            if cur and cur[0]:
                if cur not in reach_ix:
                    reach.append(cur)
                    reach_ix[cur] = len(reach)
                ri = reach_ix[cur]
            if (rruns[-1][1] if len(rruns) > first else 0) != ri:  # (a run with no reach only after one with)
                rruns.append((t, ri))
            t = e
        rranges.append((first, len(rruns) - first))
    assert all(len(m) <= 0xffff for m, _ in per), "reach runs: u16 triangles"
    loc, lrnl, nlent = local_chunks(per, nbrs, top > NBR_MAX, rruns, rranges, reach)
    for i, (b, m, bb, rect) in enumerate(blocks):
        masks, rc = per[i]
        wl = [sum(1 for mk in masks if mk[0] >> j & 1) for j in range(n)]
        print(f"world block {i} {b} ({m}): x {bb[0]:.0f}..{bb[2]:.0f} z {bb[1]:.0f}..{bb[3]:.0f} "
              f"y {heights[i][0]:.0f}..{heights[i][1]:.0f}, neighbours "
              f"{[blocks[j][0] for j in range(n) if nbr[i] >> j & 1]}, {lrnl[i][1]} links over {len(masks)} "
              f"tris (load triangles per block {wl}, reach triangles "
              f"{[sum(1 for r in rc if r and r[0] >> j & 1) for j in range(n)]}, {rranges[i][1]} reach runs)")
    # BLKS v2: the neighbour count where v1 had the u32 mask (the list is NBRS)
    blks = b"".join(struct.pack("<16s16s6f2I", b.encode(), m.encode(), *bb, *heights[i], 1 if i == 0 else 0,
                                len(nbrs[i])) for i, (b, m, bb, _) in enumerate(blocks))
    assert all(len(b) < 16 and len(m) < 16 for b, m, _, _ in blocks)
    blob = chunk.pack(b"WRLD", [(b"BLKS", 2, blks)] + loc)
    print(f"world.bin: {sum(c for _, c in lrnl)} links, {len(rruns)} reach runs, {nlent} reach entries, "
          f"{len(blob)} bytes")
    return blob


def local_chunks(per, nbrs, wide, rruns, rranges, reach):
    """NBRS, LRNL, LNKL, RCHL (see the top) from block_masks' per-block (masks, reach), the neighbour lists and
    pack_world's reach runs / ranges / entries (global masks). The triangle masks turn local before the runs are cut.
    Returns (chunks, LRNL's (first, links) per block, reach entries)"""
    nmax = NBR_WIDE if wide else NBR_MAX
    nb = b"".join(struct.pack(f"<B{nmax}B", len(ns), *(ns + [0] * (nmax - len(ns)))) for ns in nbrs)
    lrnl, lnkl = [], []
    for i, (masks, _) in enumerate(per):
        loc = [[worldlinks.to_local(x, nbrs[i]) for x in mk] for mk in masks]
        first = len(lnkl)
        t = 0
        while t < len(loc):
            e = t
            while e < len(loc) and loc[e] == loc[t] and e - t < 0xffff:
                e += 1
            if any(loc[t]):
                lnkl.append((t, e - t, *loc[t]))
            t = e
        lrnl.append((first, len(lnkl) - first))
    # reach: the runs as they are (within a block the local mapping is one to one, so the runs do not change), each
    # run's entry turned local; entries shared by equal local (mask, distances)
    lent, lix, lruns = [], {}, []
    for i, (r0, rn) in enumerate(rranges):
        for t0, ri in rruns[r0:r0 + rn]:
            li = 0
            if ri:
                e = worldreach.encode_local(reach[ri - 1], nbrs[i])
                if e not in lix:
                    lent.append(e)
                    lix[e] = len(lent)
                li = lix[e]
            lruns.append((t0, li))
    assert len(lent) < 256, "RCHL: u8 entries"
    rchl = bytearray(struct.pack("<2If", len(lruns), len(lent), worldreach.REACH_STEP))
    for r in rranges:
        rchl += struct.pack("<2I", *r)
    rchl += b"".join(struct.pack("<H", r[0]) for r in lruns)
    rchl += bytes(r[1] for r in lruns) + bytes(-(3 * len(lruns)) % 4)
    pad = (0,) * worldreach.REACH_N
    if wide:
        lnk = b"".join(struct.pack("<IHHII", l[0], l[1], 0, l[2], l[3]) for l in lnkl)
        rchl += b"".join(struct.pack("<I6B2x", m, *(ds + pad)[:worldreach.REACH_N]) for m, ds in lent)
    else:
        lnk = b"".join(struct.pack("<IHHHH", l[0], l[1], l[2], l[3], 0) for l in lnkl)
        rchl += b"".join(struct.pack("<H6B", m, *(ds + pad)[:worldreach.REACH_N]) for m, ds in lent)
    v = 2 if wide else 1
    print(f"world.bin: local chunks v{v}: {len(lnkl)} links, {len(lent)} reach entries")
    return ([(b"NBRS", v, nb), (b"LRNL", 1, struct.pack(f"<{2 * len(lrnl)}I", *(x for r in lrnl for x in r))),
             (b"LNKL", v, lnk), (b"RCHL", v, bytes(rchl))], lrnl, len(lent))


def block_nbr(game, blocks):
    """per block its neighbour mask (bit j = block j): the blocks within LINK_GAP of its bounds in the same MSB or one
    a ConnectCollision part joins it to (block_masks' nbr; NBRS adds the blocks its masks name)"""
    nbr = [0] * len(blocks)
    for i, (a, ma, ba, _) in enumerate(blocks):
        for j, (b, mb, bb, _) in enumerate(blocks):
            gap = max(ba[0] - bb[2], bb[0] - ba[2], ba[1] - bb[3], bb[1] - ba[3])
            if i != j and gap <= LINK_GAP and (ma == mb or connect_parts(game, ma, mb) or connect_parts(game, mb, ma)):
                nbr[i] |= 1 << j
    return nbr


def main():
    game, out, boot, gen = sys.argv[1:5]
    names = area.WORLDS.get(boot, [boot])
    blocks, heights = [], []
    for i, b in enumerate(names):
        m = area.BLOCKS[b]["map"] if b in area.BLOCKS else b
        col = os.path.join(gen, "collision.bin") if i == 0 else os.path.join(gen, b, "collision.bin")
        cells = area.BLOCKS.get(b, {}).get("cells")
        c = area.CELL
        rect = [(r[0] * c, r[1] * c, (r[2] + 1) * c, (r[3] + 1) * c) for r in area.cell_rects(cells)] if cells else None
        blocks.append((b, m, col_bounds(col), rect))
        heights.append(col_height(col))
    n = len(blocks)
    assert n <= 64, "planner sets are u64 (world_i.h WMask)"
    nbr = block_nbr(game, blocks)
    # the blocks' link masks are independent of each other (each block's collision crop is ~1-10 s of Python): worked
    # out side by side in forked processes, put together in block order as before. EXPORT_WORLD_JOBS=1: one at a time
    jobs = int(os.environ.get("EXPORT_WORLD_JOBS", "0")) or min(n, os.cpu_count() or 1)
    args = [(game, names, nbr, blocks, i) for i in range(n)]
    if jobs > 1 and n > 1:
        import multiprocessing
        with multiprocessing.get_context("fork").Pool(min(jobs, n)) as pool:
            per = pool.starmap(block_masks, args, chunksize=1)
    else:
        per = [block_masks(*a) for a in args]
    blob = pack_world(blocks, heights, nbr, per)
    with open(out, "wb") as f:
        f.write(blob)

if __name__ == "__main__":
    main()
