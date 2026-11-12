#!/usr/bin/env python3
"""Warm starts for tools/test-run.py: a PCSX2 savestate of the ELF parked at a test's start, so later runs of the same
build skip the boot (ISO: ~25-45 s of BIOS, disc reads and the first stream-in before the script's first command).

How (src/testscript.c "park", mailbox version 2):
  - cold run (no state yet): the ELF boots with a park script = the test's option lines + its first spawn + "wait 3" +
    "park". Parked (mailbox state 4), test-run.py saves a state over PINE into the cache, then names the test's own
    script in the mailbox (next, next_seq) and the run goes on in the same PCSX2: the first run costs no extra boot.
  - warm run: PCSX2 starts with -statefile <cached state> (tools/pcsx2-run.sh STATEFILE), test-run.py finds the parked
    ELF and hands it the script the same way. A state that does not come up parked is dropped and the run boots cold.
The key: the ELF's and the disc image's bytes, the PCSX2 build, and the park script (options + first spawn), so a
rebuilt ELF / ISO, another start spot or other boot options never load an old state. Scripts with "option save" (a save
file read at boot) or "option demo" always boot cold, and so does every host-ELF run (no --iso): the ELF keeps its
host: packs open from boot, and a loaded state has none of PCSX2's HostFs file handles, so the first pack read after it
fails ("host:data/c1201.pak model.bin: read error", smoke on 2026-11-10: 19 of 20 warm host runs failed). A host boot
is ~7 s anyway; ISO packs are read by sector (dsrsnd's disc table) and keep no handle.
Cache: WARM_CACHE (a directory; 0 = off: every run boots cold, as test-run.py --cold), default
<main tree>/build/cache/warm (DSR_CACHE: the cache root); entries older than WARM_DAYS (2) days are pruned, and the
newest WARM_KEEP (60) kept.

  warmstate.py list | clear     the cache's entries / delete them"""
import glob, hashlib, json, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = b"warm-2"  # warm-2: ISO runs only (warm-1 also keyed host-ELF runs, whose states never worked)
WAIT_PARK = 3  # game seconds at the start spot before parking: the first stream-in settles


def main_tree():
    root = os.path.dirname(HERE)
    g = os.path.join(root, ".git")
    try:
        if os.path.isfile(g):
            gitdir = os.path.normpath(os.path.join(root, open(g).read().split("gitdir:", 1)[1].strip()))
            common = open(os.path.join(gitdir, "commondir")).read().strip()
            return os.path.dirname(os.path.normpath(os.path.join(gitdir, common)))
    except (OSError, IndexError):
        pass
    return root


def cache_dir():
    c = os.environ.get("WARM_CACHE")
    if c == "0":
        return None
    return c or os.path.join(os.environ.get("DSR_CACHE") or os.path.join(main_tree(), "build", "cache"), "warm")


_hashes = {}


def file_hash(path):
    """sha256 of a file, memoised on (path, size, mtime): the ISO is ~180 MB"""
    st = os.stat(path)
    k = (os.path.realpath(path), st.st_size, st.st_mtime_ns)
    if k not in _hashes:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for b in iter(lambda: f.read(1 << 22), b""):
                h.update(b)
        _hashes[k] = h.hexdigest()
    return _hashes[k]


