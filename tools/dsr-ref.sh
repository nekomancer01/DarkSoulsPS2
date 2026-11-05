#!/bin/bash
# Usage: dsr-ref.sh start | load | cam ARGS | save|restore NAME | shot <out.png> | in <dsr-ei cmds...> | stop
#   load: from the title screen, Continue the duplicate's save until the player stands in the world (max 180 s)
#   save NAME / restore NAME: copy the duplicate's save to/from ~/dsr-ref/saves/NAME.sl2 (restore only while stopped).
#     saves/firelink.sl2 = standing at the Firelink bonfire, so `restore firelink; start; load` lands there.
#     saves/burg.sl2 = Undead Burg bonfire 1012962 (player (0.1, -10.0, -59.2), m10_01; warps reach the Parish too).
#     saves/orig-painted-world.sl2 = the user's save as copied. New spot: dsr_mem.py bonfire ID; dsr_mem.py hp 0; then save NAME.
#   cam ARGS: tools/dsr-ref-cam.py on the host (place / read the camera, autotest points, screenshot; JSON out)
#   in: keyboard/mouse through gamescope's libei socket, e.g. `in key enter wait 500 move 200 0 click` (tools/dsr-ei.c)
# Reference DSR for visual comparisons: a duplicate install (~/dsr-ref/game) with its own Proton prefix
# (~/dsr-ref/compat, a reflink copy), run inside a headless gamescope on the host. The user's Steam install,
# prefix and save files are never touched; Steam is not used to launch it, so no Steam Cloud sync runs.
HOST_HOME=${DISTROBOX_HOST_HOME:-$HOME}  # the host user's home (distrobox) or this one
T=$(cd "$(dirname "$0")" && pwd)
HX=$T/hostexec  # runs on the host (distrobox-host-exec inside a distrobox)
H=${DSR_REF:-$HOST_HOME/dsr-ref}
PROTON="$H/tools/proton-11.0/proton"
W=${W:-1920}; HT=${HT:-1080}
# RDC=1: load the RenderDoc capture layer into the game process only (not gamescope), with DXVK on plain descriptor
# sets (RenderDoc's replay of VK_EXT_descriptor_buffer captures hits VK_ERROR_DEVICE_LOST) and dump DXVK's DXBC/SPIR-V
# to ~/dsr-ref/rdc/shaders, so tools/dsr-rdc.sh can name the captured shaders' constant buffers
# runinprefix skips Proton's DXVK overrides (the game then renders through wined3d/OpenGL): set them here, as
# Proton's normal launch does (d3d11/d3d10core/d3d9/dxgi native = the DXVK copies in the prefix's system32)
GAME_ENV="WINEDLLOVERRIDES=d3d11,d3d10core,d3d9,dxgi=n $DBG_ENV"
[ -n "$RDC" ] && GAME_ENV="$GAME_ENV VK_ADD_IMPLICIT_LAYER_PATH=$H/tools/rdlayer \
    ENABLE_VULKAN_RENDERDOC_CAPTURE=1 DXVK_CONFIG='dxvk.enableDescriptorBuffer=False;dxvk.enableDescriptorHeap=False;dxvk.enableGraphicsPipelineLibrary=False' RENDERDOC_CAPFILE=$H/rdc/dsr DXVK_SHADER_DUMP_PATH=$H/rdc/shaders"
case "$1" in
start)
    "$HX" pgrep -f "^bwrap .*$H/" >/dev/null && { echo "dsr-ref: already running (dsr-ref.sh stop first)" >&2; exit 1; }
    # Offline by construction: bwrap gives the whole gamescope + game tree an empty network namespace (loopback only)
    # and hides the real Steam dirs behind tmpfs, so steam_api finds no Steam client and nothing can reach the net
    # or the user's install/saves. Proton runs from a reflink copy in ~/dsr-ref/tools.
    # Muted for the user (2026-10-31): audio goes to the null sink dsr_mute (created on demand), never the speakers.
    "$HX" bash -c "pactl list short sinks | grep -q dsr_mute || pactl load-module module-null-sink \
        sink_name=dsr_mute sink_properties=device.description=DSR-test-mute >/dev/null
        cd '$H/game' && PULSE_SINK=dsr_mute STEAM_COMPAT_DATA_PATH='$H/compat' \
        STEAM_COMPAT_CLIENT_INSTALL_PATH='$H/nosteam' SteamAppId=570940 SteamGameId=570940 $EXTRA_ENV \
        setsid bwrap --dev-bind / / --unshare-net --tmpfs $HOST_HOME/.local/share/Steam --tmpfs $HOST_HOME/.steam --tmpfs /tmp/.X11-unix \
        -- gamescope --backend headless -W $W -H $HT -w $W -h $HT -- env $GAME_ENV '$PROTON' ${VERB:-runinprefix} DarkSoulsRemastered.exe \
        > '$H/run.log' 2>&1 < /dev/null & echo \$! > '$H/gamescope.pid'"
    ;;
