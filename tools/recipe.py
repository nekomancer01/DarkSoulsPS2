#!/usr/bin/env python3
"""The export recipe (tools/recipe.toml, Polaris 3.1): its reader, the Makefile generator and the step planner.

    python3 tools/recipe.py make [--check]     write ps2/proto/export_vars.mk + export_rules.mk (--check: compare only,
                                               exit 1 when the committed files are stale; CI tools/ci/offline.sh)
    python3 tools/recipe.py list <game> <build> [--json]
                                               the default build's steps as convert.py runs them (enemy types
                                               unexpanded), --json with every field (inputs, outputs, tools, env)
    python3 tools/recipe.py format             the recipe's field reference (the comment block of recipe.toml)

Two renderings of one template language (recipe.toml header): make text for the generated rules ("{A}" -> "$(A)",
conditions -> $(if ...), lists -> $(foreach ...)), and concrete values for convert.py (plan(): every step instance of
the default build in an order that respects their files, with the steps the goals need). DERIVED names are the only
values computed twice, once here and once in ps2/proto/Makefile under the same name; keep the two together.
"""
import glob, json, os, re, shlex, subprocess, sys

try:
    import tomllib
except ImportError:  # Python < 3.11
    tomllib = None

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
PROTO = os.path.join(ROOT, "ps2", "proto")
RECIPE = os.path.join(TOOLS, "recipe.toml")
VARS_MK = os.path.join(PROTO, "export_vars.mk")
RULES_MK = os.path.join(PROTO, "export_rules.mk")
PLACE = re.compile(r"\{([A-Za-z_][A-Za-z0-9_.]*)\}")


def load(path=RECIPE):
    if tomllib is None:
        sys.exit("recipe.py: needs Python 3.11+ (tomllib)")
    with open(path, "rb") as f:
        r = tomllib.load(f)
    if r.get("version") != 1:
        sys.exit(f"recipe.py: {path}: recipe version {r.get('version')} is not 1")
    return r


def as_list(x):
    return x if isinstance(x, list) else [x]


# --- make rendering -----------------------------------------------------------------------------------------------

# template names whose make spelling is not $(NAME); CHR differs between targets (pattern) and recipes ($*)
MAKE_NAME = {"AD": "$(AREA_DATA)", "SRC": "src", "TOOLS": "../../tools", "RAW": "$(if $(BLOCK),raw:)",
             "HZN": "$(or $(HZN),1)"}
MAKE_COND = {"block": "$(BLOCK)", "world": "$(WORLD_BLOCKS)", "world_split": "$(WORLD_SPLIT)",
             "lite_run": "$(LITE_RUN)", "tex_peers": "$(TEX_PEERS)"}


class Make:
    """renders templates for one step; target=True: in a rule's target / prerequisite list (CHR -> c%)"""

    def __init__(self, loops=()):
        self.loops = set(loops)

    def name(self, n, target):
        if n in self.loops:
            return f"$({n})"
        if n == "CHR":
            return "c%" if target else "c$*"
        if n in ("LOD", "TEX"):
            return f"$(or $({n}_c$*),{1 if n == 'LOD' else 64})"
        return MAKE_NAME.get(n, f"$({n})")

    def s(self, text, target=False, quote_game=True):
        out = PLACE.sub(lambda m: self.name(m.group(1), target), text)
        if quote_game and "{GAME}" in text:
            out = f'"{out}"'
        return out

    def cond(self, c):
        if isinstance(c, list):
            return f"$(and {','.join(self.cond(x) for x in c)})"
        if c.startswith("not:"):
            return f"$(if {self.cond(c[4:])},,1)"
        if c.startswith("exists:"):
            return f"$(wildcard {self.s(c[7:], quote_game=False)})"
        return MAKE_COND[c]

    def items(self, xs, target=False):
        """a list template -> make text (words separated by spaces)"""
        out = []
        for x in as_list(xs):
            if isinstance(x, str):
                if x.startswith("?"):  # a pack entry kept only when its file exists when the step runs
                    path = x.split("=", 1)[1]
                    path = path[4:] if path.startswith("raw:") else path
                    out.append(f"$(if $(wildcard {self.s(path, target, False)}),{self.s(x[1:], target)})")
                else:
                    out.append(self.s(x, target))
            elif "if" in x:
                t, e = self.items(x.get("then", []), target), self.items(x.get("else", []), target)
                out.append(f"$(if {self.cond(x['if'])},{t}{',' + e if e else ''})")
            elif "foreach" in x:
                sub = Make(self.loops | {x["as"]})
                out.append(f"$(foreach {x['as']},$({x['foreach']}),{sub.items(x['do'], target)})")
            else:
                raise ValueError(f"recipe item {x!r}")
        return " ".join(o for o in out if o)


