"""Quadric-error edge-collapse simplification for the map exporter (pure Python).

Half-edge collapses (vertex u moves onto its neighbour v): every surviving vertex keeps its original position, so
nothing is invented (no shards) and flat floors stay exactly flat. Cost of a collapse: the larger of
  - the Garland-Heckbert quadric error (area-weighted planes, plus edge quadrics holding borders and UV seams in
    place), normalized by the quadric weight = mean squared distance, and
  - the square of a bound on how far the surface around v has moved so far (distance of u from the new triangles
    and border edges, accumulated per vertex): the mean alone dilutes over large merged regions and let thin things
    lying on a floor sink below it,
  - AREA_K x the square root of the surface area lost around v so far (|area before - after| of each collapse,
    summed per vertex): thin parts (branches, cards, window frames, railings) lie within a few cm of a plane through
    them, so the distances above let them shrink to nothing; their lost area is their screen size,
times a per-vertex scale from the caller (1 / view distance^2: angular error^2).

Topology works on position vertices (vertices welded by position, across meshes if the caller groups them). Each
triangle corner also carries an attribute index (an original vertex: UV, colour, normal; its mesh). When u moves onto
v, a corner of u takes the attributes of v from a triangle on edge uv where that is continuous (same chart);
otherwise the corner keeps its own UV / colour (per corner, as the old clustering did) and the collapse pays
SEAM_K^2 x the fraction of u's region that tears, so UV / material seams only give way where the screen error is
small anyway. Rules, meshoptimizer-style:
  - locked vertices (border vertices the caller flags, non-manifold edges, bow ties) never move; others may
    collapse onto them;
  - a border vertex only moves along a border edge onto the next border vertex (open edges stay where they are);
  - link condition (no non-manifold folds), no triangle normal turning by more than ~75 degrees, no zero slivers.

simplify() runs the collapses to the end and returns a log; replay(log, eps) gives the triangles after every collapse
whose cost (running maximum) is <= eps, so one run serves any triangle budget."""
import heapq
import math

BORDER_W = 20.0  # edge quadric weights (x edge length^2)
SEAM_W = 4.0
SEAM_K = 0.05  # metres: tearing a seam costs like this much (mean) geometric error
AREA_K = float(__import__("os").environ.get("SIMPLIFY_AREA_K", 1.0))  # 0: area loss ignored (A/B)
MIN_COS = 0.25  # a triangle's normal may not turn more than acos(0.25) = 75 degrees in one collapse


def _plane(n, p, w):
    a, b, c = n
    d = -(a * p[0] + b * p[1] + c * p[2])
    return [w * a * a, w * a * b, w * a * c, w * a * d, w * b * b, w * b * c, w * b * d, w * c * c, w * c * d,
            w * d * d, w]


def _qadd(q, r):
    for i in range(11):
        q[i] += r[i]


def _qeval(q, p):
    x, y, z = p
    return (q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z
            + 2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9])


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _area(p):
    n = _cross(_sub(p[1], p[0]), _sub(p[2], p[0]))
    return 0.5 * math.sqrt(_dot(n, n))


