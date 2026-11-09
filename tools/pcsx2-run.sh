#!/bin/bash
# Usage: pcsx2-run.sh <file.elf> [seconds=8] [shot.png]  — boot an ELF in the PCSX2 flatpak (host).
# With a shot path (tests): runs headless on the virtual X display :99 (Xvfb), screenshots it after
#   N seconds and kills only that instance. Works with the monitor off and never touches a PCSX2 the user has open.
#   KEEP=1 leaves the headless instance running (prints INST=<flatpak instance id>; stop: flatpak kill <id>).
#   Test options (env, headless only; tools/test-*.sh, tools/pine.py):
#     ARGS='test=tests/x.txt'  argv for the ELF (PCSX2 -gameargs): src/testscript.c runs that script
#     PINE=<slot>   enable PINE on <slot> (socket /run/user/<uid>/app/net.pcsx2.PCSX2/pcsx2.sock.<slot>); the instance
#                   then runs on its own X display :1xx (100 + slot % 100) and data dir
#                   ~/.var/app/net.pcsx2.PCSX2/test/<slot>/PCSX2 (host home): the user's
#                   PCSX2.ini + PINE + a GS-dump hotkey (F8), its own logs/emulog.txt, snaps/ and sstates/
#     shot path '-' no screenshot (with KEEP=1 the caller drives the instance, e.g. tools/test-run.py)
#     DISC=<image.iso>  put a disc image in the drive (-elf <elf> -- <iso>): the ELF still boots from host:, so a
#                   test script loads, and a -DPACK_CDROM=1 build reads its packs from cdrom0:
#     STATEFILE=<file.p2s>  load that savestate after boot (tools/warmstate.py)
#     BOOTISO=<image.iso>  boot the disc itself the way a player does (BIOS fast boot of SYSTEM.CNF's ELF, packs
#                   from cdrom0:); ARGS still reach that ELF (PCSX2 -gameargs) and host: (HostFs) still serves the
#                   test script and saves from <file.elf>'s folder; <file.elf> gives that folder and nothing else
# Without a shot path (user launch): replaces any visible PCSX2 and opens the ELF on the desktop.
ELF=$(realpath "$1"); T=${2:-8}; SHOT=$3
H=$(cd "$(dirname "$0")/.." && pwd)
HX=$H/tools/hostexec  # runs on the host (distrobox-host-exec inside a distrobox)
FS="--filesystem=$(dirname "$ELF"):ro"
# The ELF's saves (src/save.c, host:save/): the one writable folder under the ro host: root
mkdir -p "$(dirname "$ELF")/save"; FSW="--filesystem=$(dirname "$ELF")/save"
if [ -z "$SHOT" ]; then
    "$HX" bash -c "for i in \$(flatpak ps --columns=instance,application | awk '\$2==\"net.pcsx2.PCSX2\"{print \$1}'); do
        [ -e /tmp/pcsx2-headless/\$i ] || flatpak kill \$i; done; sleep 1
        setsid flatpak run '$FS' '$FSW' net.pcsx2.PCSX2 -fastboot -elf '$ELF' > '$H/tools/pcsx2.log' 2>&1 < /dev/null &"
    exit 0
fi
# PINE runs get their own X display (:1xx from the slot) so parallel instances never overlap in screenshots
D=:99
[ -n "$PINE" ] && D=:$((100 + PINE % 100))
if ! DISPLAY=$D xdpyinfo >/dev/null 2>&1; then
    # 9>&-: never hand the job-queue lock (tools/q holds it on fd 9) to the long-lived Xvfb
    setsid Xvfb $D -screen 0 1280x960x24 -nolisten tcp > /tmp/xvfb99.log 2>&1 < /dev/null 9>&- &
    for _ in $(seq 50); do DISPLAY=$D xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
fi
mkdir -p /tmp/pcsx2-headless
ID=/tmp/pcsx2-headless/id.$$
EXTRA=; ENVS=; DISCARG=
[ -n "$DISC" ] && DISCARG="-- '$(realpath "$DISC")'"
BOOT="-elf '$ELF'"
[ -n "$BOOTISO" ] && BOOT= DISCARG="-- '$(realpath "$BOOTISO")'"
[ -n "$ARGS" ] && EXTRA="-gameargs '$ARGS'"
# STATEFILE=<file.p2s>: start from that savestate (tools/warmstate.py warm starts; the file must be inside the PCSX2
# data dir, which the flatpak sees). The disc / ELF arguments stay: the state needs the same disc in the drive
[ -n "$STATEFILE" ] && EXTRA="$EXTRA -statefile '$STATEFILE'"
if [ -n "$PINE" ]; then
    # Own data dir per PINE slot: the shared ini stays untouched and parallel slots never share logs / states
    HH=$("$HX" printenv HOME)
    DP="$HH/.var/app/net.pcsx2.PCSX2/test/$PINE"
    python3 -I "$H/tools/test-pcsx2-ini.py" "$HH/.var/app/net.pcsx2.PCSX2/config/PCSX2" "$DP" "$PINE"
    EXTRA="$EXTRA -datapath '$DP'"
    ENVS="--env=XDG_RUNTIME_DIR=/run/user/$(id -u)/app/net.pcsx2.PCSX2"
    rm -f "/run/user/$(id -u)/app/net.pcsx2.PCSX2/pcsx2.sock.$PINE"
fi
# Headless tests are muted (user request 2026-10-31): sound goes to the null sink dsr_mute, created on demand.
"$HX" bash -c "pactl list short sinks | grep -q dsr_mute || pactl load-module module-null-sink \
    sink_name=dsr_mute sink_properties=device.description=DSR-test-mute >/dev/null
    DISPLAY=$D XAUTHORITY= WAYLAND_DISPLAY= setsid flatpak run --instance-id-fd=3 --env=PULSE_SINK=dsr_mute --socket=x11 \
    --nosocket=wayland --env=QT_QPA_PLATFORM=xcb $ENVS '$FS' '$FSW' net.pcsx2.PCSX2 -fastboot $EXTRA $BOOT $DISCARG \
    > '$H/tools/pcsx2-headless.log' 2>&1 < /dev/null 3> '$ID' &
    for _ in \$(seq 50); do [ -s '$ID' ] && break; sleep 0.1; done"
INST=$(cat "$ID"); rm -f "$ID"; touch "/tmp/pcsx2-headless/$INST"
[ -n "$KEEP" ] && echo "INST=$INST"
[ "$SHOT" = "-" ] && exit 0
sleep "$T"
# Capture the PCSX2 main window (largest top-level window) or the whole screen.
WIN=$(DISPLAY=$D xwininfo -root -children 2>/dev/null | awk '/^ +0x/{split($0,a,"  ");print $1}' | while read w; do
    DISPLAY=$D xwininfo -id "$w" 2>/dev/null | awk -v w="$w" '/Width:/{x=$2}/Height:/{y=$2}END{print x*y, w}'; done | sort -n | tail -1 | cut -d' ' -f2)
DISPLAY=$D import -window "${WIN:-root}" "$(realpath -m "$SHOT")"
[ -n "$KEEP" ] || { "$HX" flatpak kill "$INST"; rm -f "/tmp/pcsx2-headless/$INST"; }
