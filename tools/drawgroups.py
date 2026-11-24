"""DSR draw groups at runtime (TECH_DEBT area-5): dg.bin for src/drawgrp.c.

DSR draws a map piece / object while the collision part under the player has a draw group in common with the piece
(MSB1 DrawGroups, 4 x u32 on both). Many maps hold low-detail stand-ins for places seen from afar next to the detailed
pieces, each in its own groups; drawing every piece at once (tools/area.py's union of the region's collision groups is
all-ones in Undead Burg) draws both on top of each other. Checked on the data: with the collision's DrawGroups every
collision part's own floor is covered by a shown piece as often as with no filter at all (Burg 89.4 % of floor samples,
91.6 % unfiltered; Firelink 93.6 %, 93.6 %); DispGroups leaves the floor of Burg's h1112B1 uncovered.

Characters (MSB enemy parts, type 2) carry DrawGroups too: DSR draws / runs one only while it shares a group with the
collision part under the player (TECH_DEBT world-4). Their sets join the piece masks; the runtime (src/enemy.c) finds a
placement's set by its MSB position, which enemies.bin stores as the same f32 values (export_enemies.py), so the two
files need no common order and a cropped area / block keeps working.

dg.bin: a chunk file (tools/chunkfile.py, file id 'DGRP'; src/drawgrp.c reads it in place), little endian:
  DHDR v1  u32 piece masks P, collision masks C, chunks N, collision triangles T, placements E, switched entities S,
           area group A (the AA of mAA when the map is in an area load list: its maps share one draw-group bit space
           and the runtime applies the mask of the part under the player to every block of the group; 0 = the
           block's own sets only)
  PMSK v1  P x u32[4]  distinct draw-group sets of the exported pieces / objects (index 0 = all-ones: shown always)
  CMSK v1  C x u32[4]  distinct DrawGroups of the area's collision parts
  CHDG v1  N x u8      piece mask per chunk, in bump.bin's global chunk order (resident chunks, then near chunks)
  TRDG v1  T x u8      collision mask per collision.bin triangle (export_collision.area_collision order)
  CHRS v1  E x {f32 x, y, z; u32 piece mask}  every enemy part of the MSB
  ENTS v1  S x s32     entity ids of the pieces / objects events switch on / off (mapchunks.py CHUNK_ENT,
           TECH_DEBT event-5), ascending
  CHEN v1  N x u8      per chunk: 1 + index into those ids, 0 = no switched part
(before Polaris 3.3: one 'DGM3' blob of the same sections behind a 32-byte header, padded to 4 bytes between them)
Read back with read() below."""
import glob, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import chunkfile as chunk  # noqa: E402

ALL = (0xFFFFFFFF,) * 4


class Table:
    """Piece draw-group sets -> small indices (export_map gives each mesh one). With the area's collision draw groups
    set (collisions()), sets that every one of those collisions shows or hides alike share an index, so chunks are
    split only where the runtime filter can tell them apart (Burg: 194 sets -> fewer chunk splits)."""

    def __init__(self):
        self.masks, self.index, self.coll, self.area = [ALL], {}, None, 0

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


def _arealoadlists(game):
    import worldgraph
    return [[m for _, m, _ in worldgraph.loadlist(p) if m] for p in sorted(glob.glob(f"{game}/map/*/*.arealoadlist"))]


def area_group(game, map_id):
    """AA of map_id when an area load list keeps it resident with other maps (m10_01_00_00 -> 10), else 0"""
    return int(map_id[1:3]) if any(map_id in a and len(a) > 1 for a in _arealoadlists(game)) else 0


