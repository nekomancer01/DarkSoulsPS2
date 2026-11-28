#!/usr/bin/env python3
"""Frame and disc budget check of tools/test-run.py results (the budget doc's "Frame time budget (final)", 2026-11-08).

  budget.py <run dir | result.json> ... [--json out.json] [--no-disc] [--ratchet tools/ci/budget_ratchet.json]
Every result.json under each argument (a test-run.py out dir, a test-all.sh run, a folder of ISO runs) is checked; the
exit code is the number of results that fail. tools/test-report.py shows the same table in report.html.

Per shot (the ELF's "TEST passes" line, src/testscript.c pb_print: p50 / p90 of each pass over the shot's segment):
  each pass at or under its budget at p90 (PASS_MS); no more than 1 % of frames over FB_OVER_MS; the frame's p99 at or
  under FRAME_P99_MS (frame_budget.h: 41.7 ms = 2.5 fields, "never a second vsync"; on-time frames read 33.4-34.2 ms
  in these logs, a missed vsync 50 ms; the "over34" field keeps its name);
  RAM >= 1.5 MB free (ram_min), VRAM >= 128 KB free; with the disc server (ISO runs) IOP RAM >= 250 KB free.
Per run (EE log): no disc layer change during play (the "disc:" lines without "(load)"), seeks <= 2 a second on average
  while playing, no stream underruns (dsrsnd "UNDERRUN"). The "map: pass ms" lines (map.c, every 120 frames: the
  average of the timed frames and the worst one, for cull, plan, chain, tex, bump, total) are summarised (p90 of the
  averages, the largest worst), not gated: the map pass as a whole is. The "logic: pass ms" lines (lpass.c, every 120
  frames: player, stream, enemy, ai, nav, esd, move, ragdoll, pose, hits, gc, objects, events, bonfire, other, total)
  the same way (p90 of the averages, the largest single-frame value of each part), not gated: the logic pass is.
Per run, the Burg plan's streamer gates (step B0; the hitch monitor's "stream:" lines, tools/hitchsum.py stream_summary,
  after the first 30 s): near-class "no room" refusals, near pop-ins seen on screen, the median of cells drawn coarse
  while due near, and the share of seconds with a horizon-filled cell inside HZN_R. Their thresholds are per test
  script (STREAM_GATES), started at the measured numbers of main so nothing fails at first; a step of the plan may only
  lower one (they ratchet one way). The "logic: pass ms" lines (lpass.c, every 120
  frames: player, stream, enemy, ai, nav, esd, move, ragdoll, pose, hits, gc, objects, events, bonfire, other, total)
  the same way (p90 of the averages, the largest single-frame value of each part), not gated: the logic pass is.
Segments shorter than MIN_FRAMES frames are listed, not gated (a teleport's settle).
The frame numbers come from ps2/proto/src/frame_budget.h (load_budget), the game's own copy. Per shot also: the longest
  run of missed vsyncs (miss_run <= FB_MISS_RUN) and the worst frame (frame_max <= FB_FRAME_MAX_MS); busy p50/p90/p99
  (the frame minus its vsync waits) is listed, not gated.
--ratchet FILE (CI, tools/ci/pcsx2-smoke.sh): a failing gate passes when FILE allows at least the measured value for its
  (test script, shot or "*" or "run", gate key: map_p90 .. vuw_p90, p99, over34_pct, miss_run, frame_max, *_short_kb,
  the run gates); the exit code is the number of failing gates no entry covers, and "can lower" lines name entries the
  run beat. Entries only go down. gov_pct (the share of frames the frame governor, src/governor.c, spent at level >= 1;
  from the gov1..3 fields) has no budget of its own: a shot fails it only over its ratchet entry."""
import argparse, glob, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FRAME_BUDGET_H = os.path.join(ROOT, "ps2", "proto", "src", "frame_budget.h")
FB_DEFINE = re.compile(r"^#define\s+FB_(\w+)\s+(-?[\d.]+)f?\b", re.M)
FB_PASS = re.compile(r"X\((\w+),\s*(-?[\d.]+)f?\)")


