#!/bin/sh
# The Lab (Polaris 1.7b-c): build it and, where the game's exported data exists (the self-hosted runner), run the Lab's
# gate test scripts and compare each recorded trace in build/save/*.trace with its PCSX2 state log. Needs a host gcc
# with -m32.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
make -C "$ROOT/ps2/proto" lab || exit 1
FAIL=0
for t in "$ROOT"/build/save/det_walk_open.trace "$ROOT"/build/save/det_walk.trace "$ROOT"/build/save/det_burg2.trace; do  # (the gate traces: open ground, pots, a Burg walk with the world holding the player)
    [ -f "$t" ] && [ -f "$t.state" ] || continue
    python3 "$ROOT/tools/lab.py" compare "$t" "$t.state" --short || FAIL=$((FAIL + 1))
done
# test scripts in the Lab (Polaris 1.7c): the save round trip, and the gate walk against its console run
T="$ROOT/ps2/proto/tests"
python3 "$ROOT/tools/lab.py" run "$T/save_write.txt" "$T/save_check.txt" "$T/save_death.txt" \
    "$T/ev_door_save1.txt" "$T/ev_door_save2.txt" "$T/ev_door.txt" "$T/ev_warp.txt" "$T/ev_elevator.txt" \
    "$T/block_obj.txt" "$T/obj1.txt" "$T/world_route.txt" "$T/world_asylum.txt" || FAIL=$((FAIL + 1))
S="$ROOT/build/save/det_walk_open.trace.state"
if [ -f "$S" ]; then
    python3 "$ROOT/tools/lab.py" run "$ROOT/ps2/proto/tests/det_walk_open.txt" --state "$S" || FAIL=$((FAIL + 1))
fi
echo "lab: $FAIL failed"
exit $FAIL
