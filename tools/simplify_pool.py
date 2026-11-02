"""Runs tools/export_map.py's simplification jobs (one per welded group / cell, tools/simplify.py) with a result cache
and a worker pool that survives killed workers.

Cache: simplify() returns a collapse log that replays at any threshold, so a job's result depends only on its inputs
(positions, attribute vertices, triangles, locks, view weights, area_k) and the simplifier's code. Each result is
stored under a hash of exactly those in a cache shared by every build dir (and every worktree of the repo, see
tools/worktree.sh): re-exports after changes to lighting, textures, draw groups, effects, chunking or EPS / BUDGET
tuning skip the quadric collapses, and a hit starts no worker at all.
  SIMPLIFY_CACHE    cache dir (default <main tree>/build/cache/simplify; 0 = off)
  SIMPLIFY_CACHE_GB size cap (default 10): oldest entries go first

Pool: workers come from a forkserver, so each holds only its job instead of a copy-on-write fork of the 3-4 GB
exporter (memwatch counted every fork's shared pages as its own and killed them, 2026-10-31 / 11-01). A worker that
dies (killed, out of memory) fails its pool fast instead of hanging Pool.map: the finished results are kept, the rest
run again with half the workers, down to one.
  SIMPLIFY_PROCS    workers (default 16, at most the CPU count)
  SIMPLIFY_NATIVE   1: the C simplifier (tools/native/simplify.c via simplify_native.py), 0: simplify.py; default:
                    C when its library builds (same collapses, see simplify_native.py)"""
import hashlib, os, pickle, sys, time, zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import simplify

CACHE_VERSION = b"simplify-cache-2"  # 2: entries are compact logs (simplify.pack)


def main_tree():
    """The main working tree of this repo (a worktree's .git file points into <main>/.git/worktrees/<name>)"""
    root = os.path.dirname(HERE)
    g = os.path.join(root, ".git")
    try:
        if os.path.isfile(g):
            gitdir = open(g).read().split("gitdir:", 1)[1].strip()
            gitdir = os.path.normpath(os.path.join(root, gitdir))
            common = open(os.path.join(gitdir, "commondir")).read().strip()
            return os.path.dirname(os.path.normpath(os.path.join(gitdir, common)))
    except (OSError, IndexError):
        pass
    return root


def cache_dir():
    c = os.environ.get("SIMPLIFY_CACHE")
    if c == "0":
        return None
    return c or os.path.join(os.environ.get("DSR_CACHE") or os.path.join(main_tree(), "build", "cache"), "simplify")


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


def _code_hash(be):
    h = hashlib.sha256(CACHE_VERSION)
    files = ["simplify.py"] + (["simplify_native.py", os.path.join("native", "simplify.c")] if be == "native" else [])
    for f in files:
        h.update(open(os.path.join(HERE, f), "rb").read())
    return h


def job_key(code, job, area_k):
    pos, wpos, tris, locked, scale = job
    h = code.copy()
    eff = simplify.AREA_K if area_k is None else area_k
    h.update(repr(float(eff)).encode())
    for a, dt in ((pos, np.float64), (wpos, np.int64), (tris, np.int64), (scale, np.float64)):
        b = np.ascontiguousarray(np.asarray(a, dtype=dt)).tobytes()
        h.update(len(b).to_bytes(8, "little") + b)
    lk = b"" if locked is None else np.asarray(locked, dtype=np.uint8).tobytes()
    h.update(len(lk).to_bytes(8, "little") + (b"N" if locked is None else lk))
    return h.hexdigest()


def _load(path):
    try:
        with open(path, "rb") as f:
            r = pickle.loads(zlib.decompress(f.read()))
        os.utime(path)  # recently used: kept by the size cap
        return r
    except (OSError, ValueError, EOFError, pickle.UnpicklingError, zlib.error):
        return None


def _save(path, res):
    tmp = f"{path}.{os.getpid()}.tmp"
    try:
        with open(tmp, "wb") as f:
            f.write(zlib.compress(pickle.dumps(res, protocol=pickle.HIGHEST_PROTOCOL), 1))
        os.replace(tmp, path)
    except OSError as e:
        print(f"simplify: cache write failed ({e})", flush=True)
        try:
            os.remove(tmp)
        except OSError:
            pass


def _trim(d):
    cap = float(os.environ.get("SIMPLIFY_CACHE_GB", 10)) * (1 << 30)
    ents = []
    for sub in os.listdir(d):
        p = os.path.join(d, sub)
        if not os.path.isdir(p):
            continue
        for n in os.listdir(p):
            try:
                st = os.stat(os.path.join(p, n))
                ents.append((st.st_mtime, st.st_size, os.path.join(p, n)))
            except OSError:
                pass
    total = sum(e[1] for e in ents)
    if total <= cap:
        return
    for _, size, p in sorted(ents):
        try:
            os.remove(p)
        except OSError:
            continue
        total -= size
        if total <= 0.8 * cap:
            break


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
    """[(pos, wpos, tris, locked, scale)] -> [compact log] (simplify.pack of what simplify.simplify returns), in job
    order"""
    t0 = time.time()
    be = backend()
    d = cache_dir()
    res = [None] * len(jobs)
    keys = [None] * len(jobs)
    if d:
        code = _code_hash(be)
        for i, j in enumerate(jobs):
            keys[i] = job_key(code, j, area_k)
            res[i] = _load(os.path.join(d, keys[i][:2], keys[i]))
    hits = sum(r is not None for r in res)
    todo = sorted((i for i in range(len(jobs)) if res[i] is None), key=lambda i: -len(jobs[i][2]))
    procs = max(1, min(int(os.environ.get("SIMPLIFY_PROCS", 16)), os.cpu_count() or 1, len(todo) or 1))
    if procs == 1:  # one worker: in process (no pool to kill, nothing to pickle; the browser converter has no processes)
        for i in list(todo):
            res[i] = work(jobs[i], area_k, be)
            _store(d, keys[i], res[i])
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
                    _store(d, keys[i], res[i])
                    todo.remove(i)
        except BrokenProcessPool:
            tries += 1
            if tries > 6:
                raise
            procs = max(1, procs // 2)
            print(f"simplify: a worker died (killed or out of memory); {len(todo)} jobs again with {procs} workers",
                  flush=True)
    if d:
        try:
            _trim(d)
        except OSError:
            pass
    print(f"simplify: {len(jobs)} jobs, {hits} from the cache{f' ({d})' if d else ' (cache off)'}, "
          f"{len(jobs) - hits} run ({be}) in {time.time() - t0:.1f} s", flush=True)
    return res


def _store(d, key, r):
    if d and key:
        os.makedirs(os.path.join(d, key[:2]), exist_ok=True)
        _save(os.path.join(d, key[:2], key), r)
