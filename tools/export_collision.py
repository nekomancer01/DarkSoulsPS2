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
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import havok_collision
import area as area_mod
import gamepath

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
    out = []
    for s in subs:
        tris = [t for t in s["tris"] if any(keep(*p) for p in t) and len({q(p) for p in t}) == 3 and not dup(s, t)]
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


if __name__ == "__main__":
    game = gamepath.game_dir(sys.argv[1] if len(sys.argv) > 1 else None)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(gamepath.ROOT, "build/gen/collision.bin")
    area = sys.argv[3] if len(sys.argv) > 3 else "m10_02_00_00"
    detail = sys.argv[4] if len(sys.argv) > 4 else "h"
    subs, missing, unused = area_collision(game, area, detail)
    c = build(subs, float(os.environ.get("CELL", 2)))
    st = write(c, out)
    lens = [len(x) for x in c["cells"]]
    print(f"{out}: {st['size']} bytes ({len(c['verts'])} verts {st['verts']} B, {len(c['tris'])} tris {st['tris']} B, "
          f"grid {c['gw']}x{c['gd']} cells of {c['cell']} m {st['cells']} B, {st['nlist']} refs {st['list']} B); "
          f"max {max(lens)} / mean nonempty {sum(lens) / max(1, sum(1 for l in lens if l)):.1f} tris per cell; "
          f"origin {c['origin']}; MSB parts without hkx: {missing}; unplaced hkx: {unused}")
