#!/usr/bin/env python3
"""Frame and disc budget check of tools/test-run.py results (the budget doc's "Frame time budget (final)", 2026-11-08).

  budget.py <run dir | result.json> ... [--json out.json] [--no-disc]
Every result.json under each argument (a test-run.py out dir, a test-all.sh run, a folder of ISO runs) is checked; the
exit code is the number of results that fail. tools/test-report.py shows the same table in report.html.

Per shot (the ELF's "TEST passes" line, src/testscript.c pb_print: p50 / p90 of each pass over the shot's segment):
  each pass at or under its budget at p90 (PASS_MS); no more than 1 % of frames over 34 ms; the frame's p99 at or under
  FRAME_P99_MS (one NTSC vsync is 33.37 ms, so the doc's 33.3 ms reads as "never a second vsync": 34 ms);
  RAM >= 1.5 MB free (ram_min), VRAM >= 128 KB free; with the disc server (ISO runs) IOP RAM >= 250 KB free.
Per run (EE log): no disc layer change during play (the "disc:" lines without "(load)"), seeks <= 2 a second on average
  while playing, no stream underruns (dsrsnd "UNDERRUN"). The "map: pass ms" lines (map.c, every 120 frames: the
  average of the timed frames and the worst one, for cull, plan, chain, tex, bump, total) are summarised (p90 of the
  averages, the largest worst), not gated: the map pass as a whole is.
Per run, the Burg plan's streamer gates (step B0; the hitch monitor's "stream:" lines, tools/hitchsum.py stream_summary,
  after the first 30 s): near-class "no room" refusals, near pop-ins seen on screen, the median of cells drawn coarse
  while due near, and the share of seconds with a horizon-filled cell inside HZN_R. Their thresholds are per test
  script (STREAM_GATES), started at the measured numbers of main so nothing fails at first; a step of the plan may only
  lower one (they ratchet one way).
Segments shorter than MIN_FRAMES frames are listed, not gated (a teleport's settle)."""
import argparse, glob, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# budget doc, "EE per frame" and "VU1, GS and DMA per frame" (ms at p90 per shot)
PASS_MS = {"map": 14.0, "chr": 5.0, "logic": 5.0, "world": 2.0, "fxhud": 1.5, "vuw": 0.5}
FRAME_P99_MS = 34.0
OVER34_PCT = 1.0
RAM_MIN_KB = 1536
VRAM_MIN_KB = 128
IOP_MIN_KB = 250
SEEKS_PER_S = 2.0
MIN_FRAMES = 10
# the Burg plan's streamer gates per test script (step B0): the most allowed per run, from two ISO runs of main 6bb3218
# (2026-11-08, tools/test-all.sh --iso --burg --reps 2, the plan's baseline table; near_items re-measured 2026-11-09:
# distinct near items refused, the attempt count room_near swung 2x between identical runs): counts at the larger run + 30 %
# (identical runs differ by that much: near refusals 12 vs 92 on iso_ame_report3), the median + 2 cells, the horizon
# share + 3 points; None = not gated. Lower them as the plan's steps land, never raise them.
STREAM_GATE_KEYS = {"near_items": "near items refused for room (distinct)", "room_near": "near-class \"no room\" refusals", "popins": "near pop-ins",
                    "lent_near": "near parts lent a pool page and dropped for a lite set (MS_LEND)",
                    "due_med": "median cells drawn coarse while due near", "hzn_pct": "% of seconds with horizon inside HZN_R",
                    "cut": "most wanted items one plan cut for room (mapstream.c, step B4)"}
