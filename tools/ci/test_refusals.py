#!/usr/bin/env python3
"""tools/budget.py's "TEST refusals:" line (src/hitch.c refusals_print) without game data (tools/ci/offline.sh).

  1 log_summary parses the line into the nine ref_* keys (the last line of a log counts)
  2 refusal_gates: a key with no allowance (None) never fails, a count over its allowance fails, a log without the
    line gates nothing
Exit code = number of failed checks."""
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import budget  # noqa: E402

FAILS = []
LINE = "TEST refusals: room {} near 4 items 3 hard 1 | vram chr 0 obj 2 fx 1 | coltile 5 | chunk 0\n"


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        FAILS.append(what)


def summary(text):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(text)
    try:
        return budget.log_summary(f.name)
    finally:
        os.unlink(f.name)


s = summary("boot\n" + LINE.format(7) + "TEST DONE frames 10\n" + LINE.format(12) + "TEST DONE frames 20\n")
r = s["refusals"]
check(r is not None and set(r) == set(budget.REFUSAL_KEYS), "all nine ref_* keys parsed")
check(r["ref_room"] == 12 and r["ref_near"] == 4 and r["ref_items"] == 3 and r["ref_hard"] == 1, "streamer fields")
check(r["ref_vram_chr"] == 0 and r["ref_vram_obj"] == 2 and r["ref_vram_fx"] == 1, "vram fields")
check(r["ref_coltile"] == 5 and r["ref_chunk"] == 0, "coltile / chunk fields")
check(summary("no line here\n")["refusals"] is None, "a log without the line has no refusals")

saved = dict(budget.REFUSAL_ALLOW)
try:
    for k in budget.REFUSAL_KEYS:
        budget.REFUSAL_ALLOW[k] = None
    check(budget.refusal_gates({"refusals": r}) == [], "no allowance: nothing fails")
    budget.REFUSAL_ALLOW["ref_coltile"] = 5
    budget.REFUSAL_ALLOW["ref_vram_fx"] = 0
    budget.REFUSAL_ALLOW["ref_chunk"] = 0
    f = budget.refusal_gates({"refusals": r})
    check([x[0] for x in f] == ["ref_vram_fx"], "only the count over its allowance fails (fx 1 > 0; coltile 5 <= 5; chunk 0)")
    check(budget.refusal_gates({}) == [], "no line: nothing gated")
finally:
    budget.REFUSAL_ALLOW.clear()
    budget.REFUSAL_ALLOW.update(saved)

print(f"test_refusals: {len(FAILS)} failed")
sys.exit(len(FAILS))
