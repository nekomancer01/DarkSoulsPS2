# DarkSoulsPS2

A homebrew port of **Dark Souls Remastered** to the PlayStation 2: a C engine (`ps2/proto`, ps2dev + gsKit,
hand-written VU1 microprograms) that runs in PCSX2 and on real hardware. Python exporters (`tools/`) read your own
copy of the game and convert maps, characters, animations, collision, AI scripts and menus into PS2-sized packs.

Status: a prototype. Firelink Shrine and Undead Burg stream seamlessly with the player, enemies, collision, combat,
event scripts (EMEVD), saves and HUD at 30 fps in PCSX2. `notes/PLAYTHROUGH.md` lists the systems still missing for a
full playthrough.
`notes/ROADMAP.md` lists what is next and `notes/TECH_DEBT.md` lists every known difference from DSR.

## Legal note

This repository contains **no game data**: no textures, models, maps, sounds, text, scripts, params or executable
code from Dark Souls Remastered, and nothing derived from them. It does not contain decompiled code either. You need
your own legally purchased copy of Dark Souls Remastered for PC (Steam). The build reads that copy and writes every
game-derived file into `build/`, which is git-ignored and must never be shared or committed.

Dark Souls is a trademark of FromSoftware / Bandai Namco. This project is not affiliated with or endorsed by them.
Notes on the exe (`notes/EXE_FINDINGS.md`) record facts only: addresses, offsets, field names, constants and
formulas, each with its source.

## Bring your own game

Supported: the Steam PC release, build 10943698, English data (`tools/verify_game.py` lists every supported
version). Tell the build where the game folder is (the folder that holds `DarkSoulsRemastered.exe`), in any one of
these ways:

```
export DSR_GAME="$HOME/.local/share/Steam/steamapps/common/DARK SOULS REMASTERED"
```
or put `DSR_GAME = /path/to/DARK SOULS REMASTERED` in `config.local.mk` at the repo root (git-ignored), or pass
`make DSR_GAME=...`. If none is set, `tools/gamepath.py` looks through the usual Steam libraries on Linux (native,
flatpak, snap) and Windows, including every library listed in `libraryfolders.vdf`.

Check your copy:
```
python3 tools/verify_game.py
```
It hashes the exe and the data files the exporters depend on and stops with a clear message on an unknown version.
`DSR_ALLOW_UNSUPPORTED=1` builds anyway, for trying a newer patch.

## Build

Requirements:
- ps2dev toolchain with gsKit (`ps2/env.sh` expects `$PS2DEV`, default `~/tools/ps2dev-2.0.0/ps2dev`), and `dvp-as`.
- Python 3.10+ with Pillow and numpy for the exporters: `python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt`
  (used automatically when present; otherwise set `PY = /path/to/python` in `config.local.mk`).
- gcc (small native helpers in `tools/native` are compiled on first use).
- PCSX2 2.x (flatpak `net.pcsx2.PCSX2`) with `[EmuCore] HostFs = true`, so the ELF can load its packs over `host:`.

```
source ps2/env.sh
make -C ps2/proto                 # exports from your game into build/, then builds build/proto.elf
make -C ps2/proto MAP=m10_01_00_00  # another area (Undead Burg) -> build/proto_m10_01_00_00.elf
make -C ps2/proto clean           # code only;  distclean: also everything exported from the game
tools/pcsx2-run.sh build/proto.elf               # play in a PCSX2 window
tools/pcsx2-run.sh build/proto.elf 8 shot.png    # headless test run + screenshot
```

Without a game folder, `make` stops right away with instructions; nothing is generated. `build/` layout:
`gen/` (exported .bin files, generated headers), `obj/`, `data/` (the LZ4 packs the ELF loads), `proto*.elf`.
`ps2/proto/proto.elf` and `ps2/proto/data` are local links into `build/`.

Heavy jobs (the full export takes several GB of RAM) go through the job queue: `tools/q [-m GB] <command>`
(the Makefile already runs its exporters through it).

## Optional development dependencies

Not needed to build; used by cross-check and research tools. Fetch them yourself into `ext/` / `tools/ext/`
(git-ignored); check each project's licence before redistributing anything built with them.

| Dependency | Used by | Licence |
|---|---|---|
| soulstruct 2.3.2, soulstruct-havok 1.2.2 (PyPI) + data files from the soulstruct git repo (`ext/soulstruct`) | `tools/xcheck.py` etc. (format cross-checks) | GPL-3.0-or-later |
| SoulsFormatsNEXT, DSAnimStudio (`ext/`) | reference for file layouts | GPL-3.0 |
| Paramdex, DarkScript3 (`ext/`) | param / EMEVD names | see upstream |
| meshoptimizer (`tools/ext/`) | mesh experiments | MIT |
| lz4, lz4ultra (`tools/ext/`) | compressor experiments | BSD-2 (lib) / zlib |
| libimagequant (`tools/ext/`) | palette quantisation experiments | GPL-3.0-or-later or commercial |
| vgmstream (`tools/ext/`) | audio experiments | ISC-style |
| PCSX2 source (`tools/ext/pcsx2-src`) | emulator debugging | GPL-3.0 |
| Ghidra | exe research (`ghidra/scripts`) | Apache-2.0 |

`tools/native/bcdec.h` (BC texture decoder, MIT / Unlicense) is vendored with its licence header.

## Repository layout

| Path | What |
|---|---|
| `ps2/proto/` | the PS2 engine (C99, VU1 `.vsm` generated by `tools/vu1/gen.py`) |
| `tools/` | exporters (`export_*.py`), file readers (`souls.py`, `havok_*.py`, `esd.py`, `lua50.py`), test and reference-rig tools |
| `config.mk`, `tools/gamepath.py`, `tools/verify_game.py` | game path and version check |
| `notes/` | porting notes, exe findings, tech debt, roadmap, playthrough plan, tool index |
| `AGENTS.md` | rules for AI agents and contributors (legal rules in section 1) |

## Licence

GPL-3.0-or-later (`LICENSE`). It covers only the original code and docs in this repository: no game files are included,
and it grants no rights to FromSoftware / Bandai Namco material. You need your own copy of Dark Souls Remastered.
