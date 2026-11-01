#!/bin/bash
# A3 save round trip in PCSX2 (headless, tools/test-run.py): boot 1 = save_write.txt (new game, set flags / souls /
# items, save), boot 2 = save_check.txt (values back from the save, second slot, reload). Then the file itself is
# checked offline with tools/save-dump.py (host: copy from build/save/, card copy via tools/ps2-mc.sh get).
#   ps2/proto/tests/save_roundtrip.sh [host|mc|both] [elf=build/proto.elf]
# mc: a freshly formatted card (tools/ps2-mc.sh new) in slot 1 of the fixed PINE slot's data dir, kept between boots.
set -u
cd "$(dirname "$0")/../../.."
MODE=${1:-both} ELF=${2:-build/proto.elf}
OUT=build/test/save-roundtrip; mkdir -p "$OUT"
SLOT=28150
HH=${DISTROBOX_HOST_HOME:-$HOME}
CARD=$HH/.var/app/net.pcsx2.PCSX2/test/$SLOT/PCSX2/memcards/Mcd001.ps2
fail=0
run() {  # $1 device line, $2 script
    sed "s/^option save .*/option save $1/" "ps2/proto/tests/$2.txt" > "$OUT/$2_${1%% *}.txt"
    tools/q -m 4 python3 tools/test-run.py "$ELF" "$OUT/$2_${1%% *}.txt" "$OUT/run_$2_${1%% *}" --slot $SLOT \
        > "$OUT/$2_${1%% *}.log" 2>&1 || { echo "FAIL $2 ($1)"; tail -5 "$OUT/$2_${1%% *}.log"; fail=1; return; }
    grep -h 'save:' "$OUT/run_$2_${1%% *}/emulog.txt" | sed 's/^/  /'
    echo "PASS $2 ($1)"
}
if [ "$MODE" != mc ]; then
    rm -f build/save/savetest0.bin build/save/savetest1.bin
    run "host savetest" save_write; run "host savetest" save_check
    for s in 0 1; do python3 tools/save-dump.py build/save/savetest$s.bin || fail=1; done
fi
if [ "$MODE" != host ]; then
    mkdir -p "$(dirname "$CARD")"; rm -f "$CARD"; tools/ps2-mc.sh new "$CARD" > /dev/null
    run mc save_write; run mc save_check
    rm -rf "$OUT/card"; tools/ps2-mc.sh get "$CARD" DSRPS2 "$OUT/card" > /dev/null && tools/ps2-mc.sh ls "$CARD" DSRPS2
    python3 tools/save-dump.py "$OUT/card/SAVE1.BIN" --expect souls=12345 flag=11010000 flag=60000001 flag=71010000 flag=50000100 \
        flag=999 || fail=1
    python3 tools/save-dump.py "$OUT/card/SAVE0.BIN" --expect souls=777 flag=11010000 || fail=1
fi
[ $fail = 0 ] && echo "save_roundtrip: PASS" || echo "save_roundtrip: FAIL"
exit $fail
