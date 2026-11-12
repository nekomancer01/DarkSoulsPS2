#!/usr/bin/env python3
"""Terminal progress view for a long build: runs a command and shows, from top to bottom, a progress bar, the current
step and the command's latest output line (highlighted), redrawn in place. Every output line still goes to the log
file. ./build-iso.sh runs itself under this when its output is a terminal (--plain or DSR_TUI=0 for the plain log).

Usage: tools/buildtui.py [--log FILE] [--title TEXT] -- COMMAND...
       make -n -k --trace ... 2>&1 | tools/buildtui.py --plan TOOLS_DIR   (prints the "##tui plan" numbers)

The command talks to the view through its own output (stdout and stderr, merged); these lines are not shown:
  == TEXT                 a new phase (kept on screen above the view, and the step name's prefix)
  ##tui plan N S[+]       N targets to build, S their expected seconds (+: at least, the dry run stopped early)
  ##tui step TEXT         the current step's name, until the next target starts
  ##tui keep TEXT         a line to keep on screen above the view (a result, a hint)
  FILE:LINE: update target 'T' due to: ...   (GNU make --trace) target T started: counts towards the plan and
                          names the step; the recipe line that follows names the tool that builds it
Lines that look like errors (make's "***", "error:", Traceback, "build-iso:") are kept on screen too. When the
command fails the view ends with the tail of the log. Stdlib only, plain ANSI escapes (no curses), so it runs with
the system python3 before any venv exists.
"""
import os
import re
import select
import shutil
import signal
import subprocess
import sys
import time

TRACE = re.compile(r"^\S+:\d+: (?:update )?target '(.+)' due to: ")
TRACE_OTHER = re.compile(r"^\S+:\d+: target '.+' does not exist$")
TOOL = re.compile(r"(?:^|[\s/])((?!q\b)[\w.-]+\.(?:py|sh))\b")
STAMP = re.compile(r"/stamp/(.+)\.sum$")
KEEP = re.compile(r"\*\*\*|\berror:|^Traceback|^build-iso:|^\[q\] (?:killed|failed)", re.I)
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07]*\x07|[\x00-\x08\x0b-\x1f\x7f]")
COMPILERS = (("gcc", "compile"), ("g++", "compile"), ("-ld", "link"), ("ar ", "archive"), ("xorriso", "write ISO"),
             ("dvp-as", "assemble VU"), ("bin2c", "embed"))
# Expected seconds per target by the tool that builds it: median run times of these jobs in notes/jobq.log
# (2026-11-10, 46-3063 runs each). Anything else (compile, pack, small exports) counts 1 s.
SECONDS = {"horizon.py": 47, "export_audio.py": 28, "export_map.py": 27, "export_items.py": 27, "world_budget.py": 11,
           "export_model.py": 8, "export_objects.py": 7, "export_menu.py": 6, "export_screens.py": 5,
           "export_collision.py": 5, "export_chr.py": 4, "export_loading.py": 4, "export_world.py": 3,
           "export_emevd.py": 2, "ps2-iso.sh": 5}
SPIN = "|/-\\"
FPS = 10
# xterm-256 colours, soft on dark and light themes alike: the bar teal (sage when done, rose on a failure), its empty
# part a faint rule, the step lavender, the current line warm sand on a slightly raised background, phase headers
# steel blue, kept errors rose
C_BAR, C_DONE, C_FAIL, C_EMPTY = "38;5;80", "38;5;114", "38;5;174", "38;5;240"
C_STEP, C_LINE, C_PHASE, C_ERR, C_DIM = "38;5;183", "38;5;223;48;5;236", "1;38;5;110", "38;5;174", "38;5;246"
PARTS = " ▏▎▍▌▋▊▉"  # eighths of a cell, for a smooth bar edge


def col(code, text):
    return f"\x1b[{code}m{text}\x1b[0m"


def tool_of(s):
    """Short name of the tool a recipe line runs, or None if the line is not a recipe we recognise."""
    m = TOOL.search(s)
    if m:
        return m.group(1)
    first = s.split(" ", 1)[0]
    for k, v in COMPILERS:
        if k in first or (k == "xorriso" and k in s):
            return v
    return "make" if first.endswith("make") else None


