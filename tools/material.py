"""Material classes (Polaris 2.1): DSR's MTD files map to a small set of classes, each naming its VU1 program, GS state
and passes. One table for the exporters (classify(), this module) and the runtime (ps2/proto/src/material_table.h,
generated: `python3 tools/material.py header > ps2/proto/src/material_table.h`; tools/ci/offline.sh checks it is up to
date). The runtime never branches on texture or MTD names; it reads a texture's class (map.bin texture entry flags bits
4-7, write_classes) and the chunk flags derived from it (chunk_flags()).

  python3 material.py header      the C header
  python3 material.py table       the table as text

Where the class comes from (classify): the MTD's own name suffix and g_BlendMode, the diffuse name for the cases DSR
draws with a shader the port has no counterpart for (grass cards, decals), and the mesh's placement (sky role).
DSR's g_BlendMode (MTD parameter, every map MTD has it): 0 normal, 1 edge (alpha tested cut-out), 2 blend, 3 water,
4 add, 5 sub, 6 mul, 7 add-mul, 8 sub-mul, 9 water wave, 32+ the same for the light-scattering (_LS) variants: the exe's
own label table lists them in this order (UTF-16 labels "(Normal)", "(TexEdge)", "(Blend)", "(Water)", "(Add)",
"(Sub)", "(Mul)", "(AddMul)", "(SubMul)", "(WaterWave)", then the LS set; notes/EXE_FINDINGS.md "Material blend
modes"). The suffix agrees with it in the census (tools/mtd_census.py: _Edge = 1, _Alp = 2, _Add = 4, [We] = 3) except
on three MTDs, where the class follows g_BlendMode (classify, TECH_DEBT mat-1).

MAT_GS_TABLE (env, default 1; also the runtime switch of the same name, ps2/proto/src/switches.h): 0 = the classes and
texture keys of the name rules alone (Polaris 2.1 PR 1) and the old FFX blend rule, i.e. the exports from before PR 2."""
import os
import sys

