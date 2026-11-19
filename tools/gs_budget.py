#!/usr/bin/env python3
"""The GS memory budget per owner from ps2/proto/src/gs_budget.h (Polaris 1.1): one table for the runtime (gs.c) and
the checks (tools/budget.py, tools/ci/gsown.py). Import it (GSB, total_kb) or run it to print the table."""
import os
import re

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ps2", "proto", "src", "gs_budget.h")
GSB = {m[1].lower(): int(m[2]) for m in re.finditer(r"^#define GSB_(\w+)_KB (\d+)\b", open(HEADER).read(), re.M)}
BORROWED = ("load",)  # borrowed from another row, not part of the 4096 KB


def total_kb():
    return sum(v for k, v in GSB.items() if k not in BORROWED)


if __name__ == "__main__":
    for k, v in GSB.items():
        print(f"{k:12s} {v:5d} KB{'  (borrowed)' if k in BORROWED else ''}")
    print(f"{'sum':12s} {total_kb():5d} KB")
