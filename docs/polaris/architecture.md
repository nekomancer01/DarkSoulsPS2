# Architecture

How the engine is put together on `main`: the layers, one frame from input to flip, and the contracts phase 1 put in
place (memory regions, the order table, GS and VRAM ownership, the VU1 memory contract, the 2D writer, determinism).
Every section names the header that holds the authoritative description; when this page and a header disagree, the
header wins and this page needs a fix.

## Layers

Polaris is organised in layers. A layer may call only the layers below it. The source files still sit flat in
`ps2/proto/src`; the module name of each file is the "owner" column of the switch registry (`src/switches.h`,
`tools/ci/switches.py` OWNERS).

| Layer | Module | Role | Files today |
|---|---|---|---|
| 0 Platform | `hal` | EE kernel calls, DMA, VIF and GIF paths, GS registers, timers, pad, memory card, SIF | `gs.c` / `gs.h`, parts of `main.c` |
| 1 Core | `core` | memory regions, cvars, profiler, hitch monitor, logging, math | `mem.c`, `cvar.c`, `prof.c`, `hitch.c`, `vmath.c`, `ticks.h`, `aligncheck.c` |
| 2 IO | `io` + IOP module | packs, async disc and host reads, the disc server, sound RPC | `pack.c`, `iop/dsrsnd/` |
| 3 Graphics | `gfx` | order table, VU1 programs and contract, 2D writer, texture cache, relief, light passes | `ot.c`, `vu1.c`, `vu1_layout.h`, `g2d.c`, `render.c`, `texcache.c`, `bump.c`, `lpass.c`, `gstrace.c` |
| 4 World | `world` | blocks, cells, level-of-detail chain, residency, PVS, draw groups, collision and navmesh | `world.c`, `wblock.c`, `wextra.c`, `wplan.c`, `map.c`, `mapblock.c`, `mapstream.c`, `msplan.c`, `msread.c`, `msevict.c`, `msarena.c`, `residency.h`, `pvs.c`, `drawgrp.c`, `collision.c`, `coltile.c`, `nav.c` |
| 5 Scene | `scene` | skinned models, poses, ragdolls | `model.c`, `ragdoll.c` |
| 6 Game | `game` | DSR's managers: player, enemies, AI, ESD, EMEVD, combat, stats, status, items, flags, progress, objects, bonfire, save | `player.c`, `enemy.c`, `ai.c`, `lua0.c`, `esd.c`, `event.c`, `ev_*.c`, `combat.c`, `chrstat.c`, `status.c`, `items.c`, `flags.c`, `progress.c`, `objects.c`, `physics.c`, `pickup.c`, `npc.c`, `omit.c`, `bonfire.c`, `save.c`, `gamedata.c`, `camera.c` |
| 7 Front end | `ui`, `snd`, `fx`, `cine` | HUD, DRB menus, loading screen, sound, effects | `hud.c`, `menu.c`, `drb.c`, `loading.c`, `sound.c`, `fx.c` |
| Loop | `main` | boot and the frame loop | `main.c`, `frame.c`, `cam_input.c` |
| Test drivers | `tools` | in-ELF test runner, debug overlay | `testscript.c`, `debug_hud.c` |

Some calls still cross the layer rule (for example world code turning enemy and event blocks on and off, and game
code calling sound and effects directly). Each later phase closes the ones in its way, behind narrow headers. When
you add code, don't add a new crossing: put the call where the layer rule allows it, or add a request the higher layer
reads (as `src/ui_req.h` does for UI).

## The frame

`main.c` boots (packs, game data, the world table, the boot block, the player, enemies) and hands a `FrameBoot` to
`frame_run` in `frame.c`, which never returns. Each frame runs these phases in order (`src/frame.h`):

