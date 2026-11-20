#!/usr/bin/env python3
"""Golden routes (Polaris phase 0.3): a fixed set of short ISO test routes, recorded once as the base and compared after a
change: picture (GS dump replays), frame time and streaming refusals per route.

  golden.py record  [--routes a,b] [--elf E] [--iso I] [--out DIR]
  golden.py compare [--routes a,b] [--elf E] [--iso I] [--out DIR] [--no-run] [--record-missing] [--gate]
                    [--img-fail F] [--p90-ms MS] [--max-ms MS] [--refusals N]

Routes: ps2/proto/tests/golden_<route>.txt (ROUTES below), each ~30-60 s of game time, fixed cameras, no enemies (shots
must not depend on AI), booted from the ISO as a player does (tools/test-run.py --iso --gsdump, warm starts).
  record   runs every route into <out>/base/<route>/ (shots, GS dumps, result.json, frames.csv, emulog.txt) and writes
           its summary.json there: frame p50 / p90 (the shots' segment percentiles, frame-weighted), the worst frame,
           map / logic pass p90, the streamer's "no room" refusals, near refusals and pop-ins (tools/hitchsum.py
           stream lines, the whole run) and the run's errors.
  compare  runs every route again into <out>/new/<route>/ (--no-run: compare what is there), then per route:
           tools/test-gsdump.py compare new vs base (replayed GS dumps: the share of changed pixels per shot, report in
           <out>/new/<route>/gsdump.html), frame p50 / p90 / max and refusals vs the base. A route without a base is
           recorded as the base with --record-missing (tools/ci/pcsx2-smoke.sh), else skipped.
           Prints one table; exit code 0, or with --gate the number of routes over a threshold.
Thresholds (defaults just above the run-to-run noise of one build: record + two compares, see THRESH): a shot's
changed pixels > --img-fail, frame p90 up > --p90-ms, worst frame up > --max-ms, refusals up > --refusals.
Every PCSX2 job goes through tools/q (-m 2). Results stay in build/golden/ (git-ignored: game pictures)."""
import argparse, glob, json, os, re, shutil, subprocess, sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import hitchsum  # noqa: E402

ROUTES = ["asylum", "firelink", "burg_west", "burg_bonfire", "taurus", "parish_seam"]
# Run-to-run noise of one build (TECH_DEBT test-10), record + 2 compares each of main f3689ac (2026-11-18) and
# c112f8b (2026-11-19): worst shot 1.14 % changed pixels (asylum/cell: the player's idle animation phase; every other
# shot <= 0.60 %), frame p50 / p90 unchanged (vsync-quantised at 33.5 ms), worst frame +0.7 ms, refusals and pop-ins
# unchanged. Limits: about 1.75x the picture noise, p90 a third of a vsync, the worst frame below one extra vsync
# (16.7 ms), refusals a couple of items.
THRESH = {"img": 0.02, "p90_ms": 1.0, "max_ms": 10.0, "refusals": 2}


def q(cmd, log):
    """one PCSX2 job through the job queue; returns its exit code"""
    with open(log, "w") as f:
        return subprocess.call([os.path.join(TOOLS, "q"), "-m", "2"] + cmd, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT)


def run_route(route, elf, iso, out):
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    script = os.path.join(ROOT, "ps2", "proto", "tests", f"golden_{route}.txt")
    rc = q([sys.executable, os.path.join(TOOLS, "test-run.py"), elf, script, out, "--iso", iso, "--gsdump",
            "--timeout", "600", "--keep-going", "--name", route], out + ".log")
    summ = summary(out)
    summ["rc"] = rc
    json.dump(summ, open(os.path.join(out, "summary.json"), "w"), indent=1)
    return summ


def summary(run):
    """frame-time and streaming numbers of one route run (see the module doc)"""
    s = {"shots": 0, "frames": 0, "p50": 0.0, "p90": 0.0, "max": 0.0, "map_p90": 0.0, "logic_p90": 0.0, "errors": None}
    try:
        r = json.load(open(os.path.join(run, "result.json")))
    except (OSError, ValueError):
        return s
    s["errors"] = len(r.get("errors") or [])
    acc = {"p50": 0.0, "p90": 0.0}
    for sh in r.get("shots", []):
        st = sh.get("stats") or {}
        p = st.get("passes") or {}
        n = p.get("n", 0)
        if not n:
            continue
        s["shots"] += 1
        s["frames"] += n
        acc["p50"] += p.get("frame_p50", 0) * n
        acc["p90"] += p.get("frame_p90", 0) * n
        s["max"] = max(s["max"], st.get("ms_max", 0))
        s["map_p90"] = max(s["map_p90"], p.get("map_p90", 0))
        s["logic_p90"] = max(s["logic_p90"], p.get("logic_p90", 0))
    if s["frames"]:
        s["p50"], s["p90"] = acc["p50"] / s["frames"], acc["p90"] / s["frames"]
    log = os.path.join(run, "emulog.txt")
    if os.path.exists(log):
        rows = []
        for ln in open(log, errors="replace"):
            m = hitchsum.STREAM.search(ln)
            if m:
                rows.append(m)
        s["room"] = sum(int(m[2]) for m in rows)
        s["room_near"] = sum(int(m[3]) for m in rows)
        s["popins"] = sum(int(m[8]) for m in rows)
        s["refusals"] = s["room"]
    return s


