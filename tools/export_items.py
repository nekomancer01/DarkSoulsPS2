"""Export DSR's item database for the PS2 prototype's inventory and menus (src/items.c, src/menu.c).

Sources: EquipParamWeapon / Protector / Accessory / Goods, Magic, ReinforceParamWeapon (GameParam), item names and
descriptions (msg/ENGLISH/item.msgbnd), item icons (menu/menu_N.tpf IconNN sheets, cells as tools/export_menu.py).

Item ids are DSR's: category in the top nibble (progress.h ITEM_*), the param row below. A weapon id carries its
reinforcement level in the last two digits (Dagger 100000, Dagger+3 100003, Crystal Dagger 100100): the param row is
id - id % 100, ReinforceParamWeapon row reinforceTypeId + level.

Output (little-endian), four files that tools/pack.py puts into menu.pak:
  items.bin   resident (src/items.c, boot): what the inventory needs on a pickup
      header  'DSIT', u32 version, u32 goods count, u32 goods offset, u32 ammo count, u32 ammo offset, 2 x u32 0
      goods   {u32 row; u16 maxNum; u8 goodsType; u8 flags} sorted by row  (flags: 1 isConsume, 2 isDrop,
              4 isDeposit, 8 isOnlyOne, 16 isEquip)
      ammo    {u32 weapon row; u16 stack max; u16 0} sorted by row (arrowSlotEquipable / boltSlotEquipable rows)
  itemx.bin   loaded while a menu is open (src/items.c items_detail_load)
      header  'DSIX', u32 version, u32 rows, u32 rows offset, u32 reinforce rows, u32 reinforce offset,
              u32 names offset, u32 names bytes
      row     72 B, sorted by id (category | param row; weapons at level 0):
              u32 id, u32 sortId, u32 name offset (into names), u32 text offset (into desc.bin),
              u16 help bytes, u16 description bytes, u16 icon id, u8 tab (TAB_*), u8 sub, u16 weight x100,
              u16 durabilityMax, u16 x0, u16 x1, u16 x2, u16 0, s16 st[16], u32 0
        weapon:     sub weaponCategory, x0 staminaGuardDef (stability), x1 slots (1 right, 2 left, 4 both,
                    8 arrow, 16 bolt, 32 magic, 64 sorcery, 128 miracle, 256 pyromancy flag spare), st: attackBase
                    physics / magic / fire / thunder, correctStrength / Agility / Magic / Faith, proper Strength /
                    Agility / Magic / Faith, phys / mag / fire / thun GuardCutRate; x2 reinforceTypeId
        protector:  sub protectorCategory, x1 slot 0 head / 1 body / 2 arms / 3 legs, st: defense physics, blow
                    (strike), slash, thrust, magic, fire, thunder, resist poison, disease (toxic), blood, curse,
                    saDurability (poise)
        accessory:  st[0] refId (SpEffect)
        goods:      sub goodsType, x1 flags as items.bin, st: consumeHeroPoint, goodsUseAnim, refId, opmeMenuType,
                    maxNum, [5..8] spells only: Magic slotLength, requirementIntellect, requirementFaith,
                    maxQuantity, [9, 10] souls given on use (SpEffectParam.soul of refId, low / high 16 bits)
      reinforce {u32 row; u16 physicsAtkRate, magicAtkRate, fireAtkRate, thunderAtkRate, correctStrengthRate,
              correctAgilityRate, correctMagicRate, correctFaithRate (all x1000)} sorted by row
      names   NUL-terminated ASCII
  desc.bin    stored raw (read in place per item): the help line then the long description of each row, ASCII,
              each NUL-terminated (help bytes / description bytes include the NUL)
  icons.bin   stored raw: header 'DSIC', u32 version, u32 count, u32 cell bytes, u32 cell size; u16 icon ids sorted
              (count of them) padded to 64 B; then per icon at 64 + 64 x ceil(count / 32) + k x cell bytes: CLUT
              (256 x RGBA, CSM1 order, alpha 0..0x80) then ICON x ICON PSMT8 pixels

Tabs (the Inventory's eight, I01_tab_select order): 0 consumables, 1 materials, 2 key items, 3 spells, 4 weapons and
shields, 5 ammunition, 6 armor, 7 rings.
Usage: export_items.py <game dir> <out dir> [--icons-preview png]"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import resample  # Image.resize with a fixed pass order (TECH_DEBT build-7)

VERSION = 1
ICON = 48  # stored icon size: on screen DSR's 85 x 95 icon box is 42 x 59 PS2 pixels (I02 button_item_equip)
TAB_CONSUME, TAB_MATERIAL, TAB_KEY, TAB_SPELL, TAB_WEAPON, TAB_AMMO, TAB_ARMOR, TAB_RING = range(8)
CAT_WEAPON, CAT_PROTECTOR, CAT_ACCESSORY, CAT_GOODS = 0x00000000, 0x10000000, 0x20000000, 0x40000000

# FMG text uses a few characters DSFont24's ASCII 32-126 set (tools/export_menu.py) does not have
ASCII = {"’": "'", "‘": "'", "“": '"', "”": '"', "—": "-", "–": "-", "…": "...",
         "　": " ", "é": "e", "è": "e", "ê": "e", "à": "a", "ö": "o", "ü": "u",
         "ï": "i", "â": "a", "ô": "o", "ç": "c", "É": "E", "·": "-", "•": "-"}


def ascii_text(s):
    s = "".join(ASCII.get(c, c) for c in (s or ""))
    return s.encode("ascii", "replace").replace(b"\r", b"")


def i16(v):
    v = int(round(v))
    return max(-32768, min(32767, v))


def s16(v):
    """u16 bit pattern as s16 (split 32-bit values)"""
    return v - 0x10000 if v >= 0x8000 else v


def goods_flags(f):
    return (1 if f["isConsume"] else 0) | (2 if f["isDrop"] else 0) | (4 if f["isDeposit"] else 0) | \
           (8 if f["isOnlyOne"] else 0) | (16 if f["isEquip"] else 0)


def goods_tab(f):
    t = f["goodsType"]
    return {1: TAB_KEY, 2: TAB_MATERIAL, 5: TAB_SPELL}.get(t, TAB_CONSUME)


def build(game):
    P = souls.game_params(game)
    msg = {souls.basename(n): souls.fmg(d) for _, n, d in
           souls.bnd3(souls.read(os.path.join(game, "msg", "ENGLISH", "item.msgbnd.dcx")))}
    rows = []  # (id, sort, name, help, desc, icon, tab, sub, weight, dur, x0, x1, st)

    def text(kind, i, base=None):
        n, h, d = (msg[f"{kind}_name_.fmg"], msg[f"{kind}_description_.fmg"], msg[f"{kind}_long_desc_.fmg"])
        def get(t):
            v = t.get(i)
            return v if v or base is None else t.get(base)
        return get(n) or "", get(h) or "", get(d) or ""

    for i, (_, f) in sorted(P["EquipParamWeapon"].items()):
        slots = (1 if f["rightHandEquipable"] else 0) | (2 if f["leftHandEquipable"] else 0) | \
                (4 if f["bothHandEquipable"] else 0) | (8 if f["arrowSlotEquipable"] else 0) | \
                (16 if f["boltSlotEquipable"] else 0) | (32 if f["enableMagic"] else 0) | \
                (64 if f["enableSorcery"] else 0) | (128 if f["enableMiracle"] else 0)
        tab = TAB_AMMO if f["arrowSlotEquipable"] or f["boltSlotEquipable"] else TAB_WEAPON
        st = [f["attackBasePhysics"], f["attackBaseMagic"], f["attackBaseFire"], f["attackBaseThunder"],
              f["correctStrength"], f["correctAgility"], f["correctMagic"], f["correctFaith"],
              f["properStrength"], f["properAgility"], f["properMagic"], f["properFaith"],
              f["physGuardCutRate"], f["magGuardCutRate"], f["fireGuardCutRate"], f["thunGuardCutRate"]]
        name, hlp, desc = text("Weapon", i, i - i % 1000)
        rows.append((CAT_WEAPON | i, f["sortId"], name, hlp, desc, f["iconId"], tab, f["weaponCategory"],
                     f["weight"], f["durabilityMax"], f["staminaGuardDef"], slots, st, f["reinforceTypeId"]))
    for i, (_, f) in sorted(P["EquipParamProtector"].items()):
        slot = 0 if f["headEquip"] else 1 if f["bodyEquip"] else 2 if f["armEquip"] else 3
        st = [f["defensePhysics"], f["defenseBlow"], f["defenseSlash"], f["defenseThrust"], f["defenseMagic"],
              f["defenseFire"], f["defenseThunder"], f["resistPoison"], f["resistDisease"], f["resistBlood"],
              f["resistCurse"], f["saDurability"]]
        name, hlp, desc = text("Armor", i, i - i % 1000)
        rows.append((CAT_PROTECTOR | i, f["sortId"], name, hlp, desc, f["iconIdM"], TAB_ARMOR,
                     f["protectorCategory"], f["weight"], f["durabilityMax"], 0, slot, st, 0))
    for i, (_, f) in sorted(P["EquipParamAccessory"].items()):
        name, hlp, desc = text("Accessory", i)
        rows.append((CAT_ACCESSORY | i, f["sortId"], name, hlp, desc, f["iconId"], TAB_RING,
                     f["accessoryCategory"], f["weight"], 0, 0, 0, [f["refId"]], 0))
    magic = P["Magic"]
    for i, (_, f) in sorted(P["EquipParamGoods"].items()):
        st = [f["consumeHeroPoint"], f["goodsUseAnim"], f["refId"], f["opmeMenuType"], f["maxNum"]]
        if f["goodsType"] == 5 and f["magicId"] in magic:
            m = magic[f["magicId"]][1]
            st += [m["slotLength"], m["requirementIntellect"], m["requirementFaith"], m["maxQuantity"]]
        sp = P["SpEffectParam"].get(f["refId"]) if f["refCategory"] == 1 else None
        if sp and sp[1]["soul"] > 0:  # soul items: SpEffectParam.soul souls on use
            st = (st + [0] * 16)[:16]
            st[9], st[10] = s16(sp[1]["soul"] & 0xFFFF), s16(sp[1]["soul"] >> 16)
        name, hlp, desc = text("Item", i)
        rows.append((CAT_GOODS | i, f["sortId"], name, hlp, desc, f["iconId"], goods_tab(f), f["goodsType"],
                     f["weight"], 0, 0, goods_flags(f), st, 0))
    rows.sort(key=lambda r: r[0])

    # items.bin
    goods = [(i, f) for i, (_, f) in sorted(P["EquipParamGoods"].items())]
    ammo = [(i, 99) for i, (_, f) in sorted(P["EquipParamWeapon"].items())
            if f["arrowSlotEquipable"] or f["boltSlotEquipable"]]  # ASSUMED (TECH_DEBT inv-2): DS1 stacks ammo to 99
    hdr = 32
    goods_off = hdr
    ammo_off = goods_off + 8 * len(goods)
    items = struct.pack("<4s7I", b"DSIT", VERSION, len(goods), goods_off, len(ammo), ammo_off, 0, 0)
    items += b"".join(struct.pack("<IHBB", i, min(f["maxNum"], 0xFFFF), f["goodsType"], goods_flags(f))
                      for i, f in goods)
    items += b"".join(struct.pack("<IHH", i, m, 0) for i, m in ammo)

    # itemx.bin + desc.bin
    names, name_of = bytearray(), {}
    desc = bytearray()
    body = bytearray()
    for (iid, sort, name, hlp, dsc, icon, tab, sub, weight, dur, x0, x1, st, _rt) in rows:
        nb = ascii_text(name)
        if nb not in name_of:
            name_of[nb] = len(names)
            names += nb + b"\0"
        hb, db = ascii_text(hlp) + b"\0", ascii_text(dsc) + b"\0"
        toff = len(desc)
        desc += hb + db
        st = (list(st) + [0] * 16)[:16]
        body += struct.pack("<4I2HHBBHH4H16hI", iid, sort & 0xFFFFFFFF, name_of[nb], toff, min(len(hb), 0xFFFF),
                            min(len(db), 0xFFFF), icon & 0xFFFF, tab, sub & 0xFF, min(int(round(weight * 100)), 0xFFFF),
                            min(dur, 0xFFFF), x0 & 0xFFFF, x1 & 0xFFFF, _rt & 0xFFFF, 0, *[i16(v) for v in st], 0)
    rein = []
    for i, (_, f) in sorted(P["ReinforceParamWeapon"].items()):
        k = lambda n: min(0xFFFF, max(0, int(round(f[n] * 1000))))
        rein.append(struct.pack("<I8H", i, k("physicsAtkRate"), k("magicAtkRate"), k("fireAtkRate"),
                                k("thunderAtkRate"), k("correctStrengthRate"), k("correctAgilityRate"),
                                k("correctMagicRate"), k("correctFaithRate")))
    hx = 32
    rows_off = hx
    rein_off = rows_off + len(body)
    names_off = rein_off + 20 * len(rein)
    itemx = struct.pack("<4s7I", b"DSIX", VERSION, len(rows), rows_off, len(rein), rein_off, names_off, len(names))
    itemx += body + b"".join(rein) + names
    return P, rows, bytes(items), bytes(itemx), bytes(desc)


def icons(game, ids, preview=None):
    from PIL import Image
    import export_menu as em
    drb_bytes = souls.read(os.path.join(game, "menu", "menu.drb.dcx"))
    cells = em.icon_cells(drb_bytes)
    sheets = {}
    ids = sorted(i for i in set(ids) if 0 <= i < 0xFFFF)
    out, kept = [], []
    for i in ids:
        try:
            img = em.icon_image(game, sheets, cells, i)
        except (KeyError, IndexError):
            continue
        img = resample.resize(img, (ICON, ICON), Image.LANCZOS) if img.size != (ICON, ICON) else img
        clut, pix = em.quantize(img)
        out.append(clut + pix)
        kept.append((i, img))
    n = len(kept)
    cell = 1024 + ICON * ICON
    table = struct.pack(f"<{n}H", *[i for i, _ in kept])
    table += b"\0" * ((-len(table)) % 64)
    blob = struct.pack("<4s4I", b"DSIC", VERSION, n, cell, ICON) + b"\0" * (64 - 20) + table + b"".join(out)
    if preview and kept:
        cols = 16
        sheet = Image.new("RGBA", (cols * ICON, ((n + cols - 1) // cols) * ICON), (40, 40, 40, 255))
        for k, (_, img) in enumerate(kept):
            sheet.alpha_composite(img, ((k % cols) * ICON, (k // cols) * ICON))
        sheet.save(preview)
    return blob, n


def main():
    game, out = sys.argv[1], sys.argv[2]
    preview = sys.argv[sys.argv.index("--icons-preview") + 1] if "--icons-preview" in sys.argv else None
    os.makedirs(out, exist_ok=True)
    P, rows, items, itemx, desc = build(game)
    ic, nic = icons(game, [r[5] for r in rows], preview)
    for name, data in (("items.bin", items), ("itemx.bin", itemx), ("desc.bin", desc), ("icons.bin", ic)):
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
    print(f"items: {len(rows)} rows, items.bin {len(items)} B (resident), itemx.bin {len(itemx)} B, desc.bin "
          f"{len(desc)} B, icons.bin {nic} icons {len(ic)} B")


if __name__ == "__main__":
    main()
