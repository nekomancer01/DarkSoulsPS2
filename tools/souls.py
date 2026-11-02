"""Readers for the DSR file formats the port needs: DCX (zlib), BND3, FMG, PARAMDEF/PARAM (DS1), TAE (DS1).
Little-endian DSR PC files only. Layouts follow SoulsFormats (github.com/JKAnderson/SoulsFormats)."""
import math, os, struct, zlib


def undcx(b):
    """DCX 'DFLT' container -> payload; anything else is returned unchanged."""
    if b[:4] != b"DCX\0":
        return b
    return zlib.decompress(b[b.find(b"DCA\0") + 8:])


def read(path):
    with open(path, "rb") as f:
        return undcx(f.read())


def cstr(b, off, enc="cp932"):
    return b[off:b.index(b"\0", off)].decode(enc)


def wstr(b, off):
    end = off
    while b[end:end + 2] != b"\0\0":
        end += 2
    return b[off:end].decode("utf-16le")


def bnd3(b, data=None):
    """BND3 -> [(id, name, bytes)]. The format byte is stored bit-reversed (unless bit-big-endian is set).
    With `data`, b is a BHF3 header (.xxxbhd) whose file offsets point into the BDF3 `data` (.xxxbdt)."""
    assert b[:4] in (b"BND3", b"BHF3"), "not a BND3/BHF3"
    src = b if data is None else data
    raw, bitbig = b[0xC], b[0xE]
    fmt = raw if bitbig else int(f"{raw:08b}"[::-1], 2)
    ids, names, longoff, comp = fmt & 2, fmt & 0xC, fmt & 0x10, fmt & 0x20
    n = struct.unpack_from("<I", b, 0x10)[0]
    pos, out = 0x20, []
    for _ in range(n):
        size = struct.unpack_from("<I", b, pos + 4)[0]
        pos += 8
        off = struct.unpack_from("<Q" if longoff else "<I", b, pos)[0]
        pos += 8 if longoff else 4
        fid = name = None
        if ids:
            fid = struct.unpack_from("<i", b, pos)[0]
            pos += 4
        if names:
            name = cstr(b, struct.unpack_from("<I", b, pos)[0])
            pos += 4
        if comp:
            pos += 4
        out.append((fid, name, undcx(src[off:off + size])))
    return out


def bxf3(bhd_path, bdt_path):
    """Split binder (.tpfbhd/.tpfbdt, .hkxbhd/.hkxbdt) -> [(id, name, bytes)]"""
    return bnd3(read(bhd_path), read(bdt_path))


def _euler_deg_xzy(rx, ry, rz):
    return _euler_xzy_to_quat(math.radians(rx), math.radians(ry), math.radians(rz))


MSB1_MODEL_TYPES = {0: "MapPiece", 1: "Object", 2: "Enemy", 4: "Player", 5: "Collision", 6: "Navmesh"}
MSB1_PART_TYPES = {0: "MapPiece", 1: "Object", 2: "Enemy", 4: "Player", 5: "Collision", 8: "Navmesh",
                   9: "DummyObject", 10: "DummyEnemy", 11: "ConnectCollision"}
MSB1_EVENT_TYPES = {0: "Light", 1: "Sound", 2: "SFX", 3: "Wind", 4: "Treasure", 5: "Generator", 6: "Message",
                    7: "ObjAct", 8: "SpawnPoint", 9: "MapOffset", 10: "Navmesh", 11: "Environment",
                    12: "PseudoMultiplayer"}
MSB1_SHAPES = {-1: "None", 0: "Point", 1: "Circle", 2: "Sphere", 3: "Cylinder", 4: "Rectangle", 5: "Box",
               6: "Composite"}
# Part entity block (24 bytes after the type-independent header): ids into the area's draw-param banks
_MSB1_DRAW_IDS = ("light_id", "fog_id", "scatter_id", "lens_flare_id", "shadow_id", "dof_id", "tone_map_id",
                  "tone_correct_id", "lantern_id", "lod_param_id")  # lantern = PointLightBank row (soulstruct point_light_id)
_MSB1_DRAW_FLAGS = ("is_shadow_src", "is_shadow_dest", "is_shadow_only", "draw_by_reflect_cam",
                    "draw_only_reflect_cam", "use_depth_bias_float", "disable_point_light_effect")


