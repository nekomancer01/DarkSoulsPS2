#!/usr/bin/env python3
"""The residency design's budget checks without game data (tools/ci/offline.sh; TECH_DEBT world-14).

  1 ps2/proto/src/residency.h parses (tools/residency.py), every RES_BLOCK row has near <= room, and the rows fit the
    visual room of RES_ARENA_KB with pools of the sizes main ships (STRUCT_KB / MISC_KB below, the 2026-11-12 build)
  2 tools/world_budget.py --strict on a synthetic gated block passes while every spot fits the room and fails when one
    spot's hard tier (near level within RES_NEAR_R, coarse + kept within RES_PIN_R) goes over it, or when a block's
    budget promises more than the room
Exit code = number of failed checks."""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import residency  # noqa: E402

STRUCT_KB, MISC_KB = 3461, 905  # world_budget.py's pools for the shipped world (main 0f087d7 + residency.h)
FAILS = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        FAILS.append(what)


room = residency.room_kb(STRUCT_KB, MISC_KB)
for name, (r, n) in residency.BLOCKS.items():
    check(0 < n <= r, f"residency.h {name}: near {n} KB within its room {r} KB")
    check(r <= room, f"residency.h {name}: room {r} KB fits the visual room {room} KB of the {residency.RES['RES_ARENA_KB']} KB arena")
check(residency.RES["RES_NEAR_R"] < residency.RES["RES_PIN_R"] < residency.RES["RES_HZN_R"],
      "radii: near < pin < horizon")


def world(tmp, near_kb, coarse_kb):
    """one gated block (m10_01_00_00_t) with two cells around a spot at the origin: a near cell at 5 m with near_kb of
    near level, a coarse cell at 30 m with coarse_kb of coarse level"""
    d = os.path.join(tmp, "m10_01_00_00_t")
    os.makedirs(d, exist_ok=True)
    # cell rows: key (3), box lo (3), box hi (3), bytes of near, coarse, kept, backdrop
    cells = [[0, 0, 0, 2, -1, -1, 8, 1, 1, near_kb * 1024, 0, 0, 0],
             [1, 0, 0, 30, -1, -1, 32, 1, 1, 0, coarse_kb * 1024, 0, 0]]
    json.dump(dict(cells=cells, sky=0, spots=[[0, 0, 0]]), open(os.path.join(d, "map.bin.cells.json"), "w"))
    return d


def run(tmp, room_kb, near_kb, coarse_kb):
    d = world(tmp, near_kb, coarse_kb)
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "world_budget.py"),
                        os.path.join(tmp, "wb.json"), d, "--room", str(room_kb), "--strict"],
                       capture_output=True, text=True, env=dict(os.environ, MAP="default"))
    return p.returncode, p.stdout + p.stderr


with tempfile.TemporaryDirectory() as tmp:
    big = max(r for r, _ in residency.BLOCKS.values())
    rc, out = run(tmp, room, 1000, 1000)
    check(rc == 0, f"world_budget --strict passes a spot at 2000 KB in a {room} KB room" + ("" if rc == 0 else "\n" + out))
    rc, out = run(tmp, room, room, 1000)
    check(rc == 1, f"world_budget --strict fails a spot at {room + 1000} KB in a {room} KB room" + ("" if rc == 1 else "\n" + out))
    rc, out = run(tmp, big - 1, 100, 100)
    check(rc == 1 and "lower RES_BLOCK" in out,
          f"world_budget --strict fails when the room ({big - 1} KB) is under a block's budget ({big} KB)"
          + ("" if rc == 1 else "\n" + out))
print(f"test_residency: {len(FAILS)} failed")
sys.exit(len(FAILS))
