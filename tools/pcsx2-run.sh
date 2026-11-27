#!/bin/bash
# Usage: pcsx2-run.sh <file.elf> [seconds=8] [shot.png]  — boot an ELF in PCSX2.
# With a shot path (tests): runs pcsx2-dsrtest, the headless test runner (tools/pcsx2-headless/, PCSX2 v2.8.2 without
#   Qt, Vulkan surfaceless, sound to nullout), as a child process in this container: no display, no window, no audio
#   device. Screenshots after N seconds through its control socket (tools/pcsx2ctl.py), then stops only that instance.
#   KEEP=1 leaves the instance running and prints PID=<pid>, CTL=<control socket>, LOG=<EE/IOP log> (stop:
#   tools/pcsx2ctl.py --sock <CTL> quit, or kill <pid>).
#   Test options (env, headless only; tools/test-*.sh, tools/pine.py):
#     ARGS='test=tests/x.txt'  argv for the ELF (-gameargs): src/testscript.c runs that script
#     PINE=<slot>   enable PINE on <slot> (socket /run/user/<uid>/app/net.pcsx2.PCSX2/pcsx2.sock.<slot>, the path the
#                   Flatpak used, so tools/pine.py is unchanged); data dir /tmp/pcsx2-headless/data/<slot>/PCSX2: the
#                   user's PCSX2.ini (tools/test-pcsx2-ini.py: PINE, BIOS folder, turbo, PCSX2_GS keys) and its own
#                   logs/emulog.txt, snaps/, sstates/, memcards/. Without PINE the slot is 99.
#     shot path '-' no screenshot (with KEEP=1 the caller drives the instance, e.g. tools/test-run.py)
#     DISC=<image.iso>  put a disc image in the drive (-elf <elf> -- <iso>): the ELF still boots from host:, so a
#                   test script loads, and a -DPACK_CDROM=1 build reads its packs from cdrom0:
#     STATEFILE=<file.p2s>  load that savestate after boot (tools/warmstate.py)
#     BOOTISO=<image.iso>  boot the disc itself the way a player does (BIOS fast boot of SYSTEM.CNF's ELF, packs
#                   from cdrom0:); ARGS still reach that ELF (-gameargs) and host: (HostFs) is the disc image's folder
#     PCSX2_HEADLESS=<binary>  runner to use (default: tools/ext/pcsx2-build/bin/pcsx2-dsrtest of this tree, else of
#                   the main checkout; build it with tools/pcsx2-headless/build.sh)
#     PCSX2_USERCFG=<dir>  the PCSX2 config the runs start from (<dir>/inis/PCSX2.ini, BIOS in <dir>/bios; default
#                   the Flatpak's, as tools/ps2-pcsx2.sh): e.g. under WSL a Windows PCSX2's Documents/PCSX2 folder
#     PCSX2_TIMEOUT=<s>  hard wall-clock cap of the runner (-timeout, default 3600; exit 3): an instance whose harness
#                   died still ends on its own
#   /tmp/pcsx2-headless/slot.<slot>.pid holds "<pid> <starttime>" (field 22 of /proc/<pid>/stat) while it runs, for
#   tools/test-run.py reap_slot, tools/memwatch.py and tools/qpeak.py.
# Without a shot path (user launch): replaces any visible PCSX2 and opens the ELF on the desktop (the Flatpak
#   net.pcsx2.PCSX2 on the host).
ELF=$(realpath "$1"); T=${2:-8}; SHOT=$3
H=$(cd "$(dirname "$0")/.." && pwd)
# The ELF's saves (src/save.c, host:save/): the one writable folder under the ro host: root
mkdir -p "$(dirname "$ELF")/save"
if [ -z "$SHOT" ]; then
    HX=$H/tools/hostexec  # runs on the host (distrobox-host-exec inside a distrobox)
    FS="--filesystem=$(dirname "$ELF"):ro"; FSW="--filesystem=$(dirname "$ELF")/save"
    "$HX" bash -c "for i in \$(flatpak ps --columns=instance,application | awk '\$2==\"net.pcsx2.PCSX2\"{print \$1}'); do
        [ -e /tmp/pcsx2-headless/\$i ] || flatpak kill \$i; done; sleep 1
        setsid flatpak run '$FS' '$FSW' net.pcsx2.PCSX2 -fastboot -elf '$ELF' > '$H/tools/pcsx2.log' 2>&1 < /dev/null &"
    exit 0
