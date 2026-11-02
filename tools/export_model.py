"""Export the Warrior's real model and animations for the PS2 prototype.

Mesh: Hard Leather set (EquipParamProtector 210000-213000, equipModelId 9360) + Longsword (WP_A_0201), FLVER LOD2 face
sets, skinned to the c0000 Havok skeleton with the two strongest bone weights. Colour per mesh = average of its DXT1
diffuse texture. Animations: hkaSplineCompressedAnimation decoded and baked at 30 fps, local bone transforms.

Textures: each mesh's DXT1 diffuse, largest mip <= 256 px (width >= 128 for PSMT8 uploads), quantized to 256
colours; the palette is stored in the GS's CSM1 order (entries 8-15 and 16-23 of each 32 swapped).

Output (little-endian, read in place by ps2/proto/src/model.c; texture data 128-byte aligned for DMA):
  header  u32 magic DSRM, u32 bones, verts, tris, anims, textures, groups,
          u32 offsets: parents, ref, verts, tris, anims, textures, groups, uvs (60 B)
  parents s16[bones] (padded to 4)       ref  bone local (q s16x4 /32767, t s16x3 /4096, pad) = 16 B each
  verts   {f32 pa[3]; f32 pb[3]; s8 na[3]; u8 ba, bb, wa; u8 r, g, b; u8 metal, specular weight; pad} = 36 B
          (pa/pb/na in bone space; metal / weight from the _s map, 0 = no specular: see SPEC_RES)
  tris    u16[3]
  anims   per anim: u32 id, u32 frames, then frames*bones*16 B local transforms
  textures {u32 w, h, clut offset, pixel offset}; clut u32[256] RGBA; pixels u8[w*h]
  groups  {s32 texture (-1 = untextured), u32 first tri, u32 tri count, u32 flags (bit 0 cull back faces, bit 1
          helmet mesh, bit 2 hidden while the helmet is worn: see HIDE_*; bits 28-31 specular exponent k, 2^k)};
          tris are sorted by group
  uvs     f32 [tris][3][2] in texels, shifted per triangle so they are >= 0 (textures repeat)
Usage: export_model.py <game dir> <out.bin> [preview.png]"""
import math, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import havok_anim as ha
import resample  # Image.resize with a fixed pass order (TECH_DEBT build-7)

PARTS = ["HD_M_9360", "BD_M_9360", "AM_M_9360", "LG_M_9360"]
WEAPON, WEAPON_BONE = "WP_A_0201", "R_Weapon"
SKIN = (176, 132, 108)  # flat colour for a mesh whose diffuse is found nowhere (none in the Warrior's set)
# Bare-skin meshes (material "体", Ps_Body[DSB].mtd) reference BD_M_body / FC_M_face, which are not in the parts' own
# TPFs but in the loose shared parts/Common_Body.tpf (with their _n/_s/_t maps; _t is plain white)
COMMON_TPF = "Common_Body.tpf"
LOD2 = 0x02000000
# Head under the helmet. DSR builds the player's head from FC_M_0000 (back of head and neck, FC_M_face), the FaceGen
# face FG_A_0000 (face + eyes; its diffuse FG_face_gen is generated at runtime from facegen/FaceGen.fgbnd, see FACE_TEX)
# and a hair part HR_M_<FaceGenParam hairStyle>. FaceGenParam 30 "４末版　戦士_男" is the Warrior-male preset (the
# player's CharaInitParam 3000 has npcPlayerFaceGenId -1: its face comes from the character creator).
FACE_ROW = 30
CLASS_ROW = 3000  # CharaInitParam: the Warrior's starting equipment (helm / armour / gauntlets / leggings)
# Neutral face: no FaceGen morphs (face.egm) or texture modes (face.egt); the FaceGen mesh's second UV set with its
# detail diffuse FG_m_face_org_d stands in for the generated FG_face_gen
FACE_TEX = "FG_m_face_org_d"
HEAD_LOD = {"FG_A_0000": 0x01000000}  # face at LOD1 (639 + 337 tris; LOD2 loses the lips and eyelids); others LOD2
HEAD_TEX_MAX = {"FC_M_face": 128, "FG_eye": 128}  # back of head / eyes: small on screen, VRAM
# Group flag bits for DSR's hide rule: a mesh whose material starts with #NN# is hidden while a worn protector has
# EquipParamProtector invisibleFlagNN. The armour / gauntlets / leggings are fixed, so their flags drop meshes here;
# the helmet can come off at runtime (model.c hide mask)
HIDE_HELMET = 2      # the helmet's own meshes: hidden while it is off
HIDE_UNDER_HELM = 4  # #NN# meshes the helmet's invisibleFlags hide
HDR = 60  # header bytes: magic + 6 counts + 8 section offsets
# Specular (TECH_DEBT map-29, VU side vu1skin): DSR's _s maps are PBR: R roughness, G metalness, B F0 code
# (FRPG_FlverPBL: Mtl.Roughness = pbl.r, MetalMask = pbl.g). Per vertex u8 metal m and specular weight s (1 where the
# mesh has an _s map; the VU applies f = s m + 0.04 s^2 (1 - m)); per group the Blinn exponent 2^k fitted to the
# mean roughness r (e = 2 / r^4 - 2, k 1..5) in flag bits 28-31 (0 = no _s map, VU default 1)
SPEC_RES = 32  # _s mip sampled (px)
GROUP_SPEC_K_SHIFT = 28  # bits 8-23: export_chr's #NN# display mask


