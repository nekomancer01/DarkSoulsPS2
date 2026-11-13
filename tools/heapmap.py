#!/usr/bin/env python3
"""Heap census of a `make HEAPMAP=1` run: who holds the EE heap, per caller and label.

  heapmap.py <emulog.txt> <proto.sym.elf> [t ...]

The HEAPMAP build (ps2/proto/src/hitch.c) prints every live allocation summed per caller after each 10 s hitch line
("heapmap: t <s> live ..." then one "heapmap: <caller> <KB> <blocks> [<label>]" line per caller holding 4 KB or more;
labels: the pack entry in pack_load_with, bones / vertices of the model in model.c). This resolves the callers with
addr2line (ps2/env.sh in the environment) and prints one column per chosen time t (default: the first one and the one
with the most live bytes), largest first."""
import re
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    log, elf, ts = sys.argv[1], sys.argv[2], sys.argv[3:]
    d, live, cur = {}, {}, None
    for line in open(log, errors="replace"):
        m = re.search(r"heapmap: t (\d+) live (\d+) KB", line)
        if m:
            cur = m[1]
            d[cur], live[cur] = {}, int(m[2])
            continue
        m = re.search(r"heapmap: (0x[0-9a-f]+) (\d+) KB (\d+) \[(.*)\]", line)
        if m and cur:
            d[cur][(m[1], m[4])] = (int(m[2]), int(m[3]))
    if not d:
        sys.exit("no heapmap lines (build with make HEAPMAP=1)")
    ts = ts or list(dict.fromkeys([next(iter(d)), max(live, key=live.get)]))
    addrs = sorted({a for t in ts for a, _ in d[t]})
    out = subprocess.run(["mips64r5900el-ps2-elf-addr2line", "-f", "-e", elf] + addrs, capture_output=True,
                         text=True, check=True).stdout.split("\n")
    name = {a: out[2 * i] + " " + out[2 * i + 1].split("/")[-1].split(" ")[0] for i, a in enumerate(addrs)}
    keys = sorted({k for t in ts for k in d[t]}, key=lambda k: -max(d[t].get(k, (0, 0))[0] for t in ts))
    print(f"{'caller':40} {'label':22}" + "".join(f"{'t ' + t + ' KB/n':>14}" for t in ts))
    for k in keys:
        print(f"{name[k[0]][:40]:40} {k[1][:22]:22}" + "".join("%10d/%-3d" % d[t].get(k, (0, 0)) for t in ts))
    print(f"{'live (all callers)':63}" + "".join(f"{live[t]:>10}    " for t in ts))


if __name__ == "__main__":
    main()