def pcsx2_build(hostexec):
    try:
        return subprocess.run([hostexec, "flatpak", "info", "--show-commit", "net.pcsx2.PCSX2"], capture_output=True,
                              text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return ""


def parks(elf):
    """the ELF knows the "park" command (testscript.c since mailbox version 2): older ELFs boot cold"""
    with open(elf, "rb") as f:
        return b"TEST parked" in f.read()


def park_script(script):
    """The park script for a test script (its options, its first spawn when that is the first command), or None when
    the script must boot cold"""
    opts, spawn = [], None
    for line in open(script, errors="replace"):
        s = line.split("#", 1)[0].strip()
        if not s:
            continue
        w = s.split()[0]
        if w == "option":
            if re.match(r"option\s+(save|demo)\b", s):
                return None
            opts.append(" ".join(s.split()))
        elif spawn is None:
            spawn = " ".join(s.split()) if w == "spawn" else ""
    return "".join(o + "\n" for o in opts) + (spawn + "\n" if spawn else "") + f"wait {WAIT_PARK}\npark\n"


class Plan:
    """One run's warm start: key, park script, cached state (or None)"""

    def __init__(self, key, park, d):
        self.key, self.park, self.dir = key, park, d
        self.state = os.path.join(d, key + ".p2s")
        self.meta = os.path.join(d, key + ".json")
        self.cached = os.path.isfile(self.state) and os.path.isfile(self.meta)

    def mailbox(self):
        try:
            return json.load(open(self.meta)).get("mailbox")
        except (OSError, ValueError):
            return None

    def store(self, p2s, mailbox, info):
        """copy a saved state into the cache (rename: parallel runs never read half a one)"""
        os.makedirs(self.dir, exist_ok=True)
        tmp = f"{self.state}.{os.getpid()}"
        shutil.copyfile(p2s, tmp)
        with open(tmp + ".json", "w") as f:
            json.dump(dict(info, mailbox=mailbox, park=self.park, saved=time.time()), f)
        os.replace(tmp + ".json", self.meta)
        os.replace(tmp, self.state)
        self.cached = True
        prune(self.dir)

    def drop(self):
        for f in (self.state, self.meta):
            try:
                os.remove(f)
            except OSError:
                pass
        self.cached = False


def plan(elf, script, iso, hostexec):
    """Plan for this run, or None (cache off, or a script that boots cold)"""
    d = cache_dir()
    park = park_script(script) if d and iso and parks(elf) else None
    if park is None:
        return None
    h = hashlib.sha256(VERSION)
    for part in (file_hash(elf), file_hash(iso) if iso else "host", pcsx2_build(hostexec), park):
        h.update(b"\0" + part.encode())
    return Plan(h.hexdigest()[:32], park, d)


def newest_state(sstates, since):
    """the .p2s PCSX2 wrote into sstates/ after `since`, once its size holds still (None after 60 s)"""
    t0, last = time.time(), None
    while time.time() - t0 < 60:
        c = [f for f in glob.glob(os.path.join(sstates, "*.p2s")) if os.path.getmtime(f) >= since - 1]
        if c:
            f = max(c, key=os.path.getmtime)
            sz = os.path.getsize(f)
            if last == (f, sz) and sz > 0:
                return f
            last = (f, sz)
        time.sleep(0.5)
    return None


def prune(d):
    days, keep = float(os.environ.get("WARM_DAYS", 2)), int(os.environ.get("WARM_KEEP", 60))
    ents = sorted(glob.glob(os.path.join(d, "*.p2s")), key=os.path.getmtime, reverse=True)
    for i, f in enumerate(ents):
        if i >= keep or time.time() - os.path.getmtime(f) > days * 86400:
            for g in (f, f[:-4] + ".json"):
                try:
                    os.remove(g)
                except OSError:
                    pass


def main():
    d = cache_dir()
    if len(sys.argv) < 2 or sys.argv[1] not in ("list", "clear") or not d:
        sys.exit(__doc__)
    ents = sorted(glob.glob(os.path.join(d, "*.p2s")), key=os.path.getmtime)
    for f in ents:
        if sys.argv[1] == "clear":
            for g in (f, f[:-4] + ".json"):
                os.remove(g)
            continue
        try:
            m = json.load(open(f[:-4] + ".json"))
        except (OSError, ValueError):
            m = {}
        print(f"{os.path.basename(f)[:12]}  {os.path.getsize(f) >> 20} MB  {time.strftime('%m-%d %H:%M', time.localtime(os.path.getmtime(f)))}"
              f"  {m.get('elf', '?')}  {m.get('iso') or 'host ELF'}  {' / '.join(m.get('park', '').splitlines()[:-2])}")
    if sys.argv[1] == "clear":
        print(f"warmstate: {len(ents)} entries deleted from {d}")


if __name__ == "__main__":
    main()
