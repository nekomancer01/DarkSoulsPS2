#!/bin/bash
# Usage: tools/ci/pcsx2-smoke.sh <commit> [--max N] [--areas a,b]
# The PCSX2 smoke run for the self-hosted runner on the maintainer's PC (.github/workflows/pcsx2-smoke.yml): nightly and
# after each merge to main. The game data never leaves that PC (AGENTS.md section 1): the job builds and tests in a
# dedicated worktree of the maintainer's main checkout and publishes only the step summary and the budget table.
#   1 tree     $DSR_CI_TREE (default <main checkout>/wt/ci), detached at <commit>; the git-ignored helpers the build
#              needs are linked from the main checkout like tools/worktree.sh does, config.local.mk copied
#   2 build    its own build/ (seeded once from the main checkout's build with tools/newbuild, then incremental)
#   3 test     tools/test-all.sh --areas $CI_AREAS --max N: make + make sym per area, smoke, render vs the previous
#              CI run, perf history, budget (everything goes through tools/q, so it waits for other jobs on the PC)
# Exit code: 0 when every gating step (build-*, smoke) passed. render / perf / budget are reported, not gating: their
# limits are tuned per change, and a red nightly that nobody can fix in one PR would hide the breakages this run is for.
# Home paths are printed as ~ (the run log and summary are public).
# Env: DSR_MAIN (main checkout, default ~/DarkSoulsPS2), DSR_CI_TREE, CI_AREAS (default Firelink + Burg).
set -o pipefail
SHA=$1; shift
[ -n "$SHA" ] || { sed -n '2p' "$0"; exit 2; }
MAX=20; AREAS=${CI_AREAS:-m10_02_00_00,m10_01_00_00}
while [ $# -gt 0 ]; do
    case $1 in
        --max) MAX=$2; shift ;;
        --areas) AREAS=$2; shift ;;
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
# 3 test
cd "$TREE" || exit 1
echo "== $(git log -1 --format='%h %s') | areas $AREAS | max $MAX"
tools/test-all.sh --areas "$AREAS" --max "$MAX" 2>&1 | hide
RUN=$(readlink -f build/test/last)
STEPS=$RUN/steps.txt
[ -f "$STEPS" ] || { echo "pcsx2-smoke: no steps.txt (test-all.sh did not get to the tests)"; exit 1; }
GATE=0
while read -r name status rest; do
    case $name in build-*|smoke) [ "$status" = PASS ] || GATE=1 ;; esac
    case $status in FAIL*) echo "::$([ "${name%%-*}" = build ] || [ "$name" = smoke ] && echo error || echo warning)::$name $status" ;; esac
done < "$STEPS"
{
    echo "### PCSX2 smoke: $([ $GATE = 0 ] && echo pass || echo FAIL) ($(git log -1 --format=%h), $AREAS, max $MAX)"
    echo; echo '```'; hide < "$STEPS" | cut -c1-60; echo '```'
    if [ -f "$RUN/budget.log" ]; then
        echo; echo '<details><summary>budget table</summary>'; echo; echo '```'; hide < "$RUN/budget.log"; echo '```'; echo '</details>'
    fi
    echo; echo "Full report (screenshots, game data) stays on the runner PC: $RUN/report.html" | hide
} >> "$SUMMARY"
# keep the last 20 CI runs
ls -1d build/test/runs/2* 2>/dev/null | head -n -20 | grep -v "$(basename "$RUN")" | xargs -r rm -rf
exit $GATE
