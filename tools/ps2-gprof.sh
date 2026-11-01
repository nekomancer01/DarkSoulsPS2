#!/bin/bash
# gprof profile of the game (tools-hwperf): make GPROF=1 builds proto_gprof.sym.elf (every C file with -pg in
# build/obj/gprof/, ps2sdk libprofglue: its timer alarm samples the function _mcount entered last, so the flat
# profile counts time per function from its entry, and -pg adds an mcount call to every function = slower than the
# real game). src/prof.c starts sampling PROF_GPROF_START frames into the loop and writes host:gmon.out after
# PROF_GPROF_FRAMES frames; gprof then prints the flat profile + call graph.
#   ps2-gprof.sh [secs=75] [MAP=<area>]    build, run headless in PCSX2 (HostFs, ELF folder writable), report
#   ps2-gprof.sh report                     gprof on the last build/gmon.out only
# On a real PS2: tools/ps2-run.sh <ip> build/proto_gprof.sym.elf writes gmon.out into build/ via ps2client.
# Heavy: run as tools/q tools/ps2-gprof.sh.
set -e
H=$(cd "$(dirname "$0")/.." && pwd)
P=$H/ps2/proto
B=${BUILD:-$H/build}
GPROF=${GPROF_BIN:-mips64r5900el-ps2-elf-gprof}
MAPARG=; for a in "$@"; do [[ $a == MAP=* ]] && MAPARG=$a; done
ELF=$B/proto_gprof.sym.elf
[[ $MAPARG == MAP=* && $MAPARG != MAP=m10_02_00_00 ]] && ELF=$B/proto_${MAPARG#MAP=}_gprof.sym.elf
if [ "$1" != report ]; then
    T=${1:-75}; [[ $T == MAP=* ]] && T=75
    make -C "$P" GPROF=1 $MAPARG sym
    rm -f "$B/gmon.out"
    RW=1 "$H/tools/ps2-pcsx2.sh" run "$ELF" "$T"
fi
[ -s "$B/gmon.out" ] || { echo "no gmon.out (run longer: PROF_GPROF_START + PROF_GPROF_FRAMES frames)" >&2; exit 1; }
"$GPROF" -b "$ELF" "$B/gmon.out" > "$H/tools/ps2-gprof.txt"
echo "gprof report: $H/tools/ps2-gprof.txt"
sed -n '/Flat profile/,/^$/p;/cumulative/,+25p' "$H/tools/ps2-gprof.txt" | head -32
