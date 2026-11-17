#!/usr/bin/env python3
"""Heap census of a `make HEAPMAP=1` run: who holds the EE heap, per caller and label.

  heapmap.py <emulog.txt> <proto.sym.elf> [t ...]

The HEAPMAP build (ps2/proto/src/hitch.c) prints every live allocation summed per caller after each 10 s hitch line
("heapmap: t <s> live ..." then one "heapmap: <caller> <KB> <blocks> [<label>]" line per caller holding 4 KB or more;
labels: the pack entry in pack_load_with, bones / vertices of the model in model.c). This resolves the callers with
addr2line (ps2/env.sh in the environment) and prints one column per chosen time t (default: the first one and the one
with the most live bytes), largest first. The same build's VRAM census (render.c r_vram_census: "vrammap:" lines after
each heapmap block) follows: the totals line, gsKit_vram_alloc calls by file:line, r_vram_* owners by caller / label.
--min-free: one column, the time with the least free heap (the hitch line before the block)."""
import re
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    args = [a for a in sys.argv[1:] if a != "--min-free"]
    log, elf, ts = args[0], args[1], args[2:]
    d, live, cur, vram, hfree, last_free = {}, {}, None, {}, {}, None
    for line in open(log, errors="replace"):
        m = re.search(r"hitch: .*heap free (\d+) KB", line)
        if m:
            last_free = int(m[1])
        m = re.search(r"vrammap: (.*)", line)
        if m and cur:
            vram.setdefault(cur, []).append(m[1])
            continue
        m = re.search(r"heapmap: t (\d+) live (\d+) KB", line)
        if m:
            cur = m[1]
            d[cur], live[cur], hfree[cur], vram[cur] = {}, int(m[2]), last_free, []
            continue
        m = re.search(r"heapmap: (0x[0-9a-f]+) (\d+) KB (\d+) \[(.*)\]", line)
        if m and cur:
            d[cur][(m[1], m[4])] = (int(m[2]), int(m[3]))
    if not d:
        sys.exit("no heapmap lines (build with make HEAPMAP=1)")
    if "--min-free" in sys.argv:
        ts = [min((t for t in hfree if hfree[t] is not None), key=hfree.get)]
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
    print(f"{'heap free (hitch line)':63}" + "".join(f"{hfree[t] or 0:>10}    " for t in ts))
    for t in ts:
        if not vram.get(t):
            continue
        print(f"VRAM at t {t}")
        ra = [m[1] for m in (re.match(r"(0x[0-9a-f]+) ", v) for v in vram[t]) if m]
        out = subprocess.run(["mips64r5900el-ps2-elf-addr2line", "-f", "-e", elf] + ra, capture_output=True, text=True,
                             check=True).stdout.split("\n") if ra else []
        vname = {a: out[2 * i] for i, a in enumerate(ra)}
        rows = []
        for v in vram[t]:
            m = re.match(r"(0x[0-9a-f]+) (\d+) KB (\d+) \[(.*)\]", v)
            if m:
                rows.append((int(m[2]), f"  {vname[m[1]][:38]:38} {m[4][:22]:22} {m[2]:>6} KB /{m[3]}"))
            else:
                print("  " + v)
        for _, r in sorted(rows, reverse=True):
            print(r)


if __name__ == "__main__":
    main()
