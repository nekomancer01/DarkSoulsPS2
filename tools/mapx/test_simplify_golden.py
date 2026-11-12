"""Golden outputs of the map simplifier at the settings the export ships (also run by CI's offline checks).

tools/native/simplify.c is the simplifier's source of truth: the shape envelope, the triangle guard and its validation
pass (export_map.py SIMPLIFY_ENV_*, SIMPLIFY_GUARD, SIMPLIFY_VALIDATE) and the colour refit exist only in C.
tools/simplify.py is the frozen reference of the core collapse (every C-only rule off), which
tools/mapx/test_mapx.py and tools/simplify_native.py --test compare the C code against. This file pins what the
shipped path itself gives: fixed synthetic scenes (no game data) through

  core    simplify.c alone (simplify_native, simplify_run_grp) with the near and the coarse level's rules, and off
  export  export_map.simplify_meshes (libmapx: groups, cells, budget solve, guard, validation, refit) the way
          export_map.gather calls it for the coarse and the near level

and compares a hash of each output (and its triangle count) with tools/mapx/simplify_golden.json. The scenes come
from a generator written out here (splitmix64), not numpy's, so they stay the same on any numpy.

A change to the simplifier, its settings or export_map's simplify path that changes an output fails here on
purpose: check the new output (export a map, look at it, run the world budget gate), then record it with
  python3 tools/mapx/test_simplify_golden.py --update
and commit the JSON with the change, saying in the commit what moved and why.
Usage: test_simplify_golden.py [--update] [--verbose]"""
import hashlib, json, math, os, sys

# the shipped settings are export_map's / simplify.py's defaults: no environment overrides
for k in list(os.environ):
    if k.startswith(("SIMPLIFY_", "SIMP_", "WS_", "EPS", "MAPX", "KEEP_")) or k in ("STREAM_D", "BACKDROP_EPS", "CHUNK_GRID",
                                                                                 "ST_ALIGN", "INDEXED", "BUDGET"):
        del os.environ[k]
os.environ["SIMPLIFY_CACHE"] = "0"

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
import contextlib, io
import numpy as np
import export_map as em
import mapx_native
import simplify
import simplify_native

GOLDEN = os.path.join(HERE, "simplify_golden.json")


