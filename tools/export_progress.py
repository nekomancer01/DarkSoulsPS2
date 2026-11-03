"""Export the new-game progression state (ps2/proto/src/progress.c) into progress.gd (tools/gamedata.py; packed into
c0000.pak, layout in the committed src/progress_data.h).
Source: CharaInitParam row of the starting class (default 3000 = Warrior): souls, soul level, the eight stats, humanity,
equipment ids (EquipParamWeapon / Protector / Accessory, -1 = empty) and the start items (EquipParamGoods) with counts.
Humanity from kills (FUN_14032fb30, run by the death handler FUN_14032f900): a dying character's play region (MSB
collision PlayRegionID of the floor it stands on, ChrIns +0x354, set by FUN_140326b50) names a zone: area =
region / 10000 % 100 (10..18), block = region / 1000 % 10 (< 3), index = region / 100 % 10 (< 4). The exe's table
0x1413723a0 maps the zone to a GameAreaParam row (the zone's boss), 0x141352810 and 0x1413529c0 to event flags that end
the zone's counting once set (FUN_14048e3a0: the boss is dead). The row's humanityPointCountFlagIdTop is a 16-bit
point counter in the event flags, the 4 flags after it the drops so far; humanityDropPoint1..10 the points each drop
needs (FUN_14052b9e0 / FUN_14052ba70 / FUN_14052bb20). zones[] holds the rows the table names, key = (area - 10) * 12
+ block * 4 + index (tools/export_enemies.py stores key + 1 in each placement).
Usage: export_progress.py <game dir> <out.gd> [class row]"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedata, params
from gamedata import INT, S16, U8, Ptr, Struct

# Progress.equip[] slot order (progress.h EQ_*) -> CharaInitParam field
EQUIP = ["equip_Wep_Left", "equip_Wep_Right", "equip_Subwep_Left", "equip_Subwep_Right", "equip_Arrow", "equip_Bolt",
         "equip_SubArrow", "equip_SubBolt", "equip_Helm", "equip_Armer", "equip_Gaunt", "equip_Leg",
         "equip_Accessory01", "equip_Accessory02"]
# Status screen order VIT ATT END STR DEX RES INT FAI (CharaInitParam: Wil = attunement, Luc = resistance, Mag = int)
STATS = ["baseVit", "baseWil", "baseEnd", "baseStr", "baseDex", "baseLuc", "baseMag", "baseFai"]


# exe tables (DarkSoulsRemastered.exe .rdata), [area 10..18][block 0..2][index 0..3]; -1 none
ZONE_ROW = [  # 0x1413723a0: GameAreaParam row
    [[1000800, -1, -1, -1], [1010700, 1010750, 1010800, -1], [-1] * 4],
    [[1100160, -1, -1, -1], [-1] * 4, [-1] * 4],
    [[1200801, 1200800, -1, -1], [-1, 1210820, 1210840, -1], [-1] * 4],
    [[1300800, -1, -1, -1], [1310800, -1, -1, -1], [-1] * 4],
    [[1400800, -1, -1, -1], [1410600, 1410400, 1410700, 1410800], [-1] * 4],
    [[1500800, -1, -1, -1], [1510801, -1, -1, -1], [-1] * 4],
    [[1600800, -1, -1, -1], [-1] * 4, [-1] * 4],
    [[1700800, -1, -1, -1], [-1] * 4, [-1] * 4],
    [[1800800, -1, -1, -1], [1810800, -1, -1, -1], [-1] * 4]]
ZONE_END_FLAG = [  # 0x141352810: event flag (the zone boss defeated)
    [[2, -1, -1, -1], [11010901, 11010902, 3, -1], [-1] * 4],
    [[4, -1, -1, -1], [-1] * 4, [-1] * 4],
    [[11200900, 5, -1, -1], [11210000, 121, 17, 703], [-1] * 4],
    [[6, -1, -1, -1], [7, -1, -1, -1], [8, -1, -1, -1]],
    [[9, -1, -1, -1], [11410900, 11410410, 11410901, 10], [-1] * 4],
    [[11, -1, -1, -1], [120, -1, -1, -1], [-1] * 4],
    [[11600900, 13, -1, -1], [-1] * 4, [-1] * 4],
    [[14, -1, -1, -1], [-1] * 4, [-1] * 4],
    [[15, -1, -1, -1], [16, -1, -1, -1], [-1] * 4]]
ZONE_END_FLAG2 = [  # 0x1413529c0: a second event flag
    [[-1] * 4] * 3, [[-1] * 4] * 3, [[400] * 4, [-1] * 4, [-1] * 4], [[-1] * 4, [401] * 4, [-1] * 4],
    [[-1] * 4, [402] * 4, [-1] * 4], [[-1] * 4, [403] * 4, [-1] * 4], [[404] * 4, [-1] * 4, [-1] * 4],
    [[405] * 4, [-1] * 4, [-1] * 4], [[-1] * 4, [406] * 4, [-1] * 4]]


def zone_key(region):
    """MSB collision PlayRegionID -> zone key, or -1 (FUN_14048e3a0 / FUN_14052b8e0's split)"""
    if region < 0:
        return -1
    a, b, i = region // 10000 % 100, region // 1000 % 10, region // 100 % 10
    if not (10 <= a <= 18 and b < 3 and i < 4) or ZONE_ROW[a - 10][b][i] <= 0:
        return -1
    return (a - 10) * 12 + b * 4 + i


ITEM = Struct("StartItem", [("id", INT), ("count", INT)], "EquipParamGoods id, count")
ZONE = Struct("HumanityZone", [("key", INT), ("flag_top", INT), ("end_flag", INT, 2), ("drop", S16, 10)],
              "GameAreaParam humanity row of a zone: humanityPointCountFlagIdTop, the two end flags, "
              "humanityDropPoint1..10 (u16 stored as s16: 65535 = -1 none)")
RECORD = Struct("ProgressData", [
    ("start_class", INT), ("start_souls", INT), ("start_level", INT), ("start_humanity", INT),
    ("start_stats", U8, 8), ("start_equip", INT, len(EQUIP)), ("start_items", Ptr(ITEM)), ("nitems", INT),
    ("zones", Ptr(ZONE)), ("nzones", INT)],
    "CharaInitParam row of the starting class; humanity-from-kills zones")


def HEADER():
    return gamedata.header("progress", [ITEM, ZONE, RECORD], RECORD, ([], [
        "#define START_CLASS (progress_data->start_class)",
        "#define START_SOULS (progress_data->start_souls)",
        "#define START_LEVEL (progress_data->start_level)",
        "#define START_HUMANITY (progress_data->start_humanity)",
        "#define start_stats (progress_data->start_stats)  /* VIT ATT END STR DEX RES INT FAI */",
        "#define start_equip (progress_data->start_equip)  /* progress.h EQ_* slot order */",
        "#define START_ITEMS (progress_data->nitems)",
        "#define start_items (progress_data->start_items)",
        "#define HUM_ZONES (progress_data->nzones)",
        "#define hum_zones (progress_data->zones)"]))


def main():
    game, out = sys.argv[1], sys.argv[2]
    row = int(sys.argv[3]) if len(sys.argv) > 3 else 3000
    r = params.params("CharaInitParam", game)[row]
    items = [{"id": r[f"item_{i:02d}"], "count": r[f"itemNum_{i:02d}"]} for i in range(1, 11) if r[f"item_{i:02d}"] >= 0]
    area = params.params("GameAreaParam", game)
    zones = []
    for a in range(9):
        for b in range(3):
            for i in range(4):
                zr = ZONE_ROW[a][b][i]
                if zr <= 0 or zr not in area or area[zr]["humanityPointCountFlagIdTop"] < 0:
                    continue
                g = area[zr]
                zones.append({"key": a * 12 + b * 4 + i, "flag_top": g["humanityPointCountFlagIdTop"],
                              "end_flag": [ZONE_END_FLAG[a][b][i], ZONE_END_FLAG2[a][b][i]],
                              "drop": [g[f"humanityDropPoint{k}"] - (g[f"humanityDropPoint{k}"] > 32767) * 65536
                                       for k in range(1, 11)]})
    n = gamedata.write(out, "progress", RECORD, {
        "start_class": row, "start_souls": r["soul"], "start_level": r["soulLv"], "start_humanity": r["baseHeroPoint"],
        "start_stats": [r[s] for s in STATS], "start_equip": [r[f] for f in EQUIP], "start_items": items,
        "nitems": len(items), "zones": zones, "nzones": len(zones)})
    print(f"{out}: class {row} ({r.name}), {len(items)} start items, {len(zones)} humanity zones, {n} B")


if __name__ == "__main__":
    main()
