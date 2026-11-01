#!/usr/bin/env python3
"""Perf history + regression check over the smoke test's per-spot stats.

  test-perf.py record <smoke dir> <history.csv> <elf>...  append this build's spots (build id = sha256[:12] of the ELFs)
  test-perf.py check <history.csv> <elf>... [--json f]   compare this build with the previous one

No git in this tree, so a build is keyed by the ELF's hash (plus the time). Per spot (area/shot) the newest row of the
current build is compared with the newest row of the previous different build; a regression is:
  fps (segment average)   more than 10 % lower       ms_max (worst frame)   more than 20 % and 2 ms higher
  map_max (map pass ms)   more than 20 % and 1 ms higher
  ram_min (EE RAM free)   more than 256 KB lower     vram (VRAM free)       more than 64 KB lower
  chain_map / chain_chr   (VU1 chain qwords this segment) more than 15 % higher
PCSX2 does not time VU1, GS fill or the EE caches: these numbers track EE work and emulated frame pacing only."""
import argparse, csv, hashlib, json, os, sys, time

FIELDS = ["time", "build", "area", "spot", "fps", "ms_avg", "ms_max", "map_max", "ram_min", "vram", "chain_map",
          "chain_chr", "tris"]
# field: (direction, relative limit, absolute limit) ; direction -1 = lower is worse
RULES = {"fps": (-1, 0.10, 0), "ms_max": (1, 0.20, 2.0), "map_max": (1, 0.20, 1.0), "ram_min": (-1, 0, 256 * 1024),
         "vram": (-1, 0, 64 * 1024), "chain_map": (1, 0.15, 0), "chain_chr": (1, 0.15, 0)}


def build_id(elfs):
    """sha256[:12] over the ELFs (one per area) in the given order"""
    h = hashlib.sha256()
    for e in elfs:
        h.update(open(e, "rb").read())
    return h.hexdigest()[:12]


def record(smoke_dir, hist, elf):
    s = json.load(open(os.path.join(smoke_dir, "smoke.json")))
    new = not os.path.exists(hist)
    bid, now = build_id(elf), time.strftime("%Y-%m-%d %H:%M:%S")
    n = 0
    with open(hist, "a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(FIELDS)
        for area, ar in s["areas"].items():
            for p in ar["points"]:
                if p.get("fps") is None:
                    continue
                w.writerow([now, bid, area, p["name"]] + [p.get(k) for k in FIELDS[4:]])
                n += 1
    return bid, n


def check(hist, elf):
    rows = list(csv.DictReader(open(hist)))
    bid = build_id(elf)
    order = []  # builds in first-seen order
    for r in rows:
        if r["build"] not in order:
            order.append(r["build"])
    if bid not in order:
        return {"build": bid, "error": "build not recorded"}
    i = order.index(bid)
    prev = order[i - 1] if i else None
    cur = {(r["area"], r["spot"]): r for r in rows if r["build"] == bid}
    old = {(r["area"], r["spot"]): r for r in rows if r["build"] == prev} if prev else {}
    out = {"build": bid, "previous": prev, "spots": len(cur), "compared": 0, "regressions": [], "improvements": []}
    for key, r in sorted(cur.items()):
        o = old.get(key)
        if not o:
            continue
        out["compared"] += 1
        for k, (sign, rel, ab) in RULES.items():
            try:
                a, b = float(r[k]), float(o[k])
            except (TypeError, ValueError):
                continue
            delta = (a - b) * sign  # < 0 = worse
            worse = -delta > max(abs(b) * rel, ab) if rel else -delta > ab
            better = delta > max(abs(b) * rel, ab) if rel else delta > ab
            item = {"area": key[0], "spot": key[1], "field": k, "new": a, "old": b}
            if worse:
                out["regressions"].append(item)
            elif better:
                out["improvements"].append(item)
    # area summary: mean fps, worst ms, min RAM / VRAM
    summ = {}
    for (area, _), r in cur.items():
        s = summ.setdefault(area, {"spots": 0, "fps_sum": 0, "ms_max": 0, "ram_min": 1 << 40, "vram_min": 1 << 40})
        s["spots"] += 1
        s["fps_sum"] += float(r["fps"] or 0)
        s["ms_max"] = max(s["ms_max"], float(r["ms_max"] or 0))
        s["ram_min"] = min(s["ram_min"], float(r["ram_min"] or 0))
        s["vram_min"] = min(s["vram_min"], float(r["vram"] or 0))
    out["areas"] = {a: {"spots": s["spots"], "fps_mean": round(s["fps_sum"] / s["spots"], 1), "ms_max": s["ms_max"],
                        "ram_min_kb": int(s["ram_min"] / 1024), "vram_min_kb": int(s["vram_min"] / 1024)}
                    for a, s in summ.items()}
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record")
    r.add_argument("smoke_dir")
    r.add_argument("history")
    r.add_argument("elf", nargs="+")
    c = sub.add_parser("check")
    c.add_argument("history")
    c.add_argument("elf", nargs="+")
    c.add_argument("--json")
    a = ap.parse_args()
    if a.cmd == "record":
        bid, n = record(a.smoke_dir, a.history, a.elf)
        print(f"recorded {n} spots for build {bid}")
        return
    res = check(a.history, a.elf)
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1)
    if "error" in res:
        sys.exit(res["error"])
    print(f"build {res['build']} vs {res['previous']}: {res['compared']}/{res['spots']} spots compared, "
          f"{len(res['regressions'])} regressions, {len(res['improvements'])} improvements")
    for area, s in res["areas"].items():
        print(f"  {area}: {s['spots']} spots, fps mean {s['fps_mean']}, worst frame {s['ms_max']:.1f} ms, "
              f"RAM min {s['ram_min_kb']} KB, VRAM min {s['vram_min_kb']} KB")
    for g in res["regressions"][:30]:
        print(f"  REGRESSION {g['area']}/{g['spot']} {g['field']}: {g['old']:g} -> {g['new']:g}")
    sys.exit(1 if res["regressions"] else 0)


if __name__ == "__main__":
    main()
