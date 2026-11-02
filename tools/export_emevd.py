"""DSR event scripts (EMEVD) -> compact bytecode for the PS2 event runtime (ps2/proto/src/event.c).

usage: export_emevd.py GAME OUT.bin MAP [--common OUT_COMMON.bin] [--report REPORT.txt]
       export_emevd.py --coverage [maps...]      per-map counts only (no files written)
env AREA_BLOCK (tools/area.py BLOCKS): the collision hit ranges are taken from that block's collision.bin crop.

One file per map (the map's own events + an entity table resolved against its MSB); common.emevd goes into its own file
(--common, same layout, no entities), loaded once at boot. Every instruction is kept: ones the runtime does not wire
to a real system are exported as op 0 (unsupported, bank / id kept; the runtime logs and skips them) or as a stub op
(accepted, logged, no effect yet) and counted in the report, never dropped.

Substitutions (event args, X<src>_<n>) are resolved at InitializeEvent time on the PS2: each instance keeps its raw arg
bytes, and before an instruction runs the runtime patches the substituted words from them (subst records below).

Layout (little-endian, offsets from the start of the file, version 1):
  header   char magic[4] "EVB1", u32 version, u8 map[4] (AA BB CC DD; ff ff ff ff = common), u32 flags (bit 0 common),
           9 x (u32 offset, u32 count): events, instrs (count = bytes), substs, entities, hit ranges, colliders,
           treasures, objacts, item lots; pad to 96 bytes
  events   u32 id; u8 rest (0 default, 1 restart on rest, 2 end on rest); u8 0; u16 instr count;
           u32 instr offset (bytes into instrs); u32 first subst; u32 subst count            (20 B)
  instrs   u8 op (OPS below, index = op), u8 word count, u16 bank, u16 id, u16 has_layer; then u32 words: the EDF args
           widened to 32 bits (s8 / s16 sign-extended, f32 as bits) in definition order; InitializeEvent: the raw arg
           bytes (slot, event id, then the event-arg blob) padded to words
  substs   u16 instr index (in its event), u16 dst byte (into the words), u16 src byte (into the instance arg blob),
           u8 fmt (EDF type 0-6 of dst: read the source as that type and widen; 7 = raw copy of n bytes), u8 n  (8 B)
  entities sorted by id (parts, regions, map SFX and spawn points): s32 id; u8 kind (ENT_*); u8 shape (region SHAPE_*); u16 index (kind-specific: enemy
           placement / collider / first hit range); f32 pos[3]; f32 yaw (rad); f32 dim[3] (box w d h / cyl r h /
           sphere r); u32 count (hit ranges); 2 x u32 0                                                   (48 B)
  ranges   u32 first triangle, u32 count (in this block's collision.bin)
  colliders f32 centre[3] (world, box centre), f32 half[3] (object space), f32 yaw (rad), s32 entity    (32 B):
           every hkx body of every MSB object (box; cylinders / convex shapes as their bounding box, tilted bodies
           as their upright bound) but the breakables (src/objects.c cylinders) and the animated objects
           (objects.bin v3, ANIM_OBJ=<objects.bin.anim>: their boxes move with their bones in objects.c); entity -1
           = no entity id (always solid), else solid while that object is on (fog walls, gates)
  treasures s32 part entity (-1 none), f32 pos[3], s32 lot[5] (-1 none), u8 in_chest, u8 start_disabled, u16 0 (40 B)
  objacts  s32 objact entity (the flag it sets), s32 part entity, f32 pos[3], f32 yaw, f32 valid distance (m),
           s32 ObjActParam id, s32 event flag, u8 state, u8 0, s16 objAnimId (the object's anim on use, -1 none),
           u8 spQualifiedType, u8 spQualifiedType2, u16 0, s32 spQualifiedId, s32 spQualifiedId2,
           s32 spQualifiedPassEventFlag (key: type 1 = a good in the inventory, the pass flag opens it too)  (56 B)
Version 2: objacts grew from 40 B (key and anim fields), colliders cover every object.
  lots     count = rows: s32 lot id, s32 getItemFlagId, u16 first item, u8 items, u8 0                  (12 B)
           sorted by id, then the items of all rows: s32 lotItemId, s32 getItemFlagId0N, u16 lotItemBasePoint,
           u8 lotItemNum, u8 category (lotItemCategory >> 28: 0 weapon, 1 armor, 2 ring, 4 good, 15 = < 0)  (12 B)
           only the items with a weight (lotItemBasePoint > 0). Every referenced lot comes with the rows that follow
           it (id + 1, id + 2, ... while they exist): DSR's draw (FUN_1403f5d90) rolls one item from each row of that run
Version 3: lots carry every weighted item of each row (was: the first item, 20 B).
"""
import collections, math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emevd, gamepath, souls

