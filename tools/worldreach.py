"""Walking distances from a block's collision triangles to the seams of the blocks it links (world.bin v5 reach
table, tools/export_world.py; read by src/world.c WORLD_REACH).

world.c wants a neighbour block when the player can reach its seam sooner than the block takes to turn on (its lite
set read from the disc). The distance is walked along the block's own collision floor: a graph of its walkable
triangles (normal at most REACH_SLOPE from up) joined where they share a vertex (snapped to SNAP m, also across
parts: the floors of two parts meet at the same quantized positions), each step the distance between the two
triangles' centroids, Dijkstra from the seam's triangles out to REACH_MAX. A wall or ceiling triangle takes the
distance of the nearest floor it touches.

The seam of block j, seen from block i:
- another MSB: the triangles of i's ConnectCollision parts that load j (tools/worldlinks.py cross_masks; DSR's rule);
- the same MSB: i's triangles with a vertex inside j's cells (block crops keep a triangle with any vertex in their
  cells, so the triangles over the cut line are in both blocks: the first floor of j the player steps on).
Walks through a third block, drops and jumps are not counted (ASSUMED: world.c keeps today's load masks as well, so
a block is never wanted later than before; the reach only wants it earlier)."""
import heapq
import os
import math

REACH_MAX = float(os.environ.get("WORLD_REACH_MAX", 64))  # m of walking a reach entry goes out to (ASSUMED: ~10 s at
                                                         # dash speed, past any turn-on time + WORLD_PREP_S)
REACH_STEP = float(os.environ.get("WORLD_REACH_STEP", 4))  # m per distance unit (rounded down: wanted a bit early)
REACH_SLOPE = 0.5   # walkable: normal y >= this (ASSUMED: 60 degrees; steeper is wall, the player slides off)
SNAP = 0.25         # m: vertices this close are one (collision.bin quantizes to ~1-2 cm)
SEAM_IN = 0.01      # m inside j's cells that counts as j's floor


def _graph(tris):
    """walkable mask, centroids, adjacency lists (triangle -> [(triangle, metres)])"""
    n = len(tris)
    walk, cen = [False] * n, [None] * n
    by_v = {}
    for t, (a, b, c) in enumerate(tris):
        e1 = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        e2 = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        nx, ny, nz = e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]
        ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
        walk[t] = abs(ny) / ln >= REACH_SLOPE  # (either winding: DS collision faces up with negative cross)
        cen[t] = ((a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3)
        for p in (a, b, c):
            by_v.setdefault((round(p[0] / SNAP), round(p[1] / SNAP), round(p[2] / SNAP)), []).append(t)
    adj = [[] for _ in range(n)]
    for ts in by_v.values():
        if len(ts) < 2:
            continue
        for x in ts:
            cx = cen[x]
            for y in ts:
                if x != y:
                    cy = cen[y]
                    adj[x].append((y, math.sqrt((cx[0] - cy[0]) ** 2 + (cx[1] - cy[1]) ** 2 + (cx[2] - cy[2]) ** 2)))
    return walk, cen, adj


def _dijkstra(walk, adj, seeds, limit):
    """metres from the nearest seed triangle over walkable ones (walls reached get a distance, not walked through)"""
    dist = {}
    h = [(0.0, s) for s in seeds]
    heapq.heapify(h)
    while h:
        d, t = heapq.heappop(h)
        if t in dist:
            continue
        dist[t] = d
        if not walk[t] and d > 0:
            continue
        for u, w in adj[t]:
            nd = d + w
            if nd <= limit and u not in dist:
                heapq.heappush(h, (nd, u))
    return dist


def _in_rects(rs, p):
    return any(r[0] + SEAM_IN <= p[0] <= r[2] - SEAM_IN and r[1] + SEAM_IN <= p[2] <= r[3] - SEAM_IN for r in rs)


def block_reach(sub, i, nbr, rects, maps, cross):
    """(walkable per triangle, per triangle {block j: metres to j's seam} for j within REACH_MAX), triangles in block
    i's crop order (collision.bin's).
    sub: [(part, triangles)]; rects[j]: j's cells as xz rectangles or None (whole map); maps[j]: its MSB;
    cross: per triangle the cross-MSB load mask (worldlinks.cross_masks)"""
    tris = [t for _, ts in sub for t in ts]
    out = [dict() for _ in tris]
    if not tris:
        return [], out
    walk, _, adj = _graph(tris)
    for j in range(len(maps)):
        if j == i or not (nbr[i] >> j & 1):
            continue
        if maps[j] != maps[i]:
            seeds = [t for t in range(len(tris)) if cross[t] >> j & 1]
        elif rects[j] and REACH_SAME:
            seeds = [t for t, tri in enumerate(tris) if any(_in_rects(rects[j], p) for p in tri)]
        else:
            seeds = []
        if not seeds:
            continue
        for t, d in _dijkstra(walk, adj, seeds, REACH_MAX).items():
            out[t][j] = d
    return walk, out


def encode(dists):
    """a triangle's reach as world.bin stores it: (u32 mask, (u8 distance in REACH_STEP per set bit, low bit first));
    the REACH_N nearest blocks when more are within reach"""
    js = sorted(dists, key=lambda j: dists[j])[:REACH_N]
    mask = sum(1 << j for j in js)
    return mask, tuple(min(255, int(dists[j] / REACH_STEP)) for j in sorted(js))


# Seams inside one MSB already load the neighbour within 32 m in x / z (export_world.py LOAD_CELLS), more than dash
# speed covers in a lite set's turn-on time, and their reach runs were most of the table (Darkroot's six blocks: 31 k of
# 39 k runs at 4 m steps, world.bin 265 -> 592 KB): WORLD_REACH_SAME=1 adds them
REACH_SAME = os.environ.get("WORLD_REACH_SAME", "0") == "1"
REACH_N = 6  # distances per reach entry (world.bin REAC v2: u32 mask + 6 x u8)