def spec_k(rough):
    e = max(2.0, min(32.0, 2 / max(0.05, rough) ** 4 - 2))
    return max(1, min(5, round(math.log2(e))))


def spec_terms(sp, uv):
    """(metal, specular weight) u8 at uv: 3x3 texel average of the decoded _s map (w, h, RGBA)"""
    w, h, px = sp
    x, y = int((uv[0] % 1) * w), int((uv[1] % 1) * h)
    g = [px[4 * (((y + dy) % h) * w + (x + dx) % w) + 1] for dy in (-1, 0, 1) for dx in (-1, 0, 1)]
    return round(sum(g) / 9), 255
TEX_MAX = 256
# (TAE file prefix, anim id, archive): the same list and order as export_player.py's ANIMS, plus idle first
ANIMS = [("a00", 0, "c0000_a00_lo"), ("a00", 200, "c0000_a00_lo"), ("a00", 500, "c0000_a00_lo"),
         ("a00", 535, "c0000_a00_lo"), ("a00", 700, "c0000_a00_lo"), ("a23", 3000, "c0000_a2x"),
         ("a23", 3001, "c0000_a2x"), ("a23", 3002, "c0000_a2x"), ("a00", 680, "c0000_a00_lo"),
         ("a00", 900, "c0000_a00_lo"), ("a00", 6065, "c0000_a00_md"), ("a00", 6066, "c0000_a00_md"),
         ("a00", 6067, "c0000_a00_md"), ("a00", 1550, "c0000_a00_lo"), ("a00", 1500, "c0000_a00_lo"),
         ("a00", 1750, "c0000_a00_lo"), ("a00", 1755, "c0000_a00_lo"), ("a00", 1760, "c0000_a00_lo"),
         ("a00", 6070, "c0000_a00_md"),
         ("a00", 6170, "c0000_a00_md"), ("a00", 6073, "c0000_a00_md"), ("a00", 6173, "c0000_a00_md"), ("a23", 3500, "c0000_a2x"), ("a23", 3900, "c0000_a2x"), ("a23", 3800, "c0000_a2x"), ("a23", 3801, "c0000_a2x"), ("a00", 2000, "c0000_a00_lo"), ("a00", 2001, "c0000_a00_lo"), ("a00", 2002, "c0000_a00_lo"), ("a00", 2003, "c0000_a00_lo"), ("a00", 2004, "c0000_a00_lo"), ("a00", 2005, "c0000_a00_lo"), ("a00", 2006, "c0000_a00_lo"), ("a00", 2007, "c0000_a00_lo"),
         ("a00", 100, "c0000_a00_lo"), ("a00", 101, "c0000_a00_lo"), ("a00", 106, "c0000_a00_lo"),
         ("a00", 116, "c0000_a00_lo")]
