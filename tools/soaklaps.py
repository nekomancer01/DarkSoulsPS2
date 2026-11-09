#!/usr/bin/env python3
"""Cut the Burg soak (ps2/proto/tests/iso_burg_soak.txt, 8 laps) to its first N laps.

    tools/soaklaps.py <laps> [out.txt] [--src ps2/proto/tests/iso_burg_soak.txt]

Keeps everything before "log soak lap <N> start" and ends the script as the full soak does ("log soak done", the end
shot). Lap 1 has a death, so 2 laps cover the walk both ways, the spins and idles, a kill and a respawn. Out defaults to
iso_burg_soak<N>.txt beside the source (tools/budget.py STREAM_GATES keys on that name). tools/test-all.sh --burg runs
the 2-lap variant."""
import argparse, os

ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("laps", type=int)
ap.add_argument("out", nargs="?")
ap.add_argument("--src", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ps2", "proto", "tests",
                                              "iso_burg_soak.txt"))
a = ap.parse_args()
lines = open(a.src).read().splitlines()
cut = next((i for i, l in enumerate(lines) if l.strip() == f"log soak lap {a.laps} start"), None)
if cut is None:
    raise SystemExit(f"soaklaps: {a.src} has no lap {a.laps} (it is the whole soak)")
head = [f"# {a.laps}-lap cut of iso_burg_soak.txt (tools/soaklaps.py {a.laps}); the full soak's header follows"]
out = head + lines[:cut] + ["log soak done", "shot end", "end"]
dst = a.out or os.path.join(os.path.dirname(a.src), f"iso_burg_soak{a.laps}.txt")
open(dst, "w").write("\n".join(out) + "\n")
print(dst)
