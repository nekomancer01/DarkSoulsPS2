#!/usr/bin/env python3
"""Peak memory of a tools/q job: qpeak.py <pid>, started by tools/q in the background before the job.

Every second it sums PSS + swap (what memwatch counts) over the process tree under <pid> (tools/q itself), plus the
headless test runners that tree starts: tools/pcsx2-run.sh starts pcsx2-dsrtest with setsid, so once the launcher
exits the runner is no longer in the tree; it is found by the PINE slot lock a test driver holds
(/tmp/pcsx2-headless/slot.<n>.lock, tools/test-run.py) and the slot's pid note (/tmp/pcsx2-headless/slot.<n>.pid). The
runner is a plain child process in this container, so its smaps_rollup (PSS) is readable. On SIGTERM (tools/q, after the job) or when <pid> exits it prints the
peak in MB. Stdlib only."""
import os, re, signal, sys, time

ROOT = int(sys.argv[1])
SLOT_RE = re.compile(r"/pcsx2-headless/slot\.(\d+)\.lock$")
peak = 0


def done(*_):
    try:
        os.remove(f"/tmp/dsr-q-peak/{ROOT}.now")
    except OSError:
        pass
    print(peak)
    sys.exit(0)


signal.signal(signal.SIGTERM, done)


def mem_kb(pid):
    try:
        with open(f"/proc/{pid}/smaps_rollup") as f:
            return sum(int(l.split()[1]) for l in f if l.startswith(("Pss:", "Swap:")))
    except OSError:
        pass
    try:  # smaps_rollup unreadable (another user's process): RSS + swap
        with open(f"/proc/{pid}/status") as f:
            return sum(int(l.split()[1]) for l in f if l.startswith(("VmRSS:", "VmSwap:")))
    except OSError:
        return 0


def sample():
    kids, cmd = {}, {}
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            with open(f"/proc/{p}/stat") as f:
                ppid = int(f.read().rsplit(")", 1)[1].split()[1])
            kids.setdefault(ppid, []).append(int(p))
        except (OSError, IndexError, ValueError):
            pass
    tree, todo = [], [ROOT]
    while todo:
        p = todo.pop()
        tree.append(p)
        todo += kids.get(p, [])
    me = os.getpid()
    slots = set()
    for p in tree:
        try:
            for fd in os.listdir(f"/proc/{p}/fd"):
                m = SLOT_RE.search(os.readlink(f"/proc/{p}/fd/{fd}"))
                if m:
                    slots.add(m.group(1))
        except OSError:
            pass
    total = sum(mem_kb(p) for p in tree if p != me)
    for n in slots:
        try:
            pid = int(open(f"/tmp/pcsx2-headless/slot.{n}.pid").read().split()[0])
            if pid not in tree and open(f"/proc/{pid}/comm").read().strip() == "pcsx2-dsrtest":
                total += mem_kb(pid)
        except (OSError, ValueError, IndexError):
            pass
    return total >> 10


# the current value too, for tools/q's admission (what a running job may still claim of its reservation)
NOW = f"/tmp/dsr-q-peak/{ROOT}.now"
while os.path.exists(f"/proc/{ROOT}"):
    cur = sample()
    peak = max(peak, cur)
    try:
        with open(NOW + ".tmp", "w") as f:
            f.write(str(cur))
        os.replace(NOW + ".tmp", NOW)
    except OSError:
        pass
    time.sleep(1)
try:
    os.remove(NOW)
except OSError:
    pass
done()