class View:
    def __init__(self, out, title, root):
        self.out = out
        self.root = root.rstrip("/") + "/" if root else ""
        self.phase = title
        self.label = "starting"
        self.line = ""
        self.plan_n = self.plan_w = 0
        self.plan_min = False
        self.seen = set()  # every target make traced, FORCE'd checks included
        self.work = {}  # target -> [start time, expected seconds] for real work (not FORCE'd checks)
        self.pending = None  # target whose recipe line, next, names its tool
        self.t0 = time.monotonic()
        self.drawn = 0  # lines of the view currently on screen
        self.last = 0.0
        self.done = None  # None while running, else the exit code

    # --- input ---
    def short(self, s):
        return s.replace(self.root, "") if self.root else s

    def feed(self, raw):
        s = ANSI.sub("", raw.expandtabs(4)).rstrip()
        if not s:
            return
        if s.startswith("== "):
            # a new phase: what make started before it has finished, whatever its tool usually takes
            for item in self.work.values():
                item[0] = float("-inf")
            self.phase = re.sub(r" into \S.*$", "", self.short(s[3:].strip()))  # the step line names the target
            self.label = "…"
            self.keep(col(C_PHASE, self.short(s)))
            return
        if s.startswith("##tui "):
            cmd, _, arg = s[6:].partition(" ")
            if cmd == "plan":
                self.plan_min = arg.endswith("+")
                try:
                    self.plan_n, self.plan_w = (int(x) for x in arg.rstrip("+").split())
                except ValueError:
                    pass
            elif cmd == "step":
                self.label = arg
            elif cmd == "keep":
                self.keep(arg)
            return
        m = TRACE.match(s)
        if m:
            t = m.group(1)
            self.label = self.short(t)
            self.pending = None
            if t not in self.seen:
                self.seen.add(t)
                if s[m.end():] != "FORCE":
                    self.work[t] = [time.monotonic(), 1]
                    self.pending = t
            return
        if TRACE_OTHER.match(s):
            return
        if self.pending is not None:
            name = tool_of(s)
            if name:
                self.label = f"{name}  {self.short(self.pending)}"
                self.work[self.pending][1] = SECONDS.get(name, 1)
                self.pending = None
        if KEEP.search(s):
            self.keep(col(C_ERR, self.short(s)))
        self.line = self.short(s)

    # --- output ---
    def clear(self):
        if self.drawn:
            self.out.write(f"\r\x1b[{self.drawn - 1}A\x1b[J" if self.drawn > 1 else "\r\x1b[J")
            self.drawn = 0

    def keep(self, text):
        self.clear()
        self.out.write(self.fit(text) + "\n")
        self.draw(force=True)

    @staticmethod
    def width():
        return max(20, shutil.get_terminal_size((100, 24)).columns - 1)

    def fit(self, text):
        w = self.width()
        plain = ANSI.sub("", text)
        if len(plain) <= w:
            return text
        m = re.match(r"(\x1b\[[0-9;]*m)", text)  # keep a leading colour on a cut line
        return (m.group(1) if m else "") + plain[: w - 1] + "…" + ("\x1b[0m" if m else "")

    def frac(self):
        """Share of the planned work done: each started target is credited with the time it has run, up to the time
        its tool usually takes (SECONDS), since make reports starts but not ends. Never 1 before the command exits."""
        if self.done == 0:
            return 1.0
        if not self.plan_w:
            return None
        now = time.monotonic()
        done = sum(min(w, now - t) for t, w in self.work.values())
        total = max(self.plan_w, sum(w for _, w in self.work.values()))
        return min(0.99, done / total)

    def bar(self):
        w = self.width()
        el = int(time.monotonic() - self.t0)
        clock = f"{el // 3600}:{el // 60 % 60:02d}:{el % 60:02d}" if el >= 3600 else f"{el // 60}:{el % 60:02d}"
        n = len(self.work)
        frac = self.frac()
        if frac is None:
            count = f"{n} steps" if n else "working"
            right = f" {SPIN[int(time.monotonic() * 4) % 4]} {count}  {clock}"
        else:
            total = max(self.plan_n, n)
            count = f"{n}/{'≥' if self.plan_min and n < total else ''}{total} steps"
            right = f" {frac * 100:3.0f}%  {count}  {clock}"
        inner = max(10, w - len(right) - 2)
        colour = C_BAR if self.done is None else C_DONE if self.done == 0 else C_FAIL
        if frac is None:  # no plan: a block that sweeps across to show it is alive
            pos = int(time.monotonic() * 8) % (inner + 6)
            cells = "".join(col(colour, "█") if pos - 6 < i <= pos else col(C_EMPTY, "─") for i in range(inner))
        else:
            eighths = int(frac * inner * 8)
            full, part = divmod(eighths, 8)
            head = PARTS[part] if part and full < inner else ""
            cells = col(colour, "█" * full + head) + col(C_EMPTY, "─" * (inner - full - len(head)))
        return f"{col(C_DIM, '▕')}{cells}{col(C_DIM, '▏')}{col(C_DIM, right)}"

    def draw(self, force=False):
        now = time.monotonic()
        if not force and now - self.last < 1 / FPS:
            return
        self.last = now
        self.clear()
        step = f"{self.phase}: {self.label}" if self.done is None else self.phase
        rows = [self.bar(), self.fit(col(C_STEP, step))]
        if self.done is None:
            rows.append(self.fit(col(C_LINE, " " + (self.line or "…") + " ")))
        self.out.write("\n".join(rows))
        self.out.flush()
        self.drawn = len(rows)


