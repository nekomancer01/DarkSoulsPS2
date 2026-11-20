"""The map's specular / environment pass data (Polaris 2.3, src/map.c spec pass, vu1mapspec.vsm).

DSR lights every lit map material with a specular term from its _s map (PBR: R roughness, G metalness, B F0 code;
FRPG_Phn_* shaders): the collision part's GI_EnvSpc probe reflection x the LightBank envSpc colour and the LightBank
specular light, weighted by Fresnel. The PS2 pass (vu1mapspec.vsm) draws the chunks of such materials again,
untextured and added over the opaque map, with per-vertex colour = f (env + SpecI d) as vu1skin2's cheap form.

What is exported: per texture a specular code in the map.bin texture entry flags bits 2-3 (tools/texmap.py write;
bits 0-1 / 4-7 are taken), from the means of its _s map:
  0 no specular pass for its chunks, 1 rough (mean roughness fits the Blinn exponent 2, export_model.spec_k = 1),
  2 smoother (exponent 4 or more), 3 metal (mean metalness > 0.5).
map.c turns the code into the Blinn fit, Schlick's grazing weight and F0 (spec_draw's table). The normals are not
exported: map.c computes them per chunk from the chunk's own triangles when it first comes within reach (a stream of
4 bytes per vertex pushed the world budget's hard tier to 8.3 % of the spots over, 2026-11-20). Per-vertex _s values
are therefore per texture (ASSUMED, TECH_DEBT map-23).
Eligible: textures every mesh of which is opaque (class OPAQUE: no cut-outs, the pass would light their holes), with
no texel below 50 % alpha (texmap makes those transparent even in an OPAQUE texture), lit (not sky) and has an
<diffuse>_s map. MAP_SPEC=0: no codes (the pass then has nothing to draw)."""
import math
import os

import material
import souls

SPEC = os.environ.get("MAP_SPEC", "1") == "1"
SPEC_RES = 32   # _s mip sampled (export_model.SPEC_RES)
TEX = {}        # texture index -> [eligible so far, code]


def reset():
    TEX.clear()


def _code(sp):
    w, h, px = sp
    n = w * h
    rough = sum(px[0::4]) / n / 255
    metal = sum(px[1::4]) / n / 255
    if metal > 0.5:
        return 3
    e = max(2.0, min(32.0, 2 / max(0.05, rough) ** 4 - 2))  # export_model.spec_k's fit
    return 1 if round(math.log2(e)) <= 1 else 2


def note(ti, mcls, sky, dds, diffuse):
    """mapgather hook per map mesh with texture ti: its eligibility and code"""
    if not SPEC or ti < 0:
        return
    e = TEX.setdefault(ti, [True, 0])
    sp = dds.get((diffuse or "") + "_s")
    e[0] = e[0] and mcls == material.OPAQUE and not sky and sp is not None
    if e[0] and not e[1]:
        dec = souls.dds_decode_rgba(sp, SPEC_RES)
        if dec is None or _holes(dds.get(diffuse or "")):
            e[0] = False
        else:
            e[1] = _code(dec)


def _holes(d):
    """the diffuse has texels below 50 % alpha at the exported size: texmap.encode makes them the transparent index, so
    an OPAQUE-class texture still has cut-out holes (GS_TEST_CUTOUT) the pass would light (m10_01_articles_01: 40 of
    128 x 128, 2026-11-20). Unreadable counts as holes"""
    dec = souls.dds_decode_rgba(d, 128) if d else None
    return dec is None or min(dec[2][3::4]) < 128


def finalize(textures):
    """after gather: only textures whose final class is OPAQUE (another mesh may have made it a cut-out)"""
    for ti, e in TEX.items():
        if e[0] and textures[ti].get("cls", material.OPAQUE) != material.OPAQUE:
            e[0] = False


def write_flags(blob, table, ntex):
    """the codes into the texture entries' flags bits 2-3"""
    n = 0
    for ti, e in TEX.items():
        if e[0] and ti < ntex:
            blob[table + 32 * ti + 7] |= e[1] << 2
            n += 1
    return n