# Turn-lean pairs: DSR has no straight dash; a00_0535 leans into a left turn (head 0.20 m left of the feet, shoulders
# rolled -21 deg on average) and a00_0536 into a right turn, and the engine blends the two by turn rate
# (WalkAnim_Twist). Played alone, 0535 made the dash lean left. Baked here as the 50/50 blend = running straight.
TURN_PAIR = {535: 536}


def bake(files, prefix, aid, ref):
    nf, fps, t2b, frames = ha.animation(files[f"{prefix}_{aid:04d}.hkx"])
    baked = []
    for row in frames:
        pose = [(r[1], r[0]) for r in ref]  # (q, t) reference pose for bones without a track
        for track, bone in enumerate(t2b):
            if 0 <= bone < len(pose):
                t, q, _ = row[track]
                pose[bone] = (q, t)
        baked.append(pose)
    return nf, fps, baked


def blend_pose(a, b, k):
    """Per bone: nlerp of the rotations (shortest path), lerp of the translations"""
    out = []
    for (qa, ta), (qb, tb) in zip(a, b):
        s = 1 if sum(x * y for x, y in zip(qa, qb)) >= 0 else -1
        q = [qa[c] * (1 - k) + s * qb[c] * k for c in range(4)]
        l = math.sqrt(sum(c * c for c in q)) or 1.0
        out.append((tuple(c / l for c in q), tuple(ta[c] * (1 - k) + tb[c] * k for c in range(3))))
    return out


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def bind_world(fbones):
    out = []
    for name, parent, t, q, s in fbones:
        if parent < 0:
            out.append((t, q))
        else:
            pt, pq = out[parent]
            r = ha.qrot(pq, t)
            out.append(((pt[0] + r[0], pt[1] + r[1], pt[2] + r[2]), ha.qmul(pq, q)))
    return out


def to_local(bw, p):
    """World point -> bone space of bind transform bw"""
    t, q = bw
    return ha.qrot(qconj(q), (p[0] - t[0], p[1] - t[1], p[2] - t[2]))