VERSION = 3
# Ops: (name, (bank, id) or None, class). class: "real" = wired to a system of the port, "noop" = no effect in DSR
# offline single player either (network / achievements), "stub" = accepted and logged, its system does not exist yet.
# Index = op number; keep in the same order as enum Op in ps2/proto/src/event.c.
OPS = [
    ("UNSUPPORTED", None, "none"),
    # conditions (first arg = condition group)
    ("C_GROUP", (0, 0), "real"), ("C_ELAPSED_S", (1, 0), "real"), ("C_ELAPSED_F", (1, 1), "real"),
    ("C_FLAG", (3, 0), "real"), ("C_FLAG_RANGE", (3, 1), "real"), ("C_AREA", (3, 2), "real"),
    ("C_RADIUS", (3, 3), "real"), ("C_ITEM", (3, 4), "real"), ("C_ACTION", (3, 5), "real"), ("C_MP", (3, 6), "real"),
    ("C_ALL_AREA", (3, 7), "real"), ("C_IN_MAP", (3, 8), "real"), ("C_MP_EVENT", (3, 9), "noop"),
    ("C_COUNT_FLAGS", (3, 10), "real"), ("C_TENDENCY", (3, 11), "stub"), ("C_EVENT_VALUE", (3, 12), "real"),
    ("C_DROPPED_AREA", (3, 14), "stub"), ("C_DROPPED", (3, 15), "stub"), ("C_ITEM_BBOX", (3, 16), "real"),
    ("C_CYCLE", (3, 17), "stub"), ("C_ACTION_LINE", (3, 18), "real"), ("C_ACTION_BOSS", (3, 19), "real"),
    ("C_ONLINE", (3, 22), "noop"), ("C_MP_COUNT", (3, 24), "noop"), ("C_DEAD", (4, 0), "real"),
    ("C_DAMAGED", (4, 1), "real"), ("C_HP", (4, 2), "real"), ("C_CHR_TYPE", (4, 3), "stub"),
    ("C_SPEFFECT", (4, 5), "real"), ("C_PART_HP", (4, 6), "real"), ("C_BACKREAD", (4, 7), "real"),
    ("C_EVENT_MSG", (4, 8), "stub"), ("C_CLASS", (4, 11), "stub"), ("C_COVENANT", (4, 12), "stub"),
    ("C_OBJ_DESTROYED", (5, 0), "stub"), ("C_OBJACT", (5, 2), "real"), ("C_ON_HIT", (11, 2), "real"),
    # control flow
    ("SKIP_COND_U", (1000, 1), "real"), ("END_COND_U", (1000, 2), "real"), ("SKIP", (1000, 3), "real"),
    ("END", (1000, 4), "real"), ("SKIP_CMP", (1000, 5), "real"), ("END_CMP", (1000, 6), "real"),
    ("SKIP_COND_C", (1000, 7), "real"), ("END_COND_C", (1000, 8), "real"), ("WAIT_NET", (1000, 9), "noop"),
    ("WAIT_S", (1001, 0), "real"), ("WAIT_F", (1001, 1), "real"), ("WAIT_RAND", (1001, 2), "real"),
    ("SKIP_FLAG", (1003, 1), "real"), ("END_FLAG", (1003, 2), "real"), ("SKIP_FLAG_RANGE", (1003, 3), "real"),
    ("SKIP_MP", (1003, 5), "real"), ("END_MP", (1003, 6), "real"), ("SKIP_IN_MAP", (1003, 7), "real"),
    # actions
    ("INIT", (2000, 0), "real"), ("NET_SYNC", (2000, 2), "noop"), ("PREFETCH", (2000, 4), "noop"),
    ("SAVE_REQ", (2000, 5), "real"), ("CUTSCENE", (2002, 3), "stub"), ("CUTSCENE_WARP", (2002, 4), "stub"),
    ("SET_FLAG", (2003, 2), "real"), ("EVENT_STATE", (2003, 8), "real"), ("BOSS_BAR", (2003, 11), "real"),
    ("BOSS_DEFEAT", (2003, 12), "real"), ("NAVMESH", (2003, 13), "stub"), ("MP_TRIGGER", (2003, 16), "noop"),
    ("RAND_FLAG", (2003, 17), "real"), ("ANIM", (2003, 18), "stub"), ("BATCH_FLAGS", (2003, 22), "real"),
    ("RESPAWN", (2003, 23), "real"), ("REMOVE_ITEM", (2003, 24), "real"), ("SUMMON_SIGN", (2003, 25), "noop"),
    ("MSG_VIS", (2003, 26), "stub"), ("ACHIEVE", (2003, 28), "noop"), ("VAGRANT", (2003, 30), "noop"),
    ("EV_INC", (2003, 31), "real"), ("EV_CLEAR", (2003, 32), "real"), ("SNUGGLY_NEXT", (2003, 33), "stub"),
    ("SNUGGLY_SPAWN", (2003, 34), "stub"), ("AWARD_LOT", (2003, 36), "real"), ("UNK_2003_47", (2003, 47), "stub"),
    ("ERASE_SIGN", (2003, 49), "noop"),
    ("AI_STATE", (2004, 1), "real"), ("TEAM", (2004, 2), "real"), ("KILL", (2004, 4), "real"),
    ("CHR_ENABLE", (2004, 5), "real"), ("EZSTATE", (2004, 6), "stub"), ("SPEFFECT", (2004, 8), "real"),
    ("SPECIAL_ANIM", (2004, 9), "real"), ("GRAVITY", (2004, 10), "real"), ("IMMORTAL", (2004, 12), "real"),
    ("HOME", (2004, 13), "real"), ("ROTATE", (2004, 14), "real"), ("INVINCIBLE", (2004, 15), "real"),
    ("AI_TARGET", (2004, 16), "real"), ("AI_CMD", (2004, 17), "real"), ("AI_ID", (2004, 19), "real"),
    ("AI_REPLAN", (2004, 20), "real"), ("CLEAR_SPEFFECT", (2004, 21), "real"), ("NPC_PART", (2004, 22), "real"),
    ("DISPMASK", (2004, 26), "real"), ("HITMASK", (2004, 27), "stub"), ("NET_AUTH", (2004, 28), "noop"),
    ("BACKREAD", (2004, 29), "real"), ("HPBAR", (2004, 30), "stub"), ("MAPHITS", (2004, 31), "real"),
    ("NET_RATE", (2004, 34), "noop"), ("DEF_BACKREAD", (2004, 35), "real"), ("HELLKITE", (2004, 36), "stub"),
    ("FORCE_TREASURE", (2004, 37), "stub"), ("WARP_FLOOR", (2004, 40), "real"), ("SHORT_WARP", (2004, 41), "real"),
    ("WARP_COPY", (2004, 42), "real"), ("ANIM_RESET", (2004, 43), "real"), ("TEAM_EXIT", (2004, 44), "real"),
    ("HUMANITY", (2004, 45), "stub"),
    ("OBJ_DESTROY", (2005, 1), "real"), ("OBJ_RESTORE", (2005, 2), "real"), ("OBJ_ENABLE", (2005, 3), "real"),
    ("TREASURE", (2005, 4), "real"), ("OBJACT_INIT", (2005, 5), "real"), ("OBJACT_STATE", (2005, 6), "real"),
    ("OBJ_ANIM", (2005, 7), "real"), ("OBJ_REDESTROY", (2005, 8), "real"), ("DAMAGE_OBJ", (2005, 9), "stub"),
    ("OBJ_INVULN", (2005, 13), "stub"), ("OBJACT_IDX", (2005, 14), "real"), ("REROLL", (2005, 15), "stub"),
    ("SFX_DEL", (2006, 1), "real"), ("SFX_SPAWN", (2006, 2), "real"), ("SFX_ONESHOT", (2006, 3), "stub"),
    ("SFX_OBJ", (2006, 4), "stub"), ("SFX_OBJ_DEL", (2006, 5), "stub"),
    ("DIALOG", (2007, 1), "stub"), ("STATUS_MSG", (2007, 3), "real"),
    ("LADDER", (2009, 0), "stub"), ("BONFIRE", (2009, 3), "real"), ("MP_BUFF", (2009, 4), "noop"),
    ("BOSS_ENTRY", (2009, 6), "noop"),
    ("SE", (2010, 2), "stub"), ("SOUND_STATE", (2010, 3), "stub"),
    ("HIT", (2011, 1), "real"), ("MAP_PART", (2012, 1), "real"),
    ("BANNER", (2007, 2), "real"),
]
OP_OF = {k: i for i, (_, k, _) in enumerate(OPS) if k}
ENT_KIND = {"MapPiece": 1, "Object": 2, "Enemy": 3, "Player": 4, "Collision": 5, "Navmesh": 6, "DummyObject": 7,
            "DummyEnemy": 8, "ConnectCollision": 9}
