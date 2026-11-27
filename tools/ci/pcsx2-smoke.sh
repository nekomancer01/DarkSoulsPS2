#!/bin/bash
# Usage: tools/ci/pcsx2-smoke.sh <commit> [--max N] [--areas a,b] [--burg]
# The PCSX2 smoke run for the self-hosted runner on the maintainer's PC (.github/workflows/pcsx2-smoke.yml): nightly and
# after each merge to main. The game data never leaves that PC (AGENTS.md section 1): the job builds and tests in a
# dedicated worktree of the maintainer's main checkout and publishes only the step summary and the budget table.
#   1 tree     $DSR_CI_TREE (default <main checkout>/wt/ci), detached at <commit>; the git-ignored helpers the build
#              needs are linked from the main checkout like tools/worktree.sh does, config.local.mk copied
#   2 build    its own build/ (seeded once from the main checkout's build with tools/newbuild, then incremental)
#   3 test     tools/test-all.sh --areas $CI_AREAS --max N: make + make sym per area, smoke, render vs the previous
#              CI run, perf history, budget (everything goes through tools/q, so it waits for other jobs on the PC)
#   4 golden   make iso, then tools/golden.py compare --record-missing: the golden routes (ps2/proto/tests/golden_*.txt)
#              on the disc vs the base in build/golden/base (recorded by the first run), picture / frame time / refusals
#   --burg     instead of 3 + 4: tools/test-all.sh --iso --burg (builds the world ELF + ISO itself, then the Burg ISO
#              routes, ~1.5 h, one PCSX2 at a time): the manual gate before merging a perf change, not run per push
# Exit code: 0 when every gating step passed: build-* / build, smoke, the Burg routes (iso_* / world_*) and budget. The
# budget step gates through tools/budget.py --ratchet tools/ci/budget_ratchet.json: a frame / memory / disc gate of
# ps2/proto/src/frame_budget.h fails the run only when the ratchet has no entry at or above the measured value (today's
# known misses are entries; they only go down). tools/ci/test_budget.py (the ratchet's own test) gates too. render /
# perf / golden are reported, not gating: their limits are tuned per change.
# Home paths are printed as ~ (the run log and summary are public).
# Env: DSR_MAIN (main checkout, default ~/DarkSoulsPS2), DSR_CI_TREE, CI_AREAS (default Firelink + Burg).
set -o pipefail
SHA=$1; shift
[ -n "$SHA" ] || { sed -n '2p' "$0"; exit 2; }
MAX=20; AREAS=${CI_AREAS:-m10_02_00_00,m10_01_00_00}; BURG=
while [ $# -gt 0 ]; do
    case $1 in
        --max) MAX=$2; shift ;;
        --areas) AREAS=$2; shift ;;
        --burg) BURG=1 ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done
MAIN=${DSR_MAIN:-$HOME/DarkSoulsPS2}
TREE=${DSR_CI_TREE:-$MAIN/wt/ci}
SUMMARY=${GITHUB_STEP_SUMMARY:-/dev/null}
hide() { sed -u -e "s|$HOME|~|g" -e "s|/home/$USER/|~host/|g"; }  # distrobox home, then the host's

# 1 tree (fetch by commit id: the main checkout may be on any branch; its working files are never touched)
for i in 1 2 3 4; do git -C "$MAIN" fetch -q origin "$SHA" && break; sleep $((i * 10)); done
git -C "$MAIN" cat-file -e "$SHA^{commit}" || { echo "pcsx2-smoke: cannot fetch $SHA"; exit 1; }
if [ ! -e "$TREE/.git" ]; then
    git -C "$MAIN" worktree add -q --detach "$TREE" "$SHA" || exit 1
fi
git -C "$TREE" checkout -q -f --detach "$SHA" && git -C "$TREE" clean -q -fd -- . || exit 1  # ignored files stay
for p in ext tools/ext tools/vgmstream tools/liq tools/lz4hc/build data/ref .venv .venv-tools; do  # tools/worktree.sh LINKS
    [ -e "$MAIN/$p" ] && [ ! -e "$TREE/$p" ] && { mkdir -p "$(dirname "$TREE/$p")"; ln -s "$MAIN/$p" "$TREE/$p"; }
done
cp -a "$MAIN/config.local.mk" "$TREE/" 2>/dev/null
# 2 build
if [ ! -d "$TREE/build/gen" ] && [ -d "$MAIN/build/gen" ]; then
    bash "$TREE/tools/newbuild" "$TREE/build" --from "$MAIN/build" 2>&1 | hide
