"""Content cache for the occlusion bake (tools/pvs_bake.c, ps2/proto/Makefile $(A)/pvs.bin):

  pvs_cache.py <pvs_bake binary> <pvsin.bin> <dg.bin> <collision.bin> <pvs.bin> [bake options ...]

Runs the bake, or copies pvs.bin and the bake's log from an earlier run with the same inputs. The key is the bake
binary's bytes, every option, and the bytes of every input file (the block's and its --with peers'), so a new build
dir, a new worktree, or a map re-export that wrote the same pvsin.bin / dg.bin / collision.bin as before takes
seconds instead of a bake (~2-3 CPU min per block). The output is the same bytes either way.
Cache: PVS_CACHE (a directory; 0 = off), default <main tree>/build/cache/pvs (DSR_CACHE: the cache root), entries
are small (pvs.bin ~60-210 KB). Entries are written by rename, so parallel builds never read half a one."""
import hashlib, os, shutil, subprocess, sys

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


def key(argv):
    """argv = the bake's command line; the output (argv[4]) by position only, existing files by content"""
    h = hashlib.sha256(VERSION)
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
    k = key(argv) if d else None
    if d:
        ent = os.path.join(d, k[:2], k)
        if os.path.isfile(ent + ".bin") and os.path.isfile(ent + ".log"):
            shutil.copyfile(ent + ".bin", out + ".tmp")
            os.replace(out + ".tmp", out)
            sys.stdout.write(open(ent + ".log").read())
            print(f"pvs_cache: {os.path.basename(os.path.dirname(os.path.abspath(out)))}: from the cache ({k[:12]})")
            return
    r = subprocess.run(argv, stdout=subprocess.PIPE, text=True)
    sys.stdout.write(r.stdout)
    sys.stdout.flush()
    if r.returncode:
        sys.exit(r.returncode)
    if d:
        try:
            os.makedirs(os.path.dirname(ent), exist_ok=True)
            tmp = f"{ent}.{os.getpid()}"
            shutil.copyfile(out, tmp + ".bin")
            with open(tmp + ".log", "w") as f:
                f.write(r.stdout)
            os.replace(tmp + ".log", ent + ".log")
            os.replace(tmp + ".bin", ent + ".bin")
        except OSError as e:  # a full / read-only cache never fails the build
            print(f"pvs_cache: not stored ({e})")


if __name__ == "__main__":
    main()
