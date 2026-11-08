#!/usr/bin/env python3
"""Summarise the hitch monitor's EE log lines (src/hitch.c "hitch: t ..." every 10 s of game time) over a run.

    tools/hitchsum.py <emulog.txt> [--window 300] [--json out.json]

Prints one row per window (default 5 min of game time): worst frame, frames over 50 / 100 / 200 ms, re-reads (items
read again after a drop), evictions of wanted items, LOD flips / flip-flops, the lowest free heap, the smallest
"largest block" malloc found, the smallest largest free run of the streaming arena; then the ten worst periods with
their section split and the cells that flipped most. The last row compares the first and the last window
(degradation over time). Used by tools/test-run.py --hitch (gates), play-report.sh (summary.txt in a play-test report) and by hand."""
import argparse, collections, json, re, sys

NUM = r"(\d+)"
LINE = re.compile(r"hitch: t (\d+) s frames (\d+) avg (\d+) max (\d+) ms over50 (\d+) over100 (\d+) over200 (\d+) \| worst "
                  r"(.*?) \| heap free (\d+) KB largest (\d+) KB \| arena free (\d+) KB run (\d+) KB other (\d+) KB(?: lent \d+ KB)? \| reads "
                  r"(\d+) \((\d+) KB\) again (\d+) \((\d+) KB\)(?: thrash (\d+) \((\d+) KB\))? drops (\d+) evicted (\d+) \((\d+) KB\) \| blocks (\d+) "
                  r"\((\d+) KB\) \| lod flips (\d+) flops (\d+)(?: \| loads (\d+) longest (\d+) ms \|)?"
                  r"(?: spikes (\d+) \(\+(\d+)\) \| allocfail (\d+) \(max (\d+) KB\) \| at (-?\d+) (-?\d+) (-?\d+) block (-?\d+) \|)? top(.*)")
# the streamer's Burg gates (plan step B0), one line a period after the hitch line
STREAM = re.compile(r"stream: t (\d+) s room (\d+) near (\d+)(?: items (\d+))? \| due near drawn coarse median (\d+) max (\d+) \| "
                    r"hzn inside ([\d.]+) s \| pop-ins (\d+)")
KEYS = ["t", "frames", "avg", "max", "o50", "o100", "o200", "worst", "heap", "largest", "afree", "arun", "other",
        "reads", "read_kb", "again", "again_kb", "thrash", "thrash_kb", "drops", "evicts", "evict_kb", "blocks", "block_kb", "flips", "flops",
        "loads", "load_max", "spikes", "spikes_cut", "allocfail", "allocfail_kb", "x", "y", "z", "block", "top"]


def parse(path):
    rows = []
    for ln in open(path, errors="replace"):
        m = LINE.search(ln)
        if not m:
            continue
        r = dict(zip(KEYS, m.groups()))
        for k in KEYS:
            if k not in ("worst", "top"):
                r[k] = int(r[k] or 0)
        r["top"] = [(c, int(n)) for c, n in re.findall(r"(\d+\.\d+) x(\d+)", r["top"])]
        rows.append(r)
    return rows


def parse_stream(path):
    """the "stream:" lines -> [{"t", "room", "room_near", "due_med", "due_max", "hzn_s", "popins"}]"""
    out = []
    for ln in open(path, errors="replace"):
        m = STREAM.search(ln)
        if m:
            g = m.groups()
            items = int(g[3]) if g[3] is not None else -1
            g = g[:3] + g[4:]
            out.append({"t": int(g[0]), "room": int(g[1]), "room_near": int(g[2]), "near_items": items,
                        "due_med": int(g[3]), "due_max": int(g[4]), "hzn_s": float(g[5]), "popins": int(g[6])})
    return out


