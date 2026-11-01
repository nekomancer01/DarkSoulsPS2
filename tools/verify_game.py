"""Check the user's Dark Souls Remastered copy against the versions this port is tested with.

    python3 tools/verify_game.py [GAME_DIR] [--stamp FILE] [--quick] [--allow-unsupported]

GAME_DIR defaults to tools/gamepath.py (env DSR_GAME, config.local.mk, Steam auto-detect). Exit 0 = supported version,
1 = no game / missing files, 2 = files present but an unknown version (hashes differ). --allow-unsupported (or env
DSR_ALLOW_UNSUPPORTED=1) turns 2 into a warning, for trying a newer patch. --quick hashes only the exe. --stamp writes
the matched version id to FILE (the Makefile uses it so the full check runs once per game folder, not every make).

Supported versions: SUPPORTED below, one entry per known Steam build. Every exporter reads only files in KEY_FILES' folders;
the hashes are of the files whose layout the exporters depend on. To add a version: run with --print, check that the
build boots, and paste the printed entry.
"""
import hashlib, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamepath

# Files the exporters read whose format/content the port is tested against (relative to the game folder).
KEY_FILES = [
    "DarkSoulsRemastered.exe",
    "param/GameParam/GameParam.parambnd.dcx",
    "paramdef/paramdef.paramdefbnd.dcx",
    "chr/c0000.chrbnd.dcx",
    "chr/c0000.anibnd.dcx",
    "chr/c0000.esd.dcx",
    "map/MapStudio/m10_02_00_00.msb",
    "map/MapStudio/m10_01_00_00.msb",
    "map/m10_02_00_00/h10_02_00_00.hkxbhd",
    "script/aiCommon.luabnd.dcx",
    "script/m10_02_00_00.luabnd.dcx",
    "menu/menu.drb.dcx",
    "msg/ENGLISH/item.msgbnd.dcx",
    "msg/ENGLISH/menu.msgbnd.dcx",
    "font/english/DSFont24.ccm.dcx",
]

SUPPORTED = [
    {
        "id": "steam-10943698",
        "name": "Steam, Windows build (depot build id 10943698), English data",
        "files": {
            "DarkSoulsRemastered.exe": "a45aaa36dd2f6cc151670a639ea5547043cf38ea79ff4178b963c6ed71f98d7b",
            "param/GameParam/GameParam.parambnd.dcx": "cc4a81cc87f028534d965c908a8aa7dba2f9e235ac30f5e72ddf6bb45dc7b5ab",
            "paramdef/paramdef.paramdefbnd.dcx": "647bd58afc8f4f30c6d007626bf50fc0e66d9a93f6bf56666e29c0d767e0f6e6",
            "chr/c0000.chrbnd.dcx": "c6667a888bc654706facdca1d7811467c40cd8aa22d839b2c282ec2d0d8153ae",
            "chr/c0000.anibnd.dcx": "b3c90bbbeeb836aaa3e535761188b1562606d160d04d47e1f368a058e2d43137",
            "chr/c0000.esd.dcx": "85fc1eaa382b4e8eec166ca2516c19f34d1ce2c67130d623c48a7aa3431ce0f7",
            "map/MapStudio/m10_02_00_00.msb": "0d1656fe2c8a4bdf37232565abbc936b86b3cbabdb898de299a4576587bac086",
            "map/MapStudio/m10_01_00_00.msb": "d33134ff30f3811d9052d1270731eb843d7d2f333f7d968c895cc1dffed4f1d5",
            "map/m10_02_00_00/h10_02_00_00.hkxbhd": "096b72a3d745c58850edf53a337dfff6286d62636938fdfd69d4f9818271dac7",
            "script/aiCommon.luabnd.dcx": "70dde39542ea46659bbce0a9334fd5fd43453558379fc592c32cb144ed48f3a2",
            "script/m10_02_00_00.luabnd.dcx": "be92913bc75c7e96453ef77638485dcf5da05b9d019e60dd6c573e3e3d1a0044",
            "menu/menu.drb.dcx": "b7c52d98de6fc85d95cde97c3611c43fca91e2992ac5e589a99a91c03310cb96",
            "msg/ENGLISH/item.msgbnd.dcx": "20193b611fdf087da4d7d098f9ff754b0bb59f636519c483ba7960f77cfbf691",
            "msg/ENGLISH/menu.msgbnd.dcx": "0c9d6c7e6a12bf4ad7ab68ec1fd31419651667e7521f52f267c782e85967627b",
            "font/english/DSFont24.ccm.dcx": "86ece63a153405e0f561845874278d1f55ff26d8d714cce5eee8af762271c480",
        },
    },
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    stamp = sys.argv[sys.argv.index("--stamp") + 1] if "--stamp" in sys.argv else None
    if stamp in args:
        args.remove(stamp)
    quick = "--quick" in sys.argv
    allow = "--allow-unsupported" in sys.argv or os.environ.get("DSR_ALLOW_UNSUPPORTED") == "1"
    game = gamepath.game_dir(args[0] if args else None)
    print(f"game: {game}")
    missing = [f for f in KEY_FILES if not os.path.isfile(os.path.join(game, f))]
    if missing:
        sys.exit("error: the game folder is incomplete, missing:\n  " + "\n  ".join(missing) +
                 "\nVerify the install in Steam (Properties > Installed Files > Verify integrity) and try again.")
    files = KEY_FILES[:1] if quick else KEY_FILES
    have = {f: sha256(os.path.join(game, f)) for f in files}
    if "--print" in sys.argv:
        print(json.dumps({"id": "NEW", "name": "describe the version", "files": have}, indent=4))
        return 0
    for v in SUPPORTED:
        bad = [f for f in files if v["files"].get(f) != have[f]]
        if not bad:
            print(f"ok: {v['name']} ({v['id']}){' [exe only]' if quick else ''}")
            if stamp:
                os.makedirs(os.path.dirname(os.path.abspath(stamp)), exist_ok=True)
                open(stamp, "w").write(f"{v['id']}\n{game}\n")
            return 0
    best = min(SUPPORTED, key=lambda v: sum(v["files"].get(f) != have[f] for f in files))
    diff = [f for f in files if best["files"].get(f) != have[f]]
    msg = ("unsupported game version: these files differ from every supported version "
           f"(closest: {best['id']}):\n  " + "\n  ".join(diff) +
           "\nSupported:\n  " + "\n  ".join(f"{v['id']}: {v['name']}" for v in SUPPORTED) +
           "\nExporters may fail or produce wrong data. Set DSR_ALLOW_UNSUPPORTED=1 to build anyway.")
    if allow:
        print("warning: " + msg, file=sys.stderr)
        if stamp:
            os.makedirs(os.path.dirname(os.path.abspath(stamp)), exist_ok=True)
            open(stamp, "w").write(f"unsupported\n{game}\n")
        return 0
    print("error: " + msg, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