| Phase | Kind | What runs |
|---|---|---|
| `INPUT` | simulate | pad and test-script input, UI input (bonfire, Start menu), the camera's yaw and pitch |
| `SIM_A` | simulate | the player (update, status, hit sweeps, fog-wall push), enemy type streaming, the camera |
| `WORLD` | simulate | block streaming, the loading screen's close, sound, the player's floor and draw groups. Visibility is final after this phase |
| `EMIT_MAP` | emit | the map chain: Z clear, map batches, kick. VU1 draws it while `SIM_B` runs on the EE |
| `SIM_B` | simulate | enemies, objects, events, item pickup, bonfire |
| `EMIT` | emit | characters, effects, the bloodstain, debug world drawing, HUD, menus, the loading overlay |
| `PRESENT` | present | the effects chain kick, the VU1 wait, the frame-region swap, the flip; then test hooks, saves and periodic logs |

**The rule:** simulate phases write no draw packets, and emit phases change no game state. Presentation-only
animation (bar smoothing, fades, loading-screen timers) may step in emit. `frame.c` counts every draw entry point
called outside an emit phase and every update entry point called inside one, and prints them on the `frame:` log
line each hitch period. The `FRAME_STRICT` build turns those into `TEST error` lines with the caller.

`SIM_B` must not read anything `EMIT_MAP` produced; the runtime switch `FRAME_SKIP_MAP` skips the map pass to prove
it. Whatever `SIM_B` changes that the map reads (event-switched map parts, breakables, the player light) shows one
frame late, as it did before the split.

Game code asks the UI for things (a dialog, the boss bar) through `src/ui_req.h`, not by calling the UI directly.
Today the requests forward at once and are counted when made from simulate; the screen stack (step 7.1) will queue
them for the emit phase.

## Memory regions

`src/mem.h` owns how the engine allocates. Phase 1 put every boot-time allocation into named regions and made heap
use during play visible.

| Region | Lifetime | API |
|---|---|---|
| static | boot to power-off: params, item and menu tables, ESD programs | `mem_static(n, align)` |
| boot-temp | boot only: pack staging, block-size tables, the AI pack until the VM copies it | `mem_temp`, `mem_temp_mark`, `mem_temp_reset` |
| frame | one frame: two bump buffers swapped after the previous frame's DMA was waited on | `mem_frame(n, align)`, `mem_frame_swap` |
| pool | fixed-size slots with a generation per slot (the base for handles in step 5.1) | `MemPool`, `mem_pool_alloc`, `mem_pool_free` |
| pages | fixed-size pages over a caller's block, first fit of n contiguous pages | `MemPages` (the streamer's arena becomes one in step 4.1) |
| scratchpad | the EE's 16 KB scratchpad, one claimant at a time | `mem_spr_claim`, `mem_spr_release` |

The engine moves through three states: `MEM_BOOT` until `mem_boot_seal` (static shrinks in place, boot-temp is
released), `MEM_LOAD` while a loading screen is up, `MEM_PLAY` otherwise. The heap wrappers in `hitch.c` count every
`malloc` / `free` per state. Code that still allocates from the heap during play wraps itself in a legacy tag
(`MEM_LEGACY(WORLD) { ... }`; tags in `MEM_TAGS`), and that list only shrinks. An untagged allocation in play is
counted, and the `MEM_STRICT` build prints a `TEST error` with its caller. The `mem:` log line and the debug HUD
show each region's high-water mark.

The budget the regions are checked against: at least 1.5 MB of heap free at the heaviest spot (`AGENTS.md` section
4). The world arena (10,752 KB in 64 KB pages, `src/residency.h`) is the largest single region.

## The order table

`src/ot.h`: everything that reaches the GS in a frame is an entry in the order table, in bucket order, and only the
table starts transfers (VU1 chain kicks, the VU1 / PATH1 wait, the 2D chains, the flip).

Buckets, in the order the GS should receive them: `SKY`, `PANO`, `HORIZON`, `WORLD` (one map segment covers all of
these until step 4.1 splits `map.c`), `CHR`, `OBJ`, `ALPHA_TEST`, `BLEND`, `FX`, `POST` (far depth-of-field bands
and the flip), `UI`.

Entry kinds:

- **segment**: part of a VU1 chain buffer (map, characters, effects), between `ot_open(first, last)` and
  `ot_close()`. Segments of one buffer built back to back go out in one kick.
