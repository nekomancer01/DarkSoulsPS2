#!/usr/bin/env python3
"""mapperf.py: A/B tool for the map draw work (mapdraw plan). Reads tools/test-run.py run dirs (emulog.txt, frames.csv).

  mapperf.py summary DIR...                  per route: frames, "TEST passes" map p50 / p90 (frame-weighted), mean
                                             "map: pass ms" / plan / other split and the corrected values
  mapperf.py ab BASE_DIR... -- NEW_DIR...    per route side by side: delta, noise band, faster / noise / slower for the
                                             timed mean and the untimed p50 (+ rep3 when |delta| <= 1.5 x band)
  mapperf.py work BASE_DIR NEW_DIR --count C operation count C per frame ("map: work per frame" line or an
                                             "idx_fast N" counter) and its change; n/a when the count is not logged
  mapperf.py hash A_DIR B_DIR                "identical (N frames)" or the first differing frame (exit 1)
  mapperf.py --selftest                      runs on tools/testdata/mapperf/

A DIR is a run dir (has emulog.txt) or a folder of run dirs. The route of a run dir is its name without a trailing
-<rep> (world_route-1 -> world_route); runs of the same route are reps. Means of "map: pass ms" lines are over all lines
(each covers the same number of timed frames); p50 / p90 are frame-weighted over the shots ("TEST passes ... n N").
Stdlib only."""
import csv, glob, os, re, sys

PASS_K = ["cull", "plan", "chain", "tex", "bump", "total"]
PLAN_K = ["miss", "near", "chunks", "fx", "other"]
OTHER_K = ["sky", "pict", "far", "hzonly", "prologue", "hzcell", "fades", "zsend", "lvl", "setup", "rest"]
CORR_K = ["cull_c", "far_c", "hzonly_c", "prologue_c"]
WORK_K = ["boxdepth", "tclevel", "sqrt", "far_tested", "hz_tested", "idx_fast"]
NUM = r"(-?[\d.]+)"
RE_PASS = re.compile(r"map: pass ms cull %s plan %s chain %s tex %s bump %s total %s" % ((NUM,) * 6))
RE_PLAN = re.compile(r"map: plan ms miss %s near %s chunks %s fx %s other %s" % ((NUM,) * 5))
RE_PASSES = re.compile(r"TEST passes (\S+) n (\d+) map_p50 %s map_p90 %s" % (NUM, NUM))
RE_CHAIN = re.compile(r"mapchain: f (\d+) qw (\d+) ref (\d+) h ([0-9a-f]{8})")
RE_WORK = re.compile(r"map: work per frame (.*)")
RE_IDX = re.compile(r"\bidx_fast\s+(\d+)")


def run_dirs(paths):
    out = []
    for p in paths:
        p = p.rstrip("/")
        if os.path.isfile(os.path.join(p, "emulog.txt")):
            out.append(p)
        else:
            out += sorted(d for d in glob.glob(os.path.join(p, "*")) if os.path.isfile(os.path.join(d, "emulog.txt")))
    return out


def route_of(d):
    return re.sub(r"-\d+$", "", os.path.basename(d.rstrip("/")))


class Run:
    def __init__(self, d):
        self.dir = d
        self.route = route_of(d)
        self.passes, self.plans, self.others, self.corr, self.shots, self.works = [], [], [], [], [], []
        self.idx, self.chain, self.reads = [], {}, []
        for l in open(os.path.join(d, "emulog.txt"), errors="replace"):
            if "map: " in l:
                m = RE_PASS.search(l)
                if m:
                    self.passes.append([float(x) for x in m.groups()])
                    continue
                m = RE_PLAN.search(l)
                if m:
                    self.plans.append([float(x) for x in m.groups()])
                    continue
                if "map: other ms" in l:
                    kv = dict(re.findall(r"\b([a-z_]+) (-?[\d.]+)(?=[ ;]|$)", l.split("map: other ms")[1]))
                    self.others.append([float(kv.get(k, 0)) for k in OTHER_K])
                    self.corr.append([float(kv.get(k, 0)) for k in CORR_K])
                    continue
                m = RE_WORK.search(l)
                if m:
                    kv = dict(re.findall(r"([a-z_]+) (\d+)", m.group(1)))
                    self.works.append({k: float(v) for k, v in kv.items()})
            m = RE_PASSES.search(l)
            if m:
                self.shots.append((int(m.group(2)), float(m.group(3)), float(m.group(4))))
                continue
            m = RE_CHAIN.search(l)
            if m:
                self.chain[int(m.group(1))] = m.groups()[1:]
                continue
            m = RE_IDX.search(l)
            if m:
                self.idx.append(float(m.group(1)))

    def frames(self):
        return sum(n for n, _, _ in self.shots)

    def wp(self, i):  # frame-weighted percentile column (1 = p50, 2 = p90)
        n = self.frames()
        return sum(s[0] * s[i] for s in self.shots) / n if n else None


