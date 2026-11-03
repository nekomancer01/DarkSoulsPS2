#!/usr/bin/env python3
"""Host memory watchdog for the DSR PS2 work (runs on the host as a systemd user service: memwatch.service).

Why: on 2026-10-31 systemd-oomd killed the whole Claude desktop scope twice (11:27, 11:35; RAM + swap > 90 %),
taking every session down with it. oomd picks the biggest cgroup, so one runaway job inside a session kills them
all. This watchdog acts earlier and kills only disposable job processes:
  - any disposable process whose memory (PSS + swap: shared pages split between the processes sharing them, RSS
    before 2026-11-01) exceeds PROC_CAP (runaway job), at once;
  - when the system is under real memory pressure, the largest disposable process is paused (SIGSTOP), one at a
    time, and killed if pausing doesn't relieve it; paused jobs are resumed (SIGCONT) one by one once it is calm.
Pressure = time stalled on memory (PSI, /proc/pressure/memory) or swap-ins from the disk swap file, not free or
unused memory: page cache and reclaimable memory count as available (MemAvailable), and zram swap (compressed RAM,
cheap to swap in) doesn't count. Before 2026-11-01 it fired on MemAvailable < 2.5 GB with >= 16 GB swap used, which
was met with 20 GB in zram, 0 on disk and nothing stalling.
Disposable = command line matches DISPOSABLE and none of PROTECT. The user's apps (Claude, browser, RuneLite,
Steam, desktop) and the user's visible PCSX2 are never touched; headless test PCSX2 instances are (their flatpak
instance ids are marked in /tmp/pcsx2-headless by tools/pcsx2-run.sh).
Every action is logged to LOG with the top processes at that moment, and shown as a desktop notification.
The service reloads itself when this file changes (a pull or merge): until 2026-11-02 the service kept running the
code it started with, and the pre-PSI rule above (killed 4 export_map jobs on 11-02 with 20 GB idle in zram) stayed
live for 17 h after its fix was merged. Paused jobs carry over through PAUSED.
Usage: memwatch.py [--dry-run] [--once]
"""
import json, os, re, signal, subprocess, sys, time

GiB = 1 << 30
POLL = 2.0
PROC_CAP = 8 * GiB          # one job process above this (RSS + swap) is a runaway
MIN_KILL = 512 << 20        # system-low kills skip processes smaller than this (wrapper shells free nothing)
# Pressure trigger (env overrides). PSI "full" = share of time every non-idle task was stalled on memory (avg10, %);
# systemd-oomd kills a whole scope at 60 % for 30 s, so these act well before it. Disk swap-in = pswpin minus zram's
# reads, averaged over SWAPIN_WINDOW polls. Values ASSUMED (TECH_DEBT test-6).
PSI_PAUSE = float(os.environ.get("MEMWATCH_PSI_PAUSE", 10))     # pause the largest job at this full avg10 ...
SWAPIN_PAUSE = float(os.environ.get("MEMWATCH_SWAPIN_MB", 20)) * (1 << 20)  # ... or this disk swap-in rate (B/s)
PSI_KILL = float(os.environ.get("MEMWATCH_PSI_KILL", 30))       # kill the largest job at this full avg10
# Resume threshold: at 2 % a desktop session with zram (browser, Steam, launchers) can sit at 2-5 % full avg10 for
# hours with no job running, so paused jobs stayed frozen 20-30+ min holding their queue reservations. 5 % is still
# half of PSI_PAUSE, and a job that pushes pressure back up is simply paused again.
PSI_CALM = float(os.environ.get("MEMWATCH_PSI_CALM", 5))        # resume paused jobs below this (and no disk swap-in)
# hard floors that kill whatever PSI says: MemAvailable (already counts page cache and reclaimable slab), and free
# swap (zram + disk) below SWAP_FREE_PCT, since oomd's ManagedOOMSwap kills at 90 % of all swap used. Not "swap
# used": pages that sit in swap untouched cost nothing, and they stay there after the pressure is gone.
LOW_AVAIL = int(float(os.environ.get("MEMWATCH_LOW_GB", 1)) * GiB)
SWAP_FREE_PCT = float(os.environ.get("MEMWATCH_SWAP_FREE_PCT", 20))
SWAPIN_WINDOW = 5           # polls (10 s)
PAUSE_EVERY = 10.0          # s between two pauses (PSI avg10 needs a few seconds to show the effect of one)
KILL_EVERY = 10.0           # s between two pressure kills (the hard floors kill every poll)
RESUME_EVERY = 20.0         # s between two resumes, and of calm before the first
STATE = "/tmp/memwatch.json"  # this poll's view, for tools/qstat.py (and the dashboard)
PAUSED = "/tmp/memwatch-paused"  # <pid> <start time> per job this watchdog paused, so a restart resumes them
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
LOG = os.path.join(ROOT, "notes", "memwatch.log")

