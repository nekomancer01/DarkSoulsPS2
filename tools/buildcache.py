"""Content-addressed build cache shared by the export steps (Polaris 3.2; docs/polaris/data-pipeline.md "Build cache").

A step's result is stored under a SHA-256 of everything that decides it: the step's own code (its source files, a
compiled tool's bytes), the environment variables it reads, its arguments and the bytes of its inputs. Two runs with the
same key give the same bytes, so a hit is safe in any build dir, any worktree, on any machine that shares the directory,
and after a change is reverted. A key never depends on paths or modification times.

    c = buildcache.Cache("pvs", b"pvs-1", sources=[...], extra=[<env values read>], where=<PVS_CACHE value>)
    k = c.key(binary_file, args, buildcache.File(path), array)    # str / bytes / numbers / numpy / File / tuples
    r = c.cached(k, lambda: compute())                            # get, else compute + put (+ verify, below)
    # or by hand (results computed in a pool): c.get(k) -> object or None, c.put(k, obj), c.check(k, cached, fresh)

Layout: <root>/<namespace>/<key[:2]>/<key>, one zlib-compressed pickle per entry, written by rename (parallel builds
never read half an entry). A read refreshes the entry's mtime; when a namespace grows past its cap the least recently
used entries go first, down to 80 % of the cap. A full or read-only cache never fails a build.

Environment (every one registered in tools/switches.tsv):
  BUILDCACHE         0: bypass every namespace (no reads, no writes; outputs are the same bytes, slower)
  BUILDCACHE_VERIFY  1: on a hit, compute the result anyway and compare; a difference stops the step with an error
                     naming the namespace and key (and is appended to <root>/verify.log)
  BUILDCACHE_GB      size cap per namespace in GB (default 4; the simplify namespace has its own, SIMPLIFY_CACHE_GB, 10)
  DSR_CACHE          the root (default <main tree>/build/cache: shared by every worktree of the repo)
  <ns switch>        each namespace's own switch (PVS_CACHE, SIMPLIFY_CACHE, SND_CACHE, TEX_CACHE): 0 = off, a path =
                     that namespace's directory

CLI: python3 tools/buildcache.py [stats | trim [ns ...] | clear ns ...]"""
import atexit, hashlib, os, pickle, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = b"buildcache-1"  # entry format; a new value orphans every entry (the size cap removes them)
DEFAULT_GB = 4
NAMESPACES = ("pvs", "simplify", "snd", "tex")  # the CLI's default set (build/cache/warm is warmstate.py's, not ours)


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


def root():
    return os.environ.get("DSR_CACHE") or os.path.join(main_tree(), "build", "cache")


def verifying():
    return os.environ.get("BUILDCACHE_VERIFY", "") not in ("", "0")


class File:
    """a key part that stands for a file's bytes (not its path)"""
    __slots__ = ("path",)

    def __init__(self, path):
        self.path = path


_FILE_HASH = {}


def file_hash(path):
    """sha256 hex of a file's bytes (memoised per path, size and mtime within this process)"""
    st = os.stat(path)
    m = (os.path.abspath(path), st.st_size, st.st_mtime_ns)
    if m not in _FILE_HASH:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for b in iter(lambda: f.read(1 << 20), b""):
                h.update(b)
        _FILE_HASH[m] = h.hexdigest()
    return _FILE_HASH[m]


def _feed(h, p):
    """one key part into hash h, tagged with its type and length so no two different part lists hash alike"""
    if isinstance(p, File):
        h.update(b"F" + file_hash(p.path).encode())
    elif isinstance(p, (bytes, bytearray, memoryview)):
        b = bytes(p)
        h.update(b"B" + len(b).to_bytes(8, "little") + b)
    elif isinstance(p, str):
        b = p.encode()
        h.update(b"S" + len(b).to_bytes(8, "little") + b)
    elif p is None or isinstance(p, (bool, int, float)):
        h.update(b"R" + repr(p).encode() + b"\0")
    elif isinstance(p, (tuple, list)):
        h.update(b"L" + len(p).to_bytes(8, "little"))
        for q in p:
            _feed(h, q)
    elif hasattr(p, "dtype") and hasattr(p, "tobytes"):  # numpy array, without importing numpy here
        import numpy as np
        a = np.ascontiguousarray(p)
        b = a.tobytes()
        h.update(b"A" + repr((a.dtype.str, a.shape)).encode() + len(b).to_bytes(8, "little") + b)
    else:
        raise TypeError(f"buildcache: unsupported key part {type(p).__name__}")


class VerifyError(RuntimeError):
    pass


