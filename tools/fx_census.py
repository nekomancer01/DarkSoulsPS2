"""Census of the FFX actions the port does not draw yet (TECH_DEBT fx-7, Polaris 2.6 PRs 2 / 3): model particles (61,
108), tracers (40), and the effects the player's attacks spawn from their TAE (weapon trails). Prints to stdout only
(summaries of the user's game files; nothing is written into the repo).

    fx_census.py <game> [map_id ...]        (default: the areas the ISO ships, area.AREAS when present)

Per area: the placed effects' systems by appearance action, then each model / tracer system with its fields (model id,
blend, orientation, scale, life, colour) and whether the area's bundles hold the model file. Then the player: every TAE
event of c0000's attack animations whose type the DSAnimStudio template names as an FFX / SFX spawn, with its fields,
and the actions those effects use."""
import collections, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ffx, souls
import export_effects as ee

DRAWN = ee.APPEAR
NEW = {61: "model_a", 108: "model_cluster", 40: "tracer"}
OTHER = {43: "distortion", 107: "radial_blur", 66: "t66", 117: "t117"}
FIELDS = ("ModelID", "TextureID", "Orientation", "BlendMode", "Duration", "ScaleX", "ScaleY", "ScaleZ", "Scale1X",
          "Scale1Y", "Scale1Z", "Scale2X", "TrailThickness", "TrailMaxTime", "TrailLength", "TrailFadeIn",
          "TrailFadeOut", "TrailFadeMode", "FramesPerRow", "FrameCount", "ColorR", "ColorA", "Color1R", "Color1A",
          "UVScrollX", "UVScrollY", "Attached")


def _val(v):
    if isinstance(v, dict):
        try:
            x = ffx.field_eval(v, 0.0)
            x = x[0] if isinstance(x, list) else x
            return round(x, 3) if isinstance(x, float) else x
        except Exception:  # an odd field kind: its type only
            return f"t{v.get('t')}"
    return round(v, 3) if isinstance(v, float) else v


def _datas(cc):
    for p in cc["params"]:
        if p and p["t"] == 38 and p.get("container") and p["container"]["data"]:
            yield p["container"]["data"]


def bundle_files(lib):
    """{extension: count} and the set of model base names (sNNNNN) in the area's bundles"""
    ext, models = collections.Counter(), set()
    for p in lib.paths:
        for _, name, _ in ffx.bundle_entries(p):
            base = name.replace("\\", "/").rsplit("/", 1)[-1]
            e = base.rsplit(".", 1)[-1].lower()
            ext[e] += 1
            if e == "flver":
                models.add(base[:-6])
    return ext, models


def effect_actions(e, load):
    """[(system kind, appearance data)] of effect e and the effects it calls (export_effects.walk)"""
    found = []
    ee.walk(e, load, found)
    out = []
    for kind, cc, _, _ in found:
        if kind != "ps":
            continue
        ds = list(_datas(cc))
        ap = next((d for d in ds if d["sub"] in DRAWN or d["sub"] in NEW or d["sub"] in OTHER), None)
        out.append((ap["sub"] if ap else None, ap, [d["sub"] for d in ds]))
    return out


def area_census(game, map_id):
    A = ee.Area(game, map_id)
    lib = ee.Library(game, map_id)
    ext, models = bundle_files(lib)
    print(f"== {map_id}: bundles {[os.path.basename(p) for p in lib.paths]}, files {dict(ext)}")
    kinds, rows, seen = collections.Counter(), [], set()
    for fid, *_ in A.inst:
        if fid in seen:
            continue
        seen.add(fid)
        e = lib.effect(fid)
        if e is None:
            continue
        for sub, ap, subs in effect_actions(e, lib.effect):
            kinds[sub] += 1
            if sub in NEW or sub in OTHER:
                rows.append((fid, sub, ap, subs))
    print("   systems by appearance:", {(NEW.get(k) or OTHER.get(k) or k): v for k, v in sorted(kinds.items(), key=str)})
    for fid, sub, ap, subs in rows:
        f = {k: _val(ap[k]) for k in FIELDS if k in ap}
        mid = ap.get("ModelID")
        have = "" if mid is None else (" model file present" if f"s{mid:05d}" in models else " model file MISSING")
        print(f"   effect {fid} {NEW.get(sub) or OTHER.get(sub)} actions {subs}{have}: {f}")
    return kinds


def player_census(game):
    chr_dir = os.path.join(game, "chr")
    entries = souls.bnd3(souls.read(f"{chr_dir}/c0000.anibnd.dcx"))
    tpl_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ext", "DSAnimStudio",
                            "DSAnimStudioNETCore", "Res", "TAE.Template.DS1.xml")
    tpl = souls.tae_template(tpl_path) if os.path.exists(tpl_path) else {}
    if not tpl:
        print("player: no DSAnimStudio template (ext/DSAnimStudio): event types listed by number only")
    import export_combat
    want = {(t, a) for t, a in export_combat.ATTACK_ANIMS}
    spawn = collections.Counter()
    ids = collections.Counter()
    for _, n, d in entries:
        if not n.endswith(".tae"):
            continue
        tfile = souls.basename(n)[:-4]
        for aid, a in souls.tae(d).items():
            for ev in a["events"]:
                name, vals = souls.tae_named(ev, tpl) if tpl else (None, {"raw": ev[3]})
                label = name or f"type {ev[0]}"
                if tpl and not any(k in (name or "").lower() for k in ("ffx", "sfx", "blade", "trail")):
                    continue
                spawn[label] += 1
                if (tfile, aid) in want:
                    print(f"player {tfile} {aid} {ev[1]:.3f}-{ev[2]:.3f}s {label}: {vals}")
                for k, v in vals.items():
                    if "ffx" in k.lower() or "sfx" in k.lower():
                        ids[v if not isinstance(v, tuple) else v[0]] += 1
    print("player: FFX / SFX events by type over all c0000 animations:", dict(spawn))
    lib = ee.Library(game, "m10_02_00_00")
    acts = collections.Counter()
    for fid, n in ids.most_common(40):
        e = lib.effect(fid) if isinstance(fid, int) and fid > 0 else None
        subs = [NEW.get(s) or OTHER.get(s) or s for s, _, _ in effect_actions(e, lib.effect)] if e else None
        print(f"player effect {fid} x{n}: {subs if subs is not None else 'not in CommonEffects / area bundles'}")
        for s in subs or ():
            acts[s] += 1
    print("player effects' systems by appearance:", dict(acts))


def main():
    game = sys.argv[1]
    maps = sys.argv[2:]
    if not maps:
        import area
        maps = list(area.AREAS) + [m for m in ("m10_00_00_00", "m12_00_00_00", "m18_01_00_00") if m not in area.AREAS]
    total = collections.Counter()
    for m in maps:
        total.update(area_census(game, m))
    print("all areas: systems by appearance:", {(NEW.get(k) or OTHER.get(k) or k): v for k, v in total.items()})
    player_census(game)


if __name__ == "__main__":
    main()
