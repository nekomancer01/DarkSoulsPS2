#!/usr/bin/env python3
"""Run one test script in a headless PCSX2 and collect its results (needs Pillow + numpy: the Makefile's PY venv).

  test-run.py <elf> <script> <outdir> [--slot N] [--timeout 300] [--gsdump] [--holes] [--name label] [--turbo [x]]
              [--keep-going]

--turbo: PCSX2 without its speed limit (or at x times speed) for functional runs (world_cross, world_soak, routes):
the ELF's own frame stats run on emulated time and stay comparable; the wall-clock column of frames.csv does not.
(A/B 2026-11-02, world_soak.txt: 159 s -> 105 s, same frame count and shot stats.)

<script> is a src/testscript.c script (copied next to the ELF as tests/gen/<name>.txt, since host: only sees the ELF's
folder). The ELF boots with test=<that path> via tools/pcsx2-run.sh (PINE=<slot>, own data dir and Xvfb display). Without
--slot a free slot in 28110-28199 is taken (lock file /tmp/pcsx2-headless/slot.<n>.lock, held while the run lasts), so
parallel runs from several sessions never share a socket, display or data dir. Then, over PINE:
  - every shot marker (mailbox shot_seq): screenshot of the PCSX2 window -> <outdir>/<shot>.png, optional single-frame
    GS dump (F8 hotkey via xdotool) -> <outdir>/<shot>.gs.zst, magenta "hole" pixel count, then shot_ack
  - the mailbox every ~0.1 s -> <outdir>/frames.csv (fps, ms, RAM/VRAM free, chain peaks, ...)
  - a hang (mailbox frame counter stuck 15 s), PCSX2 exiting, or the timeout = failure
  - the EE log, followed while the run lasts: the first error line (LOG_ERRORS) stops the run FAIL_GRACE s later
    (fail fast: a 30 min soak with OUT OF RAM at 2 min used to run to its end); --keep-going runs to the end anyway
  - a progress line every PROGRESS s (elapsed, frame, script line, fps) for long runs
Results: <outdir>/result.json (shots with the ELF's "TEST shot" stats, errors, log lines, "stamp": git revision + hash of
uncommitted changes, ELF / script hashes and times, tools/runstamp.py) and emulog.txt.
Exit code 0 = script ran to its end without errors."""
import argparse, csv, fcntl, glob, json, os, re, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pine, runstamp

TOOLS = os.path.dirname(os.path.abspath(__file__))
HOSTEXEC = os.path.join(TOOLS, "hostexec")  # runs on the host (distrobox-host-exec inside a distrobox)
DISPLAY = ":99"  # set per run from the PINE slot (tools/pcsx2-run.sh: 100 + slot % 100)


def display_for(slot):
    return f":{100 + slot % 100}"


_slot_locks = []


def acquire_slot(slot=None, wait=600):
    """Lock a PINE slot for this process: the given one (waits while another run holds it) or the first free one."""
    os.makedirs("/tmp/pcsx2-headless", exist_ok=True)
    t0 = time.time()
    while True:
        for n in [slot] if slot else range(28110, 28200):
            f = open(f"/tmp/pcsx2-headless/slot.{n}.lock", "w")
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                f.close()
                continue
            _slot_locks.append(f)
            return n
        if time.time() - t0 > wait:
            raise RuntimeError(f"no free PINE slot after {wait} s")
        time.sleep(2)


def x(cmd, timeout=20, **kw):
    """subprocess.run on the run's Xvfb display with a timeout (X tools can block on a vanished window)"""
    try:
        return subprocess.run(cmd, env=dict(os.environ, DISPLAY=DISPLAY), capture_output=True, text=True,
                              timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, -1, "", "timeout")


