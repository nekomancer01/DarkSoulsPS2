#!/bin/sh
# The Lab (Polaris 1.7b-c): build it and, where the game's exported data exists (the self-hosted runner), run the Lab's
# gate test scripts and compare each recorded trace in build/save/*.trace with its PCSX2 state log. Needs a host gcc
# with -m32.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
make -C "$ROOT/ps2/proto" lab || exit 1
FAIL=0
for t in "$ROOT"/build/save/det_walk_open*.trace; do  # (the gate traces; tests/det_walk.txt meets an object the Lab lacks)
    [ -f "$t" ] && [ -f "$t.state" ] || continue
    python3 "$ROOT/tools/lab.py" compare "$t" "$t.state" --short || FAIL=$((FAIL + 1))
done
# test scripts in the Lab (Polaris 1.7c): the save round trip, and the gate walk against its console run
python3 "$ROOT/tools/lab.py" run "$ROOT"/ps2/proto/tests/save_write.txt "$ROOT"/ps2/proto/tests/save_check.txt \
    || FAIL=$((FAIL + 1))
S="$ROOT/build/save/det_walk_open.trace.state"
if [ -f "$S" ]; then
    python3 "$ROOT/tools/lab.py" run "$ROOT/ps2/proto/tests/det_walk_open.txt" --state "$S" || FAIL=$((FAIL + 1))
fi
echo "lab: $FAIL failed"
exit $FAIL
