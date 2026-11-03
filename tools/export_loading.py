"""Export DSR's loading screen ("NowLoading") for the PS2 prototype (src/loading.c).

What DSR draws (exe, DarkSoulsRemastered.exe; notes/PORTING.md "Loading screen"): the common menu ESD e00-00 opens
dialog E00_00 of menu/menu.drb as FrpgMenuDlgNowLoading while the menu manager's loading state is on
(FUN_14071b6a0 begin, FUN_14071b790 end). Its update FUN_1406fec50 steps two counters 0..99 that pick the soul_<n>
elements whose sprite icon_soul1 / icon_soul2 copy (FUN_1406fef40: the soul_sequence bonfire animation, 100 steps over
the 32 cells of menu/nowloading.tpf; icon_soul2 sits off screen), and shows a loading hint: an item from the 62-entry id
table at 0x141a327d0 (.data), its name and long description in the <?loadHintName?> / <?loadHintCaption?> texts and
its icon in the "icon" element (FUN_1406ff1c0; EquipParam*.iconId). The hint starts at a random entry
(FUN_1406fea10) and moves to the next every 10 s (+0x22c = 10.0 in the constructor FUN_1406fe8c0).

Layout: E00_00 flattened (tools/export_screens.py flatten), 1280x720 -> 640x448. Sprites are cut from the menu TPFs
and nowloading.tpf and stored at frame-buffer size (512 x 448: src/render.h R_HX), so the atlas holds no texel the GS
cannot show; the 32 bonfire cells use their own CLUT (CLUT 1), everything else CLUT 0. The ASCII font is DSFont24 as
the HUD's (tools/export_menu.py font). The hint table is read from the user's exe (tools/verify_game.py checks its
build), names and descriptions from msg/ENGLISH/item.msgbnd (the *_name_ / *_long_desc_ FMGs), icon ids from the
EquipParam banks: the icon pixels are icons.bin's (tools/export_items.py), read at run time.

Output loading.bin (little-endian, version 1):
  header  'DSLD', u32 version, u16 atlas w, h, u32 clut offset (2 x 1 KB CSM1 CLUTs, alpha 0..0x80), u32 pixel
          offset (PSMT8), u32 sprites, sprite offset, u32 nodes, node offset, u32 glyph offset (95 glyphs, ASCII
          32..126), u16 font line height, u16 text line height (the caption's rule spacing), u32 anim steps, anim
          offset, u32 hints, hint offset, u32 string bytes, string offset; padded to 80
  sprite  {u16 u, v, w, h; u8 clut; u8 0, u16 0}  12 B
  node    {s16 x0, y0, x1, y1 (640x448); u8 kind; u8 align; s16 sprite; u8 r, g, b, a (a 0..0x80)}  16 B, in the
          DRB's draw order. kind 0 sprite, 1 rect, 2 hint name text, 3 hint caption text, 4 hint icon box,
          5 the animated bonfire (icon_soul1: sprite = the animation's first sprite)
  glyph   {u16 u, v, w, h; s16 pre-space, advance}  12 B (CLUT 0)
  anim    u16 sprite per step (100: soul_0..soul_99, the cell each step shows)
  hint    {u32 item id; u16 icon id; u16 0; u32 name offset, u32 caption offset (into strings)}  16 B
  strings NUL-terminated ASCII
Usage: export_loading.py <game dir> <out.bin> [preview.png]"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import resample
import export_menu as em
import export_screens as es
import export_items

VERSION = 1
HDR = 80  # header bytes (68 used)
BX, BY = 512 / 1280, 448 / 720  # frame-buffer pixels per DSR 720p pixel
HINTS_VA, HINTS_N = 0x141A327D0, 62  # FUN_1406fec50: (idx + 1) % 0x3e over the u32 table at this .data address
K_SPRITE, K_RECT, K_NAME, K_CAPTION, K_ICON, K_ANIM = range(6)
CATS = {0x00000000: "Weapon", 0x10000000: "Armor", 0x20000000: "Accessory", 0x40000000: "Item"}
PARAM = {"Weapon": ("EquipParamWeapon", "iconId"), "Armor": ("EquipParamProtector", "iconIdM"),
         "Accessory": ("EquipParamAccessory", "iconId"), "Item": ("EquipParamGoods", "iconId")}


def exe_u32s(game, va, n):
    """n u32 at virtual address va of the exe's file image (PE sections)"""
    with open(os.path.join(game, "DarkSoulsRemastered.exe"), "rb") as f:
        b = f.read(4096)  # headers only (the browser converter reads files in place)
        pe = struct.unpack_from("<I", b, 0x3C)[0]
        nsec, opt = struct.unpack_from("<H", b, pe + 6)[0], struct.unpack_from("<H", b, pe + 20)[0]
        base = struct.unpack_from("<Q", b, pe + 24 + 24)[0]
        for i in range(nsec):
            _, vsize, vaddr, rsize, raw = struct.unpack_from("<8sIIII", b, pe + 24 + opt + 40 * i)
            if vaddr <= va - base < vaddr + min(vsize, rsize):
                f.seek(raw + va - base - vaddr)
                return struct.unpack(f"<{n}I", f.read(4 * n))
    raise ValueError(f"{va:#x} not in the exe's file image")


