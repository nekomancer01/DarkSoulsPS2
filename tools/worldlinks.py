"""Which world blocks the runtime has loaded at a spot, by world.bin's links (tools/export_world.py, src/world.c).

world.c loads blocks by the collision triangle under the player. Between two MSBs that is DSR's own rule: a
ConnectCollision part (MSB type 11) of map A names a map B, and B is loaded while the player stands on that part. The
map exporter's spot budgets (tools/mapsimp.py ws_shares, WS_UNION) split the visual room between the blocks loaded at a
spot; for another MSB (Firelink next to the Burg, the Asylum above Firelink) they used to guess that from 8 m voxels
around its navmesh, so the room was shared with Firelink at every Burg spot whose 64 m disc held a Firelink floor, and
not where a link loads it from further away. This module gives the exporters the runtime's own answer for the links
between MSBs: export_world.py builds world.bin's cross-MSB link masks with cross_masks() below, and the exporters ask
spot_masks() for the masks under their spots.

Approximations (ASSUMED): the floor under a spot is the triangle of the block's collision crop whose xz holds the spot
and whose plane there is nearest, within FLOOR_UP above to FLOOR_DOWN below (navmesh vertices sit on the floors); a
spot over no triangle loads nothing besides its own block. world.c's WORLD_KEEP_S (a block stays 5 s after the player
steps off its link) is not counted. Inside one MSB the links follow the cut cells (export_world.py LOAD_CELLS), which
ws_shares and tools/world_budget.py measure by the blocks' cells directly."""
import os, sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import area  # noqa: E402

FLOOR_UP, FLOOR_DOWN = 1.5, 4.0  # metres a spot may sit below / above its floor triangle's plane (ASSUMED)

_CROP = {}
_MSB = {}


def block_map(name):
    return area.BLOCKS[name]["map"] if name in area.BLOCKS else name


def msb(game, m):
    if m not in _MSB:
        import souls
        _MSB[m] = souls.msb1(souls.read(f"{game}/map/MapStudio/{m}.msb"))
    return _MSB[m]


def connect_parts(game, src, dst):
    """collision part names of MSB src whose ConnectCollision part targets map dst (-1 fields = any)"""
    want = [int(x) for x in dst[1:].split("_")]
    return {p["collision"] for p in msb(game, src)["parts"]
            if p["type"] == 11 and all(v < 0 or v == w for v, w in zip(p["map_id"], want))}


def crop(game, block):
    """the block's collision crop in collision.bin order: [(part, triangles)] (export_collision.area_collision with
    AREA_BLOCK set to the block; the caller's AREA_BLOCK and area_collision's memo are left as they were)"""
    if block not in _CROP:
        import export_collision
        m = block_map(block)
        old_env, old_memo = os.environ.get("AREA_BLOCK"), dict(export_collision._MEMO)
        try:
            if block == m:
                os.environ.pop("AREA_BLOCK", None)
            else:
                os.environ["AREA_BLOCK"] = block
            subs, _, _ = export_collision._area_collision(game, m, "h")
        finally:
            if old_env is None:
                os.environ.pop("AREA_BLOCK", None)
            else:
                os.environ["AREA_BLOCK"] = old_env
            export_collision._MEMO.clear()
            export_collision._MEMO.update(old_memo)
        _CROP[block] = [(s["part"], s["tris"]) for s in subs]
    return _CROP[block]


def cell_rects_m(name):
    """the block's cells (tools/area.py BLOCKS) as xz rectangles in metres, or None for a whole-map block"""
    cells = area.BLOCKS.get(name, {}).get("cells")
    if not cells:
        return None
    c = area.CELL
    return [(r[0] * c, r[1] * c, (r[2] + 1) * c, (r[3] + 1) * c) for r in area.cell_rects(cells)]


LINK_ALL = os.environ.get("WORLD_LINK_ALL", "1") == "1"
LINK_SHARE = 0.05  # ASSUMED: a block with fewer of a part's centroids only grazes it (the aqueduct's last part: 20 of 462)


def cross_masks(game, names, nbr, i, sub=None):
    """per triangle of block i's crop (collision.bin order) the blocks of other MSBs its ConnectCollision parts load
    (world.bin's load = keep mask between maps): the block of the target map holding most of the part's triangle
    centroids in its cells; a whole-map block (no cells) is DSR's own unit and takes the part wherever it lies"""
    sub = crop(game, names[i]) if sub is None else sub
    m = block_map(names[i])
    n = len(names)
    rects = [cell_rects_m(b) for b in names]
    by_part, nt = {}, 0
    for part, tris in sub:
        by_part.setdefault(part, []).extend(range(nt, nt + len(tris)))
        nt += len(tris)
    cen = [(sum(p[0] for p in t) / 3, sum(p[2] for p in t) / 3) for _, tris in sub for t in tris]
    out = [0] * nt
    for mj in {block_map(names[j]) for j in range(n) if nbr[i] >> j & 1 and block_map(names[j]) != m}:
        for part in connect_parts(game, m, mj) & set(by_part):
            ts = by_part[part]
            inside = lambda rs: sum(any(r[0] <= cen[t][0] < r[2] and r[1] <= cen[t][1] < r[3] for r in rs) for t in ts)
            cnt = {j: inside(rects[j]) if rects[j] else len(ts)
                   for j in range(n) if nbr[i] >> j & 1 and block_map(names[j]) == mj}
            # Every block of the target map holding at least LINK_SHARE of the part's centroids (DSR loads the whole
            # map; WORLD_LINK_ALL=0 = only the block holding most of them, as before). Firelink's aqueduct floor
            # h0028B2 (1605 triangles) has 113 centroids in Burg west's cells and 1404 in the Parish rest block's
            # since the latter was cut (2026-11-15): walking it loaded the Parish rest and not Burg west, which came
            # only from the last two parts by the Burg door, so the tunnel's far end drew nothing (white) and the
            # player was held at the door (ISO world_route 2026-11-16)
            best = max(cnt.values(), default=0)
            for j, c in cnt.items():
                if c and (c == best or (LINK_ALL and c >= LINK_SHARE * len(ts))):
                    for t in ts:
                        out[t] |= 1 << j
    return out


