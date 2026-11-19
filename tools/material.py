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
4 add, 32+ the same for the light-scattering (_LS) variants (SoulsFormats MTD.BlendMode). The suffix agrees with it in
the census (tools/mtd_census.py): _Edge = 1, _Alp = 2, _Add = 4, [We] = 3."""
import os
import sys

# ------------------------------------------------------------------------------------------------------ the classes
# Row: name, VU1 program (vu1_layout.h VU1_PROGRAMS name; the map chains pick the list / indexed / clip variant per
# batch), GS ALPHA word (gs.h name), TEST word (gs.h name), ZBUF mask (1: no Z write), passes (MP_ bits below), what
# it is. Values are what the runtime does today for that class (map.c blend_regs / the opaque TEST, render.c):
# PR 1 records them, the runtime starts reading them in PR 2.
PASSES = ("RELIEF", "PLAYER_LIGHT", "MIPS", "ENV_SPEC", "EDGE_FOG")  # MP_ bit order
CLASSES = [
    # opaque map pieces: cut-out test on (the CLUT's transparent index), relief, player light, mip chain, edge fog
    ("OPAQUE", "MAP2", "GS_ALPHA_MIX", "GS_TEST_CUTOUT", 0, ("RELIEF", "PLAYER_LIGHT", "MIPS", "EDGE_FOG"),
     "g_BlendMode 0"),
    # _Edge cut-outs (leaves, ivy, fences) and the _Alp pieces still drawn as cut-outs (walls / cloth, TECH_DEBT
    # map-30): the same state as OPAQUE, no relief (export_normal.bumped leaves them out)
    ("ALPHA_TEST", "MAP2", "GS_ALPHA_MIX", "GS_TEST_CUTOUT", 0, ("PLAYER_LIGHT", "MIPS", "EDGE_FOG"),
     "g_BlendMode 1, or 2 drawn as a cut-out"),
    # alpha blended: mist / cloud cards ([Dn]_Alp), background cards (m10_bg_*), sky cloud layers; no Z write
    ("BLEND", "MAP2", "GS_ALPHA_MIX", "GS_TEST_ZGE", 1, ("EDGE_FOG",), "g_BlendMode 2"),
    # additive light shafts (_Add); no Z write
    ("ADDITIVE", "MAP2", "GS_ALPHA_ADD", "GS_TEST_ZGE", 1, ("EDGE_FOG",), "g_BlendMode 4"),
    # [We] water: one colour from g_WaterColor / Fresnel (mapgather.water_rgba), blended; no Z write
    ("WATER", "MAP2", "GS_ALPHA_MIX", "GS_TEST_ZGE", 1, ("EDGE_FOG",), "g_BlendMode 3"),
    # grass / fern cards, thinned into tufts by distance (mapchunks foliage chunks): cut-out state
    ("FOLIAGE", "MAP2", "GS_ALPHA_MIX", "GS_TEST_CUTOUT", 0, ("PLAYER_LIGHT", "EDGE_FOG"), "grass / fern diffuse"),
    # characters and skinned objects (vu1skin / vu1skinclip): not in map.bin; the row is for the character streams
    ("SKIN", "SKIN", "GS_ALPHA_MIX", "GS_TEST_CUTOUT", 0, ("ENV_SPEC",), "character FLVERs"),
    # ground decals (blood, soot, dirt, stains on M[D][L]_Alp): blended over the floor; no Z write
    ("DECAL", "MAP2", "GS_ALPHA_MIX", "GS_TEST_ZGE", 1, ("EDGE_FOG",), "g_BlendMode 2, decal diffuse"),
    # sky dome, far ground ring and other pieces beyond SKY_DIST of the navmesh: their FogBank row, drawn first, never
    # distance culled
    ("SKY", "MAP2", "GS_ALPHA_MIX", "GS_TEST_CUTOUT", 0, (), "sky MTD or beyond SKY_DIST"),
]
NAMES = [c[0] for c in CLASSES]
globals().update({n: i for i, n in enumerate(NAMES)})  # material.OPAQUE, material.BLEND, ...
assert len(CLASSES) <= 16  # four bits in the texture entry flags
BLENDED = {BLEND, ADDITIVE, WATER, DECAL}

# map.bin chunk flags (tools/mapcfg.py CHUNK_*; ps2/proto/src/map_i.h)
_CHUNK_BLEND, _CHUNK_ADD, _CHUNK_SKY = 2, 4, 8
TEX_CLASS_SHIFT = 4  # texture entry flags bits 4-7 (tools/texmap.py write)
CELLS_FLAG = 4  # DSM5 cell table word 1 bit 2: the texture entries carry classes (tools/mapcells.py CELLS_MATCLS)

# DSR MTD g_BlendMode values (SoulsFormats MTD.BlendMode; the _LS variants are + 32)
BM_NORMAL, BM_EDGE, BM_BLEND, BM_WATER, BM_ADD = 0, 1, 2, 3, 4

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


def classify(mtd, diffuse=None, sky=False):
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
    """The map.bin chunk flag bits of a class (CHUNK_BLEND, CHUNK_ADD, CHUNK_SKY), as the exporter set them before
    the table: blended classes, the additive one, and every mesh with the sky role (whatever its class)."""
    return (_CHUNK_BLEND if cls in BLENDED else 0) | (_CHUNK_ADD if cls == ADDITIVE else 0) | (_CHUNK_SKY if sky else 0)


# A texture's class (map.bin texture entry): the exporter keys textures by (diffuse, blended), so one texture can carry
# meshes of several classes of the same blend side: OPAQUE and ALPHA_TEST (one diffuse under M[D] and M[D]_Edge: 9-15
# textures per census area, the same GS state today), BLEND and ADDITIVE (Burg's m10_bg_cloud under _Alp and _Add).
# The texture keeps the first class in this order; the chunk flags carry the rest (chunk_class).
MERGE_ORDER = [WATER, DECAL, BLEND, ADDITIVE, FOLIAGE, ALPHA_TEST, OPAQUE, SKY, SKIN]


def texture_class(cls, mtd, diffuse):
    """The class a mesh of class cls gives its texture: the sky role is a chunk flag (CHUNK_SKY), so a SKY mesh gives
    the class its MTD has without the role."""
    return classify(mtd, diffuse, False) if cls == SKY else cls


def merge(a, b):
    """Two classes on one texture -> the one it keeps (MERGE_ORDER); a may be None."""
    return b if a is None else min(a, b, key=MERGE_ORDER.index)


def chunk_class(tex_cls, flags):
    """A chunk's class from its texture's class and its flags (the runtime's mat_chunk_class): additive chunks are
    ADDITIVE, unblended sky chunks SKY, the rest their texture's."""
    if flags & _CHUNK_ADD:
        return ADDITIVE
    if flags & _CHUNK_SKY and tex_cls not in BLENDED:
        return SKY
    return tex_cls


def write_classes(blob, table, textures):
    """Each texture's class into its map.bin entry's flags byte (offset 7, bits 4-7); table = the entry table's
    index in blob (tools/texmap.py write; bits 0-1 are texmap's near level and mapsplit's TEX_CHAIN_AT)."""
    for k, t in enumerate(textures):
        blob[table + 32 * k + 7] |= t.get("cls", OPAQUE) << TEX_CLASS_SHIFT


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
            f"#define MAT_CHUNK_CLASS(tex_cls, flags) ((flags) & {_CHUNK_ADD} ? MAT_ADDITIVE : "
            f"(flags) & {_CHUNK_SKY} && !MAT_BLENDED(tex_cls) ? MAT_SKY : (tex_cls))",
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
    """classify + chunk_flags give the old flags on every suffix / diffuse / sky combination; chunk_class agrees with
    the flags it is derived from. Synthetic names only (no game data)."""
    mtds = ["M[D].mtd", "M[DB][L].mtd", "M[D]_Edge.mtd", "M[DB][L]_Alp.mtd", "A[Dn]_Alp.mtd", "A[Dn]_Add.mtd",
            "A_shaft[D]_Add.mtd", "A_Water_lake[We].mtd", "A_Sky[Dn]_LS.mtd", "A_cloud[DB]_Alp.mtd", "M_Tree[D]_Edge.mtd"]
    difs = [None, "m_wall", "m_floor_decal", "m_dirt_01", "m_grass_01", "m_fern", "m_grass_ground", "m10_bg_tree",
            "m_stain", "m_ground_01"]
    n = 0
    for mtd in mtds:
        for dif in difs:
            for sky in (False, True):
                sky = sky or sky_mtd(mtd)
                c = classify(mtd, dif, sky)
                fl = chunk_flags(c, sky)
                assert fl == _old_flags(mtd, dif, sky), (mtd, dif, sky, NAMES[c], fl)
                t = texture_class(c, mtd, dif)
                cc = chunk_class(t, fl)
                assert (cc in BLENDED) == bool(fl & _CHUNK_BLEND) and (cc == ADDITIVE) == bool(fl & _CHUNK_ADD), \
                    (mtd, dif, sky, NAMES[c], NAMES[cc])
                row = CLASSES[cc]
                assert row[4] == bool(fl & _CHUNK_BLEND) and (row[2] == "GS_ALPHA_ADD") == bool(fl & _CHUNK_ADD)
                n += 1
    assert merge(None, OPAQUE) == OPAQUE and merge(OPAQUE, ALPHA_TEST) == ALPHA_TEST and merge(ADDITIVE, BLEND) == BLEND
    print(f"material.py test: {n} cases, flags as before")


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "table"
    if what == "test":
        test()
    elif what == "header":
        sys.stdout.write(header())
    else:
        for i, (name, prog, alpha, test, zmsk, passes, src) in enumerate(CLASSES):
            print(f"{i:2} {name:11} {prog:5} {alpha:13} {test:15} zmsk {zmsk}  {','.join(passes):38} {src}")
