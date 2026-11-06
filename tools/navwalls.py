"""Walls of one collision part that seal a doorway another part leaves open (TECH_DEBT collision-6).

Some places carry two collision parts over the same volume, each in its own draw groups: Undead Burg's Taurus tower
is h1118B1 (stairs, open doorway from the ground floor landing) and h1119B1 (the same tower without the stairs and
with that doorway walled up, a 0.35 x 2.2 m panel at (53.4, 0.9 .. 3.1, -113.7), but with the only floor of the
landing in front of it). DSR's navmesh walks through the panel and players do too; the port keeps every collision
part active at once (collision-6), so the panel blocked the only way up to the Taurus Demon.

Rule (ASSUMED, from the data; how DSR switches such parts is not decoded): a wall triangle of part X goes when
  - a navmesh connection (two navmesh triangles sharing an edge, centroids within 1 m of height) crosses it at 0.6 m
    or 1.3 m above the navmesh, and
  - another part Y (not X) has a walkable floor right under the crossing point, above the wall's bottom and at least
    0.3 m below the crossing (Y's floor runs on through X's wall: the stairs' first step inside the panel), and
  - the wall triangle is at most PANEL_H tall (a door-sized panel, never a cliff face a navmesh edge cuts past).
Only solid parts (HitFilterID 0 / 8) count. m10_01: the 4 panel triangles of h1119B1 and nothing else (2026-11-07,
tools/navwalls.py m10_01_00_00 lists what it drops)."""
import collections, math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np

PANEL_H = 4.0     # m: tallest wall triangle the rule drops
HEIGHTS = (0.6, 1.3)
CELL = 2.0


def _grid(lo, hi):
    g = collections.defaultdict(list)
    for i in range(len(lo)):
        for gx in range(math.floor(lo[i, 0] / CELL), math.floor(hi[i, 0] / CELL) + 1):
            for gz in range(math.floor(lo[i, 2] / CELL), math.floor(hi[i, 2] / CELL) + 1):
                g[(gx, gz)].append(i)
    return g


def _seg_tris(p, q, A):
    """Parameters t in (0, 1) where segment p -> q crosses triangles A (n, 3, 3); nan where it does not."""
    e1, e2, d = A[:, 1] - A[:, 0], A[:, 2] - A[:, 0], q - p
    h = np.cross(d, e2)
    det = (e1 * h).sum(1)
    ok = np.abs(det) > 1e-12
    f = np.where(ok, 1 / np.where(ok, det, 1), 0)
    s = p - A[:, 0]
    u = f * (s * h).sum(1)
    qq = np.cross(s, e1)
    v = f * (qq * d).sum(1)
    t = f * (qq * e2).sum(1)
    hit = ok & (u >= 0) & (u <= 1) & (v >= 0) & (u + v <= 1) & (t > 0) & (t < 1)
    return np.where(hit, t, np.nan)


def _floor_y(x, z, A):
    """Height of floor triangles A (n, 3, 3) at (x, z); nan outside."""
    v0, v1 = A[:, 2] - A[:, 0], A[:, 1] - A[:, 0]
    d00 = v0[:, 0] ** 2 + v0[:, 2] ** 2
    d01 = v0[:, 0] * v1[:, 0] + v0[:, 2] * v1[:, 2]
    d11 = v1[:, 0] ** 2 + v1[:, 2] ** 2
    x2, z2 = x - A[:, 0, 0], z - A[:, 0, 2]
    d20, d21 = x2 * v0[:, 0] + z2 * v0[:, 2], x2 * v1[:, 0] + z2 * v1[:, 2]
    den = d00 * d11 - d01 * d01
    ok = np.abs(den) > 1e-12
    den = np.where(ok, den, 1)
    u, v = (d11 * d20 - d01 * d21) / den, (d00 * d21 - d01 * d20) / den
    inside = ok & (u > -1e-4) & (v > -1e-4) & (u + v < 1 + 1e-4)
    return np.where(inside, A[:, 0, 1] + u * v0[:, 1] + v * v1[:, 1], np.nan)


def sealed(game, area, subs, log=None):
    """{(index into subs, triangle index)} of the panel triangles to drop (subs: havok_collision.map_collision)."""
    import navroute
    try:
        nv, nt = navroute.load(game, area)
    except (OSError, KeyError):
        return set()
    if not nt:
        return set()
    NV, NT = np.array(nv), np.array(nt)
    walls, wkey, floors, fpart = [], [], [], []
    for si, s in enumerate(subs):
        if s["hit_filter"] not in (0, 8) or not s["tris"]:
            continue
        A = np.array(s["tris"], dtype=np.float64)
        n = np.cross(A[:, 1] - A[:, 0], A[:, 2] - A[:, 0])
        ny = np.abs(n[:, 1]) / (np.linalg.norm(n, axis=1) + 1e-12)
        for ti in np.nonzero((ny < 0.5) & (A[:, :, 1].max(1) - A[:, :, 1].min(1) <= PANEL_H))[0]:
            walls.append(A[ti])
            wkey.append((si, int(ti)))
        f = ny >= 0.7
        floors.append(A[f])
        fpart += [s["part"]] * int(f.sum())
    if not walls or not fpart:
        return set()
    W, F = np.array(walls), np.concatenate(floors)
    wpart = np.array([subs[si]["part"] for si, _ in wkey])
    fpart = np.array(fpart)
    wg, fg = _grid(W.min(1), W.max(1)), _grid(F.min(1), F.max(1))
    cen = NV[NT].mean(1)
    edges = collections.defaultdict(list)
    for ti, (a, b, c) in enumerate(NT):
        for u, v in ((a, b), (b, c), (c, a)):
            edges[(min(u, v), max(u, v))].append(ti)
    out = set()
    for ts in edges.values():
        if len(ts) != 2:
            continue
        P, Q = cen[ts[0]], cen[ts[1]]
        if abs(P[1] - Q[1]) > 1.0:
            continue
        cand = sorted(set(wg.get((math.floor(P[0] / CELL), math.floor(P[2] / CELL)), [])) |
                      set(wg.get((math.floor(Q[0] / CELL), math.floor(Q[2] / CELL)), [])))
        if not cand:
            continue
        cand = np.array(cand)
        for hgt in HEIGHTS:
            p, q = P + (0, hgt, 0), Q + (0, hgt, 0)
            t = _seg_tris(p, q, W[cand])
            for i, ti in zip(cand[~np.isnan(t)], t[~np.isnan(t)]):
                if wkey[i] in out:
                    continue
                X = p + (q - p) * ti
                fc = fg.get((math.floor(X[0] / CELL), math.floor(X[2] / CELL)), [])
                fc = [j for j in fc if fpart[j] != wpart[i]]
                if not fc:
                    continue
                y = _floor_y(X[0], X[2], F[fc])
                if np.any((y > W[i][:, 1].min() - 0.05) & (y < X[1] - 0.3)):
                    out.add(wkey[i])
                    if log:
                        log(f"navwalls: {area} {wpart[i]} wall at ({X[0]:.1f}, {X[1]:.1f}, {X[2]:.1f}) dropped "
                            f"(navmesh {P[0]:.1f},{P[1]:.1f},{P[2]:.1f} -> {Q[0]:.1f},{Q[1]:.1f},{Q[2]:.1f} crosses it onto {fpart[fc[int(np.nanargmin(np.abs(y - X[1])))]]})")
    return out


if __name__ == "__main__":
    import gamepath, havok_collision
    game = gamepath.game_dir()
    for area in sys.argv[1:]:
        subs = havok_collision.map_collision(game, area)[0]
        print(area, len(sealed(game, area, subs, log=print)), "triangles")