def group_collisions(game, map_id, M, reg_coll):
    """DrawGroups of the collision parts a piece of map_id can be seen from: the region's own (reg_coll: names in M),
    and with an area group every collision of every map of its area load list (another block of the same MSB, Firelink
    from the Burg)"""
    out = [p["draw_groups"] for p in M["parts"] if p["type"] == 5 and p["name"] in reg_coll]
    if not area_group(game, map_id):
        return out
    import souls
    for a in _arealoadlists(game):
        if map_id not in a:
            continue
        for m in a:
            path = f"{game}/map/MapStudio/{m}.msb"
            if os.path.exists(path):
                Mm = M if m == map_id else souls.msb1(souls.read(path))
                out += [p["draw_groups"] for p in Mm["parts"] if p["type"] == 5]
    return out


def write(path, table, chunk_dg, M, coll_subs, chunk_ent=None):
    """chunk_dg: piece mask index per chunk (global order); coll_subs: export_collision.area_collision(...)[0];
    chunk_ent: entity id per chunk (0 none), same order."""
    parts = {p["name"]: p for p in M["parts"] if p["type"] == 5}
    cidx, cmasks, tri = {}, [], []
    for s in coll_subs:
        p = parts.get(s["part"])
        g = tuple(p["draw_groups"]) if p else ALL
        if g not in cidx:
            cidx[g] = len(cmasks)
            cmasks.append(g)
        tri += [cidx[g]] * len(s["tris"])
    chars = [(p["pos"], table.add(p["draw_groups"])) for p in M["parts"] if p["type"] == 2]
    ents = sorted({e for e in chunk_ent or () if e})
    assert len(table.masks) < 256 and len(cmasks) < 256 and len(ents) < 256, "dg.bin indices are u8"
    eidx = {e: i + 1 for i, e in enumerate(ents)}
    n = chunk.write(path, b"DGRP", [
        (b"DHDR", 1, struct.pack("<7I", len(table.masks), len(cmasks), len(chunk_dg), len(tri), len(chars), len(ents),
                                 table.area)),
        (b"PMSK", 1, b"".join(struct.pack("<4I", *g) for g in table.masks)),
        (b"CMSK", 1, b"".join(struct.pack("<4I", *g) for g in cmasks)),
        (b"CHDG", 1, bytes(chunk_dg)),
        (b"TRDG", 1, bytes(tri)),
        (b"CHRS", 1, b"".join(struct.pack("<3fI", *pos, g) for pos, g in chars)),
        (b"ENTS", 1, struct.pack(f"<{len(ents)}i", *ents)),
        (b"CHEN", 1, bytes(eidx.get(e, 0) for e in (chunk_ent or [0] * len(chunk_dg))))])
    return len(table.masks), len(cmasks), n, len(ents)


class Dg:
    """dg.bin read back (read()): area, pieces / colls (lists of 4-tuples), chunk_dg / tri_dg / chunk_ent (bytes),
    chars [(x, y, z, piece mask)], ents [entity ids]"""

    def __init__(self, f):
        self.npieces, self.ncolls, self.nchunks, self.ntris, _e, _s, self.area = struct.unpack("<7I", f.get(b"DHDR", 1))
        pm, cm = f.get(b"PMSK", 1), f.get(b"CMSK", 1)
        self.pieces = [struct.unpack_from("<4I", pm, 16 * i) for i in range(len(pm) // 16)]
        self.colls = [struct.unpack_from("<4I", cm, 16 * i) for i in range(len(cm) // 16)]
        self.chunk_dg, self.tri_dg, self.chunk_ent = f.get(b"CHDG", 1), f.get(b"TRDG", 1), f.get(b"CHEN", 1)
        ch, en = f.get(b"CHRS", 1), f.get(b"ENTS", 1)
        self.chars = [struct.unpack_from("<3fI", ch, 16 * i) for i in range(len(ch) // 16)]
        self.ents = list(struct.unpack(f"<{len(en) // 4}i", en))


def read(path):
    """dg.bin -> Dg; None when the file does not exist (chunk.ChunkError when it is not a dg.bin this tool reads)"""
    if not os.path.exists(path):
        return None
    return Dg(chunk.read(path, b"DGRP"))