def msb1(b):
    """DS1 / DSR MSB -> dict(models, parts, events, regions); every field SoulsFormatsNEXT MSB1 reads
    (github.com/soulsmods/SoulsFormatsNEXT, Formats/MSB/MSB1/*.cs). Indices between entries are resolved to names.
    models: [(name, type)] (unchanged API); model_info: [dict(name, type, type_name, sib, instances)].
    parts: dict(name, type, type_name, model, pos, rot (degrees), scale, draw_groups, disp_groups, sib, entity_id,
      light_id .. lod_param_id, shadow / reflect flags, + per type: Collision hit_filter, sound_space, env_light_spot,
      reflect_plane_height, nvm_groups, vagrant_entity_ids, map_name_id, disable_start, disable_bonfire_entity_id,
      play_region_id, lock_cam_ids; ConnectCollision collision (part name) + map_id (4 bytes, mAA_BB_CC_DD);
      Object/DummyObject collision, break_term, net_sync, init_anim, unk_t0e, unk_t10; Enemy/DummyEnemy think_id,
      npc_id, talk_id, point_move_type, platoon_id, chara_init_id, collision, move_points (region names), init_anim,
      damage_anim; Navmesh nvm_groups).
    events: dict(name, type, type_name, event_id, part, region, entity_id, + type fields).
    regions: dict(name, shape, pos, rot, entity_id, + radius / height / width / depth / children)."""
    def param(pos):
        _, name_off, count = struct.unpack_from("<iii", b, pos)
        offs = struct.unpack_from(f"<{count - 1}i", b, pos + 12)
        nxt = struct.unpack_from("<i", b, pos + 12 + 4 * (count - 1))[0]
        return cstr(b, name_off), offs, nxt

    _, model_offs, nxt = param(0)
    _, event_offs, nxt = param(nxt)
    _, region_offs, nxt = param(nxt)
    _, part_offs, _ = param(nxt)
    models, model_info = [], []
    for o in model_offs:
        name_off, mtype, _, sib_off, inst = struct.unpack_from("<5i", b, o)
        models.append((cstr(b, o + name_off), mtype))
        model_info.append({"name": models[-1][0], "type": mtype, "type_name": MSB1_MODEL_TYPES.get(mtype),
                           "sib": cstr(b, o + sib_off), "instances": inst})
    part_names = [cstr(b, o + struct.unpack_from("<i", b, o)[0]) for o in part_offs]
    region_names = [cstr(b, o + struct.unpack_from("<i", b, o)[0]) for o in region_offs]
    pname = lambda i: part_names[i] if 0 <= i < len(part_names) else None
    rname = lambda i: region_names[i] if 0 <= i < len(region_names) else None

    regions = []
    for o, name in zip(region_offs, region_names):
        _, _, _, shape = struct.unpack_from("<4i", b, o)
        pos, rot = struct.unpack_from("<3f", b, o + 16), struct.unpack_from("<3f", b, o + 28)
        shape_off, ent_off = struct.unpack_from("<ii", b, o + 48)
        r = {"name": name, "shape": MSB1_SHAPES.get(shape, shape), "pos": pos, "rot": rot,
             "entity_id": struct.unpack_from("<i", b, o + ent_off)[0]}
        s = o + shape_off
        if shape in (1, 2):
            r["radius"] = struct.unpack_from("<f", b, s)[0]
        elif shape == 3:
            r["radius"], r["height"] = struct.unpack_from("<2f", b, s)
        elif shape == 4:
            r["width"], r["depth"] = struct.unpack_from("<2f", b, s)
        elif shape == 5:
            r["width"], r["depth"], r["height"] = struct.unpack_from("<3f", b, s)
        elif shape == 6:
            r["children"] = [(rname(struct.unpack_from("<i", b, s + 8 * k)[0]), struct.unpack_from("<i", b, s + 8 * k + 4)[0])
                             for k in range(8)]
        regions.append(r)

    events = []
    for o in event_offs:
        name_off, event_id, etype, _, base_off, type_off = struct.unpack_from("<6i", b, o)
        pi, ri, ent = struct.unpack_from("<3i", b, o + base_off)
        e = {"name": cstr(b, o + name_off), "type": etype, "type_name": MSB1_EVENT_TYPES.get(etype),
             "event_id": event_id, "part": pname(pi), "region": rname(ri), "entity_id": ent}
        t = o + type_off
        if etype == 0:
            e["point_light_id"] = struct.unpack_from("<i", b, t)[0]
        elif etype == 1:
            e["sound_type"], e["sound_id"] = struct.unpack_from("<2i", b, t)
        elif etype == 2:
            e["effect_id"] = struct.unpack_from("<i", b, t)[0]
        elif etype == 3:
            v = struct.unpack_from("<16f", b, t)
            e.update(wind_vec_min=v[0:3], unk_t0c=v[3], wind_vec_max=v[4:7], unk_t1c=v[7],
                     wind_swing_cycle=v[8:12], wind_swing_pow=v[12:16])
        elif etype == 4:
            e["treasure_part"] = pname(struct.unpack_from("<i", b, t + 4)[0])
            e["item_lots"] = [struct.unpack_from("<i", b, t + 8 + 8 * k)[0] for k in range(5)]
            e["in_chest"], e["start_disabled"] = bool(b[t + 48]), bool(b[t + 49])
        elif etype == 5:
            (e["max_num"], e["gen_type"], e["limit_num"], e["min_gen_num"], e["max_gen_num"], e["min_interval"],
             e["max_interval"], e["initial_spawn_count"]) = struct.unpack_from("<BbhhhffB", b, t)
            e["spawn_points"] = [rname(i) for i in struct.unpack_from("<4i", b, t + 0x30)]
            e["spawn_parts"] = [pname(i) for i in struct.unpack_from("<32i", b, t + 0x40)]
        elif etype == 6:
            e["message_id"], e["unk_t02"] = struct.unpack_from("<hh", b, t)
            e["hidden"] = bool(b[t + 4])
        elif etype == 7:
            ent2, ppi, prm, state, flag = struct.unpack_from("<iihBxi", b, t)
            e.update(obj_act_entity_id=ent2, obj_act_part=pname(ppi), obj_act_param_id=prm, obj_act_state=state,
                     event_flag_id=flag)
        elif etype == 8:
            e["spawn_point"] = rname(struct.unpack_from("<i", b, t)[0])
        elif etype == 9:
            e["offset_pos"], e["offset_degree"] = struct.unpack_from("<3f", b, t), struct.unpack_from("<f", b, t + 12)[0]
        elif etype == 10:
            e["navmesh_region"] = rname(struct.unpack_from("<i", b, t)[0])
        elif etype == 11:
            e["unk_t00"], *e["unk_t04"] = struct.unpack_from("<i5f", b, t)
        elif etype == 12:
            e["host_entity_id"], e["event_flag_id"], e["activate_goods_id"] = struct.unpack_from("<3i", b, t)
        events.append(e)

    env_names = [e["name"] for e in events if e["type"] == 11]
    coll_names = [n for o, n in zip(part_offs, part_names) if struct.unpack_from("<i", b, o + 4)[0] == 5]
    parts = []
    for o, name in zip(part_offs, part_names):
        _, ptype, _, model, sib_off = struct.unpack_from("<5i", b, o)
        pos, rot, scl = (struct.unpack_from("<3f", b, o + 20 + 12 * k) for k in range(3))
        ent_off, type_off = struct.unpack_from("<ii", b, o + 88)
        p = {"name": name, "type": ptype, "type_name": MSB1_PART_TYPES.get(ptype),
             "model": models[model][0] if model >= 0 else None, "pos": pos, "rot": rot, "scale": scl,
             "draw_groups": struct.unpack_from("<4I", b, o + 56), "disp_groups": struct.unpack_from("<4I", b, o + 72),
             "sib": cstr(b, o + sib_off), "entity_id": struct.unpack_from("<i", b, o + ent_off)[0]}
        p.update(zip(_MSB1_DRAW_IDS, struct.unpack_from("<10b", b, o + ent_off + 4)))
        p.update(zip(_MSB1_DRAW_FLAGS, b[o + ent_off + 15:o + ent_off + 22]))
        t = o + type_off
        if ptype == 5:
            (p["hit_filter"], p["sound_space"], p["env_light_spot"], p["reflect_plane_height"]) = \
                struct.unpack_from("<BBhf", b, t)
            # env_light_spot indexes the Environment events (GIEL hNNNN: GI_EnvDif / EnvSpc probe of this part)
            p["env_event"] = env_names[p["env_light_spot"]] if 0 <= p["env_light_spot"] < len(env_names) else None
            p["nvm_groups"] = struct.unpack_from("<4I", b, t + 8)
            p["vagrant_entity_ids"] = struct.unpack_from("<3i", b, t + 24)
            p["map_name_id"], dis, p["unk_t27"], p["disable_bonfire_entity_id"] = struct.unpack_from("<hBBi", b, t + 36)
            p["disable_start"] = bool(dis)
            # play_region_id < -10: -(stable footing flag) per soulstruct; map_name_id < 0 forces the banner
            p["play_region_id"], c1, c2 = struct.unpack_from("<ihh", b, t + 56)
            p["lock_cam_ids"] = (c1, c2)
        elif ptype == 11:
            # index into the Collision parts only (SoulsFormatsNEXT ConnectCollision: msb.Parts.Collisions)
            ci = struct.unpack_from("<i", b, t)[0]
            p["collision"] = coll_names[ci] if 0 <= ci < len(coll_names) else None
            p["map_id"] = struct.unpack_from("<4b", b, t + 4)  # -1 = any (m10_01_-1_-1)
        elif ptype in (1, 9):
            ci, bt, ns, ia, u0e, u10 = struct.unpack_from("<4xibb2xhhi", b, t)
            p.update(collision=pname(ci), break_term=bt, net_sync=ns, init_anim=ia, unk_t0e=u0e, unk_t10=u10)
        elif ptype in (2, 10):
            think, npc, talk, pmt, platoon, cinit, ci = struct.unpack_from("<8x3iBxHii", b, t)
            p.update(think_id=think, npc_id=npc, talk_id=talk, point_move_type=pmt, platoon_id=platoon,
                     chara_init_id=cinit, collision=pname(ci),
                     move_points=[rname(i) for i in struct.unpack_from("<8h", b, t + 0x28)])
            p["init_anim"], p["damage_anim"] = struct.unpack_from("<ii", b, t + 0x38)
        elif ptype == 8:
            p["nvm_groups"] = struct.unpack_from("<4I", b, t)
        parts.append(p)
    return {"models": models, "model_info": model_info, "parts": parts, "events": events, "regions": regions}


