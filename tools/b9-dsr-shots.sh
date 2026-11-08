#!/bin/bash
# Burg plan B9: DSR reference shots matching ps2/proto/tests/iso_burg_looks.txt (reference install, tools/dsr-ref.sh;
# save burg.sl2 = Burg bonfire). Usage: tools/b9-dsr-shots.sh <outdir>
set -e; T=$(cd "$(dirname "$0")" && pwd); O=$(realpath -m "$1"); mkdir -p "$O"
$T/dsr-ref.sh stop >/dev/null 2>&1 || true; $T/dsr-ref.sh restore burg; $T/dsr-ref.sh start; $T/dsr-ref.sh load
$T/dsr-ref.sh cam --pos -65.70 -21.58 -23.05 --yaw 137.5 --pitch -8.0 --hide --shot "$O/stairs.png"
$T/dsr-ref.sh cam --pos -51.06 -20.38 -32.57 --yaw 149.0 --pitch -5.7 --hide --shot "$O/cave_a.png"
$T/dsr-ref.sh cam --pos 1.44 -8.58 -58.04 --yaw 171.9 --pitch -8.0 --hide --shot "$O/burg_a.png"
$T/dsr-ref.sh cam --pos -9.64 -8.58 -71.34 --yaw 114.6 --pitch -8.0 --hide --shot "$O/burg_c.png"
$T/dsr-ref.sh cam --pos -2.92 -5.88 -67.51 --yaw 28.6 --pitch -8.0 --hide --shot "$O/bonfire.png"
$T/dsr-ref.sh cam --pos -59.64 -21.38 -27.84 --yaw 114.6 --pitch -5.7 --hide --shot "$O/door_w.png"
$T/dsr-ref.sh stop