ENT_REGION, ENT_SFX = 10, 11
SHAPES = {"Point": 0, "Circle": 1, "Sphere": 2, "Cylinder": 3, "Rect": 4, "Box": 5}


def map_code(map_id):
    return bytes(int(x) for x in map_id[1:].split("_")) if map_id != "common" else b"\xff" * 4


def encode_events(E):
    """-> (event records, instr bytes, subst bytes, per-class counts, {(bank, id, name): uses} unsupported)"""
    evs, ins_b, sub_b = [], bytearray(), bytearray()
    counts, missing = collections.Counter(), collections.Counter()
    nsub = 0
    for ev in E.events:
        off, first = len(ins_b), nsub
        for k, ins in enumerate(ev.instrs):
            op = OP_OF.get((ins.bank, ins.id), 0)
            if op and ins.defn is None:
                op = 0
            counts[OPS[op][2]] += 1
            if op == 0:
                missing[(ins.bank, ins.id, ins.name)] += 1
            if OPS[op][0] == "INIT" or op == 0:
                raw = ins.raw + b"\0" * (-len(ins.raw) % 4)
                words = list(struct.unpack(f"<{len(raw) // 4}I", raw))
                lay = None
            else:
                lay, _ = ins.defn.layout()
                words = []
                for (o, fmt) in lay:
                    v = struct.unpack_from("<" + fmt, ins.raw, o)[0]
                    words.append(struct.unpack("<I", struct.pack("<f", v))[0] if fmt == "f" else v & 0xFFFFFFFF)
            assert len(words) < 256
            ins_b += struct.pack("<BBHHH", op, len(words), ins.bank, ins.id, ins.layer is not None)
            ins_b += struct.pack(f"<{len(words)}I", *words)
            for ii, dst, src, n, _ in ev.params:
                if ii != k:
                    continue
                if lay is None:
                    fmt_code, dpos = 7, dst
                else:
                    idx = next((j for j, (o, f) in enumerate(lay) if o == dst), None)
                    assert idx is not None, f"event {ev.id} instr {k}: substitution at byte {dst} is not an arg start"
                    fmt_code, dpos = ins.defn.args[idx]["type"], 4 * idx
                sub_b += struct.pack("<HHHBB", k, dpos, src, fmt_code, n)
                nsub += 1
        evs.append(struct.pack("<IBBHIII", ev.id & 0xFFFFFFFF, ev.rest, 0, len(ev.instrs), off, first, nsub - first))
    return evs, bytes(ins_b), bytes(sub_b), counts, missing


