#!/bin/bash
# Usage: tools/test-all.sh [--no-build] [--areas m10_02_00_00,...] [--max N] [--gsdump] [--accept]
#        tools/test-all.sh --iso --burg [--no-build] [--reps N] [--jobs N]
# --iso --burg (the Burg plan's gate run, step B0): build the world ELF + its ISO (make, make sym, make iso), then boot
#   the ISO for each Burg route (BURG_TESTS: iso_burg_taurus, iso_ame_report3, world_route, world_sprint and a 2-lap
#   iso_burg_soak cut by tools/soaklaps.py), --reps times each (default 1), --jobs side by side (default 2), and print
#   one tools/budget.py table: every shot's passes, frames over 34 ms, RAM / VRAM, the disc gates, and the streamer's
#   run gates (budget.py STREAM_GATES: near "no room" refusals, near pop-ins, cells drawn coarse while due near,
#   horizon inside HZN_R). Results in build/test/runs/<time>/burg/<test>-<rep>/; report.html shows the same table.
# Build, then the headless test suite, then build/test/runs/<time>/report.html:
#   1 build    make + make sym for every exported area (build/data/<area>.pak), through the job queue (tools/q -m 12)
#   2 smoke    tools/test-smoke.py: FromSoft's map/autotest points per area, shot + stats per point; fails on crash /
#              hang / EE-log error, holes (magenta clear) or low fps
#   3 render   tools/test-gsdump.py compare: shots vs the previous run (build/test/last) and vs the accepted baseline
#              (build/test/baseline); with --gsdump each shot also has a GS dump and the comparison uses dump replays
#   4 perf     tools/test-perf.py: per-spot stats appended to build/test/perf-history.csv, compared with the previous build
#   5 budget   tools/budget.py: each shot's passes at p90 against the budget doc's frame budget
#   --accept   make this run the baseline for step 3
# Each PCSX2 step runs through tools/q with a small memory cap (one headless PCSX2 ~1.5 GB host RAM, outside the cap:
# the flatpak runs on the host). Exit code = number of failed steps.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
BUILD=${BUILD:-$ROOT/build}
if [ -z "$PY" ]; then  # Pillow + numpy: the Makefile's PY (config.local.mk / .venv), else python3
    PY=$(sed -n 's/^PY *= *//p' config.local.mk 2>/dev/null | tail -1)
    [ -n "$PY" ] || { [ -x .venv/bin/python ] && PY=.venv/bin/python || PY=python3; }
fi
DO_BUILD=1; AREAS=; MAX=120; GSDUMP=; ACCEPT=; ISO=; BURG=; REPS=1; JOBS=2
while [ $# -gt 0 ]; do
    case $1 in
        --no-build) DO_BUILD= ;;
        --areas) AREAS=$2; shift ;;
        --max) MAX=$2; shift ;;
        --gsdump) GSDUMP=--gsdump ;;
        --accept) ACCEPT=1 ;;
        --iso) ISO=1 ;;
        --burg) BURG=1 ;;
        --reps) REPS=$2; shift ;;
        --jobs) JOBS=$2; shift ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done