def tool_sums(tools):
    out = []
    for t in tools:
        if any(c in t for c in "*?["):
            out.append(f"$(patsubst ../../tools/%,$(TS)/%.sum,$(wildcard ../../tools/{t}))")
        else:
            out.append(f"$(TS)/{t}.sum")
    return out


def pack_inputs(step):
    """a pack step's prerequisites when it lists none: every entry's file, except the "?" ones (make's $(wildcard))"""
    def strip(xs):
        out = []
        for x in as_list(xs):
            if isinstance(x, str):
                if not x.startswith("?"):
                    p = x.split("=", 1)[1]
                    out.append(p[4:] if p.startswith("raw:") else p)
            elif "if" in x:
                out.append({"if": x["if"], "then": strip(x.get("then", [])), "else": strip(x.get("else", []))})
            else:
                out.append({**x, "do": strip(x["do"])})
        return out
    return strip(step.get("entries", []))


def step_inputs(step):
    return step["inputs"] if "inputs" in step else pack_inputs(step)


EXPORTED = []  # the recipe's export list (make_rules sets it): environment every step's tool sees


def step_cache_prefix(step, m):
    """tools/stepcache.py's arguments before the tool (Polaris 3.2 part 2: the build cache's "step" namespace), or ""
    for a step that is not cached: packs (cheap, and as large as their inputs), steps with a directory among their
    maybe outputs (sound: its samples have their own cache) and cache = false. The wrapper hashes make's $^, the reads, the step's
    environment and its command line, and stores out, side and the maybe files (absent ones as absent); see
    tools/stepcache.py"""
    maybe = as_list(step.get("maybe", []))
    if step["tool"] == "pack.py" or step.get("cache") is False or \
            any(isinstance(x, str) and x.endswith("/") for x in maybe):
        return ""
    outs = m.items(as_list(step["out"]) + as_list(step.get("side", [])) + maybe)
    reads = m.items(step.get("reads", []))
    envs = list(step.get("env", {})) + EXPORTED
    if "env_line" in step:
        envs.append(m.s(step["env_line"], quote_game=False))
    name = m.s(step["name"], quote_game=False).replace("'", "")
    return f"stepcache.py $(BUILD) '{name}' {outs} :: $^ :: {reads} :: {' '.join(envs)} :: ../../tools/"


def make_command(step, m):
    mk = step.get("make", {})
    if "runner" in mk:
        runner = mk["runner"]
    elif step.get("queue"):
        if step["queue"] != 12:
            raise ValueError(f"{step['name']}: the Makefile's $(PY) queues with 12 GB; add a runner for {step['queue']}")
        runner = "$(PY)"
    else:
        runner = "python3"
    args = list(step.get("args", []))
    if step["tool"] == "pack.py":
        args += step.get("entries", [])
    words = [f"{runner} -I ../../tools/{step_cache_prefix(step, m)}{step['tool']}", m.items(args)]
    env = " ".join(f"{k}='{m.s(v, quote_game=False)}'" for k, v in step.get("env", {}).items())
    cmd = " ".join(w for w in words if w)
    if env:
        cmd = f"{env} {cmd}"
    if "env_line" in step:
        cmd = f"env {m.s(step['env_line'], quote_game=False)} {cmd}"
    if "wrap" in mk:
        cmd = mk["wrap"].replace("{cmd}", "\0")
        cmd = m.s(cmd, quote_game=False).replace("\0", (f"{env} " if env else "") + " ".join(w for w in words if w))
    return cmd


