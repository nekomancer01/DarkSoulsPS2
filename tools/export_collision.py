"""Map collision -> ps2/proto/src/collision.bin (read by ps2/proto/src/collision.c).

usage: export_collision.py GAME OUT.bin [AREA] [h|l]      env CELL = grid cell size in metres (default 2)

Layout (little-endian, offsets from the start of the file):
  header  char magic[4] "DCOL", u32 version 1, u32 nverts, u32 ntris,
          f32 origin[3], f32 scale (metres per vertex unit),
          f32 grid_x0, f32 grid_z0, f32 cell, u32 gw, u32 gd,
          u32 off_verts, u32 off_tris, u32 off_cells, u32 off_list, u32 nlist, pad to 80 bytes
  verts   s16 x, y, z per vertex: world = origin + v * scale (shared, deduplicated after quantization)
  tris    u16 a, b, c; u8 hit_filter (MSB1 HitFilterID, 8 = Normal); u8 material (hkx subpart material id)
  cells   u32[gw * gd + 1]: triangles of cell (ix, iz) are list[cells[iz * gw + ix] .. cells[iz * gw + ix + 1]]
  list    u16 triangle indices; a triangle is listed in every x/z cell its footprint overlaps
"""
import math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import havok_collision
import area as area_mod
import gamepath
import navwalls

SCALE = 1 / 128  # 7.8 mm steps, +-256 m around the origin


def _tri_overlaps_rect(p, x0, z0, x1, z1):
    """2D separating-axis test of triangle p [(x, z)] x3 against the rectangle."""
    if max(q[0] for q in p) < x0 or min(q[0] for q in p) > x1 or max(q[1] for q in p) < z0 or min(q[1] for q in p) > z1:
        return False
    rect = ((x0, z0), (x1, z0), (x1, z1), (x0, z1))
    for i in range(3):
        a, b = p[i], p[(i + 1) % 3]
        nx, nz = b[1] - a[1], a[0] - b[0]
        side = nx * (p[(i + 2) % 3][0] - a[0]) + nz * (p[(i + 2) % 3][1] - a[1])
        if side == 0:
            continue  # degenerate in x/z (vertical wall seen from above): the AABB test is enough
        if all((nx * (r[0] - a[0]) + nz * (r[1] - a[1])) * side < 0 for r in rect):
            return False
    return True


def build(subs, cell):
    pts = [p for s in subs for t in s["tris"] for p in t]
    lo = [min(p[k] for p in pts) for k in range(3)]
    hi = [max(p[k] for p in pts) for k in range(3)]
    origin = [round((lo[k] + hi[k]) / 2) for k in range(3)]
    assert all(max(hi[k] - origin[k], origin[k] - lo[k]) / SCALE < 32767 for k in range(3)), "map too large for s16"
    vidx, verts, tris = {}, [], []

    def vert(p):
        q = tuple(round((p[k] - origin[k]) / SCALE) for k in range(3))
        if q not in vidx:
            vidx[q] = len(verts)
            verts.append(q)
        return vidx[q]

    for s in subs:
        mat = s["material"] if 0 <= s["material"] < 256 else 255
        for t in s["tris"]:
            a, b, c = (vert(p) for p in t)
            if a != b and b != c and a != c:
                tris.append((a, b, c, s["hit_filter"], mat))
    assert len(verts) < 65536 and len(tris) < 65536, \
        f"{len(verts)} verts / {len(tris)} tris exceed the u16 indices: give the area a radius in tools/area.py"
    x0, z0 = lo[0] - 0.01, lo[2] - 0.01
    gw, gd = int((hi[0] - x0) / cell) + 1, int((hi[2] - z0) / cell) + 1
    cells = [[] for _ in range(gw * gd)]
    W = lambda v: [origin[k] + verts[v][k] * SCALE for k in range(3)]
    eps = 0.02
    for ti, (a, b, c, _, _) in enumerate(tris):
        p = [(q[0], q[2]) for q in (W(a), W(b), W(c))]
        ix0 = max(0, int((min(q[0] for q in p) - eps - x0) / cell))
        ix1 = min(gw - 1, int((max(q[0] for q in p) + eps - x0) / cell))
        iz0 = max(0, int((min(q[1] for q in p) - eps - z0) / cell))
        iz1 = min(gd - 1, int((max(q[1] for q in p) + eps - z0) / cell))
        for iz in range(iz0, iz1 + 1):
            for ix in range(ix0, ix1 + 1):
                cx, cz = x0 + ix * cell, z0 + iz * cell
                if _tri_overlaps_rect(p, cx - eps, cz - eps, cx + cell + eps, cz + cell + eps):
                    cells[iz * gw + ix].append(ti)
    return dict(origin=origin, verts=verts, tris=tris, x0=x0, z0=z0, cell=cell, gw=gw, gd=gd, cells=cells)