def mean_cols(rows, k):
    return [sum(r[i] for r in rows) / len(rows) for i in range(k)] if rows else None


def by_route(paths):
    g = {}
    for d in run_dirs(paths):
        g.setdefault(route_of(d), []).append(Run(d))
    return g


def pooled_mean(runs, attr, k):
    rows = [r for x in runs for r in getattr(x, attr)]
    return mean_cols(rows, k)


def pooled_wp(runs, i):
    n = sum(x.frames() for x in runs)
    return sum(s[0] * s[i] for x in runs for s in x.shots) / n if n else None


def f2(v):
    return "  n/a" if v is None else "%.2f" % v


def cmd_summary(paths):
    g = by_route(paths)
    allruns = [x for rs in g.values() for x in rs]
    if not allruns:
        print("no run dirs (emulog.txt) under", paths)
        return 1
    groups = list(g.items()) + [("ALL", allruns)]
    print("route                 reps frames   p50    p90 | " + " ".join("%6s" % k for k in PASS_K))
    for name, rs in groups:
        p = pooled_mean(rs, "passes", 6)
        print("%-20s %4d %6d %6s %6s | %s" % (name, len(rs), sum(x.frames() for x in rs), f2(pooled_wp(rs, 1)),
              f2(pooled_wp(rs, 2)), " ".join("%6s" % f2(v) for v in (p or [None] * 6))))
    print("\nplan split (map: plan ms)         " + " ".join("%6s" % k for k in PLAN_K))
    for name, rs in groups:
        p = pooled_mean(rs, "plans", 5)
        print("%-32s %s" % (name, " ".join("%6s" % f2(v) for v in (p or [None] * 5))))
    print("\nother split (map: other ms)       " + " ".join("%8s" % k for k in OTHER_K + CORR_K))
    for name, rs in groups:
        o, c = pooled_mean(rs, "others", len(OTHER_K)), pooled_mean(rs, "corr", len(CORR_K))
        print("%-32s %s" % (name, " ".join("%8s" % f2(v) for v in (o or [None] * len(OTHER_K)) + (c or [None] * len(CORR_K)))))
    return 0


def rep_values(runs, fn):
    return [v for v in (fn(x) for x in runs) if v is not None]


def verdict(base, new):
    """base / new: per-rep values. -> (base mean, new mean, delta, band, verdict, rep3)"""
    if not base or not new:
        return None
    bm, nm = sum(base) / len(base), sum(new) / len(new)
    band = max((max(v) - min(v)) if len(v) > 1 else 0.0 for v in (base, new))
    d = nm - bm
    v = "noise" if abs(d) <= band else ("faster" if d < 0 else "slower")
    return bm, nm, d, band, v, abs(d) <= 1.5 * band


def cmd_ab(args):
    if "--" not in args:
        print("usage: mapperf.py ab BASE_DIR... -- NEW_DIR...")
        return 2
    i = args.index("--")
    gb, gn = by_route(args[:i]), by_route(args[i + 1:])
    metrics = [("timed total", lambda x: mean_cols(x.passes, 6)[5] if x.passes else None)]
    metrics += [("timed " + k, (lambda j: lambda x: mean_cols(x.passes, 6)[j] if x.passes else None)(j))
                for j, k in enumerate(PASS_K[:-1])]
    metrics += [("untimed p50", lambda x: x.wp(1)), ("untimed p90", lambda x: x.wp(2))]
    print("%-18s %-12s %7s %7s %7s %6s  %s" % ("route", "metric", "base", "new", "delta", "band", "verdict"))
    for route in sorted(set(gb) | set(gn)):
        if route not in gb or route not in gn:
            print("%-18s only on one side" % route)
            continue
        for name, fn in metrics:
            r = verdict(rep_values(gb[route], fn), rep_values(gn[route], fn))
            if r:
                print("%-18s %-12s %7.2f %7.2f %+7.2f %6.2f  %s%s" % (route, name, r[0], r[1], r[2], r[3], r[4],
                      " rep3" if r[5] else ""))
    return 0


def work_value(run, count):
    if count == "idx_fast" and run.idx:
        return run.idx[-1]
    vals = [w[count] for w in run.works if count in w]
    return sum(vals) / len(vals) if vals else None


def cmd_work(args):
    count = None
    if "--count" in args:
        j = args.index("--count")
        count = args[j + 1]
        args = args[:j] + args[j + 2:]
    if len(args) != 2 or count not in WORK_K:
        print("usage: mapperf.py work BASE_DIR NEW_DIR --count {%s}" % "|".join(WORK_K))
        return 2
    gb, gn = by_route(args[:1]), by_route(args[1:])
    rc = 0
    for route in sorted(set(gb) & set(gn)):
        b = [v for v in (work_value(x, count) for x in gb[route]) if v is not None]
        n = [v for v in (work_value(x, count) for x in gn[route]) if v is not None]
        if not b or not n:
            print("%-18s %s: n/a (count not logged on %s)" % (route, count, "base" if not b else "new"))
            continue
        bm, nm = sum(b) / len(b), sum(n) / len(n)
        print("%-18s %s per frame: base %.1f new %.1f (%+.1f%%)" % (route, count, bm, nm, 100 * (nm - bm) / bm if bm else 0))
    return rc