- **2D**: a `g2d` stream (WORLD or UI), sent at its turn once VU1 and PATH1 are done (`ot_g2d`).
- **deferred**: a function run at its turn (`ot_deferred`), used for the flip with the far depth of field.

Segment rules (in full in `ot.h`): a segment leaves the paths balanced; a segment opened in a buffer that is already
open inherits the program, GS registers and constants of the one before it; a chain buffer is rebuilt only after the
run sent from it was waited on; the table is a fixed array.

`tools/ci/gsown.py` fails CI on a `vu1_begin`, `vu1_kick`, `vu1_wait` or queue-execute call outside `ot.c`, apart
from the listed legacy exceptions. The `ot:` log line counts entries whose bucket is lower than the one before them,
which is where today's draw order and the bucket order still disagree (TECH_DEBT render-18).

## GS state and VRAM

`src/gs.h` names every GS register value the engine writes: A+D register addresses (`GSR_*`), test, alpha, filter,
clamp and TEXA words, FRAME / ZBUF builders, image-transfer headers and the display registers. Code outside `gs.h` /
`gs.c` writes GS state through those names; `tools/ci/gsown.py` fails on a raw register macro or gsKit state call
anywhere else. `map.c` keeps its own literals until it is split, and `gs.c` asserts they equal the named values.

VRAM is charged to an owner. `src/gs_budget.h` has one row per owner, in KB, and the rows plus slack sum to the GS's
4096 KB:

| Owner | Budget | Holds |
|---|---|---|
| frame | 1152 | draw buffer, Z buffer, display buffer |
| world | 1152 | the map texture pool, its CLUTs, block textures, relief and near pools |
| player | 496 | the player's textures (never evicted) |
| characters | 176 | enemies and NPCs |
| objects | 544 | objects and breakables |
| effects | 320 | effect textures |
| UI | 224 | HUD atlases, menu DRB, font |

Each allocation stores its owner (4 bits per 256-byte GS block), so a free credits the owner that paid. Today an
owner over its row logs a `TEST warn` that `tools/budget.py` counts; step 2.7 turns that into a refusal. The loading
screen borrows its VRAM (336 KB) from the map pool and must give it back after each load. Running out of the 4096 KB
stays a hard failure ("Not enough VRAM").

## The VU1 memory contract

All VU1 programs share one memory layout, defined in `tools/vu1/layout.py` and generated into
`src/vu1_layout.h` (`python3 tools/vu1/gen.py layout`). Never edit the header or the `.vsm` files by hand.

- **Data memory** (1024 quadwords): camera and fog constants at 0 to 7 for every chain kind, then per chain kind
  (map, skin, effects) an input double buffer (VIF `BASE` / `OFFSET`), output slots, clip planes and the bone
  window.
- **Micro memory** (2048 instruction pairs): one row per program variant in `VU1_PROGRAMS(X)`: name, code symbol,
  address and chain bank. Today: `MAP`, `MAP2`, `CLIP`, `SKIN2`, `SKINCLIP`, `FX`, `MAPI`.
- **Layout hash**: `python3 tools/vu1/layout.py --hash` gives a hash the exporters stamp into every file that carries
  VIF packets (map chunks, horizons), so a packet built for an older layout is caught at load.
- `python3 tools/vu1/layout.py` prints the free space per chain kind and per micro-memory bank.

`tools/ci/offline.sh` checks that the generated header and programs match `layout.py`, and runs the VU1 emulator
tests in `tools/vu1/`.

## The 2D writer

`src/g2d.h` replaces gsKit's draw queue. It has the same calls as the gsKit primitives it replaces (sprites,
textured sprites, triangles, quads, lines, clears) and draws the same pixels; the step 1.6 gate compared the decoded
draws (`src/gstrace.c`).

