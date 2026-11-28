#!/usr/bin/env python3
"""A small synthetic world block for tools/ci/test_lz4items.py (no game data): maptab.disc.bin (DSM5 header, 7 chunk
records, 2 textures, a cell table of 7 entries), mapres.bin (the chunk streams, two resident mips of a 256 x 384 T8
texture, gaps) and cells.disc.bin (5 near parts placed by tools/discalign.py). Deterministic. Covers a part and a
chunk item in the 65153..65536 B band (stored raw), an item over 64 KB (two frames), an incompressible part, a part
of 12 bytes, two entries naming one blob and an empty entry.
  make_block.py <out dir>"""
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", ".."))
import discalign  # noqa: E402

HDR, CHUNK, CELL, TEXE, TC_RES = 80, 64, 48, 32, 0x80000000


def noise(rng, n, period=4096):
    """compressible bytes: a random period repeated with sparse changes"""
    base = rng.randbytes(period)
    b = bytearray((base * (n // period + 1))[:n])
    for _ in range(n // 64):
        b[rng.randrange(n)] = rng.randrange(256)
    return bytes(b)


def build():
    """-> (maptab.disc.bin, mapres.bin, cells.disc.bin)"""
    rng = random.Random(1128)
    res = bytearray(noise(rng, 3000))  # a gap: not an item
    chunks, spans = [], [5, 20, 33, 2, 5430, 7, 3]  # triangles (x 200 for four of them); streams are tris * 12 bytes + 16
    for k, tris in enumerate(spans):
        tris = tris * (200 if k in (0, 1, 2, 5) else 1)
        res += b"\0" * (-len(res) % 16)
        lo = len(res)
        pos = lo + 16
        res += noise(rng, 16 + tris * 12, 512)
        # (flags, tris, nv, pos, ?, col): nv = 0 -> the stream ends at col + tris * 12
        chunks.append((k, 0, tris, 0, pos, 0, pos))
    res += b"\0" * (-len(res) % 16)
    res += rng.randbytes(1000) + noise(rng, 1500)  # a gap with a clut-like start
    res += b"\0" * (-len(res) % 16)
    p0 = len(res)
    res += noise(rng, 256 * 384, 8192)  # level 0: 98304 B = two frames
    p1 = len(res)
    res += noise(rng, 128 * 192, 2048)  # level 1: 24576 B
    res += noise(rng, 4000)  # tail gap
    tex = [struct.pack("<2HB", 256, 384, 0x13).ljust(16, b"\0") + struct.pack("<2I", TC_RES | p0, TC_RES | p1)
           + bytes(8),
           struct.pack("<2HB", 128, 128, 0x14).ljust(16, b"\0") + struct.pack("<2I", 0, 0) + bytes(8)]
    cells = bytearray()
    sizes = [20000, 40000, 65200, 3000, 12]
    parts = []
    for i, n in enumerate(sizes):
        cells += b"\0" * (-len(cells) % 64)
        parts.append((len(cells), n))
        cells += rng.randbytes(n) if i == 3 else noise(rng, n, 1024)
    # seven entries: the five parts, a repeat of part 1, an empty one (the last three have no chunks)
    rows = parts + [parts[1], (0, 0)]
    first, count = [0, 3, 5, 6, 7, 7, 7], [3, 2, 1, 1, 0, 0, 0]
    ctab = struct.pack("<4I", len(rows), 3, 0, 0)
    for i, (off, n) in enumerate(rows):
        ctab += bytes(24) + struct.pack("<5I", off, n, 0, first[i], count[i]) + bytes(48 - 44)
    chunk_off = HDR
    tex_off = chunk_off + CHUNK * len(chunks)
    cell_off = tex_off + TEXE * len(tex)
    hdr = bytearray(HDR)
    hdr[0:4] = b"DSM5"
    struct.pack_into("<2I", hdr, 4, len(chunks), len(tex))
    struct.pack_into("<2I", hdr, 36, chunk_off, tex_off)
    struct.pack_into("<I", hdr, 76, cell_off)
    ctb = b"".join(struct.pack("<iIHHIII", *c).ljust(CHUNK, b"\0") for c in chunks)
    tab = bytes(hdr) + ctb + b"".join(tex) + ctab
    # the cells as discalign.layout places them (the disc layout), as the export does
    tab, cells, _, _ = discalign.layout(tab, bytes(cells))
    return tab, bytes(res), cells


if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, b in zip(("maptab.disc.bin", "mapres.bin", "cells.disc.bin"), build()):
        open(os.path.join(out, name), "wb").write(b)