# ------------------------------------------------------------------------------------------------------ the classes
# Row: name, VU1 program (vu1_layout.h VU1_PROGRAMS name; the map chains pick the list / indexed / clip variant per
# batch), GS ALPHA word (gs.h name), TEST word (gs.h name), ZBUF mask (1: no Z write), passes (MP_ bits below), what
# it is. The map passes read ALPHA, the Z mask (= the batch's blend bit, which picks TEST in vu1.c; material.c asserts
# TEST matches) and the passes (map_i.h mp_row, map.c row_regs).
PASSES = ("RELIEF", "PLAYER_LIGHT", "MIPS", "ENV_SPEC", "EDGE_FOG", "FRESNEL")  # MP_ bit order
GS_TABLE = os.environ.get("MAT_GS_TABLE", "1") == "1"
_LIT = ("RELIEF", "PLAYER_LIGHT", "MIPS", "EDGE_FOG")  # the passes of every unblended map class (relief only where
# bump.bin has the chunk: export_normal.bumped leaves cut-outs and foliage out)
CLASSES = [
    # opaque map pieces: cut-out test on (the CLUT's transparent index), relief, player light, mip chain, edge fog
    # ENV_SPEC: the specular pass (Polaris 2.3, map.c spec_draw) over its chunks that carry the data (tools/mapspec.py)
    ("OPAQUE", "MAP2", "GS_ALPHA_MIX", "GS_TEST_CUTOUT", 0, _LIT + ("ENV_SPEC",), "g_BlendMode 0"),
    # _Edge cut-outs (leaves, ivy, fences) and the _Alp pieces still drawn as cut-outs (walls / cloth, TECH_DEBT
    # map-30): the same state as OPAQUE
    ("ALPHA_TEST", "MAP2", "GS_ALPHA_MIX", "GS_TEST_CUTOUT", 0, _LIT, "g_BlendMode 1, or 2 drawn as a cut-out"),
    # alpha blended: mist / cloud cards ([Dn]_Alp), background cards (m10_bg_*), sky cloud layers; no Z write
    ("BLEND", "MAP2", "GS_ALPHA_MIX", "GS_TEST_ZGE", 1, ("EDGE_FOG",), "g_BlendMode 2"),
    # additive light shafts (_Add); no Z write
    ("ADDITIVE", "MAP2", "GS_ALPHA_ADD", "GS_TEST_ZGE", 1, ("EDGE_FOG",), "g_BlendMode 4"),
    # [We] water (Polaris 2.6): blended, no Z write; FRESNEL: per-vertex colour and opacity on the EE from the eye's
    # angle to the surface (map.c water_colours: DSR's refraction tint g_WaterColor and Fresnel reflection
    # g_FresnelColor, F = bias + scale (1 - cos)^pow) over a white texture (mapgather.water_texture)
    ("WATER", "MAP2", "GS_ALPHA_MIX", "GS_TEST_ZGE", 1, ("EDGE_FOG", "FRESNEL"), "g_BlendMode 3"),
    # grass / fern cards, thinned into tufts by distance (mapchunks foliage chunks): cut-out state
    ("FOLIAGE", "MAP2", "GS_ALPHA_MIX", "GS_TEST_CUTOUT", 0, _LIT, "grass / fern diffuse"),
    # characters and skinned objects (vu1skin / vu1skinclip): not in map.bin; the row is for the character streams
    ("SKIN", "SKIN", "GS_ALPHA_MIX", "GS_TEST_CUTOUT", 0, ("ENV_SPEC",), "character FLVERs"),
    # ground decals (blood, soot, dirt, stains on M[D][L]_Alp): blended over the floor; no Z write
    ("DECAL", "MAP2", "GS_ALPHA_MIX", "GS_TEST_ZGE", 1, ("EDGE_FOG",), "g_BlendMode 2, decal diffuse"),
    # sky dome, far ground ring and other pieces beyond SKY_DIST of the navmesh: their FogBank row, drawn first, never
    # distance culled
    ("SKY", "MAP2", "GS_ALPHA_MIX", "GS_TEST_CUTOUT", 0, (), "sky MTD or beyond SKY_DIST"),
    # subtractive (g_BlendMode 5): the destination darkened by the texel, Cd - Cs x As. ASSUMED the D3D11 state is
    # BLEND_OP_REV_SUBTRACT with SRC_ALPHA / ONE (the exe's blend descs are not decoded; the label is "(Sub)");
    # the GS ALPHA (0 - Cs) x As + Cd is that equation exactly, clamped at 0 (COLCLAMP). Asylum's A19_Division
    ("SUBTRACT", "MAP2", "GS_ALPHA_SUB", "GS_TEST_ZGE", 1, ("EDGE_FOG",), "g_BlendMode 5"),
]
NAMES = [c[0] for c in CLASSES]
globals().update({n: i for i, n in enumerate(NAMES)})  # material.OPAQUE, material.BLEND, ...
assert len(CLASSES) <= 16  # four bits in the texture entry flags
BLENDED = {BLEND, ADDITIVE, WATER, DECAL, SUBTRACT}

# map.bin chunk flags (tools/mapcfg.py CHUNK_*; ps2/proto/src/map_i.h)
_CHUNK_BLEND, _CHUNK_ADD, _CHUNK_SKY = 2, 4, 8
TEX_CLASS_SHIFT = 4  # texture entry flags bits 4-7 (tools/texmap.py write)
CELLS_FLAG = 4  # DSM5 cell table word 1 bit 2: the texture entries carry classes (tools/mapcells.py CELLS_MATCLS)

# DSR MTD g_BlendMode values (SoulsFormats MTD.BlendMode; the _LS variants are + 32)
BM_NORMAL, BM_EDGE, BM_BLEND, BM_WATER, BM_ADD, BM_SUB = 0, 1, 2, 3, 4, 5

BG_BLEND = os.environ.get("BG_BLEND", "1") == "1"          # background cards (m10_bg_*) blended, else cut-outs
SKY_ALP_BLEND = os.environ.get("SKY_ALP_BLEND", "1") == "1"  # sky alpha layers blended, else cut-outs
DECAL_WORDS = ("decal", "dirt", "ground", "stain")


