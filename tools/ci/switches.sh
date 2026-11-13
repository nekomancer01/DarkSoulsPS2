#!/bin/sh
# Fallback / debug switch builds for PR CI (.github/workflows/pr-checks.yml, job "switches"): every build in the CI
# column of ps2/proto/src/switches.h, compiled with tools/ci/compile.sh's flags plus -Werror and linked into an ELF like
# make elf's. A plain build's objects are made once; each switch build recompiles only the sources the switch can reach
# (the files that name it and every file including a header that does) and relinks. Needs PS2DEV (the ps2dev/ps2dev
# container sets it).
# Usage: tools/ci/switches.sh [NAME...]   (no names: every CI build; JOBS=n parallel builds, default nproc)
#   docker run --rm -v "$PWD:/src" -w /src ps2dev/ps2dev:v2.0.0 tools/ci/switches.sh
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[ -n "$PS2DEV" ] || { echo "PS2DEV not set (source ps2/env.sh)"; exit 2; }
: "${PS2SDK:=$PS2DEV/ps2sdk}" "${GSKIT:=$PS2DEV/gsKit}"
PATH=$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2DEV/dvp/bin:$PS2SDK/bin:$PATH
export PS2DEV PS2SDK GSKIT PATH
# same compile flags as tools/ci/compile.sh (= ps2/proto/Makefile's), link as ps2sdk's Makefile.eeglobal + the Makefile
CC="mips64r5900el-ps2-elf-gcc -D_EE -G0 -O2 -std=gnu99 -Wall -Werror -fno-math-errno -I$PS2SDK/ee/include \
-I$PS2SDK/common/include -I. -I$GSKIT/include -Isrc"
LD="mips64r5900el-ps2-elf-gcc -T$PS2SDK/ee/startup/linkfile -O2"
LDFLAGS="-L$PS2SDK/ee/lib -Wl,-zmax-page-size=128 -L$GSKIT/lib -Wl,--wrap=_write \
-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign,--wrap=free"
LIBS="-lgskit -ldmakit -lpacket2 -ldma -lpad -lmc -lpatches -lm"
export CC LD LDFLAGS LIBS
cd "$ROOT/ps2/proto" || exit 2
if [ -z "$SWITCHES_OUT" ]; then
    SWITCHES_OUT=$(mktemp -d)
    trap 'rm -rf "$SWITCHES_OUT"' EXIT
fi
OUT=$SWITCHES_OUT
export OUT SWITCHES_OUT

