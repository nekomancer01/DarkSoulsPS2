"""Export an enemy / NPC character (chr/c####) for the PS2 prototype: model.bin (export_model.py's layout and writer)
+ chr.bin (one variant per NpcParam / NpcThinkParam pair placed in the map, anims with root motion and TAE windows,
attack hit capsules). Packed by tools/pack.py into data/c####.pak.

Variants: every MSB enemy part of this model in the map (tools/area.py enemy_parts) gives an (NpcParam, ThinkParam)
pair; each keeps its own HP, defence, turn rate, capsule, model masks and think values (perception, distances, the
GetExcelParam table the AI scripts read).
Model: the chrbnd FLVER at one LOD. Textures from the chrbnd tpf, the external chr/c####.chrtpfbdt (chrtpfbhd in the
chrbnd) and NpcParam.normalChangeTexChrId's chrbnd; at most TEX_MAX px (VRAM: every type stays resident).
Meshes named "#NN#..." are equipment variants shown only when a variant's NpcParam modelDispMaskNN is set: such a
mesh's triangle group gets flag bit 8 + NN, and enemy.c hides the bits its variant does not show (mdl_hide). Meshes
no variant shows are dropped.
Skeleton: anibnd skeleton.hkx. If it has more bones than the VU1 palette holds (61), bones no vertex / hit capsule
uses are folded into their children (their local transforms multiplied in per frame), keeping the hierarchy.
Animations: the ESD essentials (ANIMS) + every anim id the variants' battle / logic scripts name as a constant
(Lua chunks of script/<map>.luabnd), plus the NpcParam.animIdOffset versions (a01_ = 10000 + id, a02_ = 20000 + id)
for variants that use them.

chr.bin (little-endian):
  header  "DSRC", u32 version 5, u32 anims, windows, variants, u32 anim_off, win_off, root_off, var_off,
          u32 hide_bits (every variant mesh bit used), u32 upper[2] (bit per bone: the first "Spine" bone and its
          descendants, the guard pose layer)                                                              (48 B)
  variant i32 npc, think, hp, logic, battle, anim_offset; u32 show_bits;
          f32 radius, height, turn deg/s, def_phys, poise (superArmorDurability),
          f32 eye_dist, eye_ang_x, eye_ang_y, ear_dist, ear_cut, near, mid, far, out, sight_forget, sound_forget (s),
          i32 goal_action_caution, goal_id_caution, goal_action_find, goal_id_find, f32 call_range, call_min_dist,
          i32 team_type, f32 excel[24] (AI_EXCEL_THINK_PARAM_TYPE__*)                                    (216 B)
  anim    i32 id, f32 length s, u32 first window, u32 windows, u32 first root sample, u32 root samples   (24 B)
  window  i16 type (ChrActionFlag id, or -1 = attack hit), i16 bone A, i16 bone B, i16 damage level (AtkParam
          dmgLevel), i32 AtkParam_Npc row, f32 start, end, atkPhys, radius, poise damage (atkSuperArmor),
          f32 a[3], b[3] (capsule ends, bone-local: AtkParam hit0_DmyPoly1 / 2),
          f32 atkPhysCorrection / 100 (0 on the NPC rows read as 1)                                        (60 B)
  root    f32 x, z, yaw at 30 fps from the anim start (forward = -z), as souls.root_motion
Usage: export_chr.py <game dir> <chr id e.g. c2900> <map id> <out dir> [lod 0/1/2] [tex max px]"""
import math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import havok_anim as ha
import lua50
import export_model
from export_model import MeshBuilder, bake, bind_world, to_local, write_model
from area import enemy_parts

# Anims every enemy needs (enemyCommon.esd): idle, walk F/B/L/R, run, steps, turns, damage (2000-2014 by level /
# direction, f222), deaths 2200 / 2205, guard 9910 (DoEzAction while approaching)
ANIMS = [0, 200, 201, 202, 203, 500, 700, 701, 702, 703, 2000, 2001, 2002, 2004, 2005, 2006, 2007, 2010, 2200, 2205,
         9910]
