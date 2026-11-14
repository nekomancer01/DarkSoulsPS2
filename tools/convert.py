"""The default build's packs (`make -C ps2/proto`: Firelink + its world blocks) without make, the shell or a compiler:
every export runs in this Python process. The project page's in-browser converter (tools/pages, Pyodide) runs this;
natively it is the check that it still matches the Makefile.

    python3 tools/convert.py <game dir> <build dir> [--lean] [--only STEP...] [--list] [--groups]
    python3 tools/convert.py --compare <build dir> <make's build dir>     (same packs, byte for byte)

Writes <build dir>/gen/... and <build dir>/data/*.pak exactly as the Makefile's recipes do (same tools, arguments and
environment); the ELF is the game-free one from CI (make -C ps2/proto elf) and tools/iso9660.py makes the disc.

The steps mirror ps2/proto/Makefile by hand: when a recipe there changes (a new export, new arguments, a new pack
entry), change plan() here the same way. Knob variables (MAPENV, MAPENV_<map>, LOD_<chr>, TEX_<chr>, DEFAULT_MAP) are
read from the Makefile itself, so retuning an area needs no change here. Each step runs like its own process: the
tools' modules are dropped from sys.modules first and the environment is the recipe's, so module-level knobs read
the right values.

--lean (the in-browser converter, whose files all live in memory): each intermediate under <build dir>/gen is removed
once no later step names it (in its arguments, environment or extra inputs), so the gen tree never holds more than
the steps still to come need. The packs come out the same; check with --compare against make's build.
"""
import gc, os, re, runpy, shlex, sys, time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
MAKEFILE = os.path.join(ROOT, "ps2", "proto", "Makefile")
GC_THRESHOLD = gc.get_threshold()


def make_vars(path=MAKEFILE):
    """Plain assignments of the Makefile (NAME = / := / ?= / += value; no functions): {name: value}"""
    v = {}
    for line in open(path, encoding="utf-8"):
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*(\+=|\?=|:=|=)\s*(.*?)\s*$", line)
        if not m or "$(" in m.group(3):
            continue
        name, op, val = m.groups()
        val = val.split("#")[0].strip()
        if op == "+=":
            v[name] = (v.get(name, "") + " " + val).strip()
        elif op == "?=":
            v.setdefault(name, val)
        else:
            v[name] = val
    return v


class Step:
    def __init__(self, name, tool, args, outputs, env=None, inputs=()):
        """inputs: files the tool reads that no argument or environment value names (a directory argument)"""
        self.name, self.tool, self.args, self.outputs, self.env = name, tool, args, outputs, env or {}
        self.inputs = list(inputs)

    def files(self):
        """every path the step names: arguments and environment values (pack.py's "name=raw:path" and "?" entries
        stripped to the path) and its extra inputs"""
        out = set()
        for x in list(self.args) + list(self.env.values()) + self.inputs:
            x = x.lstrip("?").split("=", 1)[-1]
            out.add(os.path.normpath(x[4:] if x.startswith("raw:") else x))
        return out

    def __repr__(self):
        env = " ".join(f"{k}={shlex.quote(v)}" for k, v in self.env.items())
        return f"{env + ' ' if env else ''}{self.tool} {' '.join(shlex.quote(a) for a in self.args)}"


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


