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
import math, os, sys

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
# PS2 holds a part of an MSB per block (ASSUMED budget cut, TECH_DEBT world-1). All maps of a world group share one
# frame (no MapOffset event in m10), so blocks need no offset. Each block is exported like an area (same exporters, same
# knobs) with the block's entries overriding AREAS: env AREA_BLOCK=<block name> (Makefile BLOCK=).
# Block name = output directory / pack name. The boot area's block is the plain area export (no entry needed).
# Keys: map (MSB); cells (x0, z0, x1, z1): the block is the xz rectangle of CELL m grid cells x0..x1, z0..z1 (inclusive,
# cell = floor(coordinate / CELL)), the cells export_map.py groups chunks by (CHUNK_GRID), so the blocks of one MSB
# tile it with no gap or overlap: placed things and map triangles (by centroid) go to exactly one block, collision
# triangles to every block one of their corners lies in (they meet at the seam). spawn: the player start the exporters
# bake lighting / LOD around (a part name, or spawn_at (x, y, z) for a block without one).
# Undead Burg cells (m10_01_00_00; navmesh A* from the aqueduct arrival c0000_0000 to the Burg bonfire o0200_0001 and
# the Parish bonfire o0200_0005): x -4..-1 the west streets from the aqueduct stairs (h1070B1 / h1095B1 / h1127B1 ->
# m10_02); x 0..1, z -3..0 the Burg bonfire, the Taurus Demon tower and the bridge's south end; x 0..1, z 1..3 the
# bridge north to the Parish courtyard (route y 16 -> 28). Row z -4 (Lower Burg, under the street) and the Parish
# church (z 4, x 2..3) are not blocks yet: map instance / collision block / enemy block index = world block index caps
# a world build at 4 blocks (MAP_INST, COL_BLOCKS, ENEMY_BLOCKS, OBJ_BLOCKS; TECH_DEBT world-1).
# obj_budget (MB, export_objects.py): every block's breakable types load at boot with their VRAM (world.c
# world_systems, TECH_DEBT world-4); with the default 0.4 MB the three Burg blocks left no VRAM (smoke 2026-11-01 17:00:
# "Not enough VRAM", block 3's o1230 not loaded). ASSUMED cut until types load per block: the types with the fewest
# placements per byte become static map objects (drawn, not breakable).
CELL = 32.0
BLOCK_OBJ_MB = 0.12
BLOCKS = {
    "m10_01_00_00_w": dict(map="m10_01_00_00", spawn="c0000_0000", cells=(-4, -3, -1, -1), obj_budget=BLOCK_OBJ_MB),
    "m10_01_00_00_b": dict(map="m10_01_00_00", spawn="c0000_0009", cells=(0, -3, 1, 0), obj_budget=BLOCK_OBJ_MB),
    "m10_01_00_00_p": dict(map="m10_01_00_00", spawn_at=(14.5, 28.5, 102.2), cells=(0, 1, 1, 3), obj_budget=BLOCK_OBJ_MB),
}
# Blocks of a world build: the boot area first (the default build's own pack), then the blocks world.c may load
WORLDS = {
    # VRAM: enemy textures load with their types near the player, a block's effect textures and map CLUTs while it is
    # on (TECH_DEBT world-4)
    "m10_02_00_00": ["m10_02_00_00", "m10_01_00_00_w", "m10_01_00_00_b", "m10_01_00_00_p"],
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
    if c.get("spawn_at"):  # a block with no player start of its own: a point on its navmesh, facing +z
        return dict(name="spawn_at", type=4, pos=tuple(c["spawn_at"]), rot=(0.0, 0.0, 0.0))
    starts = [p for p in M["parts"] if p["type"] == 4]
    if c.get("spawn"):
        return next(p for p in starts if p["name"] == c["spawn"])
    fires = [p["pos"] for p in M["parts"] if p["type"] == 1 and p["model"] == BONFIRE]
    if not fires:
        return starts[0]
    return min(starts, key=lambda p: min(math.dist(p["pos"], f) for f in fires))  # min keeps the first on ties


def in_cells(cells):
    """(x0, z0, x1, z1) block cells -> keep(x, y, z)"""
    x0, z0, x1, z1 = cells
    return lambda x, y, z: x0 <= math.floor(x / CELL) <= x1 and z0 <= math.floor(z / CELL) <= z1


def region(M, map_id):
    """keep(x, y, z) -> bool for placed things; everything when the area has no radius or cells."""
    if config(map_id).get("cells"):
        return in_cells(config(map_id)["cells"])
    r = config(map_id).get("radius")
    if not r:
        return lambda x, y, z: True
    sx, _, sz = spawn_part(M, map_id)["pos"]
    r2 = r * r
    return lambda x, y, z: (x - sx) ** 2 + (z - sz) ** 2 <= r2


def is_whole(map_id):
    return not config(map_id).get("radius") and not config(map_id).get("cells")


def map_region(M, map_id):
    """keep(x, y, z) for map triangles (by centroid) and far scenery objects, or None (no cut): a block's own cells,
    else map_radius (xz) around the spawn"""
    c = config(map_id)
    if c.get("cells"):
        return in_cells(c["cells"])
    if not c.get("map_radius"):
        return None
    sx, _, sz = spawn_part(M, map_id)["pos"]
    r2 = c["map_radius"] ** 2
    return lambda x, y, z: (x - sx) ** 2 + (z - sz) ** 2 <= r2


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



# ---- enemy placements of the area (export_enemies.py, export_chr.py variants, lua50.py) ----
MAX_ENEMIES = 32  # ps2/proto/src/enemy.h


def enemy_parts(b, map_id=None):
    """[(model, npc, think, entity, pos, rot)] for MSB part type 2 (souls.msb1 named fields); with map_id only those
    inside the area's region (tools/area.py: the part of the MSB the PS2 build holds)"""
    import souls
    M = souls.msb1(b)
    out = []
    keep = region(M, map_id) if map_id else (lambda x, y, z: True)
    for p in M["parts"]:
        if p["type"] != 2 or not p["model"] or not keep(*p["pos"]):
            continue
        out.append((p["model"], p["npc_id"], p["think_id"], p["entity_id"], p["pos"], p["rot"]))
    if map_id and not is_whole(map_id):
        # cropped area: the runtime roster holds MAX_ENEMIES (enemy.h, 32) parts incl. c0000 (skipped later), so
        # keep the non-c0000 parts nearest to the spawn (TECH_DEBT build-6)
        sp = spawn_part(M, map_id)["pos"]
        rest = sorted((r for r in out if r[0] != "c0000"), key=lambda r: math.dist(r[4], sp))
        out = rest[:config(map_id).get("max_enemies", MAX_ENEMIES)]
    return out


def npc_parts(b, map_id):
    """The c0000 parts (human NPCs) inside the area's region, as enemy_parts rows"""
    import souls
    M = souls.msb1(b)
    keep = region(M, map_id)
    return [(p["model"], p["npc_id"], p["think_id"], p["entity_id"], p["pos"], p["rot"]) for p in M["parts"]
            if p["type"] == 2 and p["model"] == "c0000" and keep(*p["pos"])]


def signature(map_id):
    """What an export of map_id (+ env AREA_BLOCK) takes from this module: its merged config, the env knobs read here
    and the code with the AREAS / BLOCKS / WORLDS tables left out (tools/srcsum.py). Makefile: $(A)/area.sig, so
    adding or changing another area's or block's entry re-exports only that one."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import json, srcsum
    code = srcsum.tree_hash(open(os.path.abspath(__file__).replace(".pyc", ".py")).read(),
                            skip=("AREAS", "BLOCKS", "WORLDS", "BLOCK_OBJ_MB"))
    env = {k: os.environ[k] for k in ("AREA_DRAW_FROM", "AREA_DG_FIELD") if k in os.environ}
    return json.dumps(dict(map=map_id, config=config(map_id), env=env, code=code), sort_keys=True) + "\n"


if __name__ == "__main__":
    # Makefile: `area.py --sig <map> <out>` -> out rewritten only when signature(map) changes
    if sys.argv[1:2] == ["--sig"]:
        sig = signature(sys.argv[2])  # puts tools/ on sys.path
        import srcsum
        srcsum.write_if_changed(sys.argv[3], sig, first_mtime_of=os.path.abspath(__file__))
    # Makefile: `area.py --world <boot map>` -> "<map>:<block>" for every other block of that world build
    if sys.argv[1:2] == ["--world"]:
        print(" ".join(f"{BLOCKS[b]['map']}:{b}" for b in WORLDS.get(sys.argv[2], [])[1:]))
