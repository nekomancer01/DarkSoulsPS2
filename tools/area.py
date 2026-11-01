"""Per-area selection shared by every exporter: where the player starts and which part of the MSB the PS2 build holds.

The PS2 runtime keeps one area resident (no level streaming yet, TECH_DEBT build-5). A DS1 map file can be much bigger
than that: m10_01_00_00 is Undead Burg + Lower Burg + Undead Parish (166 enemies, 414 breakables, 152k collision
triangles). Every exporter therefore asks this module for
  spawn_part(M, map_id)   the MSB player start (type 4) the build boots at
  region(M, map_id)       a test keep(x, y, z) for placed things (collision triangles, enemies, objects, navmesh)
  config(map_id)          per-area values (AREAS + env), e.g. map_radius for export_map.py
  visible_groups(...)     DSR's own visibility: the draw groups shown while standing on the region's collision parts;
                          map pieces outside them are never drawn from the region and are skipped

Defaults come from the game's data; AREAS overrides values chosen to fit the PS2 RAM / VRAM budget (HANDOFF "EE RAM
budget"), each with a comment. An area without an entry is exported whole from its first player start.
Env overrides for experiments: AREA_SPAWN=<part name>, AREA_RADIUS=<metres> (0 = whole map), AREA_MAP_RADIUS=<metres>,
AREA_DRAW_FROM=<collision part,...> (export only the map pieces DSR draws while standing on those parts)."""
import math, os

AREAS = {
    # Firelink Shrine: the whole MSB fits (the test area everything was tuned on). Spawn = first player start, which is
    # also the one next to the bonfire.
    "m10_02_00_00": dict(),
    # Undead Burg (+ Lower Burg + Parish in the same MSB). Spawn at the Undead Burg bonfire (o0200_0001; its player
    # start c0000_0009 is 2.6 m away: picked by the nearest-bonfire rule, no override needed). Radius 70 m around it
    # (xz) is the budget choice: the upper Burg from the bonfire to the Taurus Demon bridge tower and the Hellkite bridge
    # stairs, the rest of the MSB needs build-5 streaming. Collision then fits the u16 indices of collision.bin.
    # map_radius: map triangles centred farther than this (xz) are not exported. Burg's pieces are dense (1.25M LOD0
    # triangles in the visible draw groups, 880k within 100 m; Firelink 334k in all): the exporter's memory (~8 KB per
    # source triangle at its QEM peak) and the 76k triangle budget both need the cut; the sky layer is kept whole.
    "m10_01_00_00": dict(radius=70.0, map_radius=100.0),
}

BONFIRE = "o0200"

# World blocks (TECH_DEBT build-5, src/world.c): the units the runtime loads and unloads by player position. DSR's own
# block is the whole MSB, and an area load list keeps several resident (m10: m10_00, m10_01, m10_02, worldgraph.py); the
# PS2 holds a cropped part of an MSB per block (ASSUMED budget cut, TECH_DEBT world-1). All maps of a world group share
# one frame (no MapOffset event in m10), so blocks need no offset. Each block is exported like an area (same
# exporters, same knobs) with the block's entries overriding AREAS: env AREA_BLOCK=<block name> (Makefile BLOCK=).
# Block name = output directory / pack name. The boot area's block is the plain area export (no entry needed).
# Keys: map (MSB), spawn (player start the crop is centred on), radius / map_radius as AREAS.
BLOCKS = {
    # Undead Burg, west: the stairs from Firelink's aqueduct (connect collisions h1070B1 / h1095B1 / h1127B1 ->
    # m10_02) and the first Burg stretch. Centred on the player start c0000_0000 (-47.8, -21.8, -35.6), DSR's arrival
    # point from the aqueduct. Radii are the RAM budget (world.c full pool + lite slot next to Firelink): radius 70 /
    # map_radius 100 gave 8.8 MB of resident streams + 3.4 MB lite set and left a 1.3 MB near-cell arena.
    "m10_01_00_00_w": dict(map="m10_01_00_00", spawn="c0000_0000", radius=50.0, map_radius=70.0),
}
# Blocks of a world build: the boot area first (the default build's own pack), then the blocks world.c may load
WORLDS = {
    "m10_02_00_00": ["m10_02_00_00", "m10_01_00_00_w"],
}


