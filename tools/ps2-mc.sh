#!/bin/bash
# PS2 memory card images (tools-hwperf; for the saves of ROADMAP 17). Wraps mymcplus 3.0.5 (GPL-3, user-local venv
# ~/tools/mymc-venv; https://git.sr.ht/~thestr4ng3r/mymcplus) for 8 MB .ps2 images as PCSX2 and real cards use them.
#
#   ps2-mc.sh new <card.ps2>                      format a blank 8 MB card
#   ps2-mc.sh add <card.ps2> <save dir> <files..> mkdir <save dir> on the card and copy the files in
#   ps2-mc.sh ls <card.ps2> [dir]                 list (root: save directories)
#   ps2-mc.sh get <card.ps2> <save dir> <out dir> extract one save directory's files
#   ps2-mc.sh info <card.ps2>                     dir (titles from icon.sys), free space, file system check
#   ps2-mc.sh user [slot=1] <out.ps2>             copy the user's PCSX2 card McdNNN.ps2 (read only, never written)
#   ps2-mc.sh pcsx2 <card.ps2> [hostfs|nohostfs]  install a card as slot 1 of tools/ps2-pcsx2.sh's own PCSX2 config
# Env: MYMC=<mymcplus command>, PCSX2_USERCFG (default <host home>/.var/app/net.pcsx2.PCSX2/config/PCSX2).
set -e
MYMC=${MYMC:-$HOME/tools/mymc-venv/bin/mymcplus}
USERCFG=${PCSX2_USERCFG:-${DISTROBOX_HOST_HOME:-$HOME}/.var/app/net.pcsx2.PCSX2/config/PCSX2}
[ $# -ge 2 ] || { sed -n 2,13p "$0"; exit 1; }
CMD=$1; shift
case "$CMD" in
new)  [ -e "$1" ] && { echo "$1 exists" >&2; exit 1; }; "$MYMC" "$1" format; "$MYMC" "$1" df;;
add)  C=$1 D=$2; shift 2
      "$MYMC" "$C" ls "/$D" >/dev/null 2>&1 || "$MYMC" "$C" mkdir "/$D"
      "$MYMC" "$C" add -d "/$D" "$@"; "$MYMC" "$C" ls "/$D";;
ls)   "$MYMC" "$1" ls "/${2#/}";;
get)  C=$(realpath "$1"); mkdir -p "$3"
      for f in $("$MYMC" "$C" ls "/$2" | awk '$1 !~ /d/ {print $NF}'); do
          "$MYMC" "$C" extract -d "/$2" -o "$3/$f" "$f"; done; ls -la "$3";;
info) "$MYMC" "$1" dir || true; "$MYMC" "$1" df; "$MYMC" "$1" check;;
user) S=1; [ $# -ge 2 ] && { S=$1; shift; }
      SRC=$USERCFG/memcards/Mcd$(printf %03d "$S").ps2
      [ -f "$SRC" ] || { echo "no card $SRC" >&2; exit 1; }
      cp --no-preserve=mode "$SRC" "$1"; echo "copied $SRC -> $1"
      if [ "$(head -c 27 "$1")" != "Sony PS2 Memory Card Format" ]; then
          echo "not formatted yet (PCSX2 creates blank cards; the BIOS browser or a game's first save formats them)"
      else "$MYMC" "$1" dir || true; "$MYMC" "$1" df; fi;;
pcsx2) CFG=$HOME/tools/pcsx2-cfg/${2:-hostfs}/PCSX2/memcards
      mkdir -p "$CFG"; cp "$1" "$CFG/Mcd001.ps2"; echo "slot 1 of the ${2:-hostfs} test config: $CFG/Mcd001.ps2";;
*)    sed -n 2,13p "$0"; exit 1;;
esac
