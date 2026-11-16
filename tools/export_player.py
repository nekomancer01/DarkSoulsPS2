"""Export the player data the PS2 character prototype uses into player.gd (tools/gamedata.py; packed into c0000.pak,
layout in the committed src/player_data.h, generated from ANIMS / FLAGS / RECORD below without the game).
Sources (all read from the installed game): c0000 TAE timelines (a00 movement, a23 straight sword), Havok root motion,
BehaviorParam_PC stamina costs, CharaInitParam (Warrior start), EquipParamWeapon (Longsword).
Usage: export_player.py <game dir> <out.gd>"""
import math, struct, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedata, souls
from gamedata import CHAR, FLOAT, INT, S16, Ptr, Struct

# (C name, TAE file, anim id, loop) — a00 = shared movement, a23 = straight-sword attacks (behaviorVariationId 2300 / 100)
# Movement sets by equip load (MoveParam rows: light, medium, heavy, overloaded; f110, src/player.c move_set)
MOVE_SETS = (0, 5, 10, 15)

ANIMS = [
    ("WALK", "a00", 200, True),
    ("RUN", "a00", 500, True),
    ("DASH", "a00", 535, True),
    ("ROLL", "a00", 700, False),
    ("R1_1", "a23", 3000, False),
    ("R1_2", "a23", 3001, False),
    ("R1_3", "a23", 3002, False),
    ("BACKSTEP", "a00", 680, False),  # c0000.esd group 1 state 37: c1_3(2, 680, ...)
    # Running jump: ESD group 1 state 36 plays f110(5) = MoveParam row 0 dashEscapeF (+0x78, FUN_14052dea0 case 5)
    ("JUMP", "a00", 900, False),
    # Fall death: ESD group 0 states 57-59 (HP < 0 while falling): 6065 start, 6066 loop, 6067 impact
    ("FALL_DEATH", "a00", 6065, False),
    ("FALL_DEATH_LOOP", "a00", 6066, True),
    ("FALL_DEATH_LAND", "a00", 6067, False),
    # The ESD's own fall (group 0): state 14 fall start 1550, state 15 fall loop 1500; landings by fall height f224
    # and equip load f263: states 31 / 32 / 33 = 1750 / 1755 / 1760 (c1_12; 34 = 1790 and 35 = 1770 not exported); lethal landing
    # state 97 (f224 >= 20 or HP < 0) = 6070 -> 98 = 6170, state 99 (HP < 0, f224 < 10) = 6073 -> 100 = 6173
    ("ESD_FALL_START", "a00", 1550, False),
    ("ESD_FALL_LOOP", "a00", 1500, True),
    ("ESD_LAND_1750", "a00", 1750, False),
    ("ESD_LAND_1755", "a00", 1755, False),
    ("ESD_LAND_1760", "a00", 1760, False),
    ("ESD_FALL_IMPACT", "a00", 6070, False),
    ("ESD_FALL_IMPACT_END", "a00", 6170, False),
    ("ESD_LAND_DEATH", "a00", 6073, False),
    ("ESD_LAND_DEATH_END", "a00", 6173, False),
    # Running (dash) attack, ESD group 1 state 59: c1_3(1, 3500); rolling attack, state 41: c1_3(1, 3900)
    ("R1_DASH", "a23", 3500, False),
    ("R1_ROLL", "a23", 3900, False),
    # Plunging attack: ESD group 0 state 15 (fall loop) + R1 -> state 19 / 20 (a23 3800 fall, 3801 impact)
    ("PLUNGE", "a23", 3800, False),
    ("PLUNGE_LAND", "a23", 3801, False),
    # Hit reactions (ESD group 0, f236 damage level + f222 direction): level 1 state 43 = 2000 + dir, level 2 state 45
    # = 2004 + dir (other levels: 2010-2087, not exported yet)
    ("DAMAGE_2000", "a00", 2000, False),
    ("DAMAGE_2001", "a00", 2001, False),
    ("DAMAGE_2002", "a00", 2002, False),
    ("DAMAGE_2003", "a00", 2003, False),
    ("DAMAGE_2004", "a00", 2004, False),
    ("DAMAGE_2005", "a00", 2005, False),
    ("DAMAGE_2006", "a00", 2006, False),
    ("DAMAGE_2007", "a00", 2007, False),
    # Guard (ESD group 1 states 75/76/78/79: c1_4(2, 100/101/106/116 + f278 guardmotionCategory)); reached when a
    # request is left unconsumed, e.g. R1 at 0 stamina
    ("GUARD", "a00", 100, False),
    ("GUARD_END", "a00", 101, False),
    ("GUARD_MOVE", "a00", 106, False),
    ("GUARD_MOVE_END", "a00", 116, False),
    # The rest of the guard set for guardmotionCategory 0 (Heater Shield; group 1 states 75-85, upper-body layer 4):
    # hold loop 1 (c1_13, state 77), guard walk / run / dash loops 210 / 520 / 515 (states 80-82), guarded-hit
    # reactions 140 / 141 / 142 (states 83 / 84 / 85 by f237 class 1 / 3 / 4 = guard break)
    ("GUARD_HOLD", "a00", 1, True),
    ("GUARD_WALK", "a00", 210, True),
    ("GUARD_RUN", "a00", 520, True),
    ("GUARD_DASH", "a00", 515, True),
    ("GUARD_HIT_S", "a00", 140, False),
    ("GUARD_HIT_L", "a00", 141, False),
    ("GUARD_BREAK", "a00", 142, False),
    # R2 (slot 5): group 1 state 89 = a23 3300, the R2 after an R1 / R2 = state 88 a23 3301, state 90 = 3310
    ("R2_1", "a23", 3300, False),
    ("R2_2", "a23", 3301, False),
    ("R2_3", "a23", 3310, False),
    # Kick (stick forward + R1, group 1 state 52: a23 3100), jumping attack (forward + R2, state 53: a23 3600), plunge
    # landing (group 0 states 21-23: a23 3810), shield parry (L2, state 86: c1_3(0, 5100 + f225(2)) on the left hand's
    # weapon motion a48; its AttackBehavior of AttackType 64 "Parry" at 0-0.23 s is the parry window)
    ("KICK", "a23", 3100, False),
    ("JUMP_ATK", "a23", 3600, False),
    ("PLUNGE_LAND_ATK", "a23", 3810, False),
    ("PARRY", "a48", 5100, False),
    # Throws (ThrowParam atkAnimId on the right weapon's motion, played by the engine, not an ESD state): backstab
    # a23 3400, riposte a23 3201
    ("BACKSTAB", "a23", 3400, False),
    # Lock-on (PR D): rolls back / left / right (group 1 states 33 / 35 / 34 via f110(1 / 2 / 3)) of every movement
    # set, the other sets' forward rolls and steps, and the strafe loops (MoveParam row 0 walkB/L/R, dashB/L/R)
    ("ROLL_B", "a00", 701, False), ("ROLL_L", "a00", 702, False), ("ROLL_R", "a00", 703, False),
    ("ROLL_M_F", "a00", 710, False), ("ROLL_M_B", "a00", 711, False), ("ROLL_M_L", "a00", 712, False),
    ("ROLL_M_R", "a00", 713, False), ("ROLL_H_F", "a00", 720, False), ("ROLL_H_B", "a00", 721, False),
    ("ROLL_H_L", "a00", 722, False), ("ROLL_H_R", "a00", 723, False), ("ROLL_O_F", "a00", 730, False),
    ("ROLL_O_B", "a00", 731, False), ("ROLL_O_L", "a00", 732, False), ("ROLL_O_R", "a00", 733, False),
    ("STEP_M", "a00", 690, False), ("STEP_H", "a00", 695, False), ("STEP_O", "a00", 696, False),
    ("WALK_B", "a00", 201, True), ("WALK_L", "a00", 202, True), ("WALK_R", "a00", 203, True),
    ("RUN_B", "a00", 501, True), ("RUN_L", "a00", 502, True), ("RUN_R", "a00", 503, True),
    ("RIPOSTE", "a23", 3201, False),
    # Played by EMEVD 2003[18] Force Animation Playback on the player (src/event.c -> player_event_anim): 7410 = the
    # walk through a boss fog wall (m10_01 events 11015370 / 11015380 / 11015390, Taurus / Capra / Gargoyles)
    ("EVENT_FOG_WALK", "a00", 7410, False),
    # Bonfire (src/bonfire.c), played by eventCommon global_event.lua: Lua_BonfireFirstInjectAnim 7698 (light; TAE 231
    # EzState message 30 at 1.4 s -> OnEvent_BonfireFirstLvUp), Lua_BonfireInjectAnim 7699 (kindle; message 10 at
    # 3.667 s -> OnEvent_BonfireLvUp), Lua_BonfireLoopAnimBegin 7700 + 10 x GetRandom(0, 2) (sit; message 20 at
    # 0.667 s -> OnEvent_BonfireRespawn), then its loop (+1) and on leaving its stand-up (+2)
    ("BONFIRE_LIGHT", "a00", 7698, False),
    ("BONFIRE_KINDLE", "a00", 7699, False),
    ("BONFIRE_SIT_0", "a00", 7700, False),
    ("BONFIRE_LOOP_0", "a00", 7701, True),
    ("BONFIRE_STAND_0", "a00", 7702, False),
    ("BONFIRE_SIT_1", "a00", 7710, False),
    ("BONFIRE_LOOP_1", "a00", 7711, True),
    ("BONFIRE_STAND_1", "a00", 7712, False),
    ("BONFIRE_SIT_2", "a00", 7720, False),
    ("BONFIRE_LOOP_2", "a00", 7721, True),
    ("BONFIRE_STAND_2", "a00", 7722, False),
    # Item pickup (src/pickup.c: treasures, enemy drops): ObjActParam row 0 playerAnimId 8000 (the generic ObjAct; the
    # treasure's own pickup anim is not found in the exe, ASSUMED the same, TECH_DEBT item-2)
    ("PICKUP", "a00", 8000, False),
]
# Played by the event Lua, which goes on at OnChrAnimEnd: the Havok clip's end, not the TAE's last event (their sound
# events run past it, e.g. 7700: clip 66 frames = 2.17 s, TAE to 5.2 s)
CLIP_LENGTH = {7698, 7699, 7700, 7701, 7702, 7710, 7711, 7712, 7720, 7721, 7722}
# ChrActionFlag (TAE event 0) values the controller reads; names from DSAnimStudio's TAE.Template.DS1.xml
FLAGS = {4: "RH_ATTACK_CANCEL", 7: "DISABLE_TURNING", 8: "INVINCIBLE", 11: "MOVE_CANCEL", 26: "STEP_CANCEL",
         27: "IGNORE_GRAVITY", 32: "GENERIC_CANCEL", 51: "DISABLE_FALL_MOTION"}
