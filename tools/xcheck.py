"""Cross-check our FromSoft readers (souls.py, havok_collision.py) against soulstruct (github.com/Grimrukh/soulstruct,
installed in ~/dsr-ps2/.venv-tools) for any DS1R map.

  .venv-tools/bin/python tools/xcheck.py [msb|col|tae|params|all] [area ...] [--game DIR] [--out report.json] [-v]

msb     every MSB part / event / region field both readers have (name-matched, types and indices resolved to names)
col     every collision model in h/l hkxbhd: subparts, vertices (model space), triangles, material ids
tae     every chr / obj anibnd TAE: animation ids, event types, times
params  every GameParam row and field (matched by internal paramdef name)
No areas = every map in map/MapStudio. Exit code 1 if any mismatch."""
import collections, glob, json, math, os, struct, sys, logging

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamepath, souls

GAME = None  # set in main(): --game, else tools/gamepath.py (env DSR_GAME, config.local.mk, Steam auto-detect)
logging.disable(logging.WARNING)  # soulstruct logs Oodle / repeated-row warnings on import and load

# our msb1 key -> soulstruct attribute (None = same name)
PART_FIELDS = {
    "model": "model", "pos": "translate", "rot": "rotate", "scale": "scale", "draw_groups": "draw_groups",
    "disp_groups": "display_groups", "sib": "sib_path", "entity_id": "entity_id",
    "light_id": "ambient_light_id", "fog_id": "fog_id", "scatter_id": "scattered_light_id",
    "lens_flare_id": "lens_flare_id", "shadow_id": "shadow_id", "dof_id": "dof_id", "tone_map_id": "tone_map_id",
    "tone_correct_id": "tone_correction_id", "lantern_id": "point_light_id", "lod_param_id": "lod_id",
    "is_shadow_src": "is_shadow_source", "is_shadow_dest": "is_shadow_destination", "is_shadow_only": None,
    "draw_by_reflect_cam": None, "draw_only_reflect_cam": None, "use_depth_bias_float": None,
    "disable_point_light_effect": None,
    # Collision
    "hit_filter": "hit_filter_id", "sound_space": "sound_space_type", "reflect_plane_height": None,
    "nvm_groups": "navmesh_groups", "vagrant_entity_ids": None, "disable_bonfire_entity_id": "attached_bonfire",
    "env_event": "environment_event", "disable_start": "starts_disabled",
    # ConnectCollision
    "map_id": "connected_map_id",
    # Object / Enemy
    "break_term": None, "net_sync": "net_sync_type", "unk_t0e": "unk_x0e", "unk_t10": "unk_x10",
    "think_id": "ai_id", "npc_id": "character_id", "talk_id": None, "point_move_type": "patrol_type",
    "platoon_id": None, "chara_init_id": "player_id", "move_points": "patrol_regions",
    "init_anim": "default_animation", "damage_anim": "damage_animation",
}
PART_COLLISION = ("collision", ("collision", "draw_parent"))  # ConnectCollision.collision, Object/Enemy.draw_parent
EVENT_FIELDS = {
    "entity_id": None, "part": "attached_part", "region": "attached_region", "point_light_id": None,
    "sound_id": None, "sound_type": None, "effect_id": "vfx_id", "wind_vec_min": "wind_vector_min",
    "wind_vec_max": "wind_vector_max", "wind_swing_cycle": "wind_swing_cycles", "wind_swing_pow": "wind_swing_powers",
    "treasure_part": None, "in_chest": "is_in_chest", "start_disabled": ("starts_disabled", "is_disabled"), "max_num": "max_count",
    "gen_type": "spawner_type", "limit_num": "limit_count", "min_gen_num": "min_spawner_count",
    "max_gen_num": "max_spawner_count", "min_interval": None, "max_interval": None, "initial_spawn_count": None,
    "spawn_points": "spawn_regions", "spawn_parts": None, "message_id": "text_id", "hidden": "is_hidden",
    "obj_act_entity_id": None, "obj_act_part": None, "obj_act_param_id": None, "obj_act_state": None,
    "event_flag_id": ("obj_act_flag", "invasion_flag_id"), "spawn_point": "spawn_point_region", "navmesh_region": "navigation_region",
    "offset_pos": "translate", "host_entity_id": None, "activate_goods_id": "activate_good_id",
}
REGION_FIELDS = {"pos": "translate", "rot": "rotate", "entity_id": None}


