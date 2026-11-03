"""Export DSR's menu screens (menu/menu.drb dialogs) for the PS2 prototype's menus (src/drb.c, src/menu.c).

Each screen is a list of root dialogs from menu.drb (the screen itself plus the templates the code places: cursors,
item buttons, popups), flattened depth first with every nested dialog, at DSR's own coordinates scaled from 1280x720
to 640x448. Their sprites go into one PSMT8 atlas per screen (the menu borrows VRAM for it while the screen is open,
src/texcache.c tc_pin): each sprite cut from its menu TPF texture once per texel rectangle, stored at its largest
on-screen size or less (the largest pieces are halved until the atlas fits: the parchments have no fine detail).
Item icons are not in it (tools/export_items.py icons.bin) and neither is the font (the HUD atlas, src/hud.c).

DRB element types used (shape parameters after the s16 left, top, right, bottom rectangle relative to the parent
dialog, see tools/export_menu.py): Dialog +16 u16 child DLG index; Sprite +16 s16 texel left, top, right, bottom,
+24 s16 texture index, +32 tint a, b, g, r; MonoRect +24 colour a, b, g, r; GouraudRect +20 four corner colours (TL, TR, BR, BL); Text and
ScrollText +24 colour a, b, g, r, +28 u8 font size class, +30 u8 alignment (1 right, 2 centre, 8 middle,
4 bottom: inferred from where labels and values sit), +36 u32 msgbnd file id, +40 u32 FMG text id (the text, which
for most elements is a <?tag?> the game fills in).

Output scr_<name>.bin (little-endian):
  header  'DSCR', u32 version, u32 nodes, u32 node offset, u32 roots, u32 root offset, u32 strings, u32 string offset,
          u16 atlas w, h, u32 clut offset, u32 pixel offset, u32 sprites, u32 sprite offset, u32 gradients,
          u32 gradient offset; padded to 64
  node    {s16 x0, y0, x1, y1 (640x448, absolute); u8 type; u8 align; u16 end (index after its subtree);
           s16 sprite (atlas sprite; Text: string index; GouraudRect: gradient index; -1 none); u8 font; u8 flags (1: hidden by default: a "bk_"
           backup or "mg_" (the cut MP stat) element, or wholly off screen = a template the code moves);
           u32 colour (r, g, b, a with a 0..0x80);
           u32 name hash (FNV-1a 32 of the element name)}  24 B
          types: 0 Null, 1 Dialog, 2 Sprite, 3 Rect (MonoRect / GouraudRect), 4 Text (also ScrollText)
  root    u32 node index (the root's node is a Dialog named after the DRB dialog)
  strings u32 offsets, then NUL-terminated ASCII (FMG text, <?tags?> kept)
  sprite  {u16 u, v, w, h}  (texels in the atlas)
  gradient {u8 r, g, b, a} x 4: top left, top right, bottom right, bottom left (a 0..0x80); the node's colour is
          their average
  atlas   CLUT (256 x RGBA, CSM1, alpha 0..0x80) at its offset, PSMT8 pixels at theirs, both 128-byte aligned
Usage: export_screens.py <game dir> <out dir> [preview dir]"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import export_menu as em
import export_items
import resample

VERSION = 2
SX, SY = 640 / 1280, 448 / 720
T_NULL, T_DIALOG, T_SPRITE, T_RECT, T_TEXT = range(5)
MENU_COLORS = {}  # MenuColorTableParam row -> (r, g, b, a), set in main
# DRB text colours are not what DSR shows: its menus draw their text neutral white (reference capture 2026-11-03,
# 1920x1080: brightest glyph pixels 254-255 on the I01 title, key guide and covenant, 202-213 only where thin small
# glyphs are antialiased), also where the DRB says ff9cb9ca / ff8cb4d2 (965 + 467 of menu.drb's 1894 text elements,
# beige / tan read as a, b, g, r) or ff000016 (the "Inventory" title, near black). MenuColorTableParam 1 / 3 / 7
# "normal text" is 240, 240, 240; the code picks the colour, so every menu text is white here, its DRB alpha kept
# (ASSUMED for texts not seen in the capture, TECH_DEBT menu-3)

# screen -> roots. The screen's own dialog first; templates after it (src/menu.c places them).
SCREENS = {
    "R00": (["R00", "cursor02", "cursor03"], 48),          # start menu icon bar (top right, the game keeps running)
    "I02": (["I02", "cursor00", "cursor01", "C07", "list_item", "list_arms", "list_armor", "I01-01", "F05"], 256),
    "I01": (["I01", "cursor00", "cursor01", "cursor07", "button_item_select", "I01-01", "F05"], 256),
    "C01": (["C01", "cursor01"], 224),                     # status
    "F04": (["F04", "F05", "cursor01"], 96),               # talk / bonfire list, yes / no dialog
}


def fnv(s):
    h = 0x811C9DC5
    for c in s.encode("utf-8"):
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


def ascii_text(s):
    return export_items.ascii_text(s).decode("ascii")


def flatten(dlgs, names, roots):
    """-> nodes [(name, type, raw, x0, y0, x1, y1, hidden)], root indices; DFS, end filled in"""
    nodes, root_idx = [], []

    def walk(dname, ox, oy, hidden, depth, kx=1.0, ky=1.0):
        if depth > 12:
            return
        for el, (t, raw) in dlgs[dname].items():
            l, tp, r, bt = struct.unpack_from("<4h", raw)
            x0, y0, x1, y1 = ox + l * kx, oy + tp * ky, ox + r * kx, oy + bt * ky
            h = hidden or el.startswith(("bk_", "mg_")) or x1 <= 0 or y1 <= 0 or x0 >= 1280 or y0 >= 720
            k = len(nodes)
            nodes.append([el, t, raw, x0, y0, x1, y1, h, k + 1])
            if t == "Dialog":
                ci = struct.unpack_from("<H", raw, 16)[0]
                if ci < len(names):
                    # a cursor template (cursor00..) or a panel background (R03_bg_*) fills the dialog that places
                    # it: drawn at the template's own size (cursor00's ink 36 x 36, R03_bg_normal's parchment taller
                    # than any popup) and stretched to the placing dialog's box, as DSR's cursors cover their slot /
                    # row and its popups end at their box (ASSUMED from the layouts, TECH_DEBT menu-3)
                    cx = cy = 1.0
                    if names[ci].startswith(("cursor", "R03_bg")):
                        ext = [struct.unpack_from("<4h", v[1]) for v in dlgs[names[ci]].values()]
                        ew = max((e[2] for e in ext), default=0)
                        eh = max((e[3] for e in ext), default=0)
                        if ew > 0 and eh > 0:
                            cx, cy = (x1 - x0) / ew, (y1 - y0) / eh
                    walk(names[ci], x0, y0, h, depth + 1, cx, cy)
            nodes[k][8] = len(nodes)

    for r in roots:
        k = len(nodes)
        root_idx.append(k)
        nodes.append([r, "Dialog", b"\0" * 48, 0, 0, 1280, 720, False, k + 1])
        walk(r, 0, 0, False, 0)
        nodes[k][8] = len(nodes)
    return nodes, root_idx


class Skyline:
    """Bottom-left skyline packer (tighter than export_menu.Packer's shelves when tall thin pieces mix with wide ones)"""
    def __init__(self, w, h):
        self.w, self.h, self.sky = w, h, [0] * w

    def add(self, w, h):
        w1, h1 = w + 1, h + 1  # 1-texel gap: bilinear filtering must not pull in a neighbour
        best = None
        x = 0
        while x + w1 <= self.w:
            y = max(self.sky[x:x + w1])
            if y + h1 <= self.h + 1 and (best is None or y < best[1]):
                best = (x, y)
            x += 1 if best is None or y >= best[1] else 1
        if best is None:
            raise ValueError(f"atlas full at {w}x{h}")
        x, y = best
        for i in range(x, x + w1):
            self.sky[i] = y + h1
        return x, y

    def used(self):
        return max(self.sky)


def fit_sizes(want, budget):
    """{key: (w, h)} -> stored sizes, largest pieces halved until the area (with packing slack) fits budget px"""
    size = dict(want)
    while sum((w + 1) * (h + 1) for w, h in size.values()) * 1.12 > budget:
        k = max(size, key=lambda k: size[k][0] * size[k][1])
        w, h = size[k]
        if w * h <= 64:
            raise ValueError("screen atlas cannot fit")
        size[k] = (max(2, w // 2), max(2, h // 2))
    return size


def orient_sprite(img, o):
    """Sprite +26 flags >> 4: bits 0-1 quarter turns clockwise, 2 (0x40) mirror left-right, 3 (0x80) mirror top-bottom,
    after the turn. The I01 / I02 status frame (R02_status_frame_r) draws its top and bottom edges from upright strips
    with flags 0x1b0, its sides from flat ones with 0x110: three turns and a top-bottom mirror put the corner curls
    inward as DSR shows them (reference capture 2026-11-03); clockwise and the mirror axes are ASSUMED (a
    counter-clockwise turn with a left-right mirror looks the same on those strips), TECH_DEBT menu-7"""
    from PIL import Image
    for _ in range(o & 3):
        img = img.transpose(Image.ROTATE_270)  # PIL: 270 degrees counter-clockwise = a quarter turn clockwise
    if o & 4:
        img = img.transpose(Image.FLIP_LEFT_RIGHT)
    if o & 8:
        img = img.transpose(Image.FLIP_TOP_BOTTOM)
    return img


def export(game, name, roots, kb, dlgs, tex_names, texs, fmgs, out_dir, preview_dir=None):
    from PIL import Image
    names = list(dlgs)
    nodes, root_idx = flatten(dlgs, names, roots)
    # sprites: one per (texture, texel rect), at the largest size drawn
    want, spr_key = {}, {}
    for k, (el, t, raw, x0, y0, x1, y1, h, end) in enumerate(nodes):
        if t != "Sprite":
            continue
        tl, tt, tr, tb, ti = struct.unpack_from("<5h", raw, 16)
        if not (0 <= ti < len(tex_names)) or tr <= tl or tb <= tt or tex_names[ti].startswith(("Icon", "SYSTEX")):
            continue
        orient = struct.unpack_from("<H", raw, 26)[0] >> 4 & 15  # Sprite +26 flags 0x10-0x80 (orient_sprite)
        key = (tex_names[ti], max(0, tl), max(0, tt), tr, tb, orient)
        w = max(1, round((x1 - x0) * SX))
        hh = max(1, round((y1 - y0) * SY))
        tw, th = (tb - max(0, tt), tr - max(0, tl)) if orient & 1 else (tr - max(0, tl), tb - max(0, tt))
        w, hh = min(w, tw), min(hh, th)
        ow, oh = want.get(key, (0, 0))
        want[key] = (max(ow, w), max(oh, hh))
        spr_key[k] = key
    aw = 512
    ah = max(32, min(512, (kb * 1024) // aw))
    budget = aw * ah
    while True:  # the shelf packer wastes space: shrink the budget until everything packs
        size = fit_sizes(want, budget)
        atlas = Image.new("RGBA", (aw, ah), (0, 0, 0, 0))
        pk = Skyline(aw, ah)
        keys = sorted(size, key=lambda k: (-size[k][1], -size[k][0], k))
        sprites, sprite_of = [None] * len(keys), {}
        try:
            for key in sorted(size):
                sprite_of[key] = len(sprite_of)
            for key in keys:
                x, y = pk.add(*size[key])
                sprites[sprite_of[key]] = [x, y, size[key][0], size[key][1]]
            break
        except ValueError:
            budget = int(budget * 0.95)
    for key, (x, y, w, h) in zip(sorted(size), sprites):
        tn, l, t, r, b, orient = key
        img = orient_sprite(texs[tn].crop((l, t, r, b)), orient)
        atlas.alpha_composite(resample.resize(img, (w, h), Image.LANCZOS), (x, y))
    used_h = min(ah, pk.used())
    used_h = (used_h + 31) & ~31  # GS page-friendly height
    atlas = atlas.crop((0, 0, aw, used_h))
    clut, pix = em.quantize(atlas)

    strings, string_of = [], {}

    def string(s):
        if s not in string_of:
            string_of[s] = len(strings)
            strings.append(s)
        return string_of[s]

    body, grads = bytearray(), []
    for k, (el, t, raw, x0, y0, x1, y1, h, end) in enumerate(nodes):
        typ = {"Dialog": T_DIALOG, "Sprite": T_SPRITE, "MonoRect": T_RECT, "GouraudRect": T_RECT,
               "Text": T_TEXT, "ScrollText": T_TEXT}.get(t, T_NULL)
        spr, font, align, col = -1, 0, 0, (0x80, 0x80, 0x80, 0x80)
        if typ == T_SPRITE:
            if k in spr_key:
                spr = sprite_of[spr_key[k]]
                # Sprite +32: tint a, b, g, r as MonoRect's (ffffffff on most sprites; c8ffffff translucent rules,
                # dc282828 the tab blackouts, ff000000 black panels, 96062a76 the list cursors' ink: r 0x76, g 0x2a,
                # b 0x06, the orange-brown DSR shows under the selected row (reference capture 2026-11-03; read as
                # a, r, g, b it was blue). ff0000ff F20 text_intrusion is red that way, as DSR's invasion text)
                a, b, g, r = raw[32:36]
                # Sprite +28: u32 MenuColorTableParam row that replaces the tint (0: none): 2 "inventory selected
                # icon colour" on the I01 tab highlights, 16 "scroll bar" on the list thumbs (DSR's are orange-brown,
                # the tint ff0000a4 is red), 20 "blackout", 1 "normal text" on I02's count rules (ASSUMED meaning,
                # from those names and the capture)
                row = MENU_COLORS.get(struct.unpack_from("<I", raw, 28)[0])
                if row:
                    r, g, b, a = row
                col = em.ui_rgb(r, g, b) + ((a * 0x80 + 127) // 255,)
            else:
                typ = T_NULL  # an icon placeholder (texture -1 or an IconNN sheet): filled in by the code
        elif typ == T_RECT:
            at = 20 if t == "GouraudRect" else 24
            cs = [raw[at + 4 * j:at + 4 + 4 * j] for j in range(4 if t == "GouraudRect" else 1)]
            a, b, g, r = (sum(c[j] for c in cs) // len(cs) for j in range(4))
            col = em.ui_rgb(r, g, b) + ((a * 0x80 + 127) // 255,)
            if len({bytes(c) for c in cs}) > 1:
                spr = len(grads)
                grads.append(b"".join(bytes(em.ui_rgb(c[3], c[2], c[1]) + ((c[0] * 0x80 + 127) // 255,)) for c in cs))
        elif typ == T_TEXT:
            a = raw[24]  # a, b, g, r as the shapes'; r, g, b: white (above)
            col = (255, 255, 255, (a * 0x80 + 127) // 255)
            font, align = raw[28], raw[30]
            fid, tid = struct.unpack_from("<2I", raw, 36)
            s = fmgs.get(fid, {}).get(tid)
            spr = string(ascii_text(s)) if s else -1
        flags = 1 if h else 0
        body += struct.pack("<4hBBHhBB4BI", round(x0 * SX), round(y0 * SY), round(x1 * SX), round(y1 * SY), typ,
                            align, end, spr, font, flags, *col, fnv(el))
    assert len(body) == 24 * len(nodes)

    HDR = 64
    blob = bytearray(b"\0" * HDR)

    def align_to(n):
        blob.extend(b"\0" * ((-len(blob)) % n))
    node_off = len(blob)
    blob += body
    root_off = len(blob)
    blob += struct.pack(f"<{len(root_idx)}I", *root_idx)
    str_off = len(blob)
    sb = bytearray()
    so = []
    for s in strings:
        so.append(len(sb))
        sb += s.encode("ascii", "replace") + b"\0"
    blob += struct.pack(f"<{len(so)}I", *[str_off + 4 * len(so) + o for o in so]) + sb
    align_to(4)
    spr_off = len(blob)
    for s in sprites:
        blob += struct.pack("<4H", *s)
    grad_off = len(blob)
    for g in grads:
        blob += g
    align_to(128)
    clut_off = len(blob)
    blob += clut
    align_to(128)
    pix_off = len(blob)
    blob += pix
    struct.pack_into("<4s7I2H6I", blob, 0, b"DSCR", VERSION, len(nodes), node_off, len(root_idx), root_off,
                     len(strings), str_off, aw, used_h, clut_off, pix_off, len(sprites), spr_off, len(grads), grad_off)
    with open(os.path.join(out_dir, f"scr_{name}.bin"), "wb") as f:
        f.write(blob)
    vram = len(clut) + len(pix)
    print(f"scr_{name}: {len(nodes)} nodes, {len(sprites)} sprites, {len(grads)} gradients, atlas {aw}x{used_h} ({vram // 1024} KB VRAM), "
          f"{len(strings)} strings, {len(blob)} B")
    if preview_dir:
        atlas.save(os.path.join(preview_dir, f"scr_{name}_atlas.png"))
    return vram


def main():
    game, out = sys.argv[1], sys.argv[2]
    preview = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(out, exist_ok=True)
    drb_bytes = souls.read(os.path.join(game, "menu", "menu.drb.dcx"))
    dlgs, tex_names = em.drb_elements(drb_bytes)
    # every texture any screen sprite needs
    needed = set()
    names = list(dlgs)
    for roots, _ in SCREENS.values():
        nodes, _r = flatten(dlgs, names, roots)
        for el, t, raw, *_ in nodes:
            if t == "Sprite":
                ti = struct.unpack_from("<h", raw, 24)[0]
                if 0 <= ti < len(tex_names):
                    needed.add(tex_names[ti])
    texs = em.load_textures(game, {n for n in needed if not n.startswith(("Icon", "SYSTEX"))})
    MENU_COLORS.update({k: (v["r"], v["g"], v["b"], v["a"]) for k, (_, v) in
                        souls.game_params(game)["MenuColorTableParam"].items() if k})
    fmgs = {}
    for i, n, d in souls.bnd3(souls.read(os.path.join(game, "msg", "ENGLISH", "menu.msgbnd.dcx"))):
        fmgs[i] = souls.fmg(d)
    for name, (roots, kb) in SCREENS.items():
        export(game, name, roots, kb, dlgs, tex_names, texs, fmgs, out, preview)


if __name__ == "__main__":
    main()
