#!/bin/bash
# Offline host-side checks for PR CI (.github/workflows/pr-checks.yml, job "offline"); no game data, no toolchain:
#   1 every Python file under tools/ and ghidra/ compiles
#   2 tools/vu1/test_*.py: the VU1 microprograms on the VU1 emulator (tools/vu1/vuemu.py), needs numpy
#   3 the generated VU1 programs are up to date: tools/vu1/gen.py <kind> == ps2/proto/src/<file>.vsm (AGENTS.md 5:
#     generated files are never hand-edited; regenerate and commit both)
#   3b the export rules ps2/proto/export_{vars,rules}.mk are generated from tools/recipe.toml (tools/recipe.py make)
#   4 the game-data headers ps2/proto/src/{player,combat,progress}_data.h match tools/gamedata.py header <name>, and
#     tools/gamedata_test.py (blob layout = what the EE compiler lays out, checked against offsetof tables)
#   4b tools/material.py test (the classifier against the exporter's old per-mesh checks) and material_table.h up to date
#   5 tools/ci/test_residency.py: the per-block budgets of ps2/proto/src/residency.h fit the arena, and
#     tools/world_budget.py --strict fails a spot over the room (synthetic block, no game data)
#   5b tools/ci/test_audio_rate.py: the sound exporter's band loss and per-sample rate pick (synthetic signals)
#   6 libmapx against the Python code (tools/mapx/test_mapx.py); the map simplifier: tools/native/simplify.c (the source
#     of truth) against its frozen reference tools/simplify.py with its own rules off, and its golden outputs at the
#     shipped settings (tools/mapx/test_simplify_golden.py, simplify_golden.json)
#   7 tools/ci/test_scan.py: the readers raise souls.UnknownVariant on files they do not know, the coverage scan's
#     runtime tables parse, and tools/blockreport.py --strict fails a synthetic block over its VRAM row
#   8 tools/ci/test_buildprof.py: tools/buildprof.py (step table, critical chain, schedule replay, output identity) on
#     a synthetic tools/stepcache.py timing file
#   9 tools/ci/test_recipe_tools_audit.py: the report-only audit of tools/recipe.toml's per-step tools lists
#     (tools/ci/recipe_tools_audit.py) on a synthetic recipe
#   tools/ci/test_refusals.py: the TEST refusals: line and its gates (budget.py)
#   tools/ci/chunkver.py: every chunk_get / chunk_opt version in ps2/proto/src equals its writer's
#   10 tools/ci/test_budget.py: tools/budget.py reads ps2/proto/src/frame_budget.h and its --ratchet gate (fixture results)
#   11 tools/ci/test_lz4items.py: tools/lz4items.py write mode (mapres.lz.bin, cells.lz.bin, lzidx.bin) round trip and
#     layout rules on a synthetic block (tools/ci/fixtures/lz4items), tools/discalign.py check_pack reading CELI
#   12 tools/ci/test_mslz.sh: ps2/proto/src/mslz.c (lzidx.bin reader, in-place frame decoder, its refusals) built 64-bit on
#     the files tools/lz4items.py writes for the same synthetic block
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
for pair in clip:vu1clip map:vu1map2 mapi:vu1mapi mapspec:vu1mapspec skin:vu1skin2 skinclip:vu1skinclip fx:vu1fx; do
    kind=${pair%%:*}; vsm=$ROOT/ps2/proto/src/${pair#*:}.vsm
    "$PY" gen.py "$kind" > "$TMP/$kind.vsm" 2> "$TMP/$kind.err" || { tail -5 "$TMP/$kind.err"; fail "gen.py $kind"; continue; }
    if cmp -s "$TMP/$kind.vsm" "$vsm"; then echo "${vsm#$ROOT/}: up to date"
    else diff -u "$vsm" "$TMP/$kind.vsm" | head -20; fail "${vsm#$ROOT/} differs from gen.py $kind (regenerate: cd tools/vu1 && python3 gen.py $kind > ../../${vsm#$ROOT/})"; fi
done
# the VU1 memory contract (Polaris 1.5): its own checks, and the EE header generated from it
"$PY" layout.py > "$TMP/layout.txt" 2>&1 || { cat "$TMP/layout.txt"; fail "tools/vu1/layout.py check"; }
h=$ROOT/ps2/proto/src/vu1_layout.h
"$PY" gen.py layout > "$TMP/vu1_layout.h" 2> "$TMP/layout.err" || { tail -5 "$TMP/layout.err"; fail "gen.py layout"; }
if cmp -s "$TMP/vu1_layout.h" "$h"; then echo "${h#$ROOT/}: up to date"
else diff -u "$h" "$TMP/vu1_layout.h" | head -20; fail "${h#$ROOT/} differs (regenerate: cd tools/vu1 && python3 gen.py layout > ../../${h#$ROOT/})"; fi
echo "== export rules generated from the recipe"
"$PY" "$ROOT/tools/recipe.py" make --check && echo "ps2/proto/export_vars.mk, export_rules.mk: up to date" ||
    fail "ps2/proto/export_*.mk differ from tools/recipe.toml (regenerate: python3 tools/recipe.py make)"
echo "== generated game-data headers up to date"
cd "$ROOT" || exit 1
for k in player combat progress; do
    h=ps2/proto/src/${k}_data.h
    "$PY" -I tools/gamedata.py header "$k" > "$TMP/$k.h" 2> "$TMP/$k.err" || { tail -5 "$TMP/$k.err"; fail "gamedata.py header $k"; continue; }
    if cmp -s "$TMP/$k.h" "$h"; then echo "$h: up to date"
    else diff -u "$h" "$TMP/$k.h" | head -20; fail "$h differs (regenerate: python3 tools/gamedata.py header $k > $h)"; fi
done
"$PY" -I tools/gamedata_test.py || fail "tools/gamedata_test.py"
echo "== material classes (Polaris 2.1): tools/material.py self-test, ps2/proto/src/material_table.h up to date"
"$PY" -I tools/material.py test || fail "tools/material.py test"
h=ps2/proto/src/material_table.h
"$PY" -I tools/material.py header > "$TMP/material_table.h" || fail "material.py header"
if cmp -s "$TMP/material_table.h" "$h"; then echo "$h: up to date"
else diff -u "$h" "$TMP/material_table.h" | head -20; fail "$h differs (regenerate: python3 tools/material.py header > $h)"; fi
echo "== libmapx (the map exporter's C stages) against the Python code"
"$PY" -I tools/mapx/test_mapx.py 0 40 || fail "tools/mapx/test_mapx.py"
echo "== map simplifier: simplify.c against the frozen simplify.py, golden outputs at the shipped settings"
"$PY" -I tools/simplify_native.py --test 20 || fail "tools/simplify_native.py --test"
"$PY" -I tools/mapx/test_simplify_golden.py || fail "tools/mapx/test_simplify_golden.py (intended change: check it, then --update)"
echo "== residency budgets (ps2/proto/src/residency.h, tools/world_budget.py on a synthetic block)"
"$PY" -I tools/ci/test_residency.py || fail "tools/ci/test_residency.py"
"$PY" -I tools/ci/test_audio_rate.py || fail "tools/ci/test_audio_rate.py"
echo "== world.bin masks written (export_world.pack_world) and read back like world.c / wplan.c do, synthetic fixture"
"$PY" -I tools/ci/test_worldbin.py || fail "tools/ci/test_worldbin.py"
echo "== map.bin and its split files cut into sections (tools/mapwrap.py sections), synthetic DSM5 fixture"
"$PY" -I tools/ci/test_mapsect.py || fail "tools/ci/test_mapsect.py"
echo "== coverage scan reader errors and the block report gate (tools/scan, tools/blockreport.py)"
"$PY" -I tools/ci/test_scan.py || fail "tools/ci/test_scan.py"
echo "== build profiler (tools/buildprof.py on a synthetic stepcache.tsv)"
"$PY" -I tools/ci/test_buildprof.py || fail "tools/ci/test_buildprof.py"
echo "== recipe tools-list audit (report only) on a synthetic recipe"
"$PY" -I tools/ci/test_recipe_tools_audit.py || fail "tools/ci/test_recipe_tools_audit.py"
echo "== frame budget and its CI ratchet (tools/budget.py on fixture results)"
"$PY" -I tools/ci/test_budget.py || fail "tools/ci/test_budget.py"
"$PY" -I tools/ci/test_refusals.py || fail "tools/ci/test_refusals.py"
"$PY" -I tools/ci/chunkver.py || fail "tools/ci/chunkver.py"
echo "== LZ4 frames for streamed map items (tools/lz4items.py write mode on a synthetic block, discalign.check_pack on lzidx.bin)"
"$PY" -I tools/ci/test_lz4items.py || fail "tools/ci/test_lz4items.py"
echo "== streamed items' LZ4 reader and in-place decoder (ps2/proto/src/mslz.c, host build, synthetic block)"
sh tools/ci/test_mslz.sh || fail "tools/ci/test_mslz.sh"
echo "offline: $FAILED failed"
exit $FAILED
