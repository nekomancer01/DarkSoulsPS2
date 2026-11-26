"""Build profiler: reads the timing rows tools/stepcache.py writes to <build>/stepcache.tsv (one per recipe step) and
answers where an export build spends its time, and checks two builds for identical outputs.

    python3 tools/buildprof.py report   <build> [--top N]      per step name: runs, hits, wall sum, child CPU sum, max
    python3 tools/buildprof.py critical <build>                longest chain through the steps' prerequisites (the step
                                                               that ended last, back through the prerequisite that ended
                                                               last), with each step's wall, the gap before it and its
                                                               tools/q queue wait from notes/jobq.log when found
    python3 tools/buildprof.py replay   <build> -j N [--order FILE] [--scale step=factor ...]
                                                               simulated wall on N job slots, with the Makefile's world
                                                               phase barriers (PHASES) and blocks started in the
                                                               observed order or in FILE's (one block per line)
    python3 tools/buildprof.py order    <build>                blocks, the most phase work first, one per line
    python3 tools/buildprof.py edits    <build> <tool>         steps whose recipe `tools` list holds <tool> (what an
                                                               edit of it re-runs), with their last wall / CPU
    python3 tools/buildprof.py hash     <build>                sha256 of every file under data/ and gen/
                                                               (sha256sum format; a sha file for `sha`)
    python3 tools/buildprof.py sha      <A> <B> [--strict] [--list]
                                                               A / B: build dirs or `hash` files; exit 0 when equal
                                                               apart from VOLATILE (--strict: those too), 1 otherwise

stepcache.tsv columns (tab-separated): t_start t_end result name key12 wall_s cpu_s outputs prereqs; result is hit, miss,
verified, off or fail; cpu_s is the step's child processes' user + system CPU; outputs / prereqs are ;-joined paths
with the build dir written as <B> (tools/stepcache.py; STEPCACHE_LOG=0 writes none). A build dir reused for several
builds holds all their rows: the commands read the rows after the last gap of more than GAP s (the latest build).
"""
import argparse, fnmatch, hashlib, heapq, os, re, sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
GAP = 600  # s: rows after a pause this long belong to a new build run
# the world's phase steps (ps2/proto/Makefile, WORLD_HZN_FIRST=1 WORLD_EARLY_CHRS=1): worldexports (blockexport:
# objects, lights, map, collision), then worldhzn, then worldpvs / worldfar / worldchrs (the bakes and EARLY_EXPORTS,
# order-only after worldhzn), then worldpacks (map split, disc layout and the packs, which stepcache does not time). A phase's steps start only after every step of the phases before it ended; steps not
# named here (packs, map split, game-wide steps) have no barrier, only their prerequisites.
PHASES = {"objects": 0, "lights": 0, "map": 0, "collision": 0, "horizon": 1,
          "visibility": 2, "far panorama": 2, "{CHR}": 2, "enemies": 2, "map AI": 2, "events": 2, "effects": 2,
          "map split": 3, "disc layout": 3}
PHASE_NAMES = ["exports", "horizons", "bakes+chrs", "packs"]
# outputs that differ between equal builds: logs, the ISO's date sectors, the timing files and the GPU-made bakes
# (PR #338: far.bin and pvs.bin vary on the GPU; their identity is checked with FAR_GPU=0 / PVS_GPU=0)
VOLATILE = ["snd.log", "proto.iso", "stepcache.tsv", "stepcache.hashes", "*/far.bin", "*/pvs.bin"]
CHR = re.compile(r"^c\d{4}$")


class Row:
    __slots__ = ("t0", "t1", "result", "name", "key", "wall", "cpu", "outs", "pre", "i")

    def __init__(self, f, i):
        self.t0, self.t1 = float(f[0]), float(f[1])
        self.result, self.name, self.key = f[2], f[3], f[4]
        self.wall, self.cpu = float(f[5]), float(f[6])
        self.outs = [p for p in f[7].split(";") if p]
        self.pre = [p for p in f[8].split(";") if p] if len(f) > 8 else []
        self.i = i

    @property
    def group(self):
        """the recipe step's name: characters fold into {CHR}"""
        if CHR.match(self.name):
            return "{CHR}"
        if self.name.endswith(" pack") and CHR.match(self.name[:-5]):
            return "{CHR} pack"
        return self.name

    @property
    def block(self):
        """the world block of gen/<block>/..., "boot" for the boot block's own files (gen/<file>, gen/c####/...,
        made by the top make, not a block sub-make), None for data/ and other game-wide outputs"""
        for o in self.outs:
            m = re.search(r"/gen/(m\d\d_\d\d_\d\d_\d\d[^/]*)/", o)
            if m:
                return m.group(1)
        for o in self.outs:
            if "/gen/" in o:
                return "boot"
        return None

    @property
    def phase(self):
        return PHASES.get(self.group) if self.block is not None else None


