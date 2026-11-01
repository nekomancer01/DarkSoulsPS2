#!/bin/bash
# A/B: build variant, run headless, screenshot; restores the plain build at the end. Usage: run_ab.sh name "flags" secs [name "flags" secs]...
R=$(cd "$(dirname "$0")/../../.." && pwd)  # repo root
PCSX2_LOG=${PCSX2_LOG:-${DISTROBOX_HOST_HOME:-$HOME}/.var/app/net.pcsx2.PCSX2/config/PCSX2/logs/emulog.txt}
cd $R/ps2/proto
source ../env.sh
OUT=/tmp/claude-1000/ab
while [ $# -ge 3 ]; do
  N=$1; FL=$2; T=$3; shift 3
  make -o $R/build/gen/map.bin -o data/m10_02_00_00.pak EE_DBGINFOFLAGS="$FL" > $OUT/$N.build.log 2>&1 || { echo "BUILD FAIL $N"; tail -5 $OUT/$N.build.log; continue; }
  $R/tools/pcsx2-run.sh proto.elf $T $OUT/$N.png
  cp $PCSX2_LOG $OUT/$N.emu.log 2>/dev/null
  echo "done $N"
done
make -o $R/build/gen/map.bin -o data/m10_02_00_00.pak > $OUT/restore.build.log 2>&1 && echo restored || echo RESTORE FAILED