def msb1_map_id(map_id):
    """ConnectCollision map_id bytes (AA, BB, CC, DD) -> 'mAA_BB_CC_DD'."""
    return "m%02d_%02d_%02d_%02d" % tuple(map_id)


def basename(name):
    return name.replace("\\", "/").rsplit("/", 1)[-1]


def fmg(b):
    """DS1 FMG -> {id: text}."""
    groups, _, offs = struct.unpack_from("<III", b, 0xC)
    out = {}
    for g in range(groups):
        idx, first, last = struct.unpack_from("<iii", b, 0x1C + 12 * g)
        for k in range(last - first + 1):
            o = struct.unpack_from("<i", b, offs + 4 * (idx + k))[0]
            if o > 0:
                out[first + k] = wstr(b, o)
    return out


def tae(b):
    """DS1 TAE (version 0x1000B, also used by DSR's x64 files: offsets stay 32-bit) ->
    {anim id: {"events": [(type, start, end, [u32 params])], "ref": file offset of the anim-file block}}"""
    assert b[:4] == b"TAE " and struct.unpack_from("<I", b, 8)[0] == 0x1000B, "not a DS1 TAE"
    count, ids_off = struct.unpack_from("<II", b, 0x54)
    anims, bodies = {}, set()
    for i in range(count):
        aid, off = struct.unpack_from("<iI", b, ids_off + 8 * i)
        ev_count, ev_off, _, _, _, _, file_off = struct.unpack_from("<7I", b, off)
        events = []
        for e in range(ev_count):
            s_off, e_off, body = struct.unpack_from("<III", b, ev_off + 12 * e)
            etype, p_off = struct.unpack_from("<iI", b, body)
            start, end = struct.unpack_from("<f", b, s_off)[0], struct.unpack_from("<f", b, e_off)[0]
            events.append((etype, start, end, p_off))
            bodies.add(body)
        anims[aid] = {"events": events, "file": file_off}
    # Parameter blocks have no stored length; each one runs to the next block or the next event body (bodies are
    # packed: type, params pointer, params), at most 16 words. Matches the DSAnimStudio DS1 template sizes.
    starts = sorted({p for a in anims.values() for *_, p in a["events"]} | bodies)
    nxt = {p: q for p, q in zip(starts, starts[1:] + [len(b)])}
    for a in anims.values():
        a["events"] = [(t, s, e, list(struct.unpack_from(f"<{min(16, (nxt[p] - p) // 4)}I", b, p)))
                       for t, s, e, p in a["events"]]
    return anims


TAE_TEMPLATE_FMT = {"b": "?", "s8": "b", "u8": "B", "s16": "h", "u16": "H", "s32": "i", "u32": "I", "f32": "f",
                    "f32grad": "2f"}


def tae_template(xml_path):
    """DSAnimStudio TAE template (github.com/Meowmaritus/DSAnimStudio, DSAnimStudioNETCore/Res/TAE.Template.DS1.xml,
    GPL-3.0; read from the user's checkout, not copied) -> {event type: (name, [(field name, type, assert value or
    None, {value: enum name})])}. Unnamed fields get 'unk_<byte offset>'; group="X" prefixes 'X.'."""
    import xml.etree.ElementTree as ET
    out = {}
    for a in ET.parse(xml_path).getroot().iter("action"):
        fields, off = [], 0
        for f in a:
            if f.tag not in TAE_TEMPLATE_FMT:
                continue
            name = f.get("name") or f"unk_{off:02x}"
            if f.get("group"):
                name = f"{f.get('group')}.{name}"
            enum = {int(e.get("value"), 0): e.get("name") for e in f.iter("entry")}
            fields.append((name, f.tag, f.get("assert"), enum))
            off += struct.calcsize("<" + TAE_TEMPLATE_FMT[f.tag])
        out[int(a.get("id"))] = (a.get("name"), fields)
    return out