fi
# pcsx2-bin: the headless test runner is built once by hand (a 20-40 min dependency build has no place in a CI run)
BIN=${PCSX2_HEADLESS:-$MAIN/tools/ext/pcsx2-build/bin/pcsx2-dsrtest}
[ -x "$BIN" ] || { echo "pcsx2-smoke: pcsx2-dsrtest missing: run tools/pcsx2-headless/build.sh in $MAIN" | hide; exit 1; }
# 3 test
cd "$TREE" || exit 1
PY=$(sed -n 's/^PY *= *//p' config.local.mk 2>/dev/null | tail -1); PY=${PY:-python3}
T0=$(date +%s)
if [ -n "$BURG" ]; then
    echo "== $(git log -1 --format='%h %s') | Burg ISO routes"
    tools/test-all.sh --iso --burg --jobs 1 2>&1 | hide
else
    echo "== $(git log -1 --format='%h %s') | areas $AREAS | max $MAX"
    SMOKE_JOBS=${SMOKE_JOBS:-1} tools/test-all.sh --areas "$AREAS" --max "$MAX" 2>&1 | hide
fi
RUN=$(readlink -f build/test/last)
STEPS=$RUN/steps.txt
[ -f "$STEPS" ] || { echo "pcsx2-smoke: no steps.txt (test-all.sh did not get to the tests)"; exit 1; }
# build/test/last moves only at the end of test-all.sh: a run that died before it left the last one's steps there,
# which then reported an old pass for this commit
[ "$(stat -c %Y "$RUN")" -ge "$T0" ] || { echo "pcsx2-smoke: build/test/last is an older run (test-all.sh ended early)"; exit 1; }
GATE=0
gating() { case $1 in build|build-*|smoke|budget|iso_*|world_*) return 0 ;; esac; return 1; }
while read -r name status rest; do
    gating "$name" && { [ "$status" = PASS ] || GATE=1; }
    case $status in FAIL*) echo "::$(gating "$name" && echo error || echo warning)::$name $status" ;; esac
done < "$STEPS"
# the ratchet's own test (fixture results): a broken gate must not pass everything
"$PY" -I tools/ci/test_budget.py > "$RUN/test_budget.log" 2>&1 || { GATE=1; echo "::error::tools/ci/test_budget.py failed"; }
# 4 golden (reported, not gating; the base is this PC's first run, rebuilt by deleting build/golden/base)
GOLDEN=$RUN/golden.txt
if [ -n "$BURG" ]; then
    :  # (the Burg run is the ISO gate itself)
elif tools/q -m 12 bash -c "source ps2/env.sh && make -C ps2/proto sym iso" > "$RUN/golden-iso.log" 2>&1; then
    "$PY" tools/golden.py compare --record-missing > "$GOLDEN" 2>&1
    echo "golden $(grep -q 'route(s) over' "$GOLDEN" && grep -o '[0-9]* route(s) over' "$GOLDEN" || echo 'no table')" | hide
else
    echo "golden FAIL (make iso, $RUN/golden-iso.log)" > "$GOLDEN"
fi
{
    echo "### PCSX2 smoke: $([ $GATE = 0 ] && echo pass || echo FAIL) ($(git log -1 --format=%h), $([ -n "$BURG" ] && echo "Burg ISO routes" || echo "$AREAS, max $MAX"))"
    echo; echo '```'; hide < "$STEPS" | cut -c1-60; echo '```'
    if grep -q '^ratchet: ' "$RUN/budget.log" 2>/dev/null; then  # the budget gate's verdict, outside the folded table
        echo; echo '**Frame budget** (ps2/proto/src/frame_budget.h, ratchet tools/ci/budget_ratchet.json):'
        echo; echo '```'; grep '^ratchet: ' "$RUN/budget.log" | hide; echo '```'
    fi
    if [ -f "$GOLDEN" ]; then
        echo; echo '<details><summary>golden routes</summary>'; echo; echo '```'; hide < "$GOLDEN"; echo '```'; echo '</details>'
    fi
    if [ -f "$RUN/budget.log" ]; then
        echo; echo '<details><summary>budget table</summary>'; echo; echo '```'; hide < "$RUN/budget.log"; echo '```'; echo '</details>'
    fi
    echo; echo "Full report (screenshots, game data) stays on the runner PC: $RUN/report.html" | hide
} >> "$SUMMARY"
# keep the last 20 CI runs
ls -1d build/test/runs/2* 2>/dev/null | head -n -20 | grep -v "$(basename "$RUN")" | xargs -r rm -rf
exit $GATE