def simplify(pos, wpos, tris, locked, scale, area_k=None):
    """pos: position-vertex coordinates; wpos[w]: position vertex of attribute vertex w; tris: attribute-vertex
    triples; locked[p]: never move p if it is on a border (the caller flags positions shared with other meshes:
    interior points may still move, open edges that meet another mesh stay welded); scale[p]: cost multiplier (screen weight).
    Returns (tris, steps): the input triangles minus degenerate ones as ((p, a) x 3) corners, and steps
    [(cost, removed triangle ids, ((tri, corner, p, a), ...))], cost = running maximum."""
    T0 = [tuple((wpos[w], w) for w in t) for t in tris if len({wpos[w] for w in t}) == 3]
    TP = [[c[0] for c in t] for t in T0]  # corner position vertex
    TA = [[c[1] for c in t] for t in T0]  # corner attributes
    nv = len(pos)
    vt = [set() for _ in range(nv)]
    for i, t in enumerate(TP):
        for p in t:
            vt[p].add(i)
    # edge -> triangles, for border / seam / non-manifold classification
    et = {}
    for i, t in enumerate(TP):
        for k in range(3):
            a, b = t[k], t[(k + 1) % 3]
            et.setdefault((a, b) if a < b else (b, a), []).append(i)
    lock = [False] * nv
    border = [False] * nv
    bnb = [None] * nv  # border neighbours of border vertices
    Q = [[0.0] * 11 for _ in range(nv)]
    for i, t in enumerate(TP):
        p = [pos[x] for x in t]
        n = _cross(_sub(p[1], p[0]), _sub(p[2], p[0]))
        ln = math.sqrt(_dot(n, n))
        if ln < 1e-12:
            continue
        n = (n[0] / ln, n[1] / ln, n[2] / ln)
        q = _plane(n, p[0], ln * 0.5)
        for x in t:
            _qadd(Q[x], q)
    for (a, b), ts in et.items():
        if len(ts) > 2:
            lock[a] = lock[b] = True
            continue
        if len(ts) == 2:
            # seam: the two triangles use different attributes at an endpoint
            w0 = dict(zip(TP[ts[0]], TA[ts[0]]))
            w1 = dict(zip(TP[ts[1]], TA[ts[1]]))
            if w0[a] == w1[a] and w0[b] == w1[b]:
                continue
            wgt = SEAM_W
        else:
            border[a] = border[b] = True
            bnb[a] = (bnb[a] or set()) | {b}
            bnb[b] = (bnb[b] or set()) | {a}
            wgt = BORDER_W
        p = [pos[x] for x in TP[ts[0]]]
        fn = _cross(_sub(p[1], p[0]), _sub(p[2], p[0]))
        e = _sub(pos[b], pos[a])
        en = _cross(e, fn)
        ln = math.sqrt(_dot(en, en))
        if ln < 1e-12:
            continue
        en = (en[0] / ln, en[1] / ln, en[2] / ln)
        q = _plane(en, pos[a], wgt * _dot(e, e))
        _qadd(Q[a], q)
        _qadd(Q[b], q)
    for p in range(nv):
        if bnb[p] and (len(bnb[p]) > 2 or locked[p]):
            lock[p] = True  # several border loops meet (bow tie), or the border meets another mesh

    dead = [False] * nv
    errm = [0.0] * nv  # bound on how far the surface around each vertex has moved
    alost = [0.0] * nv  # surface area lost around each vertex (AREA_K)
    lost = [0.0]  # attr_map's last |area change|
    ver = [0] * nv
    heap = []
    seam_k2 = SEAM_K * SEAM_K
    ak2 = (AREA_K if area_k is None else area_k) ** 2

    def neighbours(u):
        s = set()
        for t in vt[u]:
            s.update(TP[t])
        s.discard(u)
        return s

    def attr_map(u, v):
        """-> (attribute map of u's corners through the triangles on edge uv, or None if inconsistent; triangles on
        uv; other triangles of u; area of those other triangles whose u corner has no continuous attributes;
        distance of u from the surface after the collapse: the planes of its moved triangles and, on a border, the
        new border edges)"""
        removed, keep, amap = [], [], {}
        # ascending triangle ids: the sums below (area lost, torn area) and the step records then do not depend on
        # set iteration order, which lets tools/native/simplify.c reproduce every step exactly
        for t in sorted(vt[u]):
            tp = TP[t]
            if v in tp:
                removed.append(t)
                au, av = TA[t][tp.index(u)], TA[t][tp.index(v)]
                if amap.setdefault(au, av) != av:
                    return None, removed, keep, 0.0, 0.0
            else:
                keep.append(t)
        torn = dloc = 0.0
        pu, pv = pos[u], pos[v]
        da = sum(_area([pos[x] for x in TP[t]]) for t in removed)  # area lost: removed + (old - new) of the kept
        du = _sub(pu, pv)
        for t in keep:
            tp = TP[t]
            p = [pos[x] for x in tp]
            a0 = _area(p)
            if TA[t][tp.index(u)] not in amap:
                torn += a0
            p[tp.index(u)] = pv
            da += a0 - _area(p)
            n = _cross(_sub(p[1], p[0]), _sub(p[2], p[0]))
            ln = _dot(n, n)
            if ln > 0:
                dloc = max(dloc, abs(_dot(n, du)) / math.sqrt(ln))
        if border[u]:
            for x in bnb[u]:
                if x != v:
                    e = _sub(pos[x], pv)
                    le = _dot(e, e)
                    if le > 0:
                        c = _cross(du, e)
                        dloc = max(dloc, math.sqrt(_dot(c, c) / le))
        lost[0] = abs(da)
        return amap, removed, keep, torn, dloc

    def push(u, v):
        if lock[u]:
            return
        if border[u] and v not in bnb[u]:
            return
        amap, _, _, torn, dloc = attr_map(u, v)
        if amap is None:
            return
        w = Q[u][10] + Q[v][10]
        e = (_qeval(Q[u], pos[v]) + _qeval(Q[v], pos[v]) + seam_k2 * torn) / w if w > 0 else 0.0
        # mean quadric error, but never below the bound on how far the surface moved (the mean dilutes over large
        # merged regions, which let thin things on a floor sink below it)
        b = max(errm[v], errm[u] + dloc)
        c = max(e, b * b, ak2 * (alost[u] + alost[v] + lost[0])) * max(scale[u], scale[v])
        heapq.heappush(heap, (c, u, v, ver[u], ver[v]))

    for (a, b) in et:
        push(a, b)
        push(b, a)

    steps = []
    runmax = 0.0
    while heap:
        c, u, v, vu, vv = heapq.heappop(heap)
        if dead[u] or dead[v] or ver[u] != vu or ver[v] != vv:
            continue
        amap, removed, keep, _, dloc = attr_map(u, v)
        if amap is None or not removed:
            continue
        # link condition: common neighbours of u and v are exactly the opposite corners of the removed triangles
        opp = set()
        for t in removed:
            opp.update(TP[t])
        opp.discard(u)
        opp.discard(v)
        if neighbours(u) & neighbours(v) != opp:
            continue
        pv = pos[v]
        changes = []
        ok = True
        for t in keep:
            tp = TP[t]
            p = [pos[x] for x in tp]
            old = _cross(_sub(p[1], p[0]), _sub(p[2], p[0]))
            k = tp.index(u)
            a = TA[t][k]
            changes.append((t, k, v, amap.get(a, a)))
            p[k] = pv
            new = _cross(_sub(p[1], p[0]), _sub(p[2], p[0]))
            lo, ln = _dot(old, old), _dot(new, new)
            if ln <= 1e-14 * max(lo, 1e-12) or _dot(old, new) < MIN_COS * math.sqrt(lo * ln):
                ok = False
                break
        if not ok:
            continue
        # apply
        for t in removed:
            for x in TP[t]:
                vt[x].discard(t)
        for t, k, p, a in changes:
            TP[t][k] = p
            TA[t][k] = a
            vt[v].add(t)
        vt[u].clear()
        dead[u] = True
        _qadd(Q[v], Q[u])
        errm[v] = max(errm[v], errm[u] + dloc)
        alost[v] += alost[u] + lost[0]
        ver[v] += 1
        if border[u]:
            for x in bnb[u]:
                bnb[x].discard(u)
                if x != v:
                    bnb[x].add(v)
                    bnb[v].add(x)
        runmax = max(runmax, c)
        steps.append((runmax, tuple(removed), tuple(changes)))
        # costs of the edges at v changed (its quadric, its triangles' seams)
        for x in neighbours(v):
            push(x, v)
            push(v, x)
    return T0, steps


def replay(tris, steps, eps):
    """Triangles ((p, a) x 3) after every step with cost <= eps."""
    T = [list(t) for t in tris]
    alive = [True] * len(T)
    for c, removed, changes in steps:
        if c > eps:
            break
        for t in removed:
            alive[t] = False
        for t, k, p, a in changes:
            T[t][k] = (p, a)
    return [tuple(t) for t, a in zip(T, alive) if a]


def run(args, area_k=None):
    """multiprocessing entry: simplify(*args)."""
    return simplify(*args, area_k=area_k)