def tae_template_size(fields):
    return sum(struct.calcsize("<" + TAE_TEMPLATE_FMT[t]) for _, t, _, _ in fields)


def tae_named(event, template):
    """One souls.tae() event (type, start, end, [u32 words]) -> (type name, {field: value}) via tae_template().
    Enum fields give (value, name). Types missing from the template -> (None, {'raw': words})."""
    etype, _, _, words = event
    if etype not in template:
        return None, {"raw": list(words)}
    name, fields = template[etype]
    raw = struct.pack(f"<{len(words)}I", *words)
    vals, off = {}, 0
    for fname, t, _, enum in fields:
        fmt = "<" + TAE_TEMPLATE_FMT[t]
        if off + struct.calcsize(fmt) > len(raw):
            break
        v = struct.unpack_from(fmt, raw, off)
        v = v if len(v) > 1 else v[0]
        vals[fname] = (v, enum[v]) if enum and v in enum else v
        off += struct.calcsize(fmt)
    return name, vals


FIELD_FMT = {"s8": "b", "u8": "B", "s16": "h", "u16": "H", "s32": "i", "u32": "I", "f32": "f"}


def paramdef(b):
    """DS1 PARAMDEF (176-byte fields) -> (param type, [(internal name, primitive type, byte count, bits or None,
    array len)])."""
    count, fsize = struct.unpack_from("<HH", b, 8)
    ptype = cstr(b, 0xC)
    fields = []
    for i in range(count):
        f = 0x30 + fsize * i
        nbytes = struct.unpack_from("<i", b, f + 0x64)[0]
        # 32-byte fixed fields, space-padded and not always NUL-terminated
        # The internal type is often an enum name (EQUIP_BOOL); the display type at +0x40 is the primitive (u8).
        itype = b[f + 0x40:f + 0x48].split(b"\0")[0].decode("ascii").strip()
        name = b[f + 0x8C:f + 0xAC].split(b"\0")[0].decode("cp932", "replace").strip()  # one ShadowBank field is Japanese
        bits, arr = None, 1
        if ":" in name:
            name, bits = name.split(":")
            name, bits = name.strip(), int(bits)
        if "[" in name:
            name, arr = name[:-1].split("[")
            arr = int(arr)
        fields.append((name, itype, nbytes, bits, arr))
    return ptype, fields


def param(b, fields):
    """DS1 PARAM -> {row id: (name, {field: value})}. Bit fields pack LSB-first into units of their own byte size
    (a dummy8 pad bit field continues a u8 unit: ObjectParam isMoveObj:1 + pad_1:5 share one byte); signed bit
    fields are sign-extended. Duplicate row ids (LockCamParam 200, ObjectParam 4000 ...): the last row wins here,
    param_rows() lists every one."""
    return {rid: (name, vals) for rid, name, vals in param_rows(b, fields)}


def param_rows(b, fields):
    """DS1 PARAM -> [(row id, name, {field: value})] in file order, duplicates kept. A row ends at the next row's
    data (or the names / file end): the default_* DrawParam banks store older, shorter rows than the game's paramdef
    (default_ToneCorrectBank 32 B vs TONE_CORRECT_BANK 36 B without sfxMultiplier); fields past the end are omitted."""
    rows = struct.unpack_from("<H", b, 0xA)[0]
    hdr = [struct.unpack_from("<iII", b, 0x30 + 12 * r) for r in range(rows)]
    bounds = sorted({d for _, d, _ in hdr} | {n for _, _, n in hdr if n} | {len(b)})
    out = []
    for rid, doff, noff in hdr:
        name = cstr(b, noff) if noff else ""
        end = next(x for x in bounds if x > doff)
        vals, pos, unit, unit_bits, unit_size = {}, doff, 0, 0, None
        for fname, itype, nbytes, bits, arr in fields:
            if pos + (struct.calcsize(FIELD_FMT.get(itype, "B")) if bits is not None else nbytes) > end:
                break
            if bits is not None:
                size = struct.calcsize(FIELD_FMT.get(itype, "B"))
                if unit_size != size or unit_bits + bits > size * 8:
                    unit = int.from_bytes(b[pos:pos + size], "little")
                    pos += size
                    unit_bits, unit_size = 0, size
                v = (unit >> unit_bits) & ((1 << bits) - 1)
                if itype.startswith("s") and v >> (bits - 1):
                    v -= 1 << bits
                vals[fname] = v
                unit_bits += bits
                continue
            unit_size = None
            if itype in FIELD_FMT and arr == 1:
                vals[fname] = struct.unpack_from("<" + FIELD_FMT[itype], b, pos)[0]
            pos += nbytes
        out.append((rid, name, vals))
    return out


def game_params(game):
    """{param file stem: {row id: (name, {field: value})}} for GameParam, using the game's own paramdefs."""
    defs = {}
    for _, n, d in bnd3(read(game + "/paramdef/paramdef.paramdefbnd.dcx")):
        ptype, fields = paramdef(d)
        defs[ptype] = fields
    out = {}
    for _, n, d in bnd3(read(game + "/param/GameParam/GameParam.parambnd.dcx")):
        ptype = cstr(d, 0xC).strip()
        if ptype in defs:
            out[basename(n).rsplit(".", 1)[0]] = param(d, defs[ptype])
    return out


def _hk_sections(b, pos, end):
    """Havok 2015 tagfile sections: u32 big-endian (2 flag bits | 30-bit size incl. header), 4-char tag."""
    out = {}
    while pos < end:
        size = struct.unpack_from(">I", b, pos)[0] & 0x3FFFFFFF
        out[b[pos + 4:pos + 8].decode()] = (pos + 8, pos + size)
        pos += size
    return out


