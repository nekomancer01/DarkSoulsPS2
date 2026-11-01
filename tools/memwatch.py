#!/usr/bin/env python3
"""Host memory watchdog for the DSR PS2 work (runs on the host as a systemd user service: memwatch.service).

Why: on 2026-10-31 systemd-oomd killed the whole Claude desktop scope twice (11:27, 11:35; RAM + swap > 90 %),
taking every session down with it. oomd picks the biggest cgroup, so one runaway job inside a session kills them
all. This watchdog acts earlier and kills only disposable job processes:
  - any disposable process whose RSS + swap exceeds PROC_CAP (runaway job), at once;
  - when the system is low (MemAvailable < LOW_AVAIL and swap use > LOW_SWAP_PCT), the largest disposable process,
    one per POLL interval, until the system recovers.
Disposable = command line matches DISPOSABLE and none of PROTECT. The user's apps (Claude, browser, RuneLite,
Steam, desktop) and the user's visible PCSX2 are never touched; headless test PCSX2 instances are (their flatpak
instance ids are marked in /tmp/pcsx2-headless by tools/pcsx2-run.sh).
Every action is logged to LOG with the top processes at that moment, and shown as a desktop notification.
Usage: memwatch.py [--dry-run] [--once]
"""
import os, re, signal, subprocess, sys, time

GiB = 1 << 30
POLL = 2.0
PROC_CAP = 8 * GiB          # one job process above this (RSS + swap) is a runaway
MIN_KILL = 512 << 20        # system-low kills skip processes smaller than this (wrapper shells free nothing)
LOW_AVAIL = 4 * GiB         # system-low trigger: MemAvailable below this ...
LOW_SWAP_PCT = 50           # ... and swap more than this % used (oomd kills at 90 % RAM + swap)
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
LOG = os.path.join(ROOT, "notes", "memwatch.log")

# Work trees whose processes are disposable, matched by folder name so the same name works on the host and inside
# a distrobox (different home paths): this checkout's folder, plus DSR_MEMWATCH_TREES (colon-separated folder names,
# e.g. an older checkout that still runs jobs while work moves over).
TREES = [t for t in [os.path.basename(ROOT)] + os.environ.get("DSR_MEMWATCH_TREES", "").split(":") if t] + ["dsr-ref"]
TREE_RE = "|".join(re.escape(t) for t in dict.fromkeys(TREES))
DISPOSABLE = re.compile(
    r"(?<![\w.-])(?:" + TREE_RE + r")/|/osrs-wiki-backup/|analyzeHeadless|ghidra|pcsx2-gsrunner|"
    r"mips64r5900el-ps2-elf-|cc1 |ld\b.*proto\.elf|DarkSoulsRemastered\.exe|wine(server|64|-preloader)|"
    r"gamescope --backend headless|Xvfb :99|net\.pcsx2\.PCSX2|pcsx2-qt")
JOB_CWD = re.compile(r"/(?:" + TREE_RE + r")(/|$)")
PROTECT = re.compile(
    r"claude-desktop|/claude-code/|/claude |memwatch\.py|firefox|RuneLite|Jagex|steamwebhelper|steamrt64/steam|"
    r"plasmashell|kwin|discord|Discord|systemd|^/usr/bin/(kate|kwrite|code|codium|nvim|vim|gedit|dolphin|konsole)\b|progress\.sh")


def meminfo():
    m = {}
    for line in open("/proc/meminfo"):
        k, v = line.split(":")
        m[k] = int(v.split()[0]) * 1024
    return m


def headless_pcsx2_pids():
    """PIDs of the flatpak PCSX2 instances started headless by pcsx2-run.sh (never the user's visible one)."""
    ids = set(os.listdir("/tmp/pcsx2-headless")) if os.path.isdir("/tmp/pcsx2-headless") else set()
    pids = set()
    for inst in ids:
        try:
            pid = open(f"/run/user/{os.getuid()}/.flatpak/{inst}/pid").read().strip()
            pids.add(int(pid))
        except (OSError, ValueError):
            pass
    return pids


