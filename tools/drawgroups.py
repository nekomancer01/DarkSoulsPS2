"""DSR draw groups at runtime (TECH_DEBT area-5): dg.bin for src/drawgrp.c.

DSR draws a map piece / object while the collision part under the player has a draw group in common with the piece
(MSB1 DrawGroups, 4 x u32 on both). Many maps hold low-detail stand-ins for places seen from afar next to the detailed
pieces, each in its own groups; drawing every piece at once (tools/area.py's union of the region's collision groups is
all-ones in Undead Burg) draws both on top of each other. Checked on the data: with the collision's DrawGroups every
collision part's own floor is covered by a shown piece as often as with no filter at all (Burg 89.4 % of floor samples,
91.6 % unfiltered; Firelink 93.6 %, 93.6 %); DispGroups leaves the floor of Burg's h1112B1 uncovered.

dg.bin (little endian):
  u32 magic 'DGM1', piece masks P, collision masks C, chunks N, collision triangles T, 0, 0, 0
  P x u32[4]  distinct draw-group sets of the exported pieces / objects (index 0 = all-ones: shown always)
  C x u32[4]  distinct DrawGroups of the area's collision parts
  N x u8      piece mask per chunk, in bump.bin's global chunk order (resident chunks, then near chunks)
  T x u8      collision mask per collision.bin triangle (export_collision.area_collision order)
  (padded to 4 bytes)"""
import struct

ALL = (0xFFFFFFFF,) * 4


class Table:
    """Piece draw-group sets -> small indices (export_map gives each mesh one). With the area's collision draw groups
    set (collisions()), sets that every one of those collisions shows or hides alike share an index, so chunks are
    split only where the runtime filter can tell them apart (Burg: 194 sets -> fewer chunk splits)."""

    def __init__(self):
        self.masks, self.index, self.coll = [ALL], {}, None

    def collisions(self, cmasks):
        self.coll = sorted({tuple(c) for c in cmasks})

    def add(self, groups):
        g = tuple(groups) if any(groups) else ALL  # a piece in no group: never hidden (ASSUMED)
        key = tuple(any(a & b for a, b in zip(g, c)) for c in self.coll) if self.coll else g
        if self.coll and all(key):
            return 0  # shown from every collision of the area
        if key not in self.index:
            self.index[key] = len(self.masks)
            self.masks.append(g)  # any member decides the same for every collision of the area
        return self.index[key]


def write(path, table, chunk_dg, M, coll_subs):
    """chunk_dg: piece mask index per chunk (global order); coll_subs: export_collision.area_collision(...)[0]."""
    parts = {p["name"]: p for p in M["parts"] if p["type"] == 5}
    cidx, cmasks, tri = {}, [], []
    for s in coll_subs:
        p = parts.get(s["part"])
        g = tuple(p["draw_groups"]) if p else ALL
        if g not in cidx:
            cidx[g] = len(cmasks)
            cmasks.append(g)
        tri += [cidx[g]] * len(s["tris"])
    assert len(table.masks) < 256 and len(cmasks) < 256, "dg.bin indices are u8"
    b = struct.pack("<8I", 0x314D4744, len(table.masks), len(cmasks), len(chunk_dg), len(tri), 0, 0, 0)
    b += b"".join(struct.pack("<4I", *g) for g in table.masks)
    b += b"".join(struct.pack("<4I", *g) for g in cmasks)
    b += bytes(chunk_dg) + bytes(tri)
    b += b"\0" * (-len(b) % 4)
    with open(path, "wb") as f:
        f.write(b)
    return len(table.masks), len(cmasks), len(b)
