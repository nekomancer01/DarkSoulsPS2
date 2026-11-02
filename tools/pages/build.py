#!/usr/bin/env python3
"""Build the project's GitHub Pages site (.github/workflows/pages.yml): the "build your own ISO" page.

    python3 -I tools/pages/build.py OUT_DIR [--elf proto.elf]

Writes OUT_DIR/index.html + app.js (copied from tools/pages/), OUT_DIR/build.json (the commit the site was built
from and the supported game versions, i.e. the file hashes in tools/verify_game.py, which the page checks the
player's copy against in the browser) and OUT_DIR/build-iso.sh (tools/build-iso.sh with its default --ref pinned to
that commit, so the page and the script always build the same source) and, with --elf, OUT_DIR/proto.elf: the
game-free ELF from `make -C ps2/proto elf` (CI), which runs with the packs a player converts. No game data is read or written: the site is
code and hashes only (AGENTS.md section 1).
"""
import json, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import verify_game  # noqa: E402


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()


def main():
    args = sys.argv[1:]
    elf = None
    if "--elf" in args:
        i = args.index("--elf")
        elf = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    out = args[0]
    os.makedirs(out, exist_ok=True)
    sha = os.environ.get("GITHUB_SHA") or git("rev-parse", "HEAD")
    repo = os.environ.get("GITHUB_REPOSITORY") or "nekomancer01/DarkSoulsPS2"
    info = {
        "repo": repo,
        "commit": sha,
        "date": git("show", "-s", "--format=%cI", sha),
        "subject": git("show", "-s", "--format=%s", sha),
        "key_files": verify_game.KEY_FILES,
        "supported": verify_game.SUPPORTED,
    }
    with open(os.path.join(out, "build.json"), "w") as f:
        json.dump(info, f, indent=1)
    for name in ("index.html", "app.js"):
        shutil.copy(os.path.join(HERE, name), os.path.join(out, name))
    script = open(os.path.join(ROOT, "tools", "build-iso.sh")).read()
    script, n = re.subn(r"^REF=\$\{DSR_REF:-main\}", f"REF=${{DSR_REF:-{sha}}}", script, count=1, flags=re.M)
    script, m = re.subn(r"^REPO=\$\{DSR_REPO:-[^}]*\}", f"REPO=${{DSR_REPO:-{repo}}}", script, count=1, flags=re.M)
    if not (n and m):
        sys.exit("tools/build-iso.sh: REF= / REPO= default lines not found")
    with open(os.path.join(out, "build-iso.sh"), "w") as f:
        f.write(script)
    os.chmod(os.path.join(out, "build-iso.sh"), 0o755)
    if elf:
        shutil.copy(elf, os.path.join(out, "proto.elf"))
        info["elf"] = {"file": "proto.elf", "size": os.path.getsize(elf)}
        with open(os.path.join(out, "build.json"), "w") as f:
            json.dump(info, f, indent=1)
    open(os.path.join(out, ".nojekyll"), "w").close()
    print(f"site: {out} ({repo} @ {sha[:12]}, {len(verify_game.SUPPORTED)} supported game version(s))")


if __name__ == "__main__":
    main()