_MEMO = {}  # the last area_collision result (export_map.py asks for it four times per export)


def area_collision(game, area, detail="h"):
    """havok_collision.map_collision limited to the area's region (tools/area.py): a triangle is kept when one of its
    corners is inside. Shared with export_map.py (fog rows, light probes, visible draw groups). The last result is
    kept for the next call with the same arguments and region (callers must not modify it)."""
    key = (game, area, detail, os.environ.get("AREA_BLOCK"), os.environ.get("AREA_RADIUS"), os.environ.get("AREA_SPAWN"),
           os.environ.get("AREA_MAP_RADIUS"))
    if key not in _MEMO:
        _MEMO.clear()
        _MEMO[key] = _area_collision(game, area, detail)
    return _MEMO[key]


def _area_collision(game, area, detail):
    import souls
    subs, missing, unused = havok_collision.map_collision(game, area, detail)
    keep = (lambda x, y, z: True) if area_mod.is_whole(area) else \
        area_mod.region(souls.msb1(souls.read(f"{game}/map/MapStudio/{area}.msb")), area)
    # build() drops triangles whose corners meet on its 1/SCALE grid; drop them here too, so the per-triangle tables
    # of export_map.py (fog rows, dg.bin) number triangles like collision.bin (Burg: 42220 -> 42129)
    q = lambda p: tuple(round(c / SCALE) for c in p)
    # the same triangle under both water filters (m18_01: h0399B1 / h0899B1 / h1399B1 / h1699B1 as Water_A 9, the same
    # model again as _0000 parts with Water_B 16, no entity id): collision.c treats 9 and 16 alike (COL_WATER) and no
    # event can switch such a part, so the second copy is dropped (Asylum 65572 -> 65404 tris, fits the u16 indices)
    water, seen = (9, 16), set()
    dup = lambda s, t: s["hit_filter"] in water and s["msb"]["entity"] < 0 and frozenset(map(q, t)) in seen
    # walls of one part that seal a doorway another part leaves open (tools/navwalls.py: the Taurus tower's ground
    # floor doorway in Undead Burg). NAVWALLS=0: keep them (old collision)
    sealed = set() if os.environ.get("NAVWALLS") == "0" else \
        navwalls.sealed(game, area, subs, log=lambda m: print(m, file=sys.stderr))
    out = []
    for si, s in enumerate(subs):
        tris = [t for ti, t in enumerate(s["tris"]) if (si, ti) not in sealed and any(keep(*p) for p in t)
                and len({q(p) for p in t}) == 3 and not dup(s, t)]
        if s["hit_filter"] in water and s["msb"]["entity"] < 0:
            seen.update(frozenset(map(q, t)) for t in tris)
        if tris:
            out.append(dict(s, tris=tris))
    return out, missing, unused