def gsdump_compare(new, base):
    report = os.path.join(new, "gsdump.html")
    q([sys.executable, os.path.join(TOOLS, "test-gsdump.py"), "compare", new, base, report, "--fail", "1"],
      os.path.join(new, "gsdump.log"))
    try:
        return json.load(open(os.path.splitext(report)[0] + ".json"))
    except (OSError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["record", "compare"])
    ap.add_argument("--routes", default=",".join(ROUTES))
    ap.add_argument("--elf", default=os.path.join(ROOT, "build", "proto.sym.elf"))
    ap.add_argument("--iso", default=os.path.join(ROOT, "build", "proto.iso"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "golden"))
    ap.add_argument("--no-run", action="store_true", help="compare: use the runs already in <out>/new")
    ap.add_argument("--record-missing", action="store_true", help="compare: record a route without a base as the base")
    ap.add_argument("--gate", action="store_true", help="exit code = routes over a threshold")
    ap.add_argument("--img-fail", type=float, default=THRESH["img"])
    ap.add_argument("--p90-ms", type=float, default=THRESH["p90_ms"])
    ap.add_argument("--max-ms", type=float, default=THRESH["max_ms"])
    ap.add_argument("--refusals", type=int, default=THRESH["refusals"])
    a = ap.parse_args()
    routes = [r for r in a.routes.split(",") if r]
    elf, iso = os.path.realpath(a.elf), os.path.realpath(a.iso)
    for p in (elf, iso):
        if not os.path.exists(p):
            sys.exit(f"golden.py: {p} missing (make, make sym, make iso)")
    if a.cmd == "record":
        for r in routes:
            s = run_route(r, elf, iso, os.path.join(a.out, "base", r))
            print(f"{r:14} rc {s['rc']} shots {s['shots']} frame p50 {s['p50']:.2f} p90 {s['p90']:.2f} max {s['max']:.1f} "
                  f"refusals {s.get('refusals')} pop-ins {s.get('popins')} errors {s['errors']}")
        return 0
    bad = 0
    print(f"{'route':14} {'img worst':>10} {'p50':>13} {'p90':>13} {'max':>13} {'refusals':>9} {'pop-ins':>8} verdict")
    for r in routes:
        base, new = os.path.join(a.out, "base", r), os.path.join(a.out, "new", r)
        if not os.path.exists(os.path.join(base, "summary.json")):
            if a.record_missing:
                s = run_route(r, elf, iso, base)
                print(f"{r:14} no base: recorded (rc {s['rc']}, {s['shots']} shots)")
            else:
                print(f"{r:14} no base (golden.py record)")
            continue
        b = json.load(open(os.path.join(base, "summary.json")))
        n = summary(new) if a.no_run else run_route(r, elf, iso, new)
        g = gsdump_compare(new, base)
        worst = max((x["changed"] for x in g["rows"]), default=0) if g else None
        over = []
        if g is None or not g["compared"]:
            over.append("no shots compared")
        elif worst > a.img_fail:
            over.append("picture")
        if n["p90"] - b["p90"] > a.p90_ms:
            over.append("p90")
        if n["max"] - b["max"] > a.max_ms:
            over.append("max")
        if (n.get("refusals") or 0) - (b.get("refusals") or 0) > a.refusals:
            over.append("refusals")
        if n.get("errors"):
            over.append(f"{n['errors']} errors")
        bad += bool(over)
        img = f"{worst * 100:.2f} %" if worst is not None else "-"
        print(f"{r:14} {img:>10} {b['p50']:5.2f}->{n['p50']:5.2f} {b['p90']:5.2f}->{n['p90']:5.2f} "
              f"{b['max']:5.1f}->{n['max']:5.1f} {b.get('refusals', 0):>3}->{n.get('refusals', 0):<4} "
              f"{b.get('popins', 0):>3}->{n.get('popins', 0):<3} {', '.join(over) or 'ok'}")
    print(f"thresholds: picture {a.img_fail * 100:.1f} % changed pixels in a shot, p90 +{a.p90_ms} ms, max +{a.max_ms} ms, "
          f"refusals +{a.refusals}; {bad} route(s) over")
    return bad if a.gate else 0


if __name__ == "__main__":
    sys.exit(main())
