"""Check DSR's event flag id layout (ps2/proto/src/flags.h) against every flag id the game data uses: event ids and
"Event Flag" args (EMEDF arg names) of all EMEVDs, InitializeEvent extra args that fit the layout, and the flag fields of
ItemLotParam / ObjActParam / ShopLineupParam. Prints the ids outside the layout and the (group, area, section) count
(flags.c pool sizing).   python3 tools/flag_scan.py [--json out.json]"""
import collections, glob, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emevd, gamepath, params

GROUPS = {0, 1, 5, 6, 7}
AREAS = {0, 100, 101, 102, 110, 120, 121, 130, 131, 132, 140, 141, 150, 151, 160, 170, 180, 181}


def in_layout(f):
    g, a = f // 10 ** 7, (f // 10 ** 4) % 1000
    return 0 <= f < 10 ** 8 and g in GROUPS and a in AREAS and (g or not a)


def main():
    ids, src = collections.Counter(), collections.defaultdict(set)
    evdir = os.path.join(gamepath.game_dir(), "event")
    maps = sorted(os.path.basename(p).split(".")[0] for p in glob.glob(evdir + "/*.emevd.dcx"))
    for m in maps:
        for ev in emevd.load(m).events:
            if ev.id > 0:
                ids[ev.id] += 1; src[ev.id].add("event")
            for ins in ev.instrs:
                for v in ins.extra or []:
                    if v >= 10 ** 6 and in_layout(v):
                        ids[v] += 1; src[v].add("InitializeEvent arg")
                d = ins.defn
                if not d:
                    continue
                names = [(e or "") + "|" + (a.get("name") or "") for e, a in zip(d.arg_names_en or [None] * len(d.args), d.args)]
                for n, v in zip(names, ins.args or []):
                    if ("flag" in n.lower() or "フラグ" in n) and isinstance(v, int) and v > 0:
                        ids[v] += 1; src[v].add(n.split("|")[0])
    for stem in ["ItemLotParam", "ObjActParam", "ShopLineupParam"]:
        rows = params.params(stem).all_rows
        fl = [f for f in rows[0].fields if "flag" in f.lower()]
        for r in rows:
            for f in fl:
                v = r[f]
                if isinstance(v, int) and v > 0:
                    ids[v] += 1; src[v].add(f"{stem}.{f}")
    bad = sorted(f for f in ids if not in_layout(f))
    good = sorted(f for f in ids if in_layout(f))
    sections = {f // 1000 for f in good}
    print(f"{len(maps)} EMEVDs: {len(ids)} distinct flag ids, {len(good)} in the layout, {len(bad)} outside, "
          f"{len(sections)} sections used")
    for f in bad:
        print(" outside", f, sorted(src[f])[:3])
    if "--json" in sys.argv:
        json.dump(good, open(sys.argv[sys.argv.index("--json") + 1], "w"))


if __name__ == "__main__":
    main()