def sky_mtd(mtd):
    """A sky or cloud material (A10_Sky[Dn]_LS, A10_02_m9000_Sky, A10_cloud[DB]_Alp, ...)."""
    low = mtd.lower()
    return "sky" in low or "cloud" in low


def is_grass(diffuse):
    """Grass / fern cards (thinned into tufts). m10_01_grass_ground_* is solid ground (Burg floors, ~1k tris): thinned
    as tufts it left holes under the player once the draw-group filter hid the stand-in floor beneath (m2250B1 over
    m2420B1, autotest point 25)."""
    return bool(diffuse) and ("grass" in diffuse or "fern" in diffuse) and "ground" not in diffuse


def blend_mode(params):
    """g_BlendMode of an MTD's parameters (mapgather.mtd_params row), the _LS variants folded onto the base modes; None
    when the MTD has none."""
    v = (params or {}).get("g_BlendMode")
    return v[0] % 32 if v else None


def classify(mtd, diffuse=None, sky=False, params=None):
    """-> material class of one FLVER mesh (see classify_names), with g_BlendMode (params = the MTD's parameters) where
    the name rules disagree with it (TECH_DEBT mat-1; MAT_GS_TABLE=0: the name rules alone):
    - g_BlendMode 5 (sub): SUBTRACT (Asylum's A19_Division[D]_Alp, a decal / cut-out by its name).
    - g_BlendMode 2 on an _Add MTD: BLEND (Burg's A10_cloud[Dn]_Add, a sky cloud layer).
    - g_BlendMode 4 (add) under any name: ADDITIVE.
    g_BlendMode 3 off a water shader stays as named: A19_Snow[L] (FRPG_Snow_Lit: snow over its diffuse, drawn opaque,
    ASSUMED until a DSR capture shows it). Modes 6-9 (mul, add-mul, sub-mul, water wave) do not occur on map MTDs of the
    census areas and keep the name rules."""
    c = classify_names(mtd, diffuse, sky)
    bm = blend_mode(params) if GS_TABLE else None
    if bm == BM_SUB:
        return SUBTRACT
    if bm == BM_ADD:
        return ADDITIVE
    if bm == BM_BLEND and c == ADDITIVE:
        return BLEND
    return c


def classify_names(mtd, diffuse=None, sky=False):
    """-> material class of one FLVER mesh: mtd = its MTD file name, diffuse = its diffuse texture name, sky = placed
    as sky / far background (sky_mtd, or beyond SKY_DIST of the walkable navmesh: mapgather.is_sky).
    Order: water, additive, the blended cases, foliage, sky, cut-outs, opaque.
    - [We] water and every MTD named water: WATER (g_BlendMode 3).
    - _Add: ADDITIVE (g_BlendMode 4; light shafts).
    - [Dn]_Alp: BLEND (mist / cloud cards, unlit).
    - _Alp with a decal / dirt / ground / stain diffuse: DECAL; with a m10_bg_* diffuse (background cards, BG_BLEND):
      BLEND; on a sky mesh (SKY_ALP_BLEND): BLEND.
    - grass / fern diffuse: FOLIAGE. Other sky meshes: SKY.
    - _Alp otherwise, _Edge: ALPHA_TEST (DSR blends the _Alp ones: ASSUMED cut-outs, TECH_DEBT map-30).
    The name suffixes match g_BlendMode on every map MTD of the census areas (tools/mtd_census.py)."""
    low = mtd.lower()
    dif = (diffuse or "").lower()
    alp = "_alp" in low
    if "water" in low:
        return WATER
    if "_add" in low:
        return ADDITIVE
    if "[dn]" in low and alp:
        return BLEND
    if alp and any(k in dif for k in DECAL_WORDS):
        return DECAL
    if alp and BG_BLEND and "_bg_" in dif:
        return BLEND
    if alp and sky and SKY_ALP_BLEND:
        return BLEND
    if is_grass(diffuse):
        return FOLIAGE
    if sky:
        return SKY
    if alp or "_edge" in low:
        return ALPHA_TEST
    return OPAQUE


