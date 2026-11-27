#!/usr/bin/env python3
"""Report-only audit of tools/recipe.toml's per-step `tools` lists: the .py entries that no module the step runs can
reach. A step re-runs when any file in its `tools` list changes (tools/recipe.py stamps them), so an entry the step's
code never loads costs a re-run on every edit of that file for nothing.

For each [[step]] the walk starts at the step's tool and follows, transitively, inside tools/:
  - import / from ... import statements (anywhere in the file, including inside functions and try blocks)
  - importlib.import_module("name") / __import__("name") with a literal name
  - a literal string naming another tools/ .py file ("mapx_native.py", "tools/x.py"), also inside a tools/ subfolder
    the same file names in a string (os.path.join(d, "vu1", "layout.py"), sys.path.insert(0, ".../scan")): modules
    run through subprocess or loaded by path
An entry is a candidate when the walk from the step's tool never reaches it. .c / .cpp / .h files, globs and non-.py
entries are always counted as used (ctypes libraries and their sources). Nothing is removed: the rows are candidates
for a per-step A/B, and a wrong removal would give silent stale cache hits.

Usage: python3 tools/ci/recipe_tools_audit.py [--recipe tools/recipe.toml]
Prints <step>\\t<entry>\\t<reason> per candidate; exit 0 always."""
import argparse
import ast
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def module_file(tools, name, here=None):
    """tools/ path of module `name` (a.b -> a/b.py or a/b/__init__.py), looked up next to the importing file first"""
    rel = name.replace(".", "/")
    for base in ([here] if here else []) + [tools]:
        for cand in (rel + ".py", rel + "/__init__.py"):
            p = os.path.normpath(os.path.join(base, cand))
            if os.path.isfile(p) and p.startswith(tools + os.sep):
                return p
    return None


def refs(path, tools):
    """tools/ .py files `path` can load: imports, import_module / __import__ literals, string mentions of .py files"""
    try:
        tree = ast.parse(open(path, encoding="utf-8", errors="replace").read(), path)
    except (OSError, SyntaxError):
        return set()
    here = os.path.dirname(path)
    out = set()
    # tools/ subfolders the file names in a string (sys.path.insert(0, os.path.join(TOOLS, "scan")),
    # os.path.join(dir, "vu1", "layout.py")): searched for its imports and bare .py names too
    subdirs = [os.path.join(tools, n.value) for n in ast.walk(tree)
               if isinstance(n, ast.Constant) and isinstance(n.value, str) and n.value
               and "/" not in n.value and os.path.isdir(os.path.join(tools, n.value))]

    def add(name, base=None):
        for b in [base or here] + subdirs:
            p = module_file(tools, name, b)
            if p:
                out.add(p)
                return p
        return None

    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for a in node.names:
                parts = a.name.split(".")
                for i in range(1, len(parts) + 1):
                    add(".".join(parts[:i]))
        elif isinstance(node, ast.ImportFrom):
            base = here
            if node.level:
                for _ in range(node.level - 1):
                    base = os.path.dirname(base)
            mod = node.module or ""
            if mod:
                parts = mod.split(".")
                for i in range(1, len(parts) + 1):
                    add(".".join(parts[:i]), base)
            for a in node.names:
                add((mod + "." if mod else "") + a.name, base)
        elif isinstance(node, ast.Call):
            f = node.func
            fname = f.attr if isinstance(f, ast.Attribute) else f.id if isinstance(f, ast.Name) else ""
            if fname in ("import_module", "__import__") and node.args and isinstance(node.args[0], ast.Constant) \
                    and isinstance(node.args[0].value, str):
                add(node.args[0].value)
        elif isinstance(node, ast.Constant) and isinstance(node.value, str):
            s = node.value.strip()
            if s.endswith(".py") and " " not in s and len(s) < 200:
                s = s.removeprefix("../../tools/").removeprefix("tools/").removeprefix("./")
                for base in [here, tools] + subdirs:
                    p = os.path.normpath(os.path.join(base, s))
                    if os.path.isfile(p) and p.startswith(tools + os.sep):
                        out.add(p)
                        break
    out.discard(path)
    return out


def reach(start, tools, memo):
    seen, todo = set(), [start]
    while todo:
        p = todo.pop()
        if p in seen:
            continue
        seen.add(p)
        if p not in memo:
            memo[p] = refs(p, tools)
        todo.extend(memo[p] - seen)
    return seen


def audit(recipe, tools=None):
    """[(step name, entry, reason)] for every .py `tools` entry the step's tool cannot reach"""
    import tomllib
    tools = os.path.abspath(tools or os.path.dirname(os.path.abspath(recipe)))
    with open(recipe, "rb") as f:
        steps = tomllib.load(f).get("step", [])
    memo, out = {}, []
    for s in steps:
        tool = os.path.join(tools, s.get("tool", ""))
        if not os.path.isfile(tool):
            continue
        got = reach(tool, tools, memo)
        for e in s.get("tools", []):
            if not e.endswith(".py") or any(c in e for c in "*?["):
                continue
            p = os.path.normpath(os.path.join(tools, e))
            if p == tool:
                continue
            if not os.path.isfile(p):
                out.append((s["name"], e, "no such file"))
            elif p not in got:
                out.append((s["name"], e, "not imported by any listed module"))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--recipe", default=os.path.join(ROOT, "tools", "recipe.toml"))
    a = ap.parse_args()
    rows = audit(a.recipe)
    for r in rows:
        print("\t".join(r))
    print(f"recipe_tools_audit: {len(rows)} candidates (report only)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
