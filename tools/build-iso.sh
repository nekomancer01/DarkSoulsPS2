#!/bin/bash
# Usage: build-iso.sh [--game DIR] [--out FILE] [--ref REF] [--work DIR] [--engine auto|docker|podman|native] [--jobs N]
#                     [--build DIR] [--syms]
#
# One-step build of a playable disc image of the port from the player's own Dark Souls Remastered: what the project's
# web page (tools/pages/) hands out. Everything runs on this machine. The game folder is mounted read-only and never
# leaves the PC; nothing is uploaded anywhere. The only downloads are the port's source (codeload.github.com) and,
# the first time, the build container (ps2dev/ps2dev from Docker Hub plus Alpine packages, tools/iso-builder/Dockerfile).
#
#   --game DIR   the folder that holds DarkSoulsRemastered.exe. Default: $DSR_GAME, else tools/gamepath.py's Steam
#                auto-detect (needs python3 on the host), else ask.
#   --out FILE   where the ISO goes (default ./DarkSoulsPS2.iso)
#   --ref REF    which source to build: a commit, branch or tag of the repo (default: the commit the web page was
#                built from, or main). Ignored when the script runs from a checkout: the checkout is built.
#   --work DIR   source + build cache (default ~/.cache/darksoulsps2). The build dir is kept between runs, so an
#                update re-exports only what the new source changes; delete DIR to start over.
#   --engine     container engine (default: docker if it answers, else podman). native = no container: the host
#                toolchain from README.md (PS2DEV, Python with Pillow + numpy, gcc, xorriso).
#   --jobs N     parallel jobs (default: every core). The first export peaks at ~6 GB RAM.
#   --build DIR  build directory (default: build/ of the checkout, else WORK/build)
#   --syms       keep the ELF symbols on the disc (ISO_SYMS=1, for the PCSX2 debugger)
#
# Steps: fetch the source (unless run from a checkout) -> pull ps2dev/ps2dev and build the builder image on it once ->
# make -C ps2/proto (exports the game data into the build dir, builds the ELF) -> make iso (tools/ps2-iso.sh) -> copy build/proto.iso to --out.
set -euo pipefail

REPO=${DSR_REPO:-nekomancer01/DarkSoulsPS2}
REF=${DSR_REF:-main}  # the Pages workflow (tools/pages/build.py) pins this to the commit the page was built from
GAME=${DSR_GAME:-}
OUT=$PWD/DarkSoulsPS2.iso
WORK=${XDG_CACHE_HOME:-$HOME/.cache}/darksoulsps2
ENGINE=auto
JOBS=
REF_SET=
BUILD_SET=
SYMS=

die() { echo "error: $*" >&2; exit 1; }
say() { printf '\n== %s\n' "$*"; }
while [ $# -gt 0 ]; do
    case "$1" in
        --game) GAME=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        --ref) REF=$2; REF_SET=1; shift 2 ;;
        --work) WORK=$2; shift 2 ;;
        --engine) ENGINE=$2; shift 2 ;;
        --jobs) JOBS=$2; shift 2 ;;
        --build) BUILD_SET=$2; shift 2 ;;
        --syms) SYMS=1; shift ;;
        -h|--help) awk 'NR > 1 && !/^#/ {exit} NR > 1' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option $1 (see --help)" ;;
    esac
done
OUT=$(realpath -m -- "$OUT")
mkdir -p "$WORK"
WORK=$(realpath -- "$WORK")

# --- source: this checkout, or REF downloaded into WORK/src (replaced on every run; the build dir is kept) ---
HERE=  # empty when piped into bash: then always download
if [ -f "${BASH_SOURCE[0]:-}" ]; then HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); fi
if [ -n "$HERE" ] && [ -f "$HERE/../ps2/proto/Makefile" ] && [ -z "$REF_SET" ]; then
    SRC=$(cd "$HERE/.." && pwd)
    BUILD=$SRC/build
    say "source: this checkout ($SRC)"
else
    SRC=$WORK/src
    BUILD=$WORK/build
    say "source: $REPO @ $REF"
    TGZ=$WORK/src.tar.gz
    URL=https://codeload.github.com/$REPO/tar.gz/$REF
    if command -v curl >/dev/null; then curl -fL --retry 3 -o "$TGZ" "$URL"
    elif command -v wget >/dev/null; then wget -O "$TGZ" "$URL"
    else die "need curl or wget to download the source"; fi
    rm -rf "$SRC"; mkdir -p "$SRC"
    tar -xzf "$TGZ" -C "$SRC" --strip-components=1
    rm -f "$TGZ"
    echo "$REF" > "$SRC/.source-ref"
fi
[ -f "$SRC/ps2/proto/Makefile" ] || die "no ps2/proto/Makefile in $SRC"
[ -z "$BUILD_SET" ] || BUILD=$BUILD_SET
mkdir -p "$BUILD"
BUILD=$(realpath -- "$BUILD")

# --- game folder ---
if [ -z "$GAME" ] && command -v python3 >/dev/null; then
    GAME=$(python3 -I "$SRC/tools/gamepath.py" --quiet || true)
