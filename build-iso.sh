#!/bin/bash
# Usage: ./build-iso.sh [--game DIR] [--out FILE] [--build DIR] [--jobs N] [--syms] [--no-queue] [--container]
#
# Builds the whole disc image from this checkout in one command: exports and packs the game data from your own copy
# of Dark Souls Remastered, compiles the ELF and assembles build/proto.iso (README.md steps 5 and 6 in one go). Later
# runs redo only what changed. Run it from anywhere; it always builds the checkout it lives in.
#
#   --game DIR   the folder that holds DarkSoulsRemastered.exe. Default: DSR_GAME / config.local.mk, else the Steam
#                auto-detect (tools/gamepath.py).
#   --out FILE   also copy the ISO here (e.g. ~/Downloads/DarkSoulsPS2.iso). It is always left at <build>/proto.iso.
#   --build DIR  build directory (default build/). A new one starts from scratch unless seeded first with
#                tools/newbuild DIR --from build.
#   --jobs N     parallel jobs (default: every core)
#   --syms       keep the ELF symbols on the disc (ISO_SYMS=1, for the PCSX2 debugger)
#   --no-queue   run make directly instead of through the job queue (tools/q)
#   --container  build in the Docker / Podman container (tools/build-iso.sh) even when the host has the toolchain
#
# Toolchain: the host's ps2dev (ps2/env.sh) with Python (Pillow + numpy), gcc and xorriso, as in README.md. When the
# host has no ps2dev but has a distrobox that does (DSR_DISTROBOX, default "Ubuntu"), the script re-runs itself inside
# it. With neither, it falls back to the container build of this checkout (tools/build-iso.sh, needs Docker or Podman).
# The ISO is made from your copy of the game: never share it.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GAME=; OUT=; BUILD=$ROOT/build; JOBS=; SYMS=; QUEUE=1; CONTAINER=
ARGS=("$@")

die() { echo "build-iso: $*" >&2; exit 1; }
say() { printf '\n== %s\n' "$*"; }
while [ $# -gt 0 ]; do
    case "$1" in
        --game) [ $# -ge 2 ] || die "--game needs a folder"; GAME=$2; shift 2 ;;
        --out) [ $# -ge 2 ] || die "--out needs a file"; OUT=$2; shift 2 ;;
        --build) [ $# -ge 2 ] || die "--build needs a folder"; BUILD=$2; shift 2 ;;
        --jobs) [ $# -ge 2 ] || die "--jobs needs a number"; JOBS=$2; shift 2 ;;
        --syms) SYMS=1; shift ;;
        --no-queue) QUEUE=; shift ;;
        --container) CONTAINER=1; shift ;;
        -h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option $1 (see --help)" ;;
    esac
done
[ -z "$JOBS" ] || [[ $JOBS =~ ^[1-9][0-9]*$ ]] || die "--jobs takes a whole number, not $JOBS"
[ -z "$OUT" ] || OUT=$(realpath -m -- "$OUT")
BUILD=$(realpath -m -- "$BUILD")
[ -z "$GAME" ] || GAME=$(realpath -- "$GAME") || die "no such folder: $GAME"

container_build() {
    local a=(--out "${OUT:-$BUILD/proto.iso}")
    [ -n "$GAME" ] && a+=(--game "$GAME")
    [ -n "$JOBS" ] && a+=(--jobs "$JOBS")
    [ "$BUILD" = "$ROOT/build" ] || die "--build only works with the host toolchain (the container uses build/)"
    { command -v docker >/dev/null && docker info >/dev/null 2>&1; } || command -v podman >/dev/null ||
        die "neither a ps2dev toolchain nor Docker / Podman found: install one of them (README.md)"
    exec "$ROOT/tools/build-iso.sh" "${a[@]}"
}
[ -n "$CONTAINER" ] && container_build