class MeshBuilder:
    """FLVER meshes -> the model.bin vertex / triangle / texture lists (two strongest bone weights per vertex, bone-space
    positions and normals against the FLVER bind pose, per-triangle texel UVs shifted to stay >= 0)."""

    def __init__(self, sk_index):
        self.sk_index = sk_index
        self.verts, self.tris, self.tri_tex, self.tri_uv = [], [], [], []
        self.textures, self.tex_index = [], {}

    def texture(self, name, dds):
        if dds and name not in self.tex_index:
            dec = souls.dds_decode(dds, TEX_MAX)
            if dec:
                self.tex_index[name] = len(self.textures)
                self.textures.append(quantize(*dec))
        return self.tex_index.get(name, -1)

    def texture_rgba(self, name, w, h, rgba, cutout=False):
        """An already decoded RGBA image; cutout: alpha < 50% becomes the transparent CLUT index (alpha-tested)"""
        if name not in self.tex_index:
            self.tex_index[name] = len(self.textures)
            if cutout:
                from export_map import quantize_rgba
                self.textures.append(quantize_rgba(w, h, rgba))
            else:
                self.textures.append(quantize(w, h, bytes(rgba[i] for i in range(len(rgba)) if i % 4 != 3)))
        return self.tex_index[name]

    def skeleton_bone(self, F, fb):
        """Skeleton index of FLVER bone fb, walking up to the nearest ancestor the skeleton has (FLVERs carry extra
        dummy / nub bones)"""
        while fb >= 0:
            sk = self.sk_index.get(F["bones"][fb][0])
            if sk is not None:
                return sk, fb
            fb = F["bones"][fb][1]
        return 0, -1

    def add(self, F, bw, m, faceset, ti, col, rigid=None, uv=None, bits=0, spec=None):
        """rigid = (skeleton bone, flip): every vertex on that bone, flipped 180 deg about Z (DS1 right-hand weapons);
        uv: per-vertex UVs instead of the mesh's first set; bits: group flag bits (HIDE_*); spec: the mesh's _s map
        (DDS) for the specular terms (spec_terms)"""
        V = m["verts"]
        uvs = uv or V["uv"]
        sp = souls.dds_decode_rgba(spec, SPEC_RES) if spec else None
        if sp:  # Blinn exponent 2^k per group from the mesh's mean roughness (flag bits 28-31)
            n_px = sp[0] * sp[1]
            rough = sum(sp[2][i] for i in range(0, 4 * n_px, 4)) / n_px / 255
            bits |= spec_k(rough) << GROUP_SPEC_K_SHIFT
        remap = {}
        for tri in souls.triangles(faceset):
            out_tri = []
            for vi in tri:
                if vi not in remap:
                    p, n = V["pos"][vi], V["normal"][vi] if V["normal"] else (0, 1, 0)
                    if rigid:
                        if rigid[1]:
                            p, n = (-p[0], -p[1], p[2]), (-n[0], -n[1], n[2])
                        infl = [(rigid[0], 1.0)]
                        local = {rigid[0]: (p, n)}
                    else:
                        infl, fb_of = {}, {}
                        for bi, w in zip(V["bone_idx"][vi], V["bone_w"][vi]):
                            if w > 0:
                                sk, fb = self.skeleton_bone(F, m["bones"][bi])
                                infl[sk] = infl.get(sk, 0) + w
                                fb_of[sk] = fb
                        if not infl:  # unweighted: the mesh's default bone
                            sk, fb = self.skeleton_bone(F, m["default_bone"])
                            infl, fb_of = {sk: 1.0}, {sk: fb}
                        infl = sorted(infl.items(), key=lambda kv: -kv[1])[:2]
                        local = {}
                        for sk, _ in infl:
                            b = bw[fb_of[sk]] if fb_of[sk] >= 0 else ((0, 0, 0), (0, 0, 0, 1))
                            local[sk] = (to_local(b, p), ha.qrot(qconj(b[1]), n))
                    if len(infl) == 1:
                        infl = infl + [(infl[0][0], 0.0)]
                    tot = infl[0][1] + infl[1][1]
                    wa = int(round(255 * infl[0][1] / tot))
                    (ba, _), (bb, _) = infl
                    remap[vi] = len(self.verts)
                    ms = spec_terms(sp, uvs[vi]) if sp and uvs else (0, 0)
                    self.verts.append((local[ba][0], local[bb][0], local[ba][1], ba, bb, wa, col, ms))
                out_tri.append(remap[vi])
            self.tris.append(tuple(out_tri))
            self.tri_tex.append((ti, (1 if faceset[3] else 0) | bits))
            if ti >= 0:
                tw, th = self.textures[ti][0], self.textures[ti][1]  # (ti, cull) is the group key
                uv = [uvs[vi] for vi in tri]
                du, dv = math.floor(min(u for u, _ in uv)), math.floor(min(v for _, v in uv))
                self.tri_uv.append([((u - du) * tw, (v - dv) * th) for u, v in uv])
            else:
                self.tri_uv.append([(0, 0)] * 3)


