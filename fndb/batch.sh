#!/bin/bash
# Usage: batch.sh export                 once, after Ghidra analysis has finished: metadata → DB → tiers
#        batch.sh decompile [TIER]        decompile only (tiers <= TIER, default 2), no labeling
#        batch.sh [TIER] [label args...]  decompile every function in tiers <= TIER (default 2) not yet decompiled,
#                                         then label them: whole ctx bucket 4096 (x4), then 8192 (x2), then 16384 (x1)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# Python with fastembed + sqlite-vec for the vector step: FNDB_PY, else .venv-fndb in the repo root, else python3
FNDB_PY=${FNDB_PY:-$([ -x "$HERE/../.venv-fndb/bin/python" ] && echo "$HERE/../.venv-fndb/bin/python" || echo python3)}
G=$HERE/../ghidra
WORK=$HERE/work
mkdir -p "$WORK"
export JAVA_HOME=$HOME/tools/jdk-21.0.13+9 GHIDRA_HEADLESS_MAXMEM=8G
export JAVA_TOOL_OPTIONS="-XX:-UseContainerSupport -Xlog:os+container=off"
headless() {
	"$HOME/tools/ghidra_12.1.4_PUBLIC/support/analyzeHeadless" "${DSR_GHIDRA_PROJECT:-${DISTROBOX_HOST_HOME:-$HOME}/Projects/dsr-ghidra}" dsr \
		-process DarkSoulsRemastered.exe -noanalysis -readOnly -scriptPath "$G/scripts" "$@" \
		-log "$G/logs/batch.log" -scriptlog "$G/logs/batch-script.log"
}

if [ "${1:-}" = export ]; then
	headless -postScript ExportFunctions.java "$WORK/meta.jsonl"
	python3 -I "$HERE/fndb.py" import-meta "$WORK/meta.jsonl"
	python3 -I "$HERE/fndb.py" prioritize
	python3 -I "$HERE/fndb.py" status
	exit
fi

DECOMP_ONLY=
if [ "${1:-}" = decompile ]; then DECOMP_ONLY=1; shift; fi
TIER=${1:-2}
shift || true
# Resumable: anything an interrupted run decompiled is imported first, so select only lists what is still missing.
# Labeling commits each function as it finishes; rerunning this script continues both stages.
if [ -s "$WORK/decomp.jsonl" ]; then
	python3 -I "$HERE/fndb.py" import-decomp "$WORK/decomp.jsonl"
	rm "$WORK/decomp.jsonl"
fi
python3 -I "$HERE/fndb.py" select --tier "$TIER" > "$WORK/list.txt"
if [ -s "$WORK/list.txt" ]; then
	echo "decompiling $(wc -l < "$WORK/list.txt") functions"
	headless -postScript DecompileList.java "$WORK/list.txt" "$WORK/decomp.jsonl" 60 "${DECOMP_THREADS:-$(nproc)}"
	python3 -I "$HERE/fndb.py" import-decomp "$WORK/decomp.jsonl"
	rm "$WORK/decomp.jsonl"
fi
python3 -I "$HERE/fndb.py" status
[ -n "$DECOMP_ONLY" ] && exit
python3 -I "$HERE/fndb.py" label --tier "$TIER" "$@"
"$FNDB_PY" -I "$HERE/fndb.py" embed  # vectors for `fndb.py search`
python3 -I "$HERE/fndb.py" status
