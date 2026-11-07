#!/bin/bash
# Usage: ./play-report.sh [ISO] [-o FILE] [--every SECS] [--width PX] [--no-states] [--for SECS] [--no-annotate]
#
# Play the built disc in PCSX2 and get one file to send with a bug report. Starts PCSX2 with the ISO (default
# build/proto.iso, what ./build-iso.sh makes), records the emulator's log with the game's EE log in it, takes a
# screenshot of the PCSX2 window every 2 seconds, and when PCSX2 is closed (or Ctrl+C here) packs it all into one zip
# in build/play-reports/ (printed at the end).
#
# While playing, type a note in this terminal and press Enter to mark the moment ("fell through the floor"): the note
# goes into marks.txt with the time, and a savestate of that moment goes into the report (the last 2 marks).
#
#   ISO              the disc image (default build/proto.iso)
#   -o, --out FILE   the zip (default build/play-reports/report-<date>-<time>.zip)
#   --every SECS     seconds between screenshots (default 2; 0 = none)
#   --width PX       screenshot width (default 480, JPEG; the report grows ~30 MB an hour at the default)
#   --no-states      no savestates at marks
#   --for SECS       quit PCSX2 by itself after that many seconds (scripted checks; implies --mute)
#   --mute           no sound (a null PulseAudio sink dsr_mute, and PCSX2's output muted)
#   --no-annotate    do not open the report in ./annotate-report.sh when done (it opens after a manual run: step
#                    through the shots, pin comments on them; they are saved into the zip, Ctrl+C there when done)
#
# What the zip holds:
#   summary.txt   the run at a glance: hitch summary (tools/hitchsum.py), spikes, failed allocations, deaths, blocks
#                 loaded, streaming problems, marks
#   emulog.txt    PCSX2's whole log with the game's EE log ([seconds since start] prefix on every line)
#   shots/        t<seconds>.jpg, the same clock as emulog.txt's prefix
#   marks.txt     your notes, states/ the savestates at the last marks
#   info.txt      ISO, checkout, PCSX2 version, settings (pcsx2.ini without its key bindings)
#   annotations/  added by ./annotate-report.sh: notes.md, notes.json, the annotated shots with their pins drawn
#
# PCSX2 runs with your own settings, BIOS and memory cards (saves count as usual) from a separate data folder next to
# your PCSX2 config (play-report/PCSX2), so console logging can be on without changing your PCSX2.ini; savestates made
# there stay there. Needs PCSX2 (the flatpak net.pcsx2.PCSX2, or pcsx2-qt / $PCSX2 on PATH), python3, and for the
# screenshots ImageMagick (import) + xprop (or xwininfo). On Wayland PCSX2 runs through XWayland so its window can be captured.
# A savestate holds the game's memory: like the ISO, it is made from your copy of the game.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# PCSX2, the display and the screenshot tools live on the host: from inside a distrobox, run this script there
if [ -z "${PLAY_ON_HOST:-}" ] && { [ -n "${CONTAINER_ID:-}" ] || [ -e /run/.containerenv ]; } &&
    command -v distrobox-host-exec >/dev/null 2>&1; then
    exec distrobox-host-exec env PLAY_ON_HOST=1 bash "$ROOT/play-report.sh" "$@"
fi

ISO=; OUT=; EVERY=2; WIDTH=480; STATES=1; FOR=; MUTE=; ANNOTATE=1
die() { echo "play-report: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case "$1" in
        -o|--out) [ $# -ge 2 ] || die "$1 needs a file"; OUT=$2; shift 2 ;;
        --every) [ $# -ge 2 ] || die "$1 needs seconds"; EVERY=$2; shift 2 ;;
        --width) [ $# -ge 2 ] || die "$1 needs pixels"; WIDTH=$2; shift 2 ;;
        --no-states) STATES=; shift ;;
        --mute) MUTE=1; shift ;;
        --no-annotate) ANNOTATE=; shift ;;
        --for) [ $# -ge 2 ] || die "$1 needs seconds"; FOR=$2; shift 2 ;;
        -h|--help) awk 'NR > 1 && !/^#/ {exit} NR > 1' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) die "unknown option $1 (see --help)" ;;
        *) [ -z "$ISO" ] || die "one ISO only"; ISO=$1; shift ;;
    esac
