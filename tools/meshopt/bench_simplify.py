"""meshoptimizer vs tools/simplify.py on a real map export (default Firelink, m10_02_00_00).

1. capture: runs export_map.py's own gather / prune_foliage / simplify_meshes up to the simplify pool and saves the
   jobs (welded groups: positions, attribute vertices, triangles, per-vertex view weight) to <scratch>/jobs_<map>.pkl;
   export_map's env settings (BUDGET, SPAWN_K, GRASS_KEEP, FOL_*) apply as in the Makefile.
2. simplify.py: every group through simplify.run (process pool), the global threshold searched to BUDGET exactly as
   export_map does -> triangles per group.
3. meshoptimizer: every group with the same triangle count as simplify.py gave it (so the per-group budget, i.e.
   the view weighting, is identical); attribute vertices are passed as split vertices, so UV / mesh seams survive.
   A second variant adds meshopt_SimplifyPermissive | Prune (may collapse across seams, drops small loose parts).
4. error, independent of both: distance from every original vertex and triangle centroid to the whole simplified map
   (tools/meshopt/meshdist.c; a group removed entirely is judged by what remains around it), in metres and in pixels at 640x448 (x 568 / view distance, as export_map prints).
   Plus strips (meshopt_stripify, restart index) and post-transform cache ACMR (16 entries) of both outputs.
Usage: bench_simplify.py <scratch dir> [map id] [--procs N]     (run via tools/q -m 12)"""
import math, multiprocessing, os, pickle, sys, time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import meshopt


class _Captured(Exception):
    pass


def capture(scratch, map_id):
    path = os.path.join(scratch, f"jobs_{map_id}.pkl")
    if os.path.exists(path):
        return pickle.load(open(path, "rb"))
    import gamepath, export_map, texmap
    game = gamepath.game_dir()
    texin = []  # texmap.encode inputs, saved for tools/liq/bench_quant.py
    enc = texmap.encode
    texmap.encode = lambda w, h, rgba, soft=False, name="": (texin.append((w, h, bytes(rgba), soft, name)),
                                                            enc(w, h, rgba, soft, name))[1]
    try:
        meshes, _tex, nav_v, nav_t, spawn, _fog, _sky = export_map.gather(game, map_id)
    finally:
        texmap.encode = enc
    pickle.dump(texin, open(os.path.join(scratch, f"tex_{map_id}.pkl"), "wb"))
    export_map.prune_foliage(meshes, nav_v, nav_t)
    box = {}

    class Pool:
        def __init__(self, *a, **k): pass
        def __enter__(self): return self
        def __exit__(self, *a): return False
        def map(self, fn, jobs, chunksize=1):
            box["jobs"] = jobs
            raise _Captured
    real = multiprocessing.Pool
    multiprocessing.Pool = Pool
    try:
        export_map.simplify_meshes(meshes, spawn["pos"], nav_v, nav_t)
    except _Captured:
        pass
    finally:
        multiprocessing.Pool = real
    grass = sum(len(m["tris"]) for m in meshes if m["keep"] and not m["grass"])
    data = dict(jobs=box["jobs"], grass=grass, budget=export_map.BUDGET)
    pickle.dump(data, open(path, "wb"))
    return data


def timed_run(job):
    import simplify
    t = time.process_time()
    r = simplify.run(job)
    return r, time.process_time() - t


