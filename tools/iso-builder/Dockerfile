# Build environment for tools/build-iso.sh: the ps2dev toolchain the README and CI use (ps2dev/ps2dev:v2.0.0, Alpine)
# plus what the exporters and tools/ps2-iso.sh need on the host side. No game data is ever baked into the image: the
# player's game folder is mounted read-only at /game when the build runs.
#   python3 + numpy + Pillow   exporters (tools/requirements.txt)
#   gcc + g++ + musl-dev       native helpers the exporters compile on first use (tools/native/*.c; meshoptimizer's
#                              simplifier in tools/native/meshopt is C++: breakables in objects.bin, the far horizon)
#   mpg123-libs                MP3 decoding of DSR's FMOD sound banks (tools/fsb4.py, libmpg123 through ctypes)
#   xorriso                    disc image (tools/ps2-iso.sh)
#   bash make coreutils util-linux findutils gawk   tools/q (flock), tools/ps2-iso.sh (stat -c, realpath)
# Fully qualified so Podman never has to resolve a short name (Fedora's enforcing short-name mode errors without a TTY).
# tools/build-iso.sh pulls this base image itself before building on top of it.
FROM docker.io/ps2dev/ps2dev:v2.0.0
RUN apk add --no-cache bash make coreutils util-linux findutils gawk python3 py3-numpy py3-pillow gcc g++ musl-dev mpg123-libs xorriso
ENV PATH=/usr/local/ps2dev/bin:/usr/local/ps2dev/ee/bin:/usr/local/ps2dev/iop/bin:/usr/local/ps2dev/dvp/bin:/usr/local/ps2dev/ps2sdk/bin:$PATH \
    XORRISO=xorriso
WORKDIR /src
