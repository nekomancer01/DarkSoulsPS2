"""Where the user's copy of Dark Souls Remastered is installed. Every exporter and the Makefile get the game folder here.

Order: env DSR_GAME, then DSR_GAME in config.local.mk (repo root, not in git), then an auto-detect of common Steam
libraries on Linux (native, flatpak, distrobox host home) and Windows (every Steam library in libraryfolders.vdf).

    python3 tools/gamepath.py            # prints the folder, exit 1 + a message on stderr if none is found
    python3 tools/gamepath.py --quiet    # prints the folder or nothing (config.mk)
    import gamepath; gamepath.game_dir() # raises SystemExit with the same message; warns on stderr when the folder
                                         # is not a supported version (verify_game.warn_unsupported, cached)
"""
import os, re, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
INSTALL_DIR = "DARK SOULS REMASTERED"  # Steam app 570940
EXE = "DarkSoulsRemastered.exe"


def _is_game(d):
    return bool(d) and os.path.isfile(os.path.join(d, EXE)) and os.path.isdir(os.path.join(d, "chr"))


def _steam_roots():
    homes = [os.path.expanduser("~")]
    for v in ("DISTROBOX_HOST_HOME", "USERPROFILE"):
        if os.environ.get(v):
            homes.append(os.environ[v])
    roots = []
    for h in homes:
        roots += [os.path.join(h, ".local/share/Steam"), os.path.join(h, ".steam/steam"), os.path.join(h, ".steam/root"),
                  os.path.join(h, ".var/app/com.valvesoftware.Steam/.local/share/Steam"),
                  os.path.join(h, "snap/steam/common/.local/share/Steam")]
    for v in ("ProgramFiles(x86)", "ProgramFiles"):
        if os.environ.get(v):
            roots.append(os.path.join(os.environ[v], "Steam"))
    roots += ["C:\\Program Files (x86)\\Steam", "C:\\Program Files\\Steam"]
    return roots


def _libraries():
    """Steam library folders: each root plus every "path" in its steamapps/libraryfolders.vdf."""
    seen = []
    for r in _steam_roots():
        vdf = os.path.join(r, "steamapps", "libraryfolders.vdf")
        libs = [r]
        if os.path.isfile(vdf):
            try:
                libs += [p.replace("\\\\", "\\") for p in re.findall(r'"path"\s+"([^"]+)"', open(vdf, errors="replace").read())]
            except OSError:
                pass
        for l in libs:
            l = os.path.normpath(l)
            if l not in seen:
                seen.append(l)
    return seen


def _config_local():
    p = os.path.join(ROOT, "config.local.mk")
    if os.path.isfile(p):
        for line in open(p, errors="replace"):
            m = re.match(r"\s*DSR_GAME\s*[:?]?=\s*(.*?)\s*$", line)
            if m and m.group(1):
                return m.group(1)
    return None


def candidates():
    """(source, folder) in lookup order; the first that holds the game wins."""
    out = []
    if os.environ.get("DSR_GAME"):
        out.append(("env DSR_GAME", os.environ["DSR_GAME"]))
    c = _config_local()
    if c:
        out.append(("config.local.mk", c))
    out += [("Steam library", os.path.join(l, "steamapps", "common", INSTALL_DIR)) for l in _libraries()]
    return out


def find():
    """(folder, source) or (None, reason). An explicit DSR_GAME that is wrong is an error, not a fall-through."""
    for src, d in candidates():
        d = os.path.expanduser(d)
        if _is_game(d):
            return os.path.abspath(d), src
        if src != "Steam library":
            return None, f"{src} = {d!r} does not contain {EXE} and chr/"
    return None, "no Dark Souls Remastered install found in the usual Steam libraries"


HELP = """This project builds from your own copy of Dark Souls Remastered (Steam app 570940); no game data is included.
Point it at the game folder (the one with DarkSoulsRemastered.exe), one of:
  export DSR_GAME="/path/to/steamapps/common/DARK SOULS REMASTERED"
  echo 'DSR_GAME = /path/to/steamapps/common/DARK SOULS REMASTERED' > config.local.mk   (repo root)
  make DSR_GAME="/path/..."
Then check it with: python3 tools/verify_game.py"""


def game_dir(arg=None, verify=True):
    """The game folder: an explicit argument (exporter argv) if it holds the game, else find(); exits with HELP.
    verify: also check the version (tools/verify_game.py, cached per folder; a warning, not an exit)."""
    if arg and _is_game(os.path.expanduser(arg)):
        d = os.path.abspath(os.path.expanduser(arg))
    else:
        d, why = find() if not arg else (None, f"{arg!r} does not contain {EXE} and chr/")
        if not d:
            sys.exit(f"error: {why}\n{HELP}")
    if verify:
        if os.path.dirname(os.path.abspath(__file__)) not in sys.path:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import verify_game  # imports this module: loaded here, not at the top
        verify_game.warn_unsupported(d)
    return d


if __name__ == "__main__":
    d, why = find()
    if d:
        print(d)
    elif "--quiet" not in sys.argv:
        print(f"error: {why}\n{HELP}", file=sys.stderr)
        sys.exit(1)