def chunk_flags(cls, sky=False):
    """The map.bin chunk flag bits of a class (CHUNK_BLEND, CHUNK_ADD, CHUNK_SKY), which the streaming / culling code
    still reads: blended classes, the additive one, and every mesh with the sky role (whatever its class)."""
    return (_CHUNK_BLEND if cls in BLENDED else 0) | (_CHUNK_ADD if cls == ADDITIVE else 0) | (_CHUNK_SKY if sky else 0)


# A texture's class (map.bin texture entry). The exporter keys textures by (diffuse, state_key): every class on one
# entry has the same GS state, so the runtime's state from the texture is exact. Classes that share a state still meet
# on one texture (OPAQUE / ALPHA_TEST / FOLIAGE: one diffuse under M[D] and M[D]_Edge, 9-15 textures per census area;
# BLEND / DECAL / WATER); the entry keeps the first in this order (a label only: the state is the same).
MERGE_ORDER = [WATER, DECAL, BLEND, SUBTRACT, ADDITIVE, FOLIAGE, ALPHA_TEST, OPAQUE, SKY, SKIN]


def state_key(cls):
    """The texture key part that keeps classes of different GS state apart (mapgather tex_index): the row's ALPHA,
    TEST and Z mask. MAT_GS_TABLE=0: blended or not, the key before PR 2 (one texture could then serve BLEND and
    ADDITIVE meshes, TECH_DEBT mat-2)."""
    row = CLASSES[cls]
    return (row[2], row[3], row[4]) if GS_TABLE else cls in BLENDED


def texture_class(cls, mtd, diffuse, params=None):
    """The class a mesh of class cls gives its texture: the sky role is a chunk flag (CHUNK_SKY), so a SKY mesh gives
    the class its MTD has without the role."""
    return classify(mtd, diffuse, False, params) if cls == SKY else cls


def merge(a, b):
    """Two classes on one texture -> the one it keeps (MERGE_ORDER); a may be None."""
    return b if a is None else min(a, b, key=MERGE_ORDER.index)


def flag_class(flags):
    """The class the chunk flags alone give (the runtime's mat_flag_class: MAT_GS_TABLE=0, untextured chunks)."""
    if flags & _CHUNK_BLEND:
        return ADDITIVE if flags & _CHUNK_ADD and not flags & _CHUNK_SKY else BLEND
    return SKY if flags & _CHUNK_SKY else OPAQUE


def chunk_class(tex_cls, flags):
    """A chunk's class from its texture's class (None: untextured) and its flags (the runtime's mat_chunk_class):
    unblended sky chunks SKY, the rest their texture's."""
    if tex_cls is None:
        return flag_class(flags)
    if flags & _CHUNK_SKY and tex_cls not in BLENDED:
        return SKY
    return tex_cls


# FFX particle systems (tools/export_effects.py system()): BlendMode of the appearance -> class. 6 is alpha blended;
# 1 is used only by the light / fog walls (74 of ~1900 appearances, all on a plain white texture, 2026-11-17 survey):
# five 0.196 sheets overlap, and added they clipped to a flat white box (TECH_DEBT fx-12), so they blend too; every
# other mode (4: the walls' light swirls, embers, sparks) is additive. ASSUMED: the exe's FFX blend table is not
# decoded (the FFX modes are not the MTD ones: 6 is mul there); FX_WALL_ALPHA=0 keeps mode 1 additive.
FFX_CLASS = {6: BLEND, 1: BLEND}
FX_WALL_ALPHA = os.environ.get("FX_WALL_ALPHA", "1") == "1"


def ffx_class(bm):
    """class of an FFX appearance's BlendMode (None: additive like every unknown mode)"""
    if bm == 1 and not FX_WALL_ALPHA:
        return ADDITIVE
    return FFX_CLASS.get(bm, ADDITIVE)