# Value ramps over an event window (Grad): TAE 300 ChrActionGradient with output 27 = gravity multiplier
# (FUN_1403410f0 -> ctx+0x448 -> proxy+0x1E4), TAE 236 RootMotionReduction (FUN_140343660 -> ctx+0x198)
GRAD_GRAVITY, GRAD_ROOT_REDUCTION = 27, 236
EZ_MESSAGE = -300  # Window type of a TAE 231 EzState message n: EZ_MESSAGE - n (SP regen windows use -2 .. -257)
WARRIOR, LONGSWORD = 3000, 201000


WINDOW = Struct("Window", [("type", S16), ("start", FLOAT), ("end", FLOAT)],
                "type: ChrActionFlag id, or -1 hit, -2 - sp regen %")
ROOT = Struct("RootSample", [("x", FLOAT), ("z", FLOAT), ("yaw", FLOAT)], "30 fps; forward is -z")
GRAD = Struct("Grad", [("type", S16), ("start", FLOAT), ("end", FLOAT), ("v0", FLOAT), ("v1", FLOAT)],
              "value ramp from v0 at start to v1 at end: type 27 gravity multiplier, 236 root-motion reduction")
ANIM = Struct("AnimData", [("name", Ptr(CHAR)), ("id", INT), ("key", INT), ("length", FLOAT), ("loop", INT),
                           ("speed", FLOAT), ("stamina", INT), ("win", Ptr(WINDOW)), ("nwin", INT), ("root", Ptr(ROOT)),
                           ("nroot", INT), ("grad", Ptr(GRAD)), ("ngrad", INT)],
              "key: ESD anim id as resolved by c1_3: a00 ids as they are, weapon-motion files (aNN) NN * 10000 + id")
