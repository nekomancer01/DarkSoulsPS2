#!/usr/bin/env python3
"""Peak memory of a tools/q job: qpeak.py <pid>, started by tools/q in the background before the job.

Every second it sums PSS + swap (what memwatch counts) over the process tree under <pid> (tools/q itself), plus the
headless PCSX2 instances that tree runs: those run on the host through flatpak, outside the tree, and are found by the
PINE slot lock a test driver holds (/tmp/pcsx2-headless/slot.<n>.lock, tools/test-run.py) and the -datapath .../test/<n>
on the PCSX2 command line (tools/pcsx2-run.sh). On SIGTERM (tools/q, after the job) or when <pid> exits it prints the
peak in MB. Stdlib only."""
import os, re, signal, sys, time

ROOT = int(sys.argv[1])
SLOT_RE = re.compile(r"/pcsx2-headless/slot\.(\d+)\.lock$")
peak = 0


def done(*_):
    print(peak)
    sys.exit(0)


signal.signal(signal.SIGTERM, done)


def mem_kb(pid):
    try:
        with open(f"/proc/{pid}/smaps_rollup") as f:
            return sum(int(l.split()[1]) for l in f if l.startswith(("Pss:", "Swap:")))
    except OSError:
        pass
    try:  # the flatpak sandbox hides smaps_rollup from outside: RSS + swap (PCSX2: ~1.0 GB RSS for 0.85 GB PSS)
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
    if slots:
        want = re.compile(r"-datapath \S*/test/(" + "|".join(slots) + r")(?:\s|$)")
        for p in os.listdir("/proc"):
            if not p.isdigit() or int(p) in tree:
                continue
            try:
                with open(f"/proc/{p}/cmdline", "rb") as f:
                    c = f.read().replace(b"\0", b" ").decode(errors="replace")
            except OSError:
                continue
            if c.startswith("pcsx2-qt ") and want.search(c):  # the emulator, not its bwrap / flatpak wrappers
                total += mem_kb(p)
    return total >> 10


while os.path.exists(f"/proc/{ROOT}"):
    peak = max(peak, sample())
    time.sleep(1)
done()
