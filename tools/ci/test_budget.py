#!/usr/bin/env python3
"""tools/budget.py's frame budget and its CI ratchet (--ratchet) without game data (tools/ci/offline.sh step 8).

  1 load_budget() reads ps2/proto/src/frame_budget.h: the FB_PASSES names are the gated passes, a header without
    FB_PASSES is refused
  2 the ratchet on fixture results: a failing gate covered by an entry at or above the measured value passes (and is
    named for lowering when it beat the entry), a worse value fails, a gate with no entry fails, "_" keys are skipped
  3 the stutter gates: miss_run over FB_MISS_RUN and frame_max over FB_FRAME_MAX_MS fail; logs without them parse;
    gov_pct (the frame governor's share) fails only over its ratchet entry
Exit code = number of failed checks."""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import budget  # noqa: E402

FAILS = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        FAILS.append(what)


def passes(**kw):
    p = {"n": 300, "frame_p99": 33.5, "over34": 0, "secs": 10.0}
    for k in budget.PASS_MS:
        p[k + "_p50"], p[k + "_p90"] = 0.25, 0.5
    p.update(kw)
    return p


def result(d, script, shots):
    """a test-run.py result.json in d: shots = {name: passes dict}"""
    os.makedirs(d, exist_ok=True)
    res = {"name": script, "script": f"ps2/proto/tests/{script}.txt",
           "shots": [{"name": n, "stats": {"passes": p, "ram_min": 4 << 20, "vram": 512 << 10}} for n, p in shots.items()]}
    json.dump(res, open(os.path.join(d, "result.json"), "w"))


def run(args):
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "budget.py")] + args, capture_output=True,
                       text=True)
    return r.returncode, r.stdout


def main():
    fb = budget.load_budget()
    check(set(fb["passes"]) == {"map", "chr", "fxhud", "logic", "world", "vuw"}, "FB_PASSES names = the gated passes")
    check(fb["over_ms"] == 41.7 and fb["miss_run"] == 1 and fb["frame_max_ms"] == 67.0, "FB_ frame numbers read")
    with tempfile.TemporaryDirectory() as t:
        h = os.path.join(t, "no_passes.h")
        open(h, "w").write("#define FB_FRAME_MS 33.37f\n")
        try:
            budget.load_budget(h)
            check(False, "a header without FB_PASSES is refused")
        except SystemExit as e:
            check("no FB_PASSES" in str(e), "a header without FB_PASSES is refused")

        rt = os.path.join(t, "ratchet.json")
        json.dump({"_doc": "comment", "_note": {"p00_001": {"fxhud_p90": 99}},
                   "autotest_x": {"p00_001": {"fxhud_p90": 2.75}, "p00_002": {"fxhud_p90": 2.75}, "_c": "comment"}},
                  open(rt, "w"))
        # covered: fxhud 2.5 under the entry 2.75 -> passes, named for lowering
        result(os.path.join(t, "covered"), "autotest_x", {"p00_001": passes(fxhud_p90=2.5)})
        rc, out = run([os.path.join(t, "covered"), "--ratchet", rt])
        check(rc == 0, f"covered fail passes (rc {rc})")
        check("can lower autotest_x/p00_001/fxhud_p90 to 2.5" in out, "covered fail is named for lowering")
        rc0, _ = run([os.path.join(t, "covered")])
        check(rc0 == 1, f"without --ratchet the same run fails (rc {rc0})")
        # worse: fxhud 2.9 over the entry 2.75 -> fails
        result(os.path.join(t, "worse"), "autotest_x", {"p00_002": passes(fxhud_p90=2.9)})
        rc, out = run([os.path.join(t, "worse"), "--ratchet", rt])
        check(rc == 1 and "autotest_x/p00_002/fxhud_p90 2.9 > ratchet 2.75" in out, f"worse fail fails (rc {rc})")
        # uncovered: a shot with no entry -> fails
        result(os.path.join(t, "uncov"), "autotest_x", {"p00_003": passes(fxhud_p90=1.6)})
        rc, out = run([os.path.join(t, "uncov"), "--ratchet", rt])
        check(rc == 1 and "(no ratchet entry)" in out, f"uncovered fail fails (rc {rc})")
        # "_" keys: never read as a script or a shot
        table = budget.load_ratchet(rt)
        check("_doc" not in table and "_note" not in table and "_c" not in table["autotest_x"], "_ keys skipped")
        check(budget.ratchet_ok("autotest_x", "p00_001", "fxhud_p90", 2.75, table)
              and not budget.ratchet_ok("_note", "p00_001", "fxhud_p90", 1.0, table), "ratchet_ok at the entry, not on _")
        check(budget.ratchet_ok("s", "p9", "miss_run", 2, {"s": {"*": {"miss_run": 3}}}), "\"*\" matches every shot")
        # gov_pct: no budget of its own, only its ratchet entry gates it
        rg = os.path.join(t, "ratchet_gov.json")
        json.dump({"autotest_g": {"*": {"gov_pct": 10}}}, open(rg, "w"))
        result(os.path.join(t, "gov_ok"), "autotest_g", {"p00_001": passes(gov1=5, gov2=2, gov3=0)})
        result(os.path.join(t, "gov_bad"), "autotest_g", {"p00_001": passes(gov1=8, gov2=3, gov3=1)})
        rc1, _ = run([os.path.join(t, "gov_ok"), "--ratchet", rg])
        rc2, out2 = run([os.path.join(t, "gov_bad"), "--ratchet", rg])
        rc3, _ = run([os.path.join(t, "gov_bad")])
        check(rc1 == 0 and rc2 == 1 and "gov_pct 12 > ratchet 10" in out2 and rc3 == 0,
              f"gov_pct gated by its ratchet entry only (rc {rc1} / {rc2} / {rc3})")
        # stutter gates, and old logs without the fields
        result(os.path.join(t, "stutter"), "autotest_y", {"p00_001": passes(miss_run=2, frame_max=40.0),
                                                           "p00_002": passes(miss_run=1, frame_max=80.0),
                                                           "p00_003": passes(miss_run=1, frame_max=50.0)})
        r = budget.check_result(os.path.join(t, "stutter", "result.json"))
        f = {s["shot"]: s["fails"] for s in r["shots"]}
        check(any("miss_run 2" in x for x in f["p00_001"]), "miss_run over FB_MISS_RUN fails")
        check(any("frame_max 80.0" in x for x in f["p00_002"]), "frame_max over FB_FRAME_MAX_MS fails")
        check(not f["p00_003"], "a lone 3-field frame (miss_run 1, 50 ms) passes")
        result(os.path.join(t, "old"), "autotest_z", {"p00_001": passes()})
        r = budget.check_result(os.path.join(t, "old", "result.json"))
        check(not r["fails"] and "miss_run" not in r["shots"][0]["gates"], "a log without the stutter fields passes")
    print(f"test_budget: {len(FAILS)} failed")
    return len(FAILS)


if __name__ == "__main__":
    sys.exit(main())
