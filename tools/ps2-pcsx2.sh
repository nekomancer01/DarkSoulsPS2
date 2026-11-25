#!/bin/bash
# PCSX2 runs for the real-hardware / debugger flow. Uses its own PCSX2 config ($DSR_PCSX2_CFG/<hostfs|nohostfs>/PCSX2,
# default ~/tools/pcsx2-cfg: copy of the user's ini + BIOS) via -datapath, so HostFs can be off, logs / savestates
# stay apart from the user's, and the user's PCSX2 settings are never changed.
#
#   ps2-pcsx2.sh run   <elf|iso> [secs=10] [shot.png]  headless in pcsx2-dsrtest (tools/pcsx2-headless/: no display,
#                                                       no sound device; shot via its control socket, tools/pcsx2ctl.py),
#                                                       EE/IOP console in <config>/PCSX2/logs/run.log; prints the
#                                                       DSRPROTO / pack lines
#   ps2-pcsx2.sh state <elf|iso> <state.p2s> [secs] [shot]  same, starting from a savestate (-statefile)
#   ps2-pcsx2.sh debug <elf|iso>                        visible window with the debugger open (-debugger); symbols
#                                                       come from the booted ELF's .symtab (use proto.sym.elf, or
#                                                       make iso ISO_SYMS=1 for a disc); with [secs shot] it runs
#                                                       headless instead and screenshots its virtual screen
#   ps2-pcsx2.sh syms  <elf> [out.sym]                  "addr name" text symbol file from .symtab (PCSX2 debugger:
#                                                       Debug > Analyse > extra symbol files; addr2line-style lookups)
# Env: HOSTFS=0/1 (default: 1 for an ELF, 0 for an ISO, i.e. an ISO boot cannot fall back to host:), STATESAVE=<secs>
# (run: save state slot 1 over PINE (tools/pine.py) after that many seconds, into the config's PCSX2/sstates/),
# KEEP=1 (leave the headless instance running; stop: tools/pcsx2ctl.py --sock /tmp/pcsx2-headless/ctl.<slot>.sock quit),
# RW=1 (debug: the ELF folder writable for the Flatpak: host:gmon.out; the headless runner has no sandbox), DRY=1 (print
# the command instead of running it), PCSX2_HEADLESS / PCSX2_TIMEOUT as in tools/pcsx2-run.sh. debug and syms keep the
# Flatpak (the GUI debugger).
set -e
H=$(cd "$(dirname "$0")/.." && pwd)
HX=$H/tools/hostexec  # runs on the host (distrobox-host-exec inside a distrobox)
USERCFG=${PCSX2_USERCFG:-${DISTROBOX_HOST_HOME:-$HOME}/.var/app/net.pcsx2.PCSX2/config/PCSX2}
NM=${NM:-mips64r5900el-ps2-elf-nm}
MODE=$1; shift || true
case "$MODE" in run|state|debug|syms) ;; *) sed -n 2,22p "$0"; exit 1;; esac
if [ "$MODE" = syms ]; then
    ELF=$(realpath "$1"); OUT=$(realpath -m "${2:-${ELF%.elf}.sym}")
    "$NM" -n --defined-only "$ELF" | awk 'tolower($2) ~ /^[tdbr]$/ {print $1, $3}' > "$OUT"
    echo "symbols: $(wc -l < "$OUT") -> $OUT"; exit 0
fi

IMG=$(realpath "$1"); shift
[ -f "$IMG" ] || { echo "no such file: $IMG" >&2; exit 1; }
case "$IMG" in *.iso) ISO=1; BOOT=(-- "$IMG");; *) ISO=0; BOOT=(-elf "$IMG");; esac
HOSTFS=${HOSTFS:-$((1 - ISO))}
STATE=; [ "$MODE" = state ] && { STATE=$(realpath "$1"); shift; }
T=${1:-10}; SHOT=$2