def load_budget(path=FRAME_BUDGET_H):
    """the frame budget of ps2/proto/src/frame_budget.h (the game's own numbers) -> {"frame_ms", "over_ms", "over_pct",
    "p99_ms", "miss_run", "frame_max_ms", "passes": {name: ms at p90}}; every other FB_ number too, lower case"""
    try:
        src = open(path).read()
    except OSError as e:
        raise SystemExit(f"budget: {path}: {e.strerror}")
    m = re.search(r"^#define\s+FB_PASSES\(X\)(.*)$", src, re.M)
    if not m:
        raise SystemExit(f"budget: {path}: no FB_PASSES")
    out = {k.lower(): float(v) for k, v in FB_DEFINE.findall(src)}
    out["miss_run"] = int(out.get("miss_run", 1))
    out["passes"] = {k: float(v) for k, v in FB_PASS.findall(m[1])}
    return out


# the gated numbers: src/frame_budget.h (budget doc, "EE per frame" and "VU1, GS and DMA per frame"; ms at p90 per shot)
FB = load_budget()
PASS_MS = FB["passes"]
FRAME_P99_MS = FB["p99_ms"]
OVER34_PCT = FB["over_pct"]
RAM_MIN_KB = 1536
VRAM_MIN_KB = 128
IOP_MIN_KB = 250
SEEKS_PER_S = 2.0  # logged as a note since Burg plan B8b (it assumed 128 KB runs; measured: 1.2 items of ~30 KB)
# Burg plan B8b disc gate, as drive time per period of play: seeks inside the fast window x SEEK_MS + far seeks x
# FAR_SEEK_MS + bytes at DRIVE_KBPS (ASSUMED: Sony's DVD short / long seek and sustained rate figures), the p90 over
# the periods at most DISC_BUSY_PCT (the max is logged: a period over 100 % means the estimate is off, not the drive),
# and no far seek in play. Each period's estimate is printed next to dsrsnd's own busy % (PCSX2's drive time); a
# ratio drifting past ~1.5 means the seek constants need refitting. PCSX2 times no seek: counts are the proxy until
# hardware
SEEK_MS, FAR_SEEK_MS, DRIVE_KBPS, DISC_BUSY_PCT = 40, 150, 1536, 70
MIN_FRAMES = 10
# the Burg plan's streamer gates per test script (step B0): the most allowed per run, from two ISO runs of main 6bb3218
# (2026-11-08, tools/test-all.sh --iso --burg --reps 2, the plan's baseline table; near_items re-measured 2026-11-09:
# distinct near items refused, the attempt count room_near swung 2x between identical runs): counts at the larger run + 30 %
# (identical runs differ by that much: near refusals 12 vs 92 on iso_ame_report3), the median + 2 cells, the horizon
# share + 3 points; None = not gated. Lower them as the plan's steps land, never raise them.
STREAM_GATE_KEYS = {"near_items": "near items refused for room (distinct)", "room_near": "near-class \"no room\" refusals", "popins": "near pop-ins",
                    "lent_near": "near parts lent a pool page and dropped for a lite set (MS_LEND)",
                    "due_med": "median cells drawn coarse while due near",
                    "hard_items": "hard-tier items refused for room (distinct, residency.h)", "hzn_pct": "% of seconds with horizon inside HZN_R",
                    "cut": "most wanted items one plan cut for room (mapstream.c, step B4)"}
# hard_items (2026-11-12, TECH_DEBT world-14): distinct items of residency.h's hard tier (mapstream.c's "no room ...,
# hard" lines: near parts within RES_NEAR_R, pinned coarse within RES_PIN_R) refused for room, which the export
# budgeted for; from two ISO runs of main 0f087d7 (taurus 38 / 27, report3 1 / 4) + 30 %. The arena's page holes are
# the cause (page-sized groups with 0.3-1.8 MB free in holes); lower these as that is fixed.
STREAM_GATES = {
    "iso_ame_report3": {"near_items": 16, "popins": 4, "due_med": 11, "hzn_pct": 57.0, "hard_items": 5},
    "iso_burg_soak2": {"near_items": 95, "popins": 50, "due_med": 12, "hzn_pct": 100.0},
    "iso_burg_taurus": {"near_items": 99, "popins": 36, "due_med": 8, "hzn_pct": 100.0, "hard_items": 49},
    "world_route": {"near_items": 162, "popins": 60, "due_med": 3, "hzn_pct": 93.9},
    "world_sprint": {"near_items": 151, "popins": 89, "due_med": 4, "hzn_pct": 95.9},
    "iso_burg_soak": None,  # (the 8-lap soak: not measured for the baseline)
}
MEDIA = {0: "-", 1: "cd", 2: "dvd", 3: "dvd9"}
# the "TEST refusals:" line (src/hitch.c refusals_print, once before each "TEST DONE"): the four refusal counters of the
# run (the streamer's "no room", gs_vram_admit, collision tiles, chunk files) as run gate keys
REFUSALS = re.compile(r"TEST refusals: room (\d+) near (\d+) items (\d+) hard (\d+) \| vram chr (\d+) obj (\d+) fx (\d+) "
                      r"\| coltile (\d+) \| chunk (\d+)")
