"""The default build's packs (`make -C ps2/proto`: Firelink + its world blocks) without make, the shell or a compiler:
every export runs in this Python process. It is the check that both builds write the same packs (it was also the engine
of the in-browser converter, retired for now: notes/TECH_DEBT.md build-23).

    python3 tools/convert.py <game dir> <build dir> [--lean] [--only STEP...] [--list] [--groups]
    python3 tools/convert.py --compare <build dir> <make's build dir>     (same packs, byte for byte)

Writes <build dir>/gen/... and <build dir>/data/*.pak exactly as the Makefile's recipes do (same tools, arguments and
environment); the ELF is the game-free one from CI (make -C ps2/proto elf) and tools/iso9660.py makes the disc.

The steps, their knobs (MAPENV, LOD / TEX per enemy type, DEFAULT_MAP, ...) and the pack entries are tools/recipe.toml's,
the file the Makefile's export rules are generated from (tools/recipe.py), so the two cannot drift apart. The steps
that run a host program (the occlusion bake, tools/pvs_bake.c) build it natively as make does; the browser, which has
no build of it, skips them and leaves their pack entries out, as it skips the sound packs (no MP3 decoder). Each step
runs like its own process: the tools' modules are dropped from sys.modules first and the environment is the recipe's,
so module-level knobs read the right values.

--lean (the in-browser converter, whose files all live in memory): each intermediate under <build dir>/gen is removed
once no later step names it (in its arguments, environment or extra inputs), so the gen tree never holds more than
the steps still to come need. The packs come out the same; check with --compare against make's build.
"""
import gc, os, runpy, sys, time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import recipe  # noqa: E402
from recipe import Step  # noqa: E402

GC_THRESHOLD = gc.get_threshold()


def wasm_numpy():
    """Pyodide's numpy is 32-bit (intp = int32): bincount and repeat refuse int64 index / count arrays that 64-bit numpy
    takes as they are ("Cannot cast ... according to the rule 'safe'"), and the exporters build such arrays everywhere.
    In the browser both take them cast to intp first: the same values (an index or count past 2**31 cannot exist in a
    32-bit address space), so the packs stay the same bytes (TECH_DEBT build-7)."""
    import numpy as np
    if getattr(np, "_dsr_wasm", False) or np.dtype(np.intp).itemsize == 8:
        return
    bincount, repeat = np.bincount, np.repeat

    def intp(a):
        a = np.asarray(a)
        return a.astype(np.intp) if a.dtype.kind in "iu" and a.dtype.itemsize > np.dtype(np.intp).itemsize else a

    np.bincount = lambda x, weights=None, minlength=0: bincount(intp(x), weights, minlength)
    np.repeat = lambda a, repeats, axis=None: repeat(a, intp(repeats), axis)
    np._dsr_wasm = True


if sys.platform == "emscripten":
    wasm_numpy()


def run_tool(tool, args, env=None, capture=False):
    """Run tools/<tool> as `python3 -I tools/<tool> args` would, in this process. Returns its stdout when capture."""
    import io
    for name in [n for n, m in sys.modules.items() if getattr(m, "__file__", None) and
                 os.path.dirname(os.path.abspath(m.__file__)) == TOOLS]:
        del sys.modules[name]  # a fresh process: module-level knobs and caches start over
    saved_env, saved_argv, saved_path, saved_out = dict(os.environ), sys.argv, list(sys.path), sys.stdout
    os.environ.update(env or {})
    sys.argv = [os.path.join(TOOLS, tool)] + list(args)
    sys.path.insert(0, TOOLS)
    buf = io.StringIO() if capture else None
    if capture:
        sys.stdout = buf
    try:
        runpy.run_path(sys.argv[0], run_name="__main__")
    except SystemExit as e:
        if e.code not in (None, 0):
            raise RuntimeError(f"{tool} {' '.join(args)}: {e.code}") from None
    finally:
        sys.stdout = saved_out
        sys.argv, sys.path[:] = saved_argv, saved_path
        os.environ.clear()
        os.environ.update(saved_env)
        gc.unfreeze()  # export_map.py freezes its meshes out of the GC's scans; they are garbage now
        gc.set_threshold(*GC_THRESHOLD)  # and raises the thresholds for its own run
        gc.collect()
        # simplify_pool's forkserver lives on after the tool returns, and its __main__ is this tool with this
        # step's environment: a later step's workers would re-run export_map.py's top level with a stale STATIC_OBJ
        # (deleted by --lean by then). Stop it so each step starts its own (Pyodide has no multiprocessing).
        fs = sys.modules.get("multiprocessing.forkserver")
        if fs is not None and hasattr(fs._forkserver, "_stop"):
            fs._forkserver._stop()
    return buf.getvalue() if capture else None