def block():
    """The block being exported (env AREA_BLOCK), or None for a plain area export"""
    b = os.environ.get("AREA_BLOCK")
    return (b, BLOCKS[b]) if b and b in BLOCKS else None


def config(map_id):
    c = dict(AREAS.get(map_id, {}))
    blk = block()
    if blk and blk[1]["map"] == map_id:
        c.update({k: v for k, v in blk[1].items() if k != "map"})
    if os.environ.get("AREA_SPAWN"):
        c["spawn"] = os.environ["AREA_SPAWN"]
    if os.environ.get("AREA_RADIUS"):
        c["radius"] = float(os.environ["AREA_RADIUS"]) or None
    if os.environ.get("AREA_MAP_RADIUS"):
        c["map_radius"] = float(os.environ["AREA_MAP_RADIUS"]) or None
    return c


def spawn_part(M, map_id):
    """The player start (MSB part type 4) to boot at: AREAS 'spawn' by name, else the one nearest to a placed bonfire
    (object part o0200: DSR warps the player to a bonfire's start point), else the first one."""
    c = config(map_id)
    starts = [p for p in M["parts"] if p["type"] == 4]
    if c.get("spawn"):
        return next(p for p in starts if p["name"] == c["spawn"])
    fires = [p["pos"] for p in M["parts"] if p["type"] == 1 and p["model"] == BONFIRE]
    if not fires:
        return starts[0]
    return min(starts, key=lambda p: min(math.dist(p["pos"], f) for f in fires))  # min keeps the first on ties


def region(M, map_id):
    """keep(x, y, z) -> bool for placed things; everything when the area has no radius."""
    r = config(map_id).get("radius")
    if not r:
        return lambda x, y, z: True
    sx, _, sz = spawn_part(M, map_id)["pos"]
    r2 = r * r
    return lambda x, y, z: (x - sx) ** 2 + (z - sz) ** 2 <= r2


def is_whole(map_id):
    return not config(map_id).get("radius")


def visible_groups(msb_bytes, M, map_id, coll_parts):
    """Union of the draw groups of the collision parts that have a triangle inside the region (coll_parts: names);
    None = no filtering (whole area)."""
    if os.environ.get("AREA_DRAW_FROM"):  # experiment: only what DSR shows from these collision parts (comma list)
        coll_parts = set(os.environ["AREA_DRAW_FROM"].split(","))
    elif is_whole(map_id):
        return None
    g = [0, 0, 0, 0]
    # a collision part's draw groups are the groups shown while the player stands on it; a map piece is drawn when
    # its own draw groups meet them
    for p in M["parts"]:
        if p["type"] == 5 and p["name"] in coll_parts:
            g = [a | b for a, b in zip(g, p[os.environ.get("AREA_DG_FIELD", "draw_groups")])]
    return g


def drawn(groups, part_dg):
    """A part in no draw group is always drawn: DSR shows the bonfires (o0200, DrawGroups all 0: Burg's
    o0200_0001 / _0002 were never exported) and corpses with empty groups (as tools/drawgroups.py, ASSUMED)."""
    return groups is None or not any(part_dg) or any(a & b for a, b in zip(groups, part_dg))


if __name__ == "__main__":
    import sys
    # Makefile: `area.py --world <boot map>` -> "<map>:<block>" for every other block of that world build
    if sys.argv[1:2] == ["--world"]:
        print(" ".join(f"{BLOCKS[b]['map']}:{b}" for b in WORLDS.get(sys.argv[2], [])[1:]))