RECORD = Struct("PlayerData", [
    ("start_end", INT), ("weapon_id", INT), ("weapon_weight", FLOAT), ("weapon_atk_phys", INT),
    ("move_escape", S16, 24), ("weapon_motion_r", INT), ("weapon_motion_l", INT), ("guard_motion_r", INT),
    ("guard_motion_l", INT), ("anims", ANIM, len(ANIMS))],
    f"Warrior (CharaInitParam {WARRIOR}) with the Longsword; move_escape = f110(n) per movement set (equip load: MoveParam rows 0 / 5 / 10 / 15, 6 each): escapeF/B/L/R, "
    "stepMove, dashEscapeF")


def HEADER():
    consts = (["/* ChrActionFlag ids (TAE event 0) */"] + [f"#define FLAG_{name} {fid}" for fid, name in FLAGS.items()] +
              [f"#define EZ_MESSAGE ({EZ_MESSAGE})  /* Window type of TAE 231 EzState message n: EZ_MESSAGE - n */"] +
              ["", "enum { " + ", ".join(f"ANIM_{c}" for c, *_ in ANIMS) + ", ANIM_COUNT };"])
    names = [("START_END", "start_end", "Warrior END"), ("WEAPON_ID", "weapon_id", "EquipParamWeapon: Longsword"),
             ("WEAPON_WEIGHT", "weapon_weight", None), ("WEAPON_ATK_PHYS", "weapon_atk_phys", None),
             ("WEAPON_MOTION_R", "weapon_motion_r", "EquipParamWeapon.wepmotionCategory, right hand"),
             ("WEAPON_MOTION_L", "weapon_motion_l", "left hand (Kite Shield)"),
             ("GUARD_MOTION_R", "guard_motion_r", "EquipParamWeapon.guardmotionCategory (ESD f278)"),
             ("GUARD_MOTION_L", "guard_motion_l", None), ("move_escape", "move_escape", None)]
    macros = [f"#define {m} (player_data->{f}){f'  /* {c} */' if c else ''}" for m, f, c in names]
    macros.append("/* player.c reads player_data->anims[] (a bare `anims` macro would clash with model.h's field) */")
    return gamedata.header("player", [WINDOW, ROOT, GRAD, ANIM, RECORD], RECORD, (consts, macros))


