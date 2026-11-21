"""Runs tools/export_map.py's simplification jobs (one per welded group / cell) with a result cache and a worker pool
that survives killed workers: export_map.simplify_meshes' fallback when libmapx is off or missing (MAPX=0,
SIMPLIFY_NATIVE=0, no navmesh), without the C-only rules (shape envelope, triangle guard); the shipped export runs the
jobs in libmapx (export_map.simplify_meshes_c, tools/native/simplify.c).

Cache: simplify() returns a collapse log that replays at any threshold, so a job's result depends only on its inputs
(positions, attribute vertices, triangles, locks, view weights, area_k) and the simplifier's code. Each result is
stored under a hash of exactly those in a cache shared by every build dir (and every worktree of the repo, see
tools/worktree.sh): re-exports after changes to lighting, textures, draw groups, effects, chunking or EPS / BUDGET
tuning skip the quadric collapses, and a hit starts no worker at all.
  SIMPLIFY_CACHE    the "simplify" namespace of tools/buildcache.py (default <cache root>/simplify; 0 = off, a path = its
                    directory; BUILDCACHE=0 bypasses it, BUILDCACHE_VERIFY=1 runs the hits too and compares)
  SIMPLIFY_CACHE_GB size cap (default 10): least recently used entries go first

Pool: workers come from a forkserver, so each holds only its job instead of a copy-on-write fork of the 3-4 GB
exporter (memwatch counted every fork's shared pages as its own and killed them, 2026-10-31 / 11-01). A worker that
dies (killed, out of memory) fails its pool fast instead of hanging Pool.map: the finished results are kept, the rest
run again with half the workers, down to one.
  SIMPLIFY_PROCS    workers (default 16, at most the CPU count)
  SIMPLIFY_NATIVE   1: the C simplifier (tools/native/simplify.c via simplify_native.py), 0: simplify.py; default:
                    C when its library builds (same collapses, see simplify_native.py)"""
import os, sys, time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import buildcache, simplify

CACHE_VERSION = b"simplify-cache-3"  # 2: entries are compact logs (simplify.pack), 3: buildcache entries


main_tree = buildcache.main_tree


_CACHES = {}


def cache(be):
    """the simplify namespace of the build cache (tools/buildcache.py; SIMPLIFY_CACHE: 0 = off, a path = its directory;
    SIMPLIFY_CACHE_GB: its size cap); the key prefix is the chosen simplifier's code. One Cache per backend and
    setting for the process: building one hashes the simplifier sources and registers an exit trim"""
    where, cap = os.environ.get("SIMPLIFY_CACHE"), os.environ.get("SIMPLIFY_CACHE_GB", 10)
    k = (be, where, cap, os.environ.get("DSR_CACHE"), os.environ.get("BUILDCACHE"))
    if k not in _CACHES:
        files = ["simplify.py"] + (["simplify_native.py", os.path.join("native", "simplify.c")] if be == "native" else [])
        _CACHES[k] = buildcache.Cache("simplify", CACHE_VERSION, sources=[os.path.join(HERE, f) for f in files],
                                      where=where, cap_gb=cap)
    return _CACHES[k]


def cache_dir():
    return cache(backend()).dir


def backend():
    """'native' or 'python'"""
    want = os.environ.get("SIMPLIFY_NATIVE", "")
    if want == "0":
        return "python"
    try:
        import simplify_native
        simplify_native.lib()
        return "native"
    except Exception as e:  # no compiler / build failed
        if want == "1":
            raise
        print(f"simplify: C simplifier unavailable ({e}); using simplify.py", flush=True)
        return "python"


def job_key(c, job, area_k):
    pos, wpos, tris, locked, scale = job[:5]
    eff = simplify.AREA_K if area_k is None else area_k
    parts = [float(eff), np.asarray(pos, dtype=np.float64), np.asarray(wpos, dtype=np.int64),
             np.asarray(tris, dtype=np.int64), np.asarray(scale, dtype=np.float64),
             None if locked is None else np.asarray(locked, dtype=np.uint8)]
    if len(job) > 5 and job[5] is not None:  # structure weight and its saturation (simplify.simplify weight / sat)
        parts += [np.asarray(job[5], dtype=np.float64), float(job[6])]
    return c.key(*parts)


def work(job, area_k, be):
    """pool entry: one job through the chosen simplifier -> its compact log (simplify.pack)"""
    if be == "native":
        import simplify_native
        try:
            return simplify_native.simplify_log(*job, area_k=area_k)
        except Exception as e:  # never fail an export on the C path: simplify.py gives the same result, slower
            print(f"simplify: C simplifier failed on a {len(job[2])}-triangle job ({type(e).__name__}: {e}); "
                  "using simplify.py for it", flush=True)
    return simplify.pack(*simplify.simplify(*job, area_k=area_k))


def run_jobs(jobs, area_k=None):
    """[(pos, wpos, tris, locked, scale[, weight, sat])] -> [compact log] (simplify.pack of what simplify.simplify returns), in job
    order"""
    t0 = time.time()
    be = backend()
    c = cache(be)
    d = c.dir
    res = [None] * len(jobs)
    keys = [None] * len(jobs)
    if d:
        for i, j in enumerate(jobs):
            keys[i] = job_key(c, j, area_k)
            res[i] = c.get(keys[i])
    hits = sum(r is not None for r in res)
    cached = {i: r for i, r in enumerate(res) if r is not None} if c.verify else {}  # verify mode: run the hits too
    todo = sorted((i for i in range(len(jobs)) if res[i] is None or i in cached), key=lambda i: -len(jobs[i][2]))
    procs = max(1, min(int(os.environ.get("SIMPLIFY_PROCS", 16)), os.cpu_count() or 1, len(todo) or 1))
    if procs == 1:  # one worker: in process (no pool to kill, nothing to pickle; the browser converter has no processes)
        for i in list(todo):
            res[i] = work(jobs[i], area_k, be)
            _store(c, keys[i], res[i], cached.get(i))
        todo = []
    else:  # imported only here: Pyodide has no _multiprocessing
        import multiprocessing
        from concurrent.futures import ProcessPoolExecutor, as_completed
        from concurrent.futures.process import BrokenProcessPool
    tries = 0
    while todo:
        ctx = multiprocessing.get_context("forkserver")
        try:
            with ProcessPoolExecutor(max_workers=procs, mp_context=ctx) as ex:
                futs = {ex.submit(work, jobs[i], area_k, be): i for i in todo}
                for f in as_completed(futs):
                    i = futs[f]
                    res[i] = f.result()
                    _store(c, keys[i], res[i], cached.get(i))
                    todo.remove(i)
        except BrokenProcessPool:
            tries += 1
            if tries > 6:
                raise
            procs = max(1, procs // 2)
            print(f"simplify: a worker died (killed or out of memory); {len(todo)} jobs again with {procs} workers",
                  flush=True)
    print(f"simplify: {len(jobs)} jobs, {hits} from the cache{f' ({d})' if d else ' (cache off)'}, "
          f"{len(jobs) - hits} run ({be}){f', {len(cached)} verified' if cached else ''} in {time.time() - t0:.1f} s", flush=True)
    return res


def _store(c, key, r, was=None):
    if was is not None:  # verify mode: a hit recomputed
        c.check(key, was, r)
    else:
        c.put(key, r)