def write(c, path):
    starts, lst = [], []
    for cl in c["cells"]:
        starts.append(len(lst))
        lst += cl
    starts.append(len(lst))
    vb = b"".join(struct.pack("<3h", *v) for v in c["verts"])
    vb += b"\0" * (-len(vb) % 4)
    tb = b"".join(struct.pack("<3HBB", *t) for t in c["tris"])
    cb = struct.pack(f"<{len(starts)}I", *starts)
    lb = struct.pack(f"<{len(lst)}H", *lst)
    hsize = 80
    ov, ot = hsize, hsize + len(vb)
    oc = ot + len(tb)
    ol = oc + len(cb)
    hdr = struct.pack("<4sIII3ff3fII5I", b"DCOL", 1, len(c["verts"]), len(c["tris"]), *c["origin"], SCALE,
                      c["x0"], c["z0"], c["cell"], c["gw"], c["gd"], ov, ot, oc, ol, len(lst))
    hdr += b"\0" * (hsize - len(hdr))
    data = hdr + vb + tb + cb + lb
    with open(path, "wb") as f:
        f.write(data)
    return dict(size=len(data), verts=len(vb), tris=len(tb), cells=len(cb), list=len(lb), nlist=len(lst))


def load(path):
    """collision.bin -> dict(verts world floats, tris, grid) for the Python checks."""
    b = open(path, "rb").read()
    (mg, ver, nv, nt, ox, oy, oz, sc, x0, z0, cell, gw, gd, ov, ot, oc, ol, nl) = struct.unpack_from(
        "<4sIII3ff3fII5I", b, 0)
    assert mg == b"DCOL"
    verts = [(ox + x * sc, oy + y * sc, oz + z * sc) for x, y, z in struct.iter_unpack("<3h", b[ov:ov + 6 * nv])]
    tris = list(struct.iter_unpack("<3HBB", b[ot:ot + 8 * nt]))
    starts = struct.unpack_from(f"<{gw * gd + 1}I", b, oc)
    lst = struct.unpack_from(f"<{nl}H", b, ol)
    return dict(verts=verts, tris=tris, x0=x0, z0=z0, cell=cell, gw=gw, gd=gd, starts=starts, list=lst)


SOLID = lambda f: f <= 8  # HitFilterID 0-8 block the player (soulstruct is_solid_to_player)


def floor(c, x, z, y_top, y_bottom=-1e9, solid=SOLID):
    """Reference for col_floor: highest walkable-or-not triangle surface at (x, z) with y_bottom <= h <= y_top."""
    ix, iz = int((x - c["x0"]) / c["cell"]), int((z - c["z0"]) / c["cell"])
    if not (0 <= ix < c["gw"] and 0 <= iz < c["gd"]):
        return None
    k = iz * c["gw"] + ix
    best = None
    for ti in c["list"][c["starts"][k]:c["starts"][k + 1]]:
        a, b, cc, hf, _ = c["tris"][ti]
        if not solid(hf):
            continue
        A, B, C = c["verts"][a], c["verts"][b], c["verts"][cc]
        d = (B[2] - C[2]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[2] - C[2])
        if abs(d) < 1e-9:
            continue
        l1 = ((B[2] - C[2]) * (x - C[0]) + (C[0] - B[0]) * (z - C[2])) / d
        l2 = ((C[2] - A[2]) * (x - C[0]) + (A[0] - C[0]) * (z - C[2])) / d
        l3 = 1 - l1 - l2
        if min(l1, l2, l3) < 0:
            continue
        h = l1 * A[1] + l2 * B[1] + l3 * C[1]
        if y_bottom <= h <= y_top and (best is None or h > best):
            best = h
    return best