# Work trees whose processes are disposable, matched by folder name so the same name works on the host and inside
# a distrobox (different home paths): this checkout's folder, plus DSR_MEMWATCH_TREES (colon-separated folder names,
# e.g. an older checkout that still runs jobs while work moves over). Sibling clones / worktrees named
# <this folder>-<suffix> (e.g. DarkSoulsPS2-s2) count as this checkout.
TREES = [t for t in os.environ.get("DSR_MEMWATCH_TREES", "").split(":") if t] + ["dsr-ref"]
TREE_RE = "|".join([re.escape(os.path.basename(ROOT)) + r"(?:-[\w.-]+)?"] + [re.escape(t) for t in dict.fromkeys(TREES)])
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


def psi():
    """(some avg10, full avg10) of /proc/pressure/memory in %, or (0, 0) without PSI"""
    r = {}
    try:
        for line in open("/proc/pressure/memory"):
            kind, *kv = line.split()
            r[kind] = float(dict(x.split("=") for x in kv)["avg10"])
    except (OSError, KeyError, ValueError):
        pass
    return r.get("some", 0.0), r.get("full", 0.0)


def swap_split():
    """(zram swap used, other swap used, swap-ins so far from non-zram swap in bytes), from /proc/swaps, /proc/vmstat
    and the zram devices' read counts (every zram read is a swap-in of one page)"""
    zram = disk = 0
    for line in open("/proc/swaps").readlines()[1:]:
        f = line.split()
        if "/zram" in f[0]:
            zram += int(f[3]) * 1024
        else:
            disk += int(f[3]) * 1024
    pswpin = 0
    for line in open("/proc/vmstat"):
        if line.startswith("pswpin "):
            pswpin = int(line.split()[1])
    zreads = 0
    for dev in os.listdir("/sys/block"):
        if dev.startswith("zram"):
            try:
                zreads += int(open(f"/sys/block/{dev}/stat").read().split()[0])
            except (OSError, ValueError, IndexError):
                pass
    return zram, disk, max(0, pswpin - zreads) * os.sysconf("SC_PAGE_SIZE")


def start_time(pid):
    try:
        return open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()[19]
    except (OSError, IndexError):
        return None


def load_paused():
    """the jobs paused by this watchdog (also by an earlier run of it) that still exist: {pid: start time}"""
    res = {}
    try:
        for line in open(PAUSED):
            pid, st = line.split()
            if start_time(int(pid)) == st:
                res[int(pid)] = st
    except (OSError, ValueError):
        pass
    return res


def save_paused(paused):
    with open(PAUSED + ".tmp", "w") as f:
        f.write("".join(f"{p} {st}\n" for p, st in paused.items()))
    os.replace(PAUSED + ".tmp", PAUSED)


def write_state(st):
    try:
        with open(STATE + ".tmp", "w") as f:
            json.dump(st, f)
        os.replace(STATE + ".tmp", STATE)
    except OSError:
        pass


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


def pss(pid):
    """Proportional set size + swap of a process (/proc/<pid>/smaps_rollup), or None. Unlike RSS, pages shared between
    processes count once in total: the forked workers of one job no longer each count the parent's memory as theirs."""
    try:
        t = open(f"/proc/{pid}/smaps_rollup").read()
    except OSError:
        return None
    m = re.search(r"^Pss:\s+(\d+)", t, re.M)
    if not m:
        return None
    sw = re.search(r"^SwapPss:\s+(\d+)", t, re.M) or re.search(r"^Swap:\s+(\d+)", t, re.M)
    return (int(m.group(1)) + (int(sw.group(1)) if sw else 0)) * 1024


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
    for p in res:  # RSS overcounts shared pages: measure the candidates that matter by PSS (reading smaps costs more)
        if p["mem"] >= MIN_KILL:
            q = pss(p["pid"])
            if q is not None:
                p["mem"] = q
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
        os.kill(p["pid"], signal.SIGCONT)  # a paused job handles SIGTERM only once it runs again
        time.sleep(1.5)
        if os.path.exists(f"/proc/{p['pid']}"):
            os.kill(p["pid"], signal.SIGKILL)
    except ProcessLookupError:
        pass
    notify(f"Killed {p['cmd'][:80]} ({p['mem'] / GiB:.1f} GB): {why}")


def pause(p, why, ps, dry):
    log(f"{'WOULD PAUSE' if dry else 'PAUSE'} {p['pid']} ({p['mem'] / GiB:.1f}G): {why}: {p['cmd'][:160]}", ps)
    if dry:
        return False
    try:
        os.kill(p["pid"], signal.SIGSTOP)
    except ProcessLookupError:
        return False
    notify(f"Paused {p['cmd'][:80]} ({p['mem'] / GiB:.1f} GB): {why}")
    return True