def _yaw(rot):
    return math.radians(rot[1])


def entities(game, map_id, msb, block_tris):
    """Entity, hit range and collider records of the map's MSB parts / regions with an entity id."""
    import export_objects
    ents, ranges, colliders = [], [], []
    for i, p in enumerate(msb["parts"]):
        eid = p.get("entity_id", -1)
        if eid is None or eid < 0:
            continue
        kind = ENT_KIND.get(p["type_name"], 0)
        index, count = 0, 0
        if p["type_name"] == "Collision" and p["name"] in block_tris:
            index, count = len(ranges), len(block_tris[p["name"]])
            ranges += block_tris[p["name"]]
        ents.append([eid, kind, 0, index, p["pos"], _yaw(p["rot"]), (0, 0, 0), count, p])
    for r in msb["regions"]:
        if r.get("entity_id", -1) < 0:
            continue
        dim = {"Sphere": (r.get("radius", 0), 0, 0), "Circle": (r.get("radius", 0), 0, 0),
               "Cylinder": (r.get("radius", 0), r.get("height", 0), 0),
               "Box": (r.get("width", 0), r.get("depth", 0), r.get("height", 0)),
               "Rect": (r.get("width", 0), r.get("depth", 0), 0)}.get(r["shape"], (0, 0, 0))
        ents.append([r["entity_id"], ENT_REGION, SHAPES.get(r["shape"], 0), 0, r["pos"], _yaw(r["rot"]), dim, 0, None])
    for e in msb["events"]:  # map SFX: their own entity ids (Delete / Spawn Map SFX), placed at their region
        if e["type_name"] == "SFX" and e.get("entity_id", -1) >= 0:
            reg = next((r for r in msb["regions"] if r["name"] == e.get("region")), None)
            ents.append([e["entity_id"], ENT_SFX, 0, 0, reg["pos"] if reg else (0, 0, 0), 0.0, (0, 0, 0), 0, None])
        # spawn points (復活イベント): Set Player Respawn Point (2003[23]) and a bonfire rest (src/bonfire.c: bonfire
        # object entity + 1000) name them; a point at their region (復活ポイント), facing its yaw
        if e["type_name"] == "SpawnPoint" and e.get("entity_id", -1) >= 0:
            reg = next((r for r in msb["regions"] if r["name"] == e.get("spawn_point")), None)
            if reg:
                ents.append([e["entity_id"], ENT_REGION, SHAPES["Point"], 0, reg["pos"], _yaw(reg["rot"]), (0, 0, 0), 0,
                             None])
    # colliders: every object's hkx bodies (export_objects.py's breakables and animated objects have their own)
    import area as area_mod
    keep = area_mod.region(msb, map_id)
    anim_parts = set(open(os.environ["ANIM_OBJ"]).read().split()) if os.environ.get("ANIM_OBJ") else set()
    first = {}
    for p in msb["parts"]:
        if p["type_name"] != "Object" or p["name"] in anim_parts or not keep(*p["pos"]):
            continue
        for wc, half, yaw in object_boxes(game, p):
            first.setdefault(p.get("entity_id", -1), len(colliders))
            colliders.append(struct.pack("<3f3ffi", *wc, *half, yaw, p.get("entity_id", -1)))
    for ent in ents:
        if ent[8] is not None and ent[8]["type_name"] == "Object" and ent[0] in first:
            ent[3] = first[ent[0]]
    ents.sort(key=lambda e: e[0])
    out = [struct.pack("<iBBH3ff3fI8x", e[0], e[1], e[2], e[3], *e[4], e[5], *e[6], e[7]) for e in ents]
    return out, [struct.pack("<II", *r) for r in ranges], colliders