def clip_length(hkx):
    """Duration of a Havok clip (hkaSplineCompressedAnimation numFrames - 1 frames of frameDuration), s"""
    h = souls.HkTag(hkx)
    A = h.type("hkaSplineCompressedAnimation")
    o = h.objects("hkaSplineCompressedAnimation")[0]
    return (h.get(o, A, "numFrames", "i") - 1) * h.get(o, A, "frameDuration", "f")


def main():
    game, out = sys.argv[1], sys.argv[2]
    chr_dir = os.path.join(game, "chr")
    taes = {souls.basename(n)[:-4]: souls.tae(d) for _, n, d in souls.bnd3(souls.read(f"{chr_dir}/c0000.anibnd.dcx"))
            if n.endswith(".tae")}
    hkx = {}
    for f in ("c0000_a00_lo.anibnd.dcx", "c0000_a00_md.anibnd.dcx", "c0000_a2x.anibnd.dcx", "c0000_a4x.anibnd.dcx"):
        for _, n, d in souls.bnd3(souls.read(f"{chr_dir}/{f}")):
            hkx[souls.basename(n)[:-4]] = d
    P = souls.game_params(game)
    init = P["CharaInitParam"][WARRIOR][1]
    wep = P["EquipParamWeapon"][init["equip_Wep_Right"]][1]
    variation = wep["behaviorVariationId"]
    beh = P["BehaviorParam_PC"]

    f32 = lambda u: struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]
    anims = []
    for cname, tfile, aid, loop in ANIMS:
        events = taes[tfile][aid]["events"]
        root = souls.root_motion(hkx[f"{tfile}_{aid:04d}"])
        length = max([e[2] for e in events] + [(len(root) - 1) / 30])
        if aid in CLIP_LENGTH:
            length = clip_length(hkx[f"{tfile}_{aid:04d}"])
        wins, stamina, grads = [], 0, []
        for etype, s, e, p in sorted(events, key=lambda e: e[1]):
            if etype == 0 and 0 < p[0] < 128:  # every ChrActionFlag window (the ESD request model reads ~40 ids)
                wins.append((p[0], s, e))
            elif etype == 1:  # AttackBehavior: AttackType, AttackIndex, BehaviorJudgeID
                wins.append((-1, s, e))
                row = beh.get(100000000 + variation * 1000 + p[2])
                stamina = max(stamina, row[1]["stamina"] if row else 0)
            elif etype == 225:  # SetSPRegenRatePercent
                wins.append((-2 - (p[0] & 0xFF), s, e))
            elif etype == 231:  # EzState message to the event Lua (OnCheckEzStateMessage: bonfire 10 / 20 / 30)
                wins.append((EZ_MESSAGE - (p[0] & 0xFF), s, e))
            elif etype == 300 and p[0] & 0xFFFF == GRAD_GRAVITY:  # s16 output, s16 input (0), f32 from, f32 to
                grads.append((GRAD_GRAVITY, s, e, f32(p[1]), f32(p[2])))
            elif etype == 236 and p[2] & 0xFF == 0:  # f32 at start, f32 at end, u8 type 0 (scaled by mobility)
                grads.append((GRAD_ROOT_REDUCTION, s, e, f32(p[0]), f32(p[1])))
        x0, _, z0, w0 = root[0] if root else (0, 0, 0, 0)
        dist = math.hypot(root[-1][0] - x0, root[-1][2] - z0) if root else 0
        speed = dist / ((len(root) - 1) / 30) if loop and len(root) > 1 else 0
        key = aid if tfile == "a00" else int(tfile[1:]) * 10000 + aid
        # values rounded as the old generated header printed them (%.3f / %.4f), so the engine sees the same numbers
        r3, r4 = (lambda v: round(v, 3)), (lambda v: round(v, 4))
        anims.append({
            "name": cname, "id": aid, "key": key, "length": r3(length), "loop": int(loop), "speed": r3(speed),
            "stamina": stamina,
            "win": [{"type": t, "start": r3(s), "end": r3(e)} for t, s, e in wins] or [{"type": 0, "start": 0, "end": 0}],
            "nwin": len(wins),
            "root": [{"x": r4(x - x0), "z": r4(z - z0), "yaw": r4(w - w0)} for x, _, z, w in root]
                    or [{"x": 0, "z": 0, "yaw": 0}],
            "nroot": max(1, len(root)),
            "grad": [{"type": t, "start": r3(s), "end": r3(e), "v0": r3(a), "v1": r3(b)} for t, s, e, a, b in grads]
                    or [{"type": 0, "start": 0, "end": 0, "v0": 0, "v1": 0}],
            "ngrad": len(grads)})
    # ESD f110(n) (EzStateActionEnv case 0x6E -> FUN_14037aae0 -> FUN_14052dea0 case n): MoveParam fields of the
    # movement set (ChrCtrl+0x1C0[+0x210]); the set selection by equip load is not decoded, row 0 is used
    mp = P["MoveParam"][0][1]
    left = P["EquipParamWeapon"][init["equip_Wep_Left"]][1]
    n = gamedata.write(out, "player", RECORD, {
        "start_end": init["baseEnd"], "weapon_id": init["equip_Wep_Right"], "weapon_weight": round(wep["weight"], 2),
        "weapon_atk_phys": wep["attackBasePhysics"],
        "move_escape": [P["MoveParam"][row][1][k] for row in MOVE_SETS
                        for k in ("escapeF", "escapeB", "escapeL", "escapeR", "stepMove", "dashEscapeF")],
        "weapon_motion_r": wep["wepmotionCategory"], "weapon_motion_l": left["wepmotionCategory"],
        "guard_motion_r": wep["guardmotionCategory"], "guard_motion_l": left["guardmotionCategory"], "anims": anims})
    for a in anims:
        print(a["name"], a["id"], a["key"], a["length"], a["loop"], a["speed"])
    print(f"{out}: {len(anims)} anims, {n} B")


if __name__ == "__main__":
    main()