def resume(pid, why, dry):
    log(f"{'WOULD RESUME' if dry else 'RESUME'} {pid}: {why}")
    if not dry:
        try:
            os.kill(pid, signal.SIGCONT)
        except ProcessLookupError:
            pass


def main():
    dry = "--dry-run" in sys.argv
    log(f"memwatch start (cap {PROC_CAP / GiB:.0f}G/process; pause at PSI full >= {PSI_PAUSE:g} % or disk swap-in >= "
        f"{SWAPIN_PAUSE / (1 << 20):g} MB/s, kill at PSI full >= {PSI_KILL:g} %, < {LOW_AVAIL / GiB:g}G available or "
        f"< {SWAP_FREE_PCT:g} % swap free; resume below {PSI_CALM:g} %)" + (" DRY RUN" if dry else ""))
    paused = {} if dry else load_paused()

    def bye(*_):  # never leave a job stopped behind
        for pid in list(paused):
            resume(pid, "memwatch exiting", dry)
        if not dry:
            save_paused({})
        sys.exit(0)
    signal.signal(signal.SIGTERM, bye)
    signal.signal(signal.SIGINT, bye)
    swapins = []
    me = os.path.abspath(__file__)
    my_mtime = os.stat(me).st_mtime_ns
    last_pause = last_kill = 0.0
    calm_since = last_resume = time.time()
    while True:
        now = time.time()
        ps = procs()
        d = disposable(ps)
        alive = {p["pid"] for p in ps}
        paused = {pid: st for pid, st in paused.items() if pid in alive}
        for p in d:
            if p["mem"] > PROC_CAP:
                kill(p, f"over the {PROC_CAP / GiB:.0f} GB per-job cap", ps, dry)
                paused.pop(p["pid"], None)
        m = meminfo()
        some, full = psi()
        zram, disk, sin = swap_split()
        swapins = (swapins + [(now, sin)])[-(SWAPIN_WINDOW + 1):]
        rate = (swapins[-1][1] - swapins[0][1]) / (swapins[-1][0] - swapins[0][0]) if len(swapins) > 1 else 0.0
        big = [p for p in d if p["mem"] >= MIN_KILL]
        running = max((p for p in big if p["pid"] not in paused), key=lambda p: p["mem"], default=None)
        biggest = max(big, key=lambda p: p["mem"], default=None)
        why = (f"PSI full {full:.0f} % (some {some:.0f} %), disk swap-in {rate / (1 << 20):.0f} MB/s, "
               f"{m['MemAvailable'] / GiB:.1f}G available, swap {zram / GiB:.1f}G zram + {disk / GiB:.1f}G disk")
        level = "ok"
        floor = m["MemAvailable"] < LOW_AVAIL or m["SwapFree"] < m["SwapTotal"] * SWAP_FREE_PCT / 100
        if biggest and (floor or (full >= PSI_KILL and now - last_kill >= KILL_EVERY)):
            kill(biggest, ("system low: " if floor else "pausing didn't help: ") + why, ps, dry)
            paused.pop(biggest["pid"], None)
            last_kill = now
            level = "act"
        elif full >= PSI_PAUSE or rate >= SWAPIN_PAUSE:
            level = "act"
            if running and now - last_pause >= PAUSE_EVERY:
                if pause(running, "memory pressure: " + why, ps, dry):
                    paused[running["pid"]] = start_time(running["pid"])
                last_pause = now
        elif full >= PSI_CALM or rate >= SWAPIN_PAUSE / 4 or some >= PSI_PAUSE:
            level = "tight"
        if level != "ok":
            calm_since = now
        elif paused and now - calm_since >= RESUME_EVERY and now - last_resume >= RESUME_EVERY:
            pid = min(paused, key=lambda q: next((p["mem"] for p in d if p["pid"] == q), 0))  # smallest first
            resume(pid, "calm again: " + why, dry)
            paused.pop(pid)
            last_resume = now
        if paused and level == "ok":
            level = "tight"
        if not dry:  # a dry run beside the service leaves its files alone
            save_paused(paused)
            write_state({"t": now, "level": level, "psi_some": some, "psi_full": full,
                         "disk_swapin_mbs": round(rate / (1 << 20), 1), "avail_gb": round(m["MemAvailable"] / GiB, 2),
                         "zram_gb": round(zram / GiB, 2), "disk_swap_gb": round(disk / GiB, 2),
                         "paused": sorted(paused)})
        if "--once" in sys.argv:
            break
        time.sleep(POLL)
        try:
            changed = os.stat(me).st_mtime_ns != my_mtime
            if changed:
                compile(open(me).read(), me, "exec")  # a half-written or broken file: keep running this code
        except (OSError, SyntaxError):
            changed = False
        if changed:
            log("memwatch: code changed on disk, reloading")
            os.execv(sys.executable, [sys.executable, me] + sys.argv[1:])  # paused jobs: saved above, loaded at start


if __name__ == "__main__":
    main()