def write_model(out, names, parents, ref, mb, anims):
    """model.bin (layout in the module docstring); anims = [(id, [pose per frame: [(q, t) per bone]])]"""
    verts, tris, tri_tex, tri_uv, textures = mb.verts, mb.tris, mb.tri_tex, mb.tri_uv, mb.textures

    def qt(q, t):
        return struct.pack("<4h3hh", *(int(round(max(-1, min(1, c)) * 32767)) for c in q),
                           *(int(round(max(-7.99, min(7.99, c)) * 4096)) for c in t), 0)

    blob = bytearray()
    sec = {}
    sec["parents"] = len(blob) + HDR
    blob += struct.pack(f"<{len(parents)}h", *parents) + b"\0" * (2 * (len(parents) % 2))
    sec["ref"] = len(blob) + HDR
    blob += b"".join(qt(r[1], r[0]) for r in ref)
    sec["verts"] = len(blob) + HDR
    for pa, pb, na, ba, bb, wa, col, ms in verts:  # normals clamped: c5350's FLVER has components just over 1
        blob += struct.pack("<6f3b8B1x", *pa, *pb, *(max(-127, min(127, int(round(c * 127)))) for c in na), ba, bb, wa, *col, *ms)
    order = sorted(range(len(tris)), key=lambda i: tri_tex[i])
    groups = []
    for n, i in enumerate(order):
        if not groups or (groups[-1][0], groups[-1][3]) != tri_tex[i]:
            groups.append([tri_tex[i][0], n, 0, tri_tex[i][1]])
        groups[-1][2] += 1
    sec["tris"] = len(blob) + HDR
    blob += b"".join(struct.pack("<3H", *tris[i]) for i in order) + b"\0" * (2 * (len(tris) * 3 % 2))
    sec["anims"] = len(blob) + HDR
    for aid, baked in anims:
        blob += struct.pack("<II", aid, len(baked)) + b"".join(qt(q, t) for pose in baked for q, t in pose)
    def align(n):
        blob.extend(b"\0" * ((-(len(blob) + HDR)) % n))
    align(16)
    sec["textures"] = len(blob) + HDR
    table = len(blob)
    blob += b"\0" * (16 * len(textures))
    for k, (tw, th, clut, pix) in enumerate(textures):
        align(128)
        c_off = len(blob) + HDR
        blob += clut
        align(128)
        p_off = len(blob) + HDR
        blob += pix
        struct.pack_into("<4I", blob, table + 16 * k, tw, th, c_off, p_off)
    align(16)
    sec["groups"] = len(blob) + HDR
    blob += b"".join(struct.pack("<iIII", *g) for g in groups)
    sec["uvs"] = len(blob) + HDR
    blob += b"".join(struct.pack("<6f", *(c for uv in tri_uv[i] for c in uv)) for i in order)
    header = struct.pack("<4s6I8I", b"DSRM", len(names), len(verts), len(tris), len(anims), len(textures),
                         len(groups), sec["parents"], sec["ref"], sec["verts"], sec["tris"], sec["anims"],
                         sec["textures"], sec["groups"], sec["uvs"])
    with open(out, "wb") as f:
        f.write(header + blob)
    print(f"{len(names)} bones, {len(verts)} verts, {len(tris)} tris, {len(anims)} anims, "
          f"{len(textures)} textures {[(t[0], t[1]) for t in textures]}, {len(groups)} groups, {len(header) + len(blob)} bytes")