# Script constants in these ranges name anims: attacks / special actions, steps, guard poses
SCRIPT_ANIM = [(3000, 3999), (700, 799), (6000, 6999), (9900, 9999)]
# ChrActionFlag: 5 parry window, 7 disable turning, 8 invincible, 24 super armour, and the enemy ESD gates (exe
# FUN_140340580 -> ctx+0x19C): 23 bit0 (f107), 11 / 79 bit1 (f118 steps), 86 bit4 (f120 combo continuation)
FLAGS = (5, 7, 8, 11, 23, 24, 79, 86)
LODS = {0: 0x0, 1: 0x1000000, 2: 0x2000000}
PALETTE = 61
MAX_ANIMS = 64  # model.c table
# GetExcelParam index -> NpcThinkParam field (ai_define.lua AI_EXCEL_THINK_PARAM_TYPE__*); None = derived below
EXCEL = [None, "maxBackhomeDist", "backhomeDist", "backhomeBattleDist", "nonBattleActLife", "BattleStartDist", None,
         "idAttackCannotMove", "battleGoalID", "BackHome_LookTargetTime", "BackHome_LookTargetDist",
         "BackHomeLife_OnHitEneWal", None, None, "callHelp_MyPeerId", "callHelp_CallPeerId", None,
         "callHelp_CallActionId", "callHelp_ReplyBehaviorType", "callHelp_ForgetTimeByArrival", "callHelp_MinWaitTime",
         "callHelp_MaxWaitTime", "callHelp_ActionAnimId", "thinkAttr_doAdmirer"]
FORGET_FPS = 30.0  # Sight/SoundTargetForgetTime read as frames at 30 fps (600 -> 20 s; TECH_DEBT enemy-1)


class ChrMeshBuilder(MeshBuilder):
    """MeshBuilder with a smaller texture cap (every enemy type's textures stay in VRAM)"""

    def __init__(self, sk_index, tex_max):
        super().__init__(sk_index)
        self.tex_max = tex_max

    def texture(self, name, dds):
        if dds and name not in self.tex_index:
            dec = souls.dds_decode(dds, self.tex_max)
            if not dec:  # BC7 (DX10) bodies: decode via bcdec, drop alpha
                rgba = souls.dds_decode_rgba(dds, self.tex_max)
                if rgba:
                    w, h, px = rgba
                    dec = (w, h, bytes(px[i] for i in range(len(px)) if i % 4 != 3))
            # resample to <= tex_max high and exactly >= 128 wide (PSMT8 uploads; quantize would otherwise widen a
            # narrow mip to 128 keeping its aspect, e.g. 32x128 -> 128x512). UVs are normalized per texture.
            if dec and (max(dec[0], dec[1]) > self.tex_max or dec[0] < 128):
                from PIL import Image
                w, h = dec[0], dec[1]
                im = Image.frombytes("RGB", (w, h), dec[2]).resize((max(128, min(w, self.tex_max)),
                                                                    max(8, min(h, self.tex_max))), Image.LANCZOS)
                dec = (im.width, im.height, im.tobytes())
            if dec:
                self.tex_index[name] = len(self.textures)
                self.textures.append(export_model.quantize(*dec))
        return self.tex_index.get(name, -1)


def dummies(b):
    """FLVER2 dummy polys: [(ref id, pos, parent bone, attach bone)] (64 B each at 0x80)"""
    n = struct.unpack_from("<i", b, 0x14)[0]
    out = []
    for i in range(n):
        o = 0x80 + 0x40 * i
        pos = struct.unpack_from("<3f", b, o)
        ref, parent = struct.unpack_from("<hh", b, o + 28)
        attach = struct.unpack_from("<h", b, o + 44)[0]
        out.append((ref, pos, parent, attach))
    return out


def script_anims(game, map_id, goal_ids):
    """Anim ids named as numeric constants in the Lua chunks of these battle / logic goal ids (NNNNNN_battle.lua,
    NNNNNN_logic.lua in the map's luabnd and aiCommon)"""
    names = {f"{g:06d}_battle.lua" for g in goal_ids} | {f"{g:06d}_logic.lua" for g in goal_ids}
    out = set()

    def walk(f):
        for v in f.k:
            if isinstance(v, float) and v == int(v) and any(lo <= v <= hi for lo, hi in SCRIPT_ANIM):
                out.add(int(v))
        for s in f.p:
            walk(s)
    for arc in (f"{game}/script/{map_id}.luabnd.dcx", f"{game}/script/aiCommon.luabnd.dcx"):
        for _, n, d in souls.bnd3(souls.read(arc)):
            if souls.basename(n) in names and d[:5] == b"\x1bLuaP":
                walk(lua50.load(d))
    return out