def hints(game):
    """[(item id, icon id, name, caption)] in the exe's table order"""
    P = souls.game_params(game)
    msg = {souls.basename(n): souls.fmg(d) for _, n, d in
           souls.bnd3(souls.read(os.path.join(game, "msg", "ENGLISH", "item.msgbnd.dcx")))}
    out = []
    for iid in exe_u32s(game, HINTS_VA, HINTS_N):
        kind = CATS[iid & 0xF0000000]
        row = iid & 0x0FFFFFFF
        bank, field = PARAM[kind]
        p = P[bank].get(row)
        icon = p[1][field] if p else -1
        name = msg[f"{kind}_name_.fmg"].get(row) or ""
        cap = msg[f"{kind}_long_desc_.fmg"].get(row) or ""
        out.append((iid, icon if 0 <= icon < 0xFFFF else 0xFFFF, export_items.ascii_text(name),
                    export_items.ascii_text(cap)))
    return out


def rule_period(img):
    """spacing in texel rows of the ruled lines in a note texture crop (autocorrelation of the per-row ink)"""
    import numpy as np
    a = np.asarray(img.convert("RGBA")).astype(float)
    ink = (a[..., 3] * a[..., :3].mean(2)).mean(1)
    ink -= ink.mean()
    c = [float((ink[:len(ink) - p] * ink[p:]).mean()) for p in range(len(ink) // 3)]
    top = max(c[8:])
    for p in range(9, len(c) - 1):  # the first strong peak (multiples of the period correlate as well)
        if c[p] >= 0.6 * top and c[p] >= c[p - 1] and c[p] >= c[p + 1]:
            return p
    return 0


def main():
    from PIL import Image
    game, out = sys.argv[1], sys.argv[2]
    preview = sys.argv[3] if len(sys.argv) > 3 else None
    dlgs, tex_names = em.drb_elements(souls.read(os.path.join(game, "menu", "menu.drb.dcx")))
    nodes, _ = es.flatten(dlgs, list(dlgs), ["E00_00"])
    by_name = {n[0]: n for n in nodes}
    texs = em.load_textures(game, {n for n in tex_names if n.startswith("Menu")})
    for n, d in souls.tpf(souls.read(os.path.join(game, "menu", "nowloading.tpf.dcx"))).items():
        w, h, px = souls.dds_decode_rgba(d, 4096)
        texs[n] = Image.frombytes("RGBA", (w, h), px)

    # sprites: one per (texture, texel rect) at its largest frame-buffer size; the bonfire cells at icon_soul1's size
    soul = by_name["icon_soul1"]
    soul_wh = (round((soul[5] - soul[3]) * BX), round((soul[6] - soul[4]) * BY))
    want, key_of = {}, {}
    for k, (el, t, raw, x0, y0, x1, y1, hid, end) in enumerate(nodes):
        if t != "Sprite":
            continue
        tl, tt, tr, tb, ti = struct.unpack_from("<5h", raw, 16)
        if not (0 <= ti < len(tex_names)) or tex_names[ti] not in texs or tr <= tl:
            continue
        key = (tex_names[ti], max(0, tl), max(0, tt), tr, tb)
        anim = tex_names[ti] == "soul_sequence"
        if hid and not anim:
            continue
        w, h = soul_wh if anim else (max(1, round((x1 - x0) * BX)), max(1, round((y1 - y0) * BY)))
        ow, oh = want.get(key, (0, 0))
        want[key] = (max(ow, min(w, tr - tl)), max(oh, min(h, tb - tt)))
        key_of[k] = key
    glyph_atlas = Image.new("RGBA", (512, 128), (0, 0, 0, 0))
    gp = em.Packer(512, 128)
    glyphs = em.font(game, glyph_atlas, gp)
    gp.place_all(glyph_atlas)
    gh = max(g[1] + g[3] for g in glyphs) + 1
    # pack: the bonfire cells first (one CLUT), then the panel pieces and the font (the other)
    keys = sorted(want)
    anim_keys = [k for k in keys if k[0] == "soul_sequence"]
    other = [k for k in keys if k[0] != "soul_sequence"]
    aw = 512
    sky = es.Skyline(aw, 2048)
    place = {}
    for key in anim_keys + sorted(other, key=lambda k: (-want[k][1], -want[k][0], k)):
        place[key] = sky.add(*want[key])
    anim_h = max(place[k][1] + want[k][1] + 1 for k in anim_keys)
    # the font block below everything (its own rows), then the atlas height rounded to a GS page
    fy = sky.used()
    ah = (fy + gh + 31) & ~31
    atlas = [Image.new("RGBA", (aw, ah), (0, 0, 0, 0)) for _ in range(2)]
    for key in keys:
        tn, l, t, r, b = key
        x, y = place[key]
        atlas[1 if tn == "soul_sequence" else 0].alpha_composite(
            resample.resize(texs[tn].crop((l, t, r, b)), want[key], Image.LANCZOS), (x, y))
    atlas[0].alpha_composite(glyph_atlas.crop((0, 0, 512, gh)), (0, fy))
    # one PSMT8 image, two CLUTs: the animation's rows use CLUT 1, the rest CLUT 0
    clut0, pix0 = em.quantize(atlas[0])
    clut1, pix1 = em.quantize(atlas[1].crop((0, 0, aw, anim_h)))
    pix = bytearray(pix0)
    for y in range(anim_h):
        for x0, x1 in [(place[k][0], place[k][0] + want[k][0]) for k in anim_keys
                       if place[k][1] <= y < place[k][1] + want[k][1]]:
            pix[y * aw + x0:y * aw + x1] = pix1[y * aw + x0:y * aw + x1]
    sprites, sprite_of = [], {}
    for key in keys:
        sprite_of[key] = len(sprites)
        sprites.append((*place[key], *want[key], 1 if key[0] == "soul_sequence" else 0))

    # draw list: the visible nodes in DRB order; the hint texts / icon box by their tags, the bonfire as one node
    tags = {"<?loadHintName?>": K_NAME, "<?loadHintCaption?>": K_CAPTION}
    fm = {}
    for i, n, d in souls.bnd3(souls.read(os.path.join(game, "msg", "ENGLISH", "menu.msgbnd.dcx"))):
        fm.setdefault(i, souls.fmg(d))
    body = []
    caption_box = None
    for k, (el, t, raw, x0, y0, x1, y1, hid, end) in enumerate(nodes):
        if hid:
            continue
        r = (round(x0 * es.SX), round(y0 * es.SY), round(x1 * es.SX), round(y1 * es.SY))
        if t == "Sprite" and el == "icon_soul1":
            body.append((*r, K_ANIM, 0, sprite_of[key_of[k]], 255, 255, 255, 0x80))
        elif t == "Sprite" and k in key_of:
            a, rr, g, b = raw[32:36]
            body.append((*r, K_SPRITE, 0, sprite_of[key_of[k]], rr, g, b, (a * 0x80 + 127) // 255))
        elif t == "Sprite" and el == "icon":
            body.append((*r, K_ICON, 0, -1, 255, 255, 255, 0x80))
        elif t in ("MonoRect", "GouraudRect"):
            at = 20 if t == "GouraudRect" else 24
            cs = [raw[at + 4 * j:at + 4 + 4 * j] for j in range(4 if t == "GouraudRect" else 1)]
            a, rr, g, b = (sum(c[j] for c in cs) // len(cs) for j in range(4))
            body.append((*r, K_RECT, 0, -1, rr, g, b, (a * 0x80 + 127) // 255))
        elif t in ("Text", "ScrollText"):
            fid, tid = struct.unpack_from("<2I", raw, 36)
            kind = tags.get(fm.get(fid, {}).get(tid))
            if kind is None:
                continue
            a, rr, g, b = raw[24:28]
            body.append((*r, kind, raw[30], -1, rr, g, b, (a * 0x80 + 127) // 255))
            if kind == K_CAPTION:
                caption_box = (x0, y0, x1, y1)
    # the caption's line spacing: the rule period of the note sprite under it
    line_h = em.FONT_H
    for k, n in enumerate(nodes):
        el, t, raw, x0, y0, x1, y1 = n[:7]
        if k in key_of and caption_box and x0 < caption_box[2] and caption_box[0] < x1 and y0 < caption_box[3] and \
                caption_box[1] < y1 and (y1 - y0) > 4 * em.FONT_H / es.SY and key_of[k][0] != "soul_sequence":
            tn, l, tt, rr, b = key_of[k]
            p = rule_period(texs[tn].crop((l, tt, rr, b)))
            if p:
                line_h = p * (y1 - y0) / (b - tt) * es.SY
    anim = []
    for i in range(100):
        anim.append(sprite_of[key_of[nodes.index(by_name[f"soul_{i}"])]])

    hs = hints(game)
    sb, hint_rows = bytearray(), []
    for iid, icon, name, cap in hs:
        no = len(sb)
        sb += name + b"\0"
        co = len(sb)
        sb += cap + b"\0"
        hint_rows.append((iid, icon, no, co))

    blob = bytearray(HDR)

    def align(n):
        blob.extend(b"\0" * ((-len(blob)) % n))
    spr_off = len(blob)
    for s in sprites:
        blob += struct.pack("<4HBBH", *s, 0, 0)
    node_off = len(blob)
    for n in body:
        blob += struct.pack("<4hBBh4B", *n)
    align(4)
    glyph_off = len(blob)
    for u, v, w, h, pre, adv in glyphs:
        blob += struct.pack("<4H2h", u, v + fy, w, h, pre, adv)
    anim_off = len(blob)
    blob += struct.pack(f"<{len(anim)}H", *anim)
    align(4)
    hint_off = len(blob)
    for h in hint_rows:
        blob += struct.pack("<IHHII", h[0], h[1], 0, h[2], h[3])
    str_off = len(blob)
    blob += sb
    align(128)
    clut_off = len(blob)
    blob += clut0 + clut1
    pix_off = len(blob)
    blob += pix
    struct.pack_into("<4sI2H7I2H6I", blob, 0, b"DSLD", VERSION, aw, ah, clut_off, pix_off, len(sprites), spr_off,
                     len(body), node_off, glyph_off, em.FONT_H, round(line_h), len(anim), anim_off,
                     len(hint_rows), hint_off, len(sb), str_off)
    with open(out, "wb") as f:
        f.write(blob)
    print(f"loading: {len(body)} nodes, {len(sprites)} sprites ({len(anim_keys)} bonfire cells {want[anim_keys[0]]}), "
          f"atlas {aw}x{ah} ({(aw * ah + 2048) // 1024} KB VRAM), {len(hint_rows)} hints, line {line_h:.1f} px, "
          f"{len(blob)} B")
    if preview:
        pv = Image.new("RGBA", (aw, ah), (40, 0, 40, 255))
        pv.alpha_composite(atlas[0])
        pv.alpha_composite(atlas[1])
        pv.save(preview)


if __name__ == "__main__":
    main()
