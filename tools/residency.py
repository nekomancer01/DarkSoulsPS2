#!/usr/bin/env python3
"""The residency design's numbers from ps2/proto/src/residency.h (TECH_DEBT world-14): the one table the runtime, the
map exporter and the budget checks share. Import it (RES, block_budget, room_kb) or run it:

  residency.py                     print the table
  residency.py room <struct KB> <misc KB>   the visual room for pools of that size (as mapstream.c splits the arena)"""
import os
import re
import sys

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ps2", "proto", "src", "residency.h")


def _read(path=HEADER):
    src = open(path).read()
    res = {m[1]: int(m[2]) for m in re.finditer(r"^#define (RES_\w+) (\d+)\b", src, re.M)}
    blocks = {m[1]: (int(m[2]), int(m[3])) for m in re.finditer(r"^RES_BLOCK\((\w+), (\d+), (\d+)\)", src, re.M)}
    for k in ("RES_NEAR_R", "RES_PIN_R", "RES_HZN_R", "RES_ARENA_KB", "RES_PAGE_KB", "RES_FILL_PCT", "RES_ALLOW_PCT",
              "RES_ALLOW_BLOCK_PCT"):
        if k not in res:
            raise SystemExit(f"residency.py: {k} missing in {path}")
    if "default" not in blocks:
        raise SystemExit(f"residency.py: no RES_BLOCK(default, ...) row in {path}")
    return res, blocks


RES, BLOCKS = _read()


def block_budget(name):
    """(room KB, near KB) of a block (its RES_BLOCK row, else the default row) and whether it has a row of its own"""
    if name in BLOCKS:
        return BLOCKS[name] + (True,)
    return BLOCKS["default"] + (False,)


def pages_kb(kb):
    """KB rounded up to whole arena pages"""
    p = RES["RES_PAGE_KB"]
    return (int(kb) + p - 1) // p * p


def room_kb(struct_kb, misc_kb, arena_kb=None):
    """the visual room (KB) the plan fills: the arena less the two pools, each in whole pages, x RES_FILL_PCT %"""
    a = RES["RES_ARENA_KB"] if arena_kb is None else arena_kb
    return (a - pages_kb(struct_kb) - pages_kb(misc_kb)) * RES["RES_FILL_PCT"] // 100


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "room":
        print(room_kb(float(sys.argv[2]), float(sys.argv[3])))
    else:
        for k, v in RES.items():
            print(f"{k} {v}")
        for k, (r, n) in BLOCKS.items():
            print(f"block {k}: room {r} KB, near {n} KB")