REFUSAL_KEYS = ("ref_room", "ref_near", "ref_items", "ref_hard", "ref_vram_chr", "ref_vram_obj", "ref_vram_fx", "ref_coltile",
                "ref_chunk")
# the most allowed per run for each key (None: reported, not gated); the zero-refusals plan lowers these to 0 as each
# kind is fixed (reports/plans/2026-11-28-zero-refusals.md), never raises them
REFUSAL_ALLOW = {k: None for k in REFUSAL_KEYS}
REFUSAL_ALLOW["ref_chunk"] = 0  # a chunk refusal is a stale or mismatched export (tools/ci/chunkver.py), never a room problem
# VRAM refusals (gs_vram_admit, CHR / OBJ / FX past their gs_budget.h row): 0 on every Burg ISO route, 2 runs each, after
# FX 352 -> 368 KB and CHR 184 -> 168 KB (2026-11-28; FX peaks at 362 KB on golden taurus with the rows unenforced). The streamer's and the collision tiles' counts stay reported:
# 0-1490 and 3-29 a run on the same routes (the zero-refusals plan's report)
for _k in ("ref_vram_chr", "ref_vram_obj", "ref_vram_fx"):
    REFUSAL_ALLOW[_k] = 0

MAP_PASS = re.compile(r"map: pass ms (.*)")
LOGIC_PASS = re.compile(r"logic: pass ms (.*)")  # lpass.c (Burg plan B6): averages | worst frame | each part's max
DISC = re.compile(r"disc: t (\d+) s media (\S+) kbps (\d+) seeks (\d+) far (\d+) \(([\d.]+)/s\) layer (\d+)( \(load\))? \| "
                  r"ee reads (\d+) wait avg (\d+) max (\d+) ms \| stream wait max (\d+) ms over100 (\d+) \| cache (\d+) KB "
                  r"hits (\d+) miss (\d+) \| busy (\d+) % \| iop free (\d+) KB")


def pct(vals, p):
    v = sorted(vals)
    return v[min(len(v) - 1, int(p / 100 * len(v)))] if v else None


