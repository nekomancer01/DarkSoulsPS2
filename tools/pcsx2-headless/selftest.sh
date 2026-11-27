#!/bin/bash
# Usage: tools/pcsx2-headless/selftest.sh <proto.sym.elf>   — scripted check of pcsx2-dsrtest's flags, control socket
# and exit codes (tools/q -m 12 tools/pcsx2-headless/selftest.sh build/proto.sym.elf). The ELF needs its build's data/
# next to it (host:data/). Binary: $PCSX2_HEADLESS, else tools/ext/pcsx2-build/bin/pcsx2-dsrtest of this tree or of the
# main checkout. Data dir /tmp/pcsx2-headless/selftest (PINE slot 28150), the ini from the user's PCSX2 config
# ($PCSX2_USERCFG, default the Flatpak's; BIOS folder) via tools/test-pcsx2-ini.py. Exit code = number of failed checks.
set -u
H=$(cd "$(dirname "$0")/../.." && pwd)
ELF=$(realpath "${1:?usage: selftest.sh <proto.sym.elf>}")
BIN=${PCSX2_HEADLESS:-$H/tools/ext/pcsx2-build/bin/pcsx2-dsrtest}
[ -x "$BIN" ] || BIN="$(git -C "$H" rev-parse --path-format=absolute --git-common-dir)/../tools/ext/pcsx2-build/bin/pcsx2-dsrtest"
[ -x "$BIN" ] || { echo "selftest: no pcsx2-dsrtest; run tools/pcsx2-headless/build.sh"; exit 1; }
SLOT=28150
D=/tmp/pcsx2-headless/selftest
OUT=$D/out
CTL=/tmp/pcsx2-headless/ctl.selftest.sock
LOG=$D/PCSX2/logs/emulog.txt
RUN=/run/user/$(id -u)/app/net.pcsx2.PCSX2
USERCFG=${PCSX2_USERCFG:-${DISTROBOX_HOST_HOME:-$HOME}/.var/app/net.pcsx2.PCSX2/config/PCSX2}
FAIL=0
ok() { echo "PASS $*"; }
bad() { echo "FAIL $*"; FAIL=$((FAIL + 1)); }
pactl_() { if command -v pactl >/dev/null; then pactl "$@"; else "$H/tools/hostexec" pactl "$@"; fi; }
ctl() { python3 -I "$H/tools/pcsx2ctl.py" --sock "$CTL" "$@"; }

rm -rf "$D"; mkdir -p "$OUT" "$RUN"
python3 -I "$H/tools/test-pcsx2-ini.py" "$USERCFG" "$D" "$SLOT" || { echo "selftest: ini failed"; exit 1; }
# a test script that idles: the checks below drive the instance
mkdir -p "$(dirname "$ELF")/tests/gen"
printf 'option nodemo\nwait 600\nend\n' > "$(dirname "$ELF")/tests/gen/dsrtest-selftest.txt"

# 1. exit codes without a VM: bad flag -> 1, -timeout 5 -> 3 within 7 s
"$BIN" -nosuchflag > "$D/badflag.log" 2>&1; rc=$?
[ $rc = 1 ] && ok "bad flag exits 1" || bad "bad flag exits $rc (want 1)"

# 2. boot with -datapath, PINE, ctl, file log
rm -f "$RUN/pcsx2.sock.$SLOT"
t0=$(date +%s)
XDG_RUNTIME_DIR=$RUN "$BIN" -datapath "$D" -ctl "$CTL" -logfile "$LOG" -timeout 300 -fastboot -elf "$ELF" \
    -gameargs "test=tests/gen/dsrtest-selftest.txt" > "$D/stdout.log" 2>&1 < /dev/null &
PID=$!
for _ in $(seq 600); do grep -q 'TEST ' "$LOG" 2>/dev/null && break; kill -0 $PID 2>/dev/null || break; sleep 0.1; done
grep -q 'TEST ' "$LOG" 2>/dev/null && ok "boot: first TEST line after $(( $(date +%s) - t0 )) s" || bad "boot: no TEST line in $LOG"
grep -q 'OsdShowFPS = false' "$LOG" 2>/dev/null && ok "settings dump has OsdShowFPS = false" \
    || bad "no 'OsdShowFPS = false' in the log"
sleep 3
python3 -I "$H/tools/pine.py" --slot $SLOT --elf "$ELF" mailbox > "$D/mailbox.txt" 2>&1 && ok "PINE mailbox" || bad "PINE mailbox: $(tail -1 "$D/mailbox.txt")"
SINKS=$(pactl_ list short sink-inputs 2>/dev/null | wc -l)
pactl_ list sink-inputs 2>/dev/null | grep -q "application.process.id = \"$PID\"" && bad "a sink input of pid $PID" \
    || ok "no sound stream of pid $PID ($SINKS sink inputs on the host, other apps')"
