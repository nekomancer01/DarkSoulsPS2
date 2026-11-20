"""MTD census of map FLVER meshes (Polaris 2.1): per MTD, how many meshes use it, its shader (.spx / .fx path inside
the MTD), g_BlendMode / g_LightingType / g_IsFoliage, the class tools/material.py assigns and the old exporter checks
(mapgather is_effect / decal / water, mapchunks grass, cut-out), plus every (diffuse, blended) texture key that would
carry two classes (the class is stored per texture entry, so those must not exist).

  mtd_census.py [--game DIR] [--counts] <map id>...     e.g. m10_02_00_00 m10_01_00_00 m18_01_00_00

Placement is not modelled: the sky role here is the MTD name only (the exporter adds meshes beyond SKY_DIST of the
navmesh). Parts: every MSB map piece and static object (breakables left out, as gather does); draw groups ignored.
--counts prints only counts per MTD name / class (no texture names), the form that may go into a PR text."""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls  # noqa: E402
import gamepath  # noqa: E402
import material  # noqa: E402
import mapgather  # noqa: E402


def mtd_shaders(game):
    """MTD file name -> shader path written in it (N:\\FRPG\\data\\Material\\spx\\....spx), basename only."""
    out = {}
    for _, n, d in souls.bnd3(souls.read(os.path.join(game, "mtd", "Mtd.mtdbnd.dcx"))):
        m = re.search(rb"[\x20-\x7e]+?\.(spx|fx)\b", d)
        out[souls.basename(n)] = m.group().decode().replace("\\", "/").rsplit("/", 1)[-1] if m else "?"
    return out


def old_checks(mtd, diffuse):
    """The exporter's per-mesh checks before material.py (mapgather.py, mapchunks.name_is_grass), without placement."""
    low, dif = mtd.lower(), (diffuse or "").lower()
    eff = "_add" in low or ("[dn]" in low and "_alp" in low)
    decal = "_alp" in low and any(k in dif for k in ("decal", "dirt", "ground", "stain", "_bg_"))
    water = "water" in low
    tags = [t for t, on in (("effect", eff), ("decal", decal), ("water", water), ("add", "_add" in low),
                            ("grass", material.is_grass(diffuse)), ("sky", "sky" in low or "cloud" in low),
                            ("cutout", "_alp" in low or "_edge" in low)) if on]
    return "+".join(tags) or "-", bool(eff or decal or water)


def meshes(game, map_id):
    """(mtd, diffuse) of every mesh of the map's pieces and static objects (one row per placed part)."""
    M = souls.msb1(souls.read(f"{game}/map/MapStudio/{map_id}.msb"))
    flvers = {}
    for part in M["parts"]:
        key = (part["type"], part["model"])
        if part["type"] == 0:
            if key not in flvers:
                p = f"{game}/map/{map_id}/{part['model']}A{map_id[1:3]}.flver.dcx"
                flvers[key] = souls.flver(souls.read(p)) if os.path.exists(p) else None
        elif part["type"] == 1:
            if key not in flvers:
                flvers[key] = mapgather.load_object(game, part["model"], {})
        else:
            continue
        for m in (flvers[key] or {"meshes": ()})["meshes"]:
            yield m["mtd"], m["diffuse"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game")
    ap.add_argument("--counts", action="store_true")
    ap.add_argument("maps", nargs="+")
    a = ap.parse_args()
    game = gamepath.game_dir(a.game)
    params, shaders = mapgather.mtd_params(game), mtd_shaders(game)
    for map_id in a.maps:
        rows = collections.Counter(meshes(game, map_id))
        per_mtd = collections.defaultdict(lambda: [0, collections.Counter()])
        tex_cls = collections.defaultdict(set)
        mismatch = 0
        for (mtd, dif), n in rows.items():
            cls = material.classify(mtd, dif, material.sky_mtd(mtd))
            old, old_blend = old_checks(mtd, dif)
            sky = material.sky_mtd(mtd)
            old_blend = old_blend or (material.SKY_ALP_BLEND and sky and "_alp" in mtd.lower())
            fl = material.chunk_flags(cls, sky)
            if bool(fl & 2) != bool(old_blend) or bool(fl & 4) != ("_add" in mtd.lower()):
                mismatch += n
            per_mtd[mtd][0] += n
            per_mtd[mtd][1][(material.NAMES[cls], old)] += n
            tex_cls[dif, bool(fl & 2)].add(cls)
        print(f"== {map_id}: {sum(rows.values())} meshes, {len(per_mtd)} MTDs, blend/add flag mismatches {mismatch}")
        hdr = f"{'meshes':>6}  {'MTD':34} {'shader':34} {'BM':>3} {'LT':>3} {'fol':>3}  class / old checks"
        print(hdr)
        for mtd, (n, cl) in sorted(per_mtd.items(), key=lambda kv: -kv[1][0]):
            p = params.get(mtd, {})
            g = lambda k: p.get(k, ("-",))[0]
            cls = ", ".join(f"{c} [{o}] x{k}" for (c, o), k in cl.most_common())
            print(f"{n:6}  {mtd:34} {shaders.get(mtd, '?'):34} {g('g_BlendMode'):>3} {g('g_LightingType'):>3} "
                  f"{g('g_IsFoliage'):>3}  {cls}")
        by_cls = collections.Counter()
        for (mtd, dif), n in rows.items():
            by_cls[material.NAMES[material.classify(mtd, dif, material.sky_mtd(mtd))]] += n
        print("classes:", ", ".join(f"{c} {n}" for c, n in by_cls.most_common()))
        # suffix vs g_BlendMode
        bm = collections.Counter()
        for mtd in per_mtd:
            low = mtd.lower()
            suf = "_add" if "_add" in low else "_alp" if "_alp" in low else "_edge" if "_edge" in low else \
                "water" if "water" in low else "-"
            bm[suf, params.get(mtd, {}).get("g_BlendMode", (None,))[0]] += 1
        print("suffix x g_BlendMode (MTDs):", ", ".join(f"{s}={b}: {n}" for (s, b), n in sorted(bm.items(), key=str)))
        multi = {k: v for k, v in tex_cls.items() if len(v) > 1}
        print(f"texture keys with more than one class: {len(multi)}")
        if not a.counts:
            for (dif, bl), v in multi.items():
                print(f"   {dif} blend={bl}: {', '.join(material.NAMES[c] for c in sorted(v))}")


if __name__ == "__main__":
    main()
