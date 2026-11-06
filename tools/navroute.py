#!/usr/bin/env python3
"""Walking route between points of one map for a test script (ps2/proto/tests/*.txt), from the game's navmesh.

  navroute.py <map_id> x,y,z x,y,z [x,y,z ...] [--step 4] [--game DIR]

Loads every MSB navmesh part (type 8) of the map (souls.nvm models of map/<map>/<map>.nvmbnd, placed by the part's
position / rotation / scale, border vertices welded as tools/export_enemies.navmesh does), joins triangles that share
an edge, and runs A* over triangle centroids from each point to the next. Navmesh parts that do not share border
vertices (ladders, drops, doors the walker cannot take) are joined where two triangles of different connected pieces
lie within LINK_D (cost x LINK_COST): the route crosses those with a `spawn` (teleport onto the floor there) instead
of walking. Prints test-script lines: `approach x,z 0.8 1` every --step metres of walking, `spawn x y z yaw` + `wait 0.5`
over a link or a height step above STEP_Y (the walker cannot climb it), and `# waypoint i` markers at the given points.
Reads the player's own copy of the game (gamepath); writes nothing."""
import argparse, heapq, math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls, gamepath
import havok_anim as ha

WELD = 0.02       # m (export_enemies.NAV_WELD)
LINK_D = 3.0      # m between centroids of triangles of two unconnected navmesh pieces
LINK_COST = 4.0
STEP_Y = 0.7      # m: a height step between two route points above STEP_Y + SLOPE x their xz distance becomes a spawn
SLOPE = 1.0       # (the walker cannot climb it: ladders, ledges; stairs rise < 1 m per m)


def load(game, map_id):
    M = souls.msb1(souls.read(f"{game}/map/MapStudio/{map_id}.msb"))
    models = {souls.basename(n)[:-4]: d for _, n, d in souls.bnd3(souls.read(f"{game}/map/{map_id}/{map_id}.nvmbnd.dcx"))}
    weld, verts, tris = {}, [], []
    for part in M["parts"]:
        key = f"{part['model']}A{map_id[1:3]}" if part["type"] == 8 else None
        if key not in models:
            continue
        q, sc, t = souls._euler_deg_xzy(*part["rot"]), part["scale"], part["pos"]
        nv, nt = souls.nvm(models[key])
        world = []
        for p in nv:
            r = ha.qrot(q, (p[0] * sc[0], p[1] * sc[1], p[2] * sc[2]))
            world.append((r[0] + t[0], r[1] + t[1], r[2] + t[2]))
        for a, b, c, _ in nt:
            idx = []
            for p in (world[a], world[b], world[c]):
                k = tuple(round(x / WELD) for x in p)
                if k not in weld:
                    weld[k] = len(verts)
                    verts.append(p)
                idx.append(weld[k])
            if len(set(idx)) == 3:
                tris.append(tuple(idx))
    return verts, tris


def graph(verts, tris):
    cen = [tuple(sum(verts[i][a] for i in t) / 3 for a in range(3)) for t in tris]
    edge, adj = {}, [[] for _ in tris]
    for ti, (a, b, c) in enumerate(tris):
        for u, v in ((a, b), (b, c), (c, a)):
            edge.setdefault((min(u, v), max(u, v)), []).append(ti)
    for ts in edge.values():
        for i in ts:
            for j in ts:
                if i != j:
                    adj[i].append((j, math.dist(cen[i], cen[j]), 0))
    comp = [-1] * len(tris)  # connected pieces
    for s in range(len(tris)):
        if comp[s] < 0:
            comp[s], st = s, [s]
            while st:
                i = st.pop()
                for j, _, _ in adj[i]:
                    if comp[j] < 0:
                        comp[j] = s
                        st.append(j)
    grid = {}
    for i, p in enumerate(cen):
        grid.setdefault((math.floor(p[0] / LINK_D), math.floor(p[2] / LINK_D)), []).append(i)
    for i, p in enumerate(cen):
        gx, gz = math.floor(p[0] / LINK_D), math.floor(p[2] / LINK_D)
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for j in grid.get((gx + dx, gz + dz), ()):
                    if comp[j] != comp[i]:
                        d = math.dist(p, cen[j])
                        if d < LINK_D:
                            adj[i].append((j, d * LINK_COST, 1))
    return cen, adj


def nearest(cen, p):
    return min(range(len(cen)), key=lambda i: (cen[i][0] - p[0]) ** 2 + 4 * (cen[i][1] - p[1]) ** 2 + (cen[i][2] - p[2]) ** 2)


def astar(cen, adj, s, g):
    h = lambda i: math.dist(cen[i], cen[g])
    best, prev, pq = {s: 0.0}, {s: None}, [(h(s), 0.0, s)]
    while pq:
        _, d, i = heapq.heappop(pq)
        if i == g:
            break
        if d > best[i]:
            continue
        for j, w, link in adj[i]:
            nd = d + w
            if nd < best.get(j, 1e30):
                best[j], prev[j] = nd, (i, link)
                heapq.heappush(pq, (nd + h(j), nd, j))
    if g not in prev:
        return None
    path, i = [], g
    while i is not None:
        p = prev[i]
        path.append((i, p[1] if p else 0))
        i = p[0] if p else None
    return path[::-1]


def emit(cen, path, step, out):
    last = cen[path[0][0]]
    for n, (i, link) in enumerate(path[1:], 1):
        p = cen[i]
        jump = link or abs(p[1] - last[1]) > STEP_Y + SLOPE * math.hypot(p[0] - last[0], p[2] - last[2])
        if jump:
            yaw = math.atan2(p[0] - last[0], p[2] - last[2])
            out.append(f"spawn {p[0]:.1f} {p[1]:.1f} {p[2]:.1f} {yaw:.2f}")
            out.append("wait 0.5")
            last = p
        elif math.hypot(p[0] - last[0], p[2] - last[2]) >= step or n == len(path) - 1:
            if abs(p[1] - last[1]) > STEP_Y + SLOPE * math.hypot(p[0] - last[0], p[2] - last[2]):
                out.append(f"# step {abs(p[1] - last[1]):.1f} m")
                yaw = math.atan2(p[0] - last[0], p[2] - last[2])
                out.append(f"spawn {p[0]:.1f} {p[1]:.1f} {p[2]:.1f} {yaw:.2f}")
                out.append("wait 0.5")
            else:
                out.append(f"approach {p[0]:.1f},{p[2]:.1f} 0.8 1")
            last = p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map_id")
    ap.add_argument("points", nargs="+")
    ap.add_argument("--step", type=float, default=4.0)
    ap.add_argument("--game")
    a = ap.parse_args()
    game = gamepath.game_dir(a.game)
    verts, tris = load(game, a.map_id)
    cen, adj = graph(verts, tris)
    pts = [tuple(float(v) for v in s.split(",")) for s in a.points]
    out = []
    for w, (p0, p1) in enumerate(zip(pts, pts[1:])):
        s, g = nearest(cen, p0), nearest(cen, p1)
        path = astar(cen, adj, s, g)
        if not path:
            sys.exit(f"navroute: no path from {p0} to {p1}")
        out.append(f"# waypoint {w}: {p0} -> {p1}, {len(path)} triangles, {sum(l for _, l in path)} links")
        if w == 0:
            c = cen[s]
            out.append(f"spawn {c[0]:.1f} {c[1]:.1f} {c[2]:.1f} 0")
            out.append("wait 0.5")
        emit(cen, path, a.step, out)
    print("\n".join(out))


if __name__ == "__main__":
    main()
