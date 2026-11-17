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

  lab.py run SCRIPT... [--area A] [--build B] [--state STATE_LOG [--end-cm 1]] [-j N] [-v]
      runs each test script (tests/*.txt) in the Lab through the game's own interpreter (src/testscript.c) and prints
      per script: pass / fail, the checks that ran (a script whose checks all needed something the Lab lacks shows
      none), the commands that are presentation only there (shot, stats, loadscreen), the checks not run there (what is
      drawn: ev expect drawn / sfx / bossbar, pickup expect shines / dialog), the failures (TEST error lines, FAIL
      expects, a warp whose settle ran into world.c's 30 s cap, a stub whose result the simulation reads back) and the
      presentation stubs reached. Burg walks: the console's world holds the player while a tile loads, which a script
      run in the Lab never does; compare those with the recorded trace (compare replays the holds). Saves go to build/host/save (never the console tests' build/save), emptied once per
      invocation: `run tests/save_write.txt tests/save_check.txt` is the save round trip (tests/save_roundtrip.sh).
      --state: the script run against the console's own run of it (its option record .state log): the same state
      sequence and the end position within --end-cm (tests/det_walk_open.txt: 0.04 cm). Frame counts differ (the
      console's boot frames), so the sequence is compared without frame numbers.
      Replays: frames where the console's world held the player (trace flag 4) are held in the Lab too.

The commands are lab/lab_main.c's (test-script names where one exists). The Lab is a 32-bit host program: on the
distrobox run lab.py through `distrobox-host-exec` (the container has no 32-bit loader)."""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Lab:
    def __init__(self, build="build", area="m10_02_00_00", binary=None):
        build = os.path.abspath(os.path.join(ROOT, build))
        binary = binary or os.path.join(build, "host", "lab")
        self.p = subprocess.Popen([binary, build, area], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                  bufsize=1)

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

    def replay(self, trace):
        return [parse_state(s) for s in self.cmd("replay " + os.path.abspath(trace)) if s.startswith("state ")]

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


def trace_start(trace):
    """the frame the comparison starts at: the last teleport (a test spawn, a warp: record flag 1) before the first
    frame with input, so the boot's own spot (in another block than the Lab loaded) is left out"""
    import struct
    b = open(trace, "rb").read()
    start, n = 0, (len(b) - 20) // 64
    for i in range(n):
        u = struct.unpack_from("<16I", b, 20 + 64 * i)
        f = struct.unpack_from("<16f", b, 20 + 64 * i)
        if u[10] & 1:
            start = u[0]
        if any(abs(x) > 1e-6 for x in f[2:6]) or u[6] or u[7]:
            break
    return start


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
    if not rep.get("noenemies"):
        print("  note: the console runs this script with enemies; the Lab has none yet (Polaris 1.7c-3)")
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
    r = sub.add_parser("run")
    r.add_argument("scripts", nargs="+")
    r.add_argument("--area", default="m10_02_00_00")
    r.add_argument("--build", default="build")
    r.add_argument("--max-frames", type=int)
    r.add_argument("--state", help="one script: the console run's .state log (option record); the state sequence must"
                   " match and the end position be within --end-cm")
    r.add_argument("--end-cm", type=float, default=1.0)
    r.add_argument("-v", action="store_true", help="the whole Lab output per script")
    r.add_argument("-j", type=int, default=1, help="scripts run at once (save scripts share build/host/save: keep 1)")
    a = ap.parse_args()
    if a.cmd == "run":
        import shutil
        shutil.rmtree(os.path.join(ROOT, a.build, "host", "save"), ignore_errors=True)
        fails = 0
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max(1, a.j)) as ex:  # (a Lab per script: independent processes)
            reps = ex.map(lambda sc: run_script(a.build, a.area, sc, a.max_frames, a.state, a.end_cm), a.scripts)
            for rep in reps:
                print_run(rep, a.v)
                fails += not rep["pass"]
        print(f"lab run: {len(a.scripts) - fails} passed, {fails} failed")
        return 1 if fails else 0
    with Lab(a.build, a.area) as lab:
        got = lab.replay(a.trace)
        stubs = lab.stubs()
    ps2 = [parse_state(s) for s in open(a.state_log) if s.strip()]
    start = trace_start(a.trace)
    rep = compare(got, ps2, a.short, a.end_cm, start)
    rep["from_frame"] = start
    rep["stubs"] = stubs
    for k, v in rep.items():
        print(f"{k}: {v}")
    return 0 if rep["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