done
[[ $EVERY =~ ^[0-9]+$ ]] || die "--every takes whole seconds"
[[ $WIDTH =~ ^[1-9][0-9]*$ ]] || die "--width takes pixels"
[ -z "$FOR" ] || [[ $FOR =~ ^[1-9][0-9]*$ ]] || die "--for takes whole seconds"
[ -z "$FOR" ] || MUTE=1  # scripted runs are silent (manual play keeps its sound)
ISO=$(realpath -e -- "${ISO:-$ROOT/build/proto.iso}" 2>/dev/null) || die "no ISO at ${ISO:-$ROOT/build/proto.iso} (build it with ./build-iso.sh)"
command -v python3 >/dev/null || die "needs python3"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=$(realpath -m -- "${OUT:-$ROOT/build/play-reports/report-$STAMP.zip}")
WORK=$(dirname "$OUT")/.$(basename "$OUT" .zip)
rm -rf "$WORK"; mkdir -p "$WORK/shots" "$WORK/states"

# --- PCSX2: the flatpak, else a native build ---
PINE_SLOT=28199  # own PINE slot (the test tools use 28011, 28190-28191 and up from 28200)
if [ -n "${PCSX2:-}" ] || ! { command -v flatpak >/dev/null && flatpak info net.pcsx2.PCSX2 >/dev/null 2>&1; }; then
    BIN=${PCSX2:-$(command -v pcsx2-qt || command -v pcsx2 || true)}
    [ -n "$BIN" ] || die "PCSX2 not found: install the flatpak net.pcsx2.PCSX2, or set PCSX2=/path/to/pcsx2-qt"
    USERCFG=${PCSX2_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/PCSX2}
    DATAPATH=$(dirname "$USERCFG")/PCSX2-play-report
    RUN=(env); SOCK=${XDG_RUNTIME_DIR:-/tmp}/pcsx2.sock.$PINE_SLOT
    VERSION=$("$BIN" -version 2>/dev/null | head -1 || true)
else
    BIN=flatpak
    USERCFG=${PCSX2_CONFIG:-$HOME/.var/app/net.pcsx2.PCSX2/config/PCSX2}
    DATAPATH=$HOME/.var/app/net.pcsx2.PCSX2/play-report
    RT=/run/user/$(id -u)/app/net.pcsx2.PCSX2
    RUN=(flatpak run --instance-id-fd=3 --filesystem="$(dirname "$ISO")":ro --env=XDG_RUNTIME_DIR="$RT")
    SOCK=$RT/pcsx2.sock.$PINE_SLOT
    VERSION="flatpak net.pcsx2.PCSX2 $(flatpak info net.pcsx2.PCSX2 | awk '/Version:/ {print $2}')"
fi
[ -f "$USERCFG/inis/PCSX2.ini" ] || die "no PCSX2 settings at $USERCFG (start PCSX2 once and finish its setup, or set PCSX2_CONFIG)"