def norm(v):
    """soulstruct / souls.py value -> comparable plain value."""
    if v is None:
        return None
    if hasattr(v, "name") and hasattr(v, "__class__") and type(v).__name__.startswith("MSB"):
        return v.name
    if type(v).__name__ == "BitSet128":
        return tuple(sorted(v.enabled_bits)) if hasattr(v, "enabled_bits") else tuple(sorted(v))
    if hasattr(v, "value") and type(v).__name__ != "float" and isinstance(getattr(v, "value"), int):
        return v.value  # IntEnum
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, float):
        return round(v, 3)
    if isinstance(v, (list, tuple)) or type(v).__name__ in ("Vector3", "EulerDeg", "ndarray"):
        return tuple(norm(x) for x in v)
    if hasattr(v, "item"):
        return norm(v.item())
    return v


def groups_bits(words):
    return tuple(i * 32 + k for i, w in enumerate(words) for k in range(32) if w >> k & 1)


def ss_field(e, attr):
    """soulstruct attribute with its fallbacks; AttributeError -> missing."""
    if attr == "item_lots":
        return [getattr(e, f"item_lot_{k}") for k in range(1, 6)]
    if isinstance(attr, tuple):
        for a in attr:
            if hasattr(e, a):
                return getattr(e, a)
        raise AttributeError(attr)
    return getattr(e, attr)


FIELD_COUNT = collections.Counter()  # (kind, field) -> values compared


def cmp_entries(kind, ours, theirs, fields, out, area):
    """ours: [dict], theirs: [soulstruct entry]; compares names and mapped fields."""
    tn = collections.Counter(t.name for t in theirs)
    on = collections.Counter(o["name"] for o in ours)
    if on != tn:
        out.append((area, kind, "names", f"only ours {sorted((on - tn).elements())[:5]} only ss {sorted((tn - on).elements())[:5]}"))
    by_name = {}
    for t in theirs:
        by_name.setdefault(t.name, []).append(t)
    seen = collections.Counter()
    for o in ours:
        lst = by_name.get(o["name"])
        if not lst:
            continue
        t = lst[min(seen[o["name"]], len(lst) - 1)]
        seen[o["name"]] += 1
        for k, attr in fields.items():
            if k not in o:
                continue
            try:
                tv = ss_field(t, attr or k)
            except AttributeError:
                continue
            FIELD_COUNT[(kind, k)] += 1
            ov = o[k]
            if k in ("draw_groups", "disp_groups", "nvm_groups"):
                ov = groups_bits(ov)
            a, b = norm(ov), norm(tv)
            if isinstance(a, tuple) and isinstance(b, tuple) and len(a) != len(b):
                b = b[:len(a)] if all(x is None for x in b[len(a):]) else b
            if a != b and not (isinstance(a, tuple) and isinstance(b, tuple) and len(a) == len(b) and all(
                    isinstance(x, (int, float)) and isinstance(y, (int, float)) and abs(x - y) < 2e-3 for x, y in zip(a, b))):
                if isinstance(a, tuple) and isinstance(b, tuple) and len(a) == len(b) and \
                        all((x is None and y is None) or x == y for x, y in zip(a, b)):
                    continue
                out.append((area, kind, f"{o['name']}.{k}", f"ours {a!r} ss {b!r}"))
        if kind == "part" and "collision" in o:
            for attr in PART_COLLISION[1]:
                if hasattr(t, attr):
                    if norm(getattr(t, attr)) != o["collision"]:
                        out.append((area, kind, f"{o['name']}.collision", f"ours {o['collision']!r} ss {norm(getattr(t, attr))!r}"))
                    break
        if kind == "event" and o.get("type") == 4:
            if norm(ss_field(t, "item_lots")) != tuple(o["item_lots"]):
                out.append((area, kind, f"{o['name']}.item_lots", f"ours {o['item_lots']} ss {ss_field(t, 'item_lots')}"))
        if kind == "region":
            sh = getattr(t, "shape", None)
            if sh is not None:
                for k in ("radius", "height", "width", "depth"):
                    if k in o and hasattr(sh, k) and abs(o[k] - getattr(sh, k)) > 1e-4:
                        out.append((area, kind, f"{o['name']}.{k}", f"ours {o[k]} ss {getattr(sh, k)}"))
                sname = type(sh).__name__.replace("Shape", "")
                if o["shape"] != sname and not (o["shape"] == "Point" and sname in ("Point", "NoneType")):
                    out.append((area, kind, f"{o['name']}.shape", f"ours {o['shape']} ss {sname}"))