# Config: the user's ini with HostFs set, console logging on, no confirmation dialogs, sound muted; BIOS copied once.
CFGROOT=${DSR_PCSX2_CFG:-$HOME/tools/pcsx2-cfg}/$([ "$HOSTFS" = 1 ] && echo hostfs || echo nohostfs)
C=$CFGROOT/PCSX2
mkdir -p "$C/inis" "$C/bios" "$C/logs" "$C/sstates" "$C/memcards"
[ -n "$(ls -A "$C/bios")" ] || cp "$USERCFG"/bios/* "$C/bios/"
PINESLOT=$((28190 + 1 - HOSTFS))  # own PINE slots (28190 hostfs / 28191 nohostfs), apart from the test scripts'
python3 -I - "$USERCFG/inis/PCSX2.ini" "$C/inis/PCSX2.ini" "$HOSTFS" "$PINESLOT" <<'EOF'
import sys, re
src, dst, hostfs = sys.argv[1], sys.argv[2], sys.argv[3]
want = {'EmuCore': {'HostFs': 'true' if hostfs == '1' else 'false', 'EnableFastBoot': 'true', 'EnablePINE': 'true',
                     'PINESlot': sys.argv[4]},
        'Logging': {'EnableFileLogging': 'true', 'EnableEEConsole': 'true', 'EnableIOPConsole': 'true'},
        'UI': {'ConfirmShutdown': 'false', 'SetupWizardIncomplete': 'false', 'StartFullscreen': 'false'},
        'MemoryCards': {'Slot1_Filename': 'Mcd001.ps2', 'Slot2_Enable': 'false'},
        'SPU2/Output': {'OutputMuted': 'true'}}  # silent even if the stream misses the null sink dsr_mute
out, sec, seen = [], None, set()
def flush(sec):
    for k, v in want.get(sec, {}).items():
        if (sec, k) not in seen: out.append(f'{k} = {v}')
for line in open(src).read().splitlines():
    m = re.match(r'\[(.*)\]$', line)
    if m:
        flush(sec); sec = m.group(1); out.append(line); continue
    m = re.match(r'(\w+) = ', line)
    if m and m.group(1) in want.get(sec, {}):
        seen.add((sec, m.group(1))); line = f'{m.group(1)} = {want[sec][m.group(1)]}'
    out.append(line)
flush(sec)
for s in want:
    if not any(l == f'[{s}]' for l in out):
        out.append(f'[{s}]'); sec = s; flush(s)
open(dst, 'w').write('\n'.join(out) + '\n')
EOF
LOG=$H/tools/ps2-pcsx2.log
FS=(--filesystem="$(dirname "$IMG")"$([ -n "$RW" ] || echo :ro) --filesystem="$CFGROOT")
ENV=(--env=XDG_RUNTIME_DIR=/run/user/$(id -u)/app/net.pcsx2.PCSX2 --env=PULSE_SINK=dsr_mute)  # muted like pcsx2-run.sh
ARGS=(-datapath "$CFGROOT" -fastboot -logfile "$C/logs/run.log")
[ -n "$STATE" ] && { FS+=(--filesystem="$(dirname "$STATE")":ro); ARGS+=(-statefile "$STATE"); }

if [ "$MODE" = debug ] && [ -z "$SHOT" ]; then
    CMD="setsid flatpak run $(printf "%q " "${FS[@]}" "${ENV[@]}") net.pcsx2.PCSX2 $(printf "%q " "${ARGS[@]}") -earlyconsolelog -debugger $(printf "%q " "${BOOT[@]}")"
    [ -n "$DRY" ] && { echo "$CMD"; exit 0; }
    "$HX" bash -c "$CMD > '$LOG' 2>&1 < /dev/null &"
    echo "PCSX2 debugger window started (config $C, HostFs $HOSTFS); console log $LOG"; exit 0
fi

case "$MODE" in
debug)  # the debugger needs the Flatpak's Qt GUI: headless on Xvfb :99 with a shot of the screen
    ARGS+=(-earlyconsolelog -debugger)
    D=:99
    MUTE="pactl list short sinks | grep -q dsr_mute || pactl load-module module-null-sink sink_name=dsr_mute >/dev/null;"
    CMD="$MUTE DISPLAY=$D XAUTHORITY= WAYLAND_DISPLAY= setsid flatpak run --instance-id-fd=3 --socket=x11 --nosocket=wayland \
    --env=QT_QPA_PLATFORM=xcb $(printf "%q " "${ENV[@]}" "${FS[@]}") net.pcsx2.PCSX2 $(printf "%q " "${ARGS[@]}" "${BOOT[@]}")"
    [ -n "$DRY" ] && { echo "$CMD"; exit 0; }
    if ! DISPLAY=$D xdpyinfo >/dev/null 2>&1; then
        # 9>&-: never hand the job-queue lock (tools/q holds it on fd 9) to the long-lived Xvfb
        setsid Xvfb $D -screen 0 1280x960x24 -nolisten tcp > /tmp/xvfb99.log 2>&1 < /dev/null 9>&- &
        for _ in $(seq 50); do DISPLAY=$D xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
    fi
    mkdir -p /tmp/pcsx2-headless
    ID=/tmp/pcsx2-headless/id.$$
    : > "$C/logs/run.log"
    "$HX" bash -c "$CMD > '$LOG' 2>&1 < /dev/null 3> '$ID' &
        for _ in \$(seq 50); do [ -s '$ID' ] && break; sleep 0.1; done"
    INST=$(cat "$ID"); rm -f "$ID"; touch "/tmp/pcsx2-headless/$INST"   # pcsx2-run.sh's user launch leaves it alone
    if [ -n "$STATESAVE" ]; then
        sleep "$STATESAVE"
        python3 -I "$H/tools/pine.py" --slot "$PINESLOT" save 1 && sleep 2 && ls -t "$C"/sstates/*.p2s 2>/dev/null | head -1
        T=$((T > STATESAVE ? T - STATESAVE : 2))
    fi
    sleep "$T"
    if [ -n "$SHOT" ]; then
        DISPLAY=$D import -window root "$(realpath -m "$SHOT")"
    fi
    [ -n "$KEEP" ] || { "$HX" flatpak kill "$INST" 2>/dev/null || true; rm -f "/tmp/pcsx2-headless/$INST"; }
    ;;
*)  # run / state: the headless test runner, a child of this script
    if [ -n "$PCSX2_HEADLESS" ]; then  # an explicit binary is used or the run fails, as in tools/pcsx2-run.sh
        BIN=$PCSX2_HEADLESS
        [ -x "$BIN" ] || [ -n "$DRY" ] || { echo "ps2-pcsx2.sh: PCSX2_HEADLESS=$BIN is not executable" >&2; exit 1; }
    else
        BIN=$H/tools/ext/pcsx2-build/bin/pcsx2-dsrtest
        [ -x "$BIN" ] || BIN="$(git -C "$H" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)/../tools/ext/pcsx2-build/bin/pcsx2-dsrtest"
        [ -x "$BIN" ] || [ -n "$DRY" ] || { echo "ps2-pcsx2.sh: no pcsx2-dsrtest; run tools/pcsx2-headless/build.sh" >&2; exit 1; }
    fi
    CTL=/tmp/pcsx2-headless/ctl.$PINESLOT.sock
    RUN=/run/user/$(id -u)/app/net.pcsx2.PCSX2  # PINE socket dir (tools/pine.py)
    CMD=(env XDG_RUNTIME_DIR="$RUN" "$BIN" "${ARGS[@]}" -ctl "$CTL" -timeout "${PCSX2_TIMEOUT:-3600}" "${BOOT[@]}")
    [ -n "$DRY" ] && { printf "%q " "${CMD[@]}"; echo; exit 0; }
    mkdir -p /tmp/pcsx2-headless "$RUN"
    # The PINE slot's lock, the one tools/test-run.py acquire_slot takes (28190/28191 lie in its 28110-28199 range):
    # held on fd 8, which the emulator inherits, so a test-run never takes this slot, and never reaps this runner
    # through its slot.<n>.pid note, while it is alive (KEEP=1 included).
    exec 8> "/tmp/pcsx2-headless/slot.$PINESLOT.lock"
    flock -w 600 8 || { echo "ps2-pcsx2.sh: PINE slot $PINESLOT busy for 600 s" >&2; exit 1; }
    : > "$C/logs/run.log"
    rm -f "$CTL"
    setsid "${CMD[@]}" > "$LOG" 2>&1 < /dev/null 9>&- &
    PID=$!
    echo "$PID $(awk '{print $22}' "/proc/$PID/stat" 2>/dev/null)" > "/tmp/pcsx2-headless/slot.$PINESLOT.pid"
    for _ in $(seq 300); do [ -S "$CTL" ] && break; kill -0 "$PID" 2>/dev/null || break; sleep 0.05; done
    if [ -n "$STATESAVE" ]; then
        sleep "$STATESAVE"
        python3 -I "$H/tools/pine.py" --slot "$PINESLOT" save 1 && sleep 2 && ls -t "$C"/sstates/*.p2s 2>/dev/null | head -1
        T=$((T > STATESAVE ? T - STATESAVE : 2))
    fi
    sleep "$T"
    [ -n "$SHOT" ] && python3 -I "$H/tools/pcsx2ctl.py" --sock "$CTL" shot "$(realpath -m "$SHOT")" > /dev/null || true
    if [ -n "$KEEP" ]; then echo "PID=$PID CTL=$CTL"
    else
        python3 -I "$H/tools/pcsx2ctl.py" --sock "$CTL" quit > /dev/null 2>&1 || true
        for _ in $(seq 100); do kill -0 "$PID" 2>/dev/null || break; sleep 0.1; done
        kill -9 "$PID" 2>/dev/null || true
        rm -f "/tmp/pcsx2-headless/slot.$PINESLOT.pid"
    fi
    ;;
esac
echo "config $C (HostFs $HOSTFS), boot ${BOOT[*]}"
grep -a -E "DSRPROTO|pack:|PROF|gprof|OUT OF RAM|ERROR|Detected PS2 Disc|ELF Loading" "$C/logs/run.log" 2>/dev/null | head -80