# --- toolchain: this host, else a distrobox that has it, else the container build ---
# (checked in a subshell: env.sh exports PS2DEV under this host's home, which a distrobox would inherit as a wrong path)
# shellcheck disable=SC1091
if ! ( . "$ROOT/ps2/env.sh"; command -v mips64r5900el-ps2-elf-gcc && [ -d "$GSKIT/lib" ] ) >/dev/null 2>&1; then
    BOX=${DSR_DISTROBOX:-Ubuntu}
    if [ ! -e /run/.containerenv ] && [ ! -e /.dockerenv ] && command -v distrobox >/dev/null &&
        distrobox list 2>/dev/null | awk -F'|' 'NR > 1 {gsub(/ /, "", $2); print $2}' | grep -qxF "$BOX"; then
        say "no ps2dev on this host: running inside distrobox $BOX"
        exec distrobox enter "$BOX" -- "$ROOT/build-iso.sh" "${ARGS[@]}"
    fi
    echo "build-iso: no ps2dev toolchain (looked in ${PS2DEV:-~/tools/ps2dev-2.0.0/ps2dev}; set PS2DEV, see README.md):" \
        "building in the Docker / Podman container instead" >&2
    container_build
fi
# shellcheck disable=SC1091
. "$ROOT/ps2/env.sh"

cd "$ROOT"
# Python with Pillow + numpy: the one the Makefile will use (PY from the environment / config.local.mk, else the repo's
# .venv, else python3; asked from config.mk itself, goal "help" skips its game check). A plain python3 without them
# gets a .venv; a configured PY without them is an error.
PY=$(printf 'include %s/config.mk\nhelp:\n\t@echo $(PY)\n' "$ROOT" | make -s --no-print-directory -f - help)
if ! $PY -c 'import PIL, numpy' 2>/dev/null; then
    [ "$PY" = python3 ] || die "$PY (PY in config.local.mk or the environment) lacks Pillow / numpy: pip install -r tools/requirements.txt"
    say "setting up .venv (tools/requirements.txt)"
    # a half-made .venv would be picked up by config.mk next time: remove it on failure
    python3 -m venv .venv && .venv/bin/pip install -q -r tools/requirements.txt ||
        { rm -rf .venv; die "could not set up .venv (python3-venv missing?); or set PY in config.local.mk to a Python with Pillow + numpy"; }
fi
# xorriso: tools/ps2-iso.sh's default (~/tools/xorriso), else the one on PATH
if [ -z "${XORRISO:-}" ] && [ ! -x "$HOME/tools/xorriso/bin/xorriso" ]; then
    command -v xorriso >/dev/null || die "xorriso not found: install it (apt/dnf install xorriso) or set XORRISO"
    export XORRISO=xorriso
fi

MAKE=(make -C ps2/proto "BUILD=$BUILD")
[ -n "$GAME" ] && MAKE+=("DSR_GAME=$GAME")
[ -n "$JOBS" ] && MAKE+=("JOBS=$JOBS")
[ -n "$QUEUE" ] && MAKE=("$ROOT/tools/q" "${MAKE[@]}")

T0=$(date +%s)
say "building game data and ELF into $BUILD (first run: long, several GB of RAM)"
"${MAKE[@]}"
say "assembling the ISO"
# the ISO needs the packs and the ELF from the step above: a separate make, so -j never starts it early
rm -f "$BUILD/proto.iso"
"${MAKE[@]}" ${SYMS:+ISO_SYMS=1} iso
[ -f "$BUILD/proto.iso" ] || die "the build finished without $BUILD/proto.iso"

ISO=$BUILD/proto.iso
if [ -n "$OUT" ] && [ "$OUT" != "$ISO" ]; then
    mkdir -p "$(dirname "$OUT")"
    cp -f "$ISO" "$OUT"
    ISO=$OUT
fi
say "done in $(( ($(date +%s) - T0) / 60 )) min: $ISO ($(du -h "$ISO" | cut -f1)), from $(git rev-parse --short HEAD 2>/dev/null || echo '?')$(git diff --quiet HEAD 2>/dev/null || echo ' + local changes')"
echo "Open it in PCSX2 (System > Start File). It is made from your copy of the game: never share it."
