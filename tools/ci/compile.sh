#!/bin/sh
# Data-free compile check for PR CI (.github/workflows/pr-checks.yml, job "compile"): compiles every ps2/proto/src/*.c
# with the same flags as ps2/proto/Makefile plus -Werror, and assembles the VU1 .vsm programs. No source needs the game
# (game values load at boot, src/gamedata.c; the *_data.h layout headers check themselves with _Static_assert). Needs
# PS2DEV (the ps2dev/ps2dev container sets it). The game-free ELF itself: make -C ps2/proto elf (needs make).
# Usage: tools/ci/compile.sh [extra CFLAGS...]   (after source ps2/env.sh), or without a local toolchain:
#   docker run --rm -v "$PWD:/src" -w /src ps2dev/ps2dev:v2.0.0 tools/ci/compile.sh
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[ -n "$PS2DEV" ] || { echo "PS2DEV not set (source ps2/env.sh)"; exit 2; }
: "${PS2SDK:=$PS2DEV/ps2sdk}" "${GSKIT:=$PS2DEV/gsKit}"
PATH=$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2DEV/dvp/bin:$PATH
export PS2DEV PS2SDK GSKIT PATH
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
# Same flags as ps2/proto/Makefile: ps2sdk's samples/Makefile.eeglobal adds -D_EE -G0 and its include dirs, the
# Makefile adds -O2 -std=gnu99 -Wall and gsKit. Debug info is left out (it doesn't change warnings). No make needed,
# so the bare ps2dev container runs this as is.
CC="mips64r5900el-ps2-elf-gcc -D_EE -G0 -O2 -std=gnu99 -Wall -I$PS2SDK/ee/include -I$PS2SDK/common/include -I. \
-I$GSKIT/include -Isrc"
cd "$ROOT/ps2/proto" || exit 2
echo "compiler: $CC -Werror $*"
FAIL=0; N=0
for f in src/*.c; do
    N=$((N + 1))
    # shellcheck disable=SC2086  # $CC is a word list
    $CC -Werror "$@" -c "$f" -o "$OUT/$(basename "$f" .c).o" || { echo "FAIL $f"; FAIL=$((FAIL + 1)); }
done
for f in src/*.vsm; do
    N=$((N + 1))
    dvp-as "$f" -o "$OUT/$(basename "$f" .vsm).vo" || { echo "FAIL $f"; FAIL=$((FAIL + 1)); }
done
echo "compiled $N files, $FAIL failed"
[ "$FAIL" = 0 ]
