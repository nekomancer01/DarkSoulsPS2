#!/bin/sh
# The Lab (Polaris 1.7b): build it and, where the game's exported data and recorded traces exist (the self-hosted
# runner), compare each trace in build/save/*.trace with its PCSX2 state log. Needs a host gcc with -m32.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
make -C "$ROOT/ps2/proto" lab || exit 1
FAIL=0
for t in "$ROOT"/build/save/det_walk_open*.trace; do  # (the gate traces; tests/det_walk.txt meets an object the Lab lacks)
    [ -f "$t" ] && [ -f "$t.state" ] || continue
    python3 "$ROOT/tools/lab.py" compare "$t" "$t.state" --short || FAIL=$((FAIL + 1))
done
echo "lab: $FAIL failed"
exit $FAIL
