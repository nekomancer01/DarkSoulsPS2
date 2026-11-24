"""map.bin and the files split from it as chunk files (Polaris 4.8 step 4, stage 1): each file the runtime reads is a
chunk file (tools/chunkfile.py) holding a VU1L chunk (the VU1 layout stamp, as world.bin / dg.bin carry it) and one
payload chunk with the bytes the exporters write today, unchanged. Every offset inside (header sections, chunk records'
streams, texture pixels, cell blobs, the horizon) stays relative to the payload's start, so the loaders take the
payload as their base (src/map.c, mapblock.c, world.c, mapstream.c: chunk_open / chunk_open_part, then the payload
offset). Stage 2 (sections with their own versions: MHDR, CHKT, STRM, TEXS, NAVV, NAVT, FOGS, CELT, HZNB) is
TECH_DEBT core-1's follow-up.

  kind     file id  payload  align   file
  map      MAPF     MAPD     128     map.bin (the boot area of a build without world blocks: loaded whole)
  maptab   MTAB     TABD     2048    maptab.bin (a world block's tables; world.c reads it in three pieces)
  mapres   MRES     RESD     2048    mapres.bin (resident streams + cut mips, read by offset)
  maptex   MTEX     TEXD     2048    maptex.bin (an MSB's shared textures)
  cells    CELF     CBLB     2048    cells.bin (near parts, read by offset; tools/discalign.py places them on the
                                     drive's 32 KB blocks counting the payload's PAYLOAD bytes before them)

The payload starts at a fixed offset (PAYLOAD[kind]: the header, the VU1L chunk and a PAD chunk before it), so a
loader that reads a raw entry by offset finds it from the first PAYLOAD bytes (chunk_open_part) and discalign.py can
place parts before the wrap. Usage (tools/pack.py does it for entries "name=[raw:]chunk:<kind>:<file>"):
  mapwrap.py <kind> <in> <out>"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import chunkfile as chunk  # noqa: E402

KINDS = {"map": (b"MAPF", b"MAPD", 128), "maptab": (b"MTAB", b"TABD", 2048), "mapres": (b"MRES", b"RESD", 2048),
         "maptex": (b"MTEX", b"TEXD", 2048), "cells": (b"CELF", b"CBLB", 2048)}
VERSION = 1  # the payload's layout: the exporters' bytes as of main 02d7984 (DSM5 / cells.bin / mapsplit / discalign)
PAYLOAD = {k: v[2] for k, v in KINDS.items()}  # the payload's offset in the wrapped file
# MAP_CHUNKS=0 (environment, make MAP_CHUNKS=0): tools/pack.py stores the files as before (the loaders read both)
ON = os.environ.get("MAP_CHUNKS", "1") != "0"


def wrap(kind, data):
    fid, pid, align = KINDS[kind]
    b = chunk.pack(fid, [chunk.vu1_chunk(), (pid, VERSION, data, align)])
    assert b[align - 16:align - 12] == pid, f"mapwrap: {kind}: payload not at {align}"
    return b


def unwrap(data, kind=None):
    """the payload of a wrapped file (or the bytes as they are when the file is not a chunk file)"""
    if data[:4] != chunk.MAGIC:
        return data
    fid = data[4:8]
    k = kind or next((k for k, v in KINDS.items() if v[0] == fid), None)
    if k is None:  # another chunk file (world.bin, dg.bin ...): as it is
        return data
    o = PAYLOAD[k]
    n = struct.unpack_from("<I", data, o - 8)[0]
    return data[o:o + n]


if __name__ == "__main__":
    kind, src, dst = sys.argv[1:4]
    out = wrap(kind, open(src, "rb").read())
    with open(dst + ".tmp", "wb") as f:
        f.write(out)
    os.replace(dst + ".tmp", dst)