def plan(game, build, over=None):
    """The steps of `make -C ps2/proto` (default map with its world blocks) from tools/recipe.toml, in an order that
    respects every step's inputs: Step instances, and ("chr", ctx) placeholders for an area's enemy types (expanded
    when reached: the list comes from the game, export_enemies.py --models)."""
    global PLAN
    PLAN = recipe.Plan(game, build, run_tool, over)
    return PLAN.steps()


def enemy_steps(placeholder):
    """an area's / block's enemy types: export_chr.py + its pack each (recipe scope "chr")"""
    return PLAN.expand_chr(placeholder[1])


def free_unneeded(steps, i, build):
    """--lean: remove the files under build/gen that no step after steps[i] names; -> bytes freed. Enemy placeholders
    (tuples) read only the game folder, and the steps they expand to name only their own outputs."""
    keep = set()
    for s in steps[i + 1:]:
        if isinstance(s, Step):
            keep |= s.files()
    freed = 0
    for r, _, fs in os.walk(os.path.join(build, "gen")):
        for f in fs:
            p = os.path.normpath(os.path.join(r, f))
            if p not in keep:
                freed += os.path.getsize(p)
                os.remove(p)
    return freed


def run(game, build, only=None, log=print, progress=None, lean=False):
    """Run the plan; progress(i, n, name) before each step. Returns the seconds each step took. lean: see --lean."""
    return run_part(game, build, only, log, progress, lean)[0]


def step_name(s):
    """a plan entry's name: a Step's, or for an enemy placeholder (expanded when reached) '<area>: enemy types'"""
    return recipe.step_name(s)


def group_of(s):
    """the group a plan entry belongs to (recipe.Plan.instance): its area / block for its exports and enemy types,
    '<area> packs' for the steps that read other areas' outputs (map split, far panorama, visibility, the pack), 'world'
    for the world steps, 'sound', 'menu' (after the boot area: the treasure shine takes its tone map), 'shared' for the
    rest every area uses (player, menu exports, enemy ESD)"""
    if isinstance(s, tuple):
        return s[1]["OUT"]
    return s.group


def groups(game, build, over=None):
    """The plan's groups in plan order, for running them side by side (the in-browser converter's parallel workers):
    [{name, deps, maps}], deps = the groups whose outputs a step of this group names (so they must have finished and
    their build files be present first), maps = how many map exports it runs (the heavy steps; scheduled first). Enemy
    steps read only the game and their own outputs, so their placeholders need no expansion here."""
    steps = plan(game, build, over)
    made, order, out = {}, [], {}
    for s in steps:
        g = group_of(s)
        if g not in out:
            order.append(g)
            out[g] = {"name": g, "deps": [], "maps": 0}
        if isinstance(s, tuple):
            continue
        for f in s.files():
            d = made.get(f)
            if d is not None and d != g and d not in out[g]["deps"]:
                out[g]["deps"].append(d)
        for o in s.outputs:
            made[os.path.normpath(o)] = g
        out[g]["maps"] += is_map_export(s.name)
    return [out[g] for g in order]


def is_map_export(name):
    """the step name of an area's / block's export_map.py run (the converter's largest by far)"""
    return name.endswith(": map")