def areas_all():
    return sorted(os.path.basename(p)[:-4] for p in glob.glob(f"{GAME}/map/MapStudio/*.msb"))


def check_msb(areas):
    from soulstruct.darksouls1r.maps import MSB
    out, counts = [], collections.Counter()
    for a in areas:
        path = f"{GAME}/map/MapStudio/{a}.msb"
        M = souls.msb1(souls.read(path))
        try:
            S = MSB.from_path(path)
        except Exception as e:
            out.append((a, "msb", "-", f"soulstruct failed: {type(e).__name__}: {str(e)[:120]}"))
            continue
        cmp_entries("part", M["parts"], list(S.get_parts()), PART_FIELDS, out, a)
        cmp_entries("event", M["events"], list(S.get_events()), EVENT_FIELDS, out, a)
        cmp_entries("region", M["regions"], list(S.get_regions()), REGION_FIELDS, out, a)
        mi = {m.name for m in S.get_models()}
        if mi != {m["name"] for m in M["model_info"]}:
            out.append((a, "model", "names", str(mi ^ {m["name"] for m in M["model_info"]})))
        counts["parts"] += len(M["parts"])
        counts["events"] += len(M["events"])
        counts["regions"] += len(M["regions"])
        counts["models"] += len(M["models"])
    counts["values"] = sum(FIELD_COUNT.values())
    never = [f"{k}.{f}" for k, F in (("part", PART_FIELDS), ("event", EVENT_FIELDS), ("region", REGION_FIELDS))
             for f in F if not FIELD_COUNT[(k, f)]]
    if never:
        print("   fields never compared (soulstruct has no such attribute):", ", ".join(never))
    return out, counts


def check_col(areas):
    import havok_collision as hc
    import numpy as np
    from soulstruct.havok.fromsoft.darksouls1r import MapCollisionModel
    out, counts = [], collections.Counter()
    for a in areas:
        for det in ("h", "l"):
            mdir = f"{GAME}/map/{a}/{det}{a[1:]}"
            if not os.path.exists(mdir + ".hkxbhd"):
                continue
            for _, n, b in souls.bxf3(mdir + ".hkxbhd", mdir + ".hkxbdt"):
                stem = souls.basename(n).split(".")[0]
                counts["models"] += 1
                ours = S = err = None
                try:
                    ours = hc.hkx_meshes(b)
                except Exception as e:
                    err = f"ours failed: {e}"
                try:
                    S = MapCollisionModel.from_bytes(b)
                except Exception as e:
                    err = (err + "; " if err else "") + f"soulstruct failed: {type(e).__name__} {e}"
                if ours is None and S is None:
                    counts["empty_in_both"] += 1  # e.g. m10_01 h1151B1A10: 788 bytes, hkRootLevelContainer only
                    continue
                if err:
                    out.append((a, "col", stem, err))
                    continue
                if len(ours) != len(S.meshes):
                    out.append((a, "col", stem, f"subparts ours {len(ours)} ss {len(S.meshes)}"))
                    continue
                for k, ((v, t, mat), m) in enumerate(zip(ours, S.meshes)):
                    counts["tris"] += len(t)
                    sv, sf = m.vertices3D, m.face_vertex_indices
                    if len(v) != len(sv) or len(t) != len(sf):
                        out.append((a, "col", f"{stem}[{k}]", f"verts {len(v)}/{len(sv)} tris {len(t)}/{len(sf)}"))
                        continue
                    d = float(np.abs(np.asarray(v, dtype=np.float64) - sv).max()) if len(v) else 0.0
                    if d > 1e-4:
                        out.append((a, "col", f"{stem}[{k}]", f"vertex max diff {d:.4f} (subpart transform applied by ours)"))
                    if not np.array_equal(np.asarray(t, dtype=np.int64), sf.astype(np.int64)):
                        out.append((a, "col", f"{stem}[{k}]", "triangle indices differ"))
                    if mat != m.material_index and not (mat == -1 and m.material_index == 0):
                        out.append((a, "col", f"{stem}[{k}]", f"material ours {mat} ss {m.material_index}"))
    return out, counts