FAIL_GRACE = 5.0  # s the run goes on after its first error line, so the log holds what followed
PROGRESS = 60.0  # s between progress lines
LOG_ERRORS = re.compile(r"OUT OF RAM|READ ERROR|Not enough VRAM|TEST error|exception|TLB Miss|Unhandled|Bus error"
                        r"|mapstream: alloc .* FAILED|TEST ev expect .*: \S+ FAIL", re.I)
# map.c prints the most cells missing in one frame over each second; more than this is a hole in the world, not a cell
# arriving a frame late (--cells-missing N, 0 = off)
CELLS_MISSING = re.compile(r"map: cells missing (\d+)")
CELLS_MISSING_MAX = 16
# map.c counts near-level swaps seen on screen (pop-in: a cell drawn at its coarse level inside the near radius until
# its near level streamed in); more in the run than --popin-max fails it (-1 = off, the default)
POPIN = re.compile(r"map: near pop-in \d+ cells .*?; (\d+) in the run")
POPIN_MAX = -1
# enemy.c wall_check (test runs): a character's centre crossed a wall; more in the run than --walls-max fails it (-1 = off)
WALLS = re.compile(r"enemy: through wall .*?; (\d+) in the run")
WALLS_MAX = -1


def log_error(line):
    if LOG_ERRORS.search(line):
        return True
    m = CELLS_MISSING_MAX and CELLS_MISSING.search(line)
    if m and int(m[1]) > CELLS_MISSING_MAX:
        return True
    m = WALLS_MAX >= 0 and WALLS.search(line)
    if m and int(m[1]) > WALLS_MAX:
        return True
    m = POPIN_MAX >= 0 and POPIN.search(line)
    return bool(m) and int(m[1]) > POPIN_MAX


def host_home():
    return subprocess.run([HOSTEXEC, "printenv", "HOME"], capture_output=True, text=True,
                          timeout=60).stdout.strip()


def pcsx2_window():
    """Largest top-level window on the Xvfb display (the PCSX2 main window)."""
    out = x(["xwininfo", "-root", "-children"]).stdout
    best = (0, None)
    for w in re.findall(r"^\s+(0x[0-9a-f]+)", out, re.M):
        info = x(["xwininfo", "-id", w]).stdout
        m = re.search(r"Width: (\d+).*?Height: (\d+)", info, re.S)
        if m and int(m[1]) * int(m[2]) > best[0]:
            best = (int(m[1]) * int(m[2]), w)
    return best[1]


def capture(path, tries=3):
    """Screenshot of the PCSX2 window (found again each time: Qt can recreate it); returns the window id or None"""
    for _ in range(tries):
        win = pcsx2_window()
        if win and x(["import", "-window", win, path], timeout=30).returncode == 0:
            return win
        time.sleep(0.5)
    return None


def holes(path):
    """Pixels the map left uncovered: the magenta colour clear of "option holes" (CT16 dither: R,B >= 200, G < 60).
    The PCSX2 window's menu / status bars are grey, the HUD is not magenta."""
    from PIL import Image
    import numpy as np
    a = np.asarray(Image.open(path).convert("RGB")).astype(int)
    m = (a[..., 0] > 200) & (a[..., 2] > 200) & (a[..., 1] < 60)
    return int(m.sum()), int(m.size)


def gs_dump(win, snaps, before, out, wait=20):
    """F8 = GSDumpSingleFrame (tools/test-pcsx2-ini.py); the dump appears in the instance's snaps/ folder."""
    x(["xdotool", "windowfocus", win], timeout=10)  # no --sync: it blocks forever on a window that went away
    x(["xdotool", "key", "--window", win, "F8"], timeout=10)
    t0 = time.time()
    while time.time() - t0 < wait:
        new = [f for f in glob.glob(os.path.join(snaps, "*.gs*")) if f not in before]
        if new:
            f = new[0]
            for _ in range(50):  # written in the background: wait until the size settles
                s = os.path.getsize(f)
                time.sleep(0.2)
                if s and os.path.getsize(f) == s:
                    break
            shutil.copy(f, out)
            before.add(f)
            return True
        time.sleep(0.2)
    return False


