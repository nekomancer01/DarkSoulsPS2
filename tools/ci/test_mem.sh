#!/bin/sh
# Host test of the memory regions' portable parts (ps2/proto/src/mem.c with MEM_HOST_TEST; Polaris 1.2). CI job "hygiene".
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
${CC:-cc} -std=gnu99 -Wall -Werror -O1 -DMEM_HOST_TEST -I"$ROOT/ps2/proto/src" "$ROOT/tools/ci/test_mem.c" \
    "$ROOT/ps2/proto/src/mem.c" -o "$OUT/test_mem" && "$OUT/test_mem"