def kept_frames(d, chain):
    """frames of the mapchain lines that belong to a segment past its first 2 frames (seg_frames > 2, testscript.c)"""
    rows = []
    with open(os.path.join(d, "frames.csv")) as f:
        for r in csv.DictReader(f):
            rows.append((int(r["frame"]), int(r["seg_frames"])))
    rows.sort()
    out, k = set(), 0
    for fr in sorted(chain):
        while k < len(rows) and rows[k][0] < fr:
            k += 1
        if k < len(rows) and rows[k][1] > 0 and fr - (rows[k][0] - rows[k][1] + 1) >= 2:
            out.add(fr)
    return out


def cmd_hash(args):
    if len(args) != 2:
        print("usage: mapperf.py hash A_DIR B_DIR")
        return 2
    a, b = Run(args[0]), Run(args[1])
    ka, kb = kept_frames(args[0], a.chain), kept_frames(args[1], b.chain)
    # the csv rows are time-sampled, so the segment edges of the two runs differ by a few frames: compare the frames
    # both sides count, and call it a difference when that is not most of them
    common = sorted(ka & kb)
    for fr in common:
        if a.chain[fr] != b.chain[fr]:
            print("differs: first at frame %d: A qw %s ref %s h %s, B qw %s ref %s h %s" % ((fr,) + tuple(a.chain[fr]) +
                  tuple(b.chain[fr])))
            # information only: runs drift by a few frames (frame pacing), so also the share of A's frames whose
            # hash B has within +-16 frames
            near = sum(any(g in kb and b.chain[g][2] == a.chain[f][2] for g in range(f - 16, f + 17)) for f in sorted(ka))
            print("  same frame number: %d of %d equal; within +-16 frames: %d of %d of A's frames have B's hash" %
                  (sum(a.chain[f] == b.chain[f] for f in common), len(common), near, len(ka)))
            return 1
    if len(common) < 0.9 * max(len(ka), len(kb)):
        print("differs: only %d of %d / %d counted frames are on both sides" % (len(common), len(ka), len(kb)))
        return 1
    print("identical (%d frames)" % len(common))
    return 0 if common else 1


def selftest():
    td = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata", "mapperf")
    fails = []

    def chk(name, got, want):
        if got != want:
            fails.append("%s: got %r want %r" % (name, got, want))

    gb, gn = by_route([os.path.join(td, "base")]), by_route([os.path.join(td, "new")])
    chk("routes", sorted(gb), ["fast", "same"])
    chk("pass mean base", [round(v, 2) for v in pooled_mean(gb["fast"], "passes", 6)], [1.0, 6.1, 2.0, 1.0, 1.0, 11.1])
    chk("p50 weighted", round(pooled_wp(gb["fast"], 1), 2), 10.32)
    chk("p90 weighted", round(pooled_wp(gb["fast"], 2), 2), 12.32)
    chk("frames", gb["fast"][0].frames(), 400)
    chk("other", [round(v, 2) for v in pooled_mean(gb["fast"], "others", 3)], [0.15, 0.1, 0.05])
    fn = lambda x: mean_cols(x.passes, 6)[5]
    r = verdict(rep_values(gb["fast"], fn), rep_values(gn["fast"], fn))
    chk("fast verdict", (r[4], round(r[2], 2), round(r[3], 2)), ("faster", -1.05, 0.2))
    r = verdict(rep_values(gb["same"], fn), rep_values(gn["same"], fn))
    chk("same verdict", (r[4], r[5]), ("noise", True))
    r = verdict([10.0, 10.2], [10.3])
    chk("slower", r[4], "slower")
    chk("work base", work_value(gb["fast"][0], "boxdepth"), 1000.0)
    chk("work missing", work_value(gb["fast"][0], "idx_fast"), None)
    chk("hash same", cmd_hash([os.path.join(td, "hash", "a"), os.path.join(td, "hash", "b")]), 0)
    chk("hash differs", cmd_hash([os.path.join(td, "hash", "a"), os.path.join(td, "hash", "c")]), 1)
    chk("hash kept", sorted(kept_frames(os.path.join(td, "hash", "a"), Run(os.path.join(td, "hash", "a")).chain)),
        [3, 4, 5, 6])
    for f in fails:
        print("FAIL", f)
    print("selftest: %s" % ("FAILED" if fails else "ok"))
    return 1 if fails else 0


def main():
    a = sys.argv[1:]
    if not a or a[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if a[0] == "--selftest":
        return selftest()
    cmds = {"summary": cmd_summary, "ab": cmd_ab, "work": cmd_work, "hash": cmd_hash}
    if a[0] not in cmds:
        print(__doc__)
        return 2
    return cmds[a[0]](a[1:])


if __name__ == "__main__":
    sys.exit(main())
