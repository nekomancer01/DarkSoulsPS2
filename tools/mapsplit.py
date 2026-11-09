"""Split a DSM5 map.bin (tools/export_map.py) for world streaming (src/world.c, map.c map_block_init):

  mapsplit.py <map.bin> <maptab.bin> <mapres.bin> [hzn.bin]

maptab.bin = the 80-byte header + map.bin from the chunk table offset on (chunk table, textures, navmesh, fog section,
cell table: what a "lite" block needs to draw its streamed near cells). mapres.bin = map.bin up to the chunk table
(the header again, then the resident chunks' vertex streams; chunk records address them from map.bin's start), loaded
only while the block is "full". Both are packed raw (tools/pack.py raw:) so world.c reads them in place.
maptab.bin leaves out (TECH_DEBT world-5: lite sets count against the near-cell arena):
- the navmesh (navv_off .. fog_off; header nav counts 0): the runtime reads it only in map_floor, player.c's fallback
  without collision.bin, and every world block has collision;
- mip 0 pixels of textures that have a mip 1: they move to the end of mapres.bin and the entry's pix[0] becomes
  RES_FLAG | offset in mapres.bin (texcache.c tc_res resolves it while the block is full; a lite block draws mip 1).
- mip 1 pixels of textures that have a mip 2 and no near level (MIP1_OUT=1, the default; the Burg plan's lite set
  diet, step B1): they move to mapres.bin too, pix[1] becomes RES_FLAG | offset there, and the entry's near word holds
  the maptab offset of its mip 2 / 3 chain (pad bit 1, TEX_CHAIN_AT; the chain used to be found past mip 1). The runtime
  streams mip 1 per texture as it streams mip 0 (mapstream.c MS_RES_TEX | MS_RES_MIP1) and draws the resident mip 2
  until it is in. MIP1_OUT=0 (make WORLD_LITE_DIET=0): mip 1 stays in maptab as before.
Every other offset into maptab (header sections, texture CLUT / mip 1 / near pixels) moves down by the bytes cut
before it.
hzn.bin (tools/horizon.py, the resident far backdrop; empty: none) is appended to maptab.bin, 64-byte aligned, and the
cell table header's words 2 and 3 get its offset (from map.bin's start, as the runtime resolves maptab offsets) and
size; map.c finds it there (0: no horizon, the old drawing).
TEX_PEERS (env, the cube-grid plan; empty / unset: no sharing): the map.bin files of the world blocks of this MSB (this
one among them). Textures two or more of them have (same entry and bytes) leave every block's maptab for the map's
shared maptex.bin (argv[5]; every block writes the same file): what of such a texture stays in maptab otherwise (CLUT,
mip 1 when it does not stream, the mip 2 / 3 chain, near pixels) goes there, each kept range 128-aligned, and the
entry's offsets get TC_SHR (texcache.c) | offset in maptex.bin. world.c reads maptex.bin once per map."""
import hashlib, os, struct, sys

HDR = 80
PSM_T4 = 0x14
RES_FLAG = 0x80000000  # texture pix[0] / pix[1] in mapres.bin (texcache.c TC_RES)
TEX_CHAIN_AT = 2  # entry pad bit: the near word is the maptab offset of the mip 2 / 3 chain (texcache.c TE_CHAIN_AT)
TC_SHR = 0x40000000  # entry offset in the map's maptex.bin (texcache.c TC_SHR)
MIP1_OUT = os.environ.get("MIP1_OUT", "1") != "0"


