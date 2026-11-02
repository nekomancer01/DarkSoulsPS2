"""Export DSR's HUD / menu assets for the PS2 prototype (src/hud.c, src/menu.c).

Layout: menu/menu.drb (the game's dialog layout, 1280x720 coordinates) -> 640x448. Sprites: the DRB's sprite shapes
(texture index into the DRB's texture list, texel rectangle) cut from the menu TPFs (menu/menu_N.tpf), each resampled
to exactly its on-screen size on the PS2, packed into one PSMT8 atlas with the font. Font: font/english/DSFont24
(.ccm glyph table + .tpf pages), ASCII 32-126 scaled to FONT_H pixels per line. Icons: the Warrior's starting items
(CharaInitParam row 3000, Estus Flask added) from the IconNN sheets: 160x180 cells, 12 per row, 132 per sheet (their
rectangles are the DRB sprites on each sheet, taken in row-major order); icon id -> sheet
"Icon<id // 1000><(id % 1000) // 132>", cell (id % 1000) % 132; scaled to ICON px in a second atlas. Text: English
FMGs.

DRB (DS1 PC, little-endian): "DRB\\0", then chunks {char[4] name, u32 size, u32 count, 4 pad, data padded to 16}: STR
(UTF-16 strings, other chunks point at byte offsets into it), TEXI {u32 name, u32 path, ...} 16 B, SHPR (shape
parameters), SHAP {u32 type name, u32 SHPR offset}, DLGO {u32 name, u32 SHAP offset, u32 CTRL offset, ...} 32 B,
DLG {u32 name, u32 shape, u32 ctrl, ..., u32 element count at +32, u32 DLGO offset at +36} 64 B. Shape parameters
start with s16 left, top, right, bottom (relative to the parent dialog's left/top); Sprite: +16 s16 texel left, top,
right, bottom, +24 s16 texture index; Dialog: +16 u8 child DLG index; MonoRect: +24 colour a, r, g, b; GouraudRect:
+20 four such corner colours (averaged here).

Output menu.bin (little-endian):
  header  'DSMN', u32 version 1, atlases, sprites, glyphs, layout entries, strings; u32 offsets of each table;
          u16 font line height, first icon sprite; pad to 64
  atlas   {u32 w, h, clut offset, pixel offset}  (PSMT8 pixels, CSM1 CLUT of RGBA, alpha 0..0x80)
  sprite  {u16 atlas, u, v, w, h, pad}           (texels)
  glyph   {u16 u, v, w, h; s16 pre-space, advance} for ASCII 32..126 (atlas 0)
  layout  {s16 sprite (-1: solid rect), x, y, w, h (640x448 pixels); u8 r, g, b, a}  in the order of LAYOUT
  strings u32 offsets, then NUL-terminated ASCII, in the order of STRINGS
Usage: export_menu.py <game dir> <out.bin> [preview.png]"""
import math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import resample  # Image.resize with a fixed pass order (TECH_DEBT build-7)

SX, SY = 640 / 1280, 448 / 720
FONT_H = 17  # pixels per line on the PS2 (DSFont24 glyph cells are 58 px tall for the game's 720p UI)
ICON = 64
ATLAS_W, ATLAS_H = 512, 256

