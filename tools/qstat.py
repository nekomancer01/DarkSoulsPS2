#!/usr/bin/env python3
"""This machine's job-queue state (tools/q) as one JSON object, for tools/qd (picks a machine for a job) and the
dashboard. Stdlib only, reads /proc and /tmp/dsr-q; runs in well under a second.
Fields (shared with tools/jobdash.py's GET /status):
  host, t, cpus, budget_gb, reserved_gb, free_gb, mem_avail_gb, mem_total_gb, swap_pct,
  memwatch: {avail_gb, swap_pct, level: ok|tight|act} or null when memwatch isn't running,
  running: [{pid, res_gb, cap_gb, cmd, tree, build, mem_gb, cpu, since}], waiting: [{pid, cap_gb, cmd, since}]
A machine fits a job of R GB when free_gb >= R, mem_avail_gb >= R and memwatch.level != act (the rule tools/q
admits by, plus memwatch). mem_gb / cpu are null here (one snapshot; the dashboard samples them over time).
Usage: tools/qstat.py [--fits GB]   (--fits: exit 0 when a job of GB fits now, 1 when not)
"""
import json, os, re, socket, sys, time

RES = "/tmp/dsr-q"            # tools/q: <pid>.<reserve GB> per running job
WAIT = "/tmp/dsr-q-wait"      # tools/q: <pid>.<cap GB> per job waiting for budget
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def meminfo():
    m = {}
    with open("/proc/meminfo") as f:
        for line in f:
            k, v = line.split(":", 1)
            m[k] = int(v.split()[0]) * 1024
    return m


def proc(pid):
    """(cmd, cwd, environ, start time) of a live pid, or None"""
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            argv = f.read().rstrip(b"\0").split(b"\0")
        cwd = os.readlink(f"/proc/{pid}/cwd")
        try:
            with open(f"/proc/{pid}/environ", "rb") as f:
                env = dict(e.split(b"=", 1) for e in f.read().split(b"\0") if b"=" in e)
        except OSError:
            env = {}
        since = os.stat(f"/proc/{pid}").st_mtime
    except OSError:
        return None
    return [a.decode(errors="replace") for a in argv], cwd, env, since


def job_cmd(argv):
    """the job's own command: what follows `tools/q [-m GB] [-r GB]` (or the whole line)"""
    for i, a in enumerate(argv):
        if a.endswith("/q") or a == "q":
            rest = argv[i + 1:]
            while len(rest) >= 2 and rest[0] in ("-m", "-r"):
                rest = rest[2:]
            return " ".join(rest)
    return " ".join(argv)


def cap_of(argv, default=10):
    for i, a in enumerate(argv[:-1]):
        if a == "-m":
            try:
                return int(argv[i + 1])
            except ValueError:
                pass
    return default


def entries(d):
    try:
        names = os.listdir(d)
    except OSError:
        return
    for n in names:
        m = re.fullmatch(r"(\d+)\.(\d+)", n)
        if m:
            yield int(m.group(1)), int(m.group(2))


def memwatch(avail, swap_pct):
    """memwatch's view (its thresholds, read from tools/memwatch.py's env overrides) when it is running"""
    running = False
    for p in os.listdir("/proc"):
        if p.isdigit():
            try:
                with open(f"/proc/{p}/cmdline", "rb") as f:
                    if b"memwatch.py" in f.read():
                        running = True
                        break
            except OSError:
                pass
    if not running:
        return None
    low = float(os.environ.get("MEMWATCH_LOW_GB", 2.5))
    low_swap = float(os.environ.get("MEMWATCH_LOW_SWAP", 75))
    if avail < low and swap_pct > low_swap:
        level = "act"
    elif avail < low * 1.5 or swap_pct > low_swap * 0.9:
        level = "tight"
    else:
        level = "ok"
    return {"avail_gb": round(avail, 2), "swap_pct": round(swap_pct, 1), "level": level}


def status():
    gib = 1 << 30
    m = meminfo()
    budget = int(os.environ.get("DSR_Q_BUDGET", 20))
    avail = m["MemAvailable"] / gib
    swap_pct = 100.0 * (m["SwapTotal"] - m["SwapFree"]) / m["SwapTotal"] if m["SwapTotal"] else 0.0
    running, waiting, reserved = [], [], 0
    for pid, gb in entries(RES):
        p = proc(pid)
        if not p:
            continue                       # stale: tools/q removes it on its next pass
        argv, cwd, env, since = p
        reserved += gb
        build = re.search(r"BUILD=(\S+)", " ".join(argv))
        running.append({"pid": pid, "res_gb": gb, "cap_gb": cap_of(argv), "cmd": job_cmd(argv),
                        "tree": cwd, "build": build.group(1) if build else None, "mem_gb": None, "cpu": None,
                        "since": since, "from": env.get(b"DSR_Q_FROM", b"").decode() or None})
    for pid, gb in entries(WAIT):
        p = proc(pid)
        if not p:
            try:
                os.unlink(f"{WAIT}/{pid}.{gb}")   # a q killed while waiting leaves its marker
            except OSError:
                pass
        else:
            waiting.append({"pid": pid, "cap_gb": gb, "cmd": job_cmd(p[0]), "since": p[3],
                            "from": p[2].get(b"DSR_Q_FROM", b"").decode() or None})
    return {"host": socket.gethostname(), "t": time.time(), "cpus": os.cpu_count(), "budget_gb": budget,
            "reserved_gb": reserved, "free_gb": max(0, budget - reserved), "mem_avail_gb": round(avail, 2),
            "mem_total_gb": round(m["MemTotal"] / gib, 2), "swap_pct": round(swap_pct, 1),
            "memwatch": memwatch(avail, swap_pct), "running": running, "waiting": waiting}


def fits(s, gb):
    return s["free_gb"] >= gb and s["mem_avail_gb"] >= gb and (s["memwatch"] or {}).get("level") != "act"


if __name__ == "__main__":
    s = status()
    if len(sys.argv) == 3 and sys.argv[1] == "--fits":
        sys.exit(0 if fits(s, float(sys.argv[2])) else 1)
    json.dump(s, sys.stdout)
    print()