fi
if [ -n "$PCSX2_HEADLESS" ]; then  # an explicit binary is used or the run fails (never a silent fallback in an A/B)
    BIN=$PCSX2_HEADLESS
    [ -x "$BIN" ] || { echo "pcsx2-run.sh: PCSX2_HEADLESS=$BIN is not executable" >&2; exit 1; }
else
    BIN=$H/tools/ext/pcsx2-build/bin/pcsx2-dsrtest
    [ -x "$BIN" ] || BIN="$(git -C "$H" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)/../tools/ext/pcsx2-build/bin/pcsx2-dsrtest"
    [ -x "$BIN" ] || { echo "pcsx2-run.sh: no pcsx2-dsrtest; run tools/pcsx2-headless/build.sh" >&2; exit 1; }
fi
SLOT=${PINE:-99}
DP=/tmp/pcsx2-headless/data/$SLOT
CTL=/tmp/pcsx2-headless/ctl.$SLOT.sock
LOG=$DP/PCSX2/logs/emulog.txt
RUN=/run/user/$(id -u)/app/net.pcsx2.PCSX2  # PINE socket dir (tools/pine.py sock_path); the Flatpak used to create it
mkdir -p /tmp/pcsx2-headless "$RUN" "$DP/PCSX2/logs"
if [ -z "$PINE" ]; then
    # Runs without PINE share slot 99 (its socket, log, pid note and data dir), so they take turns: the lock is held on
    # fd 8, which the emulator inherits, until the last of this script and its emulator (KEEP=1) is gone. PINE slots
    # are locked by their caller (tools/test-run.py acquire_slot).
    exec 8> /tmp/pcsx2-headless/slot.99.lock
    flock -w 600 8 || { echo "pcsx2-run.sh: slot 99 busy for 600 s (another run without PINE)" >&2; exit 1; }
fi
set --
[ -n "$ARGS" ] && set -- "$@" -gameargs "$ARGS"
# STATEFILE=<file.p2s>: start from that savestate. The disc / ELF arguments stay: the state needs the same disc in the drive
[ -n "$STATEFILE" ] && set -- "$@" -statefile "$STATEFILE"
if [ -n "$BOOTISO" ]; then set -- "$@" -- "$(realpath "$BOOTISO")"
else set -- "$@" -elf "$ELF"; [ -n "$DISC" ] && set -- "$@" -- "$(realpath "$DISC")"; fi
# Own data dir per slot: the user's ini stays untouched and parallel slots never share logs / states / cards
python3 -I "$H/tools/test-pcsx2-ini.py" "${PCSX2_USERCFG:-${DISTROBOX_HOST_HOME:-$HOME}/.var/app/net.pcsx2.PCSX2/config/PCSX2}" \
    "$DP" "$SLOT" || exit 1
rm -f "$RUN/pcsx2.sock.$SLOT" "$CTL" "$LOG"
# 9>&-: never hand the job-queue lock (tools/q holds it on fd 9) to the emulator
XDG_RUNTIME_DIR=$RUN setsid "$BIN" -datapath "$DP" -ctl "$CTL" -logfile "$LOG" -timeout "${PCSX2_TIMEOUT:-3600}" \
    -fastboot "$@" > "$DP/stdout.log" 2>&1 < /dev/null 9>&- &
PID=$!
# the slot's note for tools/test-run.py reap_slot, written as soon as the process exists: a harness that dies while
# this script is still launching leaves no emulator behind that the slot's next run cannot find. The start time
# (clock ticks since boot) tells a reused pid apart.
echo "$PID $(awk '{print $22}' "/proc/$PID/stat" 2>/dev/null)" > "/tmp/pcsx2-headless/slot.$SLOT.pid"
for _ in $(seq 300); do [ -S "$CTL" ] && break; kill -0 "$PID" 2>/dev/null || break; sleep 0.05; done
if [ -n "$KEEP" ]; then echo "PID=$PID"; echo "CTL=$CTL"; echo "LOG=$LOG"; fi
[ "$SHOT" = "-" ] && exit 0
sleep "$T"
python3 -I "$H/tools/pcsx2ctl.py" --sock "$CTL" shot "$(realpath -m "$SHOT")" > /dev/null; rc=$?
if [ -z "$KEEP" ]; then
    python3 -I "$H/tools/pcsx2ctl.py" --sock "$CTL" quit > /dev/null 2>&1
    for _ in $(seq 100); do kill -0 "$PID" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
    rm -f "/tmp/pcsx2-headless/slot.$SLOT.pid"
fi
exit $rc