[ -f "$D/PCSX2/memcards/Mcd001.ps2" ] && ok "memory card Mcd001.ps2" || bad "no $D/PCSX2/memcards/Mcd001.ps2"
grep -qE 'Creating (Cubeb|SDL) audio stream' "$LOG" && bad "an audio device stream was opened: $(grep -m1 -E 'Creating (Cubeb|SDL)' "$LOG")" \
    || { grep -q 'Creating Null audio stream' "$LOG" && ok "audio backend Null"; } || bad "no 'Creating Null audio stream' in the log"

# 3. control socket: two shots one frame apart (no OSD: the top-left 200x60 px match), gsdump, save
ctl shot "$OUT/a.png" > /dev/null && ctl shot "$OUT/b.png" > /dev/null && [ -s "$OUT/a.png" ] && [ -s "$OUT/b.png" ] \
    && ok "shot a.png b.png" || bad "shot"
if [ -s "$OUT/a.png" ] && [ -s "$OUT/b.png" ]; then
    python3 -I -c "
import sys
from PIL import Image, ImageChops
a, b = (Image.open(p).convert('RGB').crop((0, 0, 200, 60)) for p in sys.argv[1:3])
sys.exit(0 if ImageChops.difference(a, b).getbbox() is None else 1)" "$OUT/a.png" "$OUT/b.png" 2>/dev/null; r=$?
    PY=$(sed -n 's/^PY *= *//p' "$H/config.local.mk" 2>/dev/null | tail -1)
    [ $r = 0 ] || { [ -n "$PY" ] && "$PY" -I -c "
import sys
from PIL import Image, ImageChops
a, b = (Image.open(p).convert('RGB').crop((0, 0, 200, 60)) for p in sys.argv[1:3])
sys.exit(0 if ImageChops.difference(a, b).getbbox() is None else 1)" "$OUT/a.png" "$OUT/b.png"; r=$?; }
    [ $r = 0 ] && ok "no OSD: top-left 200x60 identical in two shots" || bad "top-left 200x60 differs between shots (OSD?)"
fi
ctl gsdump "$OUT/d.gs.zst" > /dev/null && [ -s "$OUT/d.gs.zst" ] && ok "gsdump $(stat -c %s "$OUT/d.gs.zst") bytes" || bad "gsdump"
ctl save "$OUT/s.p2s" > /dev/null && [ -s "$OUT/s.p2s" ] && ok "save $(stat -c %s "$OUT/s.p2s") bytes" || bad "save"

# 4. quit -> exit 0
ctl quit > /dev/null || bad "quit request"
for _ in $(seq 100); do kill -0 $PID 2>/dev/null || break; sleep 0.1; done
if kill -0 $PID 2>/dev/null; then bad "still running 10 s after quit"; kill -9 $PID; wait $PID; else wait $PID; rc=$?; [ $rc = 0 ] && ok "quit exits 0" || bad "quit exits $rc"; fi
python3 -I "$H/tools/pcsx2ctl.py" --sock "$CTL" shot "$OUT/x.png" 2>/dev/null && bad "dead socket answered" || ok "dead socket: pcsx2ctl exits 1"

# 5. -statefile reload of that save
if [ -s "$OUT/s.p2s" ]; then
    : > "$D/reload.log"
    XDG_RUNTIME_DIR=$RUN "$BIN" -datapath "$D" -ctl "$CTL" -logfile "$D/reload.log" -timeout 120 -fastboot -elf "$ELF" \
        -statefile "$OUT/s.p2s" -gameargs "test=tests/gen/dsrtest-selftest.txt" > "$D/stdout2.log" 2>&1 < /dev/null &
    PID=$!
    for _ in $(seq 300); do [ -S "$CTL" ] && ctl shot "$OUT/r.png" > /dev/null 2>&1 && break; kill -0 $PID 2>/dev/null || break; sleep 0.2; done
    grep -q '(dsrtest) state loaded: .* (ok)' "$D/reload.log" && [ -s "$OUT/r.png" ] \
        && ok "-statefile reload (shot r.png)" || bad "-statefile reload"
    ctl quit > /dev/null 2>&1; for _ in $(seq 100); do kill -0 $PID 2>/dev/null || break; sleep 0.1; done
    kill -0 $PID 2>/dev/null && { kill -9 $PID; bad "reload instance did not quit"; }
    wait $PID 2>/dev/null
fi

# 6. -timeout 5 -> exit 3 within 7 s
t0=$(date +%s.%N)
XDG_RUNTIME_DIR=$RUN "$BIN" -datapath "$D" -logfile "$D/timeout.log" -timeout 5 -fastboot -elf "$ELF" \
    -gameargs "test=tests/gen/dsrtest-selftest.txt" > /dev/null 2>&1 < /dev/null; rc=$?
dt=$(echo "$(date +%s.%N) - $t0" | bc)
[ $rc = 3 ] && [ "$(echo "$dt < 7" | bc)" = 1 ] && ok "-timeout 5 exits 3 after ${dt%.*} s" || bad "-timeout 5: exit $rc after $dt s"

pgrep -x pcsx2-dsrtest > /dev/null && pgrep -af pcsx2-dsrtest | grep -q "$D" && bad "an instance of this selftest is left"
echo "selftest: $FAIL failed"
exit $FAIL