def main():
    game, out = sys.argv[1], sys.argv[2]
    preview = sys.argv[3] if len(sys.argv) > 3 else None
    chr_dir, parts_dir = os.path.join(game, "chr"), os.path.join(game, "parts")
    anibnd = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(f"{chr_dir}/c0000.anibnd.dcx"))}
    names, parents, ref = ha.skeleton(anibnd["Skeleton.hkx"])
    sk_index = {n: i for i, n in enumerate(names)}

    mb = MeshBuilder(sk_index)
    common = souls.tpf(souls.read(f"{parts_dir}/{COMMON_TPF}"))
    params = souls.game_params(game)
    cls = params["CharaInitParam"][CLASS_ROW][1]
    hidden = {}
    for slot in ("Helm", "Armer", "Gaunt", "Leg"):
        row = params["EquipParamProtector"][cls[f"equip_{slot}"]][1]
        hidden[slot] = {i for i in range(48) if row[f"invisibleFlag{i:02d}"]}
    always = hidden["Armer"] | hidden["Gaunt"] | hidden["Leg"]

    def hide_bits(part, m):
        """None = dropped (a fixed piece hides it), else the HIDE_* bits of the mesh"""
        nn = re.match(r"#(\d+)#", m["material"])
        nn = int(nn.group(1)) if nn else -1
        if nn in always:
            return None
        return (HIDE_HELMET if part.startswith("HD_") else 0) | (HIDE_UNDER_HELM if nn in hidden["Helm"] else 0)

    print(f"hide flags: helm {sorted(hidden['Helm'])}, armour/gauntlets/leggings {sorted(always)}")
    for part in PARTS + [WEAPON]:
        files = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(f"{parts_dir}/{part}.partsbnd.dcx"))}
        F = souls.flver(files[f"{part}.flver"])
        tex = souls.tpf(files[f"{part}.tpf"])
        bw = bind_world(F["bones"])
        for m in F["meshes"]:
            fs = [f for f in m["facesets"] if f[0] == LOD2]
            bits = hide_bits(part, m)
            if not fs or bits is None:
                if fs:
                    print(f"{part} {m['material']}: hidden by the worn armour")
                continue
            d = tex.get(m["diffuse"]) or common.get(m["diffuse"])
            col = (souls.dds_average(d) if d else None) or SKIN
            ti = mb.texture(m["diffuse"], d)
            spec = tex.get(m["diffuse"] + "_s") or common.get(m["diffuse"] + "_s")
            if part == WEAPON:
                col = tuple(min(255, int(c * 2.2)) for c in col)  # the averaged blade reads too dark unlit
            # DS1/DSR right-hand weapons are flipped backwards (180 deg about X) and sideways (about Y), i.e. 180 deg
            # about Z: DSAnimStudio NewChrAsm.cs, isOldGame -> rback/rside
            mb.add(F, bw, m, fs[0], ti, col, (sk_index[WEAPON_BONE], True) if part == WEAPON else None, bits=bits,
                   spec=spec)
    add_head(game, parts_dir, params, mb, common, hide_bits)

    anims = []
    for prefix, aid, archive in ANIMS:
        files = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(f"{chr_dir}/{archive}.anibnd.dcx"))}
        nf, fps, baked = bake(files, prefix, aid, ref)
        if aid in TURN_PAIR:
            _, _, other = bake(files, prefix, TURN_PAIR[aid], ref)
            n = min(len(baked), len(other))
            baked = [blend_pose(baked[f], other[f], 0.5) for f in range(n)]
            print(f"{prefix}_{aid:04d} blended 50/50 with {prefix}_{TURN_PAIR[aid]:04d} (straight)")
        anims.append((aid, baked))
        print(f"{prefix}_{aid:04d}: {nf} frames @ {fps:.0f} fps")
    write_model(out, names, parents, ref, mb, anims)
    if preview:
        render_preview(preview, parents, mb.verts, mb.tris, anims)


def add_head(game, parts_dir, params, mb, common, hide_bits):
    """Back of head, neutral FaceGen face + eyes and the preset's hair (FACE_ROW), skinned to the head like the armour"""
    face = params["FaceGenParam"][FACE_ROW][1]
    fgbnd = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(f"{game}/facegen/FaceGen.fgbnd"))}
    fgtex = souls.tpf(fgbnd["facegen.tpf"])
    hair = f"HR_M_{face['hairStyle']:04d}"
    hair_rgb = (face["hairColor_R"], face["hairColor_G"], face["hairColor_B"])

    def image(name, dds, tint=None):
        dec = souls.dds_decode_rgba(dds, HEAD_TEX_MAX.get(name, TEX_MAX))
        w, h, px = dec
        if tint:  # ASSUMED Ps_Hair g_useHairColor: grey hair texture x hairColor / 255
            px = bytearray(px)
            for i in range(0, len(px), 4):
                for c in range(3):
                    px[i + c] = px[i + c] * tint[c] // 255
        return mb.texture_rgba(name, w, h, bytes(px), cutout=tint is not None)

    for part in ("FC_M_0000", "FG_A_0000", hair):
        files = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(f"{parts_dir}/{part}.partsbnd.dcx"))}
        F = souls.flver(files[f"{part}.flver"])
        tex = souls.tpf(files[f"{part}.tpf"]) if f"{part}.tpf" in files else {}
        bw = bind_world(F["bones"])
        lod = HEAD_LOD.get(part, LOD2)
        n = 0
        for m in F["meshes"]:
            bits = hide_bits(part, m)
            fss = {f[0]: f for f in m["facesets"] if not f[0] & 0x80000000}
            fs = fss.get(lod) or fss.get(0x01000000)  # some hair cards have no LOD2
            if bits is None or not fs or m["mtd"].startswith("M["):  # M[D]_Alp: the eyes' highlight quad
                continue
            uv = None
            if m["diffuse"] == "FG_face_gen":
                ti, uv = image(FACE_TEX, fgtex[FACE_TEX]), m["verts"]["uv2"]
            elif m["diffuse"] in fgtex:
                ti = image(m["diffuse"], fgtex[m["diffuse"]])
            elif m["diffuse"] in common:
                ti = image(m["diffuse"], common[m["diffuse"]])
            else:
                ti = image(m["diffuse"], tex[m["diffuse"]], hair_rgb if part == hair else None)
            mb.add(F, bw, m, fs, ti, SKIN, uv=uv, bits=bits)
            n += len(list(souls.triangles(fs)))
        print(f"{part}: {n} tris")