shot)
    OUT=$(realpath -m "$2")
    # the headless instance's control socket (Connector Name: Headless); full-res PNG, gamescope writes it async
    SOCK=$("$HX" bash -c "for s in \$(ls \$XDG_RUNTIME_DIR | grep -E '^gamescope-[0-9]+$'); do
        GAMESCOPE_WAYLAND_DISPLAY=\$s gamescopectl 2>&1 | grep -q 'Connector Name: Headless' && echo \$s; done | tail -1")
    "$HX" bash -c "rm -f '$H/shot.png'; GAMESCOPE_WAYLAND_DISPLAY=$SOCK gamescopectl screenshot '$H/shot.png'
        for _ in \$(seq 50); do [ -s '$H/shot.png' ] && break; sleep 0.1; done; sleep 0.3; cat '$H/shot.png'" > "$OUT"
    ls -la "$OUT"
    ;;
load)
    # title: any key -> "offline mode" OK (E) -> Continue (E); keep tapping until dsr_mem sees a loaded player.
    # E is DSR's keyboard confirm; Enter only passes "press any button". Tapping E in the world would interact,
    # so it stops as soon as the player exists, then waits for the fade-in.
    MEM="python3 -I $(cd "$(dirname "$0")" && pwd)/dsr_mem.py"
    for i in $(seq 60); do
        "$HX" $MEM ready 2>/dev/null && { sleep 6; "$HX" $MEM pos; exit 0; }
        "$0" in key enter wait 300 key e 2>/dev/null; sleep 2.5
    done
    echo "dsr-ref: no player after 180 s" >&2; exit 1
    ;;
save|restore)
    SAV="$H/compat/pfx/drive_c/users/steamuser/Documents/NBGI/DARK SOULS REMASTERED/70049876/DRAKS0005.sl2"
    if [ "$1" = save ]; then "$HX" cp "$SAV" "$H/saves/$2.sl2"
    else
        "$HX" pgrep -f 'Z:.*[D]arkSoulsRemastered' >/dev/null && { echo "dsr-ref: stop the game first" >&2; exit 1; }
        "$HX" cp "$H/saves/$2.sl2" "$SAV"
    fi
    ;;
cam)
    shift
    # the shot path must be visible on the host: make it absolute
    A=(); while [ $# -gt 0 ]; do A+=("$1"); [ "$1" = --shot ] && { A+=("$(realpath -m "$2")"); shift; }; shift; done
    "$HX" python3 -I "$T/dsr-ref-cam.py" "${A[@]}"
    ;;
in)
    shift
    "$HX" bash -c "for s in \$(ls \$XDG_RUNTIME_DIR | grep -E '^gamescope-[0-9]+$'); do
        GAMESCOPE_WAYLAND_DISPLAY=\$s gamescopectl 2>&1 | grep -q 'Connector Name: Headless' && S=\$s; done
        '$H/tools/dsr-ei' \$XDG_RUNTIME_DIR/\$S-ei $*"
    ;;
stop)
    # wineserver -k ends every wine process of this prefix; then every sandboxed instance by process group (an
    # anchored ^bwrap pattern, so this shell's own command line never matches; the pid file alone missed a restarted
    # instance once, 2026-10-31)
    "$HX" bash -c "WINEPREFIX='$H/compat/pfx' '$H/tools/proton-11.0/files/bin/wineserver' -k 2>/dev/null; sleep 2
        for p in \$(pgrep -f '^(bwrap|gamescope|gamescopereaper) .*$H/'); do kill -- -\$(ps -o pgid= -p \$p | tr -d ' ') 2>/dev/null; kill \$p 2>/dev/null; done
        sleep 2; pkill -9 -f '^(bwrap|gamescope|gamescopereaper) .*$H/'; true"   # a stuck headless gamescope ignored SIGTERM once
    ;;
*) echo "usage: $0 start | shot out.png | in cmds... | stop"; exit 1 ;;
esac