TEMPLATE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ext", "DSAnimStudio",
                            "DSAnimStudioNETCore", "Res")


def _taes(path):
    """(name, TAE bytes) of a chr anibnd or an objbnd (whose anibnd is nested)."""
    for _, n, b in souls.bnd3(souls.read(path)):
        n = n or ""
        if n.lower().endswith(".tae"):
            yield souls.basename(n), b
        elif n.lower().endswith(".anibnd"):
            for _, n2, b2 in souls.bnd3(b):
                if n2 and n2.lower().endswith(".tae"):
                    yield souls.basename(n2), b2


def check_tae(areas=None):
    """Every chr / obj TAE (areas ignored) against the DSAnimStudio DS1 templates (soulstruct's TAE reader is the
    64-bit DS3+ layout): event type known, template size fits the parameter block, asserted fields hold."""
    out, counts = [], collections.Counter()
    tpl = {"chr": souls.tae_template(os.path.join(TEMPLATE_DIR, "TAE.Template.DS1.xml")),
           "obj": souls.tae_template(os.path.join(TEMPLATE_DIR, "TAE.Template.DS1.OBJ.xml"))}
    unknown, short = collections.Counter(), collections.Counter()
    bad_assert = collections.defaultdict(collections.Counter)
    for kind, pattern in (("chr", "chr/*.anibnd.dcx"), ("obj", "obj/*.objbnd.dcx")):
        T = tpl[kind]
        for f in sorted(glob.glob(f"{GAME}/{pattern}")):
            for n, b in _taes(f):
                key = f"{os.path.basename(f).split('.')[0]}/{n}"
                counts["taes"] += 1
                try:
                    anims = souls.tae(b)
                except Exception as e:
                    out.append((kind, "tae", key, f"ours failed: {e}"))
                    continue
                counts["anims"] += len(anims)
                for aid, a in anims.items():
                    for ev in a["events"]:
                        counts["events"] += 1
                        etype, _, _, words = ev
                        if etype not in T:
                            unknown[(kind, etype)] += 1
                            continue
                        size = souls.tae_template_size(T[etype][1])
                        first = souls.tae_template_size(T[etype][1][:1])
                        if 4 * len(words) < first:
                            out.append((kind, "tae", f"{key}:{aid}", f"type {etype} {T[etype][0]}: block {4 * len(words)} B shorter than its first field"))
                            continue
                        counts["named"] += 1
                        capped = len(words) >= 16  # last block before other data: length unknown, tail is not ours
                        if size > 4 * len(words):
                            short[(kind, etype, 4 * len(words), size)] += 1  # template is a superset (ChrActionFlag 12 vs 16)
                        if capped:
                            continue
                        _, vals = souls.tae_named(ev, T)
                        for fname, t, asrt, _ in T[etype][1]:
                            if asrt is not None and fname in vals:
                                v = vals[fname][0] if isinstance(vals[fname], tuple) and not t == "f32grad" else vals[fname]
                                if float(v) != float(asrt):
                                    bad_assert[(kind, etype, T[etype][0], fname, asrt)][v] += 1
    for (kind, t), c in sorted(unknown.items()):
        out.append((kind, "tae", f"type {t}", f"not in the DSAnimStudio {kind} template ({c} events)"))
    for (kind, t, name, f, asrt), vals in sorted(bad_assert.items()):
        out.append((kind, "tae", f"type {t} {name}.{f}", f"template asserts {asrt}, data has {dict(vals.most_common(4))} ({sum(vals.values())} events)"))
    for (kind, t, blk, size), c in sorted(short.items()):
        print(f"   info: {kind} type {t}: {c} blocks of {blk} B, template {size} B (decoded up to the block end)")
    return out, counts