def load(build, every=False):
    p = os.path.join(build, "stepcache.tsv")
    if not os.path.exists(p):
        print(f"buildprof: no {p} (a build with tools/stepcache.py's timing rows; STEPCACHE_LOG=0 writes none)",
              file=sys.stderr)
        sys.exit(2)
    rows = []
    for line in open(p, errors="replace"):
        f = line.rstrip("\n").split("\t")
        if len(f) >= 8:
            try:
                rows.append(Row(f, len(rows)))
            except ValueError:
                pass
    rows.sort(key=lambda r: r.t0)
    if not every:
        cut, end = 0, None
        for i, r in enumerate(rows):
            if end is not None and r.t0 - end > GAP:
                cut = i
            end = max(end or r.t1, r.t1)
        rows = rows[cut:]
    for i, r in enumerate(rows):
        r.i = i
    return rows


def span(rows):
    return (max(r.t1 for r in rows) - min(r.t0 for r in rows)) if rows else 0.0


def cmd_report(a):
    rows = load(a.build, a.all)
    g = {}
    for r in rows:
        s = g.setdefault(r.group, [0, 0, 0.0, 0.0, 0.0])
        s[0] += 1
        s[1] += r.result == "hit"
        s[2] += r.wall
        s[3] += r.cpu
        s[4] = max(s[4], r.wall)
    tw, tc = sum(s[2] for s in g.values()), sum(s[3] for s in g.values())
    print(f"{len(rows)} steps, {sum(r.result == 'hit' for r in rows)} hits, span {span(rows):.1f} s, "
          f"wall sum {tw:.1f} s, CPU sum {tc:.1f} s")
    print(f"{'step':<24}{'runs':>6}{'hits':>6}{'wall s':>10}{'CPU s':>10}{'CPU %':>7}{'max s':>9}")
    for name, s in sorted(g.items(), key=lambda kv: -kv[1][3] - kv[1][2] * 1e-6)[:a.top]:
        print(f"{name[:23]:<24}{s[0]:>6}{s[1]:>6}{s[2]:>10.1f}{s[3]:>10.1f}{100 * s[3] / (tc or 1):>7.1f}{s[4]:>9.1f}")
    return 0


def producers(rows):
    made = {}
    for r in rows:
        for o in r.outs:
            made[o] = r  # the last row writing it
    return made


def deps(rows, barriers=False):
    """per row, the rows that wrote its prerequisites (by end time); barriers=True adds the step of the phase before
    its own that ended last (the Makefile's phase sub-makes wait for it)"""
    made = producers(rows)
    out = [{made[p].i for p in r.pre if p in made and made[p] is not r and made[p].t0 <= r.t0} for r in rows]
    if barriers:
        last = {}
        for r in rows:
            if r.phase is not None and (r.phase not in last or r.t1 > rows[last[r.phase]].t1):
                last[r.phase] = r.i
        for r in rows:
            if r.phase and r.phase - 1 in last and rows[last[r.phase - 1]].t1 <= r.t0 + 1:
                out[r.i].add(last[r.phase - 1])
    return [sorted(x, key=lambda j: rows[j].t1) for x in out]


def queue_waits(build, rows):
    """{row index: tools/q wait s} for start lines in notes/jobq.log naming this build and the row's first output"""
    log = os.path.join(ROOT, "notes", "jobq.log")
    out = {}
    if not os.path.exists(log):
        return out
    import time as _t
    b = os.path.abspath(build)
    want = {}
    for r in rows:
        if r.outs:
            want.setdefault(r.outs[0].replace("<B>", b), []).append(r)
    pat = re.compile(r"^(\S+ \S+) start pid \d+ .* wait (\d+) s \[[^]]*\]: (.*)$")
    for line in open(log, errors="replace"):
        if " start pid " not in line or "stepcache.py" not in line or b not in line:
            continue
        m = pat.match(line.rstrip("\n"))
        if not m:
            continue
        t = _t.mktime(_t.strptime(m.group(1), "%Y-%m-%d %H:%M:%S"))
        for w in m.group(3).split():
            for r in want.get(w, ()):
                if abs(t - r.t0) < 3:
                    out[r.i] = int(m.group(2))
    return out