def ffx_blended(bm):
    """1 if fx.c draws the system alpha blended (export_effects system's blend byte), 0 additive"""
    return 1 if CLASSES[ffx_class(bm)][2] == "GS_ALPHA_MIX" else 0


def write_classes(blob, table, textures):
    """Each texture's class into its map.bin entry's flags byte (offset 7, bits 4-7); table = the entry table's
    index in blob (tools/texmap.py write; bits 0-1 are texmap's near level and mapsplit's TEX_CHAIN_AT)."""
    for k, t in enumerate(textures):
        blob[table + 32 * k + 7] |= t.get("cls", OPAQUE) << TEX_CLASS_SHIFT


def is_ivy(mtd):
    """ivy cards (thinned as wall foliage: mapchunks.thin_foliage)"""
    return "ivy" in mtd.lower()


def two_sided(mtd):
    """_Edge cut-out cards (leaves, ivy, fences): two-sided light (DSR's Phn pixel shaders read both faces;
    mapgather EDGE_2SIDED)"""
    return "_edge" in mtd.lower()


def has_relief(mtd):
    """MTDs whose shader reads g_Bumpmap: a B in the letter block ([DB], [DSB], [DB][M]); cut-outs (_Edge), blends
    (_Alp / _Add) and foliage / trees are left out (a second pass over cut-out texels would darken what is behind).
    (export_normal.bumped)"""
    import re
    low = mtd.lower()
    return bool(re.search(r"\[[a-z]*b[a-z]*\]", low)) and not any(k in low for k in ("_edge", "_alp", "_add",
                                                                                    "foliage", "ivy", "tree"))


def is_cutout(mtd):
    """The simplifier's cut-out test (mapsimp: keep alpha-tested silhouettes): every _Alp / _Edge MTD, drawn
    blended or not."""
    low = mtd.lower()
    return "_alp" in low or "_edge" in low


def header():
    """ps2/proto/src/material_table.h"""
    out = ["/* material_table.h: GENERATED by tools/material.py header (Polaris 2.1 material classes). Do not edit:",
           " * change tools/material.py, then regenerate (tools/ci/offline.sh checks it is up to date). */",
           "#ifndef MATERIAL_TABLE_H", "#define MATERIAL_TABLE_H", "",
           "/* passes a class takes (MatRow.passes) */"]
    out += [f"#define MP_{p} {1 << i}" for i, p in enumerate(PASSES)]
    out += ["", "typedef enum {"] + [f"    MAT_{n}," for n in NAMES] + ["} MatClass;"]
    out += ["", "/* X(name, VU1 program, GS ALPHA, GS TEST, Z write masked, passes): one row per class, in enum order */",
            "#define MAT_CLASSES(X) \\"]
    for name, prog, alpha, test, zmsk, passes, _ in CLASSES:
        pm = " | ".join(f"MP_{p}" for p in passes) or "0"
        out.append(f"    X({name}, {prog}, {alpha}, {test}, {zmsk}, {pm}) \\")
    out[-1] = out[-1][:-2]
    out += ["", f"#define MAT_COUNT {len(CLASSES)}", f"#define MAT_TEX_SHIFT {TEX_CLASS_SHIFT}  /* texture entry flags bits 4-7 */",
            f"#define MAT_CELLS_FLAG {CELLS_FLAG}  /* cell table word 1: the export wrote classes (tools/mapcells.py) */",
            "/* chunk class from the texture's class and the chunk flags (material.chunk_class) */",
            f"#define MAT_CHUNK_CLASS(tex_cls, flags) ((flags) & {_CHUNK_SKY} && !MAT_BLENDED(tex_cls) ? MAT_SKY : (tex_cls))",
            "#define MAT_BLENDED(c) (" + " || ".join(f"(c) == MAT_{NAMES[c]}" for c in sorted(BLENDED)) + ")",
            "", "#endif", ""]
    return "\n".join(out)