STREAM_GATES = {
    "iso_ame_report3": {"near_items": 16, "popins": 4, "due_med": 11, "hzn_pct": 57.0},
    "iso_burg_soak2": {"near_items": 95, "popins": 50, "due_med": 12, "hzn_pct": 100.0},
    "iso_burg_taurus": {"near_items": 99, "popins": 36, "due_med": 8, "hzn_pct": 100.0},
    "world_route": {"near_items": 162, "popins": 60, "due_med": 3, "hzn_pct": 93.9},
    "world_sprint": {"near_items": 151, "popins": 89, "due_med": 4, "hzn_pct": 95.9},
    "iso_burg_soak": None,  # (the 8-lap soak: not measured for the baseline)
}
MEDIA = {0: "-", 1: "cd", 2: "dvd", 3: "dvd9"}

MAP_PASS = re.compile(r"map: pass ms (.*)")
DISC = re.compile(r"disc: t (\d+) s media (\S+) kbps (\d+) seeks (\d+) far (\d+) \(([\d.]+)/s\) layer (\d+)( \(load\))? \| "
                  r"ee reads (\d+) wait avg (\d+) max (\d+) ms \| stream wait max (\d+) ms over100 (\d+) \| cache (\d+) KB "
                  r"hits (\d+) miss (\d+) \| busy (\d+) % \| iop free (\d+) KB")


def pct(vals, p):
    v = sorted(vals)
    return v[min(len(v) - 1, int(p / 100 * len(v)))] if v else None


def log_summary(log):
    """the run's "disc:" and "map: pass ms" lines -> dict"""
    disc, mp, under = [], {}, 0
    try:
        lines = open(log, errors="replace").read().splitlines()
    except OSError:
        return None
    for line in lines:
        m = DISC.search(line)
        if m:
            g = m.groups()
            disc.append({"t": int(g[0]), "media": g[1], "kbps": int(g[2]), "seeks": int(g[3]), "far": int(g[4]),
                         "seeks_s": float(g[5]), "layer": int(g[6]), "load": bool(g[7]), "reads": int(g[8]),
                         "wait_avg": int(g[9]), "wait_max": int(g[10]), "stream_max": int(g[11]), "over100": int(g[12]),
                         "cache_kb": int(g[13]), "hits": int(g[14]), "miss": int(g[15]), "busy": int(g[16]),
                         "iop_free": int(g[17])})
        m = MAP_PASS.search(line)
        if m:  # "cull A plan B ... total F | worst cull ... total F (N timed frames)": averages, then the worst frame
            avg, _, worst = m[1].partition("|")
            for part, key in ((avg, "avg"), (worst.replace("worst", "", 1).split("(")[0], "max")):
                t = part.split()
                for k, v in zip(t[::2], t[1::2]):
                    try:
                        mp.setdefault(k, {"avg": [], "max": []})[key].append(float(v.split("/")[0]))
                    except ValueError:
                        pass
        if "UNDERRUN" in line and "dsrsnd" in line:
            under += 1
    play = [d for d in disc if not d["load"]]
    out = {"underruns": under, "map_pass": {k: {"p90_avg": pct(v["avg"], 90), "max": max(v["max"]) if v["max"] else None}
                                            for k, v in mp.items()}}
    if disc:
        n = max(1, len(play))
        out["disc"] = {
            "media": disc[-1]["media"], "periods": len(disc), "play_periods": len(play),
            "kbps": round(sum(d["kbps"] for d in play) / n), "seeks_s": round(sum(d["seeks_s"] for d in play) / n, 2),
            "far_seeks": sum(d["far"] for d in disc), "layer_play": sum(d["layer"] for d in play),
            "layer_load": sum(d["layer"] for d in disc if d["load"]),
            "wait_avg": round(sum(d["wait_avg"] for d in play) / n), "wait_max": max(d["wait_max"] for d in disc),
            "stream_wait_max": max(d["stream_max"] for d in disc), "stream_over100": disc[-1]["over100"],
            "cache_kb": disc[-1]["cache_kb"], "cache_hits": sum(d["hits"] for d in disc),
            "cache_miss": sum(d["miss"] for d in disc), "busy_pct": round(sum(d["busy"] for d in play) / n),
            "iop_free_min": min(d["iop_free"] for d in disc)}
    return out