def quantize(w, h, rgb):
    """RGB image -> (w, h, CSM1-ordered RGBA palette bytes, index bytes), PSMT8-ready."""
    from PIL import Image
    if w < 128:  # PSMT8 uploads want a buffer width of at least 128
        img = resample.resize(Image.frombytes("RGB", (w, h), rgb), (128, h * 128 // w), Image.BILINEAR)
        w, h, rgb = 128, h * 128 // w, img.tobytes()
    img = Image.frombytes("RGB", (w, h), rgb).quantize(256, method=Image.Quantize.MEDIANCUT)
    pal = img.getpalette()[:768] + [0] * (768 - len(img.getpalette()[:768]))
    clut = bytearray(1024)
    for i in range(256):
        j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)  # CSM1 swizzle
        clut[4 * j:4 * j + 4] = bytes((pal[3 * i], pal[3 * i + 1], pal[3 * i + 2], 0x80))
    return w, h, bytes(clut), img.tobytes()


def render_preview(path, parents, verts, tris, anims):
    """Side-by-side views of a few decoded frames, skinned on the CPU exactly as the PS2 code does it."""
    from PIL import Image, ImageDraw
    shots = [(0, 0), (1, 10), (1, 30), (5, 10), (5, 17), (4, 8)]  # (anim index, frame)
    W = 240
    img = Image.new("RGB", (W * len(shots), 330), "white")
    dr = ImageDraw.Draw(img)
    light = (0.4, 0.8, 0.45)
    for k, (ai, f) in enumerate(shots):
        aid, baked = anims[ai]
        pose = ha.world(parents, [(t, q, None) for q, t in baked[min(f, len(baked) - 1)]])
        P, C = [], []
        for pa, pb, na, ba, bb, wa, col in verts:
            ta, qa = pose[ba]
            tb, qb = pose[bb]
            a, b = ha.qrot(qa, pa), ha.qrot(qb, pb)
            w = wa / 255
            P.append(tuple(w * (a[i] + ta[i]) + (1 - w) * (b[i] + tb[i]) for i in range(3)))
            n = ha.qrot(qa, na)
            l = max(0, (n[0] * light[0] + n[1] * light[1] + n[2] * light[2]))
            C.append(tuple(int(c * (0.45 + 0.55 * l)) for c in col))
        # 3/4 view from the character's front-left: rotate around Y
        ang = 2.4
        ca, sa = math.cos(ang), math.sin(ang)
        S = [(W * k + W / 2 + 120 * (p[0] * ca + p[2] * sa), 300 - 120 * p[1], -p[0] * sa + p[2] * ca) for p in P]
        for t in sorted(tris, key=lambda t: -sum(S[i][2] for i in t)):
            dr.polygon([S[i][:2] for i in t], fill=C[t[0]])
        dr.text((W * k + 8, 8), f"a{aid:04d} f{f}", fill=(0, 0, 0))
    img.save(path)


if __name__ == "__main__":
    main()
