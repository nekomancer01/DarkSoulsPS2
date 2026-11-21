"""Export-step cache (Polaris 3.2 part 2): runs one recipe step's tool, or restores its outputs from the build cache's
"step" namespace (tools/buildcache.py) when the same step ran before with the same inputs, anywhere.

    python3 -I tools/stepcache.py <build> <name> <outputs...> :: <prerequisites...> :: <reads...> :: <env names...> ::
                                  <tool> <args...>

The generated make rules (tools/recipe.py make_command) call the step's tool through this. The key covers:
  - every prerequisite's bytes (make's $^: the step's inputs, its tools' code stamps $(TS)/<tool>.sum, the knob and
    area stamps), and the bytes of the files it reads that are not prerequisites (the recipe's `reads`)
  - the tool and its arguments, the environment variables the recipe sets for the step, the exported knobs and every
    registered build switch that is set (tools/switches.tsv, except kind config), with the build dir, the repo and the
    game folder written as placeholders, so another worktree, build dir or machine shares the entry
  - the game version (tools/verify_game.py); an unsupported or modified game copy is never cached
The entry holds every output (out and side) and the tool's output, replayed on a hit, so logs read the same.

  STEPCACHE=0         run every step's tool directly (the build cache's other namespaces stay on); BUILDCACHE=0 also
  BUILDCACHE_VERIFY=1 a hit runs the tool too and its outputs must equal the entry's (else the step fails)
  STEPCACHE_GB        size cap of the step namespace (default 12)
Each step prints one line: "stepcache: hit|miss|verified|off <name> ..." (tools/buildtui.py counts them)."""
import os, subprocess, sys, time

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import buildcache  # noqa: E402

ROOT = os.path.dirname(TOOLS)
VERSION = b"step-1"
SEP = "::"
# registered switches that never change what a step writes (queue, cache and job settings are kind config already)
NOT_OUTPUT = {"DSR_CACHE", "BUILDCACHE", "BUILDCACHE_VERIFY", "BUILDCACHE_GB", "STEPCACHE", "STEPCACHE_GB"}


def switch_names():
    """environment switches registered in tools/switches.tsv that can change an export (kind other than config)"""
    out = set()
    try:
        for line in open(os.path.join(TOOLS, "switches.tsv")):
            f = line.rstrip("\n").split("\t")
            if len(f) > 3 and not line.startswith("#") and f[0] != "name" and f[3] != "config":
                out.add(f[0])
    except OSError:
        pass
    return out - NOT_OUTPUT


def game_id():
    """the supported game version's id, or None (no game found, unsupported or changed copy)"""
    try:
        import gamepath, verify_game
        g = gamepath.find()  # (folder, where it was found) or None
        g = g[0] if isinstance(g, tuple) else g
        if not g:
            return None, None
        v, _ = verify_game.identify(g)
        return (v["id"] if v else None), os.path.abspath(g)
    except Exception:
        return None, None


def split(argv):
    parts, cur = [], []
    for a in argv:
        if a == SEP and len(parts) < 4:
            parts.append(cur)
            cur = []
        else:
            cur.append(a)
    parts.append(cur)
    if len(parts) != 5 or len(parts[0]) < 3 or not parts[4]:
        sys.exit(__doc__)
    return parts


def main(argv):
    (build, name, *outs), prereq, reads, envs, cmd = split(argv)
    c = buildcache.Cache("step", VERSION, where=os.environ.get("STEPCACHE"),
                         cap_gb=os.environ.get("STEPCACHE_GB", 12))
    run = [sys.executable, "-I"] + cmd
    gid, game = game_id()
    if not c.dir or not gid:
        print(f"stepcache: off {name}" + ("" if c.dir else " (STEPCACHE / BUILDCACHE)") +
              ("" if gid or not c.dir else " (game version unknown: not cached)"), flush=True)
        sys.exit(subprocess.run(run).returncode)
    subst = [(os.path.abspath(build), "<B>"), (ROOT, "<R>")] + ([(game, "<G>")] if game else [])

    def norm(s):
        for a, b in subst:
            s = s.replace(a, b)
        return s

    def files(paths):
        out = []
        for p in paths:
            out.append(norm(os.path.abspath(p)))
            out.append(buildcache.File(p) if os.path.isfile(p) else ("absent" if not os.path.exists(p) else "dir"))
        return out

    t0 = time.time()
    env = sorted((k, norm(os.environ[k])) for k in set(envs) | switch_names() if k in os.environ)
    k = c.key(gid, [norm(a) for a in cmd], [norm(os.path.abspath(o)) for o in outs], files(prereq), files(reads), env)
    tk = time.time() - t0
    hit = c.get(k)
    if hit is not None and not c.verify:
        for o, data in zip(outs, hit["outs"]):
            if data is None:  # the run did not write it (a maybe output): neither does a hit
                try:
                    os.remove(o)
                except OSError:
                    pass
                continue
            try:
                if os.path.getsize(o) == len(data) and open(o, "rb").read() == data:
                    os.utime(o)  # the same bytes: newer, like a fresh write, without rewriting
                    continue
            except OSError:
                pass
            tmp = f"{o}.{os.getpid()}.sc"
            os.makedirs(os.path.dirname(os.path.abspath(o)), exist_ok=True)
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, o)
        sys.stdout.write(hit["log"])
        print(f"stepcache: hit {name} ({hit['secs']:.1f} s saved, key {k[:12]}, {tk:.2f} s to key)", flush=True)
        return 0
    t1 = time.time()
    p = subprocess.Popen(run, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    log = []
    for line in p.stdout:  # passed on as it comes (progress views, tee'd logs), kept for the entry
        sys.stdout.write(line)
        sys.stdout.flush()
        log.append(line)
    rc = p.wait()
    if rc:
        return rc
    data = []
    for o in outs:  # a missing file is stored as absent: make reports a missing out / side file itself
        try:
            data.append(open(o, "rb").read())
        except FileNotFoundError:
            data.append(None)
    secs = time.time() - t1
    if hit is not None:
        bad = [o for o, a, b in zip(outs, hit["outs"], data) if a != b]
        if bad or len(hit["outs"]) != len(data):
            msg = f"stepcache verify: {name}: {', '.join(map(os.path.basename, bad))} differ from entry {k} " \
                  "(its key misses an input)"
            try:
                with open(os.path.join(buildcache.root(), "verify.log"), "a") as f:
                    f.write(msg + "\n")
            except OSError:
                pass
            print(msg, flush=True)
            return 1
        print(f"stepcache: verified {name} (key {k[:12]})", flush=True)
        return 0
    c.put(k, {"outs": data, "log": "".join(log), "secs": secs})
    print(f"stepcache: miss {name} ({secs:.1f} s, stored {k[:12]})", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