# (layout name, dialog, element, offset dialogs to add): HUD elements in draw order. hud.h's HUD_* enum follows it.
LAYOUT = [
    ("bar_bg_hp", "F20-00", "bar_BG_0"), ("bar_bg_sp", "F20-00", "bar_BG_1"),
    ("plate_hp", "F20-00", "plate_hp"), ("plate_sp", "F20-00", "plate_sp"),
    ("fill_hp", "F20-00", "bar_0_hp"), ("fill_sp", "F20-00", "bar_0_sp"),
    ("frame_hp", "F20-00", "bar_2_hp"), ("frame_sp", "F20-00", "bar_2_sp"),
    ("end_hp", "F20-00", "HP_endbar"), ("end_sp", "F20-00", "SP_endbar"),
    ("bar_start", "F20-00", "bar_start"), ("bar_start1", "F20-00", "bar_start1"),
    ("slot_l_plate", "F20-00/F20-00_arms_l", "icon_plate_0"), ("slot_l_icon", "F20-00/F20-00_arms_l", "icon"),
    ("slot_r_plate", "F20-00/F20-00_arms_ｒ", "icon_plate_0"), ("slot_r_icon", "F20-00/F20-00_arms_ｒ", "icon"),
    ("slot_i_frame", "F20-00/F20-00_item", "FE_lineframe_item"), ("slot_i_plate", "F20-00/F20-00_item", "icon_plate_0"),
    ("slot_i_icon", "F20-00/F20-00_item", "icon"), ("slot_i_count", "F20-00/F20-00_item", "text"),
    ("item_name", "F20-00", "text_item_name"),
    ("soul_plate", "F20-00/soul_param", "bg_parchment_0"), ("soul_line", "F20-00/soul_param", "Option_line_0"),
    ("soul_text", "F20-00/soul_param", "text_param_soul"),
    ("died_bg", "F23_YouDied", "BG"), ("died_grad0", "F23_YouDied", "BG_grad00"),
    ("died_grad1", "F23_YouDied", "BG_grad01"), ("died_bar", "F23_YouDied", "BG_bar"),
    ("died_text", "F23_YouDied", "main_text_youdied"),
    # BONFIRE LIT (global_event.lua SetTextEffect(TEXT_TYPE_Bonfire = 13)): the same backdrop pieces as YOU DIED
    ("lit_bg", "F23_Bonfire", "BG"), ("lit_grad0", "F23_Bonfire", "BG_grad00"),
    ("lit_grad1", "F23_Bonfire", "BG_grad01"), ("lit_bar", "F23_Bonfire", "BG_bar"),
    ("lit_text", "F23_Bonfire", "main_text_revive"),
]
# stored size factors (x, y) for long / large sprites that are drawn stretched (fits the atlas in 512x256)
STORE = {"frame_hp": (0.5, 1), "frame_sp": (0.5, 1), "fill_hp": (0.5, 1), "fill_sp": (0.5, 1), "died_text": (0.5, 0.5), "lit_text": (0.5, 0.5)}
# menu panel pieces (not from a DRB dialog: a plain read-only screen built from Menu01's parts): texture, texel rect,
# 1280x720 screen rect, stored scale (the leather panel is stored small and stretched, it has no fine detail)
MENU_SPRITES = [
    ("menu_panel", "Menu01", (560, 5, 1520, 760), (160, 60, 1120, 660), 0.25),
    ("menu_rule", "Menu01", (355, 805, 1170, 830), (200, 140, 1080, 152), 1.0),
]
# the Warrior's starting kit (CharaInitParam 3000) + Estus: (string name, FMG, item id, icon id source)
KIT = [("wep_r", "Weapon_name_.fmg", 201000, "w"), ("wep_l", "Weapon_name_.fmg", 1450000, "w"),
       ("helm", "Armor_name_.fmg", 210000, "a"), ("armor", "Armor_name_.fmg", 211000, "a"),
       ("gaunt", "Armor_name_.fmg", 212000, "a"), ("leg", "Armor_name_.fmg", 213000, "a"),
       ("estus", "Item_name_.fmg", 201, "g"), ("darksign", "Item_name_.fmg", 117, "g"),
       ("crystal", "Item_name_.fmg", 103, "g")]
