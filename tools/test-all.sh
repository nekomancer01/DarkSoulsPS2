#!/bin/bash
# Usage: tools/test-all.sh [--no-build] [--areas m10_02_00_00,...] [--max N] [--gsdump] [--accept]
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
DO_BUILD=1; AREAS=; MAX=120; GSDUMP=; ACCEPT=
while [ $# -gt 0 ]; do
    case $1 in
        --no-build) DO_BUILD= ;;
        --areas) AREAS=$2; shift ;;
        --max) MAX=$2; shift ;;
        --gsdump) GSDUMP=--gsdump ;;
        --accept) ACCEPT=1 ;;
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