def run_part(game, build, only=None, log=print, progress=None, lean=False, start=0, pause=None, select=None):
    """run() from step index start (the plan is the same every time, so an index names a step), stopping after any
    step for which pause(name of the next step) is true -> (seconds per step, index to resume at, or None when the
    plan is done). The in-browser converter goes on in a fresh Pyodide before each map export and whenever its
    WebAssembly memory, which never shrinks, has grown past what the next steps need (tools/pages/worker.js).
    select: run only the steps of these groups (group_of; the parallel workers run one group each, see groups());
    the step indices then count the plan with only those groups' enemy lists expanded, the same in every run with
    the same select."""
    steps = plan(game, build)
    browser = sys.platform == "emscripten"
    skipped = set()  # outputs of steps the browser cannot run: their pack entries are left out
    times, i = [], 0
    while i < len(steps):
        s = steps[i]
        if select is not None and group_of(s) not in select:
            i += 1
            continue
        if isinstance(s, tuple):  # enemy list known now
            steps[i:i + 1] = enemy_steps(s)
            continue
        if i < start:
            i += 1
            continue
        n = len(steps)  # grows as enemy lists are expanded
        if progress:
            progress(i, n, s.name)
        if only and not any(o in s.name for o in only):
            i += 1
            continue
        if browser and (s.raw.get("browser") is False or s.raw.get("host")):
            # sound: fsb4.py decodes DSR's MP3 banks with libmpg123, which has no WebAssembly build here yet: the disc
            # boots without data/snd (sound.c stays silent). Host programs (the occlusion bake): no browser build yet,
            # the packs go without pvs.bin (src/pvs.c: every chunk visible). TECH_DEBT build-18.
            why = "no MP3 decoder" if s.raw.get("browser") is False else f"no browser build of {s.raw['host']}"
            log(f"[{i + 1}/{n}] {s.name}: skipped in the browser ({why}); build-iso.sh makes it")
            skipped |= {os.path.normpath(o) for o in s.outputs}
            i += 1
            continue
        if s.raw.get("host") and not browser:
            PLAN.host(s.raw["host"], log)
        for o in s.outputs:
            os.makedirs(os.path.dirname(o), exist_ok=True)
        args = []
        for x in s.args:  # "?name=path": a pack entry the Makefile adds with $(wildcard)
            path = x.lstrip("?").split("=", 1)[-1]
            path = os.path.normpath(recipe.entry_path(path))
            if s.tool == "pack.py" and "=" in x and path in skipped:
                continue
            if x.startswith("?"):
                if not os.path.exists(path):
                    continue
                x = x[1:]
            args.append(x)
        # DSR_GAME: tools that look the game up themselves (gamepath.game_dir() without an argument, e.g. emevd.py
        # under export_emevd.py) find it by Steam auto-detect natively, which a browser has none of
        env = {"DSR_GAME": os.path.abspath(game), **s.env}
        t = time.time()
        log(f"[{i + 1}/{n}] {s.name}")
        run_tool(s.tool, args, env)
        times.append((s.name, time.time() - t))
        if lean and not only:
            freed = free_unneeded(steps, i, build)
            if freed:
                log(f"  freed {freed / 1048576:.1f} MB of intermediates")
        i += 1
        while select is not None and i < len(steps) and group_of(steps[i]) not in select:
            i += 1
        if pause and i < len(steps) and pause(step_name(steps[i])):
            return times, i
    return times, None


def compare(a, b):
    """Every .pak under b/data (make's build) exists in a/data with the same bytes"""
    bad = 0
    da, db = os.path.join(a, "data"), os.path.join(b, "data")
    for r, _, fs in os.walk(db):
        for f in sorted(fs):
            if not f.endswith(".pak"):
                continue
            rel = os.path.relpath(os.path.join(r, f), db)
            pa = os.path.join(da, rel)
            if not os.path.exists(pa):
                print(f"missing: {rel}")
                bad += 1
            elif open(pa, "rb").read() != open(os.path.join(db, rel), "rb").read():
                print(f"differs: {rel}")
                bad += 1
            else:
                print(f"same: {rel}")
    return bad


def main():
    a = sys.argv[1:]
    if a and a[0] == "--compare":
        sys.exit(1 if compare(a[1], a[2]) else 0)
    pos = [x for x in a[:a.index("--only")] if x != "--lean"] if "--only" in a else [x for x in a if x != "--lean"]
    if len(pos) < 2 or any(x.startswith("--") for x in pos[:2]):
        sys.exit(__doc__)
    game, build = pos[0], os.path.abspath(pos[1])
    if os.path.realpath(build) == os.path.realpath(game) or \
            os.path.realpath(build).startswith(os.path.realpath(game) + os.sep):
        sys.exit("convert.py: the build dir must be outside the game folder (the game folder is only read)")
    if "--groups" in a:
        for g in groups(game, build):
            print(f"{g['name']}: {g['maps']} map export(s), after {', '.join(g['deps']) or 'nothing'}")
        return
    if "--list" in a:
        for s in plan(game, build):
            print(s if isinstance(s, Step) else f"(enemy types of {s[1]['OUT']}: export_chr.py + pack.py each)")
        return
    only = [x for x in a[a.index("--only") + 1:] if x != "--lean"] if "--only" in a else None
    t = time.time()
    times = run(game, build, only, lean="--lean" in a)
    for name, dt in sorted(times, key=lambda x: -x[1])[:8]:
        print(f"  {dt:7.1f} s  {name}")
    print(f"convert: {len(times)} steps in {time.time() - t:.0f} s -> {build}/data")


if __name__ == "__main__":
    main()
