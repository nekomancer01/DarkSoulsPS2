#!/bin/bash
# Offline host-side checks for PR CI (.github/workflows/pr-checks.yml, job "offline"); no game data, no toolchain:
#   1 every Python file under tools/ and ghidra/ compiles
#   2 tools/vu1/test_*.py: the VU1 microprograms on the VU1 emulator (tools/vu1/vuemu.py), needs numpy
#   3 the generated VU1 programs are up to date: tools/vu1/gen.py <kind> == ps2/proto/src/<file>.vsm (AGENTS.md 5:
#     generated files are never hand-edited; regenerate and commit both)
#   4 the game-data headers ps2/proto/src/{player,combat,progress}_data.h match tools/gamedata.py header <name>, and
#     tools/gamedata_test.py (blob layout = what the EE compiler lays out, checked against offsetof tables)
# Usage: tools/ci/offline.sh   Exit code = number of failed steps.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PY=${PY:-python3}
FAILED=0
fail() { echo "FAIL: $*"; FAILED=$((FAILED + 1)); }

echo "== compile Python"
"$PY" -m compileall -q tools ghidra > /dev/null || fail "Python syntax errors (run: $PY -m compileall tools ghidra)"

echo "== VU1 emulator tests"
cd "$ROOT/tools/vu1" || exit 1
for t in test_*.py; do
    out=$("$PY" "$t" 2>&1); rc=$?
    echo "$t: $(echo "$out" | tail -1)"
    [ $rc = 0 ] || { echo "$out" | tail -20; fail "$t exit $rc"; }
done

echo "== generated VU1 programs up to date"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
for pair in clip:vu1clip map:vu1map2 skin:vu1skin2 skinclip:vu1skinclip fx:vu1fx; do
    kind=${pair%%:*}; vsm=$ROOT/ps2/proto/src/${pair#*:}.vsm
    "$PY" gen.py "$kind" > "$TMP/$kind.vsm" 2> "$TMP/$kind.err" || { tail -5 "$TMP/$kind.err"; fail "gen.py $kind"; continue; }
    if cmp -s "$TMP/$kind.vsm" "$vsm"; then echo "${vsm#$ROOT/}: up to date"
    else diff -u "$vsm" "$TMP/$kind.vsm" | head -20; fail "${vsm#$ROOT/} differs from gen.py $kind (regenerate: cd tools/vu1 && python3 gen.py $kind > ../../${vsm#$ROOT/})"; fi
done
echo "== generated game-data headers up to date"
cd "$ROOT" || exit 1
for k in player combat progress; do
    h=ps2/proto/src/${k}_data.h
    "$PY" -I tools/gamedata.py header "$k" > "$TMP/$k.h" 2> "$TMP/$k.err" || { tail -5 "$TMP/$k.err"; fail "gamedata.py header $k"; continue; }
    if cmp -s "$TMP/$k.h" "$h"; then echo "$h: up to date"
    else diff -u "$h" "$TMP/$k.h" | head -20; fail "$h differs (regenerate: python3 tools/gamedata.py header $k > $h)"; fi
done
"$PY" -I tools/gamedata_test.py || fail "tools/gamedata_test.py"
echo "== libmapx (the map exporter's C stages) against the Python code"
"$PY" -I tools/mapx/test_mapx.py 0 40 || fail "tools/mapx/test_mapx.py"
echo "offline: $FAILED failed"
exit $FAILED
