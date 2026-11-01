#!/bin/bash
# Usage: tools/dsr-rdc.sh OUTDIR [dsr-ref-cam args, e.g. --pos X Y Z --yaw D --pitch D | --point MAP N]
#        tools/dsr-rdc.sh OUTDIR --replay-only CAPTURE.rdc
# RenderDoc capture of the reference DSR (duplicate install, offline; tools/dsr-ref.sh) at a placed camera, then
# a headless replay that dumps every pass's constant buffers, named through DSR's own shader reflection:
#   OUTDIR/shot.png        the frame (gamescope screenshot just before the capture)
#   OUTDIR/camera.json     camera at capture time (tools/dsr-ref-cam.py)
#   OUTDIR/frame.json      raw replay dump (per draw: SPIR-V sha1, cbuffer bytes)  [tools/dsr_rdc_replay.py]
#   OUTDIR/constants.json  named: shaderbnd entry (e.g. FRPG_Fil_HDR_PBL_ColAdj.fpo), RDEF cbuffer/variable names,
#                          values; one row per unique pass                          [tools/dsr_rdc.py]
#   OUTDIR/glsl/           every DSR shader DXVK compiled, decompiled to GLSL (SPIRV-Cross) with its RDEF layout
# Restarts the game with RDC=1 (RenderDoc layer in the game process only) from saves/${SAVE:-firelink}.sl2 (SAVE=burg:
# Undead Burg bonfire 1012962, player at (0.1, -10.0, -59.2); warps reach the rest of m10_01); the replay runs
# through tools/q (8 GB) inside a second headless gamescope (qrenderdoc only has the xcb Qt platform).
set -e
H=${DSR_REF:-${DISTROBOX_HOST_HOME:-$HOME}/dsr-ref}
T=$(cd "$(dirname "$0")" && pwd)
RD=$H/tools/dl/renderdoc/renderdoc_1.46
PY=${PY:-$T/../.venv-tools/bin/python}
OUT=$(realpath -m "${1:?usage: $0 OUTDIR [cam args]}"); shift
mkdir -p "$OUT"
if [ "$1" = --replay-only ]; then
    CAP=$(realpath -m "$2")
else
    "$T/dsr-ref.sh" stop
    "$T/dsr-ref.sh" restore "${SAVE:-firelink}"
    distrobox-host-exec bash -c "rm -f $H/rdc/dsr_*.rdc $H/rdc/shaders/*"
    RDC=1 "$T/dsr-ref.sh" start
    "$T/dsr-ref.sh" load
    [ $# -gt 0 ] && "$T/dsr-ref.sh" cam "$@" > "$OUT/camera.json" || "$T/dsr-ref.sh" cam > "$OUT/camera.json"
    "$T/dsr-ref.sh" shot "$OUT/shot.png" >/dev/null
    "$T/dsr-ref.sh" in key f12
    # the capture is written asynchronously (~1 GB): wait until a file exists and its size stops changing
    CAP=$(distrobox-host-exec bash -c "for i in \$(seq 120); do f=\$(ls -t $H/rdc/dsr_*.rdc 2>/dev/null | head -1)
        if [ -n \"\$f\" ]; then s1=\$(stat -c %s \"\$f\"); sleep 2; [ \"\$s1\" = \"\$(stat -c %s \"\$f\")\" ] && { echo \$f; exit; }
        else sleep 1; fi; done")
    [ -n "$CAP" ] || { echo "dsr-rdc: no capture written" >&2; exit 1; }
    "$T/dsr-ref.sh" stop   # frees ~2 GB before the replay
fi
echo "dsr-rdc: capture $CAP"
# qrenderdoc gets its own config dir: on first run it shows a modal analytics prompt that blocks --python scripts.
# The config opts out of analytics and update checks.
C=$H/tools/rdconf
mkdir -p "$C/qrenderdoc"
[ -f "$C/qrenderdoc/UI.config" ] || printf '{\n "rdocConfigData": 1,\n "Analytics_TotalOptOut": true,\n "Analytics_ManualCheck": false,\n "CheckUpdate_AllowChecks": false,\n "Tips_HasSeenFirst": true\n}\n' > "$C/qrenderdoc/UI.config"
"$T/q" -m 8 distrobox-host-exec bash -c "cd '$OUT' && rm -f frame.json frame.json.err && RDC_IN='$CAP' RDC_OUT='$OUT/frame.json' \
    XDG_DATA_HOME='$C' XDG_CONFIG_HOME='$C' \
    timeout 1200 gamescope --backend headless -W 640 -H 480 -- '$RD/bin/qrenderdoc' --python '$T/dsr_rdc_replay.py' \
    > replay.log 2>&1; true"
[ -s "$OUT/frame.json" ] || { echo "dsr-rdc: replay failed:" >&2; cat "$OUT/frame.json.err" "$OUT/replay.log" 2>/dev/null | tail -20 >&2; exit 1; }
"$PY" "$T/dsr_rdc.py" "$OUT/frame.json" "$H/rdc/shaders" "$OUT/constants.json"
"$PY" "$T/dsr_rdc.py" glsl "$H/rdc/shaders" "$OUT/glsl"