def _hk_varint(b, p):
    """Big-endian length-prefixed varint: 0xxxxxxx, 10xxxxxx+1, 110xxxxx+2, 11100xxx+3 bytes."""
    x = b[p]
    if x < 0x80:
        return x, p + 1
    if x < 0xC0:
        return ((x & 0x3F) << 8) | b[p + 1], p + 2
    if x < 0xE0:
        return ((x & 0x1F) << 16) | (b[p + 1] << 8) | b[p + 2], p + 3
    return ((x & 0x07) << 24) | (b[p + 1] << 16) | (b[p + 2] << 8) | b[p + 3], p + 4


class HkType:
    def __init__(self, name):
        self.name, self.parent, self.flags, self.fmt, self.sub, self.size, self.fields = name, None, 0, 0, None, 0, []

    def field(self, name):
        """(byte offset, HkType) of a field, searching parent types too."""
        t = self
        while t:
            for fname, off, ftype in t.fields:
                if fname == name:
                    return off, ftype
            t = t.parent
        raise KeyError(f"{self.name} has no field {name}")


class HkTag:
    """Havok 2015 binary tagfile (TAG0), after SoulsAssetPipeline's HkBinaryTagfileReader/HkBinaryTypeReader.
    Pointer, string and array fields hold a u32 index into the ITEM list (type, DATA offset, count)."""

    def __init__(self, b):
        assert b[4:8] == b"TAG0", "not a Havok 2015 tagfile"
        top = _hk_sections(b, 8, struct.unpack_from(">I", b, 0)[0] & 0x3FFFFFFF)
        self.data = b[top["DATA"][0]:top["DATA"][1]]
        typ = _hk_sections(b, *top["TYPE"])
        tstr = [x.decode() for x in b[typ["TSTR"][0]:typ["TSTR"][1]].split(b"\0")]
        fstr = [x.decode() for x in b[typ["FSTR"][0]:typ["FSTR"][1]].split(b"\0")]
        p, _ = typ["TNAM"]
        count, p = _hk_varint(b, p)
        self.types = [HkType(None) for _ in range(count)]
        for t in self.types:
            ni, p = _hk_varint(b, p)
            t.name = tstr[ni]
            nt, p = _hk_varint(b, p)
            for _ in range(nt & 0x3F):
                _, p = _hk_varint(b, p)
                _, p = _hk_varint(b, p)
        ti = lambda i: self.types[i - 1] if i else None
        p, end = typ["TBOD"]
        while p < end:
            i, p = _hk_varint(b, p)
            if not i:
                continue
            t = ti(i)
            par, p = _hk_varint(b, p)
            t.parent = ti(par)
            t.flags, p = _hk_varint(b, p)
            if t.flags & 1:
                t.fmt, p = _hk_varint(b, p)
            if t.flags & 2:
                sub, p = _hk_varint(b, p)
                t.sub = ti(sub)
            if t.flags & 4:
                _, p = _hk_varint(b, p)
            if t.flags & 8:
                t.size, p = _hk_varint(b, p)
                _, p = _hk_varint(b, p)
            if t.flags & 0x10:
                _, p = _hk_varint(b, p)
            if t.flags & 0x20:
                n = b[p]
                p += 1
                if n == 0xC3:
                    n = b[p]
                    p += 1
                    if n == 0:
                        n, p = _hk_varint(b, p)
                for _ in range(n & 0x3F):
                    nm, p = _hk_varint(b, p)
                    _, p = _hk_varint(b, p)
                    off, p = _hk_varint(b, p)
                    ft, p = _hk_varint(b, p)
                    t.fields.append((fstr[nm], off, ti(ft)))
            if t.flags & 0x40:
                n, p = _hk_varint(b, p)
                for _ in range(2 * n):
                    _, p = _hk_varint(b, p)
        idx = _hk_sections(b, *top["INDX"])
        self.items = []
        p, end = idx["ITEM"]
        while p + 12 <= end:
            flags, off, cnt = struct.unpack_from("<III", b, p)
            self.items.append((ti(flags & 0xFFFFFF), off, cnt))
            p += 12

    def objects(self, type_name):
        """DATA offsets of every object of this type (items, not array elements)."""
        return [off for t, off, cnt in self.items if t and t.name == type_name for _ in [0]]

    def item(self, obj_off, obj_type, field):
        """(element type, offset, count) of the item a pointer/array/string field points to."""
        off, _ = obj_type.field(field)
        i = struct.unpack_from("<I", self.data, obj_off + off)[0]
        return self.items[i] if i else (None, 0, 0)

    def get(self, obj_off, obj_type, field, fmt):
        off, _ = obj_type.field(field)
        return struct.unpack_from("<" + fmt, self.data, obj_off + off)[0]

    def type(self, name):
        return next(t for t in self.types if t.name == name)


def hk_tag(b):
    """Havok TAG0 file -> (DATA bytes, type names, [(type name, data offset, count)] items)."""
    h = HkTag(b)
    return h.data, [t.name for t in h.types], [(t.name if t else None, o, c) for t, o, c in h.items]


def root_motion(hkx):
    """Root-motion samples of an animation (hkaDefaultAnimatedReferenceFrame): [(x, y, z, yaw)] at 30 fps."""
    data, _, items = hk_tag(hkx)
    vecs = [(o, c) for t, o, c in items if t == "hkVector4" and c > 1]
    if not vecs:
        return []
    o, c = max(vecs, key=lambda v: v[1])
    return [struct.unpack_from("<4f", data, o + 16 * i) for i in range(c)]


def sin_cos(a):
    """(sin a, cos a) correctly rounded, the same bits on every platform: math.sin / cos come from the C library and
    differ by an ulp between glibc (native) and musl (Pyodide, the in-browser converter), e.g. sin(-0.8163371682167053)
    on c1201's bones (TECH_DEBT build-7); glibc itself is off by an ulp on ~0.1% of random inputs in [-pi, pi]. Taylor
    series in 50-digit decimal, then one rounding to double; for the few angles per bone / part, not per vertex."""
    import decimal
    if a == 0:
        return a, 1.0  # keeps the sign of -0.0, as libm does
    with decimal.localcontext() as ctx:
        ctx.prec = 50
        x = decimal.Decimal(a)
        x2, eps = x * x, decimal.Decimal("1e-45")
        s, c, ts, tc, n = x, decimal.Decimal(1), x, decimal.Decimal(1), 0
        while abs(ts) >= eps or abs(tc) >= eps:
            n += 2
            ts, tc = -ts * x2 / (n * (n + 1)), -tc * x2 / ((n - 1) * n)
            s, c = s + ts, c + tc
        return float(s), float(c)


