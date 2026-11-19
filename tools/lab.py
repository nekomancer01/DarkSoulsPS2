#!/usr/bin/env python3
"""The Lab (Polaris 1.7b): drive the host build of the simulate phase (build/host/lab, `make -C ps2/proto lab` on a host
gcc with -m32) from Python, and compare a recorded PCSX2 run (test option `record`) against it.

  from lab import Lab
  with Lab("build", "m10_02_00_00") as lab:
      lab.cmd("spawn -47.41 -59.82 53.3 5.2185")
      lab.cmd("stick 0 1")
      print(lab.step(30))            # {'frame': ..., 'pos': (x, y, z), 'yaw': ..., 'state': 'ESD 0/0 RUN 500'}

  lab.py compare TRACE STATE_LOG [--area A] [--build B] [--short]
      replays TRACE (build/save/<name>.trace) in the Lab and compares each frame with the PCSX2 run's STATE_LOG
      (<name>.trace.state). Gate (--short, traces of 10-20 s): every frame within 1 cm and the same state sequence.
      Long traces: the same sequence of states with each change within one frame, the end position within --end-cm.
      Always reported: the first frame more than 1 mm apart (how far the Lab drifts from the console).
      Version 3 traces recorded with enemies (NAME.trace.enemies): every enemy's (ESD, AI, action) and HP sequence must
      match the console's (the hard gate), positions within --enemy-cm are the target, the first frame more than 1 mm
      apart per enemy is reported, and the enemies whose draw group the console hid are listed.

  lab.py run SCRIPT... [--area A] [--build B] [--state STATE_LOG [--end-cm 1]] [-j N] [-v]
      runs each test script (tests/*.txt) in the Lab through the game's own interpreter (src/testscript.c) and prints
      per script: pass / fail, the checks that ran (a script whose checks all needed something the Lab lacks shows
      none), the commands that are presentation only there (shot, stats, loadscreen), the checks not run there (what is
      drawn: ev expect drawn / sfx / bossbar, pickup expect shines / dialog), the failures (TEST error lines, FAIL
      expects, a warp whose settle ran into world.c's 30 s cap, a stub whose result the simulation reads back) and the
      presentation stubs reached. Burg walks: the console's world holds the player while a tile loads, which a script
      run in the Lab never does; compare those with the recorded trace (compare replays the holds). Saves go to
      build/host/save (never the console tests' build/save), emptied once per invocation: `run tests/save_check.txt`
      is the save round trip (tests/save_roundtrip.sh): a script's "# needs: other.txt" lines run those first, in the
      same job (with -j, a job's scripts stay in order on one worker).
      --state: the script run against the console's own run of it (its option record .state log): the same state
      sequence and the end position within --end-cm (tests/det_walk_open.txt: 0.04 cm). Frame counts differ (the
      console's boot frames), so the sequence is compared without frame numbers.
      Replays: frames where the console's world held the player (trace flag 4) are held in the Lab too. Version 3:
      the draw-group state hash per frame (record word 21, drawgrp.c dg_state_hash) must match the Lab's.

The commands are lab/lab_main.c's (test-script names where one exists). The Lab is a 32-bit host program: on the
distrobox run lab.py through `distrobox-host-exec` (the container has no 32-bit loader)."""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Lab:
    def __init__(self, build="build", area="m10_02_00_00", binary=None, noenemies=False):
        build = os.path.abspath(os.path.join(ROOT, build))
        binary = binary or os.path.join(build, "host", "lab")
        self.p = subprocess.Popen([binary, build, area] + (["noenemies"] if noenemies else []), stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)

    def cmd(self, line):
        """one command; returns its answer line (or the lines up to "ok" for multi-line answers)"""
        self.p.stdin.write(line.rstrip("\n") + "\n")
        self.p.stdin.flush()
        out = []
        while True:
            s = self.p.stdout.readline()
            if not s:
                raise RuntimeError("lab: no answer to %r" % line)
            s = s.rstrip("\n")
            if s.startswith(("state ", "ok", "error")) and not line.startswith(("replay", "stubs")):
                return s
            if s == "ok":
                return out
            out.append(s)

    def step(self, n=1):
        return parse_state(self.cmd("step %d" % n))

    def replay(self, trace, enemies=None):
        """the Lab's state per frame; with a list for enemies, also its enemy rows (the replay's "enemy" lines)"""
        out = self.cmd("replay " + os.path.abspath(trace))
        self.dg = {int(t[1]): int(t[2]) for t in (s.split() for s in out if s.startswith("dg "))}
        if enemies is not None:
            enemies += [parse_enemy(s[len("enemy "):]) for s in out if s.startswith("enemy ")]
        return [parse_state(s) for s in out if s.startswith("state ")]

    def stubs(self):
        return [s.split(" ", 1)[1] for s in self.cmd("stubs") if s.startswith("stub ")]

    def close(self):
        if self.p.poll() is None:
            try:
                self.cmd("quit")
            except (RuntimeError, BrokenPipeError):
                pass
            self.p.wait(timeout=10)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def parse_state(s):
    """"state FRAME X Y Z YAW STATE..." (the Lab) or "FRAME X Y Z YAW STATE..." (option record's .state log)"""
    t = s.split()
    if t and t[0] == "state":
        t = t[1:]
    return {"frame": int(t[0]), "pos": tuple(float(v) for v in t[1:4]), "yaw": float(t[4]), "state": " ".join(t[5:8])}