fi
if [ -z "$GAME" ]; then
    [ -t 0 ] || die "Dark Souls Remastered not found: pass --game \"/path/to/DARK SOULS REMASTERED\""
    read -rp "Folder that holds DarkSoulsRemastered.exe: " GAME
fi
GAME=$(realpath -- "$GAME") || die "no such folder: $GAME"
[ -f "$GAME/DarkSoulsRemastered.exe" ] || die "$GAME has no DarkSoulsRemastered.exe (pick the game's install folder)"
say "game: $GAME (read only)"

MAKE_ARGS=(-C ps2/proto)
[ -n "$JOBS" ] && MAKE_ARGS+=("JOBS=$JOBS")
ISO_ARGS=()
[ -n "$SYMS" ] && ISO_ARGS+=(ISO_SYMS=1)

# --- engine ---
if [ "$ENGINE" = auto ]; then
    if command -v docker >/dev/null && docker info >/dev/null 2>&1; then ENGINE=docker
    elif command -v podman >/dev/null; then ENGINE=podman
    else die "no container engine: install Docker or Podman, or use --engine native (README.md toolchain)"; fi
fi

if [ "$ENGINE" = native ]; then
    say "building with the host toolchain"
    cd "$SRC"
    # shellcheck disable=SC1091
    . ps2/env.sh
    if [ -z "${XORRISO:-}" ] && command -v xorriso >/dev/null; then export XORRISO=xorriso; fi
    [ -x "$SRC/.venv/bin/python" ] || { python3 -m venv .venv && .venv/bin/pip install -q -r tools/requirements.txt; }
    make "${MAKE_ARGS[@]}" "DSR_GAME=$GAME" "BUILD=$BUILD"
    rm -f "$BUILD/proto.iso"
    make "${MAKE_ARGS[@]}" "DSR_GAME=$GAME" "BUILD=$BUILD" ${ISO_ARGS[@]+"${ISO_ARGS[@]}"} iso
else
    command -v "$ENGINE" >/dev/null || die "$ENGINE not found"
    # the image is named after its Dockerfile's hash: a changed Dockerfile builds a new one, an unchanged one is reused
    DF=$SRC/tools/iso-builder/Dockerfile
    IMG=darksoulsps2-builder:$(sha256sum "$DF" | cut -c1-12)
    if ! "$ENGINE" image inspect "$IMG" >/dev/null 2>&1; then
        # the ps2dev toolchain image the Dockerfile builds on (pinned tag, so later runs reuse it), pulled as its own step
        # so a registry or network failure says which image it was
        BASE=$(awk 'toupper($1) == "FROM" {print $2; exit}' "$DF")
        say "pulling the ps2dev toolchain ($BASE, first run only, ~1 GB download)"
        "$ENGINE" pull "$BASE" || die "could not pull $BASE (network, or $ENGINE not allowed to reach docker.io?)"
        say "building the build container on it (first run only)"
        "$ENGINE" build -t "$IMG" -f "$DF" "$SRC/tools/iso-builder"
    fi
    say "building the port (first run: several minutes and several GB of RAM)"
    # the image's own python3, not the host's: config.local.mk and a repo .venv point at host paths (config.mk)
    RUN=(run --rm --init --security-opt label=disable -e HOME=/tmp -e "DSR_ALLOW_UNSUPPORTED=${DSR_ALLOW_UNSUPPORTED:-}"
         -e DSR_LOCAL_CONFIG=0 -e PY=python3
         -e "DSR_AUDIO_JOBS=${DSR_AUDIO_JOBS:-$JOBS}"
         -v "$SRC:/src" -v "$BUILD:/build" -v "$GAME:/game:ro" -w /src)
    # rootful Docker would leave root-owned files in WORK; rootless Podman already maps root to this user
    [ "$ENGINE" = docker ] && RUN+=(--user "$(id -u):$(id -g)")
    [ -t 0 ] && [ -t 1 ] && RUN+=(-it)
    rm -f "$BUILD/proto.iso"
    [ -n "$SYMS" ] && RUN+=(-e ISO_SYMS=1)
    "$ENGINE" "${RUN[@]}" "$IMG" \
        bash -c 'make "$@" DSR_GAME=/game BUILD=/build && make "$@" DSR_GAME=/game BUILD=/build ${ISO_SYMS:+ISO_SYMS=1} iso' \
        make "${MAKE_ARGS[@]}"
fi

[ -f "$BUILD/proto.iso" ] || die "the build finished without $BUILD/proto.iso"
if [ "$OUT" != "$BUILD/proto.iso" ]; then  # the repo-root build-iso.sh may ask for the ISO right where it is built
    mkdir -p "$(dirname "$OUT")"
    cp -f "$BUILD/proto.iso" "$OUT"
fi
say "done: $OUT ($(du -h "$OUT" | cut -f1))"
echo "Open it in PCSX2 (System > Start File, or drag it onto the window). No BIOS settings or HostFs needed."
echo "Never share the ISO or $BUILD: they are made from your copy of the game."