# moved to emevd.py (export_map.py needs them without depending on this file)
from emevd import SWITCH_OPS, switched_entities  # noqa: E402,F401


_bodies = {}


def object_boxes(game, p):
    """[(world centre, half extents, yaw)] of MSB object part p's hkx bodies; [] for breakables, objects without a
    readable hkx and scaled ones. Body rotations about y add to the part's yaw; tilted bodies become their upright
    bound. Cylinders: their bounding box; a convex shape: the FLVER mesh bounds when it is the object's only body, else
    none (TECH_DEBT event-11)."""
    import export_objects
    model = p["model"]
    if model not in _bodies:
        out = []
        try:
            files = export_objects.objbnd(game, model)
            if not export_objects.is_breakable(files, model) and model + ".hkx" in files:
                bounds = None
                bl = export_objects.bodies(files[model + ".hkx"])
                for _, _, rot, pos, _, _, _, shape in bl:
                    if shape[0] == "box":
                        _, half, centre, rad = shape
                        half = tuple(h + rad for h in half)
                    elif shape[0] == "cyl":
                        _, cr, va, vb, crad = shape
                        centre = tuple((a + b) / 2 for a, b in zip(va, vb))
                        half = tuple(abs(a - b) / 2 + cr + crad for a, b in zip(va, vb))
                    elif len(bl) > 1:  # one convex body of several (a door frame's hull): the mesh box would
                        continue           # close the doorway; skipped (TECH_DEBT event-11)
                    else:
                        if bounds is None and model + ".flver" in files:
                            bounds = export_objects.mesh_bounds(souls.flver(files[model + ".flver"])).get(None)
                        if not bounds:
                            continue
                        lo, hi = bounds
                        rot, pos = [[1, 0, 0], [0, 1, 0], [0, 0, 1]], (0, 0, 0)
                        centre, half = tuple((a + b) / 2 for a, b in zip(lo, hi)), tuple((b - a) / 2 for a, b in zip(lo, hi))
                    c = tuple(pos[r] + sum(rot[r][k] * centre[k] for k in range(3)) for r in range(3))
                    if abs(rot[1][1]) > 0.999:  # upright: yaw about y (x' = x cos + z sin)
                        out.append((c, half, math.atan2(rot[0][2], rot[0][0])))
                    else:
                        out.append((c, tuple(sum(abs(rot[r][k]) * half[k] for k in range(3)) for r in range(3)), 0.0))
        except Exception as e:  # objbnd without a readable hkx: no collider
            print(f"export_emevd: {model}: no collision body ({type(e).__name__}: {e})")
        _bodies[model] = out
    if any(abs(s - 1) > 1e-3 for s in p["scale"]) or abs(p["rot"][0]) > 15 or abs(p["rot"][2]) > 15:
        return []  # ASSUMED: scaled / steeply tilted objects keep no collider; a tilt up to 15 deg is ignored (chests)
    yaw = _yaw(p["rot"])
    c, s = math.cos(yaw), math.sin(yaw)
    # MSB yaw turns object space into world space as x' = x cos + z sin, z' = -x sin + z cos (DS1 Y rotation)
    return [((p["pos"][0] + bc[0] * c + bc[2] * s, p["pos"][1] + bc[1], p["pos"][2] - bc[0] * s + bc[2] * c), half,
             yaw + by) for bc, half, by in _bodies[model]]