def cmd_critical(a):
    rows = load(a.build, a.all)
    if not rows:
        return 0
    d = deps(rows, barriers=True)
    r = max(rows, key=lambda x: x.t1)
    chain = [r]
    while d[r.i]:
        r = rows[d[r.i][-1]]  # the prerequisite that ended last
        chain.append(r)
    chain.reverse()
    waits = queue_waits(a.build, chain)
    t_begin = min(x.t0 for x in rows)
    print(f"critical chain: {len(chain)} steps, {sum(x.wall for x in chain):.1f} s of step wall in a "
          f"{span(rows):.1f} s build")
    print(f"{'start s':>8}{'gap s':>8}{'wall s':>9}{'CPU s':>9}{'q wait':>8}  result  step")
    prev = t_begin
    for x in chain:
        w = waits.get(x.i)
        print(f"{x.t0 - t_begin:>8.1f}{x.t0 - prev:>8.1f}{x.wall:>9.1f}{x.cpu:>9.1f}"
              f"{(str(w) if w is not None else '-'):>8}  {x.result:<7} {x.name}"
              + (f" [{x.block}]" if x.block else ""))
        prev = x.t1
    print("gap = waiting on something the rows do not show: a free job slot, a phase barrier, a step stepcache does not "
          "time (packs, the ELF)")
    for p in range(len(PHASE_NAMES)):
        ph = [x for x in rows if x.phase == p]
        if ph:
            print(f"  phase {PHASE_NAMES[p]:<11} {min(x.t0 for x in ph) - t_begin:>7.1f} -> "
                  f"{max(x.t1 for x in ph) - t_begin:>7.1f} s, {len(ph)} steps, {sum(x.wall for x in ph):.0f} s wall, "
                  f"{sum(x.cpu for x in ph):.0f} s CPU")
    return 0


def block_order(rows, order_file=None):
    seen = []
    for r in sorted(rows, key=lambda x: x.t0):
        b = r.block
        if b and r.phase is not None and b not in seen:
            seen.append(b)
    if order_file:
        want = [ln.strip().split(":")[-1] for ln in open(order_file) if ln.strip()]
        seen = [b for b in want if b in seen] + [b for b in seen if b not in want]
    return seen


def replay(rows, slots, order=None, scale=None):
    """list scheduling of the rows' wall times on `slots` slots: a step starts when its producers ended, its phase's
    barrier is open (every step of the earlier phases ended) and a slot is free; ready phase steps go first, by
    phase, block order and recorded start, then the other steps by recorded start. Returns (simulated span, {phase: (start, end)})"""
    scale = scale or {}
    d = deps(rows)
    order = order or block_order(rows)
    rank = {b: i for i, b in enumerate(order)}
    dur = [r.wall * scale.get(r.group, scale.get(r.name, 1.0)) for r in rows]
    left = [len(x) for x in d]
    users = [[] for _ in rows]
    for i, x in enumerate(d):
        for j in x:
            users[j].append(i)
    per_phase = [sum(1 for r in rows if r.phase == p) for p in range(len(PHASE_NAMES))]
    done_phase = [0] * len(PHASE_NAMES)

    def barrier_open(p):
        return p is None or all(done_phase[q] == per_phase[q] for q in range(p))

    def prio(i):
        r = rows[i]
        if r.phase is None:  # steps outside the phases fill free slots, in their recorded order
            return (0, 1, 0, r.t0, i)
        return (r.phase, 0, rank.get(r.block, len(rank)), r.t0, i)

    waiting = set(i for i in range(len(rows)) if left[i] == 0)
    ready, running, t, free = [], [], 0.0, slots
    started, ph = [None] * len(rows), {}

    def admit():
        for i in [i for i in waiting if barrier_open(rows[i].phase)]:
            waiting.discard(i)
            heapq.heappush(ready, (prio(i), i))

    admit()
    while ready or running:
        while ready and free:
            _, i = heapq.heappop(ready)
            started[i] = t
            heapq.heappush(running, (t + dur[i], i))
            free -= 1
        if not running:
            break
        t, i = heapq.heappop(running)
        free += 1
        p = rows[i].phase
        if p is not None:
            done_phase[p] += 1
            s, e = ph.get(p, (started[i], t))
            ph[p] = (min(s, started[i]), max(e, t))
        for u in users[i]:
            left[u] -= 1
            if left[u] == 0:
                waiting.add(u)
        admit()
    stuck = [rows[i] for i in range(len(rows)) if started[i] is None]
    if stuck:
        print(f"replay: {len(stuck)} steps never became ready (a phase waits on a later step): "
              + ", ".join(sorted({x.name for x in stuck})[:8]), file=sys.stderr)
    return t, ph


def cmd_replay(a):
    rows = load(a.build, a.all)
    scale = {}
    for s in a.scale or []:
        k, _, v = s.rpartition("=")
        scale[k] = float(v)
    order = block_order(rows, a.order)
    t, ph = replay(rows, a.j, order, scale)
    print(f"replay -j {a.j}: {t:.1f} s (recorded span {span(rows):.1f} s, {len(rows)} steps)")
    for p in sorted(ph):
        print(f"  {PHASE_NAMES[p]:<9} {ph[p][0]:>8.1f} -> {ph[p][1]:>8.1f} s")
    return 0