# ---- DCOL v2: fragments (COL_V1=1 writes v1) ----
# Ame asked (2026-11-08) for the Burg blocks' collision to take less of the near-cell arena (two blocks' lite sets
# held 4.2 of 8 MB). The packing follows Naughty Dog's Jak 2 background collision (OpenGOAL collide-hash fragments:
# u16 vertices relative to the fragment, 4-byte polygons of three u8 indices + a u8 attribute index, a small grid per
# fragment with u8 lists): 8-byte triangles -> 4, u16 grid refs -> u8, plus a u16 per triangle back to the v1 number
# (events, draw groups, fog rows and block links address triangles by it). Same quantized vertices, so the same answers.
#   header  96 B: "DCOL", u32 2, u32 nverts (all fragments), u32 ntris; f32 origin[3], scale; f32 x0, z0, cell (top
#           grid), u32 gw, gd; u32 off_frag, nfrag, off_top, off_toplist, ntoplist, off_pat, npat, off_gid;
#           f32 ymin, ymax (vertex heights), f32 fcell (fragment grid cell)
#   top     u32[gw * gd + 1] starts into toplist (u16 fragment indices): fragments whose box overlaps the top cell
#   frags   32 B each: s16 lo[3], hi[3] (quantized, from origin); u16 ntri, nvert; u32 tri0 (first slot in gid);
#           u32 data offset; u8 gw, gd; u16 pad
#   data    per fragment, 4-aligned: u16 verts[nvert][3] (from lo); u8 tris[ntri][4] (a, b, c, pat);
#           u16 cells[gw * gd + 1] starts into u8 list (fragment-local triangle indices); grid origin = lo - 0.02 m
#   pat     u16 per entry: hit_filter | material << 8
#   gid     u16 per triangle slot (fragments' triangles in order): its v1 triangle number
FRAG_VERTS, FRAG_TRIS, TOP_CELL = 255, 255, 8.0


def _fragments(c, idx0=None):
    """v1 build() -> [triangle indices] per fragment: kd split on centroids (longest axis) until a node has at most
    FRAG_VERTS distinct vertices and FRAG_TRIS triangles (idx0: only these triangles; default all)"""
    V, T = c["verts"], c["tris"]
    cen = [tuple(sum(V[t[i]][k] for i in range(3)) / 3 for k in range(3)) for t in T]
    out, stack = [], [list(range(len(T))) if idx0 is None else list(idx0)]
    while stack:
        idx = stack.pop()
        nv = len({v for i in idx for v in T[i][:3]})
        if nv <= FRAG_VERTS and len(idx) <= FRAG_TRIS:
            out.append(idx)
            continue
        # the split (axis, then a few cut points around the median) whose halves share the fewest vertices: a shared
        # vertex is stored in both fragments
        best = None
        for ax in range(3):
            srt = sorted(idx, key=lambda i: cen[i][ax])
            for q in (0.35, 0.5, 0.65):
                h = max(1, min(len(srt) - 1, int(len(srt) * q)))
                a = {v for i in srt[:h] for v in T[i][:3]}
                shared = sum(1 for i in srt[h:] for v in T[i][:3] if v in a)
                if best is None or shared < best[0]:
                    best = (shared, srt[:h], srt[h:])
        stack += [best[1], best[2]]
    out.sort(key=lambda f: (min(cen[i][2] for i in f), min(cen[i][0] for i in f)))
    return out


# Collision tiles (the cube-grid plan's step 1, 2026-11-09; COL_TILE=0: one unpaged file as before). Triangles go to the
# COL_TILE m cube (x, y, z) their centroid lies in and fragments never mix tiles, so a tile's fragment data is one byte
# range: a page. The runtime (src/coltile.c) keeps a block's index resident (header, top grid, fragment records,
# patterns, v1 numbers, page table) and reads the pages within reach of the player into the structural pool; at the
# Burg test spots 0.7-1.0 MB of the blocks' 1.6 MB of collision lies within 56 m (xz) / 16 m (y). Pages are ordered by
# their 32 m map cell (the tile key >> 1, eight tiles per cell) and lie next to each other on the disc, so the tiles of
# one cell read in one request. No triangle is stored twice: a tile's page box is its fragments' true box (it overhangs
# the cube by up to a triangle), and the runtime ranks pages by that box.
#   page table (after gid, 16-aligned): u32 'CTP1', npage, data_lo (first page byte = the index's size), f32 tile;
#           32 B per page: s16 key[3] (tile, floor(centroid / tile)), u16 nfrag; u32 frag0, offset (file), bytes;
#           s16 lo[3], hi[3] (quantized like the fragments: the page's box)
#   pages   from data_lo, each 128-aligned: its fragments' data in fragment order (fragment data offsets stay file
#           offsets: in a page buffer at offset - page offset)
COL_TILE = float(os.environ.get("COL_TILE", 16))
PAGE_ALIGN = 128