def check_params(areas=None):
    from soulstruct.darksouls1r.params import GameParamBND
    out, counts = [], collections.Counter()
    defs = {t: f for t, f in (souls.paramdef(d) for _, _, d in souls.bnd3(souls.read(f"{GAME}/paramdef/paramdef.paramdefbnd.dcx")))}
    ours, dups = {}, []
    for _, n, d in souls.bnd3(souls.read(f"{GAME}/param/GameParam/GameParam.parambnd.dcx")):
        ptype = souls.cstr(d, 0xC).strip()
        if ptype not in defs:
            continue
        stem, rows = souls.basename(n).rsplit(".", 1)[0], {}
        for rid, name, vals in souls.param_rows(d, defs[ptype]):
            if rid in rows:  # soulstruct keeps the first; souls.param() the last (which one the exe finds: open)
                dups.append((stem, rid, [k for k in vals if vals[k] != rows[rid][1].get(k)]))
                continue
            rows[rid] = (name, vals)
        ours[stem] = rows
    for stem, rid, diff in dups:
        print(f"   duplicate row id {stem} {rid}: copies differ in {diff or 'nothing'}")
    counts["duplicate_rows"] = len(dups)
    G = GameParamBND.from_path(f"{GAME}/param/GameParam/GameParam.parambnd.dcx")
    for path, P in G.params.items():
        stem = path.replace("\\", "/").rsplit("/", 1)[-1]
        if stem not in ours:
            out.append(("param", stem, "-", "missing in ours"))
            continue
        meta = P.ROW_TYPE.get_all_field_metadata()
        O = ours[stem]
        if set(O) != set(P.keys()):
            out.append(("param", stem, "row ids", f"ours {len(O)} ss {len(P.keys())}"))
        for rid, row in P.items():
            if rid not in O:
                continue
            counts["rows"] += 1
            name, vals = O[rid]
            for nick, md in meta.items():
                iname = md.internal_name
                if iname not in vals or iname.startswith(("pad", "reserve", "dummy")):
                    continue
                counts["fields"] += 1
                a, b = vals[iname], getattr(row, nick)
                a, b = norm(a), norm(b)
                if a != b and not (isinstance(a, float) and isinstance(b, float) and (abs(a - b) < 1e-3 or (math.isnan(a) and math.isnan(b)))):
                    out.append(("param", stem, f"{rid}.{iname}", f"ours {a!r} ss {b!r}"))
    return out, counts