def plan(lines, tools):
    """The work a build will really do, from the `make -n -k --trace` output of its makes: 'STEPS SECONDS[+]'
    (targets, and their expected seconds by SECONDS; +: a lower bound, the dry run stopped early somewhere).
    A dry run assumes every FORCE'd target (tool stamps $(BUILD)/stamp/<tool>.sum, area.sig, splitenv, ...) changes
    and so lists everything built from them (~380 targets on a no-op build). A stamp only changes when tools/<tool>
    is newer than its .key (the stamp rule in ps2/proto/Makefile), and the other FORCE'd files are rewritten only when
    their text changes, taken not to happen; a target whose every reason is such an unchanged file, or a target
    dropped that way, is not counted. Neither are the FORCE'd checks themselves (View counts them neither)."""
    seen, quiet, work, err, pending = set(), set(), {}, False, None
    for s in lines:
        s = s.rstrip("\n")
        if "***" in s:
            err = True
        m = TRACE.match(s)
        if not m:
            if pending is not None:
                name = tool_of(s)
                if name:
                    work[pending] = SECONDS.get(name, 1)
                    pending = None
            continue
        pending = None
        t, why = m.group(1), s[m.end():]
        if t in seen:
            continue
        seen.add(t)
        if why == "FORCE":
            st = STAMP.search(t)
            try:
                if not st or os.path.getmtime(os.path.join(tools, st.group(1))) <= os.path.getmtime(t + ".key"):
                    quiet.add(t)
            except OSError:
                pass  # no stamp / key yet, or the tool is gone: the stamp is (re)made, and what uses it
        elif why and all(r in quiet for r in why.split()):
            quiet.add(t)
        else:
            work[t] = 1
            pending = t
    return f"{len(work)} {sum(work.values())}{'+' if err else ''}"


def tail(path, n):
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - 64 * 1024))
            return f.read().decode("utf-8", "replace").splitlines()[-n:]
    except OSError:
        return []


def main():
    args = sys.argv[1:]
    log = title = None
    while args and args[0] != "--":
        a = args.pop(0)
        if a == "--log" and args:
            log = args.pop(0)
        elif a == "--title" and args:
            title = args.pop(0)
        elif a == "--plan" and args:
            print(plan(sys.stdin, args.pop(0)))
            return 0
        elif a in ("-h", "--help"):
            print(__doc__.strip())
            return 0
        else:
            sys.exit(f"buildtui: unknown option {a} (see --help)")
    if not args or args[0] != "--" or len(args) < 2:
        sys.exit("usage: buildtui.py [--log FILE] [--title TEXT] -- COMMAND...")
    cmd = args[1:]
    if log:
        os.makedirs(os.path.dirname(os.path.abspath(log)), exist_ok=True)
    logf = open(log, "w", encoding="utf-8", errors="replace") if log else None

    env = dict(os.environ, PYTHONUNBUFFERED="1")
    # the child keeps the terminal's stdin and process group: Ctrl-C reaches it (and us) the usual way; we wait for it
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, lambda *_: proc.terminate())
    root = os.environ.get("DSR_TUI_ROOT", os.getcwd())
    view = View(sys.stdout, title or os.path.basename(cmd[0]), root)
    sys.stdout.write("\x1b[?25l")  # hide the cursor while the view redraws
    signal.signal(signal.SIGWINCH, lambda *_: view.draw(force=True))
    fd = proc.stdout.fileno()
    buf = b""
    try:
        view.draw(force=True)
        while True:
            try:
                r, _, _ = select.select([fd], [], [], 1 / FPS)
            except InterruptedError:
                continue
            if r:
                chunk = os.read(fd, 65536)
                if not chunk:
                    break
                buf += chunk
                *lines, buf = buf.split(b"\n")
                for b in lines:
                    s = b.decode("utf-8", "replace")
                    if logf:
                        logf.write(s + "\n")
                    view.feed(s.split("\r")[-1])
                if logf:
                    logf.flush()
            view.draw()
        if buf:
            s = buf.decode("utf-8", "replace")
            if logf:
                logf.write(s + "\n")
            view.feed(s)
        rc = proc.wait()
    finally:
        if proc.poll() is None:
            proc.wait()
        rc = proc.returncode
        if logf:
            logf.close()
        view.done = rc
        if rc == 0:
            view.phase = "finished"
        elif rc < 0 or rc in (130, 143):
            view.phase = "interrupted"
        else:
            view.phase = f"failed (exit code {rc}) at: {view.label}"
        view.draw(force=True)
        sys.stdout.write("\n\x1b[?25h")
        if rc != 0 and log:
            lines = [ln for ln in tail(log, 400) if not (ln.startswith("##tui ") or TRACE.match(ln) or TRACE_OTHER.match(ln))]
            sys.stdout.write("\n".join(view.fit(view.short(ln)) for ln in lines[-30:]) + "\n")
        if log and rc != 0:
            sys.stdout.write(f"full log: {log}\n")
        sys.stdout.flush()
    return 128 - rc if rc < 0 else rc


if __name__ == "__main__":
    sys.exit(main())
