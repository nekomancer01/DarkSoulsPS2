"""Content cache for the occlusion bake (tools/pvs_bake.c, ps2/proto/Makefile $(A)/pvs.bin):

  pvs_cache.py <pvs_bake binary> <pvsin.bin> <dg.bin> <collision.bin> <pvs.bin> [bake options ...]

Runs the bake, or copies pvs.bin and the bake's log from an earlier run with the same inputs. The key is the bake
binary's bytes, every option, and the bytes of every input file (the block's and its --with peers'), so a new build
dir, a new worktree, or a map re-export that wrote the same pvsin.bin / dg.bin / collision.bin as before takes
seconds instead of a bake (~2-3 CPU min per block). The output is the same bytes either way.
Cache: the "pvs" namespace of tools/buildcache.py (<cache root>/pvs; PVS_CACHE: 0 = off, a path = its directory;
BUILDCACHE=0 bypasses it, BUILDCACHE_VERIFY=1 bakes on a hit too and compares). Entries are small (pvs.bin ~60-210 KB).

GPU hand-over (container builds, tools/build-iso.sh): when DSR_PVS_SPOOL names a spool directory whose server
(tools/pvs_gpu_server.py, in a container with the GPU) is up, the bake runs there instead of here; the cache key then
carries "gpu", as its output differs from a CPU bake's. When the server is not up, or stops, the bake runs here."""
import json, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # run with python3 -I: no script dir on sys.path
import buildcache

# pvs_bake's OpenMP threads sleep between its many short parallel loops instead of spinning (libgomp's default spins):
# with the GPU path a bake spun ~200 of its ~270 CPU-s per Burg block, starving the rest of the build (2026-11-17);
# the bytes are the same either way
os.environ.setdefault("OMP_WAIT_POLICY", "passive")

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = b"pvs-cache-2"  # 2: one buildcache entry {"bin", "log"}


def cache():
    """the PVS namespace of the build cache (tools/buildcache.py; PVS_CACHE: 0 = off, a path = its directory)"""
    return buildcache.Cache("pvs", VERSION, where=os.environ.get("PVS_CACHE"))


def spool():
    """the GPU server's spool directory when it is up (ready refreshed within the last 5 s), else None"""
    d = os.environ.get("DSR_PVS_SPOOL")
    try:
        return d if d and time.time() - os.path.getmtime(os.path.join(d, "ready")) < 5 else None
    except OSError:
        return None


def remote(d, argv):
    """the bake on the GPU server -> (exit code, output), or None when the server went away before it was done"""
    name = f"{os.getpid()}-{time.time_ns()}"
    job, tmp = os.path.join(d, name + ".job"), os.path.join(d, name + ".tmp")
    with open(tmp, "w") as f:
        json.dump({"argv": argv, "cwd": os.getcwd()}, f)
    os.replace(tmp, job)
    done = os.path.join(d, name + ".done")
    while not os.path.exists(done):
        time.sleep(0.25)
        if not spool():
            try:
                os.remove(job)  # not claimed yet: bake here
            except OSError:
                pass
            if not os.path.exists(done):
                return None
    rc = int(open(done).read() or 1)
    out = open(os.path.join(d, name + ".log")).read()
    for ext in (".done", ".log"):
        try:
            os.remove(os.path.join(d, name + ext))
        except OSError:
            pass
    return rc, out


def key(c, argv, gpu=False):
    """argv = the bake's command line; the bake binary and every existing input file by content, the output (argv[4])
    by position only, the rest as given"""
    parts = ["gpu" if gpu else "cpu", buildcache.File(argv[0])]
    for i, a in enumerate(argv[1:], 1):
        parts.append("<out>" if i == 4 else buildcache.File(a) if os.path.isfile(a) else a)
    return c.key(*parts)


def bake(gs, argv):
    """-> (exit code, log, gpu): on the GPU server when it is up, else here"""
    res = remote(gs, argv) if gs else None
    if res is None:
        if gs:
            print("pvs_cache: the GPU server stopped: baking here")
        r = subprocess.run(argv, stdout=subprocess.PIPE, text=True)
        return r.returncode, r.stdout, False
    return res[0], res[1], True


def main():
    argv = sys.argv[1:]
    if len(argv) < 5:
        sys.exit(__doc__)
    out = argv[4]
    c = cache()
    gs = spool()
    k = key(c, argv, bool(gs)) if c.dir else None
    hit = c.get(k)
    if hit is not None:
        if c.verify:  # recompute; the key carries cpu / gpu, so verify on the same path as the entry was made
            rc, log, gpu = bake(gs, argv)
            if rc:
                sys.exit(rc)
            if gpu == bool(gs):
                c.check(k, hit["bin"], open(out, "rb").read())
            print(f"pvs_cache: verified against the cache ({k[:12]})")
        tmp = f"{out}.{os.getpid()}.tmp"
        with open(tmp, "wb") as f:
            f.write(hit["bin"])
        os.replace(tmp, out)
        sys.stdout.write(hit["log"])
        print(f"pvs_cache: {os.path.basename(os.path.dirname(os.path.abspath(out)))}: from the cache ({k[:12]})")
        return
    rc, log, gpu = bake(gs, argv)
    sys.stdout.write(log)
    sys.stdout.flush()
    if rc:
        sys.exit(rc)
    if c.dir:
        if gpu != bool(gs):
            k = key(c, argv, gpu)
        c.put(k, {"bin": open(out, "rb").read(), "log": log})


if __name__ == "__main__":
    main()
