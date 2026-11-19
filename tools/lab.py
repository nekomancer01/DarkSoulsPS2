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

The commands are lab/lab_main.c's (test-script names where one exists)."""
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
    a = ap.parse_args()
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