def trace_header(trace):
    """(version, header bytes, record bytes, flags): version 2 = 20-byte header, 64-byte records, no flags (its gate
    traces were all recorded with option noenemies: flags 1); version 3 = 24 bytes with flags, records of h[4] bytes"""
    import struct
    h = struct.unpack_from("<6I", open(trace, "rb").read(24))
    return (2, 20, 64, 1) if h[1] == 2 else (h[1], 24, h[4], h[5])


def trace_records(trace):
    import struct
    _, hb, rb, _ = trace_header(trace)
    b = open(trace, "rb").read()
    for i in range((len(b) - hb) // rb):
        yield struct.unpack_from("<16I", b, hb + rb * i), struct.unpack_from("<16f", b, hb + rb * i)


def trace_start(trace):
    """the frame the comparison starts at: the last teleport (a test spawn, a warp: record flag 1) before the first
    frame with input, so the boot's own spot (in another block than the Lab loaded) is left out"""
    start = 0
    for u, f in trace_records(trace):
        if u[10] & 1:
            start = u[0]
        if any(abs(x) > 1e-6 for x in f[2:6]) or u[6] or u[7]:
            break
    return start


def trace_first_input(trace):
    """the first frame with any input: enemy comparisons start there (the wait before it lets the console's enemy types
    load; the Lab has them at once, so the two can only be compared once both have them)"""
    for u, f in trace_records(trace):
        if any(abs(x) > 1e-6 for x in f[2:6]) or u[6] or u[7]:
            return u[0]
    return 0


def trace_held(trace):
    """frames the console's world held the player (record flag 4: a block or tile still loading)"""
    return sum(1 for u, _ in trace_records(trace) if u[10] & 4)


def parse_enemy(s):
    """an enemy row (enemy.c enemy_trace_lines): frame entity x y z yaw esd ai act hp omit dg hx hz, keyed by
    (entity, hx, hz) (characters without an entity id are -1); None for a "FRAME T ..." types line"""
    t = s.split()
    if len(t) > 1 and t[1] == "T":  # (the frame's loaded types)
        return None
    key = (int(t[1]), t[12], t[13]) if len(t) > 13 else (int(t[1]),)
    return {"frame": int(t[0]), "entity": key, "pos": tuple(float(v) for v in t[2:5]), "yaw": float(t[5]),
            "state": " ".join(t[6:9]), "hp": int(t[9]), "omit": int(t[10]), "dg": int(t[11])}


def compare_enemies(lab_rows, ps2_rows, start, bound_cm=1.0):
    """per enemy (by entity) from frame `start`: the same sequence of (ESD, AI, action) states (the hard gate), HP
    sequence, the largest distance, the first frame more than 1 mm apart; the enemies whose draw group the console hid
    are listed (the Lab runs the same draw groups since Polaris 1.7c-4)"""
    by = lambda rows: {}
    lab, ps2 = {}, {}
    lab_rows = [r for r in lab_rows if r]
    ps2_rows = [r for r in ps2_rows if r]
    for r in lab_rows:
        if r["frame"] >= start:
            lab.setdefault(r["entity"], {})[r["frame"]] = r
    for r in ps2_rows:
        if r["frame"] >= start:
            ps2.setdefault(r["entity"], {})[r["frame"]] = r
    seq = lambda d, k: [v for i, v in enumerate([d[f][k] for f in sorted(d)]) if i == 0 or [d[f][k] for f in sorted(d)][i - 1] != v]
    out, ok_states, worst = {}, True, 0.0
    hidden = sorted({e for e, d in ps2.items() for r in d.values() if not r["dg"]})
    for e in sorted(set(lab) | set(ps2)):
        a, b = lab.get(e, {}), ps2.get(e, {})
        frames = sorted(set(a) & set(b))
        dist = [sum((x - y) ** 2 for x, y in zip(a[f]["pos"], b[f]["pos"])) ** 0.5 for f in frames]
        same = seq(a, "state") == seq(b, "state") and seq(a, "hp") == seq(b, "hp") and set(a) == set(b)
        ok_states &= same
        worst = max([worst] + dist)
        out[e] = {"frames": len(frames), "only_lab": len(set(a) - set(b)), "only_console": len(set(b) - set(a)),
                  "same_states": same, "max_cm": round(100 * max(dist), 2) if dist else None,
                  "first_over_1mm": next((f for f, d in zip(frames, dist) if d > 0.001), None),
                  "states": len(seq(b, "state"))}
        if not same:
            sa, sb = seq(a, "state"), seq(b, "state")
            k = next((i for i, (x, y) in enumerate(zip(sa, sb)) if x != y), min(len(sa), len(sb)))
            out[e]["first_state_difference"] = {"lab": sa[k] if k < len(sa) else None, "console": sb[k] if k < len(sb) else None}
    return {"enemies": len(out), "same_states": ok_states, "max_cm": round(100 * worst, 2),
            "within_bound": worst * 100 <= bound_cm, "hidden_groups_on_console": hidden,
            "pass": ok_states, "per_enemy": out}


def compare(lab_states, ps2_states, short, end_cm, start=0):
    ps2 = {s["frame"]: s for s in ps2_states}
    pairs = [(s, ps2[s["frame"]]) for s in lab_states if s["frame"] in ps2 and s["frame"] >= start]
    dist = [sum((a - b) ** 2 for a, b in zip(x["pos"], y["pos"])) ** 0.5 for x, y in pairs]
    first_mm = next((x["frame"] for (x, _), d in zip(pairs, dist) if d > 0.001), None)
    seq = lambda ss: [(s["frame"], s["state"]) for i, s in enumerate(ss) if i == 0 or ss[i - 1]["state"] != s["state"]]
    la, pa = seq([x for x, _ in pairs]), seq([y for _, y in pairs])
    same_states = [s for _, s in la] == [s for _, s in pa]
    within_frame = same_states and all(abs(a[0] - b[0]) <= 1 for a, b in zip(la, pa))
    rep = {"frames": len(pairs), "max_cm": round(100 * max(dist), 2) if dist else None,
           "end_cm": round(100 * dist[-1], 2) if dist else None, "first_frame_over_1mm": first_mm,
           "state_changes": len(pa), "same_states": same_states, "changes_within_a_frame": within_frame}
    if short:
        rep["pass"] = bool(dist) and max(dist) <= 0.01 and same_states
    else:
        rep["pass"] = within_frame and bool(dist) and dist[-1] <= end_cm / 100
    if not same_states:
        k = next(i for i, (a, b) in enumerate(zip(la, pa)) if a[1] != b[1]) if any(a[1] != b[1] for a, b in zip(la, pa)) \
            else min(len(la), len(pa))
        rep["first_state_difference"] = {"lab": la[k] if k < len(la) else None, "ps2": pa[k] if k < len(pa) else None}
    return rep


PRESENTATION = {"shot": "screenshot / GS dump", "stats": "frame-time segment", "loadscreen": "loading screen"}
# "ev expect" / "pickup expect" kinds that test what is drawn, heard or shown (map chunks, effects, the boss bar, the
# treasure shines, the item dialog): the Lab has none of that, so they are listed as not run, never passed or failed
PRESENTATION_EXPECT = {"drawn", "sfx", "bossbar", "shines", "dialog"}


def group_errors(errors):
    """the same message on many lines once, with its line numbers"""
    import re
    seen = {}
    for e in errors:
        m = re.match(r"TEST error line (\d+): (.*)", e)
        key = m.group(2) if m else e
        seen.setdefault(key, []).append(m.group(1) if m else None)
    out = []
    for k, ls in seen.items():
        ls = [l for l in ls if l]
        out.append(k if not ls else f"line {ls[0]}: {k}" if len(ls) == 1 else f"lines {', '.join(ls[:6])}"
                   f"{' ...' if len(ls) > 6 else ''} ({len(ls)}x): {k}")
    return out


def run_script(build, area, script, max_frames=None, state_log=None, end_cm=1.0):
    """one script in a fresh Lab: the report dict (pass, checks, errors, presentation, stubs, end); with state_log
    (the console run's option record .state log) also the state sequence and the end position against it"""
    build = os.path.abspath(os.path.join(ROOT, build))
    args = [os.path.join(build, "host", "lab"), build, area, os.path.abspath(script), str(max_frames or 0)]
    if state_log:
        args.append("states")
    try:
        p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)
        out, rc = p.stdout, p.returncode
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        out, rc = out + "\nerror lab: no end after 600 s\n", 1
    lines = out.splitlines()
    rep = {"script": os.path.relpath(os.path.abspath(script), os.getcwd()), "checks": [], "errors": [],
           "presentation": {}, "readback": [], "stubs": [], "end": None, "done": False, "log": lines}
    for s in lines:
        if s.startswith("TEST ") and " expect " in s and (s.endswith((" PASS", " FAIL")) or " PASS (" in s or
                                                         " FAIL (" in s):
            kind = s.split(" expect ", 1)[1].split()[0]
            what = s[len("TEST "):]
            if kind in PRESENTATION_EXPECT:
                rep.setdefault("not_run", []).append(what)
            elif " FAIL" in s:
                rep["errors"].append(s)
            else:
                rep["checks"].append(what)
        elif s.startswith("TEST check ") and s.endswith(" ok"):
            rep["checks"].append(s[len("TEST check "):-len(" ok")])
        elif s.startswith("TEST ") and "check " in s and " ok" in s and "error" not in s:
            rep["checks"].append(s[len("TEST "):])
        elif s.startswith("TEST error") or s.startswith("error "):
            rep["errors"].append(s)
        elif s.startswith("world: warp to ") and " settled in " in s and int(s.split(" settled in ")[1].split()[0]) >= 30000:
            rep["errors"].append(s + "  (world.c's settle ran into its 30 s cap: the console's loading screen waits 30 s)")
        elif s.startswith(("world: settle stopped", "world: block ")) and ("no room" in s or "did not load" in s):
            rep.setdefault("findings", []).append(s)  # (the console's own world rule: shown, not a failure)
        elif s.startswith("TEST DONE"):
            rep["done"] = True
        elif s.startswith("lab frames "):
            rep["end"] = s[len("lab "):]
        elif s.startswith("stub "):
            kind, n, name = s.split(" ", 3)[1:]
            (rep["readback"] if kind == "readback" else rep["stubs"]).append(f"{name} x{n}")
    if not os.path.exists(script):
        rep["errors"].append(f"no script {script}")
    else:
        with open(script) as f:
            for line in f:
                w = line.split("#")[0].split()
                if not w:
                    continue
                if w[0] in PRESENTATION:
                    rep["presentation"][w[0]] = rep["presentation"].get(w[0], 0) + 1
                if w[:2] == ["option", "noenemies"]:
                    rep["noenemies"] = True
    rep["pass"] = rc == 0 and rep["done"] and not rep["errors"] and not rep["readback"]
    if state_log:
        lab = [parse_state(s) for s in lines if s.startswith("state ")]
        ps2 = [parse_state(s) for s in open(state_log) if s.strip()]
        seq = lambda ss: [s["state"] for i, s in enumerate(ss) if i == 0 or ss[i - 1]["state"] != s["state"]]
        d = sum((a - b) ** 2 for a, b in zip(lab[-1]["pos"], ps2[-1]["pos"])) ** 0.5 if lab and ps2 else None
        same = seq(lab) == seq(ps2)
        rep["console"] = {"same_states": same, "state_changes": len(seq(ps2)) - 1,
                          "end_cm": round(100 * d, 3) if d is not None else None,
                          "frames": {"lab": len(lab), "console": len(ps2)}}
        rep["pass"] = rep["pass"] and same and d is not None and d * 100 <= end_cm
    return rep


