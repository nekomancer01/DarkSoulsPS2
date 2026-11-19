#!/usr/bin/env python3
"""GS ownership check (Polaris 1.1, CI job "hygiene"): GS state is written only through ps2/proto/src/gs.h / gs.c.

Fails on any of these outside gs.h / gs.c:
  - GS_SETREG_<state register> (TEST ALPHA ZBUF FRAME_1 TEX1 CLAMP TEXA BITBLTBUF TRXPOS TRXREG TRXDIR MIPTBP1):
    use gs.h's named words (GS_TEST_CUTOUT, gs_zbuf, gs_frame, gs_blt_to, ...)
  - a raw A+D register address of those registers (0x42 0x47 0x4C 0x4E 0x14 0x08 0x3B 0x34 0x50-0x53) in a REG / q64 /
    packet store: use GSR_*
  - the display registers (GS_SET_*, SetGsCrt) and gsKit's state calls (gsKit_set_test / _primalpha / _clamp,
    gsKit_vram_alloc): gs_display_init, gs_test / gs_primalpha / gs_clamp, gs_vram_fixed
map.c is allowed its hand-written words until Polaris 4.1 splits it (streaming work is live there); gs.c's
_Static_asserts keep them equal to gs.h's names. The count of those is printed so it only goes down.
Also checks gs_budget.h: its rows sum to 4096 KB (tools/gs_budget.py)."""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SRC = os.path.join(ROOT, "ps2", "proto", "src")
OWN = {"gs.h", "gs.c"}
LEGACY = {"map.c": 20}  # map.c's count on main (14 when the check was added; #252's horizon / near fades and sky state made it 20, 2026-11-19): it may only go down
STATE = re.compile(r"\bGS_SETREG_(TEST|ALPHA|ZBUF|FRAME_1|TEX1|CLAMP|TEXA|BITBLTBUF|TRXPOS|TRXREG|TRXDIR|MIPTBP1)\b"
                   r"|\bGS_SET_[A-Z0-9]+\b|\bSetGsCrt\b|\bgsKit_set_(test|primalpha|clamp)\b|\bgsKit_vram_alloc\s*\(")
ADDR = re.compile(r"(?:\b(?:REG|q64)\(.*,\s*|\*p\+\+ = |p\[\d+\] = |\{.*,\s*|^\s*)"
                  r"0x(42|47|4[cC]|4[eE]|14|08|3[bB]|34|5[0-3])\s*[)};,]")


def scan(path):
    n, hits = 0, []
    for i, line in enumerate(open(path, errors="replace"), 1):
        code = re.sub(r"/\*.*?\*/|//.*", "", line)
        if STATE.search(code) or ADDR.search(code):
            n += 1
            hits.append(f"{os.path.basename(path)}:{i}: {line.strip()}")
    return n, hits


def main():
    fails = []
    for f in sorted(os.listdir(SRC)):
        if not f.endswith((".c", ".h")) or f in OWN:
            continue
        n, hits = scan(os.path.join(SRC, f))
        if f in LEGACY:
            print(f"gsown: {f}: {n} raw GS writes (allowed up to {LEGACY[f]} until Polaris 4.1)")
            if n > LEGACY[f]:
                fails += hits
        else:
            fails += hits
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import gs_budget
    total = gs_budget.total_kb()
    if total != 4096:
        fails.append(f"gs_budget.h: rows sum to {total} KB, not 4096")
    for x in fails:
        print("gsown: FAIL " + x)
    print(f"gsown: {'FAIL' if fails else 'ok'} ({len(fails)} problems)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
