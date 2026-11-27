#!/usr/bin/env python3
"""A synthetic DSM5 map.bin (no game data) for tools/ci/test_mapsect.py: header, resident streams, 2 chunk records, one
T4 texture with three mip levels, a navmesh big enough that tools/mapsplit.py cuts it, a fog section and a cell table
of one cell. Layout as tools/export_map.py writes it (map_i.h Header).
  make_map_dsm5.py <out map.bin> [<out hzn.bin>]"""
import struct
import sys

HDR = 80


def build():
    """-> (map.bin bytes, hzn.bin bytes of a horizon for its one cell)"""
    blob = bytearray(b"\x5a" * 96)  # resident vertex streams (16-byte aligned)
    chunk_off = HDR + len(blob)
    blob += struct.pack("<iII3I6f4f", 0, 1, 0, 0, 0, 0, *[0.0] * 6, *[0.0] * 3, 1.0) * 2
    tex_off = HDR + len(blob)
    clut, p0 = tex_off + 32 + 0, tex_off + 32 + 64
    p1 = p0 + 64 * 64 // 2
    blob += struct.pack("<2H4B4x3I4xI", 64, 64, 0x14, 0x14, 3, 0, clut, p0, p1, 0)  # 32-byte entry (texmap.py)
    blob += b"\x11" * 64 + b"\x22" * (64 * 64 // 2) + b"\x33" * 512 + b"\x44" * 144  # CLUT, mip 0, mip 1, mip 2 / 3
    navv_off = HDR + len(blob)
    blob += b"".join(struct.pack("<3f", i, i + 1, i + 2) for i in range(6))
    navt_off = HDR + len(blob)
    blob += b"".join(struct.pack("<3H", i, i + 1, i + 2) for i in range(4))
    blob += b"\0" * (-len(blob) % 4)
    fog_off = HDR + len(blob)
    blob += struct.pack("<4I", 0, 0, 0, 0) + b"\0" * 32
    blob += b"\0" * (-(len(blob) + HDR) % 16)
    cell_off = HDR + len(blob)
    blob += struct.pack("<4I", 1, 3, 0, 0) + struct.pack("<6f6I", *[0.0] * 6, 0, 16, 0, 0, 1, 0)
    hdr = struct.pack("<4s4I4f4I", b"DSM5", 2, 1, 6, 4, 0.0, 0.0, 0.0, 0.0, chunk_off, tex_off, navv_off, navt_off)
    hdr += struct.pack("<3f4B", 10.0, 100.0, 1.0, 1, 2, 3, 0x80) + struct.pack("<4B", 0, 0, 0, 0)
    hdr += struct.pack("<2I", fog_off, cell_off)
    hdr += b"\0" * (HDR - len(hdr))
    hzn = b"HZN1" + struct.pack("<2I", 0, 1) + bytes(range(40))  # word 2: the cell count of the map's table
    return bytes(hdr + blob), hzn


if __name__ == "__main__":
    m, h = build()
    open(sys.argv[1], "wb").write(m)
    if len(sys.argv) > 2:
        open(sys.argv[2], "wb").write(h)