def hit_ranges(game, map_id):
    """{collision part name: [(first tri, count)]} in this area / block's collision.bin numbering (tools/export_collision
    area_collision drops the same triangles build() does)."""
    import export_collision
    subs, _, _ = export_collision.area_collision(game, map_id)
    out, t = {}, 0
    for s in subs:
        n = len(s["tris"])
        rs = out.setdefault(s["part"], [])
        if rs and rs[-1][0] + rs[-1][1] == t:
            rs[-1] = (rs[-1][0], rs[-1][1] + n)
        else:
            rs.append((t, n))
        t += n
    return out


def treasures_objacts_lots(game, msb, E, common):
    import params
    parts = {p["name"]: p for p in msb["parts"]}
    tr, oa, lot_ids = [], [], set()
    for e in msb["events"]:
        if e["type_name"] == "Treasure" and e.get("treasure_part") in parts:
            p = parts[e["treasure_part"]]
            lots = list(e["item_lots"]) + [-1] * 5
            lot_ids.update(l for l in lots[:5] if l > 0)
            tr.append(struct.pack("<i3f5iBBH", p.get("entity_id", -1), *p["pos"], *lots[:5], e["in_chest"],
                                  e["start_disabled"], 0))
        elif e["type_name"] == "ObjAct" and e.get("obj_act_part") in parts:
            p = parts[e["obj_act_part"]]
            OP = params.params("ObjActParam")
            pid = e["obj_act_param_id"]
            # ObjActParam -1 = the row of the object's model number (soulstruct: "finds the params that share the
            # object's ID"); validDist is in cm (chest o0110: 70)
            row = OP.get(pid if pid > 0 else int(p["model"][1:]))
            dist = (row.validDist / 100.0) if row else 1.0
            g = (lambda f, d: row[f] if row else d)
            oa.append(struct.pack("<ii3fffiiBxhBBHiii", e["obj_act_entity_id"], p.get("entity_id", -1), *p["pos"],
                                  _yaw(p["rot"]), dist, pid if pid > 0 else int(p["model"][1:]), e["event_flag_id"],
                                  e["obj_act_state"], g("objAnimId", -1), g("spQualifiedType", 0),
                                  g("spQualifiedType2", 0), 0, g("spQualifiedId", 0), g("spQualifiedId2", 0),
                                  g("spQualifiedPassEventFlag", -1)))
    IL = params.params("ItemLotParam")
    for ev in list(E.events) + list(common.events if common else []):  # Award Items literals + int event args
        for ins in ev.instrs:
            if (ins.bank, ins.id) == (2003, 36) and ins.args:
                lot_ids.add(ins.args[0])
            elif (ins.bank, ins.id) == (2000, 0) and ins.args:
                lot_ids.update(v for v in ins.args[2:] + ins.extra if isinstance(v, int) and v > 0 and v in IL)
    for lid in list(lot_ids):  # the rows after a lot are drawn with it (FUN_1403f5d90 steps id + 1 until none)
        while lid + 1 in IL:
            lid += 1
            lot_ids.add(lid)
    rows, items = [], []
    for lid in sorted(lot_ids):
        r = IL.get(lid)
        if r is None:
            continue
        its = [struct.pack("<iiHBB", r[f"lotItemId0{k}"], r[f"getItemFlagId0{k}"], r[f"lotItemBasePoint0{k}"],
                           min(r[f"lotItemNum0{k}"], 255), (r[f"lotItemCategory0{k}"] & 0xFFFFFFFF) >> 28)
               for k in range(1, 9) if r[f"lotItemBasePoint0{k}"] > 0]
        rows.append(struct.pack("<iiHBB", lid, r.getItemFlagId, len(items), len(its), 0))
        items += its
    return tr, oa, (rows + items, len(rows))