def _euler_xzy_to_quat(rx, ry, rz):
    """FLVER bone rotation: Euler radians applied X, then Z, then Y (SoulsFormats FLVER.Bone.ComputeLocalTransform)."""
    def axis(ax, a):
        s, c = sin_cos(a / 2)
        return (s if ax == 0 else 0.0, s if ax == 1 else 0.0, s if ax == 2 else 0.0, c)

    def mul(a, b):
        ax, ay, az, aw = a
        bx, by, bz, bw = b
        return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)
    return mul(axis(1, ry), mul(axis(2, rz), axis(0, rx)))


# FLVER vertex buffers read as numpy strided views (was one struct call per attribute per vertex: ~10 s of a map
# export); the lists hold the same tuples of Python floats / ints. SOULS_NP=0: the per-vertex loop (A/B).
FLVER_NP = os.environ.get("SOULS_NP", "1") == "1"
_FL_SNORM = (0x10, 0x11, 0x13, 0x2F)


def _flver_verts_np(b, base, vsize, vcount, layout, verts):
    """One vertex buffer's attributes -> appended to verts like flver()'s per-vertex loop (attributes of one list
    from several layout members interleave per vertex, as appended there). False when numpy is missing."""
    try:
        import numpy as np
    except ImportError:
        return False

    def view(off, dt, k):
        return np.ndarray((vcount, k), dt, buffer=b, offset=base + off, strides=(vsize, np.dtype(dt).itemsize))
    cols = {}
    for _, so, typ, sem, si in layout:
        if sem == 0 and typ == 0x02:
            cols.setdefault("pos", []).append(view(so, "<f4", 3).astype(np.float64))
        elif sem == 3 and typ in _FL_SNORM:
            cols.setdefault("normal", []).append((view(so, np.uint8, 3).astype(np.float64) - 127) / 127.0)
        elif sem == 2:
            cols.setdefault("bone_idx", []).append(view(so, np.uint8, 4).astype(np.int64))
        elif sem == 1:
            w = view(so, "<i2", 4).astype(np.float64) / 32767.0 if typ == 0x1A else \
                view(so, np.uint8, 4).astype(np.float64) / 255.0
            cols.setdefault("bone_w", []).append(w)
        elif sem in (6, 7) and si == 0 and typ in _FL_SNORM:
            cols.setdefault("tangent" if sem == 6 else "binormal", []).append(
                (view(so, np.uint8, 4).astype(np.float64) - 127) / 127.0)
        elif sem == 10 and si == 0 and typ in _FL_SNORM:
            cols.setdefault("color", []).append(view(so, np.uint8, 4).astype(np.float64) / 255.0)
        elif sem == 5 and si == 0 and typ in (0x15, 0x16):
            four = typ == 0x16 and so + 8 <= vsize
            u = view(so if so + 4 <= vsize else vsize - 4, "<i2", 4 if four else 2).astype(np.float64)
            cols.setdefault("uv", []).append(u[:, :2] / 1024.0)
            if four:
                cols.setdefault("uv2", []).append(u[:, 2:4] / 1024.0)
    for name, arrs in cols.items():
        a = arrs[0] if len(arrs) == 1 else np.stack(arrs, 1).reshape(-1, arrs[0].shape[1])
        verts[name].extend(map(tuple, a.tolist()))
    return True


