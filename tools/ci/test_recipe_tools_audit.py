#!/usr/bin/env python3
"""tools/ci/recipe_tools_audit.py on a synthetic recipe and module tree (tools/ci/offline.sh step 9): an import chain,
a function-level import, importlib, a .py file run by name, a subfolder module loaded by path, a C entry and a glob are
used; a listed module nothing loads is the one candidate; a missing file is reported.
Exit code = number of failed checks."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recipe_tools_audit  # noqa: E402

FILES = {
    "tool.py": "import a\ndef f():\n    from b import x\nimport importlib\nimportlib.import_module('c')\n"
               "RUN = ['run_me.py']\nL = os.path.join(D, 'sub', 'lay.py')\n",
    "a.py": "import deep\n",
    "deep.py": "",
    "b.py": "x = 1\n",
    "c.py": "",
    "run_me.py": "",
    "sub/lay.py": "",
    "unused.py": "import deep\n",
    "native/k.c": "",
}
RECIPE = '''[[step]]
name = "s"
tool = "tool.py"
tools = ["tool.py", "a.py", "deep.py", "b.py", "c.py", "run_me.py", "sub/lay.py", "unused.py", "native/k.c",
         "mapx/*.c", "gone.py"]
'''


def main():
    with tempfile.TemporaryDirectory() as d:
        for rel, src in FILES.items():
            os.makedirs(os.path.dirname(os.path.join(d, rel)), exist_ok=True)
            open(os.path.join(d, rel), "w").write(src)
        open(os.path.join(d, "recipe.toml"), "w").write(RECIPE)
        got = recipe_tools_audit.audit(os.path.join(d, "recipe.toml"))
    want = [("s", "unused.py", "not imported by any listed module"), ("s", "gone.py", "no such file")]
    ok = sorted(got) == sorted(want)
    print(("ok   " if ok else "FAIL ") + f"audit candidates {got}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
