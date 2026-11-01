#!/usr/bin/env python3
"""What a test run ran: the tree's git revision + a hash of its uncommitted changes, and the hash / time of each ELF and
script, so a result can be matched to the build it came from (stale ELFs, logs or scripts made threads redo runs).

  runstamp.py <file>...            print the stamp as JSON (test-all.sh writes it to <run>/stamp.json)
  import runstamp; runstamp.stamp([elf, script])   (tools/test-run.py: result.json "stamp")"""
import hashlib, json, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _git(*args):
    try:
        return subprocess.run(["git", "-C", ROOT, *args], capture_output=True, timeout=20).stdout
    except (OSError, subprocess.TimeoutExpired):
        return b""


def sha(path, n=16):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()[:n]


def stamp(files=()):
    rev = _git("rev-parse", "--short=12", "HEAD").decode().strip()
    # uncommitted tracked changes in the code that builds / runs (notes and logs left out)
    diff = _git("diff", "HEAD", "--", "ps2", "tools", "config.mk")
    untracked = _git("ls-files", "--others", "--exclude-standard", "--", "ps2", "tools").decode().split()
    h = hashlib.sha256(diff)
    for u in sorted(untracked):
        try:
            h.update(u.encode() + open(os.path.join(ROOT, u), "rb").read())
        except OSError:
            pass
    st = {"time": time.strftime("%Y-%m-%d %H:%M:%S"), "tree": os.path.basename(ROOT), "git": rev,
          "dirty": h.hexdigest()[:12] if diff or untracked else "", "files": {}}
    for f in files:
        try:
            st["files"][os.path.relpath(os.path.realpath(f), ROOT)] = {
                "sha": sha(f), "mtime": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(f)))}
        except OSError:
            st["files"][f] = None
    return st


def line(st):
    """one-line summary for logs / reports"""
    fs = ", ".join(f"{os.path.basename(k)} {v['sha'][:8]} ({v['mtime']})" if v else f"{os.path.basename(k)} missing"
                   for k, v in st["files"].items())
    return f"{st['tree']} {st['git']}{' +dirty ' + st['dirty'] if st['dirty'] else ''}; {fs}"


if __name__ == "__main__":
    print(json.dumps(stamp(sys.argv[1:]), indent=1))
