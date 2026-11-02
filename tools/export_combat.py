"""Export what the HP / damage core (ps2/proto/src/combat.c) needs into combat.gd (tools/gamedata.py; packed into
c0000.pak, layout in the committed src/combat_data.h):
Warrior stats and equip weight, Longsword attack values, CalcCorrectGraph rows (stat scaling, max HP), the c0000
skeleton parents and the R_Weapon bone, the Longsword's hit dummy polys in R_Weapon space, every AttackBehavior
window of the exported R1 attacks with its AtkParam_Pc row, and the debug dummy's NpcParam row.
Usage: export_combat.py <game dir> <out.gd>"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedata, souls, havok_anim
from gamedata import FLOAT, INT, S8, S16, Ptr, Struct

WARRIOR = 3000
ATTACK_ANIMS = [("a23", 3000), ("a23", 3001), ("a23", 3002)]  # R1 chain (export_player.py ANIMS)
WEAPON_PART, WEAPON_BONE = "WP_A_0201", "R_Weapon"
DUMMY_NPC = 206000  # NpcParam: Depths ghoul (sword) - HP 203, def_phys 71, no poise: the debug dummy target
GRAPH_ROWS = (100, 150)  # CalcCorrectGraph rows used besides the weapon's correctType: 100 = max HP (FUN_1402df9a0),
# 150 = bonfire Estus refill by kindle points (回復スポットの最大補充数; FUN_1407a41b0, step lookup, src/bonfire.c)


SK_BONES_MAX = 256  # world[] in combat.c is sized by this; the loader refuses a skeleton with more bones

DUMMY = Struct("DummyPoly", [("id", S16), ("p", FLOAT, 3)], "weapon dummy polys in R_Weapon bone space")
HIT = Struct("AtkHit", [("r", FLOAT), ("dp1", S16), ("dp2", S16)],
             "radius, dummy poly 1, dummy poly 2; a sphere when both are the same")
WINDOW = Struct("AtkWindow", [("anim", INT), ("start", FLOAT), ("end", FLOAT), ("atk_param", INT), ("phys_corr", INT),
                              ("sa_corr", INT), ("dmg_level", INT), ("nhit", INT), ("hit", HIT, 4)],
                "AttackBehavior window: anim id, start, end, AtkParam_Pc row, atkPhysCorrection, "
                "atkSuperArmorCorrection, dmgLevel, hit shapes")
RECORD = Struct("CombatData", [
    ("pl_vit", INT), ("pl_str", INT), ("pl_dex", INT), ("pl_end", INT), ("pl_equip_weight", FLOAT),
    ("wep_atk_phys", INT), ("wep_correct_str", FLOAT), ("wep_correct_dex", FLOAT), ("wep_req_str", INT),
    ("wep_req_dex", INT), ("wep_poise_dmg", INT), ("graph_weapon", FLOAT, 15)] +
    [(f"graph_{rid}", FLOAT, 15) for rid in GRAPH_ROWS] + [
    ("sk_bones", INT), ("sk_r_weapon", INT), ("sk_parent", Ptr(S8)), ("wep_dummy", Ptr(DUMMY)), ("wep_dummies", INT),
    ("atk_windows", Ptr(WINDOW)), ("natk_windows", INT), ("dummy_hp", INT), ("dummy_def_phys", INT),
    ("dummy_radius", FLOAT), ("dummy_height", FLOAT), ("dummy_poise", INT)],
    "Warrior stats + start equip weight, Longsword values, CalcCorrectGraph rows (stageMaxVal[5], stageMaxGrowVal[5], "
    "adjPt_maxGrowVal[5]; graph_weapon = the weapon's correctType), c0000 skeleton, debug dummy (NpcParam "
    f"{DUMMY_NPC})")


def HEADER():
    names = ["PL_VIT", "PL_STR", "PL_DEX", "PL_END", "PL_EQUIP_WEIGHT", "WEP_ATK_PHYS", "WEP_CORRECT_STR",
             "WEP_CORRECT_DEX", "WEP_REQ_STR", "WEP_REQ_DEX", "WEP_POISE_DMG", "SK_BONES", "SK_R_WEAPON", "WEP_DUMMIES",
             "DUMMY_HP", "DUMMY_DEF_PHYS", "DUMMY_RADIUS", "DUMMY_HEIGHT", "DUMMY_POISE"]
    macros = [f"#define {m} (combat_data->{m.lower()})" for m in names]
    macros += ["#define ATK_WINDOWS (combat_data->natk_windows)"]
    macros += [f"#define {a} (combat_data->{a})" for a in
               ["graph_weapon"] + [f"graph_{rid}" for rid in GRAPH_ROWS] + ["sk_parent", "wep_dummy", "atk_windows"]]
    return gamedata.header("combat", [DUMMY, HIT, WINDOW, RECORD], RECORD,
                           ([f"#define SK_BONES_MAX {SK_BONES_MAX}  /* cap for arrays sized by the skeleton */"], macros))


def graph_row(P, rid):
    r = P["CalcCorrectGraph"][rid][1]
    return ([r[f"stageMaxVal{i}"] for i in range(5)] + [r[f"stageMaxGrowVal{i}"] for i in range(5)] +
            [r[f"adjPt_maxGrowVal{i}"] for i in range(5)])


def main():
    game, out = sys.argv[1], sys.argv[2]
    P = souls.game_params(game)
    init = P["CharaInitParam"][WARRIOR][1]
    wep_id = init["equip_Wep_Right"]
    wep = P["EquipParamWeapon"][wep_id][1]
    # equip weight: weapons from EquipParamWeapon, armour from EquipParamProtector (ids overlap between tables)
    weight = sum(P["EquipParamWeapon"][init[k]][1]["weight"] for k in ("equip_Wep_Right", "equip_Wep_Left")
                 if init[k] in P["EquipParamWeapon"])
    weight += sum(P["EquipParamProtector"][init[k]][1]["weight"] for k in ("equip_Helm", "equip_Armer", "equip_Gaunt",
                                                                        "equip_Leg") if init[k] in P["EquipParamProtector"])
    chr_dir = os.path.join(game, "chr")
    anibnd = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(f"{chr_dir}/c0000.anibnd.dcx"))}
    names, parents, _ = havok_anim.skeleton(anibnd["Skeleton.hkx"])
    wbone = names.index(WEAPON_BONE)
    taes = {souls.basename(n)[:-4]: souls.tae(d) for n, d in ((n, d) for _, n, d in
                                                                souls.bnd3(souls.read(f"{chr_dir}/c0000.anibnd.dcx")))
            if n.endswith(".tae")}

    # weapon dummy polys: FLVER dummies (0x40 bytes after the header): position, ..., refID s16 @0x1C,
    # parent bone s16 @0x1E. The Longsword's hit dummies hang off its root bone (identity bind). The weapon mesh is
    # placed on R_Weapon flipped 180 deg about Z (export_model.py), so the dummies get the same (-x, -y, z).
    files = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(f"{game}/parts/{WEAPON_PART}.partsbnd.dcx"))}
    fb = files[f"{WEAPON_PART}.flver"]
    F = souls.flver(fb)
    n_dummy = struct.unpack_from("<i", fb, 0x14)[0]
    dummies = {}
    for i in range(n_dummy):
        o = 0x80 + 0x40 * i
        x, y, z = struct.unpack_from("<3f", fb, o)
        ref, parent = struct.unpack_from("<hh", fb, o + 0x1C)
        t, q, _ = F["bones"][parent][2:5] if parent >= 0 else ((0, 0, 0), (0, 0, 0, 1), None)
        assert t == (0, 0, 0) or max(abs(c) for c in t) < 1e-6, f"dummy {ref} parent bone not at the origin"
        dummies.setdefault(ref, (-x, -y, z))

    beh, atk = P["BehaviorParam_PC"], P["AtkParam_Pc"]
    attacks = []
    for tfile, aid in ATTACK_ANIMS:
        for etype, s, e, p in sorted(taes[tfile][aid]["events"], key=lambda ev: ev[1]):
            if etype != 1:  # AttackBehavior: AttackType, AttackIndex, BehaviorJudgeID
                continue
            b = beh[100000000 + wep["behaviorVariationId"] * 1000 + p[2]][1]
            a = atk[b["refId"]][1]
            hits = []
            for h in range(4):
                r, d1, d2 = a[f"hit{h}_Radius"], a[f"hit{h}_DmyPoly1"], a[f"hit{h}_DmyPoly2"]
                if r > 0 and d1 >= 0:
                    hits.append((r, d1, d2 if d2 >= 0 else d1))
            attacks.append((aid, s, e, b["refId"], a["atkPhysCorrection"], a["atkSuperArmorCorrection"],
                            a["dmgLevel"], hits))

    npc = P["NpcParam"][DUMMY_NPC][1]
    used_dp = sorted({d for at in attacks for _, d1, d2 in at[7] for d in (d1, d2)})
    assert len(parents) <= SK_BONES_MAX, f"{len(parents)} bones > SK_BONES_MAX"
    r3, r4 = (lambda v: round(v, 3)), (lambda v: round(v, 4))  # as the old generated header printed them
    windows = [{"anim": aid, "start": r3(s), "end": r3(e), "atk_param": row, "phys_corr": pc, "sa_corr": sac,
                "dmg_level": lvl, "nhit": len(hits),
                "hit": [{"r": r3(r), "dp1": d1, "dp2": d2} for r, d1, d2 in hits] +
                       [{"r": 0, "dp1": 0, "dp2": 0}] * (4 - len(hits))}
               for aid, s, e, row, pc, sac, lvl, hits in attacks]
    g6 = lambda v: float(f"{float(v):.6g}")  # noqa: E731
    value = {
        "pl_vit": init["baseVit"], "pl_str": init["baseStr"], "pl_dex": init["baseDex"], "pl_end": init["baseEnd"],
        "pl_equip_weight": round(weight, 2), "wep_atk_phys": wep["attackBasePhysics"],
        "wep_correct_str": round(wep["correctStrength"], 1), "wep_correct_dex": round(wep["correctAgility"], 1),
        "wep_req_str": wep.get("properStrength", 0), "wep_req_dex": wep.get("properAgility", 0),
        "wep_poise_dmg": wep["saWeaponDamage"], "graph_weapon": [g6(v) for v in graph_row(P, wep["correctType"])],
        "sk_bones": len(parents), "sk_r_weapon": wbone, "sk_parent": list(parents),
        "wep_dummy": [{"id": d, "p": [r4(c) for c in dummies[d]]} for d in used_dp] or [{"id": -1, "p": [0, 0, 0]}],
        "wep_dummies": len(used_dp), "atk_windows": windows, "natk_windows": len(windows),
        "dummy_hp": npc["hp"], "dummy_def_phys": npc["def_phys"], "dummy_radius": r3(npc["hitRadius"]),
        "dummy_height": r3(npc["hitHeight"]), "dummy_poise": npc["superArmorDurability"]}
    for rid in GRAPH_ROWS:
        value[f"graph_{rid}"] = [g6(v) for v in graph_row(P, rid)]
    n = gamedata.write(out, "combat", RECORD, value)
    print(f"{out}: weight {weight:.1f}, {len(attacks)} attack windows, dummies {used_dp}, R_Weapon bone {wbone}, {n} B")


if __name__ == "__main__":
    main()