def floor_tris(game, name, P):
    """index (collision.bin order) of the floor triangle of block `name` under each spot of P (m, 3), -1 for none"""
    T = np.asarray([t for _, ts in crop(game, name) for t in ts], np.float64).reshape(-1, 3, 3)
    P = np.asarray(P, np.float64).reshape(-1, 3)
    out = np.full(len(P), -1, np.int64)
    if not len(T) or not len(P):
        return out
    G = 4.0
    lo = np.floor(T[:, :, [0, 2]].min(1) / G).astype(np.int64)
    hi = np.floor(T[:, :, [0, 2]].max(1) / G).astype(np.int64)
    grid = {}
    for t in np.nonzero(((hi - lo) <= 64).all(1))[0]:  # (a triangle wider than 256 m is no floor anyone stands on)
        for gx in range(lo[t, 0], hi[t, 0] + 1):
            for gz in range(lo[t, 1], hi[t, 1] + 1):
                grid.setdefault((gx, gz), []).append(t)
    a, b, c = T[:, 0], T[:, 1], T[:, 2]
    e1, e2 = b - a, c - a
    nrm = np.cross(e1, e2)
    for s, p in enumerate(P):
        cand = grid.get((int(np.floor(p[0] / G)), int(np.floor(p[2] / G))))
        if not cand:
            continue
        cand = np.asarray(cand)
        ny = nrm[cand, 1]
        ok = np.abs(ny) > 1e-9  # (walls have no height over a spot)
        if not ok.any():
            continue
        cand, ny = cand[ok], ny[ok]
        # barycentric in xz
        d = p[[0, 2]] - a[cand][:, [0, 2]]
        u1, u2 = e1[cand][:, [0, 2]], e2[cand][:, [0, 2]]
        den = u1[:, 0] * u2[:, 1] - u1[:, 1] * u2[:, 0]
        w1 = (d[:, 0] * u2[:, 1] - d[:, 1] * u2[:, 0]) / den
        w2 = (u1[:, 0] * d[:, 1] - u1[:, 1] * d[:, 0]) / den
        inside = (w1 >= -1e-6) & (w2 >= -1e-6) & (w1 + w2 <= 1 + 1e-6)
        y = a[cand, 1] + w1 * e1[cand, 1] + w2 * e2[cand, 1]
        dy = p[1] - y  # spot above the plane: positive
        ok = inside & (dy >= -FLOOR_UP) & (dy <= FLOOR_DOWN)
        if ok.any():
            k = np.nonzero(ok)[0]
            out[s] = int(cand[k[np.argmin(np.abs(dy[k]))]])
    return out


_LINKS = {}


def block_links(game, names, i):
    """block i's cross-MSB load mask per collision triangle (cached). Every block of another MSB counts as a neighbour
    here: export_world.py's LINK_GAP test needs every block's collision bounds (the whole world's crops, ~60 s), and a
    ConnectCollision part only ever names a map it touches. Checked against world.bin (2026-11-16, 14 blocks): every
    triangle's mask equal."""
    key = (tuple(names), i)
    if key not in _LINKS:
        m = block_map(names[i])
        nbr = [0] * len(names)
        nbr[i] = sum(1 << j for j, b in enumerate(names) if block_map(b) != m)
        _LINKS[key] = np.asarray(cross_masks(game, names, nbr, i), np.int64)
    return _LINKS[key]


def spot_masks(game, names, i, P):
    """u16-style block masks of the blocks of other MSBs the runtime loads at spots P standing on block i (names: the
    world's blocks, tools/area.py WORLDS order = world.bin's), by the floor triangle under each spot; 0 off any link"""
    names = list(names)
    if not any(block_map(b) != block_map(names[i]) for b in names):
        return np.zeros(len(P), np.int64)
    masks = block_links(game, names, i)
    t = floor_tris(game, names[i], P)
    return np.where(t >= 0, masks[np.maximum(t, 0)] if len(masks) else 0, 0)


def world_of(name):
    """the WORLDS list holding block `name` (or [name])"""
    return next((w for w in area.WORLDS.values() if name in w), [name])