def plan(game, build, mv=None):
    """The steps of `make -C ps2/proto` (default map with its world blocks), in an order that respects every recipe's
    inputs. Paths follow the Makefile: G = gen, D = data, A = the area's gen dir, AREA_DATA = its pack dir."""
    mv = mv or make_vars()
    G, D = os.path.join(build, "gen"), os.path.join(build, "data")
    default = mv.get("DEFAULT_MAP", "m10_02_00_00")
    steps = []

    def S(name, tool, args, outputs, env=None, inputs=()):
        steps.append(Step(name, tool, [str(a) for a in args], outputs, env, inputs))

    def mapenv(msb):
        kv = (mv.get("MAPENV", "") + " " + mv.get(f"MAPENV_{msb}", "")).split()
        return dict(x.split("=", 1) for x in kv)

    def area(msb, block, world_split):
        """one area (block=None) or world block: everything $(D)/$(OUT).pak needs, then its enemy packs; common.pak
        only for an area (blockpacks has none)"""
        out = block or msb
        A = G if out == default else os.path.join(G, out)
        AD = D if out == default else os.path.join(D, out)
        benv = {"AREA_BLOCK": block} if block else {}
        S(f"{out}: objects", "export_objects.py", [game, f"{A}/objects.bin", msb], [f"{A}/objects.bin"], benv)
        S(f"{out}: lights", "export_effects.py", [game, "--static-lights", f"{A}/lights.json", msb],
          [f"{A}/lights.json"], benv)
        S(f"{out}: map", "export_map.py", [game, f"{A}/map.bin", msb], [f"{A}/map.bin", f"{A}/bump.bin", f"{A}/dg.bin"],
          {**benv, **mapenv(msb), "STATIC_OBJ": f"{A}/objects.bin.static", "ANIM_OBJ": f"{A}/objects.bin.anim",
           "FX_LIGHTS_FILE": f"{A}/lights.json"})
        S(f"{out}: effects", "export_effects.py", [game, f"{A}/fx.bin", msb, "--tone", f"{A}/fx_tone.json"],
          [f"{A}/fx.bin"], benv)
        S(f"{out}: collision", "export_collision.py", [game, f"{A}/collision.bin", msb, "h"], [f"{A}/collision.bin"],
          benv)
        S(f"{out}: events", "export_emevd.py", [game, f"{A}/events.bin", msb, "--common", f"{A}/evcommon.bin",
                                                 "--report", f"{A}/events.txt"],
          [f"{A}/events.bin", f"{A}/evcommon.bin"], {**benv, "ANIM_OBJ": f"{A}/objects.bin.anim"})
        S(f"{out}: enemies", "export_enemies.py", [game, f"{A}/enemies.bin", msb], [f"{A}/enemies.bin"], benv)
        S(f"{out}: map AI", "lua50.py", ["--pack", "--msb", game, msb, f"{A}/ai_map.lua0",
                                        f"{game}/script/{msb}.luabnd.dcx"], [f"{A}/ai_map.lua0"], benv)
        if world_split:
            S(f"{out}: horizon", "horizon.py", [f"{A}/map.bin", f"{A}/hzn.bin"], [f"{A}/hzn.bin"],
              {"HZN_PANO": "1" if out == default else "0"})
            S(f"{out}: map split", "mapsplit.py", [f"{A}/map.bin", f"{A}/maptab.bin", f"{A}/mapres.bin", f"{A}/hzn.bin"],
              [f"{A}/maptab.bin", f"{A}/mapres.bin"])
        raw = "raw:" if block else ""
        entries = [f"map.bin={A}/map.bin", f"collision.bin={raw}{A}/collision.bin", f"enemies.bin={raw}{A}/enemies.bin",
                   f"objects.bin={raw}{A}/objects.bin", f"ai.lua0={raw}{A}/ai_map.lua0",
                   f"evcommon.bin={A}/evcommon.bin", f"bump.bin={raw}{A}/bump.bin", f"fx.bin={raw}{A}/fx.bin",
                   f"dg.bin={A}/dg.bin"]
        # "?" = a $(wildcard ...) entry: kept when the file exists by the time the step runs
        entries += [f"?cells.bin=raw:{A}/cells.bin", f"?pano.bin={A}/pano.bin"]
        if world_split:
            entries += [f"maptab.bin=raw:{A}/maptab.bin", f"mapres.bin=raw:{A}/mapres.bin", f"far.bin=raw:{A}/far.bin"]
        entries += [f"?events.bin={raw}{A}/events.bin"]
        # the pack comes after every block's horizon: its far panorama (farpano.py) is baked from the others'
        packs.append((out, A, [f"{D}/{out}.pak"] + entries, world_split))
        steps.append(("enemies", msb, block, A, AD))  # expanded once enemies.bin exists (export_enemies.py --models)
        if not block:
            S(f"{out}: common AI", "lua50.py", ["--pack", "--msb", game, msb, f"{A}/ai_common.lua0",
                                               f"{game}/script/aiCommon.luabnd.dcx"], [f"{A}/ai_common.lua0"])
            S(f"{out}: common pack", "pack.py", [f"{AD}/common.pak", f"esd.bin={G}/enemyCommon.esd.bin",
                                                 f"ai.lua0={A}/ai_common.lua0"], [f"{AD}/common.pak"])

    packs = []
    # shared by every area
    S("player model", "export_model.py", [game, f"{G}/model.bin"], [f"{G}/model.bin"])
    S("player ESD", "export_esd.py", [game, f"{G}/esd.bin"], [f"{G}/esd.bin"])
    for k in ("player", "combat", "progress"):
        S(f"{k} values", f"export_{k}.py", [game, f"{G}/{k}.gd"], [f"{G}/{k}.gd"])
    S("player pack", "pack.py", [f"{D}/c0000.pak", f"model.bin={G}/model.bin", f"esd.bin={G}/esd.bin"] +
      [f"{k}.gd={G}/{k}.gd" for k in ("player", "combat", "progress")], [f"{D}/c0000.pak"])
    S("menu", "export_menu.py", [game, f"{G}/menu.bin"], [f"{G}/menu.bin"])
    items = [f"{G}/items/{f}" for f in ("items.bin", "itemx.bin", "desc.bin", "icons.bin")]
    S("items", "export_items.py", [game, f"{G}/items"], items)
    screens = mv.get("SCREEN_NAMES", "").split()
    S("menu screens", "export_screens.py", [game, f"{G}/screens"], [f"{G}/screens/scr_{n}.bin" for n in screens])
    S("loading screen", "export_loading.py", [game, f"{G}/loading.bin"], [f"{G}/loading.bin"])
    S("menu pack", "pack.py", [f"{D}/menu.pak", f"menu.bin={G}/menu.bin", f"items.bin={G}/items/items.bin",
                               f"itemx.bin={G}/items/itemx.bin", f"desc.bin=raw:{G}/items/desc.bin",
                               f"icons.bin=raw:{G}/items/icons.bin"] +
      [f"scr_{n}.bin={G}/screens/scr_{n}.bin" for n in screens] + [f"loading.bin={G}/loading.bin"],
      [f"{D}/menu.pak"])
    S("enemy ESD", "export_esd.py", [game, f"{G}/enemyCommon.esd.bin", "enemyCommon"], [f"{G}/enemyCommon.esd.bin"])
    blocks = run_tool("area.py", ["--world", default], capture=True).split()  # Makefile WORLD_BLOCKS
    # $(A)/snd.stamp: every sound pack of the boot map and its world blocks' maps (SND_MAPS) in one export, which
    # chooses the SPU2 sample rate over the whole set; it reads only the game folder (DSR_GAME)
    snd_maps = sorted({default} | {b.split(":")[0] for b in blocks})
    S("sound", "export_audio.py", [f"{D}/snd", f"{G}/snd"] + snd_maps, [f"{D}/snd/main.pak"])
    for b in blocks:  # worldpacks: one sub-make per block (MAP=<msb> BLOCK=<block> blockpacks)
        msb, block = b.split(":")
        area(msb, block, True)
    area(default, None, bool(blocks))
    gens = [G] + [f"{G}/{b.split(':')[1]}" for b in blocks]  # Makefile PVS_PEERS: the world's blocks, boot map first
    for out, A, args, world_split in packs:
        if world_split:
            S(f"{out}: far panorama", "farpano.py", [f"{A}/far.bin", A] + gens, [f"{A}/far.bin"],
              inputs=[f"{g}/{f}" for g in gens for f in ("hzn.bin", "dg.bin", "map.bin")])
        if world_split and out == default:  # the boot map's far backdrop as picture layers (Makefile $(A)/pano.bin)
            S(f"{out}: backdrop pictures", "pano.py", ["bake", A, f"{A}/pano.bin"], [f"{A}/pano.bin"], {"HZN_PANO": "1"})
        S(f"{out}: pack", "pack.py", args, [f"{D}/{out}.pak"])
    if blocks:
        # reads <gen dir>/collision.bin and <gen dir>/<block>/collision.bin (export_world.main)
        cols = [f"{G}/collision.bin"] + [f"{G}/{b.split(':')[1]}/collision.bin" for b in blocks]
        S("world", "export_world.py", [game, f"{G}/world.bin", default, G], [f"{G}/world.bin"], inputs=cols)
        S("world pack", "pack.py", [f"{D}/world.pak", f"world.bin={G}/world.bin"], [f"{D}/world.pak"])
    return steps