def log_summary(log):
    """the run's "disc:" and "map: pass ms" lines -> dict"""
    disc, mp, lp, under, gs_warn, refusals = [], {}, {}, 0, [], None
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
        m = LOGIC_PASS.search(line)
        if m:  # "player A ... total T | worst player ... total T | max player ... other O (N frames)"
            parts = m[1].split("|")
            for part, key in zip(parts, ("avg", "worst", "max")):
                t = part.replace("worst", "", 1).replace("max", "", 1).split("(")[0].split()
                for k, v in zip(t[::2], t[1::2]):
                    try:
                        lp.setdefault(k, {"avg": [], "worst": [], "max": []})[key].append(float(v))
                    except ValueError:
                        pass
        if "UNDERRUN" in line and "dsrsnd" in line:
            under += 1
        m = REFUSALS.search(line)
        if m:  # the last line counts (a parked run prints one per script; the counters run over the whole ELF run)
            refusals = dict(zip(REFUSAL_KEYS, map(int, m.groups())))
        if line.startswith("TEST warn: gs vram"):  # gs.c / loading.c: an owner over gs_budget.h, or LOAD left borrowed
            gs_warn.append(line[len("TEST warn: gs vram "):].strip())
    for i, d in enumerate(disc):  # estimated drive time of each period (the gate above)
        span = d["t"] - disc[i - 1]["t"] if i else (disc[1]["t"] - d["t"] if len(disc) > 1 else 10)
        span = span if span > 0 else 10
        d["est_pct"] = round(100 * ((d["seeks"] - d["far"]) * SEEK_MS + d["far"] * FAR_SEEK_MS +
                                    1000 * d["kbps"] * span / DRIVE_KBPS) / (1000 * span), 1)
    play = [d for d in disc if not d["load"]]
    out = {"underruns": under, "gs_vram_warn": gs_warn, "refusals": refusals, "map_pass": {k: {"p90_avg": pct(v["avg"], 90), "max": max(v["max"]) if v["max"] else None}
                                            for k, v in mp.items()},
           "logic_pass": {k: {"p90_avg": pct(v["avg"], 90), "max": max(v["max"]) if v["max"] else None}
                          for k, v in lp.items()}}
    if disc:
        n = max(1, len(play))
        out["disc"] = {
            "media": disc[-1]["media"], "periods": len(disc), "play_periods": len(play),
            "kbps": round(sum(d["kbps"] for d in play) / n), "seeks_s": round(sum(d["seeks_s"] for d in play) / n, 2),
            "far_seeks": sum(d["far"] for d in disc), "layer_play": sum(d["layer"] for d in play),
            "far_play": sum(d["far"] for d in play), "est_max": max((d["est_pct"] for d in play), default=0),
            "est_p90": pct([d["est_pct"] for d in play], 90) or 0,
            "est_ratio": round(sum(d["est_pct"] for d in play) / max(1, sum(d["busy"] for d in play)), 2),
            "est_avg": round(sum(d["est_pct"] for d in play) / n, 1),
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
                  "media", "miss_run", "frame_max", "busy_p50", "busy_p90", "busy_p99", "gov1", "gov2", "gov3"):
            if k in p:
                row[k] = p[k]
        if row.get("secs"):
            row["kbps"] = round(row.get("disc_kb", 0) / row["secs"]) if "disc_kb" in row else None
            row["seeks_s"] = round(row.get("seeks", 0) / row["secs"], 2) if "seeks" in row else None
        if n < MIN_FRAMES:
            row["note"] = f"{n} frames: not gated"
            out["shots"].append(row)
            continue
        f, gates = row["fails"], row.setdefault("gates", {})

        def gate(key, value, bad, msg):  # every frame gate's measured value (the ratchet's "can lower"), its fail
            if value is None:
                return
            gates[key] = value
            if bad:
                f.append(msg)
                row.setdefault("fail_gates", {})[key] = value
        for k, b in PASS_MS.items():
            v = p.get(k + "_p90")
            gate(k + "_p90", v, v is not None and v > b, v is not None and f"{k} p90 {v:.2f} > {b} ms")
        gate("p99", row["frame_p99"] or 0, (row["frame_p99"] or 0) > FRAME_P99_MS,
             f"frame p99 {row['frame_p99'] or 0:.1f} > {FRAME_P99_MS} ms")
        # stutter (testscript.c pb_frame; absent in logs of ELFs before it: not gated): missed vsyncs in a row, and the
        # worst frame of the segment (settles excluded like over34)
        mr = int(row["miss_run"]) if row.get("miss_run") is not None else None
        gate("miss_run", mr, mr is not None and mr > FB["miss_run"],
             f"miss_run {mr} missed vsyncs in a row > {FB['miss_run']}")
        fm = row.get("frame_max")
        gate("frame_max", fm, fm is not None and fm > FB["frame_max_ms"],
             fm is not None and f"frame_max {fm:.1f} > {FB['frame_max_ms']:g} ms")
        if row.get("gov1") is not None:  # the frame governor (governor.c): % of frames at level >= 1, gated by the ratchet only
            row["gov_pct"] = gates["gov_pct"] = int(row["gov1"]) + int(row.get("gov2", 0)) + int(row.get("gov3", 0))
        o_pct = round(100 * row["over34"] / n, 2)
        gate("over34_pct", o_pct, row["over34"] * 100 > OVER34_PCT * n,
             f"{row['over34']} of {n} frames over {FB['over_ms']:g} ms ({100 * row['over34'] / n:.1f} % > {OVER34_PCT} %)")
        ram = row["ram_min"]  # (memory gates: the KB short of the floor, so a larger value is worse like the others)
        if ram is not None and 0 < ram < 0x7FFFFFFF and ram / 1024 < RAM_MIN_KB:
            gate("ram_short_kb", round(RAM_MIN_KB - ram / 1024), True, f"RAM free {ram / 1024:.0f} KB < {RAM_MIN_KB} KB")
        if row["vram"] is not None and 0 <= row["vram"] / 1024 < VRAM_MIN_KB:
            gate("vram_short_kb", round(VRAM_MIN_KB - row["vram"] / 1024), True,
                 f"VRAM free {row['vram'] / 1024:.0f} KB < {VRAM_MIN_KB} KB")
        if disc_gates and row.get("iop_free") and row["iop_free"] < IOP_MIN_KB:
            gate("iop_short_kb", round(IOP_MIN_KB - row["iop_free"]), True,
                 f"IOP free {row['iop_free']:.0f} KB < {IOP_MIN_KB} KB")
        out["shots"].append(row)
        out["fails"] += [f"{s['name']}: {x}" for x in f]
    run = log_summary(os.path.join(d, "emulog.txt")) or {}
    out["run"] = run
    out["script"] = script_key(res, d)
    run_gates = out["run_gates"] = {}

    def rfail(key, value, msg):  # a run gate's fail (the ratchet's shot key "run")
        out["fails"].append(msg)
        run_gates[key] = value
    if disc_gates and run.get("disc"):
        dd = run["disc"]
        if dd["layer_play"]:
            rfail("layer_play", dd["layer_play"], f"run: {dd['layer_play']} disc layer changes during play (budget: none)")
        if dd.get("est_p90", 0) > DISC_BUSY_PCT:
            rfail("drive_p90", dd["est_p90"],
                  f"run: drive time p90 {dd['est_p90']} % of the periods in play > {DISC_BUSY_PCT} % (seeks "
                  f"x {SEEK_MS} ms, far x {FAR_SEEK_MS} ms, bytes at {DRIVE_KBPS} KB/s; max {dd['est_max']} %)")
        if dd.get("far_play"):
            rfail("far_play", dd["far_play"], f"run: {dd['far_play']} far seeks in play (budget: none)")
        if dd["seeks_s"] > SEEKS_PER_S:
            out.setdefault("notes", []).append(f"run: {dd['seeks_s']} seeks/s while playing (was gated at {SEEKS_PER_S})")
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
            if k != "hard_items" and b is not None and (st.get(k) or 0) > b:
                rfail(k, st[k], f"run: {STREAM_GATE_KEYS[k]} {st[k]} > {b} ({key})")
    if run.get("underruns"):
        rfail("underruns", run["underruns"], f"run: {run['underruns']} stream underruns")
    for k, v, msg in residency_gates(d, run, st, out):
        rfail(k, v, msg)
    for k, v, msg in refusal_gates(run):
        rfail(k, v, msg)
    return out