def _tile_groups(c, tile):
    """[(tile key, [triangle indices])] in page order: by 32 m cell (key >> 1), then the octant inside it"""
    V, T, org = c["verts"], c["tris"], c["origin"]
    groups = {}
    for i, t in enumerate(T):
        cen = [org[k] + sum(V[t[j]][k] for j in range(3)) / 3 * SCALE for k in range(3)]
        key = tuple(int(math.floor(cen[k] / tile)) for k in range(3))
        groups.setdefault(key, []).append(i)
    order = sorted(groups, key=lambda k: (k[2] >> 1, k[0] >> 1, k[1] >> 1, k[2] & 1, k[0] & 1, k[1] & 1))
    return [(k, groups[k]) for k in order]


def write_v2(c, path):
    V, T, org, cell = c["verts"], c["tris"], c["origin"], c["cell"]
    W = lambda q: [org[k] + q[k] * SCALE for k in range(3)]
    tile = COL_TILE
    if tile > 0:
        groups = _tile_groups(c, tile)
        frags, pages = [], []
        for key, idx in groups:
            fs = _fragments(c, idx)
            pages.append((key, len(frags), len(fs)))
            frags += fs
    else:
        frags, pages = _fragments(c), []
    pats, pat_of = [], {}
    frec, blobs, gid = [], [], []
    eps = 0.02
    hsize = 96
    for f in frags:
        vs = sorted({v for i in f for v in T[i][:3]})
        loc = {v: j for j, v in enumerate(vs)}
        lo = [min(V[v][k] for v in vs) for k in range(3)]
        hi = [max(V[v][k] for v in vs) for k in range(3)]
        vb = b"".join(struct.pack("<3H", *(V[v][k] - lo[k] for k in range(3))) for v in vs)
        vb += b"\0" * (-len(vb) % 4)
        tb = b""
        for i in f:
            a, b_, cc, hf, mat = T[i]
            key = hf | mat << 8
            if key not in pat_of:
                pat_of[key] = len(pats)
                pats.append(key)
            assert pat_of[key] < 256, "more than 256 hit filter / material pairs"
            tb += struct.pack("<4B", loc[a], loc[b_], loc[cc], pat_of[key])
        lw, hw = W(lo), W(hi)
        fx0, fz0 = lw[0] - eps, lw[2] - eps
        gw, gd = int((hw[0] - fx0) / cell) + 1, int((hw[2] - fz0) / cell) + 1
        assert gw < 256 and gd < 256
        cells = [[] for _ in range(gw * gd)]
        for j, i in enumerate(f):
            p = [(q[0], q[2]) for q in (W(V[T[i][0]]), W(V[T[i][1]]), W(V[T[i][2]]))]
            ix0 = max(0, int((min(q[0] for q in p) - eps - fx0) / cell))
            ix1 = min(gw - 1, int((max(q[0] for q in p) + eps - fx0) / cell))
            iz0 = max(0, int((min(q[1] for q in p) - eps - fz0) / cell))
            iz1 = min(gd - 1, int((max(q[1] for q in p) + eps - fz0) / cell))
            for iz in range(iz0, iz1 + 1):
                for ix in range(ix0, ix1 + 1):
                    cx, cz = fx0 + ix * cell, fz0 + iz * cell
                    if _tri_overlaps_rect(p, cx - eps, cz - eps, cx + cell + eps, cz + cell + eps):
                        cells[iz * gw + ix].append(j)
        starts, lst = [], []
        for cl in cells:
            starts.append(len(lst))
            lst += cl
        starts.append(len(lst))
        assert len(lst) < 65536
        blob = vb + tb + struct.pack(f"<{len(starts)}H", *starts) + bytes(lst)
        blob += b"\0" * (-len(blob) % 4)
        frec.append([lo, hi, len(f), len(vs), len(gid), 0, gw, gd])
        gid += f
        blobs.append(blob)
    # top grid over the fragments' xz boxes
    allw = [W(v) for v in V]
    # the origin as v1 had it (build(): the unquantized minimum - 0.01 m): export_world's block bounds stay the same
    x0, z0 = c.get("x0", min(p[0] for p in allw) - 0.01), c.get("z0", min(p[2] for p in allw) - 0.01)
    tgw = int((max(p[0] for p in allw) - x0) / TOP_CELL) + 1
    tgd = int((max(p[2] for p in allw) - z0) / TOP_CELL) + 1
    top = [[] for _ in range(tgw * tgd)]
    for fi, (lo, hi, *_r) in enumerate(frec):
        lw, hw = W(lo), W(hi)
        for iz in range(max(0, int((lw[2] - eps - z0) / TOP_CELL)), min(tgd - 1, int((hw[2] + eps - z0) / TOP_CELL)) + 1):
            for ix in range(max(0, int((lw[0] - eps - x0) / TOP_CELL)), min(tgw - 1, int((hw[0] + eps - x0) / TOP_CELL)) + 1):
                top[iz * tgw + ix].append(fi)
    tstarts, tlist = [], []
    for cl in top:
        tstarts.append(len(tlist))
        tlist += cl
    tstarts.append(len(tlist))
    off_top = hsize
    off_tl = off_top + 4 * len(tstarts)
    off_frag = (off_tl + 2 * len(tlist) + 15) & ~15
    if pages:  # index first (fragment records, patterns, v1 numbers, page table), the pages after it
        off_pat = off_frag + 32 * len(frec)
        off_gid = (off_pat + 2 * len(pats) + 3) & ~3
        off_ptab = (off_gid + 2 * len(gid) + 15) & ~15
        data_lo = (off_ptab + 16 + 32 * len(pages) + PAGE_ALIGN - 1) & ~(PAGE_ALIGN - 1)
        data, prec = b"", []
        for key, f0, nf in pages:
            data += b"\0" * (-(data_lo + len(data)) % PAGE_ALIGN)
            p0 = data_lo + len(data)
            for fi in range(f0, f0 + nf):
                frec[fi][5] = data_lo + len(data)
                data += blobs[fi]
            lo = [min(frec[fi][0][k] for fi in range(f0, f0 + nf)) for k in range(3)]
            hi = [max(frec[fi][1][k] for fi in range(f0, f0 + nf)) for k in range(3)]
            assert all(-32768 <= v < 32768 for v in key)
            prec.append(struct.pack("<3hHIII3h3h", *key, nf, f0, p0, data_lo + len(data) - p0, *lo, *hi))
        ptab = struct.pack("<4sIIf", b"CTP1", len(pages), data_lo, tile) + b"".join(prec)
    else:  # fragment data right after the records (the old layout)
        off_data = off_frag + 32 * len(frec)
        data = b""
        for fi, blob in enumerate(blobs):
            frec[fi][5] = off_data + len(data)
            data += blob
        off_pat = off_data + len(data)
        off_gid = (off_pat + 2 * len(pats) + 3) & ~3
    fb = b"".join(struct.pack("<3h3hHHIIBBH4x", *lo, *hi, nt, nv, t0, do, gw, gd, 0)
                  for lo, hi, nt, nv, t0, do, gw, gd in frec)
    ys = [p[1] for p in allw]
    hdr = struct.pack("<4sIII3ff3fII8I3f", b"DCOL", 2, sum(r[3] for r in frec), len(T), *org, SCALE, x0, z0, TOP_CELL,
                      tgw, tgd, off_frag, len(frec), off_top, off_tl, len(tlist), off_pat, len(pats), off_gid,
                      min(ys), max(ys), cell)
    assert len(hdr) == hsize, len(hdr)
    out = hdr + struct.pack(f"<{len(tstarts)}I", *tstarts) + struct.pack(f"<{len(tlist)}H", *tlist)
    out += b"\0" * (off_frag - len(out)) + fb
    if pages:
        out += struct.pack(f"<{len(pats)}H", *pats)
        out += b"\0" * (off_gid - len(out)) + struct.pack(f"<{len(gid)}H", *gid)
        out += b"\0" * (off_ptab - len(out)) + ptab
        out += b"\0" * (data_lo - len(out)) + data
    else:
        out += data + struct.pack(f"<{len(pats)}H", *pats)
        out += b"\0" * (off_gid - len(out)) + struct.pack(f"<{len(gid)}H", *gid)
    with open(path, "wb") as fo:
        fo.write(out)
    return dict(size=len(out), frags=len(frec), data=len(data), gid=2 * len(gid), top=4 * len(tstarts) + 2 * len(tlist),
                verts=sum(r[3] for r in frec), pages=len(pages), index=data_lo if pages else len(out))