def script_key(res, d):
    sc = res.get("script") or res.get("name") or os.path.basename(d)
    return os.path.splitext(os.path.basename(sc))[0]


def check_result(path, disc_gates=True):
    """one result.json -> {"name", "shots": [...], "run": {...}, "fails": [...]}"""
    res = json.load(open(path))
    d = os.path.dirname(path)
    out = {"name": res.get("name") or os.path.basename(d), "dir": d, "shots": [], "fails": []}
    for s in res.get("shots", []):
        st = s.get("stats", {})
        p = st.get("passes")
        row = {"shot": s["name"], "fails": []}
        if not p:
            row["note"] = "no TEST passes line (ELF before the budget passes)"
            out["shots"].append(row)
            continue
        n = int(p.get("n", 0))
        row.update(n=n, **{k: p.get(k) for k in p if k.endswith("_p90") or k.endswith("_p50")},
                   frame_p99=p.get("frame_p99"), over34=int(p.get("over34", 0)), ram_min=st.get("ram_min"),
                   vram=st.get("vram"), secs=p.get("secs"))
        for k in ("disc_kb", "seeks", "far_seeks", "layer_changes", "wait_avg", "cache_hits", "cache_miss", "iop_free",
                  "media"):
            if k in p:
                row[k] = p[k]
        if row.get("secs"):
            row["kbps"] = round(row.get("disc_kb", 0) / row["secs"]) if "disc_kb" in row else None
            row["seeks_s"] = round(row.get("seeks", 0) / row["secs"], 2) if "seeks" in row else None
        if n < MIN_FRAMES:
            row["note"] = f"{n} frames: not gated"
            out["shots"].append(row)
            continue
        f = row["fails"]
        for k, b in PASS_MS.items():
            v = p.get(k + "_p90")
            if v is not None and v > b:
                f.append(f"{k} p90 {v:.2f} > {b} ms")
        if (row["frame_p99"] or 0) > FRAME_P99_MS:
            f.append(f"frame p99 {row['frame_p99']:.1f} > {FRAME_P99_MS} ms")
        if row["over34"] * 100 > OVER34_PCT * n:
            f.append(f"{row['over34']} of {n} frames over 34 ms ({100 * row['over34'] / n:.1f} % > {OVER34_PCT} %)")
        ram = row["ram_min"]
        if ram is not None and 0 < ram < 0x7FFFFFFF and ram / 1024 < RAM_MIN_KB:
            f.append(f"RAM free {ram / 1024:.0f} KB < {RAM_MIN_KB} KB")
        if row["vram"] is not None and 0 <= row["vram"] / 1024 < VRAM_MIN_KB:
            f.append(f"VRAM free {row['vram'] / 1024:.0f} KB < {VRAM_MIN_KB} KB")
        if disc_gates and row.get("iop_free") and row["iop_free"] < IOP_MIN_KB:
            f.append(f"IOP free {row['iop_free']:.0f} KB < {IOP_MIN_KB} KB")
        out["shots"].append(row)
        out["fails"] += [f"{s['name']}: {x}" for x in f]
    run = log_summary(os.path.join(d, "emulog.txt")) or {}
    out["run"] = run
    if disc_gates and run.get("disc"):
        dd = run["disc"]
        if dd["layer_play"]:
            out["fails"].append(f"run: {dd['layer_play']} disc layer changes during play (budget: none)")
        if dd["seeks_s"] > SEEKS_PER_S:
            out["fails"].append(f"run: {dd['seeks_s']} seeks/s while playing > {SEEKS_PER_S}")
    try:
        import hitchsum
        st = hitchsum.stream_summary(hitchsum.parse_stream(os.path.join(d, "emulog.txt")))
    except OSError:
        st = {}
    if st:
        run["stream"] = st
        key = script_key(res, d)
        out["script"] = key
        for k, b in (STREAM_GATES.get(key) or {}).items():
            if b is not None and (st.get(k) or 0) > b:
                out["fails"].append(f"run: {STREAM_GATE_KEYS[k]} {st[k]} > {b} ({key})")
    if run.get("underruns"):
        out["fails"].append(f"run: {run['underruns']} stream underruns")
    return out


