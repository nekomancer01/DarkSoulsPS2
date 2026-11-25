#!/bin/bash
# Usage: tools/pcsx2-headless/build.sh [--src DIR] [--build DIR]     env: JOBS (default 8), CC / CXX
#
# Builds pcsx2-dsrtest, the headless PCSX2 test runner (tools/pcsx2-headless/Main.cpp, derived from upstream
# pcsx2-gsrunner) against a pinned PCSX2 source tree, without Qt. Every automated test run (tools/pcsx2-run.sh with a
# shot path, tools/test-run.py, tools/test-gsdump.py, ...) starts this binary instead of the Flatpak.
#   --src DIR    PCSX2 checkout (default tools/ext/pcsx2-src; cloned at $PCSX2_TAG when missing)
#   --build DIR  CMake build dir (default tools/ext/pcsx2-build); the binary lands in DIR/bin/pcsx2-dsrtest
# Steps: source (git clone, needs network once), deps (PCSX2's own build-dependencies-runner.sh downloads and builds
# SDL3, shaderc, freetype, ... into tools/ext/pcsx2-deps, ~20-40 min once; stamp .stamp-$PCSX2_TAG), frontend (copies
# Main.cpp + CMakeLists.txt into <src>/pcsx2-dsrtest), configure, build. A second run with nothing changed is a ninja
# no-op. Compiler: clang / clang++ when on PATH (what PCSX2's CI uses), else gcc / g++; CC / CXX override.
# PCSX2 is GPLv3 (as this repo); tools/ext/ is git-ignored, only tools/pcsx2-headless/ is tracked.
# Run it through the job queue: tools/q -m 6 tools/pcsx2-headless/build.sh
set -u
PCSX2_TAG=v2.8.2
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
EXT=$ROOT/tools/ext
SRC=$EXT/pcsx2-src
BUILD=$EXT/pcsx2-build
DEPS=$EXT/pcsx2-deps
JOBS=${JOBS:-8}
while [ $# -gt 0 ]; do
    case "$1" in
        --src) SRC=$2; shift 2 ;;
        --build) BUILD=$2; shift 2 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "build.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
fail() { echo "build.sh: step $1 failed${2:+: $2}" >&2; exit 1; }

# host tools the dependency script and the build need
missing=
for t in git curl patch pkg-config nasm cmake ninja; do command -v "$t" >/dev/null || missing="$missing $t"; done
if [ -z "${CXX:-}" ] && ! command -v clang++ >/dev/null && ! command -v g++ >/dev/null; then missing="$missing c++-compiler"; fi
[ -z "$missing" ] || { echo "build.sh: missing host tools:$missing (e.g. sudo apt install$missing)" >&2; exit 1; }
if command -v clang >/dev/null && command -v clang++ >/dev/null; then CC=${CC:-clang} CXX=${CXX:-clang++}
else CC=${CC:-gcc} CXX=${CXX:-g++}; fi
export CC CXX

t0=$(date +%s)
# source: a shallow clone at the pinned tag
if [ "$(git -C "$SRC" describe --tags --exact-match 2>/dev/null)" != "$PCSX2_TAG" ]; then
    [ -e "$SRC" ] && fail source "$SRC exists but is not at $PCSX2_TAG (move it away or pass --src)"
    echo "== source: cloning PCSX2 $PCSX2_TAG -> $SRC"
    git clone --depth 1 --branch "$PCSX2_TAG" https://github.com/PCSX2/pcsx2.git "$SRC" || fail source
    git -C "$SRC" submodule update --init --depth 1 || fail source "submodules"
fi

