"""World block table for the PS2 block manager (src/world.c, TECH_DEBT build-5): which block packs exist, where each
one lies, which ones neighbour it and which of its collision triangles load which neighbour. Blocks are defined in
tools/area.py (WORLDS / BLOCKS); this script reads their exported collision.bin files for the bounds, the same
collision crops (export_collision.area_collision, collision.bin's triangle order) for the links, and the game's MSB
ConnectCollision parts (tools/worldgraph.py) for the links between maps.

  export_world.py <game dir> <out world.bin> <boot map> <gen dir>

world.bin (little-endian):
  header  u32 'DSW1', version 5, blocks, links (total)
  block   (64 B) char pack[16] (pack name under PACK_ROOT without '.pak', NUL padded), char map[16] (MSB id);
          f32 min x, min z, max x, max z (xz bounds of the block's collision, world frame = the maps' shared frame);
          f32 min y, max y (its collision's height range: the Asylum lies over Firelink and the Burg in x / z);
          u32 flags (bit 0: boot block, already loaded by main.c), u32 neighbour mask (bit n = block n)
  (version 3: 8 B links with u8 masks, at most 8 blocks; version 2: char pack[24], no height range)
  ranges  per block: u32 first link, u32 links (into the link table)
  link    (12 B) u32 first triangle, u16 triangles, u16 load mask, u16 keep mask, u16 0: while the player stands on
          triangles [first, first + n) of this block's collision.bin, the blocks in `load` are wanted (read in the
          background) and the blocks in `keep` stay loaded; a block in neither is unloaded. Sorted by first triangle;
          triangles with no link record want nothing.
  (version 5, tools/worldreach.py; world.c WORLD_REACH wants a block once the player could reach its seam before it
  turns on:)
  u32 reach runs (total), u32 reach entries, f32 metres per distance unit
  ranges  per block: u32 first reach run, u32 reach runs
  runs    u16 first triangle per run, then u8 reach entry per run (1-based, 0 none), padded to 4 bytes: a run covers
          its block's triangles from its first to the next run's first (or the last), sorted by first triangle
  entry   (8 B) u16 block mask, u8 distance[6]: walking distance in worldreach.REACH_STEP units (rounded down) from the
          run's triangles to the seam of each block in the mask, low bit first
  (version 4 ends after the links)
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
import area, souls, worldlinks, worldreach

LINK_GAP = 20.0  # metres between two blocks' bounds that still count as touching (ASSUMED: one load radius apart)
LOAD_CELLS, KEEP_CELLS = 1.0, 2.0  # in area.CELL units, see above
# version 5 (world.c WORLD_REACH): the reach table, walking distances to the seams (tools/worldreach.py). WORLD_REACH=0
# (env): no reach entries (the links as version 4 had them; world.c reads the file as before)
REACH = os.environ.get("WORLD_REACH", "1") != "0"


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
    assert n <= 16, "load / keep masks are u16 (world.c WORLD_MAX)"
    nbr = [0] * n
    for i, (a, ma, ba, _) in enumerate(blocks):
        for j, (b, mb, bb, _) in enumerate(blocks):
            gap = max(ba[0] - bb[2], bb[0] - ba[2], ba[1] - bb[3], bb[1] - ba[3])
            if i != j and gap <= LINK_GAP and (ma == mb or connect_parts(game, ma, mb) or connect_parts(game, mb, ma)):
                nbr[i] |= 1 << j
    links, ranges, reach, reach_ix, rruns, rranges = [], [], [], {}, [], []
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
    for i, (b, m, bb, rect) in enumerate(blocks):
        masks, rc = per[i]
        # reach runs: a wall triangle (None) joins the run it lies in
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
        first = len(links)
        t = 0
        while t < len(masks):
            e = t
            while e < len(masks) and masks[e] == masks[t] and e - t < 0xffff:
                e += 1
            if any(masks[t]):
                links.append((t, e - t, *masks[t]))
            t = e
        ranges.append((first, len(links) - first))
        wl = [sum(1 for mk in masks if mk[0] >> j & 1) for j in range(n)]
        print(f"world block {i} {b} ({m}): x {bb[0]:.0f}..{bb[2]:.0f} z {bb[1]:.0f}..{bb[3]:.0f} "
              f"y {heights[i][0]:.0f}..{heights[i][1]:.0f}, neighbours "
              f"{[blocks[j][0] for j in range(n) if nbr[i] >> j & 1]}, {len(links) - first} links over {len(masks)} "
              f"tris (load triangles per block {wl}, reach triangles "
              f"{[sum(1 for r in rc if r and r[0] >> j & 1) for j in range(n)]}, {rranges[-1][1]} reach runs)")
    blob = bytearray(struct.pack("<4s3I", b"DSW1", 5, n, len(links)))
    for i, (b, m, bb, _) in enumerate(blocks):
        assert len(b) < 16 and len(m) < 16
        blob += struct.pack("<16s16s6f2I", b.encode(), m.encode(), *bb, *heights[i], 1 if i == 0 else 0, nbr[i])
    for r in ranges:
        blob += struct.pack("<2I", *r)
    for l in links:
        blob += struct.pack("<I4H", *l, 0)
    assert len(reach) < 256 and all(len(m) <= 0xffff for m, _ in per), "reach runs: u8 entries, u16 triangles"
    blob += struct.pack("<2If", len(rruns), len(reach), worldreach.REACH_STEP)
    for r in rranges:
        blob += struct.pack("<2I", *r)
    blob += b"".join(struct.pack("<H", r[0]) for r in rruns)
    blob += bytes(r[1] for r in rruns) + bytes(-(3 * len(rruns)) % 4)
    for mask, ds in reach:
        blob += struct.pack("<H6B", mask, *(ds + (0,) * (worldreach.REACH_N - len(ds))))
    print(f"world.bin: {len(links)} links, {len(rruns)} reach runs, {len(reach)} reach entries, {len(blob)} bytes")
    with open(out, "wb") as f:
        f.write(blob)


if __name__ == "__main__":
    main()