def results(paths):
    for a in paths:
        if a.endswith(".json"):
            yield a
        else:
            yield from sorted(glob.glob(os.path.join(a, "**", "result.json"), recursive=True))


def table(r):
    """plain-text summary of one checked result"""
    o = [f"{r['name']} ({r['dir']}): {'FAIL' if r['fails'] else 'PASS'}"]
    for s in r["shots"]:
        if "map_p90" not in s:
            o.append(f"  {s['shot']:24s} {s.get('note', '')}")
            continue
        disc = f" disc {s.get('kbps')} KB/s {s.get('seeks_s')} seeks/s layer {int(s.get('layer_changes', 0))} " \
               f"hits {int(s.get('cache_hits', 0))}" if "disc_kb" in s else ""
        o.append(f"  {s['shot']:24s} n {s['n']:4d} map {s['map_p90']:5.2f} chr {s['chr_p90']:5.2f} logic "
                 f"{s['logic_p90']:5.2f} world {s['world_p90']:5.2f} fx {s['fxhud_p90']:4.2f} vuw {s['vuw_p90']:4.2f} "
                 f"p99 {s['frame_p99']:5.1f} >34 {s['over34']:3d}{disc}{'  ' + s['note'] if s.get('note') else ''}")
    run = r.get("run", {})
    if run.get("disc"):
        d = run["disc"]
        o.append(f"  disc ({d['media']}, {d['play_periods']} periods of play): {d['kbps']} KB/s, {d['seeks_s']} seeks/s, "
                 f"{d['far_seeks']} far, layer changes {d['layer_play']} play / {d['layer_load']} load, EE wait avg "
                 f"{d['wait_avg']} max {d['wait_max']} ms, stream wait max {d['stream_wait_max']} ms, cache "
                 f"{d['cache_kb']} KB {d['cache_hits']} hits / {d['cache_miss']} misses, busy {d['busy_pct']} %, "
                 f"IOP free min {d['iop_free_min']} KB")
    if run.get("map_pass"):
        o.append("  map pass p90 (30-frame avgs): " + " ".join(
            f"{k} {v['p90_avg']:.2f}" + (f"/{v['max']:.1f}" if v["max"] is not None else "")
            for k, v in run["map_pass"].items() if v["p90_avg"] is not None))
    if run.get("stream"):
        st = run["stream"]
        o.append(f"  stream ({st['periods']} periods): no room {st['room']} (near {st['room_near']}, {st.get('near_items')} items), pop-ins "
                 f"{st['popins']}, lent near dropped {st.get('lent_near', 0)}, due near drawn coarse median {st['due_med']} max {st['due_max']}, horizon inside "
                 f"HZN_R {st['hzn_s']} s ({st['hzn_pct']} %), plan cut at most {st.get('cut', 0)} items in a plan (near {st.get('cut_near', 0)})")
    o += [f"  FAIL {x}" for x in r["fails"]]
    return "\n".join(o)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--json")
    ap.add_argument("--no-disc", action="store_true", help="skip the disc gates (layer changes, seeks, IOP RAM)")
    a = ap.parse_args()
    rs = [check_result(p, not a.no_disc) for p in results(a.paths)]
    for r in rs:
        print(table(r))
    if a.json:
        json.dump(rs, open(a.json, "w"), indent=1)
    nf = sum(1 for r in rs if r["fails"])
    print(f"budget: {len(rs)} runs, {nf} over budget")
    sys.exit(min(nf, 125))


if __name__ == "__main__":
    main()
