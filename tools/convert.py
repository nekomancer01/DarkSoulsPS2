"""The default build's packs (`make -C ps2/proto`: Firelink + its world blocks) without make, the shell or a compiler:
every export runs in this Python process. The project page's in-browser converter (tools/pages, Pyodide) runs this;
natively it is the check that it still matches the Makefile.

    python3 tools/convert.py <game dir> <build dir> [--only STEP...] [--list]
    python3 tools/convert.py --compare <build dir> <make's build dir>     (same packs, byte for byte)

Writes <build dir>/gen/... and <build dir>/data/*.pak exactly as the Makefile's recipes do (same tools, arguments and
environment); the ELF is the game-free one from CI (make -C ps2/proto elf) and tools/iso9660.py makes the disc.

The steps mirror ps2/proto/Makefile by hand: when a recipe there changes (a new export, new arguments, a new pack
entry), change plan() here the same way. Knob variables (MAPENV, MAPENV_<map>, LOD_<chr>, TEX_<chr>, DEFAULT_MAP) are
read from the Makefile itself, so retuning an area needs no change here. Each step runs like its own process: the
tools' modules are dropped from sys.modules first and the environment is the recipe's, so module-level knobs read
the right values.
"""
import gc, os, re, runpy, shlex, sys, time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
MAKEFILE = os.path.join(ROOT, "ps2", "proto", "Makefile")


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
    def __init__(self, name, tool, args, outputs, env=None):
        self.name, self.tool, self.args, self.outputs, self.env = name, tool, args, outputs, env or {}

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
        gc.collect()
    return buf.getvalue() if capture else None


def plan(game, build, mv=None):
    """The steps of `make -C ps2/proto` (default map with its world blocks), in an order that respects every recipe's
    inputs. Paths follow the Makefile: G = gen, D = data, A = the area's gen dir, AREA_DATA = its pack dir."""
    mv = mv or make_vars()
    G, D = os.path.join(build, "gen"), os.path.join(build, "data")
    default = mv.get("DEFAULT_MAP", "m10_02_00_00")
    steps = []

    def S(name, tool, args, outputs, env=None):
        steps.append(Step(name, tool, [str(a) for a in args], outputs, env))

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
            S(f"{out}: map split", "mapsplit.py", [f"{A}/map.bin", f"{A}/maptab.bin", f"{A}/mapres.bin"],
              [f"{A}/maptab.bin", f"{A}/mapres.bin"])
        raw = "raw:" if block else ""
        entries = [f"map.bin={A}/map.bin", f"collision.bin={raw}{A}/collision.bin", f"enemies.bin={raw}{A}/enemies.bin",
                   f"objects.bin={raw}{A}/objects.bin", f"ai.lua0={raw}{A}/ai_map.lua0",
                   f"evcommon.bin={A}/evcommon.bin", f"bump.bin={raw}{A}/bump.bin", f"fx.bin={raw}{A}/fx.bin",
                   f"dg.bin={A}/dg.bin"]
        # "?" = a $(wildcard ...) entry: kept when the file exists by the time the step runs
        entries += [f"?cells.bin=raw:{A}/cells.bin", f"?pano.bin={A}/pano.bin"]
        if world_split:
            entries += [f"maptab.bin=raw:{A}/maptab.bin", f"mapres.bin=raw:{A}/mapres.bin"]
        entries += [f"?events.bin={raw}{A}/events.bin"]
        S(f"{out}: pack", "pack.py", [f"{D}/{out}.pak"] + entries, [f"{D}/{out}.pak"])
        steps.append(("enemies", msb, block, A, AD))  # expanded once enemies.bin exists (export_enemies.py --models)
        if not block:
            S(f"{out}: common AI", "lua50.py", ["--pack", "--msb", game, msb, f"{A}/ai_common.lua0",
                                               f"{game}/script/aiCommon.luabnd.dcx"], [f"{A}/ai_common.lua0"])
            S(f"{out}: common pack", "pack.py", [f"{AD}/common.pak", f"esd.bin={G}/enemyCommon.esd.bin",
                                                 f"ai.lua0={A}/ai_common.lua0"], [f"{AD}/common.pak"])

    # shared by every area
    S("player model", "export_model.py", [game, f"{G}/model.bin"], [f"{G}/model.bin"])
    S("player ESD", "export_esd.py", [game, f"{G}/esd.bin"], [f"{G}/esd.bin"])
    for k in ("player", "combat", "progress"):
        S(f"{k} values", f"export_{k}.py", [game, f"{G}/{k}.gd"], [f"{G}/{k}.gd"])
    S("player pack", "pack.py", [f"{D}/c0000.pak", f"model.bin={G}/model.bin", f"esd.bin={G}/esd.bin"] +
      [f"{k}.gd={G}/{k}.gd" for k in ("player", "combat", "progress")], [f"{D}/c0000.pak"])
    S("menu", "export_menu.py", [game, f"{G}/menu.bin"], [f"{G}/menu.bin"])
    S("menu pack", "pack.py", [f"{D}/menu.pak", f"menu.bin={G}/menu.bin"], [f"{D}/menu.pak"])
    S("enemy ESD", "export_esd.py", [game, f"{G}/enemyCommon.esd.bin", "enemyCommon"], [f"{G}/enemyCommon.esd.bin"])
    blocks = run_tool("area.py", ["--world", default], capture=True).split()  # Makefile WORLD_BLOCKS
    for b in blocks:  # worldpacks: one sub-make per block (MAP=<msb> BLOCK=<block> blockpacks)
        msb, block = b.split(":")
        area(msb, block, True)
    area(default, None, bool(blocks))
    if blocks:
        S("world", "export_world.py", [game, f"{G}/world.bin", default, G], [f"{G}/world.bin"])
        S("world pack", "pack.py", [f"{D}/world.pak", f"world.bin={G}/world.bin"], [f"{D}/world.pak"])
    return steps