# deps: PCSX2's runner dependency script with our job count, run once per tag
if [ ! -f "$DEPS/.stamp-$PCSX2_TAG" ]; then
    S=$SRC/.github/workflows/scripts/linux
    [ -f "$S/build-dependencies-runner.sh" ] || fail deps "no $S/build-dependencies-runner.sh"
    # a sibling file in the same dir: the script finds ../common/shaderc-changes.patch through its own SCRIPTDIR
    sed 's/NPROCS="$(getconf _NPROCESSORS_ONLN)"/NPROCS="${JOBS:-8}"/' "$S/build-dependencies-runner.sh" \
        > "$S/build-dependencies-dsrtest.sh" || fail deps
    grep -q 'NPROCS="${JOBS:-8}"' "$S/build-dependencies-dsrtest.sh" || fail deps "NPROCS line not found"
    echo "== deps: building into $DEPS (log $EXT/pcsx2-deps.log)"
    mkdir -p "$EXT/pcsx2-deps-work"
    (cd "$EXT/pcsx2-deps-work" && JOBS=$JOBS bash "$S/build-dependencies-dsrtest.sh" "$DEPS") > "$EXT/pcsx2-deps.log" 2>&1 \
        || { tail -20 "$EXT/pcsx2-deps.log" >&2; fail deps "see $EXT/pcsx2-deps.log"; }
    rm -rf "$EXT/pcsx2-deps-work"
    touch "$DEPS/.stamp-$PCSX2_TAG"
fi

# frontend: our two files into the checkout, one add_subdirectory line
mkdir -p "$SRC/pcsx2-dsrtest"
rsync -a "$HERE/Main.cpp" "$HERE/CMakeLists.txt" "$SRC/pcsx2-dsrtest/" || fail frontend
grep -q '^add_subdirectory(pcsx2-dsrtest)' "$SRC/CMakeLists.txt" || echo 'add_subdirectory(pcsx2-dsrtest)' >> "$SRC/CMakeLists.txt" \
    || fail frontend

# configure (once; ninja re-runs cmake itself when a CMakeLists changes). A cache made for another source dir
# (the retired ~/dsr-ps2 checkout) is deleted first.
if [ -f "$BUILD/CMakeCache.txt" ]; then
    home=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt")
    cxx=$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BUILD/CMakeCache.txt")
    if [ "$home" != "$(cd "$SRC" && pwd)" ] || [ "$(command -v "$CXX")" != "$cxx" ] || [ ! -f "$BUILD/build.ninja" ]; then
        echo "== configure: stale cache ($home, $cxx) removed"
        rm -rf "$BUILD"
    fi
fi
if [ ! -f "$BUILD/build.ninja" ]; then
    echo "== configure: $BUILD ($CC / $CXX)"
    LD=; case "$CXX" in *clang*) command -v ld.lld >/dev/null && LD="-DCMAKE_EXE_LINKER_FLAGS_INIT=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS_INIT=-fuse-ld=lld" ;; esac
    cmake -G Ninja -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DENABLE_QT_UI=OFF -DENABLE_GSRUNNER=OFF \
        -DENABLE_TESTS=OFF -DWAYLAND_API=OFF -DCMAKE_PREFIX_PATH="$DEPS" \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" $LD > "$EXT/pcsx2-configure.log" 2>&1 \
        || { tail -20 "$EXT/pcsx2-configure.log" >&2; fail configure "see $EXT/pcsx2-configure.log"; }
fi

echo "== build: ninja -j $JOBS pcsx2-dsrtest"
ninja -C "$BUILD" -j "$JOBS" pcsx2-dsrtest > "$EXT/pcsx2-build.log" 2>&1 \
    || { grep -E 'error|FAILED' "$EXT/pcsx2-build.log" | head -20 >&2; fail build "see $EXT/pcsx2-build.log"; }
[ -x "$BUILD/bin/pcsx2-dsrtest" ] || fail output "no $BUILD/bin/pcsx2-dsrtest"
[ -f "$BUILD/bin/resources/fonts/Roboto-Regular.ttf" ] || fail output "no resources next to the binary"
echo "== done in $(( $(date +%s) - t0 )) s: $BUILD/bin/pcsx2-dsrtest"
