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
# chunkver.py (tools/ci/chunkver.py): the current tree passes, and a reader / writer pair with different versions fails
${PY:-python3} -I "$ROOT/tools/ci/chunkver.py" >/dev/null || { echo "test_chunk: chunkver.py fails on the tree"; exit 1; }
mkdir -p "$OUT/fx/ps2/proto/src" "$OUT/fx/tools"
cat > "$OUT/fx/ps2/proto/src/a.c" <<'CEOF'
void f(void) { chunk_open(&f, b, n, CHUNK_ID('T', 'E', 'S', 'T'), "t"); chunk_get(&f, CHUNK_ID('D', 'A', 'T', 'A'), 2, &n); }
CEOF
echo 'x = chunkfile.pack(b"TEST", [(b"DATA", 1, b"")])' > "$OUT/fx/tools/w.py"
if ${PY:-python3} -I "$ROOT/tools/ci/chunkver.py" --root "$OUT/fx" >"$OUT/fx.out"; then
    echo "test_chunk: chunkver.py accepted a reader v2 / writer v1 mismatch"; exit 1
fi
grep -q "TEST/DATA reader v2" "$OUT/fx.out" || { echo "test_chunk: chunkver.py row missing"; cat "$OUT/fx.out"; exit 1; }
echo 'x = chunkfile.pack(b"TEST", [(b"DATA", 2, b"")])' > "$OUT/fx/tools/w.py"
${PY:-python3} -I "$ROOT/tools/ci/chunkver.py" --root "$OUT/fx" >/dev/null || { echo "test_chunk: chunkver.py rejected matching versions"; exit 1; }
echo "test_chunk: chunkver.py catches a version mismatch"
