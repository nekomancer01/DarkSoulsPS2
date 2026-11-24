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
    if _jit() is not None:  # the same loop compiled (Numba); its own events in the loop's order
        return _sealed_jit(W, F, wkey, wpart, fpart, wg, fg, cen, edges, area, log)
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


# ---- Numba path (2026-11-24 build profile: sealed() was ~13 % of a block's export_map, a Python loop over navmesh
# edges with small numpy calls per edge). The kernel runs the same loop over the same candidates in the same order
# with the same scalar arithmetic as _seg_tris / _floor_y (np.cross's terms, numpy's left-to-right sums of three; no
# fastmath, so no fused multiply-adds): the same walls. NAVWALLS_JIT=0, or no numba: the loop above.
_JIT = []


def _jit():
    if not _JIT:
        k = None
        if os.environ.get("NAVWALLS_JIT", "1") != "0":
            try:
                import numba
                k = numba.njit(cache=True)(_kernel)
            except Exception:  # no numba (or no compiler for this platform): the Python loop
                k = None
        _JIT.append(k)
    return _JIT[0]


def _csr(g):
    """grid dict {(gx, gz): [i...]} -> (sorted packed keys, starts, items)"""
    ks = sorted(g)
    keys = np.array([(gx << 32) + (gz & 0xFFFFFFFF) for gx, gz in ks], np.int64) if ks else np.zeros(0, np.int64)
    o = np.argsort(keys, kind="stable")
    st, it = [0], []
    for j in o:
        it += g[ks[j]]
        st.append(len(it))
    return keys[o], np.array(st, np.int64), np.array(it, np.int64)


def _kernel(W, Wmy, F, wpart, fpart, wk, ws, wi, fk, fs, fi, cen, E, heights, cell):
    """per wall triangle: dropped (1) or not; events (wall, edge, height index, x, y, z, floor triangle) in order"""
    nw = W.shape[0]
    out = np.zeros(nw, np.uint8)
    ev = np.empty((max(nw, 1), 6))  # per dropped wall in drop order: wall, edge, x, y, z, floor triangle
    nev = 0
    for e in range(E.shape[0]):
        P = cen[E[e, 0]]
        Q = cen[E[e, 1]]
        if abs(P[1] - Q[1]) > 1.0:
            continue
        # candidates: the sorted union of the walls in P's and Q's cells
        cp = (np.int64(math.floor(P[0] / cell)) << 32) + (np.int64(math.floor(P[2] / cell)) & 0xFFFFFFFF)
        cq = (np.int64(math.floor(Q[0] / cell)) << 32) + (np.int64(math.floor(Q[2] / cell)) & 0xFFFFFFFF)
        a0 = a1 = b0 = b1 = 0
        k = np.searchsorted(wk, cp)
        if k < wk.shape[0] and wk[k] == cp:
            a0, a1 = ws[k], ws[k + 1]
        k = np.searchsorted(wk, cq)
        if k < wk.shape[0] and wk[k] == cq:
            b0, b1 = ws[k], ws[k + 1]
        cand = np.empty(a1 - a0 + b1 - b0, np.int64)
        nc = 0
        i, j = a0, b0
        while i < a1 or j < b1:
            if j >= b1 or (i < a1 and wi[i] < wi[j]):
                v = wi[i]
                i += 1
            elif i >= a1 or wi[j] < wi[i]:
                v = wi[j]
                j += 1
            else:
                v = wi[i]
                i += 1
                j += 1
            cand[nc] = v
            nc += 1
        if nc == 0:
            continue
        for hi in range(heights.shape[0]):
            hgt = heights[hi]
            p0, p1, p2 = P[0] + 0.0, P[1] + hgt, P[2] + 0.0
            q0, q1, q2 = Q[0] + 0.0, Q[1] + hgt, Q[2] + 0.0
            d0, d1, d2 = q0 - p0, q1 - p1, q2 - p2
            for ci in range(nc):
                w = cand[ci]
                A = W[w]
                e10, e11, e12 = A[1, 0] - A[0, 0], A[1, 1] - A[0, 1], A[1, 2] - A[0, 2]
                e20, e21, e22 = A[2, 0] - A[0, 0], A[2, 1] - A[0, 1], A[2, 2] - A[0, 2]
                h0 = d1 * e22 - d2 * e21
                h1 = d2 * e20 - d0 * e22
                h2 = d0 * e21 - d1 * e20
                det = e10 * h0 + e11 * h1 + e12 * h2
                if not abs(det) > 1e-12:
                    continue
                f = 1 / det
                s0, s1, s2 = p0 - A[0, 0], p1 - A[0, 1], p2 - A[0, 2]
                u = f * (s0 * h0 + s1 * h1 + s2 * h2)
                qq0 = s1 * e12 - s2 * e11
                qq1 = s2 * e10 - s0 * e12
                qq2 = s0 * e11 - s1 * e10
                vv = f * (qq0 * d0 + qq1 * d1 + qq2 * d2)
                t = f * (qq0 * e20 + qq1 * e21 + qq2 * e22)
                if not (u >= 0 and u <= 1 and vv >= 0 and u + vv <= 1 and t > 0 and t < 1):
                    continue
                if out[w]:
                    continue
                X0, X1, X2 = p0 + d0 * t, p1 + d1 * t, p2 + d2 * t
                cf = (np.int64(math.floor(X0 / cell)) << 32) + (np.int64(math.floor(X2 / cell)) & 0xFFFFFFFF)
                k = np.searchsorted(fk, cf)
                if not (k < fk.shape[0] and fk[k] == cf):
                    continue
                hit = False
                best, bestj = np.inf, -1
                for m in range(fs[k], fs[k + 1]):
                    fj = fi[m]
                    if fpart[fj] == wpart[w]:
                        continue
                    B = F[fj]
                    v00, v01, v02 = B[2, 0] - B[0, 0], B[2, 1] - B[0, 1], B[2, 2] - B[0, 2]
                    v10, v11, v12 = B[1, 0] - B[0, 0], B[1, 1] - B[0, 1], B[1, 2] - B[0, 2]
                    d00 = v00 * v00 + v02 * v02
                    d01 = v00 * v10 + v02 * v12
                    d11 = v10 * v10 + v12 * v12
                    x2, z2 = X0 - B[0, 0], X2 - B[0, 2]
                    d20, d21 = x2 * v00 + z2 * v02, x2 * v10 + z2 * v12
                    den = d00 * d11 - d01 * d01
                    if not abs(den) > 1e-12:
                        continue
                    uu, vf = (d11 * d20 - d01 * d21) / den, (d00 * d21 - d01 * d20) / den
                    if not (uu > -1e-4 and vf > -1e-4 and uu + vf < 1 + 1e-4):
                        continue
                    y = B[0, 1] + uu * v01 + vf * v11
                    if abs(y - X1) < best:
                        best, bestj = abs(y - X1), fj
                    if y > Wmy[w] - 0.05 and y < X1 - 0.3:
                        hit = True
                if hit:
                    out[w] = 1
                    ev[nev, 0], ev[nev, 1], ev[nev, 2], ev[nev, 3], ev[nev, 4], ev[nev, 5] = w, e, X0, X1, X2, bestj
                    nev += 1
    return out, ev[:nev]


