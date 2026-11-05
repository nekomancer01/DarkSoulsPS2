#!/bin/bash
# Real-PS2 loop over ps2link. UNTESTED ON HARDWARE: no PS2 was
# available when this was written; the commands are the documented ps2link / ps2client ones and DRY=1 prints them.
#
#   ps2-run.sh <ps2 ip> [elf=build/proto.elf] [args...]
#       ps2client -h <ip> execee host:<elf> from the ELF's folder: ps2client serves host: from its working directory,
#       so the game's host:data/*.pak paths (pack.h PACK_ROOT) load unchanged, as under PCSX2 HostFs. The PS2's printf
#       output (DSRPROTO / PROF lines) streams to the terminal and to tools/ps2-run.log. Ctrl-C stops listening.
#       RESET=1 resets ps2link first (after a crash), PROFILE=1 runs proto_prof.elf.
#   ps2-run.sh setup <ps2 ip> [netmask=255.255.255.0] [gateway=<ip with .1>] [out=ps2link-usb/]
#       folder to copy to a FAT32 USB stick or the memory card: PS2LINK.ELF + IPCONFIG.DAT ("ip netmask gateway").
#       Boot PS2LINK.ELF with uLaunchELF / wLaunchELF (FreeMcBoot), see the thread note.
#   ps2-run.sh reset|listen|dump <ps2 ip>     ps2client reset / listen (printf only) / scrdump
# Env: DRY=1 print the commands only; PS2CLIENT=<path> (default: ps2client from $PS2DEV/bin);
#      PS2LINK_DIR (default ~/tools/ps2link-bin/ps2link: ps2link release "latest" 0c6138c5, default build).
set -e
H=$(cd "$(dirname "$0")/.." && pwd)
PS2CLIENT=${PS2CLIENT:-$(command -v ps2client || echo "$HOME/tools/ps2dev-2.0.0/ps2dev/bin/ps2client")}
PS2LINK_DIR=${PS2LINK_DIR:-$HOME/tools/ps2link-bin/ps2link}
run() { if [ -n "$DRY" ]; then printf '[dry]'; printf ' %q' "$@"; echo; else "$@"; fi; }
ipok() { [[ $1 =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "not an IPv4 address: $1" >&2; exit 1; }; }
[ $# -ge 1 ] || { sed -n 2,19p "$0"; exit 1; }

case "$1" in
setup)
    ipok "$2"; MASK=${3:-255.255.255.0}; GW=${4:-${2%.*}.1}; OUT=${5:-ps2link-usb}
    ipok "$MASK"; ipok "$GW"
    [ -f "$PS2LINK_DIR/PS2LINK.ELF" ] || { echo "PS2LINK.ELF missing in $PS2LINK_DIR" >&2; exit 1; }
    run mkdir -p "$OUT"
    run cp "$PS2LINK_DIR/PS2LINK.ELF" "$PS2LINK_DIR/icon.sys" "$PS2LINK_DIR/PS2LINK.icn" "$OUT/"
    if [ -n "$DRY" ]; then echo "[dry] IPCONFIG.DAT: $2 $MASK $GW"; else printf '%s %s %s\n' "$2" "$MASK" "$GW" > "$OUT/IPCONFIG.DAT"; fi
    echo "copy $OUT/* to the USB stick (or mc0:/BOOT/ps2link/) and start PS2LINK.ELF from uLaunchELF"
    exit 0;;
reset|listen)
    ipok "$2"; run "$PS2CLIENT" -h "$2" "$1"; exit 0;;
dump)
    ipok "$2"; run "$PS2CLIENT" -h "$2" scrdump; exit 0;;
esac

IP=$1; shift; ipok "$IP"
DEF=${BUILD:-$H/build}/proto.elf; [ -n "$PROFILE" ] && DEF=${BUILD:-$H/build}/proto_prof.elf
ELF=$(realpath "${1:-$DEF}"); shift || true
[ -f "$ELF" ] || { echo "no such ELF: $ELF (build it first)" >&2; exit 1; }
[ -d "$(dirname "$ELF")/data" ] || echo "warning: no data/ next to $ELF: the packs will not load" >&2
[ -x "$PS2CLIENT" ] || [ -n "$DRY" ] || { echo "ps2client not found ($PS2CLIENT); source ps2/env.sh" >&2; exit 1; }
if [ -z "$DRY" ] && ! ping -c 1 -W 2 "$IP" >/dev/null 2>&1; then
    echo "$IP does not answer ping: is PS2LINK.ELF running on the PS2 (ps2-run.sh setup)?" >&2; exit 1
fi
cd "$(dirname "$ELF")"
[ -n "$RESET" ] && { run "$PS2CLIENT" -h "$IP" reset; [ -n "$DRY" ] || sleep 3; }
if [ -n "$DRY" ]; then
    echo "[dry] (cd $(pwd))"; run "$PS2CLIENT" -h "$IP" execee "host:$(basename "$ELF")" "$@"
    echo "[dry] output -> $H/tools/ps2-run.log"
else
    "$PS2CLIENT" -h "$IP" -t 0 execee "host:$(basename "$ELF")" "$@" 2>&1 | tee "$H/tools/ps2-run.log"
fi
