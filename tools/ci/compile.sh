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
# Makefile adds -O2 -std=gnu99 -Wall -fno-math-errno (MATHFLAGS) and gsKit. Debug info is left out (it doesn't change
# warnings). No make needed, so the bare ps2dev container runs this as is.
CC="mips64r5900el-ps2-elf-gcc -D_EE -G0 -O2 -std=gnu99 -Wall -fno-math-errno -I$PS2SDK/ee/include -I$PS2SDK/common/include \
-I. -I$GSKIT/include -Isrc"
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
# No software double math on the EE (its FPU is single precision only, so a double op is a libgcc call of ~100+ cycles).
# Converting a float to double is allowed: printf varargs promote floats (log lines only), and esd.c reads the ESD
# bytecode's float64 literals. Arithmetic, compares and int<->double conversions are not, nor libm's double functions.
DBL='__(add|sub|mul|div|neg)df3|__(eq|ne|lt|le|gt|ge|unord)df2|__float(un)?(si|di)df|__fix(uns)?df(si|di)'
LIBM='(sin|cos|tan|asin|acos|atan|atan2|pow|exp|log|log10|sqrt|fabs|floor|ceil|fmod|round|trunc|hypot|ldexp)'
for o in "$OUT"/*.o; do
    bad=$(mips64r5900el-ps2-elf-nm -u "$o" | awk '{print $2}' | grep -E "^($DBL|$LIBM)\$" | tr '\n' ' ')
    [ -z "$bad" ] || { echo "FAIL $(basename "$o" .o).c: double-precision math: $bad"; FAIL=$((FAIL + 1)); }
done
# the IOP sound module (iop/dsrsnd, src/sound.c's driver): compiled and linked as ps2sdk's Makefile.iopglobal does
IOPCC="mipsel-none-elf-gcc -D_IOP -fno-builtin -G0 -O2 -Wall -Werror -msoft-float -mno-explicit-relocs \
-I$PS2SDK/iop/include -I$PS2SDK/common/include -Isrc -Iiop/dsrsnd"
N=$((N + 1))
{ printf '#include "irx_imports.h"\n'; cat iop/dsrsnd/imports.lst; } > "$OUT/dsrsnd_imports.c"
# shellcheck disable=SC2086  # $IOPCC is a word list
{ $IOPCC -c iop/dsrsnd/dsrsnd.c -o "$OUT/dsrsnd.o" && $IOPCC -fno-toplevel-reorder -c "$OUT/dsrsnd_imports.c" -o \
  "$OUT/dsrsnd_imports.o" && $IOPCC -T"$PS2SDK/iop/startup/linkfile" -nostdlib -dc -r -o "$OUT/dsrsnd.elf" \
  "$OUT/dsrsnd.o" "$OUT/dsrsnd_imports.o"; } || { echo "FAIL iop/dsrsnd"; FAIL=$((FAIL + 1)); }
echo "compiled $N files, $FAIL failed"
[ "$FAIL" = 0 ]
