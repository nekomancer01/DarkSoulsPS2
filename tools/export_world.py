"""World block table for the PS2 block manager (src/world.c, TECH_DEBT build-5): which block packs exist, where each
one lies and which ones neighbour it. Blocks are defined in tools/area.py (WORLDS / BLOCKS); this script reads their
exported collision.bin files for the bounds and the game's MSB ConnectCollision parts (tools/worldgraph.py) for the
links between maps.

  export_world.py <game dir> <out world.bin> <boot map> <gen dir>

world.bin (little-endian):
  header  u32 'DSW1', version 1, blocks, 0
  block   (64 B) char pack[24] (pack name under PACK_ROOT without '.pak', NUL padded), char map[16] (MSB id);
          f32 min x, min z, max x, max z (xz bounds of the block's collision, world frame = the maps' shared frame);
          u32 flags (bit 0: boot block, already loaded by main.c), u32 neighbour mask (bit n = block n)
Neighbours: two blocks of the same MSB whose bounds touch (within LINK_GAP), or blocks of two MSBs where one has a
ConnectCollision part to the other (DSR loads the target map while the player stands on that part) and the bounds touch
too (a crop may hold no part near the link)."""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import area, souls

LINK_GAP = 20.0  # metres between two blocks' bounds that still count as touching (ASSUMED: one load radius apart)


def col_bounds(path):
    """xz bounds of a collision.bin (tools/export_collision.py header: grid origin x0, z0, cell, gw, gd)"""
    with open(path, "rb") as f:
        h = f.read(64)
    assert h[:4] == b"DCOL", path
    x0, z0, cell = struct.unpack_from("<3f", h, 32)
    gw, gd = struct.unpack_from("<2I", h, 44)
    return x0, z0, x0 + gw * cell, z0 + gd * cell


def connects(game, src, dst):
    """MSB src has a ConnectCollision part (type 11) whose target matches map dst (-1 = any)"""
    M = souls.msb1(souls.read(f"{game}/map/MapStudio/{src}.msb"))
    want = [int(x) for x in dst[1:].split("_")]
    return any(p["type"] == 11 and all(v < 0 or v == w for v, w in zip(p["map_id"], want)) for p in M["parts"])


def main():
    game, out, boot, gen = sys.argv[1:5]
    names = area.WORLDS.get(boot, [boot])
    blocks = []
    for i, b in enumerate(names):
        msb = area.BLOCKS[b]["map"] if b in area.BLOCKS else b
        col = os.path.join(gen, "collision.bin") if i == 0 else os.path.join(gen, b, "collision.bin")
        blocks.append((b, msb, col_bounds(col)))
    link = {}
    for i, (a, ma, ba) in enumerate(blocks):
        for j, (b, mb, bb) in enumerate(blocks):
            if i == j:
                continue
            gap = max(ba[0] - bb[2], bb[0] - ba[2], ba[1] - bb[3], bb[1] - ba[3])
            if gap > LINK_GAP:
                continue
            if ma == mb or (ma, mb) in link or connects(game, ma, mb) or connects(game, mb, ma):
                link[(ma, mb)] = link[(mb, ma)] = True
                link[(i, j)] = True
    blob = bytearray(struct.pack("<4s3I", b"DSW1", 1, len(blocks), 0))
    for i, (b, msb, bb) in enumerate(blocks):
        mask = sum(1 << j for j in range(len(blocks)) if link.get((i, j)))
        assert len(b) < 24 and len(msb) < 16
        blob += struct.pack("<24s16s4f2I", b.encode(), msb.encode(), *bb, 1 if i == 0 else 0, mask)
        print(f"world block {i} {b} ({msb}): x {bb[0]:.0f}..{bb[2]:.0f} z {bb[1]:.0f}..{bb[3]:.0f}, neighbours "
              f"{[blocks[j][0] for j in range(len(blocks)) if mask >> j & 1]}")
    with open(out, "wb") as f:
        f.write(blob)


if __name__ == "__main__":
    main()
