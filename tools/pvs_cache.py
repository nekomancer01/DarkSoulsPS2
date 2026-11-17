"""Content cache for the occlusion bake (tools/pvs_bake.c, ps2/proto/Makefile $(A)/pvs.bin):

  pvs_cache.py <pvs_bake binary> <pvsin.bin> <dg.bin> <collision.bin> <pvs.bin> [bake options ...]

Runs the bake, or copies pvs.bin and the bake's log from an earlier run with the same inputs. The key is the bake
binary's bytes, every option, and the bytes of every input file (the block's and its --with peers'), so a new build
dir, a new worktree, or a map re-export that wrote the same pvsin.bin / dg.bin / collision.bin as before takes
seconds instead of a bake (~2-3 CPU min per block). The output is the same bytes either way.
Cache: PVS_CACHE (a directory; 0 = off), default <main tree>/build/cache/pvs (DSR_CACHE: the cache root), entries
are small (pvs.bin ~60-210 KB). Entries are written by rename, so parallel builds never read half a one.

GPU hand-over (container builds, tools/build-iso.sh): when DSR_PVS_SPOOL names a spool directory whose server
(tools/pvs_gpu_server.py, in a container with the GPU) is up, the bake runs there instead of here; the cache key then
carries "gpu", as its output differs from a CPU bake's. When the server is not up, or stops, the bake runs here."""
import hashlib, json, os, shutil, subprocess, sys, time

# pvs_bake's OpenMP threads sleep between its many short parallel loops instead of spinning (libgomp's default spins):
# with the GPU path a bake spun ~200 of its ~270 CPU-s per Burg block, starving the rest of the build (2026-11-17);
# the bytes are the same either way
os.environ.setdefault("OMP_WAIT_POLICY", "passive")

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = b"pvs-cache-1"


def main_tree():
    """The main working tree of this repo (tools/simplify_pool.main_tree, without its numpy imports)"""
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
    c = os.environ.get("PVS_CACHE")
    if c == "0":
        return None
    return c or os.path.join(os.environ.get("DSR_CACHE") or os.path.join(main_tree(), "build", "cache"), "pvs")


def file_hash(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


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


def key(argv, gpu=False):
    """argv = the bake's command line; the output (argv[4]) by position only, existing files by content"""
    h = hashlib.sha256(VERSION + (b"\0<gpu>" if gpu else b""))
    h.update(file_hash(argv[0]).encode())
    for i, a in enumerate(argv[1:], 1):
        if i == 4:
            h.update(b"\0<out>")
        elif os.path.isfile(a):
            h.update(b"\0<file>" + file_hash(a).encode())
        else:
            h.update(b"\0" + a.encode())
    return h.hexdigest()


def main():
    argv = sys.argv[1:]
    if len(argv) < 5:
        sys.exit(__doc__)
    out = argv[4]
    d = cache_dir()
    gs = spool()
    k = key(argv, bool(gs)) if d else None
    if d:
        ent = os.path.join(d, k[:2], k)
        if os.path.isfile(ent + ".bin") and os.path.isfile(ent + ".log"):
            shutil.copyfile(ent + ".bin", out + ".tmp")
            os.replace(out + ".tmp", out)
            sys.stdout.write(open(ent + ".log").read())
            print(f"pvs_cache: {os.path.basename(os.path.dirname(os.path.abspath(out)))}: from the cache ({k[:12]})")
            return
    res = remote(gs, argv) if gs else None
    if res is None:
        if gs:
            print("pvs_cache: the GPU server stopped: baking here")
            k = key(argv) if d else None
        r = subprocess.run(argv, stdout=subprocess.PIPE, text=True)
        res = r.returncode, r.stdout
    rc, log = res
    sys.stdout.write(log)
    sys.stdout.flush()
    if rc:
        sys.exit(rc)
    if d:
        ent = os.path.join(d, k[:2], k)
        try:
            os.makedirs(os.path.dirname(ent), exist_ok=True)
            tmp = f"{ent}.{os.getpid()}"
            shutil.copyfile(out, tmp + ".bin")
            with open(tmp + ".log", "w") as f:
                f.write(log)
            os.replace(tmp + ".log", ent + ".log")
            os.replace(tmp + ".bin", ent + ".bin")
        except OSError as e:  # a full / read-only cache never fails the build
            print(f"pvs_cache: not stored ({e})")


if __name__ == "__main__":
    main()