Streams: `G2D_WORLD` (the EE-path triangles of map and effects, debug lines; buckets `BLEND` to `FX`), `G2D_UI` (HUD,
menus, dialogs, text, the debug overlay; bucket `UI`), and `G2D_LOAD` (the loading screen's own pair, flushed by
`loading.c`). Pick a stream with `g2d_select`; the order table sends it at its turn. The writer caches TEST, ALPHA,
CLAMP and TEX1 and skips unchanged writes; the cache is cleared at every stream start and after raw reservations
(`g2d_raw`). A full stream cuts the rest of its primitives and prints a `TEST error`.

Textured UI sprites are alpha-tested (`UI_ATEST`, on by default) so transparent texels write no depth and the far
depth of field blurs the scene under them.

## Determinism

Tests and the Lab need the same input to give the same simulation. `src/frame.h` provides three test-script options:

| Option | Effect |
|---|---|
| `option fixeddt MS` | every frame's dt is exactly MS milliseconds; frame pacing stays real |
| `option seed N` | `rand()` seeded with N (the loading screen's hint pick too) |
| `option record NAME` | each frame's input after the pad and test layers, plus dt, frame number and camera, to `build/save/NAME`; its simulate state (position, yaw, ESD state) to `NAME.state`; enemies to `NAME.enemies` |

With `fixeddt`, `frame_clock()` counts frames instead of the EE timer, so everything simulate reads about time is
deterministic. Work budgets that cut a job short (the Lua collector step, event instance runs) read
`frame_budget_clock()`, which with `fixeddt` advances by the work callers report through `frame_det_work`, so a
budget cuts at the same point in every run.

Rules that follow for new code:

- Measure gameplay time with `frame_clock()` or the frame's dt, never with `ticks()`.
- A per-frame work budget reads `frame_budget_clock()` and reports its work with `frame_det_work`.
- Random numbers come from `rand()` (seeded by the test).
- What depends on the disc (a block or an enemy model arriving) stays as non-deterministic as the drive: record
  traces at spots where everything needed is resident, after a settle wait.

Trace format version 3 is described in `ps2/proto/lab/README.md`.

## Switches and runtime variables

Every compile-time switch the C sources test is a row in `src/switches.h` (default, kind, the build CI makes of it,
owner module, adding PR). Build-time variables (Makefile, exporter environment) are rows in `tools/switches.tsv`.
`tools/ci/switches.py` fails CI on an unregistered name; `tools/ci/switches.sh` builds one ELF per fallback and
debug switch, so an old path that stops compiling fails the PR.

A switch tested by plain C `if`s can also be a runtime variable: a row in `CVAR_LIST` in `src/cvar.h`, read as
`cv_NAME`. A test script's `option set NAME VALUE` (or `tools/test-run.py --set NAME=VALUE`) flips it at boot, so one
ELF or ISO serves both sides of an A/B test.

Kinds: `fallback` (the other value builds the old path; new paths are always on by default), `debug` (test and
measurement builds), `tune` (a number), `make` (set by the Makefile), `host` (host harnesses only).

## Logging

The EE log is the engine's instrument panel. Lines worth knowing:

| Prefix | From | What |
|---|---|---|
| `hitch:` | `hitch.c` | each period: worst frame with its phase split, frames over 50 / 100 / 200 ms, free heap, arena state, streaming churn |
| `spike:` | `hitch.c` | a single slow frame outside a loading screen, with where the player stood |
| `alloc:` | `hitch.c` | a failed allocation, with the caller's return address |
| `mem:` | `mem.c` | allocations per state and legacy tag, region high-water marks |
| `frame:` | `frame.c` | phase-rule violations per entry point |
| `ot:` | `ot.c` | entries, out-of-order buckets, inherited segments |
| `g2d:` | `g2d.c` | peak bytes per 2D stream, cut primitives |
| `gsvram:` | `gs.c` | VRAM per owner against its budget |
| `cvar:` | `cvar.c` | runtime switches that differ from their default |
| `TEST error` / `TEST warn` | anywhere | failures and warnings the test tools count |

`tools/test-run.py` stops a run shortly after the first error in the EE log (`--keep-going` runs on), and `tools/hitchsum.py`
summarises the `hitch:` lines of a long run.