def _sealed_jit(W, F, wkey, wpart, fpart, wg, fg, cen, edges, area, log):
    pid = {}
    wp = np.array([pid.setdefault(p, len(pid)) for p in wpart.tolist()], np.int64)
    fp = np.array([pid.setdefault(p, len(pid)) for p in fpart.tolist()], np.int64)
    names = {v: k for k, v in pid.items()}
    wk, ws, wi = _csr(wg)
    fk, fs, fi = _csr(fg)
    E = np.array([ts for ts in edges.values() if len(ts) == 2], np.int64).reshape(-1, 2)
    out, ev = _JIT[0](np.ascontiguousarray(W, np.float64), W[:, :, 1].min(1),
                                          np.ascontiguousarray(F, np.float64), wp, fp, wk, ws, wi, fk, fs, fi,
                                          np.ascontiguousarray(cen, np.float64), E, np.array(HEIGHTS, np.float64),
                                          CELL)
    if log:
        for row in ev:
            w, e, X, fj = int(row[0]), int(row[1]), row[2:5], int(row[5])
            P, Q = cen[E[e, 0]], cen[E[e, 1]]
            log(f"navwalls: {area} {wpart[w]} wall at ({X[0]:.1f}, {X[1]:.1f}, {X[2]:.1f}) dropped "
                f"(navmesh {P[0]:.1f},{P[1]:.1f},{P[2]:.1f} -> {Q[0]:.1f},{Q[1]:.1f},{Q[2]:.1f} crosses it onto {names[int(fp[fj])]})")
    return {wkey[w] for w in np.flatnonzero(out)}


if __name__ == "__main__":
    import gamepath, havok_collision
    game = gamepath.game_dir()
    for area in sys.argv[1:]:
        subs = havok_collision.map_collision(game, area)[0]
        print(area, len(sealed(game, area, subs, log=print)), "triangles")