# fixed strings: (FMG file, id) from msg/ENGLISH/menu.msgbnd
UI_STRINGS = [("Ingame_menu_.fmg", 110000), ("Ingame_menu_.fmg", 110001), ("Ingame_menu_.fmg", 110003),
              ("Ingame_menu_.fmg", 112010), ("Ingame_menu_.fmg", 112000), ("Menu_general_text_.fmg", 101013),
              ("Menu_general_text_.fmg", 101030), ("Menu_general_text_.fmg", 101032),
              ("Menu_general_text_.fmg", 101041)] + [
    # bonfire (src/bonfire.c; ids from the bonfire talk ESDs t1xx1xx / t1xx2xx): prompts, menu entries, dialogs
    ("Event_text_.fmg", i) for i in (10010182, 10010183, 15000100, 15000111, 15000112, 15000120, 15000130, 15000220,
                                     15000150, 10010744, 15000170, 15000270, 15000005, 10010741, 10010722, 10010723,
                                     10010724, 10010725, 10010730, 10010731, 10010732)]


def drb(b):
    """menu.drb -> (string at offset, chunk table {name: (data offset, size, count)})"""
    sec, p = {}, 0x10
    while True:
        name = b[p:p + 4].rstrip(b"\0").decode()
        size, count = struct.unpack_from("<II", b, p + 4)
        sec[name] = (p + 16, size, count)
        if name == "END":
            break
        p = (p + 16 + size + 15) & ~15

    def s(off):
        a = e = sec["STR"][0] + off
        while b[e:e + 2] != b"\0\0":
            e += 2
        return b[a:e].decode("utf-16le")
    return s, sec


def icon_cells(b):
    """{icon sheet name: [texel rect]} from every DRB sprite on an IconNN texture, row-major"""
    s, sec = drb(b)
    tex = [s(struct.unpack_from("<I", b, sec["TEXI"][0] + 16 * i)[0]) for i in range(sec["TEXI"][2])]
    cells = {}
    for k in range(sec["SHAP"][2]):
        t, sp = struct.unpack_from("<2I", b, sec["SHAP"][0] + 8 * k)
        if s(t) != "Sprite":
            continue
        tl, tt, tr, tb, ti = struct.unpack_from("<5h", b, sec["SHPR"][0] + sp + 16)
        if 0 <= ti < len(tex) and tex[ti].startswith("Icon") and tr > tl:
            cells.setdefault(tex[ti], set()).add((tl, tt, tr, tb))
    return {n: sorted(v, key=lambda r: (r[1], r[0])) for n, v in cells.items()}


def drb_elements(b):
    """{dialog name: {element name: (shape type, SHPR bytes)}}, texture names"""
    s, sec = drb(b)
    tex = [s(struct.unpack_from("<I", b, sec["TEXI"][0] + 16 * i)[0]) for i in range(sec["TEXI"][2])]
    out = {}
    for i in range(sec["DLG"][2]):
        v = struct.unpack_from("<16I", b, sec["DLG"][0] + 64 * i)
        els = {}
        for k in range(v[8]):
            o = struct.unpack_from("<3I", b, sec["DLGO"][0] + v[9] + 32 * k)
            t, sp = struct.unpack_from("<2I", b, sec["SHAP"][0] + o[1])
            els[s(o[0])] = (s(t), b[sec["SHPR"][0] + sp:sec["SHPR"][0] + sp + 48])
        out[s(v[0])] = els
    return out, tex


def find(dlgs, path, elem):
    """element in 'dialog/sub/sub' -> (type, SHPR, x0, y0, x1, y1 in 1280x720): sub-dialog origins are added"""
    parts = path.split("/")
    ox = oy = 0
    for a, c in zip(parts, parts[1:]):
        t, raw = dlgs[a][c]
        l, tp = struct.unpack_from("<2h", raw)
        ox, oy = ox + l, oy + tp
    t, raw = dlgs[parts[-1]][elem]
    l, tp, r, bt = struct.unpack_from("<4h", raw)
    return t, raw, ox + l, oy + tp, ox + r, oy + bt


def load_textures(game, names):
    """menu TPF texture name -> PIL RGBA image"""
    from PIL import Image
    out = {}
    for f in sorted(os.listdir(os.path.join(game, "menu"))):
        if not (f.startswith("menu_") and f.endswith(".tpf.dcx")):
            continue
        for n, d in souls.tpf(souls.read(os.path.join(game, "menu", f))).items():
            if n in names and n not in out:
                w, h, px = souls.dds_decode_rgba(d, 4096)
                out[n] = Image.frombytes("RGBA", (w, h), px)
    return out