RUN=$BUILD/test/runs/$(date +%Y%m%d-%H%M%S)
mkdir -p "$RUN"
STEPS=$RUN/steps.txt
FAILED=0
step() {  # step <name> <command...>: run, log status + time to steps.txt
    local name=$1; shift
    local t0=$(date +%s)
    echo "== $name: $*"
    "$@" 2>&1 | tee "$RUN/$name.log"
    local rc=${PIPESTATUS[0]}
    printf '%-8s %s  %4ss  %s\n' "$name" "$([ $rc = 0 ] && echo PASS || echo "FAIL($rc)")" $(( $(date +%s) - t0 )) "$*" >> "$STEPS"
    [ $rc = 0 ] || FAILED=$((FAILED + 1))
    return $rc
}
if [ -n "$BURG" ]; then
    [ -n "$ISO" ] || { echo "test-all.sh: --burg needs --iso (the Burg gates are measured on the disc)"; exit 2; }
    # test script, timeout (s); the soak's 2-lap cut is written into the run dir
    BURG_TESTS=${BURG_TESTS:-"iso_burg_taurus:1500 iso_ame_report3:900 world_route:1800 world_sprint:1500 iso_burg_soak2:1800"}
    if [ -n "$DO_BUILD" ]; then
        step build tools/q -m 12 bash -c "source ps2/env.sh && make -C ps2/proto BUILD=$BUILD && make -C ps2/proto BUILD=$BUILD sym && make -C ps2/proto BUILD=$BUILD iso"
    fi
    ELF=$BUILD/proto.sym.elf; IMG=$BUILD/proto.iso
    [ -e "$ELF" ] && [ -e "$IMG" ] || { echo "test-all.sh: no $ELF / $IMG"; exit 1; }
    mkdir -p "$RUN/burg"
    "$PY" tools/soaklaps.py 2 "$RUN/burg/iso_burg_soak2.txt" > /dev/null
    burg_one() {  # <test> <timeout> <rep>
        local sc=ps2/proto/tests/$1.txt
        [ -e "$sc" ] || sc=$RUN/burg/$1.txt
        tools/q -m 2 "$PY" tools/test-run.py "$ELF" "$sc" "$RUN/burg/$1-$3" --iso "$IMG" --timeout "$2" --keep-going \
            --hitch --name "$1-$3" > "$RUN/burg/$1-$3.log" 2>&1
        echo "$1-$3 rc $?"
    }
    n=0
    for r in $(seq 1 "$REPS"); do
        for t in $BURG_TESTS; do
            burg_one "${t%%:*}" "${t##*:}" "$r" &
            n=$((n + 1))
            [ $((n % JOBS)) = 0 ] && wait
        done
    done
    wait
    step budget "$PY" tools/budget.py "$RUN/burg" --json "$RUN/budget.json"
    "$PY" tools/runstamp.py "$ELF" > "$RUN/stamp.json" 2>/dev/null
    "$PY" tools/test-report.py "$RUN" > /dev/null
    echo
    echo "== summary ($RUN)"
    cat "$STEPS"
    echo "report: $RUN/report.html"
    exit $FAILED
fi
[ -n "$AREAS" ] || AREAS=$(ls "$BUILD"/data/m??_??_??_??.pak 2>/dev/null | xargs -n1 basename 2>/dev/null | sed 's/\.pak$//' | paste -sd,)
[ -n "$AREAS" ] || AREAS=m10_02_00_00
if [ -n "$DO_BUILD" ]; then
    for A in ${AREAS//,/ }; do
        step build-$A tools/q -m 12 bash -c "source ps2/env.sh && make -C ps2/proto MAP=$A && make -C ps2/proto MAP=$A sym"
    done
fi
# areas side by side (SMOKE_JOBS, default 2; tools/test-smoke.py --jobs): one headless PCSX2 each
step smoke tools/q -m 4 "$PY" tools/test-smoke.py "$RUN/smoke" --areas "$AREAS" --max "$MAX" $GSDUMP
LAST=$(readlink -f "$BUILD/test/last" 2>/dev/null)
if [ -n "$LAST" ] && [ -d "$LAST/smoke" ]; then
    step render-last tools/q -m 3 "$PY" tools/test-gsdump.py compare "$RUN/smoke" "$LAST/smoke" "$RUN/regress-last.html"
fi
if [ -d "$BUILD/test/baseline/smoke" ]; then
    step render-base tools/q -m 3 "$PY" tools/test-gsdump.py compare "$RUN/smoke" "$BUILD/test/baseline/smoke" "$RUN/regress-baseline.html"
fi
ELFS=
for A in ${AREAS//,/ }; do
    [ "$A" = m10_02_00_00 ] && E=$BUILD/proto.sym.elf || E=$BUILD/proto_$A.sym.elf
    [ -e "$E" ] && ELFS="$ELFS $E"
done
if [ -f "$RUN/smoke/smoke.json" ] && [ -n "$ELFS" ]; then
    "$PY" tools/test-perf.py record "$RUN/smoke" "$BUILD/test/perf-history.csv" $ELFS
    step perf "$PY" tools/test-perf.py check "$BUILD/test/perf-history.csv" $ELFS --json "$RUN/perf.json"
fi
# the frame budget per shot (tools/budget.py; host ELF runs: no disc server, so no disc gates)
[ -d "$RUN/smoke" ] && step budget "$PY" tools/budget.py "$RUN/smoke"
# what this run tested (tools/runstamp.py): git revision + uncommitted-change hash, ELF hashes; shown in report.html
"$PY" tools/runstamp.py $ELFS > "$RUN/stamp.json" 2>/dev/null
ln -sfn "$RUN" "$BUILD/test/last"
[ -n "$ACCEPT" ] && ln -sfn "$RUN" "$BUILD/test/baseline"
"$PY" tools/test-report.py "$RUN" > /dev/null
echo
echo "== summary ($RUN)"
cat "$STEPS"
echo "report: $RUN/report.html"
exit $FAILED