def make_rules(r):
    """export_rules.mk: one rule per step (the shared, area, chr and world scopes all live in every make: the targets
    it builds are chosen by the hand-written goals in ps2/proto/Makefile)"""
    EXPORTED[:] = r.get("export", [])
    made = {x for st in r["step"] for x in as_list(st["out"]) + as_list(st.get("side", [])) if isinstance(x, str)}
    out = [HEADER, ""]
    for name, h in r.get("host", {}).items():
        var = name.upper()
        # a source is a path under tools/, or a template ("{SRC}/chunk.c": ps2/proto/src)
        srcs = " ".join(Make().s(s, quote_game=False) if s.startswith("{") else f"../../tools/{s}" for s in h["sources"])
        cmd = h["cmd"].replace("{out}", "$@").replace("{GPU}", f"$(if $({var}_GPU_OK),{h['gpu_flags']})")
        out += [f"{Make().s('{' + var + '}', True)}: {srcs}", "\t@mkdir -p $(@D)", "\t" + Make().s(cmd, quote_game=False), ""]
    for step in r["step"]:
        m = Make()
        mk = step.get("make", {})
        # one make word per out item (a conditional item, $(if ...), stays one word)
        outs = [w for x in as_list(step["out"]) for w in ([m.items([x], True)] if isinstance(x, dict) and "if" in x
                                                           else m.items([x], True).split()) if w]
        target = m.s(mk["target"], True) if "target" in mk else None
        prereq = [m.items(step_inputs(step), True)] + tool_sums(step.get("tools", [])) + list(mk.get("deps", []))
        prereq = " ".join(p for p in prereq if p)
        # reads another step makes come first (order only: not a reason to re-run), so a read is never missing or
        # half-written while the step runs and the step cache's key sees the same reads every build
        made_reads = [m.s(x, True) for x in as_list(step.get("reads", [])) if isinstance(x, str) and x in made]
        order = list(mk.get("order_only", [])) + made_reads
        if order:
            prereq += " | " + " ".join(order)
        out.append(f"# {m.s(step['name'], quote_game=False).replace('c$*', 'c####')} ({step['scope']}): tools/{step['tool']}")
        for k, v in mk.get("vars", {}).items():
            out.append(f"{target or outs[0]}: {k} := {v}")
        if target:
            head = target
        elif len(outs) > 1:
            head = " ".join(outs) + " &"
        else:
            head = outs[0]
        out.append(f"{head}: {prereq}")
        mkdirs = " ".join(["$(@D)"] + [m.s(d, quote_game=False) for d in mk.get("mkdir", [])])
        out.append(f"\t@mkdir -p {mkdirs}")
        out += ["\t" + m.s(x, quote_game=False) for x in mk.get("pre", [])]
        out.append("\t" + make_command(step, m))
        out += ["\t" + x for x in mk.get("post", [])]
        if target and mk.get("outs") == "stamp":
            out.append(f"{' '.join(outs)}: {target} ;")
        main = target or outs[0]
        checks = (outs if target and mk.get("outs") == "check" else []) + m.items(step.get("side", []), True).split()
        for c in checks:
            what = "it" if target else f"{os.path.basename(main).replace('%', 'c####')}"
            out.append(f"{c}: {main}")
            out.append(f'\t@test -f $@ || {{ echo "$@ missing: re-export {what}"; rm -f $<; exit 1; }}')
        if step["scope"] == "chr" and not step.get("goal"):
            out.append(f".PRECIOUS: {' '.join(outs + m.items(step.get('side', []), True).split())}")
        out.append("")
    return "\n".join(out)