def parse_kv(text):
    """'k v k v ...' -> {k: float or str}"""
    toks, d = text.split(), {}
    for k, v in zip(toks[::2], toks[1::2]):
        try:
            d[k] = float(v)
        except ValueError:
            d[k] = v
    return d


def parse_shot_line(line):
    """'TEST shot <name> seq N frames .. fps .. ms_avg ..' -> dict"""
    m = re.search(r"TEST shot (\S+) (.*)", line)
    if not m:
        return None
    toks = m[2].split()
    d = {"name": m[1]}
    i = 0
    while i < len(toks) - 1:
        k = toks[i]
        if k == "pos":
            d["pos"] = [float(t) for t in toks[i + 1:i + 4]]
            i += 4
            continue
        try:
            d[k] = float(toks[i + 1])
        except ValueError:
            d[k] = toks[i + 1]
        i += 2
    return d


def run(a):
    global DISPLAY
    a.slot = acquire_slot(a.slot)
    DISPLAY = display_for(a.slot)
    elf = os.path.realpath(a.elf)
    proto = os.path.dirname(elf)
    name = a.name or os.path.splitext(os.path.basename(a.script))[0]
    os.makedirs(a.outdir, exist_ok=True)
    gen = os.path.join(proto, "tests", "gen")
    os.makedirs(gen, exist_ok=True)
    rel = f"tests/gen/{name}.txt"
    if os.path.realpath(a.script) != os.path.join(proto, rel):
        shutil.copy(a.script, os.path.join(proto, rel))
    hh = host_home()
    data = f"{hh}/.var/app/net.pcsx2.PCSX2/test/{a.slot}"
    log = f"{data}/PCSX2/logs/emulog.txt"  # -datapath <dir> -> <dir>/PCSX2
    snaps = f"{data}/PCSX2/snaps"
    os.makedirs(snaps, exist_ok=True)
    before = set(glob.glob(os.path.join(snaps, "*")))
    if os.path.exists(log):
        os.remove(log)
    env = dict(os.environ, KEEP="1", PINE=str(a.slot), ARGS=f"test={rel}")
    if a.iso:  # boot the disc itself (tools/pcsx2-run.sh BOOTISO): the ISO must sit in the ELF's folder (host: root)
        env["BOOTISO"] = os.path.realpath(a.iso)
    if a.turbo:  # no speed limit (tools/test-pcsx2-ini.py TURBO)
        env["TURBO"] = a.turbo
    out = subprocess.run([os.path.join(TOOLS, "pcsx2-run.sh"), elf, "0", "-"], env=env, capture_output=True, text=True,
                         timeout=120)
    inst = re.search(r"INST=(\S+)", out.stdout)
    res = {"name": name, "elf": elf, "script": a.script, "shots": [], "errors": [], "log": [], "started": time.time(),
           "stamp": runstamp.stamp([elf, a.script]), "turbo": a.turbo or None}  # which tree / ELF / script this result belongs to
    print(f"[test-run] {name}: {runstamp.line(res['stamp'])}", flush=True)
    if not inst:
        res["errors"].append("PCSX2 did not start: " + out.stdout + out.stderr)
        return res
    inst = inst[1]
    try:
        p = pine.connect(a.slot, wait=30)
        syms = pine.elf_symbols(elf)
        addr = None
        t0 = time.time()
        while addr is None and time.time() - t0 < 60:  # stripped ELF: wait for the boot line in the EE log
            try:
                addr = pine.resolve("test_mb", syms, log)[0]
            except KeyError:
                time.sleep(0.5)
        if addr is None:
            raise RuntimeError("test mailbox not found (no symbol, no 'TEST mailbox' log line)")
        win = None
        f = open(os.path.join(a.outdir, "frames.csv"), "w", newline="")
        w = csv.writer(f)
        w.writerow(["t"] + pine.MAILBOX_FIELDS)
        t0 = time.time()
        last_frame, last_change, acked = -1, time.time(), 0
        mb = None
        log_pos, log_next, first_err, next_progress = 0, t0, None, t0 + PROGRESS
        while True:
            now = time.time()
            if now - t0 > a.timeout:
                res["errors"].append(f"timeout after {a.timeout} s (script line {mb and mb['line']})")
                break
            if now >= log_next and not getattr(a, "keep_going", False) and first_err is None:
                log_next = now + 1.0  # the EE log so far: errors end the run early (the scan below lists them)
                try:
                    with open(log, "rb") as lf:
                        lf.seek(log_pos)
                        chunk = lf.read()
                    chunk = chunk[:chunk.rfind(b"\n") + 1]  # whole lines only
                    log_pos += len(chunk)
                    err = next((l for l in chunk.decode(errors="replace").splitlines() if log_error(l)), None)
                    if err:
                        first_err = (now, err.strip())
                        print(f"[test-run] {name}: error at {now - t0:.0f} s, stopping in {FAIL_GRACE:.0f} s "
                              f"(--keep-going to run on): {first_err[1][:160]}", flush=True)
                except OSError:
                    pass
            if first_err and now - first_err[0] > FAIL_GRACE:
                res["errors"].append(f"stopped early at {now - t0:.0f} s after the first error (script line "
                                     f"{mb and mb['line']}; --keep-going runs to the end)")
                break
            if now >= next_progress and mb is not None:
                next_progress = now + PROGRESS
                print(f"[test-run] {name}: {now - t0:.0f} s, frame {mb['frame']}, script line {mb['line']}, "
                      f"{mb.get('fps', 0):.0f} fps", flush=True)
            try:
                mb = pine.read_mailbox(p, addr)
            except (ValueError, IOError) as e:  # no VM yet (PINE IPC_FAIL) / ELF still loading (magic not set)
                if isinstance(e, ConnectionError):
                    raise
                if mb is not None:
                    raise
                last_change = now
                time.sleep(0.2)
                continue
            if mb["frame"] != last_frame:
                w.writerow([f"{now - t0:.2f}"] + [mb[k] for k in pine.MAILBOX_FIELDS])
                last_frame, last_change = mb["frame"], now
            elif now - last_change > (90 if last_frame < 300 else 30):
                res["errors"].append(f"hang: frame {last_frame} for {now - last_change:.0f} s (script line {mb['line']}, PCSX2 {p.status()})")
                if os.environ.get("DSR_HANG_STATE"):  # a savestate of the hang (slot 9) for tools/hangstate.py
                    try:
                        p.save_state(9)
                        time.sleep(5)
                        res["hang_state"] = 9
                    except Exception as e:
                        print(f"[test-run] hang savestate failed: {e}", flush=True)
                break
            if mb["shot_seq"] != acked and mb["state"] == 3:
                pine.ack_shot(p, addr, mb["shot_seq"] | pine.SHOT_BUSY)  # ELF keeps waiting (60 s) while we capture
                png = os.path.join(a.outdir, f"{mb['shot_name']}.png")
                time.sleep(0.15)  # two more frames of the frozen scene reach the window
                win = capture(png)
                shot = {"name": mb["shot_name"], "png": png, "seq": mb["shot_seq"]}
                if not win:
                    res["errors"].append(f"screenshot failed: {mb['shot_name']}")
                    acked = mb["shot_seq"]
                    pine.ack_shot(p, addr, acked)
                    continue
                if a.holes:
                    shot["holes"], shot["pixels"] = holes(png)
                if a.gsdump:
                    shot["gsdump"] = gs_dump(win, snaps, before, os.path.join(a.outdir, f"{mb['shot_name']}.gs.zst")) \
                        and os.path.join(a.outdir, f"{mb['shot_name']}.gs.zst")
                res["shots"].append(shot)
                acked = mb["shot_seq"]
                pine.ack_shot(p, addr, acked)
                last_change = time.time()
            if mb["state"] == 2:
                # the ELF queues log text while a disc read is in flight (pack.c, cdrom0:), so "TEST DONE" can reach
                # the EE log after the mailbox says done: wait for it before PCSX2 is killed (world_death_swap.txt
                # from the ISO lost it, 2026-11-05)
                for _ in range(50):
                    if os.path.exists(log) and b"TEST DONE" in open(log, "rb").read():
                        break
                    time.sleep(0.1)
                break
            time.sleep(0.1)
        f.close()
        res["frames"] = last_frame
    except (ConnectionError, OSError, RuntimeError) as e:
        res["errors"].append(f"{type(e).__name__}: {e}")
    finally:
        time.sleep(0.5)
        subprocess.run([HOSTEXEC, "flatpak", "kill", inst], capture_output=True, timeout=60)
        try:
            os.remove(f"/tmp/pcsx2-headless/{inst}")
        except OSError:
            pass
    time.sleep(0.5)
    if os.path.exists(log):
        shutil.copy(log, os.path.join(a.outdir, "emulog.txt"))
        stats = {}
        for line in open(log, errors="replace"):
            if "TEST " in line:
                res["log"].append(line.rstrip())
                d = parse_shot_line(line)
                if d:
                    stats[d["name"]] = d
                m = re.search(r"TEST passes (\S+) (.*)", line)
                if m:  # budget passes of the shot (testscript.c pb_print; tools/budget.py checks them)
                    stats.setdefault(m[1], {"name": m[1]})["passes"] = parse_kv(m[2])
            if log_error(line):
                res["errors"].append("log: " + line.strip())
        for s in res["shots"]:
            s["stats"] = stats.get(s["name"], {})
        if not any("TEST DONE" in l for l in res["log"]):
            res["errors"].append("script did not reach its end (no TEST DONE)")
        if a.hitch:
            hitch_gates(log, res)
    else:
        res["errors"].append(f"no EE log at {log}")
    res["seconds"] = round(time.time() - res.pop("started"), 1)
    json.dump(res, open(os.path.join(a.outdir, "result.json"), "w"), indent=1)
    return res