class Packer:
    """Shelf packer for one atlas: add() places at once; defer() collects images and place_all() packs them tallest
    first (much denser than insertion order)"""
    def __init__(self, w, h):
        self.w, self.h, self.x, self.y, self.row = w, h, 0, 0, 0
        self.pending = []

    def defer(self, img, done):
        """queue img; done(x, y) is called with its place"""
        self.pending.append((img, done))

    def place_all(self, atlas):
        for img, done in sorted(self.pending, key=lambda p: (-p[0].size[1], -p[0].size[0])):
            x, y = self.add(*img.size)
            atlas.alpha_composite(img, (x, y))
            done(x, y)
        self.pending = []

    def add(self, w, h):
        if self.x + w > self.w:
            self.x, self.y, self.row = 0, self.y + self.row + 1, 0
        if self.y + h > self.h:
            raise ValueError(f"atlas full at {w}x{h}")
        p = (self.x, self.y)
        self.x += w + 1
        self.row = max(self.row, h)
        return p


def quantize(img):
    """RGBA image -> (CSM1 CLUT bytes with alpha 0..0x80, PSMT8 pixels)"""
    from PIL import Image
    import octree  # Pillow's FASTOCTREE, the same on every platform (TECH_DEBT build-7)
    q = octree.quantize(img, 256)
    pal = list(q.getpalette("RGBA") or [])[:1024]
    pal += [0] * (1024 - len(pal))
    clut = bytearray(1024)
    for i in range(256):
        j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
        r, g, b, a = pal[4 * i:4 * i + 4]
        clut[4 * j:4 * j + 4] = bytes([r, g, b, (a * 0x80 + 127) // 255])
    return bytes(clut), q.tobytes()


def font(game, atlas, packer):
    """DSFont24 ASCII glyphs -> [(u, v, w, h, pre-space, advance)] scaled to FONT_H, drawn into atlas"""
    from PIL import Image
    ccm = souls.read(os.path.join(game, "font", "english", "DSFont24.ccm.dcx"))
    full_h, tw, th = struct.unpack_from("<3h", ccm, 8)
    groups = struct.unpack_from("<h", ccm, 0x10)[0]
    code_glyph = {}
    for g in range(groups):
        first, last, idx = struct.unpack_from("<3I", ccm, 0x20 + 12 * g)
        for c in range(first, last + 1):
            code_glyph.setdefault(c, idx + c - first)  # the first group wins (later ones repeat codes)
    glyph_off = 0x20 + 12 * groups
    pages = {n: d for n, d in souls.tpf(souls.read(os.path.join(game, "font", "english", "DSFont24.tpf.dcx"))).items()}
    page_img = {}
    k = FONT_H / full_h
    out = []
    for c in range(32, 127):
        u0, v0, u1, v1 = struct.unpack_from("<4f", ccm, glyph_off + 24 * code_glyph[c])
        pre, width, adv, page = struct.unpack_from("<4h", ccm, glyph_off + 24 * code_glyph[c] + 16)
        if page not in page_img:
            name = list(pages)[page]  # the glyph's page is an index into the TPF's texture order
            w, h, px = souls.dds_decode_rgba(pages[name], 4096)
            page_img[page] = Image.frombytes("RGBA", (w, h), px)
        src = page_img[page].crop((round(u0 * tw), round(v0 * th), round(u1 * tw), round(v1 * th)))
        gw = max(1, round(src.size[0] * k))
        g = resample.resize(src, (gw, FONT_H), Image.LANCZOS)
        out.append([0, 0, gw, FONT_H, round(pre * k), max(1, round(adv * k))])
        packer.defer(g, lambda x, y, e=out[-1]: e.__setitem__(slice(0, 2), [x, y]))
    return out


def icon_image(game, sheets, cells, icon_id):
    """icon id -> ICON x ICON image: the 160x180 cell, centred on a square"""
    from PIL import Image
    name = f"Icon{icon_id // 1000}{(icon_id % 1000) // 132}"
    if name not in sheets:
        sheets.update(load_textures(game, {name}))
    l, t, r, b = cells[name][(icon_id % 1000) % 132]
    cell = sheets[name].crop((l, t, r, b))
    sq = Image.new("RGBA", (b - t, b - t), (0, 0, 0, 0))
    sq.alpha_composite(cell, ((b - t - (r - l)) // 2, 0))
    return resample.resize(sq, (ICON, ICON), Image.LANCZOS)


def main():
    from PIL import Image
    game, out = sys.argv[1], sys.argv[2]
    preview = sys.argv[3] if len(sys.argv) > 3 else None
    drb_bytes = souls.read(os.path.join(game, "menu", "menu.drb.dcx"))
    dlgs, tex_names = drb_elements(drb_bytes)
    cells = icon_cells(drb_bytes)
    want = []
    for name, path, elem in LAYOUT:
        t, raw, x0, y0, x1, y1 = find(dlgs, path, elem)
        want.append((name, t, raw, x0, y0, x1, y1))
    needed = {tex_names[struct.unpack_from("<h", raw, 24)[0]] for _, t, raw, *_ in want if t == "Sprite"
              and 0 <= struct.unpack_from("<h", raw, 24)[0] < len(tex_names)} | {m[1] for m in MENU_SPRITES}
    texs = load_textures(game, needed)

    atlas = Image.new("RGBA", (ATLAS_W, ATLAS_H), (0, 0, 0, 0))
    pk = Packer(ATLAS_W, ATLAS_H)
    glyphs = font(game, atlas, pk)
    sprites, sprite_of = [], {}

    def add_sprite(img_name, src, dw, dh):
        key = (img_name, src, dw, dh)
        if key not in sprite_of:
            img = resample.resize(texs[img_name].crop(src), (dw, dh), Image.LANCZOS)
            sprite_of[key] = len(sprites)
            sprites.append([0, 0, 0, dw, dh])
            pk.defer(img, lambda x, y, e=sprites[-1]: e.__setitem__(slice(1, 3), [x, y]))
        return sprite_of[key]

    layout = []
    for name, t, raw, x0, y0, x1, y1 in want:
        x, y = round(x0 * SX), round(y0 * SY)
        w, h = max(1, round(x1 * SX) - x), max(1, round(y1 * SY) - y)
        spr, col = -1, (255, 255, 255, 0x80)
        if t == "Sprite":
            tl, tt, tr, tb, ti = struct.unpack_from("<5h", raw, 16)
            if 0 <= ti < len(tex_names) and tr > tl:
                kx, ky = STORE.get(name, (1, 1))
                spr = add_sprite(tex_names[ti], (max(0, tl), max(0, tt), tr, tb), max(1, round(w * kx)),
                                 max(1, round(h * ky)))
        elif t in ("MonoRect", "GouraudRect"):
            at = 20 if t == "GouraudRect" else 24
            cs = [raw[at + 4 * k:at + 4 + 4 * k] for k in range(4 if t == "GouraudRect" else 1)]
            a, r, g, b = (sum(c[j] for c in cs) // len(cs) for j in range(4))
            col = (r, g, b, (a * 0x80 + 127) // 255)
        layout.append((spr, x, y, w, h) + col)
    menu_layout = []
    for name, img_name, src, dst, keep in MENU_SPRITES:
        x, y = round(dst[0] * SX), round(dst[1] * SY)
        w, h = round(dst[2] * SX) - x, round(dst[3] * SY) - y
        spr = add_sprite(img_name, src, max(1, round(w * keep)), max(1, round(h * keep)))
        menu_layout.append((spr, x, y, w, h, 255, 255, 255, 0x80))
    pk.place_all(atlas)

    # icons
    P = souls.game_params(game)
    msg = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(os.path.join(game, "msg", "ENGLISH", "item.msgbnd.dcx")))}
    menu_msg = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(os.path.join(game, "msg", "ENGLISH", "menu.msgbnd.dcx")))}
    icons = Image.new("RGBA", (ATLAS_W, 128), (0, 0, 0, 0))
    sheets, names = {}, []
    for k, (key, fmg, item, kind) in enumerate(KIT):
        if kind == "w":
            icon = P["EquipParamWeapon"][item][1]["iconId"]
        elif kind == "a":
            icon = P["EquipParamProtector"][item][1]["iconIdM"]
        else:
            icon = P["EquipParamGoods"][item][1]["iconId"]
        icons.alpha_composite(icon_image(game, sheets, cells, icon), ((k % 8) * ICON, (k // 8) * ICON))
        sprites.append((1, (k % 8) * ICON, (k // 8) * ICON, ICON, ICON))
        names.append(souls.fmg(msg[fmg]).get(item) or key)
    icon_sprite0 = len(sprites) - len(KIT)
    strings = names + [souls.fmg(menu_msg[f]).get(i, "") for f, i in UI_STRINGS]

    atlases = [quantize(atlas), quantize(icons)]
    blob = bytearray()
    HDR = 64

    def align(n):
        blob.extend(b"\0" * ((-(len(blob) + HDR)) % n))
    offs = {}
    align(16)
    offs["atlas"] = len(blob) + HDR
    table = len(blob)
    blob += b"\0" * (16 * len(atlases))
    for k, ((clut, pix), img) in enumerate(zip(atlases, (atlas, icons))):
        align(128)
        c = len(blob) + HDR
        blob += clut
        align(128)
        p = len(blob) + HDR
        blob += pix
        struct.pack_into("<4I", blob, table + 16 * k, img.size[0], img.size[1], c, p)
    align(4)
    offs["sprites"] = len(blob) + HDR
    for s_ in sprites:
        blob += struct.pack("<6H", *s_, 0)
    offs["glyphs"] = len(blob) + HDR
    for g in glyphs:
        blob += struct.pack("<4H2h", *g)
    offs["layout"] = len(blob) + HDR
    for e in layout + menu_layout:
        blob += struct.pack("<5h4B", *e)
    offs["strings"] = len(blob) + HDR
    st = bytearray()
    so = []
    for t in strings:
        so.append(len(st))
        st += t.encode("ascii", "replace") + b"\0"
    base = offs["strings"] + 4 * len(strings)
    blob += struct.pack(f"<{len(so)}I", *[base + o for o in so]) + st
    header = struct.pack("<4s6I5I2H", b"DSMN", 1, len(atlases), len(sprites), len(glyphs),
                         len(layout) + len(menu_layout), len(strings), offs["atlas"], offs["sprites"],
                         offs["glyphs"], offs["layout"], offs["strings"], FONT_H, icon_sprite0)
    assert len(header) <= HDR
    header += b"\0" * (HDR - len(header))
    with open(out, "wb") as f:
        f.write(header + blob)
    vram = sum(len(c) + len(p) for c, p in atlases)
    print(f"menu.bin: {len(sprites)} sprites ({pk.y + pk.row} of {ATLAS_H} atlas rows used), {len(glyphs)} glyphs, "
          f"{len(layout) + len(menu_layout)} layout entries, {len(strings)} strings, VRAM {vram // 1024} KB, "
          f"{len(header) + len(blob)} bytes")
    if preview:
        prev_img = Image.new("RGBA", (640, 448), (60, 70, 80, 255))
        for spr, x, y, w, h, r, g, b, a in layout:
            if spr >= 0:
                at, u, v, sw, sh = sprites[spr]
                piece = resample.resize((atlas if at == 0 else icons).crop((u, v, u + sw, v + sh)), (w, h), Image.BILINEAR)
                prev_img.alpha_composite(piece, (x, y))
        prev_img.save(preview)
        atlas.save(preview.replace(".png", "_atlas.png"))
        icons.save(preview.replace(".png", "_icons.png"))


if __name__ == "__main__":
    main()
