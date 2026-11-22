#!/usr/bin/env python3
"""Automatic per-block budget fit (Polaris 4.8 step 2): the RES_BLOCK rows of ps2/proto/src/residency.h from what the
exported blocks ask of the arena, instead of hand-set numbers.

  budgetfit.py <out.json> <boot gen dir> [<gen dir> ...] [--write] [--report out.md] [--prev <last out.json>]

Runs tools/world_budget.py in-process over the world's exported blocks (as tools/blockreport.py does: the structural
pool from the boot block's world.bin) and, for every block, takes the hard tier at its own navmesh spots (the spots of
its MSB inside its cells, each once; the whole MSB for a block without cells) against the visual room:

  f = the RES_ALLOW_PCT x TARGET_SHARE % quantile of room / hard over those spots (the worst 0.5 % may stay over: the world
      gate allows 1 % of all spots, and seam spots count for several blocks)
  fitted room = row room x f, rounded down to 10 KB, at most the visual room less RES_PAGE_KB (a lite set growing a page),
  at least FLOOR_KB (a raise takes DAMP of the step, see fit()); fitted near = row near x (fitted room / row room) (the row's near share kept)

A block over the room (f < 1) gets a smaller row; one with any spot over is never raised: its export's spot solve (export_map.py solve_budget) then simplifies
more where it is dense; a block with headroom (f > 1) gets a larger one, up to the room. The hard tier at a spot is the
sum over every block loaded there, so a seam spot's overshoot lowers every block at it by the same factor (their shares
of the spot follow export_map.py WS_SHARE_TRIS). The fit is a step toward a fixed point: rows change the exports, which
change the asks; run it again after the re-export until no row moves by more than TOL_PCT (the report says when).

A row that cannot move the ask is held and listed: with --prev (the last fit's json), a row the last fit cut whose
block's hard tier p99 at its own spots did not drop by 0.5 % keeps its value. Also listed: when the block's export reports spots at its error caps
(map.bin.budget.json: a level whose solve did not meet its budget within WS_PX0 / WS_PX1 in tools/recipe.toml [mapenv], hand-set per map), a smaller row
does not shrink it. Those rows and the [mapenv] knobs they depend on are what is still hand-set.

--write rewrites the RES_BLOCK rows of residency.h in place (only the numbers; comments and order kept; a block without
a row gets one before the default row). Without it the file is unchanged and the fit only reported.
Exit 0; the gate itself is world_budget.py --strict (every block since step 2)."""
import contextlib
import io
import json
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import residency  # noqa: E402

TOL_PCT = 2.0     # a fit is settled when no row moves by more than this
FLOOR_KB = 3000   # no row below this (ASSUMED: the coarse level and kept meshes of a dense block need about 2.5 MB)
STEP_KB = 10
DAMP = 0.5      # share of the headroom a raise takes per fit
TARGET_SHARE = 0.5  # of RES_ALLOW_PCT: the blocks' spots over add up across seams in the world gate
SEAM_SHARE = 0.25  # a block counts another block's spot when it holds this share of its hard tier


def spots_of(name, L, k, wb):
    """indices of block k's own spots: in its cells (area.BLOCKS), each position once"""
    own = np.flatnonzero(L["own"] == k)
    rects, _ = wb.block_rects(name)
    if rects:
        own = own[wb.rect_dist(rects, L["P"][own]) <= 0.0]
    _, first = np.unique(np.round(L["P"][own], 1), axis=0, return_index=True)
    return own[np.sort(first)]


def capped(gen):
    """levels whose spot solve could not meet the budget within the error caps and left own spots over
    (map.bin.budget.json "met" false), or None when the file is missing"""
    p = os.path.join(gen, "map.bin.budget.json")
    try:
        J = json.load(open(p))
    except (OSError, ValueError):
        return None
    rows = J if isinstance(J, list) else [J]
    return sum(1 for r in rows if isinstance(r, dict) and r.get("met") is False and r.get("own_over_pct", 0) > 0)


