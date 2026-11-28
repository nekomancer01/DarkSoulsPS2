#!/bin/sh
# Host test of the streamed items' LZ4 index reader and in-place decoder (MAP_LZ4; ps2/proto/src/mslz.c), 64-bit, no PS2 SDK:
# tools/lz4items.py writes mapres.lz.bin / cells.lz.bin / lzidx.bin for the synthetic block of tools/ci/fixtures/lz4items (and,
# with a gen dir argument, for every block under it), tools/ci/test_mslz.c opens them and decodes every near part, every
# resident item and every group of consecutive items in a slot as the streamer sizes it (raw + LZ_SLACK(packed), read at the
# slot's end), frame by frame, and checks every refusal. lz4_decode is cut out of pack.c as tools/lz4hc/bench.py does.
# Usage: tools/ci/test_mslz.sh [<build>/gen]    CI job "hygiene" (via tools/ci/offline.sh)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
${PY:-python3} -I - "$ROOT" "$OUT" "$1" <<'PYEOF' || exit 1
import os, re, sys
root, out, gen = sys.argv[1:4]
sys.path[:0] = [os.path.join(root, "tools"), os.path.join(root, "tools", "ci", "fixtures", "lz4items")]
import lz4items as lz
import make_block
src = open(os.path.join(root, "ps2/proto/src/pack.c")).read()
m = re.search(r"static int lz4_decode\(.*?\n}\n", src, re.S)
if not m:
    sys.exit("test_mslz: lz4_decode not found in pack.c")
open(os.path.join(out, "dec.c"), "w").write("#include <string.h>\n" + m.group(0) +
    "\nint pack_lz4_decode(const unsigned char *s, unsigned n, unsigned char *dst, unsigned cap)\n{\n    return lz4_decode(s, n, dst, cap);\n}\n")
blocks = [("fixture",) + make_block.build()]
if gen:
    for d in sorted(os.listdir(gen)):
        p = [os.path.join(gen, d, f) for f in ("maptab.disc.bin", "mapres.bin", "cells.disc.bin")]
        if os.path.exists(p[0]) and os.path.exists(p[2]):
            blocks.append((d,) + tuple(open(x, "rb").read() if os.path.exists(x) else b"" for x in p))
n = 0
for name, tab, res, cells in blocks:
    r = lz.build(tab, res, cells, store=False)
    if r is None:
        continue
    d = os.path.join(out, "b%d" % n)
    os.mkdir(d)
    for fn, b in (("lzidx.bin", r["lzidx"]), ("mapres.lz.bin", r["res_lz"]), ("cells.lz.bin", r["cells_lz"]),
                  ("mapres.bin", res), ("cells.disc.bin", cells)):
        open(os.path.join(d, fn), "wb").write(b)
    rows, h = lz.cell_table(tab)
    lines = [name, "cells %d" % len(rows)] + ["cell %d %d %d" % (i, r_[0], r_[1]) for i, r_ in enumerate(rows)]
    items = sorted(set((o, k) for _, o, k in lz.res_items(tab, h) if k))
    its, end = [], 0
    for o, k in items:  # (no overlaps in a real block; skip one if there were)
        if o >= end:
            its.append((o, k))
            end = o + k
    lines += ["item %d %d" % x for x in its]
    i = 0
    while i < len(its):  # groups as ms_issue forms them: rel contiguous with gaps <= 64 B, cut at the page cap
        j, e = i + 1, its[i][0] + its[i][1]
        while j < len(its) and its[j][0] - e <= 64 and its[j][0] + its[j][1] - its[i][0] <= lz.LZ_GRP_CAP:
            e = its[j][0] + its[j][1]
            j += 1
        if j > i + 1:
            lines.append("grp %d %d" % (its[i][0], e - its[i][0]))
        i = j
    open(os.path.join(d, "manifest.txt"), "w").write("\n".join(lines) + "\n")
    n += 1
open(os.path.join(out, "count"), "w").write("%d\n" % n)
PYEOF
${CC:-cc} -std=gnu11 -Wall -Werror -O1 -I"$ROOT/ps2/proto/src" "$ROOT/tools/ci/test_mslz.c" "$ROOT/ps2/proto/src/mslz.c" \
    "$ROOT/ps2/proto/src/chunk.c" "$OUT/dec.c" -o "$OUT/test_mslz" || exit 1
N=$(cat "$OUT/count")
i=0
while [ "$i" -lt "$N" ]; do
    "$OUT/test_mslz" "$OUT/b$i" || exit 1
    i=$((i + 1))
done