def enemy_steps(game, msb, block, A, AD, mv):
    """$(ENEMIES:%=$(AREA_DATA)/%.pak) for one area / block: export_chr.py per enemy type, then its pack"""
    env = {"AREA_BLOCK": block} if block else {}
    chrs = run_tool("export_enemies.py", ["--models", game, msb], env, capture=True).split()
    out = []
    for c in chrs:
        lod, tex = mv.get(f"LOD_{c}", "1"), mv.get(f"TEX_{c}", "64")
        out.append(Step(f"{block or msb}: {c}", "export_chr.py", [game, c, msb, f"{A}/{c}", lod, tex, mv["CHR_BONES"]],
                        [f"{A}/{c}/chr.bin"], env))
        out.append(Step(f"{block or msb}: {c} pack", "pack.py", [f"{AD}/{c}.pak", f"model.bin={A}/{c}/model.bin",
                                                                  f"chr.bin={A}/{c}/chr.bin"], [f"{AD}/{c}.pak"]))
    return out


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
    return s.name if isinstance(s, Step) else f"{s[2] or s[1]}: enemy types"


def group_of(s):
    """the group a plan entry belongs to: its area / block ('<out>: ...' steps and enemy placeholders), 'world' for the
    world steps, 'sound' for the sound packs, 'shared' for the steps every area uses (player, menu, enemy ESD)"""
    if isinstance(s, tuple):
        return s[2] or s[1]
    if ": " in s.name:
        return s.name.split(": ", 1)[0]
    return "world" if s.name.startswith("world") else "sound" if s.name == "sound" else "shared"