def stream_summary(rows, t_min=30):
    """run totals of the "stream:" lines after the first t_min s (the boot): refusals, near refusals, pop-ins, the
    median of the periods' due-near-drawn-coarse medians and the largest, seconds and share of time with a horizon-filled
    cell inside HZN_R"""
    rows = [r for r in rows if r["t"] > t_min]
    if not rows:
        return {}
    secs = max(1, rows[-1]["t"] - rows[0]["t"] + 10)
    meds = sorted(r["due_med"] for r in rows)
    hz = sum(r["hzn_s"] for r in rows)
    return {"periods": len(rows), "room": sum(r["room"] for r in rows), "room_near": sum(r["room_near"] for r in rows),
            "near_items": sum(r["near_items"] for r in rows) if all(r["near_items"] >= 0 for r in rows) else None,
            "popins": sum(r["popins"] for r in rows), "due_med": meds[len(meds) // 2],
            "due_max": max(r["due_max"] for r in rows), "hzn_s": round(hz, 1), "hzn_pct": round(100 * hz / secs, 1)}


def window_stats(rows):
    if not rows:
        return {}
    s = lambda k: sum(r[k] for r in rows)
    return {"t0": rows[0]["t"], "t1": rows[-1]["t"], "frames": s("frames"), "max": max(r["max"] for r in rows),
            "o50": s("o50"), "o100": s("o100"), "o200": s("o200"),
            "avg": round(sum(r["avg"] * r["frames"] for r in rows) / max(1, s("frames")), 1),
            "again": s("again"), "again_kb": s("again_kb"), "thrash_kb": s("thrash_kb"), "evicts": s("evicts"), "evict_kb": s("evict_kb"),
            "read_kb": s("read_kb"), "block_kb": s("block_kb"), "flips": s("flips"), "flops": s("flops"),
            "loads": s("loads"), "load_max": max(r["load_max"] for r in rows),
            "spikes": s("spikes") + s("spikes_cut"), "allocfail": s("allocfail"),
            "heap_min": min(r["heap"] for r in rows), "largest_min": min(r["largest"] for r in rows),
            "arun_min": min(r["arun"] for r in rows), "afree_min": min(r["afree"] for r in rows)}


def summarize(rows, window=300):
    wins = collections.OrderedDict()
    for r in rows:
        wins.setdefault(r["t"] // window, []).append(r)
    out = {"windows": [window_stats(w) for w in wins.values()], "all": window_stats(rows)}
    cells = collections.Counter()
    for r in rows:
        for c, n in r["top"]:
            cells[c] += n
    out["top_cells"] = cells.most_common(10)
    out["worst"] = [{"t": r["t"], "max": r["max"], "split": r["worst"], "at": f"{r['x']} {r['y']} {r['z']} block {r['block']}"} for r in sorted(rows, key=lambda r: -r["max"])[:10]]
    w = out["windows"]
    if len(w) >= 2:
        a, b = w[0], w[-1]
        out["drift"] = {k: (a[k], b[k]) for k in ("max", "o50", "o100", "again", "flops", "heap_min", "largest_min",
                                                   "arun_min")}
    return out


COLS = ["t0", "max", "avg", "o50", "o100", "o200", "loads", "load_max", "again", "thrash_kb", "evicts", "read_kb", "block_kb",
        "flips", "flops", "spikes", "allocfail",
        "heap_min", "largest_min", "arun_min"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--window", type=int, default=300)
    ap.add_argument("--json")
    a = ap.parse_args()
    rows = parse(a.log)
    if not rows:
        sys.exit("no hitch: lines")
    s = summarize(rows, a.window)
    print(" ".join(f"{c:>9}" for c in COLS))
    for w in s["windows"] + [s["all"]]:
        print(" ".join(f"{w[c]:>9}" for c in COLS))
    print("worst periods:")
    for x in s["worst"]:
        print(f"  t {x['t']:5} max {x['max']:4} ms: {x['split']} (at {x['at']})")
    print("cells flipping most:", ", ".join(f"{c} x{n}" for c, n in s["top_cells"]))
    if "drift" in s:
        print("first -> last window:", ", ".join(f"{k} {v[0]} -> {v[1]}" for k, v in s["drift"].items()))
    st = stream_summary(parse_stream(a.log))
    if st:
        s["stream"] = st
        print("stream (after 30 s): " + ", ".join(f"{k} {v}" for k, v in st.items()))
    if a.json:
        json.dump(s, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