# one switch build: $1 = index, $2 = name, $3 = compiler args
if [ "$1" = --one ]; then
    i=$2 name=$3 args=$4 d=$OUT/v$2
    mkdir -p "$d"
    # sources the switch reaches: files that name it, then (to a fixed point) files including a header in the set
    set_=$(grep -lw "$name" src/*.c src/*.h | grep -v 'src/switches.h')
    for d_ in $(echo "$args" | grep -o -- '-D[A-Za-z_][A-Za-z0-9_]*' | sed 's/^-D//'); do
        set_="$set_ $(grep -lw "$d_" src/*.c src/*.h | grep -v 'src/switches.h')"
    done
    set_=$(echo "$set_" | tr ' ' '\n' | sort -u)
    while :; do
        more=$set_
        for h in $(echo "$set_" | grep '\.h$'); do
            more="$more $(grep -l "#include \"$(basename "$h")\"" src/*.c src/*.h)"
        done
        more=$(echo "$more" | tr ' ' '\n' | grep . | sort -u)
        [ "$more" = "$set_" ] && break
        set_=$more
    done
    objs=$(cat "$OUT/base/objs")
    log=$d/log
    : > "$log"
    pg=
    case " $args " in *" -DGPROF "*) pg=-pg ;; esac
    for c in $(echo "$set_" | grep '\.c$'); do
        o=$(basename "$c" .c).o
        # shellcheck disable=SC2086  # word lists
        $CC $pg $args -c "$c" -o "$d/$o" >> "$log" 2>&1 || { echo "FAIL $name [$args]: $c"; sed 's/^/    /' "$log" | head -20; exit 1; }
        objs=$(echo "$objs" | sed "s| $OUT/base/$o | $d/$o |")
    done
    extra=
    case " $args " in *" -DPROFILE "*) $CC $pg $args -c src/prof.c -o "$d/prof.o" >> "$log" 2>&1 && extra="$d/prof.o" ;; esac
    case " $args " in *" -DGPROF "*) extra="$extra -lprofglue" ;; esac
    # shellcheck disable=SC2086
    $LD -o "$d/proto.elf" $objs $LDFLAGS $LIBS $extra >> "$log" 2>&1 || { echo "FAIL $name [$args]: link"; sed 's/^/    /' "$log" | head -20; exit 1; }
    echo "ok   $name [$args] ($(echo "$set_" | grep -c '\.c$') files)"
    rm -rf "$d"
    exit 0
fi

# the plain build, once: every object of make elf (all src/*.c but prof.c / aligncheck.c, the VU1 programs, the IRX
# modules as bin2c arrays: dsrsnd as tools/ci/compile.sh links it, libsd from ps2sdk)
B=$OUT/base
mkdir -p "$B"
echo "plain build"
FAIL=0
for c in src/*.c; do
    case $c in src/prof.c|src/aligncheck.c) continue ;; esac
    echo "$c"
done | xargs -P "${JOBS:-$(nproc)}" -I{} sh -c '$CC -c {} -o "$OUT/base/$(basename {} .c).o" || { echo "FAIL {}"; exit 255; }' \
    || FAIL=1
for v in src/*.vsm; do
    dvp-as "$v" -o "$B/$(basename "$v" .vsm).o" || FAIL=1
done
IOPCC="mipsel-none-elf-gcc -D_IOP -fno-builtin -G0 -O2 -Wall -Werror -msoft-float -mno-explicit-relocs \
-I$PS2SDK/iop/include -I$PS2SDK/common/include -Isrc -Iiop/dsrsnd"
{ printf '#include "irx_imports.h"\n'; cat iop/dsrsnd/imports.lst; } > "$B/dsrsnd_imports.c"
# shellcheck disable=SC2086
{ $IOPCC -c iop/dsrsnd/dsrsnd.c -o "$B/dsrsnd.io" && $IOPCC -fno-toplevel-reorder -c "$B/dsrsnd_imports.c" -o \
  "$B/dsrsnd_imports.io" && $IOPCC -T"$PS2SDK/iop/startup/linkfile" -nostdlib -dc -r -o "$B/dsrsnd.irx" \
  "$B/dsrsnd.io" "$B/dsrsnd_imports.io"; } || FAIL=1
cp "$PS2SDK/iop/irx/libsd.irx" "$B/libsd.irx"
for m in dsrsnd libsd; do
    bin2c "$B/$m.irx" "$B/${m}_irx.c" "${m}_irx" > /dev/null && $CC -c "$B/${m}_irx.c" -o "$B/${m}_irx.o" || FAIL=1
done
[ "$FAIL" = 0 ] || { echo "FAIL: the plain build does not compile (tools/ci/compile.sh shows why)"; exit 1; }
# shellcheck disable=SC2010  # object names have no spaces
echo " $(ls "$B"/*.o | tr '\n' ' ')" > "$B/objs"
# shellcheck disable=SC2046,SC2086
$LD -o "$B/proto.elf" $(cat "$B/objs") $LDFLAGS $LIBS > "$B/link.log" 2>&1 || { cat "$B/link.log"; echo "FAIL: the plain build does not link"; exit 1; }
echo "ok   plain build ($(wc -c < "$B/proto.elf") bytes)"

# the switch builds
# (the rows as tools/ci/switches.py --list reads them; awk, so the bare ps2dev container needs no python3)
awk -F'|' '/^ \* [A-Z][A-Z0-9_]* *\|/ && NF >= 6 {
    name = $1; sub(/^ \* /, "", name); gsub(/ /, "", name); ci = $4; gsub(/^ +| +$/, "", ci)
    if (ci == "-" || ci == "" || ci ~ /^\(/) next
    n = split(ci, b, ";"); for (i = 1; i <= n; i++) { gsub(/^ +| +$/, "", b[i]); if (b[i] != "") print name "\t" b[i] }
}' src/switches.h > "$OUT/list"
if [ $# -gt 0 ]; then
    for n in "$@"; do awk -F'\t' -v n="$n" '$1 == n' "$OUT/list"; done > "$OUT/list.sel"
    mv "$OUT/list.sel" "$OUT/list"
fi
N=$(wc -l < "$OUT/list")
awk -F'\t' '{ printf "%d\t%s\t%s\n", NR, $1, $2 }' "$OUT/list" | tr '\t\n' '\001\000' |
    xargs -0 -P "${JOBS:-$(nproc)}" -n1 sh -c 'IFS=$(printf "\001"); set -- $1; "$0" --one "$1" "$2" "$3"' "$ROOT/tools/ci/switches.sh" \
    > "$OUT/results" 2>&1
cat "$OUT/results"
BAD=$((N - $(grep -c '^ok' "$OUT/results")))
echo "switch builds: $N, $BAD failed"
[ "$BAD" = 0 ]
