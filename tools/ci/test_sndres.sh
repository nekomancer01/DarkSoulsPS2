#!/bin/sh
# Host test of the SPU2 residency decisions (ps2/proto/src/sndres.c; TECH_DEBT audio-2). CI job "hygiene".
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
${CC:-cc} -std=gnu99 -Wall -Werror -O1 -I"$ROOT/ps2/proto/src" "$ROOT/tools/ci/test_sndres.c" \
    "$ROOT/ps2/proto/src/sndres.c" -o "$OUT/test_sndres" && "$OUT/test_sndres"