def flver(b):
    """DS1 FLVER2 (version 0x2000C) -> dict(bones=[(name, parent, t, q, s)], meshes=[dict(dynamic, material, mtd,
    default_bone, diffuse, diffuse2, bump,
    bones, facesets=[(flags, strip, indices)], verts=dict(pos, normal, bone_idx, bone_w, uv, uv2, color, tangent,
    binormal))], materials=[name])"""
    assert b[:6] == b"FLVER\0", "not a FLVER"
    data_off = struct.unpack_from("<i", b, 0xC)[0]
    n_dummy, n_mat, n_bone, n_mesh, n_vb = struct.unpack_from("<5i", b, 0x14)
    idx_size = b[0x48]
    n_fs, n_layout, n_tex = struct.unpack_from("<3i", b, 0x50)
    p = 0x80 + 0x40 * n_dummy
    mats, mat_tex, mtds = [], [], []
    for i in range(n_mat):
        name_off, mtd_off, tcount, tindex = struct.unpack_from("<4i", b, p + 0x20 * i)
        mats.append(wstr(b, name_off) if b[0x49] else cstr(b, name_off))
        mat_tex.append(range(tindex, tindex + tcount))
        mtds.append(basename(wstr(b, mtd_off) if b[0x49] else cstr(b, mtd_off)))
    p += 0x20 * n_mat
    bones = []
    for i in range(n_bone):
        o = p + 0x80 * i
        t = struct.unpack_from("<3f", b, o)
        name = wstr(b, struct.unpack_from("<i", b, o + 12)[0])
        rot = struct.unpack_from("<3f", b, o + 16)
        parent = struct.unpack_from("<h", b, o + 28)[0]
        s = struct.unpack_from("<3f", b, o + 32)
        bones.append((name, parent, t, _euler_xzy_to_quat(*rot), s))
    p += 0x80 * n_bone
    mesh_hdr = []
    for i in range(n_mesh):
        o = p + 0x30 * i
        dyn = b[o]
        mat, _, _, dbone, nb, _, b_off, nfs, fs_off, nvb, vb_off = struct.unpack_from("<11i", b, o + 4)
        mesh_hdr.append((dyn, mat, dbone, list(struct.unpack_from(f"<{nb}i", b, b_off)) if nb else [],
                         list(struct.unpack_from(f"<{nfs}i", b, fs_off)), list(struct.unpack_from(f"<{nvb}i", b, vb_off))))
    p += 0x30 * n_mesh
    fsets = []
    for i in range(n_fs):
        o = p + 0x20 * i
        flags, strip, cull, _, count, ioff = struct.unpack_from("<IBBhii", b, o)
        isz = struct.unpack_from("<i", b, o + 0x18)[0] or idx_size
        fmt = "H" if isz == 16 else "I"
        fsets.append((flags, strip, list(struct.unpack_from(f"<{count}{fmt}", b, data_off + ioff)), cull))
    p += 0x20 * n_fs
    vbufs = [struct.unpack_from("<8i", b, p + 0x20 * i) for i in range(n_vb)]
    p += 0x20 * n_vb
    layouts = []
    for i in range(n_layout):
        cnt, _, _, m_off = struct.unpack_from("<4i", b, p + 0x10 * i)
        layouts.append([struct.unpack_from("<5i", b, m_off + 0x14 * k) for k in range(cnt)])
    p += 0x10 * n_layout
    rd = wstr if b[0x49] else cstr
    textures = []  # (path, type) e.g. ("N:\\...\\BD_M_9360_1.tif", "g_Diffuse")
    for i in range(n_tex):
        path_off, type_off = struct.unpack_from("<ii", b, p + 0x20 * i)
        textures.append((rd(b, path_off), rd(b, type_off)))
    meshes = []
    for dyn, mat, dbone, mbones, fs_idx, vb_idx in mesh_hdr:
        verts = {"pos": [], "normal": [], "bone_idx": [], "bone_w": [], "uv": [], "uv2": [], "color": [],
                 "tangent": [], "binormal": []}
        for vbi in vb_idx:
            _, li, vsize, vcount, _, _, _, boff = vbufs[vbi]
            if FLVER_NP and vcount and _flver_verts_np(b, data_off + boff, vsize, vcount, layouts[li], verts):
                continue
            for v in range(vcount):
                base = data_off + boff + v * vsize
                for _, so, typ, sem, si in layouts[li]:
                    o = base + so
                    if sem == 0 and typ == 0x02:
                        verts["pos"].append(struct.unpack_from("<3f", b, o))
                    elif sem == 3 and typ in (0x10, 0x11, 0x13, 0x2F):
                        verts["normal"].append(tuple((c - 127) / 127.0 for c in b[o:o + 3]))
                    elif sem == 2:
                        verts["bone_idx"].append(tuple(b[o:o + 4]))
                    elif sem == 1:
                        if typ == 0x1A:
                            verts["bone_w"].append(tuple(c / 32767.0 for c in struct.unpack_from("<4h", b, o)))
                        else:
                            verts["bone_w"].append(tuple(c / 255.0 for c in b[o:o + 4]))
                    elif sem in (6, 7) and si == 0 and typ in (0x10, 0x11, 0x13, 0x2F):  # xyz, w = handedness
                        verts["tangent" if sem == 6 else "binormal"].append(tuple((c - 127) / 127.0 for c in b[o:o + 4]))
                    elif sem == 10 and si == 0 and typ in (0x10, 0x11, 0x13, 0x2F):
                        verts["color"].append(tuple(c / 255.0 for c in b[o:o + 4]))
                    elif sem == 5 and si == 0 and typ in (0x15, 0x16):
                        if so + 4 > vsize:  # FG_A_0000's eye buffer: the layout lists a tangent the 36-byte
                            o = base + vsize - 4  # vertices lack, so its UV offset points past the end (UV is last)
                        u = struct.unpack_from("<4h" if typ == 0x16 and so + 8 <= vsize else "<2h", b, o)
                        verts["uv"].append((u[0] / 1024.0, u[1] / 1024.0))
                        if len(u) == 4:  # UV pair: the second set (FaceGen face: the detail texture's UVs)
                            verts["uv2"].append((u[2] / 1024.0, u[3] / 1024.0))
        diffuse = next((basename(textures[t][0]).rsplit(".", 1)[0] for t in mat_tex[mat]
                        if textures[t][1].lower().startswith("g_diffuse")), None) if mat < len(mats) else None
        # [M] (multi-texture) materials: second diffuse, blended in by vertex colour alpha
        diffuse2 = next((basename(textures[t][0]).rsplit(".", 1)[0] for t in mat_tex[mat]
                         if textures[t][1].lower() == "g_diffuse_2"), None) if mat < len(mats) else None
        # normal map (g_Bumpmap: BC5, red along the binormal, green along the tangent; FRPG_Phn_*Bmp* shaders)
        bump = next((basename(textures[t][0]).rsplit(".", 1)[0] for t in mat_tex[mat]
                     if textures[t][1].lower() == "g_bumpmap"), None) if mat < len(mats) else None
        meshes.append({"dynamic": dyn, "material": mats[mat] if mat < len(mats) else "", "default_bone": dbone,
                       "diffuse": diffuse, "diffuse2": diffuse2, "bump": bump, "mtd": mtds[mat] if mat < len(mtds) else "",
                       "bones": mbones, "facesets": [fsets[i] for i in fs_idx], "verts": verts})
    return {"bones": bones, "meshes": meshes, "materials": mats, "textures": textures}


def triangles(faceset):
    """Face set -> triangle index triples (strips are unrolled; 0xFFFF restarts; degenerate triangles dropped).
    Winding is kept as stored: DS front faces are clockwise on screen (Direct3D)."""
    flags, strip, idx = faceset[:3]
    tris = []
    if not strip:
        return [tuple(idx[i:i + 3]) for i in range(0, len(idx) - 2, 3)]
    flip = False
    for i in range(len(idx) - 2):
        a, b, c = idx[i], idx[i + 1], idx[i + 2]
        if 0xFFFF in (a, b, c):
            flip = False
            continue
        if a != b and b != c and a != c:
            tris.append((a, c, b) if flip else (a, b, c))
        flip = not flip
    return tris


def tpf(b):
    """TPF (PC) -> {texture name: DDS bytes}. Entries are 0x14 bytes; names are 1-byte strings."""
    n = struct.unpack_from("<i", b, 8)[0]
    out = {}
    for i in range(n):
        off, size, _, _, _, _, noff = struct.unpack_from("<iiBBBBi", b, 0x10 + 0x14 * i)
        out[cstr(b, noff, "latin-1")] = b[off:off + size]
    return out