PLAYCFG=$DATAPATH/PCSX2  # (-datapath D: settings in D/PCSX2)
# Separate data folder: everything linked to the user's own (BIOS, memory cards, game settings, input profiles, ...)
# except inis/ (a copy with logging on), logs/ and sstates/ (this tool's)
mkdir -p "$PLAYCFG/inis" "$PLAYCFG/logs" "$PLAYCFG/sstates"
for e in "$USERCFG"/*; do
    n=$(basename "$e")
    case $n in inis|logs|sstates) continue ;; esac
    [ -e "$PLAYCFG/$n" ] && [ ! -L "$PLAYCFG/$n" ] && continue
    ln -sfn "$(realpath --relative-to="$PLAYCFG" "$e")" "$PLAYCFG/$n"
done
cp -f "$USERCFG"/inis/*.ini "$PLAYCFG/inis/" 2>/dev/null || true
python3 -I - "$USERCFG/inis/PCSX2.ini" "$PLAYCFG/inis/PCSX2.ini" "$PINE_SLOT" "${MUTE:-0}" <<'EOF'
import re, sys
src, dst, slot, mute = sys.argv[1:5]
want = {'Logging': {'EnableFileLogging': 'true', 'EnableTimestamps': 'true', 'EnableEEConsole': 'true',
                    'EnableIOPConsole': 'true', 'EnableVerbose': 'false'},
        'EmuCore': {'EnablePINE': 'true', 'PINESlot': slot},
        'UI': {'ConfirmShutdown': 'false', 'SetupWizardIncomplete': 'false'}}
if mute == '1':  # (else the user's own setting)
    want['SPU2/Output'] = {'OutputMuted': 'true'}
out, sec, seen = [], None, set()
def flush(sec):
    out.extend(f'{k} = {v}' for k, v in want.get(sec, {}).items() if (sec, k) not in seen)
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
    if f'[{s}]' not in out:
        out.append(f'[{s}]'); flush(s)
open(dst, 'w').write('\n'.join(out) + '\n')
EOF
EMULOG=$PLAYCFG/logs/emulog.txt
: > "$EMULOG"

# --- start PCSX2 (X11 / XWayland: its window can be captured) ---
XENV=(QT_QPA_PLATFORM=xcb)
[ -n "${DISPLAY:-}" ] || die "no DISPLAY (start it from your desktop session)"
if [ -n "$MUTE" ]; then  # sound to a null sink (created on demand, like the test scripts')
    command -v pactl >/dev/null && { pactl list short sinks | grep -q dsr_mute ||
        pactl load-module module-null-sink sink_name=dsr_mute sink_properties=device.description=DSR-test-mute >/dev/null; }
    if [ "$BIN" = flatpak ]; then RUN+=(--env=PULSE_SINK=dsr_mute); else RUN+=(PULSE_SINK=dsr_mute); fi
fi
if [ "$BIN" = flatpak ]; then
    RUN+=(--socket=x11 --nosocket=wayland --env=QT_QPA_PLATFORM=xcb net.pcsx2.PCSX2)
else
    RUN+=("${XENV[@]}" "$BIN")
fi
T0=$(date +%s.%N)
echo "play-report: PCSX2 starting with $ISO"
echo "play-report: play; type a note + Enter here to mark a moment; close PCSX2 (or Ctrl+C here) to finish"
rm -f "$SOCK"
WAYLAND_DISPLAY= "${RUN[@]}" -datapath "$DATAPATH" -fastboot -- "$ISO" > "$WORK/pcsx2-stdout.txt" 2>&1 < /dev/null \
    3> "$WORK/instance" &
PID=$!
INST=  # the flatpak instance: closing kills only this one, never another PCSX2 (headless test runs)
if [ "$BIN" = flatpak ]; then
    for _ in $(seq 50); do [ -s "$WORK/instance" ] && break; sleep 0.1; done
    INST=$(cat "$WORK/instance" 2>/dev/null || true)
fi
rm -f "$WORK/instance"

now() { python3 -c "import time; print(f'{time.time() - $T0:.1f}')"; }
WIN=
find_win() {  # PCSX2's main X window (the client list of the window manager: xprop; or xwininfo)
    local w c
    if command -v xprop >/dev/null; then
        for w in $(xprop -root _NET_CLIENT_LIST 2>/dev/null | grep -o '0x[0-9a-f]*'); do
            c=$(xprop -id "$w" WM_CLASS _NET_WM_NAME 2>/dev/null) || continue
            grep -qi 'WM_CLASS.*pcsx2' <<< "$c" || continue
            grep -qi -E '_NET_WM_NAME.*(settings|properties|log|debugger|controller|memory card)' <<< "$c" && continue
            WIN=$w; return 0
        done
    elif command -v xwininfo >/dev/null; then  # the largest window of class pcsx2*
        WIN=$(xwininfo -root -tree 2>/dev/null | awk 'tolower($0) ~ /\("pcsx2/ && $1 ~ /^0x/ {
            for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+x[0-9]+\+/) { split($i, d, /[x+]/); print d[1] * d[2], $1 } }' |
            sort -n | tail -1 | cut -d' ' -f2)
    fi
}
shot() {
    [ "$EVERY" -gt 0 ] && command -v import >/dev/null || return 0
    [ -n "$WIN" ] || find_win
    [ -n "$WIN" ] || return 0
    local t; t=$(now)
    import -silent -window "$WIN" -resize "${WIDTH}x" -quality 60 "$WORK/shots/t$(printf '%07.1f' "$t").jpg" 2>/dev/null ||
        WIN=  # window gone or replaced (fullscreen toggle): look again next time
}
NMARK=0
mark() {
    local t; t=$(now); NMARK=$((NMARK + 1))
    printf '%s s  mark %d: %s\n' "$t" "$NMARK" "${1:-(no note)}" >> "$WORK/marks.txt"
    if [ -n "$STATES" ]; then
        python3 -I - "$SOCK" "$((NMARK % 10))" <<'EOF' && echo "play-report: mark $NMARK at $t s, savestate slot $((NMARK % 10))" ||
import socket, struct, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect(sys.argv[1])
s.sendall(struct.pack('<IBB', 6, 9, int(sys.argv[2])))  # PINE MsgSaveState
r = s.recv(5)
sys.exit(0 if len(r) == 5 and r[4] == 0 else 1)
EOF
            echo "play-report: mark $NMARK at $t s (savestate failed: PINE not reachable)"
        echo "$NMARK $t" >> "$WORK/states.txt"
    else
        echo "play-report: mark $NMARK at $t s"
    fi
}
STOP=
trap 'STOP=1' INT TERM
TTY=; [ -t 0 ] && TTY=1
while kill -0 "$PID" 2>/dev/null && [ -z "$STOP" ]; do
    if [ -n "$FOR" ] && python3 -c "import sys; sys.exit(0 if $(now) >= $FOR else 1)"; then break; fi
    shot
    if [ -n "$TTY" ]; then
        if read -r -t "${EVERY/#0/2}" line 2>/dev/null; then mark "$line"; fi
    else
        sleep "${EVERY/#0/2}" || true
    fi
done
trap - INT TERM
DUR=$(now)
if kill -0 "$PID" 2>/dev/null; then
    echo "play-report: closing PCSX2"
    if [ -n "$INST" ]; then flatpak kill "$INST" 2>/dev/null || true; else kill "$PID" 2>/dev/null || true; fi
    for _ in $(seq 50); do kill -0 "$PID" 2>/dev/null || break; sleep 0.2; done
fi
wait "$PID" 2>/dev/null || true

# --- the report ---
echo "play-report: packing the report"
cp -f "$EMULOG" "$WORK/emulog.txt" 2>/dev/null || : > "$WORK/emulog.txt"
# savestates of the last 2 marks (PINE slot n % 10 -> "<serial> (<crc>).0n.p2s" in sstates/)
if [ -s "$WORK/states.txt" ]; then
    tail -2 "$WORK/states.txt" | while read -r n t; do
        f=$(ls -t "$PLAYCFG/sstates/"*".$(printf '%02d' $((n % 10))).p2s" 2>/dev/null | head -1 || true)
        [ -n "$f" ] && cp -f "$f" "$WORK/states/mark$n-t${t}s.p2s"
    done
fi
rm -f "$WORK/states.txt"
rmdir "$WORK/states" 2>/dev/null || true
{
    echo "report     $(basename "$OUT")   made $(date -Is)"
    echo "played     $DUR s, $(ls "$WORK/shots" | wc -l) screenshots every $EVERY s, $NMARK marks"
    echo "iso        $ISO ($(du -h "$ISO" | cut -f1), built $(date -Is -r "$ISO"))"
    echo "checkout   $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?') $(git -C "$ROOT" log -1 --format=%s 2>/dev/null || true)$(git -C "$ROOT" diff --quiet HEAD 2>/dev/null || echo ' + local changes')"
    echo "           (the checkout the script lives in: the ISO may be older, see its build time)"
    echo "pcsx2      $VERSION, data folder $PLAYCFG"
    echo "host       $(uname -srm), $(nproc) threads, session ${XDG_SESSION_TYPE:-?}"
    echo
    echo "pcsx2.ini (without key bindings):"
    grep -v -E '^\s*$' "$PLAYCFG/inis/PCSX2.ini" | awk '/^\[/ {sec = $0} sec !~ /Pad|Hotkeys|USB|InputSources|GameList|RecentIsos|Filenames/'
} > "$WORK/info.txt"
python3 -I - "$WORK" "$ROOT/tools/hitchsum.py" <<'EOF' > "$WORK/summary.txt" || true
import os, re, subprocess, sys
work, hitchsum = sys.argv[1:3]
log = os.path.join(work, 'emulog.txt')
lines = open(log, errors='replace').read().splitlines()
def show(title, pat, cap=40):
    hits = [l for l in lines if re.search(pat, l)]
    print(f'== {title}: {len(hits)}')
    for l in hits[:cap]:
        print('  ' + l)
    if len(hits) > cap:
        print(f'  ... {len(hits) - cap} more in emulog.txt')
    print()
info = open(os.path.join(work, 'info.txt')).read().split('\n\n')[0]
print(info, '\n')
marks = os.path.join(work, 'marks.txt')
print('== marks')
print(open(marks).read() if os.path.exists(marks) else '  (none)\n')
print('== hitch summary (tools/hitchsum.py, one row per 5 min of game time, last row the whole run)')
r = subprocess.run([sys.executable, '-I', hitchsum, log], capture_output=True, text=True)
print(r.stdout or r.stderr or '  (no hitch: lines: the game did not reach the world, or an older build)')
show('crashes / errors', r'OUT OF RAM|READ ERROR|Not enough VRAM|exception|TLB Miss|Unhandled|Bus error|stopping|NOT READ')
show('failed allocations (alloc: lines: addr2line -f -e build/proto.sym.elf <caller>)', r'alloc: |alloc .* FAILED|no RAM')
show('single slow frames (spike:, outside loading screens)', r'spike: ')
show('deaths and falls out of the world', r'player: (died|fell)')
show('warps / loading screens', r'world: warp|loading: closed')
show('world blocks on / off', r'world: block \d+ \S+ (on|off)|off to make room')
show('streaming trouble', r'settle stopped|held at|did not load|cells missing|near pop-in|slow plan|pack: waited|no room', 25)
print('== log, last 40 lines (without the renderer\'s)')
ee = [l for l in lines if not re.search(r'(?i)vulkan|opengl|\bGS\b|achievements|discord|\bhost\b', l)]
for l in ee[-40:]:
    print('  ' + l)
EOF
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
python3 -I - "$WORK" "$OUT" <<'EOF'
import os, sys, zipfile
work, out = sys.argv[1:3]
with zipfile.ZipFile(out, 'w') as z:
    for d, _, files in os.walk(work):
        for f in sorted(files):
            p = os.path.join(d, f)  # JPEGs and savestates are compressed already
            z.write(p, os.path.relpath(p, work), zipfile.ZIP_STORED if f.endswith(('.jpg', '.p2s')) else zipfile.ZIP_DEFLATED)
EOF
rm -rf "$WORK"
echo "play-report: $OUT ($(du -h "$OUT" | cut -f1)): send this file with the report"
# a manual run: open the report to pin comments on its shots (saved into the same zip)
if [ -n "$ANNOTATE" ] && [ -z "$FOR" ] && [ -n "$TTY" ] && [ -t 1 ]; then
    echo "play-report: opening it to annotate (pin comments on the shots; Ctrl+C when done, --no-annotate to skip)"
    python3 -I "$ROOT/tools/annotate_report.py" "$OUT" || true
fi