class Rng:
    """splitmix64: the same numbers on every machine and Python / numpy version"""

    def __init__(self, seed):
        self.s = (seed * 0x9E3779B97F4A7C15 + 0x632BE59BD9B4E5B) & 0xFFFFFFFFFFFFFFFF

    def u64(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        return z ^ (z >> 31)

    def random(self):  # [0, 1), 53 bits
        return (self.u64() >> 11) / 9007199254740992.0

    def integers(self, lo, hi):  # [lo, hi)
        return lo + self.u64() % (hi - lo)

    def uniform(self, lo, hi):
        return lo + (hi - lo) * self.random()

    def choice(self, xs):
        return xs[self.integers(0, len(xs))]


def lattice(rng, g, step, ox, oz, y0, bump):
    """a g x g height field: positions rounded to the mm (so a libm ulp never shows), its quads' corners"""
    pos, idx = [], {}
    for i in range(g):
        for k in range(g):
            x, z = (ox + i) * step, (oz + k) * step
            y = y0 + (round(math.sin(x * 0.3) + math.cos(z * 0.2) * bump, 3) if bump else 0.0)
            if rng.random() < 0.08:  # a step: ledges, kerbs
                y += 0.25
            idx[i, k] = len(pos)
            pos.append((x, y, z))
    return pos, idx


def scene(seed):
    """export_map.simplify_meshes' input: bumpy lattice patches that weld at shared edges, UV seams, holes, a wall
    standing on a floor, a thin board lying on it (the 2026-11-10 sliver), exact copies, kept and grass meshes, and a
    navmesh over the floor"""
    rng = Rng(seed)
    ms, step = [], rng.choice([1.0, 1.5, 2.5])
    for _ in range(rng.integers(2, 7)):
        g = rng.integers(4, 20)
        ox, oz = (rng.integers(-3, 3) * (g - 1), rng.integers(-3, 3) * (g - 1))
        pos, idx = lattice(rng, g, step, ox, oz, rng.integers(-1, 2) * 8.0, rng.random() < 0.7)
        nw = len(pos)
        wpos = list(range(nw)) + [rng.integers(0, nw) for _ in range(nw // 4)]
        seam = {p: nw + n for n, p in enumerate(wpos[nw:])}
        tris = []
        for i in range(g - 1):
            for k in range(g - 1):
                if rng.random() < 0.04:
                    continue
                a, b, c, d = idx[i, k], idx[i + 1, k], idx[i + 1, k + 1], idx[i, k + 1]
                w = (lambda p: seam.get(p, p)) if (i + k) % 3 == 0 else (lambda p: p)
                tris += [(w(a), w(b), w(c)), (w(a), w(c), w(d))]
        m = dict(pos=pos, wpos=wpos, tris=tris, keep=rng.random() < 0.1, grass=rng.random() < 0.08, ti=rng.integers(0, 3),
                 wrgb=[(rng.integers(0, 256), rng.integers(0, 256), rng.integers(0, 256)) for _ in wpos])
        ms.append(m)
        if rng.random() < 0.25:  # an exact copy (drawn once)
            ms.append(dict(m, tris=list(reversed(m["tris"])), keep=False))
    # rubble: a patch of random heights (where the guard's flip and shape tests refuse collapses)
    g = rng.integers(6, 16)
    pos = [(i * 0.6 - 4.0, round(rng.uniform(-0.6, 0.6), 3), k * 0.6 - 4.0) for i in range(g) for k in range(g)]
    tris = [t for i in range(g - 1) for k in range(g - 1) for t in
            (((i * g + k), (i + 1) * g + k, (i + 1) * g + k + 1), (i * g + k, (i + 1) * g + k + 1, i * g + k + 1))]
    ms.append(dict(pos=pos, wpos=list(range(len(pos))), tris=tris, keep=False, grass=False, ti=0,
                   wrgb=[(100, 100, 100)] * len(pos)))
    # a wall: a vertical lattice standing on the first patch's floor
    h, n = rng.integers(3, 9), rng.integers(4, 14)
    pos = [(i * 0.75, j * 0.75, 0.3) for i in range(n) for j in range(h)]
    tris = [(i * h + j, (i + 1) * h + j, (i + 1) * h + j + 1) for i in range(n - 1) for j in range(h - 1)] + \
           [(i * h + j, (i + 1) * h + j + 1, i * h + j + 1) for i in range(n - 1) for j in range(h - 1)]
    ms.append(dict(pos=pos, wpos=list(range(len(pos))), tris=tris, keep=False, grass=False, ti=1,
                   wrgb=[(128, 120, 110)] * len(pos)))
    # a board: a thin strip 2 cm above the floor, its own material
    L = rng.integers(6, 20)
    pos = [(i * 0.5, 0.02, z) for i in range(L) for z in (2.0, 2.2)]
    tris = [(2 * i, 2 * i + 1, 2 * i + 3) for i in range(L - 1)] + [(2 * i, 2 * i + 3, 2 * i + 2) for i in range(L - 1)]
    ms.append(dict(pos=pos, wpos=list(range(len(pos))), tris=tris, keep=False, grass=False, ti=2,
                   wrgb=[(90, 60, 40)] * len(pos)))
    ms[0]["keep"] = False  # one live mesh at least
    nav_v = [(rng.uniform(-30, 30), rng.uniform(-1, 1), rng.uniform(-30, 30)) for _ in range(rng.integers(6, 40))]
    nav_t = [(rng.integers(0, len(nav_v)), rng.integers(0, len(nav_v)), rng.integers(0, len(nav_v)))
             for _ in range(rng.integers(4, 30))]
    spawn = (rng.uniform(-10, 10), 0.0, rng.uniform(-10, 10))
    return ms, nav_v, nav_t, spawn, rng


def h_update(h, x):
    """hash of an output: arrays by dtype, shape and bytes; Tris by their corner arrays; everything else by repr"""
    if isinstance(x, mapx_native.Tris):
        x = (x.P, x.W)
    if isinstance(x, np.ndarray):
        x = np.ascontiguousarray(x)
        h.update(f"{x.dtype.str}{x.shape}".encode())
        h.update(x.tobytes())
    elif isinstance(x, (list, tuple)):
        h.update(f"{type(x).__name__}{len(x)}[".encode())
        for v in x:
            h_update(h, v)
        h.update(b"]")
    elif isinstance(x, dict):
        for k in sorted(x):
            h.update(repr(k).encode())
            h_update(h, x[k])
    else:
        h.update(repr(x).encode())


def digest(x):
    h = hashlib.sha256()
    h_update(h, x)
    return h.hexdigest()[:24]


def flip_cos():
    return math.cos(math.radians(em.SIMPLIFY_FLIP_DEG))


def core_cases():
    """simplify.c alone on each scene's welded meshes, with the coarse level's rules, the near level's and none"""
    levels = {"near": em.SIMPLIFY_ENV_NEAR, "far": em.SIMPLIFY_ENV_FAR}
    for seed in range(6):
        ms, *_ = scene(1000 + seed)
        # one job: every mesh's positions and attributes side by side, agrp = mesh
        pos, wpos, tris, agrp = [], [], [], []
        for mi, m in enumerate(ms):
            po, wo = len(pos), len(wpos)
            pos += m["pos"]
            wpos += [po + p for p in m["wpos"]]
            agrp += [mi] * len(m["wpos"])
            tris += [tuple(wo + a for a in t) for t in m["tris"]]
        rng = Rng(seed)
        locked = [rng.random() < 0.01 for _ in pos]
        scale = [1.0 / (2.0 + math.hypot(p[0], p[2])) ** 2 for p in pos]  # 1 / view distance^2 from the origin
        for name, pad in [("off", 0.0)] + sorted(levels.items()):
            for ak in (simplify.AREA_K, 0.0):  # the coarse level's area term, the near level's none
                if name == "off":
                    simplify_native.rules()
                elif em.SIMPLIFY_GUARD:
                    simplify_native.rules(pad, em.SIMPLIFY_QMIN, flip_cos(), em.SIMPLIFY_VALIDATE_TOL or pad)
                else:
                    simplify_native.rules(pad)
                try:
                    log = simplify_native.simplify_log(pos, wpos, tris, locked, scale, area_k=ak, agrp=agrp)
                finally:
                    simplify_native.rules()
                d = digest(list(log))
                if name == "off" and digest(list(simplify.pack(*simplify.simplify(pos, wpos, tris, locked, scale,
                                                                                  area_k=ak)))) != d:
                    raise SystemExit(f"FAIL core/{seed}/off: simplify.c with its own rules off differs from the frozen "
                                     "reference tools/simplify.py")
                n = [len(simplify.replay_log(log, (px / 568) ** 2)) for px in (3, 12, 24, 1e6)]
                yield f"core/{seed}/{name}/ak{ak:g}", d, n


def export_cases():
    """export_map.simplify_meshes as export_map.gather calls it (STREAM): the coarse level (EPS_FAR, the far cells'
    BACKDROP_EPS, SIMPLIFY_ENV_FAR, the spot budget solve) then the near level (no area term, SIMPLIFY_ENV_NEAR)"""
    import copy
    for seed in range(16):
        ms, nav_v, nav_t, spawn, rng = scene(seed)
        em.KEEP_POS[:] = [(spawn[0] + 1.0, spawn[1], spawn[2])] if seed % 3 == 0 else []
        for m in ms:
            m["tris_in"] = m["tris"]
        with contextlib.redirect_stdout(io.StringIO()):
            far = em.far_cells(ms, nav_v)
        budget = seed % 4 != 3  # the spot budget solve (WS_BUDGET, on in the shipped export); every 4th: without
        nav_s = nav_v if budget else None
        bc = (em.WS_R1_IN, em.WS_R1, em.WS_B1 / 2048, "coarse level", em.WS_PX1) if budget else None
        bn = (0.0, em.WS_R0, em.WS_B0 / 2048, "near level", em.WS_PX0) if budget else None
        M = copy.deepcopy(ms)
        f = io.StringIO()
        with contextlib.redirect_stdout(f):
            e1 = em.simplify_meshes(M, spawn, nav_v, nav_t, src="tris_in", out="tris_far", eps_fixed=em.EPS_FAR, far=far,
                                    floor=em.STREAM_D, cells=True, eps_far=min(em.EPS_FAR, em.BACKDROP_EPS), budget=bc,
                                    nav_s=nav_s, env_pad=em.SIMPLIFY_ENV_FAR)
            e0 = em.simplify_meshes(M, spawn, nav_v, nav_t, src="tris_in", out="tris", cells=True, far=far, area_k=0.0,
                                    budget=bn, nav_s=nav_s)
        if em.SIMPLIFY_VALIDATE and "validate (" not in f.getvalue() and any(m.get("tris") for m in M):
            raise SystemExit(f"FAIL export/{seed}: the triangle guard's validation pass did not run")
        outs = [(e1, e0)] + [{k: v for k, v in m.items() if k.startswith(("tris_far", "tris")) and k != "tris_in"}
                              for m in M]
        n = [sum(len(m.get("tris_far") or ()) for m in M), sum(len(m.get("tris") or ()) for m in M)]
        yield f"export/{seed}{'/budget' if budget else ''}", digest(outs), n


# tools/simplify.py's frozen part: the reference core collapse simplify.c is compared against with its own rules off
# (test_mapx.py, simplify_native.py --test), and the helpers C mirrors (structure_weight: simpmesh.c struct_weight)
FROZEN = ("_plane", "_qadd", "_qeval", "_sub", "_cross", "_dot", "_area", "structure_weight", "simplify", "replay", "pack",
          "unpack", "replay_log")


def frozen():
    """a hash of the frozen reference functions' source in tools/simplify.py"""
    import inspect
    return digest([inspect.getsource(getattr(simplify, f)) for f in FROZEN])


def settings():
    """the shipped settings the goldens were made with: a default changing shows here by name"""
    names = ["SIMPLIFY_ENV_STRICT", "SIMPLIFY_ENV_NEAR", "SIMPLIFY_ENV_FAR", "SIMPLIFY_GUARD", "SIMPLIFY_QMIN",
             "SIMPLIFY_FLIP_DEG", "SIMPLIFY_VALIDATE", "SIMPLIFY_VALIDATE_TOL", "SIMPLIFY_REFIT", "EPS_FAR", "STREAM_D",
             "BACKDROP_EPS", "CHUNK_GRID", "KEEP_R", "KEEP_D", "WS_PX0", "WS_PX1", "WS_R0", "WS_R1", "WS_R1_IN"]
    s = {k: getattr(em, k) for k in names}
    s.update({f"simplify.{k}": getattr(simplify, k) for k in ("BORDER_W", "SEAM_W", "SEAM_K", "AREA_K", "MIN_COS",
                                                               "STRUCT_W", "STRUCT_FLAT_W")})
    return s


def main(argv):
    update, verbose = "--update" in argv, "--verbose" in argv
    if mapx_native.lib() is None:
        raise SystemExit("FAIL: libmapx did not build")
    got = {"settings": settings(), "cases": {}, "frozen_simplify_py": frozen()}
    for name, d, n in list(core_cases()) + list(export_cases()):
        got["cases"][name] = {"hash": d, "tris": n}
        if verbose:
            print(name, d, n)
    if update:
        with open(GOLDEN, "w") as f:
            json.dump(got, f, indent=1, sort_keys=True)
            f.write("\n")
        print(f"simplify golden: wrote {len(got['cases'])} cases to {os.path.relpath(GOLDEN)}")
        return 0
    with open(GOLDEN) as f:
        want = json.load(f)
    bad = 0
    for k in sorted(set(want["settings"]) | set(got["settings"])):
        if want["settings"].get(k) != got["settings"].get(k):
            bad += 1
            print(f"FAIL setting {k}: golden {want['settings'].get(k)!r}, now {got['settings'].get(k)!r}")
    if want.get("frozen_simplify_py") != got["frozen_simplify_py"]:
        bad += 1
        print("FAIL tools/simplify.py: its frozen reference (" + ", ".join(FROZEN) + ") changed. New simplifier rules go "
              "into tools/native/simplify.c only, off by default in its comparison with simplify.py; simplify.py "
              "changes only to fix a bug in the reference, with simplify.c fixed the same way in the same commit")
    for k in sorted(set(want["cases"]) | set(got["cases"])):
        w, g = want["cases"].get(k), got["cases"].get(k)
        if w != g:
            bad += 1
            print(f"FAIL {k}: golden {w}, now {g}")
    if bad:
        print(f"simplify golden: {bad} settings / outputs differ from {os.path.relpath(GOLDEN)}. If the "
              "change is intended, check it on a real export, then: python3 tools/mapx/test_simplify_golden.py --update")
        return 1
    print(f"simplify golden: {len(got['cases'])} outputs at the shipped settings match (core simplify.c and "
          "export_map.simplify_meshes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