def check_emevd(areas=None):
    """Every event/*.emevd.dcx (areas ignored): our emevd.py vs soulstruct's EMEVD reader: event ids, rest behaviour,
    instruction bank / id, decoded arg values (InitializeEvent extra args included), event-arg substitutions."""
    import emevd
    from soulstruct.darksouls1r.events import EMEVD
    out, counts = [], collections.Counter()
    defs, _, _ = emevd.definitions(GAME)
    for path in sorted(glob.glob(f"{GAME}/event/*.emevd.dcx")):
        mid = os.path.basename(path).split(".")[0]
        ours = emevd.load(mid, GAME)
        try:
            S = EMEVD.from_path(path)
        except Exception as e:
            out.append((mid, "emevd", "-", f"soulstruct failed: {type(e).__name__}: {str(e)[:100]}"))
            continue
        counts["files"] += 1
        sev = S.events if isinstance(S.events, dict) else {e.event_id: e for e in S.events}
        if set(sev) != {e.id for e in ours.events}:
            out.append((mid, "emevd", "event ids", f"ours {len(ours.events)} ss {len(sev)}"))
        for ev in ours.events:
            se = sev.get(ev.id)
            if se is None:
                continue
            counts["events"] += 1
            if int(se.on_rest_behavior) != ev.rest:
                out.append((mid, "emevd", f"{ev.id}.rest", f"ours {ev.rest} ss {se.on_rest_behavior}"))
            if len(se.instructions) != len(ev.instrs):
                out.append((mid, "emevd", f"{ev.id}", f"instrs ours {len(ev.instrs)} ss {len(se.instructions)}"))
                continue
            for k, (a, b) in enumerate(zip(ev.instrs, se.instructions)):
                counts["instrs"] += 1
                if (a.bank, a.id) != (b.category, b.index):
                    out.append((mid, "emevd", f"{ev.id}[{k}]", f"ours {a.bank}[{a.id}] ss {b.category}[{b.index}]"))
                    continue
                av = [round(x, 4) if isinstance(x, float) else x for x in (a.args or []) + (a.extra or [])]
                bv = [round(x, 4) if isinstance(x, float) else x for x in b.args_list]
                if av != bv and [int(x) if isinstance(x, bool) else x for x in bv] != av:
                    if len(av) == len(bv) and all(x == y or (isinstance(x, int) and isinstance(y, float) and
                            struct.pack("<i", x) == struct.pack("<f", y)) for x, y in zip(av, bv)):
                        counts["extra_args_typed_differently"] += 1  # same bits; soulstruct infers the float
                        continue
                    out.append((mid, "emevd", f"{ev.id}[{k}] {a.bank}[{a.id:02d}]", f"ours {av} ss {bv}"))
                if a.layer != (None if not b.event_layers else int(b.event_layers)) and b.event_layers is not None:
                    out.append((mid, "emevd", f"{ev.id}[{k}] layer", f"ours {a.layer} ss {b.event_layers}"))
            ours_sub = sorted((p[0], p[1], p[2], p[3]) for p in ev.params)
            ss_sub = sorted((k, r.instruction_arg_start_byte if hasattr(r, "instruction_arg_start_byte") else None)
                            for k, ins in enumerate(se.instructions) for r in ins.event_arg_replacements)
            counts["substitutions"] += len(ours_sub)
            if len(ss_sub) != len(ours_sub):
                out.append((mid, "emevd", f"{ev.id} substitutions", f"ours {len(ours_sub)} ss {len(ss_sub)}"))
    return out, counts


CHECKS = {"msb": check_msb, "col": check_col, "tae": check_tae, "params": check_params, "emevd": check_emevd}


def main(argv):
    out_path = None
    if "--out" in argv:
        i = argv.index("--out")
        out_path = argv[i + 1]
        del argv[i:i + 2]
    global GAME
    game_arg = None
    if "--game" in argv:
        i = argv.index("--game")
        game_arg = argv[i + 1]
        del argv[i:i + 2]
    GAME = gamepath.game_dir(game_arg)
    verbose = "-v" in argv
    argv = [x for x in argv if x != "-v"]
    what = argv[0] if argv and argv[0] in list(CHECKS) + ["all"] else "all"
    areas = [x for x in argv if x not in list(CHECKS) + ["all"]] or areas_all()
    report, bad = {}, 0
    for name in (CHECKS if what == "all" else [what]):
        mism, counts = CHECKS[name](areas)
        bad += len(mism)
        by = collections.Counter((m[0], m[1]) for m in mism)
        print(f"== {name}: {dict(counts)}  mismatches {len(mism)}")
        for (a, k), c in sorted(by.items()):
            print(f"   {a} {k}: {c}")
        for m in (mism if verbose else mism[:25]):
            print("   ", *m)
        report[name] = {"counts": dict(counts), "mismatches": [list(map(str, m)) for m in mism]}
    if out_path:
        with open(out_path, "w") as f:
            json.dump(report, f, indent=1, ensure_ascii=False)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