# --hitch: gates on the hitch monitor's lines (src/hitch.c, tools/hitchsum.py). Loading screens (warps, deaths) count
# apart. A hitch is a frame over HITCH_MAX_MS while playing; churn is the share of map KB read that had been dropped
# less than ~30 s before (walking back to a place reads it again legitimately); flops are cells in view swapping their level of detail and back within 2 s; drift compares the
# first and the last HITCH_WINDOW s of the run (worse late than early = degradation over time).
HITCH_MAX_MS = 150      # worst frame outside loading screens (after the first 30 s)
HITCH_OVER100 = 3       # frames over 100 ms per 10 min of play
HITCH_THRASH_PCT = 15   # KB read again within ~30 s of being dropped / KB read (src/mapstream.c MS_THRASH_TICKS)
HITCH_FLOPS_MIN = 3.0   # flip-flops per minute
HITCH_WINDOW = 300
HITCH_DRIFT_KB = 512    # lowest free heap / largest arena run may fall at most this much first -> last window


def hitch_gates(log, res):
    import hitchsum
    rows = [r for r in hitchsum.parse(log) if r["t"] > 30]
    if not rows:
        res["errors"].append("hitch: no hitch monitor lines (src/hitch.c)")
        return
    s = hitchsum.summarize(rows, HITCH_WINDOW)
    al, mins = s["all"], max(1.0, sum(r["frames"] for r in rows) / 30 / 60)
    res["hitch"] = s
    if al["max"] > HITCH_MAX_MS:
        w = s["worst"][0]
        res["errors"].append(f"hitch: worst frame {al['max']} ms > {HITCH_MAX_MS} at t {w['t']} s ({w['split']})")
    if al["o100"] > HITCH_OVER100 * mins / 10:
        res["errors"].append(f"hitch: {al['o100']} frames over 100 ms in {mins:.0f} min (gate {HITCH_OVER100} / 10 min)")
    if al["read_kb"] and 100 * al["thrash_kb"] > HITCH_THRASH_PCT * al["read_kb"]:
        res["errors"].append(f"hitch: churn, {al['thrash_kb']} of {al['read_kb']} KB read had been dropped under 30 s "
                             f"before (gate {HITCH_THRASH_PCT} %)")
    if al["flops"] > HITCH_FLOPS_MIN * mins:
        res["errors"].append(f"hitch: {al['flops']} level-of-detail flip-flops in {mins:.0f} min (gate {HITCH_FLOPS_MIN}"
                             " / min)")
    w = s["windows"]
    if len(w) >= 3:
        a, b = w[0], w[-1]
        if b["heap_min"] + HITCH_DRIFT_KB < a["heap_min"] or b["largest_min"] + HITCH_DRIFT_KB < a["largest_min"]:
            res["errors"].append(f"hitch: free heap drifts down over the run ({a['heap_min']} -> {b['heap_min']} KB, "
                                 f"largest block {a['largest_min']} -> {b['largest_min']} KB)")
        if b["max"] > max(2 * a["max"], HITCH_MAX_MS // 2) or b["o50"] > 2 * a["o50"] + 5:
            res["errors"].append(f"hitch: frames get worse over the run (worst {a['max']} -> {b['max']} ms, over 50 ms "
                                 f"{a['o50']} -> {b['o50']} per {HITCH_WINDOW} s)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("script")
    ap.add_argument("outdir")
    ap.add_argument("--slot", type=int, help="PINE slot (default: first free in 28110-28199)")
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--iso", help="boot this disc image as a player does (the ELF only gives symbols and the host: "
                    "folder; build both with the same make, the ISO in the ELF's folder)")
    ap.add_argument("--popin-max", type=int, default=POPIN_MAX,
                    help="fail when more near-level swaps were seen on screen in the run (map.c pop-in; -1 = off)")
    ap.add_argument("--walls-max", type=int, default=WALLS_MAX,
                    help="fail when more enemies went through a wall in the run (enemy.c wall_check; -1 = off)")
    ap.add_argument("--cells-missing", type=int, default=CELLS_MISSING_MAX,
                    help=f"fail when map.c reports more cells missing in a frame (default {CELLS_MISSING_MAX}, 0 = off)")
    ap.add_argument("--hitch", action="store_true", help="gate on the hitch monitor (worst frame, hitches, churn, "
                    "flip-flops, drift over the run; hitch_gates)")
    ap.add_argument("--gsdump", action="store_true")
    ap.add_argument("--holes", action="store_true", help="count magenta pixels (script needs 'option holes')")
    ap.add_argument("--name")
    ap.add_argument("--keep-going", action="store_true",
                    help="run to the end after an error line in the EE log (default: stop FAIL_GRACE s after it)")
    ap.add_argument("--turbo", nargs="?", const="1", default=os.environ.get("TURBO", ""),
                    help="run unthrottled (or at speed x: --turbo 3) for functional tests; not for perf numbers")
    a = ap.parse_args()
    globals()["CELLS_MISSING_MAX"] = a.cells_missing
    globals()["POPIN_MAX"] = a.popin_max
    globals()["WALLS_MAX"] = a.walls_max
    res = run(a)
    print(f"{res['name']}: {len(res['shots'])} shots, {res.get('frames', 0)} frames, {res['seconds']} s, "
          f"{len(res['errors'])} errors")
    for e in res["errors"][:20]:
        print("  ERROR", e)
    sys.exit(1 if res["errors"] else 0)


if __name__ == "__main__":
    main()
