#!/bin/bash
# DSR reference shots matching ps2/proto/tests/iso_burg_outline.txt (reference install, tools/dsr-ref.sh; save burg.sl2).
# court and the taurus_* views do not match yet: DSR draws by the draw groups of the collision under the player, and the
# floating camera player keeps the bonfire's (TECH_DEBT tools). The port's eye: feet + 1.42 up, 4 m back along yaw / pitch (pitch > 0 looks down). Usage: tools/outline-dsr-shots.sh <outdir>
set -e; T=$(cd "$(dirname "$0")" && pwd); O=$(realpath -m "$1"); mkdir -p "$O"
$T/dsr-ref.sh stop >/dev/null 2>&1 || true; $T/dsr-ref.sh restore burg; $T/dsr-ref.sh start; $T/dsr-ref.sh load
$T/dsr-ref.sh cam --pos -7.90 -8.28 -73.38 --yaw 0.0 --pitch -5.7 --hide --shot "$O/bf_n.png"
$T/dsr-ref.sh cam --pos -11.88 -8.28 -69.40 --yaw 90.0 --pitch -5.7 --hide --shot "$O/bf_e.png"
$T/dsr-ref.sh cam --pos -7.91 -8.28 -65.42 --yaw 179.9 --pitch -5.7 --hide --shot "$O/bf_s.png"
$T/dsr-ref.sh cam --pos -3.92 -8.28 -69.39 --yaw 269.9 --pitch -5.7 --hide --shot "$O/bf_w.png"
$T/dsr-ref.sh cam --pos 1.44 -8.02 -58.08 --yaw 171.9 --pitch -8.0 --hide --shot "$O/street_a.png"
$T/dsr-ref.sh cam --pos -9.60 -8.02 -71.35 --yaw 114.6 --pitch -8.0 --hide --shot "$O/street_c.png"
$T/dsr-ref.sh cam --pos 8.29 -4.18 -101.44 --yaw 68.8 --pitch -5.7 --hide --shot "$O/court.png"
$T/dsr-ref.sh cam --pos 25.02 -11.79 -107.00 --yaw 286.5 --pitch -14.3 --hide --shot "$O/taurus_base.png"
$T/dsr-ref.sh cam --pos 18.17 -13.77 -103.62 --yaw 126.1 --pitch 14.3 --hide --shot "$O/taurus_up.png"
$T/dsr-ref.sh cam --pos 18.08 -12.38 -103.56 --yaw 126.1 --pitch -5.7 --hide --shot "$O/taurus_side.png"
$T/dsr-ref.sh stop