def textures_of(game, cid, chrbnd, alt):
    tex = {}
    for k, d in chrbnd.items():
        if k.endswith(".tpf"):
            tex.update(souls.tpf(d))
        elif k.endswith(".chrtpfbhd"):
            bdt = f"{game}/chr/{cid}.chrtpfbdt"
            if os.path.exists(bdt):
                for _, n, d2 in souls.bnd3(d, souls.read(bdt)):
                    if souls.basename(n).endswith((".tpf", ".tpf.dcx")):
                        tex.update({nm: v for nm, v in souls.tpf(d2).items() if nm not in tex})
    if alt > 0 and os.path.exists(f"{game}/chr/c{alt:04d}.chrbnd.dcx"):
        altbnd = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(f"{game}/chr/c{alt:04d}.chrbnd.dcx"))}
        for k, d in altbnd.items():
            if k.endswith(".tpf"):
                tex.update({n: v for n, v in souls.tpf(d).items() if n not in tex})
    return tex


def qt_mul(a, b):
    """(q, t) local transforms: a * b (b in a's space)"""
    qa, ta = a
    qb, tb = b
    r = ha.qrot(qa, tb)
    return ha.qmul(qa, qb), (ta[0] + r[0], ta[1] + r[1], ta[2] + r[2])


def reduce_skeleton(names, parents, ref, keep):
    """Keep the bones in `keep` (+ the root): returns (new names, parents, ref, old -> new index map, fold) where
    fold(pose) turns a full pose [(q, t)] into the reduced one, folding dropped ancestors into their kept children"""
    keep = sorted(set(keep) | {i for i, p in enumerate(parents) if p < 0})
    new_of = {o: n for n, o in enumerate(keep)}

    def kept_parent(i):
        p = parents[i]
        chain = []
        while p >= 0 and p not in new_of:
            chain.append(p)
            p = parents[p]
        return p, chain[::-1]  # nearest kept ancestor, dropped bones from it down to i's parent
    info = [kept_parent(o) for o in keep]

    def fold(pose):
        out = []
        for o, (kp, chain) in zip(keep, info):
            x = None
            for c in chain:
                x = pose[c] if x is None else qt_mul(x, pose[c])
            out.append(pose[o] if x is None else qt_mul(x, pose[o]))
        return out
    ref_pose = fold([(r[1], r[0]) for r in ref])
    n_ref = [(t, q, (1, 1, 1)) for q, t in ref_pose]
    n_parents = [new_of[kp] if kp >= 0 else -1 for kp, _ in info]
    return [names[o] for o in keep], n_parents, n_ref, new_of, fold


def _q16(c):
    return int(round(max(-1, min(1, c)) * 32767))


def _t16(c):
    return int(round(max(-7.99, min(7.99, c)) * 4096))


def append_compact_anims(path, nbones, anims):
    """Replace model.bin's (empty) anim section by compact anims at the end of the file (model.c reads both kinds).
    Compact anim: u32 id, u32 frames | 1 << 31, u8 fps, u8 rotated bones, u8 translated bones, u8 pad,
    u8 rot bone ids[], u8 trans bone ids[] (padded to 4), base pose PackedBone[bones] (16 B: q s16x4 / 32767,
    t s16x3 / 4096, pad), then per frame q s16x4 of the rotated bones and t s16x3 of the translated ones (padded to
    4). Bones whose rotation / translation never leaves the first frame's (quantized) value use the base pose.
    Sampled at 15 fps (RAM; poses are interpolated, TAE windows and root motion keep their own timing)."""
    blob = bytearray(open(path, "rb").read())
    blob += b"\0" * (-len(blob) % 16)
    anim_off = len(blob)
    for aid, baked in anims:
        fps = 15
        step = 30 // fps
        frames = baked[::step]
        if (len(baked) - 1) % step:
            frames.append(baked[-1])
        q = [[tuple(_q16(c) for c in pose[b][0]) for pose in frames] for b in range(nbones)]
        t = [[tuple(_t16(c) for c in pose[b][1]) for pose in frames] for b in range(nbones)]
        rot = [b for b in range(nbones) if any(abs(x - y) > 2 for f in q[b] for x, y in zip(f, q[b][0]))]
        tra = [b for b in range(nbones) if any(abs(x - y) > 2 for f in t[b] for x, y in zip(f, t[b][0]))]
        head = struct.pack("<II4B", aid, len(frames) | 1 << 31, fps, len(rot), len(tra), 0) + bytes(rot) + bytes(tra)
        head += b"\0" * (-len(head) % 4)
        base = b"".join(struct.pack("<4h3hh", *q[b][0], *t[b][0], 0) for b in range(nbones))
        body = bytearray()
        for i in range(len(frames)):
            body += b"".join(struct.pack("<4h", *q[b][i]) for b in rot)
            body += b"".join(struct.pack("<3h", *t[b][i]) for b in tra)
            body += b"\0" * (-len(body) % 4)
        blob += head + base + body
    struct.pack_into("<I", blob, 16, len(anims))
    struct.pack_into("<I", blob, 44, anim_off)
    open(path, "wb").write(blob)
    print(f"  compact anims: {len(blob) - anim_off} bytes for {len(anims)}")