def main():
    a = [x for x in sys.argv[1:] if not x.startswith("--")]
    scratch, map_id = a[0], (a[1] if len(a) > 1 else "m10_02_00_00")
    procs = int(sys.argv[sys.argv.index("--procs") + 1]) if "--procs" in sys.argv else 4
    os.makedirs(scratch, exist_ok=True)
    t0 = time.time()
    D = capture(scratch, map_id)
    jobs, grass, budget = D["jobs"], D["grass"], D["budget"]
    print(f"{map_id}: {len(jobs)} welded groups, {sum(len(j[2]) for j in jobs)} tris to simplify, {grass} kept, "
          f"budget {budget} (capture {time.time() - t0:.0f} s)", flush=True)

    # simplify.py, exactly as export_map (pool, global threshold)
    order = sorted(range(len(jobs)), key=lambda j: -len(jobs[j][2]))
    t0 = time.time()
    with multiprocessing.Pool(procs) as pool:
        res = dict(zip(order, pool.map(timed_run, [jobs[j] for j in order], chunksize=1)))
    py_wall = time.time() - t0
    py_cpu = sum(res[j][1] for j in res)
    logs = [res[j][0] for j in range(len(jobs))]
    full = sum(len(t) for t, _ in logs)
    cost = np.array([s[0] for _, st in logs for s in st])
    gone = np.array([len(s[1]) for _, st in logs for s in st])
    count = lambda eps: grass + full - int(gone[cost <= eps].sum())
    lo, hi = 1e-16, 1.0
    while count(hi) > budget and hi < 1e6:
        hi *= 100
    for _ in range(40):
        mid = math.sqrt(lo * hi)
        lo, hi = (lo, mid) if count(mid) <= budget else (mid, hi)
    eps = hi
    import simplify
    py_out = [simplify.replay(T0, st, eps) for T0, st in logs]

    # meshoptimizer at the same per-group counts: plain (seams kept, may stop above the target) and permissive
    # (meshopt_SimplifyPermissive: may collapse across attribute seams, + Prune of small disconnected parts)
    variants = {"meshopt": 0, "meshopt perm": meshopt.PERMISSIVE | meshopt.PRUNE}
    outs, times = {"simplify.py": None}, {"simplify.py": f"{py_cpu:6.1f} s cpu"}
    for name, opts in variants.items():
        t0 = time.time()
        res_ = []
        for (pos, wpos, tris, _, _), out in zip(jobs, py_out):
            P = np.asarray(pos, np.float32)[np.asarray(wpos)]
            idx = np.asarray(tris, np.uint32).reshape(-1)
            res_.append(meshopt.simplify(P, idx, len(out), options=opts)[0])
        outs[name] = res_
        times[name] = f"{time.time() - t0:6.2f} s"

    # one global mesh per method (positions of every group back to back), so a group removed entirely is measured
    # against what remains around it
    offs = np.cumsum([0] + [len(j[0]) for j in jobs])
    allpos = np.vstack([np.asarray(j[0], np.float32) for j in jobs])
    scale = np.concatenate([np.asarray(j[4], np.float64) for j in jobs])
    orig = np.vstack([np.asarray([[j[1][w] for w in t] for t in j[2] if len({j[1][w] for w in t}) == 3]).reshape(-1, 3)
                      + o for j, o in zip(jobs, offs)])
    used = np.unique(orig.reshape(-1))
    pts = np.vstack([allpos[used], allpos[orig].mean(1)])
    pw = np.concatenate([np.sqrt(scale[used]), np.sqrt(scale[orig]).mean(1)])
    print(f"\n{len(pts)} sample points (vertices + centroids of the input)")
    print(f"{'':13} {'tris':>7} {'time':>14} {'mean m':>7} {'p99 m':>7} {'max m':>7} {'mean px':>7} {'p99 px':>7} "
          f"{'max px':>7} {'strip/list idx':>16} {'ACMR':>5}")
    for name in ["simplify.py"] + list(variants):
        gidx, strip, lst, acm = [], 0, 0, 0.0
        for gi, ((pos, wpos, tris, _, _), po) in enumerate(zip(jobs, py_out)):
            if name == "simplify.py":
                ids = {}
                av = np.asarray([ids.setdefault(c, len(ids)) for t in po for c in t], np.uint32)
                nverts = len(ids)
                pidx = np.asarray([c[0] for t in po for c in t], np.uint32)
            else:
                av = outs[name][gi]
                nverts = len(wpos)
                pidx = np.asarray(wpos, np.uint32)[av] if len(av) else np.zeros(0, np.uint32)
            gidx.append(pidx + offs[gi])
            if len(av):  # strips / cache in attribute-vertex space (what the GS gets)
                av = meshopt.optimize_vertex_cache(av, nverts)
                strip += len(meshopt.stripify(av, nverts))
                lst += len(av)
                acm += meshopt.cache_stats(av, nverts)[0] * len(av) / 3
        gidx = np.concatenate(gidx)
        d = meshopt.point_mesh_distance(pts, allpos, gidx).astype(np.float64)
        px = d * pw * 568
        print(f"{name:13} {len(gidx) // 3 + grass:7} {times[name]:>14} {d.mean():7.4f} {np.percentile(d, 99):7.3f} "
              f"{d.max():7.3f} {px.mean():7.3f} {np.percentile(px, 99):7.2f} {px.max():7.2f} {strip:7}/{lst:<8} "
              f"{acm / max(1, lst / 3):5.2f}")
    print(f"simplify.py wall {py_wall:.1f} s with {procs} processes; eps {eps:.3g}")


if __name__ == "__main__":
    main()
