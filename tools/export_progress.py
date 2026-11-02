"""Export the new-game progression state (ps2/proto/src/progress.c) into progress.gd (tools/gamedata.py; packed into
c0000.pak, layout in the committed src/progress_data.h).
Source: CharaInitParam row of the starting class (default 3000 = Warrior): souls, soul level, the eight stats, humanity,
equipment ids (EquipParamWeapon / Protector / Accessory, -1 = empty) and the start items (EquipParamGoods) with counts.
Usage: export_progress.py <game dir> <out.gd> [class row]"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedata, params
from gamedata import INT, U8, Ptr, Struct

# Progress.equip[] slot order (progress.h EQ_*) -> CharaInitParam field
EQUIP = ["equip_Wep_Left", "equip_Wep_Right", "equip_Subwep_Left", "equip_Subwep_Right", "equip_Arrow", "equip_Bolt",
         "equip_SubArrow", "equip_SubBolt", "equip_Helm", "equip_Armer", "equip_Gaunt", "equip_Leg",
         "equip_Accessory01", "equip_Accessory02"]
# Status screen order VIT ATT END STR DEX RES INT FAI (CharaInitParam: Wil = attunement, Luc = resistance, Mag = int)
STATS = ["baseVit", "baseWil", "baseEnd", "baseStr", "baseDex", "baseLuc", "baseMag", "baseFai"]


ITEM = Struct("StartItem", [("id", INT), ("count", INT)], "EquipParamGoods id, count")
RECORD = Struct("ProgressData", [
    ("start_class", INT), ("start_souls", INT), ("start_level", INT), ("start_humanity", INT),
    ("start_stats", U8, 8), ("start_equip", INT, len(EQUIP)), ("start_items", Ptr(ITEM)), ("nitems", INT)],
    "CharaInitParam row of the starting class")


def HEADER():
    return gamedata.header("progress", [ITEM, RECORD], RECORD, ([], [
        "#define START_CLASS (progress_data->start_class)",
        "#define START_SOULS (progress_data->start_souls)",
        "#define START_LEVEL (progress_data->start_level)",
        "#define START_HUMANITY (progress_data->start_humanity)",
        "#define start_stats (progress_data->start_stats)  /* VIT ATT END STR DEX RES INT FAI */",
        "#define start_equip (progress_data->start_equip)  /* progress.h EQ_* slot order */",
        "#define START_ITEMS (progress_data->nitems)",
        "#define start_items (progress_data->start_items)"]))


def main():
    game, out = sys.argv[1], sys.argv[2]
    row = int(sys.argv[3]) if len(sys.argv) > 3 else 3000
    r = params.params("CharaInitParam", game)[row]
    items = [{"id": r[f"item_{i:02d}"], "count": r[f"itemNum_{i:02d}"]} for i in range(1, 11) if r[f"item_{i:02d}"] >= 0]
    n = gamedata.write(out, "progress", RECORD, {
        "start_class": row, "start_souls": r["soul"], "start_level": r["soulLv"], "start_humanity": r["baseHeroPoint"],
        "start_stats": [r[s] for s in STATS], "start_equip": [r[f] for f in EQUIP], "start_items": items,
        "nitems": len(items)})
    print(f"{out}: class {row} ({r.name}), {len(items)} start items, {n} B")


if __name__ == "__main__":
    main()