def refusal_gates(run):
    """the "TEST refusals:" line's counts against REFUSAL_ALLOW: (ratchet gate key, value, message) per key over its
    allowance; a log without the line (an ELF before it) gates nothing"""
    ref = run.get("refusals")
    if not ref:
        return []
    return [(k, ref[k], f"run: {k} {ref[k]} refusals > {REFUSAL_ALLOW[k]} (TEST refusals: line)")
            for k in REFUSAL_KEYS if REFUSAL_ALLOW.get(k) is not None and ref.get(k, 0) > REFUSAL_ALLOW[k]]


ARENA_UNDER = re.compile(r"mapstream: arena (\d+) KB is under the build's (\d+) KB")
ARENA_POOLS = re.compile(r"mapstream: arena structural (\d+) KB, misc (\d+) KB, visual (\d+) KB")
NO_ROOM = re.compile(r"mapstream: no room for item (\d+\.\d+) \(\d+ KB, class \d+, \d+ m(, hard)?\)")


def residency_gates(d, run, st, out):
    """The residency design's runtime checks (ps2/proto/src/residency.h, TECH_DEBT world-14): the arena came out at
    RES_ARENA_KB, its visual room x RES_FILL_PCT holds the largest per-block budget the export was solved against, and
    the hard tier's refusals (STREAM_GATES hard_items). Returns the fails as (ratchet gate key, value, message)."""
    import residency
    try:
        log = open(os.path.join(d, "emulog.txt"), errors="replace").read()
    except OSError:
        return []
    fails = []
    m = ARENA_UNDER.search(log)
    if m:
        fails.append(("arena_short_kb", int(m[2]) - int(m[1]),
                      f"run: arena {m[1]} KB under the build's {m[2]} KB (residency.h RES_ARENA_KB: the heap at boot)"))
    m = ARENA_POOLS.search(log)
    if m:
        room = int(m[3]) * residency.RES["RES_FILL_PCT"] // 100
        need = max(r for r, _ in residency.BLOCKS.values())
        run["visual_room_kb"] = room
        if room < need:
            fails.append(("visual_short_kb", need - room,
                          f"run: visual room {room} KB < the {need} KB the export's blocks were budgeted (residency.h)"))
    hard = {i for i, h in NO_ROOM.findall(log) if h}
    run["hard_items"] = len(hard)
    if st is not None and st:
        st["hard_items"] = len(hard)
        b = (STREAM_GATES.get(out.get("script")) or {}).get("hard_items")
        if b is not None and len(hard) > b:
            fails.append(("hard_items", len(hard), f"run: {STREAM_GATE_KEYS['hard_items']} {len(hard)} > {b} ({out.get('script')})"))
    return fails


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
        stutter = f" run {int(s['miss_run'])} max {s['frame_max']:5.1f} busy {s.get('busy_p50', 0):5.2f}/" \
                  f"{s.get('busy_p90', 0):5.2f}/{s.get('busy_p99', 0):5.2f}" if s.get("miss_run") is not None else ""
        stutter += f" gov {s['gov_pct']:3d} %" if s.get("gov_pct") is not None else ""
        o.append(f"  {s['shot']:24s} n {s['n']:4d} map {s['map_p90']:5.2f} chr {s['chr_p90']:5.2f} logic "
                 f"{s['logic_p90']:5.2f} world {s['world_p90']:5.2f} fx {s['fxhud_p90']:4.2f} vuw {s['vuw_p90']:4.2f} "
                 f"p99 {s['frame_p99']:5.1f} >34 {s['over34']:3d}{stutter}{disc}{'  ' + s['note'] if s.get('note') else ''}")
    run = r.get("run", {})
    if run.get("disc"):
        d = run["disc"]
        o.append(f"  disc ({d['media']}, {d['play_periods']} periods of play): {d['kbps']} KB/s, {d['seeks_s']} seeks/s, "
                 f"{d['far_seeks']} far ({d.get('far_play', 0)} in play), est. drive time {d.get('est_avg')} % avg / "
                 f"{d.get('est_p90')} % p90 / {d.get('est_max')} % max (x{d.get('est_ratio')} dsrsnd busy), layer changes {d['layer_play']} play / {d['layer_load']} load, EE wait avg "
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
                 f"HZN_R {st['hzn_s']} s ({st['hzn_pct']} %), plan cut at most {st.get('cut', 0)} items in a plan (near {st.get('cut_near', 0)}), "
                 f"hard tier refused {st.get('hard_items', 0)} items, visual room {run.get('visual_room_kb', '-')} KB")
    if run.get("logic_pass"):
        o.append("  logic pass p90 (120-frame avgs)/max frame: " + " ".join(
            f"{k} {v['p90_avg']:.2f}" + (f"/{v['max']:.1f}" if v["max"] is not None else "")
            for k, v in run["logic_pass"].items() if v["p90_avg"] is not None))
    if run.get("refusals"):
        o.append("  refusals: " + " ".join(f"{k[4:]} {v}" for k, v in run["refusals"].items()))
    if run.get("gs_vram_warn"):  # Polaris 1.1: VRAM owner budgets report only (gs_budget.h); 2.7 enforces
        o.append(f"  gs vram over budget ({len(run['gs_vram_warn'])} warn lines): " + "; ".join(run["gs_vram_warn"][:4]))
    o += [f"  FAIL {x}" for x in r["fails"]]
    return "\n".join(o)