def make_vars(r):
    """export_vars.mk: the knobs (?=), plain values, map tuning, per-character tables and the host programs' paths,
    included at the top of ps2/proto/Makefile"""
    m = Make()
    out = [HEADER, ""]
    for k, v in r.get("knobs", {}).items():
        out.append(f"{k} ?= {m.s(v, quote_game=False)}".rstrip())
    for k in r.get("export", []):
        out.append(f"export {k}")
    for k, v in r.get("vars", {}).items():
        out.append(f"{k} = {v}")
    me = r.get("mapenv", {})
    out.append(f"MAPENV = {me.get('all', '')}")
    for row in me.get("area", []):
        val = row["env"] if "when" not in row else f"$(if {m.cond(row['when'])},{row['env']})"
        out.append(f"MAPENV_{row['map']} += {val}")
    for kind, var in (("lod", "LOD"), ("tex", "TEX")):
        for c, v in r.get("chr", {}).get(kind, {}).items():
            out.append(f"{var}_{c} = {v}")
    for name, h in r.get("host", {}).items():
        var = name.upper()
        out.append(f"{var} = {m.s(h['out'], quote_game=False)}")
        out.append(f"{var}_GPU_OK := $(if $(filter 1,$(PVS_GPU)),$(shell {h['gpu_probe']}))")
    return "\n".join(out) + "\n"


HEADER = ("# Generated by tools/recipe.py from tools/recipe.toml: do not edit (python3 tools/recipe.py make; CI checks it is\n"
          "# current). The export steps' documentation is in tools/recipe.toml.")


def write_make(check=False):
    r = load()
    stale = []
    for path, text in ((VARS_MK, make_vars(r)), (RULES_MK, make_rules(r))):
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old == text:
            continue
        if check:
            stale.append(os.path.relpath(path, ROOT))
        else:
            with open(path + ".tmp", "w", encoding="utf-8") as f:
                f.write(text)
            os.replace(path + ".tmp", path)
            print(f"wrote {os.path.relpath(path, ROOT)}")
    if stale:
        print(f"recipe.py: {', '.join(stale)} not generated from tools/recipe.toml: run python3 tools/recipe.py make",
              file=sys.stderr)
        return 1
    return 0


# --- Python rendering (convert.py) --------------------------------------------------------------------------------

class Ctx(dict):
    """the values of one step instance: paths, context, knobs, derived values; list values for list names"""

    def s(self, text):
        def one(m):
            v = self[m.group(1)]
            return " ".join(v) if isinstance(v, list) else str(v)
        return PLACE.sub(one, text)

    def cond(self, c):
        if isinstance(c, list):
            return all(self.cond(x) for x in c)
        if c.startswith("not:"):
            return not self.cond(c[4:])
        if c.startswith("exists:"):
            return os.path.exists(self.s(c[7:]))
        return bool(self[c] if c != "world_split" else self["world_split"])

    def items(self, xs):
        """a list template -> [str]; "?" entries stay marked (decided when the step runs)"""
        out = []
        for x in as_list(xs):
            if isinstance(x, str):
                m = PLACE.fullmatch(x)
                if m and isinstance(self.get(m.group(1)), list):
                    out += self[m.group(1)]
                elif m and m.group(1) in self.get("_words", ()):
                    out += self.s(x).split()
                else:
                    out.append(self.s(x))
            elif "if" in x:
                out += self.items(x.get("then", []) if self.cond(x["if"]) else x.get("else", []))
            else:
                vals = self[x["foreach"]]
                for v in (vals.split() if isinstance(vals, str) else vals):
                    sub = Ctx(self)
                    sub[x["as"]] = v
                    out += sub.items(x["do"])
        return out


def knob_values(r, over=None):
    """knob name -> value (knobs referring to knobs resolved), overridden by over"""
    over = over or {}
    k = {**r.get("knobs", {}), **r.get("vars", {}), **over}
    for _ in range(4):
        k = {n: PLACE.sub(lambda m: k.get(m.group(1), ""), v) for n, v in k.items()}
    return k


