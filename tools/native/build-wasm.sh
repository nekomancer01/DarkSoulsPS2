#!/bin/sh
# Build the C helpers (lz4pack, bcdecode, simplify) as WebAssembly side modules for Pyodide (the in-browser converter,
# tools/pages): the same sources and flags as the gcc builds in pack.py / souls.py / simplify_native.py, linked with
# wasm-ld's dynamic-linking (dylink.0) format, so Pyodide's ctypes.CDLL loads them like the native .so files and
# resolves memcpy / malloc / qsort from its own libc. Needs clang and wasm-ld (LLVM 17 or newer; Ubuntu: clang lld).
# Usage: tools/native/build-wasm.sh <out dir>     -> <out dir>/lib{lz4pack,bcdecode,simplify}.so
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: build-wasm.sh <out dir>}
mkdir -p "$OUT"
CLANG=${CLANG:-clang}
WASMLD=${WASMLD:-wasm-ld}
RES=$("$CLANG" -print-resource-dir)/include
"$WASMLD" --version
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for m in lz4pack bcdecode simplify; do
    extra=
    [ $m = simplify ] && extra="-ffp-contract=off -fno-fast-math"  # as simplify_native.py: same collapses as simplify.py
    # shellcheck disable=SC2086
    "$CLANG" --target=wasm32-unknown-emscripten -fPIC -O2 -fvisibility=default $extra -nostdinc -isystem "$RES" \
        -isystem "$HERE/wasm" -c "$HERE/$m.c" -o "$TMP/$m.o"
    # --allow-undefined: libc (malloc, calloc, ...) comes from Pyodide at load time; newer wasm-ld refuses undefined
    # symbols in a shared module without it
    "$WASMLD" --experimental-pic -shared --export-dynamic --allow-undefined -o "$OUT/lib$m.so" "$TMP/$m.o"
    echo "$OUT/lib$m.so: $(wc -c < "$OUT/lib$m.so") B"
done