def load_ratchet(path):
    """tools/ci/budget_ratchet.json: {script: {shot | "*" | "run": {gate: allowed value}}}; "_" keys are comments"""
    t = json.load(open(path))
    return {k: {s: g for s, g in v.items() if not s.startswith("_")} for k, v in t.items()
            if not k.startswith("_") and isinstance(v, dict)}


RATCHET_ONLY = ("gov_pct",)  # gates with no budget of their own (the governor's share of frames at level >= 1)


def ratchet_entry(script, shot, gate, table):
    """the allowed value of (script, shot, gate), or None; the shot key "*" matches every shot (not "run")"""
    shots = table.get(script) or {}
    for k in (shot, "*") if shot != "run" else (shot,):
        v = (shots.get(k) or {}).get(gate)
        if v is not None:
            return v
    return None


def ratchet_ok(script, shot, gate, value, table):
    """a failing gate is covered when the ratchet allows at least the measured value"""
    v = ratchet_entry(script, shot, gate, table)
    return v is not None and value is not None and value <= v


def ratchet(rs, table):
    """-> (uncovered fail lines, "can lower" notes) of checked results against the ratchet table"""
    bad, notes, worst, failed = [], [], {}, {}
    for r in rs:
        sc = r.get("script") or r["name"]
        items = [(s["shot"], s.get("fail_gates", {}), s.get("gates", {})) for s in r["shots"]]
        items.append(("run", r.get("run_gates", {}), r.get("run_gates", {})))
        for shot, fails, vals in items:
            for gate in RATCHET_ONLY:  # (no frame budget of their own: only a ratchet entry gates them)
                allowed = ratchet_entry(sc, shot, gate, table)
                if allowed is not None and vals.get(gate) is not None and vals[gate] > allowed:
                    bad.append(f"{sc}/{shot}/{gate} {vals[gate]} > ratchet {allowed} ({r['dir']})")
            for gate, value in fails.items():
                if not ratchet_ok(sc, shot, gate, value, table):
                    allowed = ratchet_entry(sc, shot, gate, table)
                    bad.append(f"{sc}/{shot}/{gate} {value}" + (f" > ratchet {allowed}" if allowed is not None
                                                                 else " (no ratchet entry)") + f" ({r['dir']})")
            for gate in ((table.get(sc) or {}).get(shot) or {}):
                v = vals.get(gate, 0 if shot == "run" else None)
                if v is not None:
                    k = (sc, shot, gate)
                    worst[k] = max(worst.get(k, v), v)
                    failed[k] = failed.get(k, False) or gate in fails
    for (sc, shot, gate), v in sorted(worst.items()):  # (the worst of the reps: an entry is the worse of them)
        allowed = table[sc][shot][gate]
        if not failed[sc, shot, gate]:
            notes.append(f"ratchet: {sc}/{shot}/{gate} passes ({v}): remove the entry")
        elif v < allowed:
            notes.append(f"ratchet: can lower {sc}/{shot}/{gate} to {v}")
    return bad, notes


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--json")
    ap.add_argument("--no-disc", action="store_true", help="skip the disc gates (layer changes, seeks, IOP RAM)")
    ap.add_argument("--ratchet", metavar="FILE", help="allowed misses (tools/ci/budget_ratchet.json): the exit code "
                    "counts only the failing gates no entry covers")
    a = ap.parse_args()
    rs = [check_result(p, not a.no_disc) for p in results(a.paths)]
    for r in rs:
        print(table(r))
    if a.json:
        json.dump(rs, open(a.json, "w"), indent=1)
    nf = sum(1 for r in rs if r["fails"])
    print(f"budget: {len(rs)} runs, {nf} over budget")
    if a.ratchet:
        bad, notes = ratchet(rs, load_ratchet(a.ratchet))
        for x in sorted(set(notes)):
            print(x)
        for x in bad:
            print(f"ratchet: FAIL {x}")
        print(f"ratchet: {len(bad)} failing gates not covered by {os.path.basename(a.ratchet)}")
        sys.exit(min(len(bad), 125))
    sys.exit(min(nf, 125))


if __name__ == "__main__":
    main()
