"""File names of the native helper libraries the exporters build on first use (tools/native/*.so, tools/mapx,
tools/adpcm), per C library: the container build (tools/build-iso.sh, Alpine: musl) mounts the same checkout as a host
build (glibc), and a library one of them built does not load in the other. The newer one also looked up to date to
the other's mtime check, so the other's exports failed or silently fell back to slower paths. glibc keeps the plain
name (existing builds stay cached); musl gets ".musl.so".
    so(path_without_suffix) -> path + ".so" or ".musl.so"      TAG: "" or "-musl" (host executables, ps2/proto/Makefile)"""
import glob, platform

MUSL = platform.libc_ver()[0] != "glibc" and bool(glob.glob("/lib/ld-musl-*.so.1"))
TAG = "-musl" if MUSL else ""


def so(base):
    return base + (".musl.so" if MUSL else ".so")
