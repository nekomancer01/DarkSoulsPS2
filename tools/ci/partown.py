#!/usr/bin/env python3
"""Part ownership check (Polaris 4.1, CI job "hygiene"): the world streaming parts keep their state to themselves.

  block lifecycle    world.c wblock.c wextra.c wplan.c       private header world_i.h, public world.h
  residency planner  mapstream.c msplan.c msread.c msevict.c private header mapstream_i.h, public mapstream.h
  page arena         msarena.c                               state static in msarena.c, its interface msarena.h

Fails when a private header is included outside its part, msarena.h outside the planner and the arena, the arena
includes the planner's header (it must not see map items), or a file outside a part declares one of its shared names
(w_* / ms_* / msa_*) extern. ps2/proto/lab/ is the Lab: it compiles the block lifecycle's files as they are."""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SRC = os.path.join(ROOT, "ps2", "proto", "src")
LAB = os.path.join(ROOT, "ps2", "proto", "lab")
WORLD = {"world.c", "wblock.c", "wextra.c", "wplan.c", "world_i.h"}
PLAN = {"mapstream.c", "msplan.c", "msread.c", "msevict.c", "mapstream_i.h"}
ARENA = {"msarena.c", "msarena.h"}
RULES = [  # header, files that may include it
    ("world_i.h", WORLD),
    ("mapstream_i.h", PLAN),
    ("msarena.h", PLAN | ARENA),
]
PREFIX = re.compile(r"\bextern\b[^;]*\b(w|ms|msa)_\w+")
INC = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def code(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def main():
    fails = []
    files = [(d, f) for d in (SRC, LAB) if os.path.isdir(d) for f in sorted(os.listdir(d)) if f.endswith((".c", ".h"))]
    for d, f in files:
        text = code(open(os.path.join(d, f), errors="replace").read())
        incs = INC.findall(text)
        for hdr, allowed in RULES:
            if hdr in incs and (d != SRC or f not in allowed):
                fails.append(f"{os.path.relpath(os.path.join(d, f), ROOT)} includes {hdr} (only {' '.join(sorted(allowed))})")
        if d == SRC and f in ARENA and "mapstream_i.h" in incs:
            fails.append(f"src/{f}: the page arena includes mapstream_i.h (it must not see map items)")
        if d != SRC or f not in WORLD | PLAN | ARENA:
            for m in PREFIX.finditer(text):
                fails.append(f"{os.path.relpath(os.path.join(d, f), ROOT)}: extern of a part's shared name: {m.group(0)[:80]}")
    for x in fails:
        print("partown: FAIL " + x)
    print(f"partown: {'FAIL' if fails else 'ok'} ({len(fails)} problems)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