def enemy_steps(game, msb, block, A, AD, mv):
    """$(ENEMIES:%=$(AREA_DATA)/%.pak) for one area / block: export_chr.py per enemy type, then its pack"""
    env = {"AREA_BLOCK": block} if block else {}
    chrs = run_tool("export_enemies.py", ["--models", game, msb], env, capture=True).split()
    out = []
    for c in chrs:
        lod, tex = mv.get(f"LOD_{c}", "1"), mv.get(f"TEX_{c}", "64")
        out.append(Step(f"{block or msb}: {c}", "export_chr.py", [game, c, msb, f"{A}/{c}", lod, tex],
                        [f"{A}/{c}/chr.bin"], env))
        out.append(Step(f"{block or msb}: {c} pack", "pack.py", [f"{AD}/{c}.pak", f"model.bin={A}/{c}/model.bin",
                                                                  f"chr.bin={A}/{c}/chr.bin"], [f"{AD}/{c}.pak"]))
    return out


def run(game, build, only=None, log=print, progress=None):
    """Run the plan; progress(i, n, name) before each step. Returns the seconds each step took."""
    mv = make_vars()
    steps = plan(game, build, mv)
    times, i = [], 0
    while i < len(steps):
        s = steps[i]
        if isinstance(s, tuple):  # enemy list known now
            steps[i:i + 1] = enemy_steps(game, *s[1:], mv)
            continue
        n = len(steps)  # grows as enemy lists are expanded
        if progress:
            progress(i, n, s.name)
        if only and not any(o in s.name for o in only):
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
        env = dict(s.env)
        t = time.time()
        log(f"[{i + 1}/{n}] {s.name}")
        run_tool(s.tool, args, env)
        times.append((s.name, time.time() - t))
        i += 1
    return times


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
    if len(a) < 2:
        sys.exit(__doc__)
    game, build = a[0], os.path.abspath(a[1])
    if "--list" in a:
        for s in plan(game, build):
            print(s if isinstance(s, Step) else f"(enemy types of {s[2] or s[1]}: export_chr.py + pack.py each)")
        return
    only = a[a.index("--only") + 1:] if "--only" in a else None
    t = time.time()
    times = run(game, build, only)
    for name, dt in sorted(times, key=lambda x: -x[1])[:8]:
        print(f"  {dt:7.1f} s  {name}")
    print(f"convert: {len(times)} steps in {time.time() - t:.0f} s -> {build}/data")


if __name__ == "__main__":
    main()