def order_list(rows):
    obs = block_order(rows)
    work = {}
    for r in rows:
        if r.phase is not None and r.block:
            work[r.block] = work.get(r.block, 0.0) + r.wall
    return sorted(obs, key=lambda b: (-work.get(b, 0.0), obs.index(b)))


def cmd_order(a):
    for b in order_list(load(a.build, a.all)):
        print(b)
    return 0


def recipe_steps():
    import tomllib
    with open(os.path.join(TOOLS, "recipe.toml"), "rb") as f:
        return tomllib.load(f).get("step", [])


def tool_match(entry, tool):
    tool = tool.removeprefix("tools/")
    return entry == tool or fnmatch.fnmatch(tool, entry)


def cmd_edits(a):
    steps = [s for s in recipe_steps() if any(tool_match(e, a.tool) for e in s.get("tools", []))]
    names = {s["name"] for s in steps}
    rows = load(a.build, a.all)
    last = {}
    for r in rows:
        if r.group in names:
            last[(r.name, r.block)] = r
    n, w, c = 0, 0.0, 0.0
    print(f"{a.tool}: in the tools of {len(steps)} recipe steps: {', '.join(sorted(names)) or '-'}")
    per = {}
    for r in last.values():
        s = per.setdefault(r.group, [0, 0.0, 0.0])
        s[0] += 1
        s[1] += r.wall
        s[2] += r.cpu
        n, w, c = n + 1, w + r.wall, c + r.cpu
    for g, s in sorted(per.items(), key=lambda kv: -kv[1][1]):
        print(f"  {g:<24}{s[0]:>5} steps {s[1]:>9.1f} s wall {s[2]:>9.1f} s CPU")
    print(f"total: {n} steps, {w:.1f} s wall, {c:.1f} s CPU (their last run in this build; packs not counted)")
    return 0


def volatile(rel):
    return any(fnmatch.fnmatch(rel, v) or fnmatch.fnmatch(os.path.basename(rel), v) for v in VOLATILE)


def hashes(src):
    """{relative path: sha256} of a build dir's data/ and gen/ (or of a `hash` file)"""
    out = {}
    if os.path.isfile(src):
        for line in open(src):
            if line.startswith("#") or not line.strip():
                continue
            h, rel = line.rstrip("\n").split("  ", 1)
            out[rel] = h
        return out
    for top in ("data", "gen"):
        base = os.path.join(src, top)
        for d, dirs, files in os.walk(base):
            dirs.sort()
            for f in sorted(files):
                p = os.path.join(d, f)
                if not os.path.isfile(p):
                    continue
                h = hashlib.sha256()
                with open(p, "rb") as fh:
                    for chunk in iter(lambda: fh.read(1 << 20), b""):
                        h.update(chunk)
                out[os.path.relpath(p, src)] = h.hexdigest()
    return out


def cmd_hash(a):
    for rel, h in sorted(hashes(a.build).items()):
        print(f"{h}  {rel}")
    return 0


def cmd_sha(a):
    A, B = hashes(a.a), hashes(a.b)
    diff = []
    for rel in sorted(set(A) | set(B)):
        if not a.strict and volatile(rel):
            continue
        if A.get(rel) != B.get(rel):
            diff.append((rel, "only in B" if rel not in A else "only in A" if rel not in B else "differs"))
    for rel, why in diff if a.list or len(diff) <= 20 else diff[:20]:
        print(f"{why}: {rel}")
    if len(diff) > 20 and not a.list:
        print(f"... {len(diff) - 20} more (--list)")
    n = len(set(A) | set(B))
    print(f"sha: {n} files, {len(diff)} differ" + ("" if a.strict else f" (volatile skipped: {', '.join(VOLATILE)})"))
    return 1 if diff else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def add(name, fn, *args):
        p = sub.add_parser(name)
        for x in args:
            p.add_argument(x)
        if name not in ("hash", "sha"):
            p.add_argument("--all", action="store_true", help="every row, not only the latest build run")
        p.set_defaults(fn=fn)
        return p

    add("report", cmd_report, "build").add_argument("--top", type=int, default=40)
    add("critical", cmd_critical, "build")
    p = add("replay", cmd_replay, "build")
    p.add_argument("-j", type=int, default=8)
    p.add_argument("--order")
    p.add_argument("--scale", action="append", metavar="STEP=FACTOR")
    add("order", cmd_order, "build")
    add("edits", cmd_edits, "build", "tool")
    add("hash", cmd_hash, "build")
    p = add("sha", cmd_sha, "a", "b")
    p.add_argument("--strict", action="store_true")
    p.add_argument("--list", action="store_true")
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