def build(sections, map_id):
    hsize = 96
    off, body, table = hsize, bytearray(), []
    for recs, count in sections:
        blob = b"".join(recs) if isinstance(recs, list) else recs
        table += [off + len(body), count]
        body += blob + b"\0" * (-len(blob) % 4)
    hdr = struct.pack("<4sI4sI", b"EVB1", VERSION, map_code(map_id), map_id == "common") + struct.pack(
        f"<{len(table)}I", *table)
    return hdr + b"\0" * (hsize - len(hdr)) + bytes(body)


def export(game, map_id, with_entities=True):
    E = emevd.load(map_id, game)
    evs, ins_b, sub_b, counts, missing = encode_events(E)
    ents, ranges, cols, tr, oa, lots = [], [], [], [], [], ([], 0)
    if with_entities:
        msb = souls.msb1(souls.read(f"{game}/map/MapStudio/{map_id}.msb"))
        ents, ranges, cols = entities(game, map_id, msb, hit_ranges(game, map_id))
        tr, oa, lots = treasures_objacts_lots(game, msb, E, emevd.load("common", game))
    data = build([(evs, len(evs)), (ins_b, len(ins_b)), (sub_b, len(sub_b) // 8), (ents, len(ents)),
                  (ranges, len(ranges)), (cols, len(cols)), (tr, len(tr)), (oa, len(oa)), lots], map_id)
    stats = dict(events=len(E.events), instrs=sum(counts.values()), counts=counts, missing=missing, bytes=len(data),
                 entities=len(ents), ranges=len(ranges), colliders=len(cols), treasures=len(tr), objacts=len(oa),
                 lots=lots[1])
    return data, stats


def report_lines(map_id, st):
    c = st["counts"]
    n = st["instrs"] or 1
    out = [f"{map_id}: {st['events']} events, {st['instrs']} instrs: real {c['real']} ({100 * c['real'] / n:.1f} %), "
           f"noop {c['noop']}, stub {c['stub']}, unsupported {c['none']}; supported (real+noop) "
           f"{100 * (c['real'] + c['noop']) / n:.1f} %; {st['bytes']} B, {st.get('entities', 0)} entities, "
           f"{st.get('colliders', 0)} colliders, {st.get('treasures', 0)} treasures, {st.get('objacts', 0)} objacts"]
    for (bk, i, name), u in sorted(st["missing"].items(), key=lambda kv: -kv[1]):
        out.append(f"  unsupported {bk}[{i:02d}] {name}: {u}")
    return out


def coverage(maps, game):
    """Instruction classes per map (no entity tables, no files)."""
    stub_use = collections.Counter()
    for m in maps:
        E = emevd.load(m, game)
        _, _, _, counts, missing = encode_events(E)
        for ev in E.events:
            for ins in ev.instrs:
                op = OP_OF.get((ins.bank, ins.id), 0)
                if OPS[op][2] == "stub":
                    stub_use[(ins.bank, ins.id, ins.name)] += 1
        print("\n".join(report_lines(m, dict(events=len(E.events), instrs=sum(counts.values()), counts=counts,
                                              missing=missing, bytes=0))))
    print("stub uses in these maps:", ", ".join(f"{b}[{i:02d}] {n} {u}" for (b, i, n), u in stub_use.most_common()))


def main():
    a = sys.argv[1:]
    if a and a[0] == "--coverage":
        coverage(a[1:] or ["m10_02_00_00", "m10_01_00_00", "m18_01_00_00", "common"], gamepath.game_dir())
        return
    game, out, map_id = a[0], a[1], a[2]
    data, st = export(game, map_id)
    with open(out, "wb") as f:
        f.write(data)
    lines = report_lines(map_id, st)
    if "--common" in a:
        cdata, cst = export(game, "common", with_entities=False)
        with open(a[a.index("--common") + 1], "wb") as f:
            f.write(cdata)
        lines += report_lines("common", cst)
    print("\n".join(l for l in lines if not l.startswith("  ")))  # one summary line per file
    if "--report" in a:
        with open(a[a.index("--report") + 1], "w") as f:
            f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