def tex_cuts(b):
    """map.bin bytes -> (sections, per texture (span start, span end, [(offset, bytes, level)] cut to mapres))"""
    chunk_off, tex_off, navv_off, navt_off = struct.unpack_from("<4I", b, 36)
    ntex = struct.unpack_from("<I", b, 8)[0]
    fog_off, cell_off = struct.unpack_from("<2I", b, 72)
    starts = sorted((struct.unpack_from("<I", b, tex_off + 32 * i + 12)[0], i) for i in range(ntex))
    end = {i: (starts[k + 1][0] if k + 1 < len(starts) else navv_off) for k, (_, i) in enumerate(starts)}
    out = []
    for i in range(ntex):
        e = tex_off + 32 * i
        w, h, psm, cpsm, levels, pad = struct.unpack_from("<2H4B", b, e)
        clut, p0, p1 = struct.unpack_from("<3I", b, e + 12)
        near = struct.unpack_from("<I", b, e + 28)[0]
        cuts = []
        if levels >= 2 and tex_off <= p0 < navv_off:
            n0 = w * h // (2 if psm == PSM_T4 else 1)
            cuts.append((p0, n0, 0))
            if MIP1_OUT and levels >= 3 and not ((pad & 1) and near) and tex_off <= p1 < navv_off:
                n1 = ((w >> 1) * (h >> 1) // (2 if psm == PSM_T4 else 1) + 15) & ~15
                if n1 >= 64:
                    cuts.append((p1, n1, 1))
        out.append((clut, end[i], cuts))
    return (chunk_off, tex_off, navv_off, fog_off, cell_off, ntex), out


def tex_key(b, tex_off, i, span):
    """identity of texture i (entry without offsets / density, and its bytes)"""
    e = tex_off + 32 * i
    return hashlib.md5(b[e:e + 8] + b[e + 24:e + 28] + b[span[0]:span[1]]).hexdigest()


def kept(span, cuts):
    """the byte ranges of a texture's span not cut to mapres: [(start, end)]"""
    r, pos = [], span[0]
    for a, n, _ in sorted(cuts):
        if a > pos:
            r.append((pos, a))
        pos = max(pos, a + n)
    if span[1] > pos:
        r.append((pos, span[1]))
    return r


def shared_textures(src, peers, mtx_path):
    """TEX_PEERS: write maptex.bin; -> {texture index of src: [(start, end, maptex offset)]} for its shared textures"""
    if not peers:
        return {}
    keys = {}  # key -> peers that have it
    first = {}  # key -> (bytes, span, cuts) of its first occurrence (peers in path order)
    mine = {}
    for path in sorted(set(peers) | {src}):
        b = open(path, "rb").read()
        sec, tex = tex_cuts(b)
        for i, (c0, c1, cuts) in enumerate(tex):
            k = tex_key(b, sec[1], i, (c0, c1))
            keys.setdefault(k, set()).add(path)
            first.setdefault(k, (b, (c0, c1), cuts))
            if os.path.abspath(path) == os.path.abspath(src):
                mine[i] = k
    out, at = bytearray(), {}
    for k in sorted(kk for kk, ps in keys.items() if len(ps) >= 2):
        b, span, cuts = first[k]
        at[k] = []
        for a, z in kept(span, cuts):
            out.extend(b"\0" * (-len(out) % 128))
            at[k].append((a - span[0], z - span[0], len(out)))
            out += b[a:z]
    with open(mtx_path, "wb") as f:
        f.write(out)
    res = {}
    if mine:
        b = open(src, "rb").read()
        sec, tex = tex_cuts(b)
        for i, k in mine.items():
            if k in at:
                c0 = tex[i][0]
                res[i] = [(c0 + a, c0 + z, o) for a, z, o in at[k]]
    print(f"{src}: maptex {len(out)} bytes ({len(at)} textures in two or more blocks), {len(res)} of them here")
    return res


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
    l1 = {}  # texture -> (mip 1 offset in map.bin, bytes cut, mip 2 offset in map.bin)
    for i in range(ntex):
        e = tex_off + 32 * i
        w, h, psm, cpsm, levels, pad = struct.unpack_from("<2H4B", b, e)
        p0, p1 = struct.unpack_from("<2I", b, e + 16)
        near = struct.unpack_from("<I", b, e + 28)[0]
        if levels < 2 or not tex_off <= p0 < navv_off:
            continue
        n0 = w * h // (2 if psm == PSM_T4 else 1)
        assert n0 % 16 == 0 and gone.get(p0, n0) == n0, f"texture {i}: mip 0 at {p0} ({n0} bytes)"
        gone[p0] = n0
        l0[i] = (p0, n0)
        if MIP1_OUT and levels >= 3 and not ((pad & 1) and near) and tex_off <= p1 < navv_off:
            n1 = ((w >> 1) * (h >> 1) // (2 if psm == PSM_T4 else 1) + 15) & ~15  # texmap.py: levels 16-byte aligned
            if n1 >= 64 and gone.get(p1, n1) == n1 and p1 not in gone:
                gone[p1] = n1
                l1[i] = (p1, n1, p1 + n1)
    peers = os.environ.get("TEX_PEERS", "").split()
    shr = shared_textures(src, peers, sys.argv[5]) if len(sys.argv) > 5 else {}
    shr_cut = set()
    for rs in shr.values():  # the shared textures' kept ranges leave maptab too (not for mapres)
        for a, z, _ in rs:
            if z > a and a not in gone:
                gone[a] = z - a
                shr_cut.add(a)

    def shr_off(i, o):  # a shared texture's offset in maptex.bin (TC_SHR), else None
        for a, z, m in shr.get(i, ()):
            if a <= o < z:
                return TC_SHR | (m + o - a)
        return None
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
        if (a0 != navv_off or not cut) and a0 not in shr_cut:
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
        mv = lambda o: shr_off(i, o) if shr_off(i, o) is not None else moved(o)
        q1 = RES_FLAG | at[l1[i][0]] if i in l1 else mv(p1)
        struct.pack_into("<3I", tab, e + 12, mv(clut), RES_FLAG | at[l0[i][0]] if i in l0 else mv(p0), q1)
        if i in l1:  # the chain's place in maptab (mip 2 follows mip 1 in map.bin)
            struct.pack_into("<I", tab, e + 28, mv(l1[i][2]))
            tab[e + 7] |= TEX_CHAIN_AT
        elif near:
            struct.pack_into("<I", tab, e + 28, mv(near))
    hzn = open(sys.argv[4], "rb").read() if len(sys.argv) > 4 else b""
    if hzn:
        assert hzn[:4] == b"HZN1" and struct.unpack_from("<I", hzn, 8)[0] == struct.unpack_from("<I", b, cell_off)[0], \
            "hzn.bin: not this map's horizon"
        tab.extend(b"\0" * (-len(tab) % 64))
        struct.pack_into("<2I", tab, HDR + moved(cell_off) - chunk_off + 8, len(tab) - HDR + chunk_off, len(hzn))
        tab += hzn
    with open(tab_path, "wb") as f:
        f.write(tab)
    with open(res_path, "wb") as f:
        f.write(res)
    cut_l1 = sum(n1 for _, n1, _ in l1.values())
    cut_l0 = sum(n0 for a0, n0 in ranges if a0 != navv_off or not cut) - cut_l1
    print(f"{src}: maptab {len(tab)} bytes (navmesh {cut}, mip 0 of {len(l0)} textures {cut_l0} and mip 1 of {len(l1)} "
          f"{cut_l1} moved out), "
          f"mapres {len(res)} bytes, horizon {len(hzn)} bytes")


if __name__ == "__main__":
    main()
