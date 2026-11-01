#!/bin/bash
# usage: spots.sh tag "flags" secs spot...   (spot = A S L G E or n:x:y:z:yaw); HUD_DEBUG shots, emulog kept; restores map.bin + pak + plain build
R=$(cd "$(dirname "$0")/../../.." && pwd)  # repo root
PCSX2_LOG=${PCSX2_LOG:-${DISTROBOX_HOST_HOME:-$HOME}/.var/app/net.pcsx2.PCSX2/config/PCSX2/logs/emulog.txt}
S=/tmp/claude-1000/ab; TAG=$1; FL=$2; T=$3; shift 3
cd $R/ps2/proto; source ../env.sh
PAK=data/m10_02_00_00.pak
pak() { python3 -I ../../tools/pack.py $PAK map.bin=$R/build/gen/map.bin collision.bin=$R/build/gen/collision.bin enemies.bin=$R/build/gen/enemies.bin objects.bin=$R/build/gen/objects.bin ai.lua0=$R/build/gen/ai_map.lua0 >/dev/null; }
MK="make -o $R/build/gen/map.bin -o $PAK"
cp $R/build/gen/map.bin $S/map.bin.orig
restore() { cp $S/map.bin.orig $R/build/gen/map.bin; pak; $MK >/dev/null 2>&1 && echo restored; }
trap restore EXIT
ALL="A:-47.41:-59.82:53.30:325 S:-47.41:-59.82:53.30:35 L:-11.7:-42.5:108.1:270 G:-34.8:-57.0:53.1:20 E:-34.9:-30.85:106.46:233"
LOG=$PCSX2_LOG
for spot in "$@"; do
  case "$spot" in *:*) ;; *) spot=$(echo $ALL | tr ' ' '\n' | grep "^$spot:");; esac
  IFS=: read n x y z yaw <<< "$spot"
  $R/tools/py -c "
import struct, math
b = bytearray(open('$S/map.bin.orig', 'rb').read())
struct.pack_into('<4f', b, 20, $x, $y, $z, math.radians($yaw))
open('$R/build/gen/map.bin', 'wb').write(b)"
  pak
  $MK EE_DBGINFOFLAGS="-DHUD_DEBUG $FL" > $S/make.log 2>&1 || { echo build failed; grep -E " error|Error" $S/make.log | head -3; exit 1; }
  $R/tools/pcsx2-run.sh proto.elf $T $S/${TAG}_$n.png
  cp $LOG $S/${TAG}_$n.log
  echo "$TAG $n done"
done