def needs(script):
    """the scripts a script runs after ("# needs: other.txt" lines: a save pair's second half needs its first)"""
    out = []
    try:
        for line in open(script):
            if line.startswith("# needs:"):
                out += [os.path.join(os.path.dirname(script), n) for n in line[len("# needs:"):].split()]
    except OSError:
        pass
    return out


def jobs(scripts):
    """the scripts as jobs that run in order: a script with "# needs:" lines runs right after what it needs (pulled in
    as a prerequisite when not listed), in one job, so no runner reports an ordering accident as a failure"""
    norm = lambda p: os.path.realpath(p)
    listed = {norm(s) for s in scripts}
    taken, out = set(), []

    def chain(s, seen=()):
        pre = []
        for n in needs(s):
            if norm(n) not in seen:
                pre += chain(n, seen + (norm(s),))
        return pre + [s]

    needed = {norm(n) for s in scripts for n in needs(s)}
    for s in scripts:
        if norm(s) in needed and norm(s) in listed:
            continue  # (runs inside the job of the script that needs it)
        job = [j for j in chain(s) if norm(j) not in taken]
        taken |= {norm(j) for j in job}
        out.append(job)
    for s in scripts:  # a needed script whose dependent was not run: on its own
        if norm(s) not in taken:
            out.append([s]), taken.add(norm(s))
    return out


