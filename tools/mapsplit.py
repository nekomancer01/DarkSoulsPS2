"""Split a DSM5 map.bin (tools/export_map.py) for world streaming (src/world.c, map.c map_block_init):

  mapsplit.py <map.bin> <maptab.bin> <mapres.bin>

maptab.bin = the 80-byte header + map.bin from the chunk table offset on (chunk table, textures, navmesh, fog section,
cell table: what a "lite" block needs to draw its streamed near cells). mapres.bin = map.bin up to the chunk table
(the header again, then the resident chunks' vertex streams; chunk records address them from map.bin's start), loaded
only while the block is "full". Both are packed raw (tools/pack.py raw:) so world.c reads them in place.
maptab.bin leaves out (TECH_DEBT world-5: lite sets count against the near-cell arena):
- the navmesh (navv_off .. fog_off; header nav counts 0): the runtime reads it only in map_floor, player.c's fallback
  without collision.bin, and every world block has collision;
- mip 0 pixels of textures that have a mip 1: they move to the end of mapres.bin and the entry's pix[0] becomes
  RES_FLAG | offset in mapres.bin (texcache.c tc_res resolves it while the block is full; a lite block draws mip 1).
Every other offset into maptab (header sections, texture CLUT / mip 1 / near pixels) moves down by the bytes cut
before it."""
import struct, sys

HDR = 80
PSM_T4 = 0x14
RES_FLAG = 0x80000000  # texture pix[0] in mapres.bin (texcache.c TC_RES)


def main():
    src, tab_path, res_path = sys.argv[1:4]
    b = open(src, "rb").read()
    assert b[:4] == b"DSM5", f"{src}: not a DSM5 map.bin (world streaming needs STREAM=1)"
    chunk_off, tex_off, navv_off, navt_off = struct.unpack_from("<4I", b, 36)
    ntex = struct.unpack_from("<I", b, 8)[0]
    fog_off, cell_off = struct.unpack_from("<2I", b, 72)
    assert HDR <= chunk_off < len(b) and chunk_off % 16 == 0
    assert chunk_off <= tex_off <= navv_off <= navt_off <= fog_off < cell_off, "section order"
    cut = (fog_off - navv_off) & ~63  # keeps the later sections' alignment
    gone = {navv_off: cut} if cut else {}  # offset -> bytes left out of maptab
    l0 = {}  # texture -> (offset in map.bin, bytes)
    for i in range(ntex):
        e = tex_off + 32 * i
        w, h, psm, cpsm, levels = struct.unpack_from("<2H3B", b, e)
        p0, p1 = struct.unpack_from("<2I", b, e + 16)
        if levels < 2 or not tex_off <= p0 < navv_off:
            continue
        n0 = w * h // (2 if psm == PSM_T4 else 1)
        assert n0 % 16 == 0 and gone.get(p0, n0) == n0, f"texture {i}: mip 0 at {p0} ({n0} bytes)"
        gone[p0] = n0
        l0[i] = (p0, n0)
    ranges = sorted(gone.items())
    for (a0, n0), (a1, _) in zip(ranges, ranges[1:]):
        assert a0 + n0 <= a1, f"overlapping cuts at {a0}"

    def moved(o):  # maptab offset (from map.bin's start, as the runtime resolves it) of map.bin offset o
        d = 0
        for a0, n0 in ranges:
            if a0 + n0 <= o:
                d += n0
            else:
                assert o <= a0, f"offset {o} inside a cut"
                break
        return o - d

    res = bytearray(b[:chunk_off])
    at = {}
    for a0, n0 in ranges:
        if a0 != navv_off or not cut:
            at[a0] = len(res)
            res += b[a0:a0 + n0]
    body, pos = bytearray(), chunk_off
    for a0, n0 in ranges:
        body += b[pos:a0]
        pos = a0 + n0
    body += b[pos:]
    tab = bytearray(b[:HDR]) + body
    struct.pack_into("<2I", tab, 12, 0, 0)  # nav_verts, nav_tris
    struct.pack_into("<4I", tab, 36, chunk_off, tex_off, moved(navv_off), moved(navv_off))
    struct.pack_into("<2I", tab, 72, moved(fog_off), moved(cell_off))
    for i in range(ntex):  # the table itself lies before every cut
        e = HDR + tex_off - chunk_off + 32 * i
        clut, p0, p1 = struct.unpack_from("<3I", b, tex_off + 32 * i + 12)
        near = struct.unpack_from("<I", b, tex_off + 32 * i + 28)[0]
        struct.pack_into("<3I", tab, e + 12, moved(clut), RES_FLAG | at[l0[i][0]] if i in l0 else moved(p0), moved(p1))
        if near:
            struct.pack_into("<I", tab, e + 28, moved(near))
    with open(tab_path, "wb") as f:
        f.write(tab)
    with open(res_path, "wb") as f:
        f.write(res)
    cut_l0 = sum(n0 for a0, n0 in ranges if a0 != navv_off or not cut)
    print(f"{src}: maptab {len(tab)} bytes (navmesh {cut}, mip 0 of {len(l0)} textures {cut_l0} moved out), "
          f"mapres {len(res)} bytes")


if __name__ == "__main__":
    main()