class Cache:
    def __init__(self, ns, version, sources=(), extra=(), where=None, cap_gb=None):
        """ns: namespace (directory name); version: bytes, bump when the result's form changes without a source change;
        sources: files whose bytes decide the result (the tool's code, a compiled helper; a missing one counts as
        absent); extra: any other key parts common to every entry (environment values the tool reads, library versions,
        module-level knobs); where: the value of the namespace's own switch (os.environ.get("PVS_CACHE"): 0 = off, a
        path = its directory, unset = <root>/<ns>); cap_gb: the namespace's own size cap (else BUILDCACHE_GB, 4)"""
        self.ns = ns
        if os.environ.get("BUILDCACHE") == "0" or where == "0":
            self.dir = None
        else:
            self.dir = where or os.path.join(root(), ns)
        self.verify = bool(self.dir) and verifying()
        self.cap = float(cap_gb or os.environ.get("BUILDCACHE_GB") or DEFAULT_GB) * (1 << 30)
        h = hashlib.sha256(VERSION)
        _feed(h, (ns, bytes(version)))
        for f in sources:
            try:
                _feed(h, ("src", File(f)))
            except OSError:
                _feed(h, ("src", None))
        _feed(h, ("extra", tuple(extra)))
        self._base = h
        self.hits = self.misses = self.verified = 0
        self._pid = os.getpid()
        if self.dir:
            atexit.register(self._exit)

    def key(self, *parts):
        h = self._base.copy()
        for p in parts:
            _feed(h, p)
        return h.hexdigest()

    def _path(self, k):
        return os.path.join(self.dir, k[:2], k)

    def get(self, k):
        """the stored object for key k, or None (cache off, missing or unreadable entry)"""
        if not self.dir or not k:
            return None
        p = self._path(k)
        try:
            with open(p, "rb") as f:
                r = pickle.loads(zlib.decompress(f.read()))
        except (OSError, ValueError, EOFError, pickle.UnpicklingError, zlib.error, AttributeError, ImportError):
            self.misses += 1
            return None
        try:
            os.utime(p)  # recently used: kept by the size cap
        except OSError:
            pass
        self.hits += 1
        return r

    def put(self, k, obj):
        if not self.dir or not k:
            return
        p = self._path(k)
        tmp = f"{p}.{os.getpid()}.tmp"
        try:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(tmp, "wb") as f:
                f.write(zlib.compress(pickle.dumps(obj, protocol=pickle.HIGHEST_PROTOCOL), 1))
            os.replace(tmp, p)
        except OSError as e:  # a full / read-only cache never fails the build
            print(f"buildcache: {self.ns}: not stored ({e})", flush=True)
            try:
                os.remove(tmp)
            except OSError:
                pass

    def check(self, k, cached, fresh, same=None):
        """verify mode: a cached result against a recomputed one (same(a, b) -> bool, default: equal pickles)"""
        ok = same(cached, fresh) if same else (
            pickle.dumps(cached, protocol=pickle.HIGHEST_PROTOCOL) == pickle.dumps(fresh, protocol=pickle.HIGHEST_PROTOCOL))
        if ok:
            self.verified += 1
            return
        msg = f"buildcache verify: {self.ns} entry {k} differs from a fresh run (its key misses an input)"
        try:
            with open(os.path.join(root(), "verify.log"), "a") as f:
                f.write(msg + "\n")
        except OSError:
            pass
        raise VerifyError(msg)

    def cached(self, k, compute, same=None):
        """the result for key k: from the cache, else compute() (stored); in verify mode a hit is recomputed and
        compared"""
        r = self.get(k)
        if r is None:
            r = compute()
            self.put(k, r)
        elif self.verify:
            self.check(k, r, compute(), same)
        return r

    def summary(self):
        if not self.dir:
            return f"{self.ns} cache off"
        v = f", {self.verified} verified" if self.verify else ""
        return f"{self.ns} cache {self.hits} hits, {self.misses} misses{v} ({self.dir})"

    def trim(self):
        """drop least recently used entries until the namespace is under 80 % of its cap (when it is over the cap)"""
        if self.dir:
            trim_dir(self.dir, self.cap)

    def _exit(self):
        if os.getpid() == self._pid:  # not in forked workers (they never run atexit anyway)
            try:
                self.trim()
            except OSError:
                pass


def _entries(d):
    out = []
    try:
        subs = os.listdir(d)
    except OSError:
        return out
    for sub in subs:
        p = os.path.join(d, sub)
        if not os.path.isdir(p):
            continue
        for n in os.listdir(p):
            try:
                st = os.stat(os.path.join(p, n))
                out.append((st.st_mtime, st.st_size, os.path.join(p, n)))
            except OSError:
                pass
    return out


def trim_dir(d, cap):
    ents = _entries(d)
    total = sum(e[1] for e in ents)
    if total <= cap:
        return 0
    n = 0
    for _, size, p in sorted(ents):
        try:
            os.remove(p)
        except OSError:
            continue
        n += 1
        total -= size
        if total <= 0.8 * cap:
            break
    return n


def main(argv):
    cmd = argv[0] if argv else "stats"
    r = root()
    names = argv[1:] or [n for n in NAMESPACES if os.path.isdir(os.path.join(r, n))]
    if cmd == "stats":
        print(f"cache root {r} (DSR_CACHE)")
        for n in names:
            e = _entries(os.path.join(r, n))
            print(f"  {n:10} {len(e):7} entries {sum(x[1] for x in e) / (1 << 20):9.1f} MB")
    elif cmd == "trim":
        for n in names:
            cap = float(os.environ.get("BUILDCACHE_GB") or (10 if n == "simplify" else DEFAULT_GB)) * (1 << 30)
            print(f"{n}: {trim_dir(os.path.join(r, n), cap)} entries removed")
    elif cmd == "clear" and argv[1:]:
        import shutil
        for n in argv[1:]:
            shutil.rmtree(os.path.join(r, n), ignore_errors=True)
            print(f"{n}: cleared")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