def print_run(rep, verbose):
    print(f"{'PASS' if rep['pass'] else 'FAIL'} {rep['script']}")
    print(f"  checks run: {len(rep['checks'])}" + ("" if rep["checks"] else
          "  (none: the script passes by running to its end)"))
    for c in rep["checks"]:
        print(f"    {c}")
    if rep.get("not_run"):
        print(f"  checks not run (presentation, nothing drawn in the Lab): {len(rep['not_run'])}")
        for c in rep["not_run"]:
            print(f"    {c}")
    if rep["presentation"]:
        print("  presentation only in the Lab: " + ", ".join(f"{k} x{v}" for k, v in rep["presentation"].items()))
    for e in group_errors(rep["errors"]):
        print(f"  error: {e}")
    for f in rep.get("findings", []):
        print(f"  finding (world.c, as on the console): {f}")
    for r in rep["readback"]:
        print(f"  readback stub (the simulation depends on it): {r}")
    if rep["stubs"]:
        print("  presentation stubs: " + ", ".join(rep["stubs"]))
    if not rep["done"]:
        print("  error: the script did not reach its end")
    if rep["end"]:
        print(f"  end: {rep['end']}")
    if "console" in rep:
        print(f"  against the console run: {rep['console']}")
    if verbose:
        for s in rep["log"]:
            print(f"  | {s}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compare")
    c.add_argument("trace")
    c.add_argument("state_log")
    c.add_argument("--area", default="m10_02_00_00")
    c.add_argument("--build", default="build")
    c.add_argument("--short", action="store_true", help="the short-trace gate: 1 cm every frame, same states")
    c.add_argument("--end-cm", type=float, default=50.0, help="long traces: the end position bound")
    c.add_argument("--enemy-cm", type=float, default=1.0, help="version 3 traces with enemies: the target position bound "
                   "(reported; the gate is the state sequence)")
    c.add_argument("-v", action="store_true", help="every enemy's comparison")
    r = sub.add_parser("run")
    r.add_argument("scripts", nargs="+")
    r.add_argument("--area", default="m10_02_00_00")
    r.add_argument("--build", default="build")
    r.add_argument("--max-frames", type=int)
    r.add_argument("--state", help="one script: the console run's .state log (option record); the state sequence must"
                   " match and the end position be within --end-cm")
    r.add_argument("--end-cm", type=float, default=1.0)
    r.add_argument("-v", action="store_true", help="the whole Lab output per script")
    r.add_argument("-j", type=int, default=1, help="jobs run at once (a script and what it needs: one job)")
    a = ap.parse_args()
    if a.cmd == "run":
        import shutil
        shutil.rmtree(os.path.join(ROOT, a.build, "host", "save"), ignore_errors=True)
        fails = n = 0
        from concurrent.futures import ThreadPoolExecutor

        def run_job(job):  # (a Lab per script: independent processes; a job's scripts in order)
            return [run_script(a.build, a.area, sc, a.max_frames, a.state, a.end_cm) for sc in job]
        with ThreadPoolExecutor(max(1, a.j)) as ex:
            for reps in ex.map(run_job, jobs(a.scripts)):
                for rep in reps:
                    print_run(rep, a.v)
                    fails += not rep["pass"]
                    n += 1
        print(f"lab run: {n - fails} passed, {fails} failed")
        return 1 if fails else 0
    noen = trace_header(a.trace)[3] & 1
    rows = []
    with Lab(a.build, a.area, noenemies=noen) as lab:
        got = lab.replay(a.trace, rows)
        lab_dg = lab.dg
        stubs = lab.stubs()
    ps2 = [parse_state(s) for s in open(a.state_log) if s.strip()]
    start = trace_start(a.trace)
    rep = compare(got, ps2, a.short, a.end_cm, start)
    rep["from_frame"] = start
    rep["held_frames"] = trace_held(a.trace)  # (the console's disc waits in the reference: many = a poor reference)
    rep["stubs"] = stubs
    if trace_header(a.trace)[0] >= 3:  # the draw-group state per frame (record word 21) against the Lab's
        import struct
        _, hb, rb, _ = trace_header(a.trace)
        raw = open(a.trace, "rb").read()
        rec_dg = {struct.unpack_from("<I", raw, hb + rb * i)[0]: struct.unpack_from("<I", raw, hb + rb * i + 84)[0]
                  for i in range((len(raw) - hb) // rb)}
        fr = sorted(f for f in rec_dg if f in lab_dg and f >= start) if any(rec_dg.values()) else []  # (0: recorded
        # before the hash existed)
        bad = [f for f in fr if rec_dg[f] != lab_dg[f]]
        rep["dg_frames"] = len(fr)
        rep["dg_first_difference"] = bad[0] if bad else None
        rep["pass"] = rep["pass"] and not bad
    ef = a.trace + ".enemies"
    if not noen and os.path.exists(ef):
        er = compare_enemies(rows, [parse_enemy(s) for s in open(ef) if s.strip()], trace_first_input(a.trace),
                             a.enemy_cm)
        per = er.pop("per_enemy")
        rep["enemy_gate"] = er  # the hard gate: the same state sequence for every enemy (positions: the target bound)
        rep["pass"] = rep["pass"] and er["pass"]
        if a.v:
            for e, d in per.items():
                print(f"enemy {e}: {d}")
        else:
            rep["enemies_differing"] = {e: d for e, d in per.items() if not d["same_states"]}
    for k, v in rep.items():
        print(f"{k}: {v}")
    return 0 if rep["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
