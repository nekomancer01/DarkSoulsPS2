"""World block table for the PS2 block manager (src/world.c, TECH_DEBT build-5): which block packs exist, where each
one lies, which ones neighbour it and which of its collision triangles load which neighbour. Blocks are defined in
tools/area.py (WORLDS / BLOCKS); this script reads their exported collision.bin files for the bounds, the same
collision crops (export_collision.area_collision, collision.bin's triangle order) for the links, and the game's MSB
ConnectCollision parts (tools/worldgraph.py) for the links between maps.

  export_world.py <game dir> <out world.bin> <boot map> <gen dir>

world.bin (little-endian):
  header  u32 'DSW1', version 3, blocks, links (total)
  block   (64 B) char pack[16] (pack name under PACK_ROOT without '.pak', NUL padded), char map[16] (MSB id);
          f32 min x, min z, max x, max z (xz bounds of the block's collision, world frame = the maps' shared frame);
          f32 min y, max y (its collision's height range: the Asylum lies over Firelink and the Burg in x / z);
          u32 flags (bit 0: boot block, already loaded by main.c), u32 neighbour mask (bit n = block n)
  (version 2: char pack[24], no height range)
  ranges  per block: u32 first link, u32 links (into the link table)
  link    (8 B) u32 first triangle, u16 triangles, u8 load mask, u8 keep mask: while the player stands on triangles
          [first, first + n) of this block's collision.bin, the blocks in `load` are wanted (read in the background)
          and the blocks in `keep` stay loaded; a block in neither is unloaded. Sorted by first triangle; triangles
          with no link record want nothing.
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
import area, souls

LINK_GAP = 20.0  # metres between two blocks' bounds that still count as touching (ASSUMED: one load radius apart)
LOAD_CELLS, KEEP_CELLS = 1.0, 2.0  # in area.CELL units, see above


def col_bounds(path):
    """xz bounds of a collision.bin (tools/export_collision.py header: grid origin x0, z0, cell, gw, gd)"""
    with open(path, "rb") as f:
        h = f.read(64)
    assert h[:4] == b"DCOL", path
    x0, z0, cell = struct.unpack_from("<3f", h, 32)
    gw, gd = struct.unpack_from("<2I", h, 44)
    return x0, z0, x0 + gw * cell, z0 + gd * cell


def col_height(path):
    """(min y, max y) of a collision.bin's vertices"""
    with open(path, "rb") as f:
        b = f.read()
    nv = struct.unpack_from("<I", b, 8)[0]
    oy, scale = struct.unpack_from("<f", b, 20)[0], struct.unpack_from("<f", b, 28)[0]
    ov = struct.unpack_from("<I", b, 52)[0]
    ys = [oy + v[1] * scale for v in struct.iter_unpack("<3h", b[ov:ov + 6 * nv])]
    return min(ys), max(ys)


def msb(game, m, cache={}):
    if m not in cache:
        cache[m] = souls.msb1(souls.read(f"{game}/map/MapStudio/{m}.msb"))
    return cache[m]


def connect_parts(game, src, dst):
    """collision part names of MSB src whose ConnectCollision part targets map dst (-1 fields = any)"""
    want = [int(x) for x in dst[1:].split("_")]
    return {p["collision"] for p in msb(game, src)["parts"]
            if p["type"] == 11 and all(v < 0 or v == w for v, w in zip(p["map_id"], want))}


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
    assert n <= 8, "load / keep masks are u8 (world.c WORLD_MAX)"
    nbr = [0] * n
    for i, (a, ma, ba, _) in enumerate(blocks):
        for j, (b, mb, bb, _) in enumerate(blocks):
            gap = max(ba[0] - bb[2], bb[0] - ba[2], ba[1] - bb[3], bb[1] - ba[3])
            if i != j and gap <= LINK_GAP and (ma == mb or connect_parts(game, ma, mb) or connect_parts(game, mb, ma)):
                nbr[i] |= 1 << j
    links, ranges = [], []
    for i, (b, m, bb, rect) in enumerate(blocks):
        masks, by_part = [], {}  # per triangle [load, keep]; part -> its triangle indices
        sub = crop(game, b, m)
        for part, tris in sub:
            for t in tris:
                by_part.setdefault(part, []).append(len(masks))
                load = keep = 0
                for j, (_, mj, bj, rj) in enumerate(blocks):
                    if nbr[i] >> j & 1 and mj == m and rj and rect:
                        d = min(rect_d(rj, p[0], p[2]) for p in t)
                        load |= (d <= LOAD_CELLS * area.CELL) << j
                        keep |= (d <= KEEP_CELLS * area.CELL) << j
                masks.append([load, keep])
        cen = [(sum(p[0] for p in t) / 3, sum(p[2] for p in t) / 3) for _, tris in sub for t in tris]
        for mj in {blocks[j][1] for j in range(n) if nbr[i] >> j & 1 and blocks[j][1] != m}:
            for part in connect_parts(game, m, mj) & set(by_part):
                # the block of map mj holding most of the part's triangle centroids in its cells; a part over no
                # block's cells (the Parish elevator next to Firelink, not a block yet) links nothing. A whole-map
                # block (no cells) is DSR's own unit and takes the part wherever it lies: the Asylum's crow ledge
                # (h0054B1, z -95..-82) is outside Firelink's collision bounds (z >= -51)
                ts = by_part[part]
                inside = lambda rs: sum(any(r[0] <= cen[t][0] < r[2] and r[1] <= cen[t][1] < r[3] for r in rs) for t in ts)
                cnt = {j: inside(blocks[j][3]) if blocks[j][3] else len(ts)
                       for j in range(n) if nbr[i] >> j & 1 and blocks[j][1] == mj}
                j = max(cnt, key=cnt.get, default=None)
                if j is not None and cnt[j]:
                    for t in ts:
                        masks[t][0] |= 1 << j
                        masks[t][1] |= 1 << j
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
              f"tris (load triangles per block {wl})")
    blob = bytearray(struct.pack("<4s3I", b"DSW1", 3, n, len(links)))
    for i, (b, m, bb, _) in enumerate(blocks):
        assert len(b) < 16 and len(m) < 16
        blob += struct.pack("<16s16s6f2I", b.encode(), m.encode(), *bb, *heights[i], 1 if i == 0 else 0, nbr[i])
    for r in ranges:
        blob += struct.pack("<2I", *r)
    for l in links:
        blob += struct.pack("<IH2B", *l)
    with open(out, "wb") as f:
        f.write(blob)


if __name__ == "__main__":
    main()