def think_values(think, npc):
    ex = [0.0] * 24
    for i, f in enumerate(EXCEL):
        if f and f in think:
            ex[i] = float(think[f])
    ex[6] = 1.0  # bMoveOnHearSound
    ex[12] = 1.0 if think["callHelp_CallValidRange"] > 0 else 0.0   # callHelp_IsCall
    ex[13] = 1.0 if think["callHelp_ReplyBehaviorType"] >= 0 and think["callHelp_CallValidRange"] >= 0 else 0.0
    ex[16] = float(think.get("callHelp_MinWaitTime", 0))           # callHelp_DelayTime (no own field)
    return ex


def main():
    game, cid, map_id, out_dir = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    lod = LODS[int(sys.argv[5]) if len(sys.argv) > 5 else 1]
    tex_max = int(sys.argv[6]) if len(sys.argv) > 6 else 128
    chr_dir = os.path.join(game, "chr")
    P = souls.game_params(game)
    msb = souls.read(f"{game}/map/MapStudio/{map_id}.msb")
    pairs = []
    for model, npc_id, think_id, *_ in enemy_parts(msb, map_id):
        if model == cid and (npc_id, think_id) not in pairs:
            pairs.append((npc_id, think_id))
    if not pairs:
        raise SystemExit(f"{cid}: no placement in {map_id}")
    variants = []
    for npc_id, think_id in pairs:
        npc = P["NpcParam"][npc_id][1]
        tid = think_id if think_id in P["NpcThinkParam"] else npc["aiThinkId"]
        variants.append((npc_id, tid, npc, P["NpcThinkParam"][tid][1]))
    npc0 = variants[0][2]

    anibnd = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(f"{chr_dir}/{cid}.anibnd.dcx"))}
    skel = next(k for k in anibnd if k.lower() == "skeleton.hkx")
    names, parents, ref = ha.skeleton(anibnd[skel])
    sk_index = {n: i for i, n in enumerate(names)}
    chrbnd = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(f"{chr_dir}/{cid}.chrbnd.dcx"))}
    fb = chrbnd[f"{cid}.flver"]
    F = souls.flver(fb)
    tex = textures_of(game, cid, chrbnd, npc0.get("normalChangeTexChrId", 0))
    bw = bind_world(F["bones"])

    shown_nn = set()
    for _, _, npc, _ in variants:
        shown_nn |= {k for k in range(16) if npc.get(f"modelDispMask{k}", 0)}
    def build(sk_map):
        mb = ChrMeshBuilder(sk_map, tex_max)
        shown = 0
        for m in F["meshes"]:
            mat, bits = m["material"], 0
            if mat.startswith("#") and mat[3:4] == "#" and mat[1:3].isdigit():
                nn = int(mat[1:3])
                if nn not in shown_nn or nn > 15:
                    continue
                bits = 1 << (8 + nn)
            fs = [f for f in m["facesets"] if f[0] == lod] or [f for f in m["facesets"] if not f[0] & 0x80000000]
            if not fs:
                continue
            d = tex.get(m["diffuse"])
            col = (souls.dds_average(d) if d else None) or (160, 150, 130)
            mb.add(F, bw, m, fs[0], mb.texture(m["diffuse"], d), col, bits=bits,
                   spec=tex.get(m["diffuse"] + "_s"))  # metal / specular per vertex (export_model.spec_terms)
            shown += 1
        return mb, shown
    mb, shown = build(sk_index)
    beh, atk = P["BehaviorParam"], P["AtkParam_Npc"]
    dmy_by_ref = {}
    for r, pos, parent, attach in dummies(fb):
        dmy_by_ref.setdefault(r, (pos, parent, attach))

    def dmy_local(ref_id):
        """Dummy poly -> (skeleton bone, position in that bone's space)"""
        if ref_id not in dmy_by_ref:
            return 0, (0.0, 0.0, 0.0)
        pos, parent, attach = dmy_by_ref[ref_id]
        world = pos
        if 0 <= parent < len(bw):
            t, q = bw[parent]
            r = ha.qrot(q, pos)
            world = (t[0] + r[0], t[1] + r[1], t[2] + r[2])
        bone = attach if attach >= 0 else parent
        sk, fbone = mb.skeleton_bone(F, bone) if bone >= 0 else (0, -1)
        b = bw[fbone] if fbone >= 0 else ((0, 0, 0), (0, 0, 0, 1))
        return sk, to_local(b, world)

    # anim set: essentials + script constants (+ offset versions)
    goal_ids = {t["battleGoalID"] for _, _, _, t in variants} | {t["logicId"] for _, _, _, t in variants}
    want = sorted(set(ANIMS) | script_anims(game, map_id, goal_ids) |
                  {t["callHelp_CallActionId"] for _, _, _, t in variants if t["callHelp_CallActionId"] > 0})
    offsets = sorted({npc["animIdOffset"] for _, _, npc, _ in variants if npc["animIdOffset"] > 0})
    ids = []
    for aid in want:
        if f"a00_{aid:04d}.hkx" in anibnd:
            ids.append(("a00", aid, aid))
        for off in offsets:
            pre = f"a{off // 10000:02d}"
            if f"{pre}_{aid:04d}.hkx" in anibnd:
                ids.append((pre, aid, off + aid))
    if not any(i[2] == 0 for i in ids):  # characters without a00_0000 (c1000): their first anim as the idle
        first = sorted(k for k in anibnd if k.endswith(".hkx") and k.lower() != "skeleton.hkx")
        if first:
            ids.insert(0, (first[0][:3], int(first[0][4:8]), 0))
    ids = ids[:MAX_ANIMS]
    tae_name = next((k for k in anibnd if k.endswith(".tae")), None)
    tae = souls.tae(anibnd[tae_name]) if tae_name else {}

    # skeleton reduction (palette room): the bones carrying the most vertex weight plus hit-capsule bones; vertices of
    # the others ride on their nearest kept FLVER ancestor (cloth / feather bones: no cloth simulation here anyway)
    capsule_bones = set()
    for pre, aid, full in ids:
        for etype, s_, e_, p in tae.get(full, tae.get(aid, {})).get("events", []):
            if etype == 1:
                b = beh.get(200000000 + npc0["behaviorVariationId"] * 1000 + p[2])
                a = atk.get(b[1]["refId"]) if b and b[1]["refId"] >= 0 else None
                if a:
                    capsule_bones.add(dmy_local(a[1]["hit0_DmyPoly1"])[0])
                    capsule_bones.add(dmy_local(a[1]["hit0_DmyPoly2"] if a[1]["hit0_DmyPoly2"] > 0
                                                else a[1]["hit0_DmyPoly1"])[0])
    remap, fold = None, None
    full_bones = len(names)
    if len(names) > PALETTE:
        weight = {}
        for v in mb.verts:
            weight[v[3]] = weight.get(v[3], 0) + v[5] / 255
            weight[v[4]] = weight.get(v[4], 0) + 1 - v[5] / 255
        keep = set(capsule_bones) | {i for i, p in enumerate(parents) if p < 0}
        for b in sorted(weight, key=lambda b: -weight[b]):
            if len(keep) >= PALETTE:
                break
            keep.add(b)
        names, parents, ref, remap, fold = reduce_skeleton(names, parents, ref, keep)
        mb, shown = build({n: i for i, n in enumerate(names)})
        print(f"  skeleton {full_bones} -> {len(names)} bones (by vertex weight + capsule bones + roots)")
    anims, info, windows, roots = [], [], [], []
    ref_full = ha.skeleton(anibnd[skel])[2]
    for pre, aid, full in ids:
        key = f"{pre}_{aid:04d}.hkx"
        nf, fps, baked = bake(anibnd, pre, aid, ref_full)
        if fold:
            baked = [fold(pose) for pose in baked]
        anims.append((full, baked))
        root = souls.root_motion(anibnd[key])
        x0, _, z0, w0 = root[0] if root else (0, 0, 0, 0)
        first_root = len(roots)
        roots += [(x - x0, z - z0, w - w0) for x, _, z, w in root]
        events = tae.get(full, tae.get(aid, {})).get("events", [])
        length = max([e[2] for e in events] + [(nf - 1) / 30])
        first_win = len(windows)
        for etype, s, e, p in sorted(events, key=lambda e: e[1]):
            if etype == 0 and p[0] in FLAGS:
                windows.append((p[0], 0, 0, 0, 0, s, e, 0, 0, 0, (0, 0, 0), (0, 0, 0), 1.0))
            elif etype == 1:  # AttackBehavior: AttackType, AttackIndex, BehaviorJudgeID
                b = beh.get(200000000 + npc0["behaviorVariationId"] * 1000 + p[2])
                a = atk.get(b[1]["refId"]) if b and b[1]["refId"] >= 0 else None
                if not a:
                    continue
                arow, a = b[1]["refId"], a[1]
                ba, pa = dmy_local(a["hit0_DmyPoly1"])
                bb, pb = dmy_local(a["hit0_DmyPoly2"] if a["hit0_DmyPoly2"] > 0 else a["hit0_DmyPoly1"])
                if remap:
                    ba, bb = remap.get(ba, 0), remap.get(bb, 0)
                windows.append((-1, ba, bb, a["dmgLevel"], arow, s, e, a["atkPhys"], a["hit0_Radius"],
                                a["atkSuperArmor"], pa, pb, a["atkPhysCorrection"] / 100 or 1.0))
        info.append((full, length, first_win, len(windows) - first_win, first_root, len(roots) - first_root))

    os.makedirs(out_dir, exist_ok=True)
    write_model(os.path.join(out_dir, "model.bin"), names, parents, ref, mb, [])
    append_compact_anims(os.path.join(out_dir, "model.bin"), len(names), anims)
    HDR, VAR = 48, 216
    var_off = HDR
    anim_off = var_off + VAR * len(variants)
    win_off = anim_off + 24 * len(info)
    root_off = win_off + 60 * len(windows)
    hide_bits = 0
    var_blob = b""
    for npc_id, tid, npc, t in variants:
        show = sum(1 << (8 + k) for k in range(16) if npc.get(f"modelDispMask{k}", 0))
        hide_bits |= show
        v = struct.pack("<6iI5f11f4i2fi", npc_id, tid, npc["hp"], t["logicId"], t["battleGoalID"], npc["animIdOffset"],
                        show, npc["hitRadius"], npc["hitHeight"], npc["turnVellocity"], npc["def_phys"],
                        npc["superArmorDurability"], t["eye_dist"], t["eye_angX"], t["eye_angY"], t["ear_dist"],
                        t["ear_soundcut_dist"], t["nearDist"], t["midDist"], t["farDist"], t["outDist"],
                        t["SightTargetForgetTime"] / FORGET_FPS, t["SoundTargetForgetTime"] / FORGET_FPS,
                        t["goalAction_ToCaution"], t["goalID_ToCaution"], t["goalAction_ToFind"], t["goalID_ToFind"],
                        t["callHelp_CallValidRange"], t["callHelp_CallValidMinDistTarget"], npc["teamType"])
        v += struct.pack("<24f", *think_values(t, npc))
        assert len(v) == VAR, len(v)
        var_blob += v
    upper = 0
    spine = next((i for i, n in enumerate(names) if "spine" in n.lower()), -1)
    for i in range(len(names)):
        j = i
        while j >= 0 and j != spine:
            j = parents[j]
        if spine >= 0 and j == spine:
            upper |= 1 << i
    hdr = struct.pack("<4s11I", b"DSRC", 5, len(info), len(windows), len(variants), anim_off, win_off, root_off,
                      var_off, hide_bits, upper & 0xFFFFFFFF, upper >> 32)
    assert len(hdr) == HDR
    body = var_blob + b"".join(struct.pack("<if4I", *i) for i in info)
    body += b"".join(struct.pack("<4hi5f6ff", t, ba, bb, lvl, arow, s, e, dmg, rad, poise, *pa, *pb, rate)
                     for t, ba, bb, lvl, arow, s, e, dmg, rad, poise, pa, pb, rate in windows)
    body += b"".join(struct.pack("<3f", *r) for r in roots)
    with open(os.path.join(out_dir, "chr.bin"), "wb") as f:
        f.write(hdr + body)
    print(f"{cid}: {len(variants)} variants {[(n, t) for n, t, _, _ in variants]}, {shown} meshes, {len(mb.tris)} tris, "
          f"{len(names)} bones, {len(anims)} anims {[i[2] for i in ids]}, {len(windows)} windows")


if __name__ == "__main__":
    main()