def host_tag():
    try:
        out = subprocess.run(["ldd", "--version"], capture_output=True, text=True).stdout + \
            subprocess.run(["ldd", "--version"], capture_output=True, text=True).stderr
    except OSError:
        return ""
    return "-musl" if "musl" in out.lower() else ""


def mapenv_line(r, msb, block):
    me = r.get("mapenv", {})
    words = me.get("all", "").split()
    for row in me.get("area", []):
        if row["map"] == msb and (row.get("when") != "not:block" or not block):
            if row.get("when") not in (None, "not:block"):
                raise ValueError(f"mapenv: condition {row['when']} not supported in convert.py")
            words += row["env"].split()
    return " ".join(words)


class Step:
    """one step instance: what convert.py runs. args may hold "?name=path" pack entries (kept when the file exists)."""

    def __init__(self, name, tool, args, outputs, env=None, inputs=(), group=None, raw=None, inst=None):
        self.name, self.tool, self.args, self.outputs, self.env = name, tool, list(args), list(outputs), env or {}
        self.inputs = list(inputs)
        self.group, self.raw, self.inst = group, raw or {}, inst
        self.maybe = []  # outputs written only sometimes (recipe "maybe"; also in outputs)

    def files(self):
        """every path the step names: arguments and environment values (pack.py's "name=raw:path" and "?" entries
        stripped to the path) and its extra inputs"""
        out = set()
        for x in list(self.args) + list(self.env.values()) + self.inputs:
            x = x.lstrip("?").split("=", 1)[-1]
            out.add(os.path.normpath(x[4:] if x.startswith("raw:") else x))
        return out

    def need_files(self):
        """files() without the "?" pack entries: a file that only may be there pulls no step into the plan (unless the
        step also lists it as an input)"""
        return Step(self.name, self.tool, [x for x in self.args if not x.startswith("?")], [], self.env,
                    self.inputs).files()

    def __repr__(self):
        env = " ".join(f"{k}={shlex.quote(v)}" for k, v in self.env.items())
        return f"{env + ' ' if env else ''}{self.tool} {' '.join(shlex.quote(a) for a in self.args)}"


def run_list(r, name, ctx, run_tool):
    """[lists] NAME: the words a tool prints (convert.run_tool, capture)"""
    spec = r["lists"][name]
    env = {k: ctx.s(v) for k, v in spec.get("env", {}).items()}
    env = {k: v for k, v in env.items() if v}
    return run_tool(spec["tool"], ctx.items(spec["args"]), env, capture=True).split()


