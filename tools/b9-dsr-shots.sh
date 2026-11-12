#!/bin/bash
# Burg plan B9: DSR reference shots matching ps2/proto/tests/iso_burg_looks.txt (reference install, tools/dsr-ref.sh;
# save burg.sl2 = Burg bonfire). The port's eye (main.c): feet + 1.42 up, 4 m back along yaw / pitch (pitch > 0 looks down;
# its camera collision may pull it in near walls). Usage: tools/b9-dsr-shots.sh <outdir>
set -e; T=$(cd "$(dirname "$0")" && pwd); O=$(realpath -m "$1"); mkdir -p "$O"
$T/dsr-ref.sh stop >/dev/null 2>&1 || true; $T/dsr-ref.sh restore burg; $T/dsr-ref.sh start; $T/dsr-ref.sh load
$T/dsr-ref.sh cam --pos -65.68 -21.02 -23.08 --yaw 137.5 --pitch -8.0 --hide --shot "$O/stairs.png"
$T/dsr-ref.sh cam --pos -51.05 -19.98 -32.59 --yaw 149.0 --pitch -5.7 --hide --shot "$O/cave_a.png"
$T/dsr-ref.sh cam --pos 1.44 -8.02 -58.08 --yaw 171.9 --pitch -8.0 --hide --shot "$O/burg_a.png"
$T/dsr-ref.sh cam --pos -9.60 -8.02 -71.35 --yaw 114.6 --pitch -8.0 --hide --shot "$O/burg_c.png"
$T/dsr-ref.sh cam --pos -2.90 -5.32 -67.48 --yaw 28.6 --pitch -8.0 --hide --shot "$O/bonfire.png"
$T/dsr-ref.sh cam --pos -59.62 -20.98 -27.84 --yaw 114.6 --pitch -5.7 --hide --shot "$O/door_w.png"
$T/dsr-ref.sh stop