def procs():
    out = []
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            cmd = open(f"/proc/{d}/cmdline", "rb").read().replace(b"\0", b" ").decode(errors="replace").strip()
            st = open(f"/proc/{d}/status").read()
        except OSError:
            continue
        if not cmd:
            continue
        rss = int(re.search(r"VmRSS:\s+(\d+)", st).group(1)) * 1024 if "VmRSS" in st else 0
        swp = int(re.search(r"VmSwap:\s+(\d+)", st).group(1)) * 1024 if "VmSwap" in st else 0
        ppid = int(re.search(r"PPid:\s+(\d+)", st).group(1))
        try:
            cwd = os.readlink(f"/proc/{d}/cwd")
        except OSError:
            cwd = ""
        out.append(dict(pid=int(d), ppid=ppid, cmd=cmd, cwd=cwd, mem=rss + swp))
    return out


def descendants(ps, root):
    kids = {}
    for p in ps:
        kids.setdefault(p["ppid"], []).append(p["pid"])
    stack, seen = [root], set()
    while stack:
        q = stack.pop()
        if q not in seen:
            seen.add(q)
            stack += kids.get(q, [])
    return seen


def disposable(ps):
    """Disposable processes; a headless PCSX2 instance counts with its whole process tree."""
    hl = set()
    for root in headless_pcsx2_pids():
        hl |= descendants(ps, root)
    res = []
    for p in ps:
        c = p["cmd"]
        if PROTECT.search(c):
            continue
        if "PCSX2" in c or "pcsx2" in c:
            if p["pid"] in hl:
                res.append(p)
            continue  # the user's visible PCSX2
        # 2026-10-31 11:43: export_map.py ran as "python -I ../../tools/export_map.py" (cwd ps2/proto) and grew to
        # 24 GB unseen, so a job is also any process whose working directory is inside the project
        if DISPOSABLE.search(c) or JOB_CWD.search(p["cwd"]):
            res.append(p)
    return res


def log(msg, ps=None):
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    lines = [f"{stamp} {msg}"]
    if ps:
        for p in sorted(ps, key=lambda p: -p["mem"])[:8]:
            lines.append(f"    {p['mem'] / GiB:5.1f}G {p['pid']:>7} {p['cmd'][:140]}")
    with open(LOG, "a") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines), flush=True)


def notify(msg):
    subprocess.run(["notify-send", "-a", "memwatch", "-u", "critical", "Memory watchdog", msg],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def kill(p, why, ps, dry):
    log(f"{'WOULD KILL' if dry else 'KILL'} {p['pid']} ({p['mem'] / GiB:.1f}G): {why}: {p['cmd'][:160]}", ps)
    if dry:
        return
    try:
        os.kill(p["pid"], signal.SIGTERM)
        time.sleep(1.5)
        if os.path.exists(f"/proc/{p['pid']}"):
            os.kill(p["pid"], signal.SIGKILL)
    except ProcessLookupError:
        pass
    notify(f"Killed {p['cmd'][:80]} ({p['mem'] / GiB:.1f} GB): {why}")


def main():
    dry = "--dry-run" in sys.argv
    log(f"memwatch start (cap {PROC_CAP / GiB:.0f}G/process, low < {LOW_AVAIL / GiB:.0f}G avail + > {LOW_SWAP_PCT}% swap)"
        + (" DRY RUN" if dry else ""))
    while True:
        ps = procs()
        d = disposable(ps)
        for p in d:
            if p["mem"] > PROC_CAP:
                kill(p, f"over the {PROC_CAP / GiB:.0f} GB per-job cap", ps, dry)
        m = meminfo()
        swap_pct = 100 * (m["SwapTotal"] - m["SwapFree"]) / max(m["SwapTotal"], 1)
        big = max((p for p in d if p["mem"] >= MIN_KILL), key=lambda p: p["mem"], default=None)
        if m["MemAvailable"] < LOW_AVAIL and swap_pct > LOW_SWAP_PCT and big:
            kill(big, f"system low: {m['MemAvailable'] / GiB:.1f}G available, swap {swap_pct:.0f}% used", ps, dry)
        if "--once" in sys.argv:
            break
        time.sleep(POLL)


if __name__ == "__main__":
    main()