def v1_dict(path):
    """a v1 collision.bin back to build()'s dict (quantized verts, tris, cell): repacking without the game"""
    b = open(path, "rb").read()
    (mg, ver, nv, nt, ox, oy, oz, sc, x0, z0, cell, gw, gd, ov, ot, oc, ol, nl) = struct.unpack_from(
        "<4sIII3ff3fII5I", b, 0)
    assert mg == b"DCOL" and ver == 1 and abs(sc - SCALE) < 1e-9
    return dict(origin=[ox, oy, oz], verts=list(struct.iter_unpack("<3h", b[ov:ov + 6 * nv])),
                tris=list(struct.iter_unpack("<3HBB", b[ot:ot + 8 * nt])), cell=cell, x0=x0, z0=z0)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--repack":  # v1 collision.bin -> v2 (tests, no game needed)
        st = write_v2(v1_dict(sys.argv[2]), sys.argv[3])
        print(f"{sys.argv[3]}: {st}")
        sys.exit(0)
    game = gamepath.game_dir(sys.argv[1] if len(sys.argv) > 1 else None)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(gamepath.ROOT, "build/gen/collision.bin")
    area = sys.argv[3] if len(sys.argv) > 3 else "m10_02_00_00"
    detail = sys.argv[4] if len(sys.argv) > 4 else "h"
    subs, missing, unused = area_collision(game, area, detail)
    c = build(subs, float(os.environ.get("CELL", 2)))
    if os.environ.get("COL_V1") != "1":  # DCOL v2 (write_v2), the default; COL_V1=1: the old layout
        st = write_v2(c, out)
        print(f"{out}: v2 {st['size']} bytes ({len(c['tris'])} tris in {st['frags']} fragments, {st['verts']} verts, "
              f"v1 numbers {st['gid']} B, top grid {st['top']} B, {st['pages']} tile pages, index {st['index']} B); MSB parts without hkx: {missing}; unplaced hkx: {unused}")
        sys.exit(0)
    st = write(c, out)
    lens = [len(x) for x in c["cells"]]
    print(f"{out}: {st['size']} bytes ({len(c['verts'])} verts {st['verts']} B, {len(c['tris'])} tris {st['tris']} B, "
          f"grid {c['gw']}x{c['gd']} cells of {c['cell']} m {st['cells']} B, {st['nlist']} refs {st['list']} B); "
          f"max {max(lens)} / mean nonempty {sum(lens) / max(1, sum(1 for l in lens if l)):.1f} tris per cell; "
          f"origin {c['origin']}; MSB parts without hkx: {missing}; unplaced hkx: {unused}")
