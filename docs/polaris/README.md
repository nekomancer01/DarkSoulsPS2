# Polaris: the engine

Polaris is the name of the engine in `ps2/proto`: the C99 runtime for the PS2 (ps2dev, gsKit, VU1 microprograms and
our own IOP module) plus the Python exporters in `tools/` that turn the player's own copy of Dark Souls Remastered
into PS2-sized packs. It is being rebuilt in place, one subsystem at a time, so it stays a working game at every
merge. These pages describe what is on `main` today (phase 1 complete) and say where the plan goes next.

The idea in one paragraph: the game's **content and rules come from DSR's own data**, read at build time from the
player's copy (params, SpEffects, TAE timelines, ESD state machines, EMEVD event scripts, Lua AI, MSB maps, FLVER
models, Havok collision and animation, FMOD banks, DRB menus). The **core systems that run that data are written
for the PS2 by hand**: damage, physics, streaming, rendering, sound, the frame loop and memory. Where DSR has a
manager (ChrIns, the param tables, the ESD and EMEVD runtimes, TAE events, the AI goal system), Polaris has a module
of the same shape, so every finding from the exe has one obvious home.

## Pages

| Page | What it covers |
|---|---|
| [architecture.md](architecture.md) | Layers, the frame in phases, memory regions, the order table, GS and VRAM ownership, the VU1 memory contract, the 2D writer, determinism |
| [subsystems.md](subsystems.md) | Every runtime module by layer: what it does, its files, its data, and what is missing |
| [data-pipeline.md](data-pipeline.md) | From the game folder to the disc: exporters, generated headers, packs, and how each kind of DSR data reaches the runtime |
| [build-and-test.md](build-and-test.md) | Building the ELF and ISO, test scripts, PCSX2 runs, the Lab, deterministic traces, graphics gates, CI, switches |
| [adding-a-mechanic.md](adding-a-mechanic.md) | A worked path for porting one DSR mechanic, from the exe to a merged PR |

Read `AGENTS.md` first if you are going to change anything: its legal rules (no game data or decompiled code in the
repository) apply to every page here.

## Principles

1. **Rewrite in place.** Each step replaces one module behind a stable interface, ships on by default in the ISO
   build, and keeps the old path behind one fallback switch until the new one has passed its gate. There is no
   second engine tree.
2. **C99 and the existing toolchain.** ps2dev, gsKit, the VU1 generator `tools/vu1/gen.py`, our IOP module
   `ps2/proto/iop/dsrsnd`. Nothing needs C++.
3. **30 fps on a real PS2.** The frame is split into simulate and emit phases so VU1 and the GS draw one part of the
   frame while the EE simulates the next. PCSX2 does not time VU1, GS fill or the EE cache, so every performance
   claim says whether it still needs hardware to confirm.
4. **DSR's data, not ours.** Values come from params, the exe or the game files. A guess is marked `ASSUMED` in a
   comment and logged in `notes/TECH_DEBT.md`.
5. **Whole game, not one area.** Exporters and runtime systems take an area or character ID. Firelink Shrine is the
   test area; Undead Burg and the Asylum are the second checks.
6. **Script first, C second** (planned for phase 5). New game-layer systems may start as Lua on the engine's VM or
   as Python in the Lab, then move to C when they are settled or over budget.

## The roadmap at a glance

Each numbered step is about one PR, on by default with one fallback switch. Phases 0 and 1 are merged.

| Phase | Content | State on `main` |
|---|---|---|
| 0 Ground | Memory and frame census, switch registry, golden routes, module boundary map, Lua translation trial (translation dropped: the trial's C was 2 to 3 times the bytecode's size and no faster) | Merged: census counters (`PROFILE` / `HEAPMAP` builds), switch registry (`src/switches.h`, `tools/switches.tsv`, CI checks) |
| 1 Frame | GS and VRAM ownership, memory regions, simulate and emit phases, order table, VU1 memory contract, own 2D writer, the Lab | Merged (steps 1.1 to 1.7): see [architecture.md](architecture.md) |
| 2 Renderer | Material table from MTD, prebuilt character packets, env and specular, character lights, shadows, effects on VU1, VRAM enforcement and mips | In progress |
| 3 Pipeline | One recipe file for every export step, a shared cache, versioned chunk streams, coverage scan, browser converter on the recipe, report gates | Planned |
| 4 World and disc | Split of `world.c` / `map.c` / `mapstream.c`, one level chain, streaming sources, DSR's read priority, IOP split, disc alignment, whole-game manifest | Planned |
| 5 Characters and scripting | Handles, one character table, shared animation / TAE / control / hit, live Lua with hot reload, SpEffect system, full damage and poise | Planned |
| 6 Parity | Throws, bullets and magic, part assembly, human NPCs, EMEVD op table, talk ESD and shops, objects, bosses, a vertical slice from the Asylum to the Bell Gargoyles | Planned |
| 7 Front end | Screen stack, the missing menus, HUD, audio quality, effects coverage, cutscenes and movies, background saves | Planned |
| 8 Hardware and whole game | Console loop, display modes, disc robustness, every area exported and smoke-tested | Planned |

Step numbers (for example "Polaris 1.4") appear in source comments, switch rows and commit messages, so `grep -rn
'Polaris 1.4' ps2/proto` finds the code a step touched.

## Status in numbers

Rough figures for orientation; the census builds give exact ones (see [build-and-test.md](build-and-test.md)).

| What | Value |
|---|---|
| Engine source | about 130 C files and headers in `ps2/proto/src`, plus 7 generated VU1 programs |
| Frame buffer | 512 x 448, 16-bit colour and depth, two depth ranges split at 50 m |
| EE memory floor | 1.5 MB of heap must stay free at the heaviest spot |
| World arena | 10,752 KB in 64 KB pages (`src/residency.h`) |
| VRAM | 4096 KB split by owner (`src/gs_budget.h`) |
| Areas | 14 world blocks (`tools/area.py` WORLDS: Firelink Shrine, Undead Burg, the Undead Asylum and the edges of the maps next to them) stream as one world |
| Real hardware | not yet run (TECH_DEBT hw-1) |
