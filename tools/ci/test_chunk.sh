#!/bin/sh
# Host test of the chunk streams (Polaris 3.3): the runtime reader ps2/proto/src/chunk.c and the C writer
# tools/native/chunkw.h, 64-bit (tools/ci/test_chunk.c: every refusal), and tools/chunkfile.py writing the same bytes as the
# C writer and reading them back. CI job "hygiene".
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
${CC:-cc} -std=gnu11 -Wall -Werror -O1 -I"$ROOT/ps2/proto/src" "$ROOT/tools/ci/test_chunk.c" "$ROOT/ps2/proto/src/chunk.c" \
    -o "$OUT/test_chunk" && "$OUT/test_chunk" "$OUT/c.bin" || exit 1
${PY:-python3} - "$ROOT/tools" "$OUT/c.bin" <<'PYEOF' || exit 1
import struct, sys
sys.path.insert(0, sys.argv[1])
import chunkfile as chunk
b = chunk.pack(b"TEST", [(b"HEAD", 2, struct.pack("<3I", 1, 2, 3)), (b"DATA", 1, b"abcdefghijklmnopq"), (b"NONE", 7, b""),
                         (b"ALGN", 1, b"256", 256), chunk.vu1_chunk()])
c = open(sys.argv[2], "rb").read()
assert b == c, f"tools/chunkfile.py and chunkw.h differ ({len(b)} / {len(c)} B)"
f = chunk.File(c, b"TEST")
assert f.get(b"DATA", 1) == b"abcdefghijklmnopq" and f.opt(b"MISS", 1) is None and b"PAD " not in f.order
assert c.index(b"256") % 256 == 0, "ALGN payload not 256-aligned"
for bad, what in ((lambda: f.get(b"HEAD", 3), "version"), (lambda: chunk.File(c[:-16]), "truncated"),
                  (lambda: chunk.File(c, b"WRLD"), "file id")):
    try:
        bad()
        sys.exit(f"tools/chunkfile.py accepted a bad {what}")
    except chunk.ChunkError:
        pass
print("test_chunk: tools/chunkfile.py = chunkw.h, reader refusals ok")
PYEOF