class Plan:
    """The default build (make -C ps2/proto: the boot map and its world blocks) as step instances.
    over: knob overrides; run_tool(tool, args, env, capture): convert.run_tool (lists come from the tools)."""

    def __init__(self, game, build, run_tool, over=None, recipe=None):
        self.r = recipe or load()
        self.game, self.build = game, os.path.abspath(build)
        self.run_tool = run_tool
        self.k = knob_values(self.r, over)
        self.G, self.D = os.path.join(self.build, "gen"), os.path.join(self.build, "data")
        self.default = self.k["DEFAULT_MAP"]
        base = Ctx(self.k, GAME=game, BUILD=self.build, G=self.G, D=self.D, MAP=self.default, BLOCK="",
                   OUT=self.default, TOOLS=TOOLS, SRC=os.path.join(PROTO, "src"), HOST_TAG=host_tag())
        self.blocks = [b.split(":") for b in run_list(self.r, "WORLD_BLOCKS", base, run_tool)]
        self.peers = [self.G] + [os.path.join(self.G, b) for _, b in self.blocks] if self.blocks else []
        base["_words"] = set(self.r.get("knobs", {}))  # a knob given whole as an argument splits into words
        for name, h in self.r.get("host", {}).items():
            base[name.upper()] = base.s(h["out"])
        self.base = base

    def ctx(self, msb=None, block=""):
        """the values of one area / block (msb None: the boot map; the shared and world steps use it too)"""
        msb = msb or self.default
        out = block or msb
        boot = out == self.default
        A = self.G if boot else os.path.join(self.G, out)
        c = Ctx(self.base)
        world = bool(self.blocks) and boot
        split = bool(block) or world
        c.update(MAP=msb, BLOCK=block, OUT=out, A=A, AD=self.D if boot else os.path.join(self.D, out),
                 block=bool(block), world=world, world_split=split,
                 PVS_PEERS=self.peers, PVS_WITH=[p for p in self.peers if os.path.abspath(p) != os.path.abspath(A)],
                 SND_MAPS=sorted({self.default} | {m for m, _ in self.blocks}),
                 MAPENV_LINE=mapenv_line(self.r, msb, block),
                 HZN=os.environ.get("HZN") or "1", HZN_PANO="1" if world and self.k["PANO"] == "1" else "0",
                 RAW="raw:" if block else "")
        c["lite_run"] = self.k["WORLD_SEAM_BUNDLE"] == "1" and bool(block) and split
        tex = [os.path.join(p, "map.bin") for p in self.peers if os.path.basename(p).startswith(msb + "_")] \
            if self.k["WORLD_TEX_SHARED"] == "1" and block else []
        c["TEX_PEERS"] = tex
        c["tex_peers"] = bool(tex)
        return c

    def instance(self, step, c, inst):
        """a recipe step in context c -> Step"""
        env = {k: c.s(v) for k, v in step.get("env", {}).items()}
        if "env_line" in step:
            env = {**dict(w.split("=", 1) for w in c.s(step["env_line"]).split()), **env}
        if step["scope"] in ("area", "chr") and c["BLOCK"]:
            env = {"AREA_BLOCK": c["BLOCK"], **env}
        for k in self.r.get("export", []):
            env.setdefault(k, self.k[k])
        args = c.items(step.get("args", []))
        if step["tool"] == "pack.py":
            args += c.items(step.get("entries", []))
        inputs = c.items(step_inputs(step)) + c.items(step.get("reads", []))
        name = c.s(step["name"])
        if step["scope"] in ("area", "chr"):
            name = f"{c['OUT']}: {name}"
        group = step.get("group") or ("world" if step["scope"] == "world" else "shared" if step["scope"] == "shared"
                                      else c["OUT"] + (" packs" if step.get("stage") == "late" else ""))
        S = Step(name, step["tool"], args, c.items(step["out"]) + c.items(step.get("side", [])) +
                 [os.path.normpath(m) for m in c.items(step.get("maybe", []))], env, inputs, group, step, inst)
        S.maybe = [os.path.normpath(m) for m in c.items(step.get("maybe", []))]
        return S

    def steps(self):
        """every step instance of the default build that its goals need, in recipe order per instance (shared, the
        world blocks, the boot map, the world) with each step after the steps that write its inputs. Enemy types
        are placeholders ("chr", ctx) until expanded (expand_chr: the list comes from the area's enemies.bin export
        tool, which reads the game)."""
        insts = [("shared", self.ctx())] + [(b, self.ctx(m, b)) for m, b in self.blocks] + [(self.default, self.ctx())]
        if self.blocks:
            insts.append(("world", self.ctx()))
        cand = []
        for iname, c in insts:
            scope = "shared" if iname == "shared" else "world" if iname == "world" else "area"
            for step in self.r["step"]:
                if step["scope"] == "chr" and scope == "area":
                    if step is next(s for s in self.r["step"] if s["scope"] == "chr"):
                        cand.append(("chr", c))
                    continue
                if step["scope"] != scope or ("when" in step and not c.cond(step["when"])):
                    continue
                cand.append(self.instance(step, c, iname))
        # what the goals need: the goals and, transitively, the steps writing their inputs
        made = {}
        for s in cand:
            if isinstance(s, Step):
                for o in s.outputs:
                    made[os.path.normpath(o)] = s
        need, todo = set(), [s for s in cand if not isinstance(s, Step) or s.raw.get("goal")]
        while todo:
            s = todo.pop()
            if id(s) in need:
                continue
            need.add(id(s))
            if isinstance(s, Step):
                todo += [made[f] for f in s.need_files() if f in made and made[f] is not s]
        cand = [s for s in cand if id(s) in need]
        # stable topological order: the earliest step whose inputs are all written
        pos = {id(s): i for i, s in enumerate(cand)}
        deps = {id(s): {id(made[f]) for f in s.files() if f in made and made[f] is not s and id(made[f]) in pos}
                if isinstance(s, Step) else set() for s in cand}
        done, order = set(), []
        while len(order) < len(cand):
            for s in cand:
                if id(s) not in done and deps[id(s)] <= done:
                    done.add(id(s))
                    order.append(s)
                    break
            else:
                raise RuntimeError("recipe: a cycle among " + ", ".join(step_name(s) for s in cand if id(s) not in done))
        return order

    def expand_chr(self, c, inst=None):
        """the chr steps of one area / block: one export + pack per enemy type"""
        chrs = run_list(self.r, "ENEMIES", c, self.run_tool)
        out = []
        lod, tex = self.r.get("chr", {}).get("lod", {}), self.r.get("chr", {}).get("tex", {})
        for ch in chrs:
            cc = Ctx(c)
            cc.update(CHR=ch, LOD=lod.get(ch, "1"), TEX=tex.get(ch, "64"))
            out += [self.instance(s, cc, inst or c["OUT"]) for s in self.r["step"] if s["scope"] == "chr"]
        return out

    def host(self, name, log=print):
        """build a host program the way make's rule does (the same command; PVS_GPU probe), unless up to date"""
        h = self.r["host"][name]
        c = self.base
        out = c.s(h["out"])
        srcs = [c.s(s) if s.startswith("{") else os.path.join(TOOLS, s) for s in h["sources"]]
        if os.path.exists(out) and all(os.path.getmtime(s) <= os.path.getmtime(out) for s in srcs):
            return out
        gpu = ""
        if self.k.get("PVS_GPU") == "1":
            ok = subprocess.run(h["gpu_probe"], shell=True, capture_output=True, text=True).stdout.strip()
            gpu = c.s(h["gpu_flags"]) if ok == "1" else ""
        cmd = h["cmd"].replace("{out}", shlex.quote(out)).replace("{GPU}", gpu)
        cmd = Ctx(c, TOOLS=shlex.quote(TOOLS)).s(cmd)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        log(f"  host program {name}: {cmd}")
        subprocess.run(cmd, shell=True, check=True)
        return out


def step_name(s):
    return s.name if isinstance(s, Step) else f"{s[1]['OUT']}: enemy types"


def main():
    a = sys.argv[1:]
    if a[:1] == ["make"]:
        sys.exit(write_make(check="--check" in a))
    if a[:1] == ["format"]:
        for line in open(RECIPE, encoding="utf-8"):
            if not line.startswith("#"):
                break
            print(line[2:].rstrip())
        return
    if a[:1] == ["list"] and len(a) >= 3:
        sys.path.insert(0, TOOLS)
        import convert
        p = Plan(a[1], a[2], convert.run_tool)
        for s in p.steps():
            if "--json" in a:
                print(json.dumps({"name": step_name(s)} if not isinstance(s, Step) else
                                 {"name": s.name, "group": s.group, "tool": s.tool, "args": s.args, "env": s.env,
                                  "outputs": s.outputs, "maybe": s.maybe, "inputs": s.inputs,
                                  "tools": s.raw.get("tools", [])}))
            else:
                print(f"[{s.group if isinstance(s, Step) else s[1]['OUT']}] {step_name(s)}")
        return
    sys.exit(__doc__)


if __name__ == "__main__":
    main()