def fit(dirs, boot_map, prev_fit=None):
    import world_budget as wb
    real = wb.structural
    wb.structural = lambda _d: real(dirs[0])
    with contextlib.redirect_stdout(io.StringIO()):
        tmp = os.path.join(os.path.dirname(os.path.abspath(dirs[0])), ".budgetfit.world_budget.json")
        wb.main([tmp] + dirs)
    wb.structural = real
    try:
        os.remove(tmp)
    except OSError:
        pass
    L = wb.LAST
    room = float(L["room"])
    page = residency.RES["RES_PAGE_KB"]
    q = residency.RES["RES_ALLOW_PCT"] / 100.0 * TARGET_SHARE
    out = []
    for k, name in enumerate(L["names"]):
        row_name = boot_map if name == "gen" or os.path.normpath(dirs[0]).endswith(name) else name
        r_room, r_near, own_row = residency.block_budget(row_name)
        idx = spots_of(name, L, k, wb)
        own_p99 = float(np.percentile(L["tot"]["hard"][idx], 99)) if len(idx) else 0.0
        if "hard_by" in L:  # and the other blocks' spots this block holds a SEAM_SHARE of (a seam spot's overshoot
            # is cut from every block there, or a raised neighbour would undo the cut)
            tot = L["tot"]["hard"]
            sh = np.flatnonzero(L["hard_by"][k] > SEAM_SHARE * np.maximum(tot, 1.0))
            _, first = np.unique(np.round(L["P"][sh], 1), axis=0, return_index=True)
            idx = np.union1d(idx, sh[first])
        hard = L["tot"]["hard"][idx]
        if not len(hard):
            out.append(dict(block=row_name, gen=name, spots=0, row=[r_room, r_near], fit=None, note="no spots"))
            continue
        f = float(np.quantile(room / np.maximum(hard, 1.0), q))
        # a raise moves half way (DAMP): every block at a seam spot is raised from the same headroom, so the full
        # step would overshoot there on the next export; a cut takes the whole step
        over = float((hard > room).mean() * 100)
        g = f if f < 1 else 1.0 if over > 0 else 1 + (f - 1) * DAMP  # no raise while any of its spots is over
        new = min(room - page, max(FLOOR_KB, r_room * g))
        new = int(new // STEP_KB * STEP_KB)
        near = int(round(r_near * new / r_room))
        d = next(x for x in dirs if os.path.basename(os.path.normpath(x)) == name)
        cap = capped(d)
        note = ""
        prev = (prev_fit or {}).get(row_name)
        if f < 1 and prev and prev.get("note", "").startswith("held"):
            new, near, note = r_room, r_near, prev["note"]
        elif f < 1 and prev and prev["row"][0] > r_room and prev.get("own_p99", prev["hard_p99"]) - own_p99 < 0.005 * own_p99:
            # the last fit cut this row and the ask did not follow: the export is at its error caps; hold the row
            new, near = r_room, r_near
            note = "held: the last cut did not shrink the ask (the export is at its error caps, [mapenv] WS_PX0 / WS_PX1)"
        elif f < 1 and cap:
            note = f"{cap} level(s) at the export's error caps: a smaller row may not shrink them ([mapenv] WS_PX0 / WS_PX1)"
        out.append(dict(block=row_name, gen=name, spots=int(len(idx)), over_pct=round(over, 2),
                        hard_p50=round(float(np.percentile(hard, 50))), hard_p99=round(float(np.percentile(hard, 99))),
                        hard_max=round(float(hard.max())), own_p99=round(own_p99), f=round(f, 3), row=[r_room, r_near], own_row=own_row,
                        fit=[new, near], move_pct=round(abs(new - r_room) / r_room * 100, 1), note=note))
    return dict(room_kb=room, allow_pct=q * 100, tol_pct=TOL_PCT, blocks=out,
                settled=all(b["fit"] is None or b["move_pct"] <= TOL_PCT for b in out))


def write_rows(R, path=residency.HEADER):
    src = open(path).read()
    for b in R["blocks"]:
        if not b["fit"]:
            continue
        new = f"RES_BLOCK({b['block']}, {b['fit'][0]}, {b['fit'][1]})"
        pat = re.compile(rf"^RES_BLOCK\({re.escape(b['block'])}, \d+, \d+\)", re.M)
        if pat.search(src):
            src = pat.sub(new, src)
        else:
            src = re.sub(r"^RES_BLOCK\(default,", new + "\nRES_BLOCK(default,", src, flags=re.M)
    with open(path + ".tmp", "w") as f:
        f.write(src)
    os.replace(path + ".tmp", path)


def markdown(R):
    L = [f"# Budget fit (visual room {R['room_kb']:.0f} KB, worst {R['allow_pct']:.0f} % of a block's spots may stay over)",
         "", "| block | spots | hard p50 / p99 / max KB | over % | f | row room / near | fit room / near | note |",
         "|---|---:|---|---:|---:|---|---|---|"]
    for b in R["blocks"]:
        if b["fit"] is None:
            L.append(f"| {b['block']} | 0 | | | | {b['row'][0]} / {b['row'][1]} | | {b['note']} |")
            continue
        L.append(f"| {b['block']} | {b['spots']} | {b['hard_p50']} / {b['hard_p99']} / {b['hard_max']} | "
                 f"{b['over_pct']} | {b['f']} | {b['row'][0]} / {b['row'][1]}{'' if b['own_row'] else ' (default)'} | "
                 f"{b['fit'][0]} / {b['fit'][1]} | {b['note']} |")
    L += ["", "Settled (no row moves by more than %.0f %%): %s" % (R["tol_pct"], "yes" if R["settled"] else "no")]
    return "\n".join(L) + "\n"


def main(argv):
    args = [a for a in argv if a != "--write"]
    rep = None
    if "--report" in args:
        i = args.index("--report")
        rep = args[i + 1]
        del args[i:i + 2]
    out, dirs = args[0], args[1:]
    prev = None
    if "--prev" in args:
        i = args.index("--prev")
        prev = {b["block"]: b for b in json.load(open(args[i + 1]))["blocks"] if b.get("fit")}
        del args[i:i + 2]
    R = fit(dirs, os.environ.get("MAP", "m10_02_00_00"), prev)
    with open(out, "w") as f:
        json.dump(R, f, indent=1)
    if rep:
        with open(rep, "w") as f:
            f.write(markdown(R))
    for b in R["blocks"]:
        if b["fit"]:
            print(f"budget fit: {b['block']}: hard p99 {b['hard_p99']} / max {b['hard_max']} KB at {b['spots']} spots "
                  f"({b['over_pct']} % over), row {b['row'][0]} / {b['row'][1]} -> {b['fit'][0]} / {b['fit'][1]}"
                  + (f"; {b['note']}" if b["note"] else ""))
    print(f"budget fit: room {R['room_kb']:.0f} KB, settled: {'yes' if R['settled'] else 'no'}")
    if "--write" in argv:
        write_rows(R)
        print(f"budget fit: rows written to {residency.HEADER}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