def groups(game, build, mv=None):
    """The plan's groups in plan order, for running them side by side (the in-browser converter's parallel workers):
    [{name, deps, maps}], deps = the groups whose outputs a step of this group names (so they must have finished and
    their build files be present first), maps = how many map exports it runs (the heavy steps; scheduled first). Enemy
    steps read only the game and their own outputs, so their placeholders need no expansion here."""
    steps = plan(game, build, mv)
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
    mv = make_vars()
    steps = plan(game, build, mv)
    times, i = [], 0
    while i < len(steps):
        s = steps[i]
        if select is not None and group_of(s) not in select:
            i += 1
            continue
        if isinstance(s, tuple):  # enemy list known now
            steps[i:i + 1] = enemy_steps(game, *s[1:], mv)
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
        if s.name == "sound" and sys.platform == "emscripten":
            # fsb4.py decodes DSR's MP3 banks with libmpg123, which has no WebAssembly build here yet: the disc boots
            # without data/snd (sound.c stays silent) rather than the whole conversion failing
            log(f"[{i + 1}/{n}] sound: skipped in the browser (no MP3 decoder); build-iso.sh makes the sound packs")
            i += 1
            continue
        for o in s.outputs:
            os.makedirs(os.path.dirname(o), exist_ok=True)
        args = []
        for x in s.args:  # "?name=path": a pack entry the Makefile adds with $(wildcard)
            if x.startswith("?"):
                path = x.split("=", 1)[1]
                if not os.path.exists(path[4:] if path.startswith("raw:") else path):
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
            print(s if isinstance(s, Step) else f"(enemy types of {s[2] or s[1]}: export_chr.py + pack.py each)")
        return
    only = [x for x in a[a.index("--only") + 1:] if x != "--lean"] if "--only" in a else None
    t = time.time()
    times = run(game, build, only, lean="--lean" in a)
    for name, dt in sorted(times, key=lambda x: -x[1])[:8]:
        print(f"  {dt:7.1f} s  {name}")
    print(f"convert: {len(times)} steps in {time.time() - t:.0f} s -> {build}/data")


if __name__ == "__main__":
    main()