def dds_average(d, target=16):
    """Average RGB of a DXT1/DXT3/DXT5 DDS, from the first mip level no larger than `target` px. None if unsupported."""
    h, w, mips = struct.unpack_from("<iii", d, 12)[0], struct.unpack_from("<i", d, 16)[0], struct.unpack_from("<i", d, 28)[0]
    four = d[84:88]
    bsize = {b"DXT1": 8, b"DXT3": 16, b"DXT5": 16}.get(four)
    if not bsize:
        return None
    off = 128
    for _ in range(max(mips, 1)):
        if max(w, h) <= target or mips <= 1:
            break
        off += max(1, w // 4) * max(1, h // 4) * bsize
        w, h, mips = max(1, w // 2), max(1, h // 2), mips - 1
    tot, n = [0, 0, 0], 0
    for blk in range(max(1, w // 4) * max(1, h // 4)):
        o = off + blk * bsize + (bsize - 8)
        c0, c1, bits = struct.unpack_from("<HHI", d, o)
        cols = [((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31) for c in (c0, c1)]
        cols += [tuple((2 * a + b) // 3 for a, b in zip(*cols)), tuple((a + 2 * b) // 3 for a, b in zip(*cols))]
        if bsize == 8 and c0 <= c1:
            cols[2] = tuple((a + b) // 2 for a, b in zip(cols[0], cols[1]))
        for px in range(16):
            c = cols[bits >> (2 * px) & 3]
            for k in range(3):
                tot[k] += c[k]
            n += 1
    return tuple(t // n for t in tot)


def dds_decode(d, max_dim=256):
    """DXT1/3/5 DDS -> (width, height, RGB bytes) of the largest mip level whose sides are <= max_dim. None if the
    format is not DXT1/3/5. Alpha is ignored."""
    h, w, mips = struct.unpack_from("<i", d, 12)[0], struct.unpack_from("<i", d, 16)[0], struct.unpack_from("<i", d, 28)[0]
    bsize = {b"DXT1": 8, b"DXT3": 16, b"DXT5": 16}.get(d[84:88])
    if not bsize:
        return None
    off = 128
    for _ in range(max(mips, 1) - 1):
        if max(w, h) <= max_dim:
            break
        off += max(1, w // 4) * max(1, h // 4) * bsize
        w, h = max(1, w // 2), max(1, h // 2)
    out = bytearray(w * h * 3)
    bw = max(1, w // 4)
    for by in range(max(1, h // 4)):
        for bx in range(bw):
            o = off + (by * bw + bx) * bsize + (bsize - 8)
            c0, c1, bits = struct.unpack_from("<HHI", d, o)
            cols = [((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31) for c in (c0, c1)]
            if bsize == 8 and c0 <= c1:
                cols += [tuple((a + b) // 2 for a, b in zip(*cols)), (0, 0, 0)]
            else:
                cols += [tuple((2 * a + b) // 3 for a, b in zip(*cols)), tuple((a + 2 * b) // 3 for a, b in zip(*cols))]
            for py in range(4):
                for px in range(4):
                    x, y = bx * 4 + px, by * 4 + py
                    if x < w and y < h:
                        c = cols[bits >> (2 * (py * 4 + px)) & 3]
                        out[(y * w + x) * 3:(y * w + x) * 3 + 3] = bytes(c)
    return w, h, bytes(out)


_bc = None


def dds_decode_rgba(d, max_dim=256):
    """DDS (BC1/2/3, or DX10 BC7) -> (w, h, RGBA bytes) of the largest mip with sides <= max_dim, via
    tools/native/libbcdecode.so (bcdec). None if the format is unsupported."""
    import ctypes, os
    global _bc
    if _bc is None:
        native = os.path.join(os.path.dirname(os.path.abspath(__file__)), "native")
        so = os.path.join(native, "libbcdecode.so")
        if not os.path.exists(so):  # built on first use, like pack.py's liblz4pack.so (not tracked in git)
            if os.system(f"gcc -O2 -shared -fPIC -o '{so}' '{os.path.join(native, 'bcdecode.c')}'"):
                raise RuntimeError("building tools/native/libbcdecode.so failed")
        _bc = ctypes.CDLL(so)
    h, w = struct.unpack_from("<ii", d, 12)
    mips = struct.unpack_from("<i", d, 28)[0]
    four = d[84:88]
    data = 128
    if four == b"DX10":
        fmt = {98: 7, 99: 7, 71: 1, 72: 1, 74: 2, 75: 2, 77: 3, 78: 3}.get(struct.unpack_from("<i", d, 128)[0])
        data = 148
    else:
        fmt = {b"DXT1": 1, b"DXT3": 2, b"DXT5": 3}.get(four)
    if not fmt:
        return None
    bsize = 8 if fmt == 1 else 16
    for _ in range(max(mips, 1) - 1):
        if max(w, h) <= max_dim:
            break
        data += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bsize
        w, h = max(1, w // 2), max(1, h // 2)
    out = ctypes.create_string_buffer(w * h * 4)
    src = d[data:data + max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bsize]
    if _bc.decode_image(src, w, h, fmt, out):
        return None
    return w, h, out.raw


def nvm(b):
    """DS1 navmesh -> (vertices [(x, y, z)], triangles [(a, b, c, flags)])"""
    nv, _, nt, toff = struct.unpack_from("<iiii", b, 4)
    verts = [struct.unpack_from("<3f", b, 0x80 + 12 * i) for i in range(nv)]
    tris = []
    for i in range(nt):
        a, bb, c, _, _, _, fl = struct.unpack_from("<7i", b, toff + 28 * i)
        tris.append((a, bb, c, fl))
    return verts, tris


def flver_bind_world(F):
    """World (t, q) of every FLVER bone in bind pose. Non-skinned (dynamic 0) mesh vertices are relative to their
    mesh's default bone: verified on Firelink map pieces, whose raw vertices fall outside the model bounding box."""
    out = []
    for name, parent, t, q, s in F["bones"]:
        if parent < 0:
            out.append((t, q))
        else:
            pt, pq = out[parent]
            r = _qrot(pq, t)
            out.append(((pt[0] + r[0], pt[1] + r[1], pt[2] + r[2]), _qmul(pq, q)))
    return out


def _qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def _qrot(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    return (vx + w * tx + y * tz - z * ty, vy + w * ty + z * tx - x * tz, vz + w * tz + x * ty - y * tx)