def _old_flags(mtd, diffuse, sky):
    """The exporter's checks before this module (mapgather.py up to 2026-11-19): chunk blend / add / sky bits."""
    low, dif = mtd.lower(), (diffuse or "").lower()
    water = "water" in low
    decal = "_alp" in low and any(k in dif for k in DECAL_WORDS + (("_bg_",) if BG_BLEND else ()))
    effect = "_add" in low or ("[dn]" in low and "_alp" in low)
    blend = effect or decal or water or (SKY_ALP_BLEND and sky and "_alp" in low)
    return (_CHUNK_BLEND if blend else 0) | (_CHUNK_ADD if "_add" in low else 0) | (_CHUNK_SKY if sky else 0)


def test():
    """Without MTD parameters classify gives the name rules' old flags on every suffix / diffuse / sky combination;
    with g_BlendMode the mat-1 cases change class; a chunk's class (from its texture's) agrees with the flags it is
    derived from; textures of different GS state never share a key. Synthetic names only (no game data)."""
    mtds = ["M[D].mtd", "M[DB][L].mtd", "M[D]_Edge.mtd", "M[DB][L]_Alp.mtd", "A[Dn]_Alp.mtd", "A[Dn]_Add.mtd",
            "A_shaft[D]_Add.mtd", "A_Water_lake[We].mtd", "A_Sky[Dn]_LS.mtd", "A_cloud[DB]_Alp.mtd", "M_Tree[D]_Edge.mtd"]
    difs = [None, "m_wall", "m_floor_decal", "m_dirt_01", "m_grass_01", "m_fern", "m_grass_ground", "m10_bg_tree",
            "m_stain", "m_ground_01"]
    n = 0
    for mtd in mtds:
        for dif in difs:
            for sky in (False, True):
                sky = sky or sky_mtd(mtd)
                for bm in (None, 0, 2, 4, 5, 36):
                    params = None if bm is None else {"g_BlendMode": (bm,)}
                    c = classify(mtd, dif, sky, params)
                    fl = chunk_flags(c, sky)
                    if bm is None:
                        assert fl == _old_flags(mtd, dif, sky), (mtd, dif, sky, NAMES[c], fl)
                    elif GS_TABLE and bm % 32 in (BM_ADD, BM_SUB):
                        assert c == (ADDITIVE if bm % 32 == BM_ADD else SUBTRACT), (mtd, bm, NAMES[c])
                    elif GS_TABLE and bm == BM_BLEND and "_add" in mtd.lower():
                        assert c == BLEND
                    t = texture_class(c, mtd, dif, params)
                    assert state_key(t) == state_key(c if c != SKY else t)
                    cc = chunk_class(t, fl)
                    row = CLASSES[cc]
                    assert row[4] == bool(fl & _CHUNK_BLEND) and (row[2] == "GS_ALPHA_ADD") == bool(fl & _CHUNK_ADD), \
                        (mtd, dif, sky, bm, NAMES[c], NAMES[cc])
                    assert row[3] == ("GS_TEST_ZGE" if row[4] else "GS_TEST_CUTOUT")
                    n += 1
    assert merge(None, OPAQUE) == OPAQUE and merge(OPAQUE, ALPHA_TEST) == ALPHA_TEST
    if GS_TABLE:
        assert state_key(ADDITIVE) != state_key(BLEND) and state_key(SUBTRACT) != state_key(BLEND) and \
            state_key(OPAQUE) == state_key(ALPHA_TEST) == state_key(FOLIAGE) and state_key(DECAL) == state_key(BLEND)
    assert ffx_blended(6) == 1 and ffx_blended(4) == 0 and ffx_blended(None) == 0 and ffx_blended(1) == int(FX_WALL_ALPHA)
    print(f"material.py test: {n} cases (GS_TABLE {int(GS_TABLE)}), flags as before without g_BlendMode")


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "table"
    if what == "test":
        test()
    elif what == "header":
        sys.stdout.write(header())
    else:
        for i, (name, prog, alpha, test, zmsk, passes, src) in enumerate(CLASSES):
            print(f"{i:2} {name:11} {prog:5} {alpha:13} {test:15} zmsk {zmsk}  {','.join(passes):38} {src}")
