# Data pipeline

Polaris runs DSR's own data. None of it is in the repository: the build reads the player's copy of Dark Souls
Remastered, converts what the engine needs into PS2-sized files under `build/` (git-ignored), and packs them for the
ELF or the ISO. This page follows the data from the game folder to the runtime.

## Overview

```
game folder (DSR_GAME)
  └─ readers         tools/souls.py, params.py, havok_*.py, esd.py, emevd.py, lua50.py, fev.py, fsb4.py, ffx.py
      └─ exporters   tools/export_*.py and the map tools        → build/gen/<area>/*.bin, *.gd, *.lua0
          └─ packs   tools/pack.py (LZ4, 64 KB blocks)           → build/data/*.pak, build/data/snd/*.pak
              └─ ELF / ISO  make -C ps2/proto [iso], ./build-iso.sh → build/proto.elf, build/proto.iso
                  └─ runtime  pack.c reads host: or cdrom0:, each module parses its own entry
```

The ELF holds no game data. Game values the code needs are loaded at boot from packs, so one ELF built on CI (no game
data) serves every player's own packs (the retired in-browser converter relied on this and only produced the packs, TECH_DEBT
build-23).

## Finding the game

`config.mk` reads `DSR_GAME` (environment) or `config.local.mk` (git-ignored); otherwise `tools/gamepath.py` looks for
a Steam install. `tools/verify_game.py` checks the folder. Never hard-code a path to an install (AGENTS.md section 1).

## Readers

| Tool | Reads |
|---|---|
| `tools/souls.py` | DCX, BND3 / BXF binders, FLVER, TPF, MSB, TAE, param and paramdef files |
| `tools/params.py` | every param table by name, with field names from the game's paramdefs (and Paramdex row names when a checkout is present outside the repo) |
| `tools/havok_anim.py`, `havok_collision.py`, `havok_ragdoll.py` | Havok animations, collision meshes, ragdolls |
| `tools/esd.py` | EzState files |
| `tools/emevd.py` | EMEVD event scripts |
| `tools/lua50.py` | Lua 5.0 compiled chunks in `script/*.luabnd` (disassembler and packer) |
| `tools/fev.py`, `tools/fsb4.py` | FMOD event and sound banks |
| `tools/ffx.py` | FFX particle systems |

`python3 tools/params.py NpcParam 2750` prints a param row: the quickest way to check a value before porting it.

## Exporters and their outputs

`G` is `build/gen` (shared by every area), `A` is `build/gen/<area or block>`.

| Data from DSR | Exporter | Output | Runtime |
|---|---|---|---|
| Player model, skeleton, animations | `export_model.py` | `G/model.bin` | `model.c` |
| Player TAE timelines (a00 movement, weapon set), root motion, `BehaviorParam_PC`, `CharaInitParam`, `EquipParamWeapon` | `export_player.py` | `G/player.gd` | `player.c` through `player_data.h` |
| Weapon dummy polys, attack windows, `AtkParam`, `CalcCorrectGraph`, `ThrowParam`, `KnockBackParam`, `NpcParam` | `export_combat.py` | `G/combat.gd` | `combat.c` through `combat_data.h` |
| Starting class (`CharaInitParam`), humanity zones | `export_progress.py` | `G/progress.gd` | `progress.c` through `progress_data.h` |
| Player ESD (`c0000.esd`), shared enemy ESD (`enemyCommon.esd`) | `export_esd.py` | `G/esd.bin`, `G/enemyCommon.esd.bin` | `esd.c` |
| Enemy and NPC characters: FLVER, textures, animations with TAE windows, hit capsules, ragdoll, one variant per `NpcParam` / `NpcThinkParam` pair placed | `export_chr.py` | `A/c####/model.bin`, `chr.bin` | `enemy.c`, `model.c`, `ragdoll.c` |
| MSB enemy placements, souls and humanity drops, navmesh | `export_enemies.py` | `A/enemies.bin` | `enemy.c`, `nav.c`, `npc.c` |
| AI scripts (`aiCommon`, the map's own) | `lua50.py --pack` | `A/ai_common.lua0`, `A/ai_map.lua0` | `lua0.c`, `ai.c` |
| EMEVD (map and `common`) | `export_emevd.py` | `A/events.bin`, `A/evcommon.bin` | `event.c`, `ev_*.c` |
| Map pieces and static objects, lighting, fog, textures | `export_map.py` and the map tools (`mapchunks.py`, `simplify.py`, `texmap.py`, `horizon.py`, `pano.py`, `farpano.py`, `export_normal.py`) | `A/map.bin`, `cells.bin`, `bump.bin`, `hzn.bin`, `pano.bin`, `far.bin` | `map.c`, `mapstream.c`, `texcache.c`, `bump.c` |
| Draw groups, PVS | `drawgroups.py`, `pvs_bake.c` | `A/dg.bin`, `A/pvs.bin` | `drawgrp.c`, `pvs.c` |
| Havok collision | `export_collision.py` | `A/collision.bin` | `collision.c`, `coltile.c` |
| Breakables and animated objects | `export_objects.py` | `A/objects.bin` | `objects.c`, `physics.c` |
| FFX effects and effect lights | `export_effects.py` | `A/fx.bin` | `fx.c` |
| World block table, links, reach distances | `export_world.py` | `world.pak` | `world.c` |
| Item params, names, icons, descriptions | `export_items.py` | `G/items/` | `items.c`, `menu.c` |
| HUD, menu screens, loading screen, help text | `export_menu.py`, `export_screens.py`, `export_loading.py` | `G/menu.bin`, `G/screens/`, `G/loading.bin`, `G/help.bin` | `hud.c`, `drb.c`, `menu.c`, `loading.c` |
| FMOD banks, TAE sound cues, MSB sound emitters | `export_audio.py` | `build/data/snd/*.pak` | `sound.c`, IOP module |

Areas and world blocks are defined in `tools/area.py` (AREAS, BLOCKS, WORLDS). Exporters take the area or block as
input (`MAP=`, `AREA_BLOCK`); a value tuned on one area goes into that area's override row with a comment saying why.

## How each kind of DSR data reaches the game

**Params.** Read by the exporters, never at runtime. An exporter picks the rows and fields a system needs and writes
them into a game-data record (`.gd`) or into the file of the system that uses them (`chr.bin` holds each enemy
variant's `NpcParam` values, `enemies.bin` the drops). Code cites the param field it reads (for example
`NpcParam.turnVellocity`).

**SpEffects.** Read through the param that points at them. `export_chr.py` follows each enemy attack's
`AtkParam.spEffectId0-4` to the first status `SpEffectParam` row and writes its buildup and effect into that attack
window in `chr.bin`; `status.c` applies it. `export_items.py` follows an item's `refId` to the SpEffect for souls given
and humanity lost on use. A general SpEffect table at runtime (rings, buffs, every effect type) is step 5.8.

**TAE.** The exporters read each animation's TAE events and keep the ones the runtime runs: action flags (event 0:
cancels, invincibility, turning), attack windows with their `AtkParam` rows and dummy polys, the stamina regen rate
(225), root-motion reduction (236), gravity, sound cues and item-use windows. Animations are resampled to 30 fps with
their root motion. A shared TAE runner for every character is step 5.4.

**ESD.** `export_esd.py` packs the whole state machine (groups, states, conditions, commands, the expression
bytecode) unchanged, and `esd.c` runs it. Porting an ESD function means implementing its id in the environment or
command dispatch, not rewriting the state machine.

**EMEVD.** `export_emevd.py` keeps every instruction, resolves entities against the MSB, and marks what the runtime
does not support as unsupported or stub ops (logged, counted in the coverage report: `export_emevd.py --coverage`).

**Lua AI.** `lua50.py --pack` re-serializes DSR's compiled Lua 5.0 chunks (floats instead of doubles, no debug info)
and `lua0.c` runs them. Only the map's own script and the common library are loaded. All Lua, the AI included, runs on this
interpreter, in the ELF and the ISO alike. Translating the bytecode to C at build time was trialled in phase 0 and
dropped: the translated code was 2 to 3 times the bytecode's size and no faster. AI speed work goes into the C side
of the enemy pass instead.

**Maps.** The map exporter places every MSB piece, simplifies it to a per-block triangle budget solved against the
residency design (`src/residency.h`), bakes DSR's light banks into vertex colours, splits the result into cells with
coarse and near levels, and writes prebuilt VIF packets stamped with the VU1 layout hash.

## Generated headers and game-data records

`tools/gamedata.py` lets an exporter declare a record (scalars, arrays, pointers) once and get both:

- the blob, written at export: header `GDAT`, the layout id, the record, what its pointers point to, and a relocation
  table;
- the C header (`src/<name>_data.h`), generated without the game and committed:
  `python3 tools/gamedata.py header combat > ps2/proto/src/combat_data.h`.

The layout id is the CRC-32 of the generated header. `gamedata.c` refuses a blob whose id differs from the ELF's and
names the file, so packs from an older or newer build are caught at boot. `tools/ci/offline.sh` checks that the
committed headers match their declarations. The headers hold layouts only, never values.

## Binary formats

Exported files the runtime reads in place are chunk streams (Polaris 3.3): a 16-byte header (`CHNK`, a four-letter
file id, the total size, the chunk count), chunks of (four-letter id, version, size) with 16-byte aligned payloads,
and a footer that repeats the size. `tools/chunkfile.py` writes and reads them in Python, `tools/native/chunkw.h` writes
them from the C tools (`pvs_bake`, libmapx), and `ps2/proto/src/chunk.c` reads them in the runtime and the Lab.

A loader asks for each chunk by id and the one version it reads. Another version, a missing required chunk, another
file id and a truncated file are refused with a `TEST error: chunk:` line naming the file, the chunk and both
versions, so packs from an older or newer build fail at boot instead of being misread. Bump a chunk's version when
its payload layout changes; a new optional chunk needs no bump. Payloads hold offsets, not pointers: the loaders keep
the chunk's base and read through it (`CHUNK_AT`), which is what lets a 64-bit host build run them (TECH_DEBT core-1).
Files whose payloads hold VIF packets carry the VU1 layout stamp as a `VU1L` chunk, checked by `chunk_vu1_check`
(map.bin keeps its stamp word until phase 4 and goes through the same check).

| File | Id | Chunks |
|---|---|---|
| `world.bin` | `WRLD` | `BLKS` blocks, `LRNG` link ranges, `LINK` links, `REAC` reach table (optional) |
| `enemies.bin` | `ENMY` | `PLAC` placements, `NAV ` navmesh (optional) |
| `items/items.bin`, `itemx.bin` | `ITEM`, `ITMX` | `GOOD`, `AMMO`, `ICMP`; `ROWS`, `REIN`, `NAME` |
| `items/desc.bin`, `icons.bin` (raw, read by offset) | `DESC`, `ICON` | `TEXT`; `IHDR`, `IDS `, `CELL` (64-byte aligned) |
| `menu.bin`, `help.bin` | `MENU`, `HELP` | `MHDR`, `ATLS`, `TEXL` (128-byte aligned), `SPRT`, `GLYP`, `LAYT`, `GRAD`, `STRS`; `HIDX`, `HTXT` |
| `screens/scr_*.bin`, `loading.bin` | `SCRN`, `LOAD` | header, tables, then `CLUT` and `PIXL` (128-byte aligned): the runtime keeps the chunks before `CLUT` |
| `objects.bin` | `OBJS` | `OHDR`, `TYPE`, `PIEC`, `PART`, `ATYP`, `BODY`, `APRT`, `SANM`, `MODL` (128-byte aligned model.bin files), `ATEX`, `BTEX` (texture models; BTEX last, read alone when a block turns on) |
| `dg.bin` | `DGRP` | `DHDR`, `PMSK`, `CMSK`, `CHDG`, `TRDG`, `CHRS`, `ENTS`, `CHEN` |
| `pvs.bin` (written by `pvs_bake.c` with `chunkw.h`) | `PVS ` | `PHDR`, `EOFF`, `ULST`, `LOC `, `KEYS`, `ROWO`, `ROWS` |

The layout of each chunk is documented at the top of the exporter that writes it. map.bin, maptab.bin, mapres.bin,
maptex.bin and cells.bin are chunk files since Polaris 4.8 step 4: `tools/pack.py` wraps them on the way into the pack
(`name=[raw:]chunk:<kind>:<file>`, `tools/mapwrap.py`: a VU1L chunk and one payload chunk holding the exporters' bytes at a
fixed offset), and the loaders take the payload as their base (`src/mapfile.h`; `MAP_CHUNKS=0`: as exported). Formats not yet moved
(model.bin, chr.bin and the others) keep a magic and a version field and relocate pointers in place; they
move with their owner's rewrite (phases 4 and 5).

## Packs

`tools/pack.py` writes packs: a 2048-byte aligned header of entries (name, offset, sizes, block count) and each entry
as LZ4 blocks of 64 KB, so the EE decompresses one batch while the IOP reads the next. Entries marked `raw:` are stored
uncompressed for streaming. Packs start on 32 KB boundaries in the ISO (one DVD ECC block).

| Pack | Holds |
|---|---|
| `c0000.pak` | the player model, the player ESD, `player.gd`, `combat.gd`, `progress.gd` |
| `<area>.pak` / `<block>.pak` | map, cells, relief, horizon and panorama, draw groups, PVS, collision, enemies, objects, effects, the map's AI, events |
| `common.pak` | `enemyCommon` ESD and `aiCommon` Lua |
| `c####.pak` | one enemy type: model and `chr.bin` |
| `menu.pak` | HUD, screens, items, loading screen, help text |
| `world.pak` | the world block table |
| `snd/*.pak` | sound banks per group: `main`, per map, per enemy type |

## Incremental builds

Every export depends on a stamp of the tools it runs (`build/stamp/<tool>.sum`, a hash of the tool's code), so a
comment-only edit or a checkout re-exports nothing. Exports depend on what they take from another exporter, not on
its whole file ("Narrow dependencies" in `ps2/proto/Makefile`): when a new export reads another exporter's code or
data, add that file to its prerequisites. A map re-export takes about four minutes, so start a new build directory
from an existing one: `tools/newbuild "$PWD/build" --from <other tree>/build`.

Build-time variables that change what is exported are registered in `tools/switches.tsv`.

## The export recipe

`tools/recipe.toml` lists every export step once: its tool, arguments, environment, the files it writes, the files it
reads, the `tools/` sources it runs and, for packs, the entries. It also holds every knob an export reads: the
`?=` defaults (PVS bake settings, world split switches, `CHR_BONES`, ...), `DEFAULT_MAP`, the per-area map tuning
(`[mapenv]`, formerly `MAPENV_<area>`) and the per-enemy-type LOD and texture caps (`[chr.lod]`, `[chr.tex]`).
`config.local.mk` and the make command line still override the knobs. Everything reads the recipe:

- **make.** `ps2/proto/export_vars.mk` (knobs) and `export_rules.mk` (one rule per step) are generated from it and
  committed: `python3 tools/recipe.py make` after editing the recipe, CI (`tools/ci/offline.sh`) fails when they are
  stale. The ELF, IOP module, ISO and the world sub-make orchestration (`worldexports`, `worldhzn`, ...) stay
  hand-written in `ps2/proto/Makefile`, with the small "derived" values both sides compute (PVS_PEERS, TEX_PEERS,
  LITE_RUN, ...; their Python twins are `recipe.Plan.ctx`).
- **The converter.** `tools/convert.py` runs the same step instances in one process, ordered by their files, the
  steps the goals need and nothing else (`--compare` checks its packs against make's). It was the engine of the
  in-browser converter, retired for now (TECH_DEBT build-23).
- **Tools that look at steps.** `python3 tools/recipe.py list <game> <build> --json` prints every step instance of
  the default build with its inputs, outputs, tools and environment: the hook for the export cache and the build
  gates.

Adding an export is one `[[step]]` plus its pack entry, then `python3 tools/recipe.py make`. `python3 tools/recipe.py
format` prints the field reference (the comment block at the top of the recipe). Check a change with a dry run of the
whole world build: `make -n -B -C ps2/proto BUILD=<empty dir>` before and after lists the commands of every step.

## Build cache

`tools/buildcache.py` is a content-addressed cache for export results. An entry's key is a SHA-256 of everything
that decides the result: the step's code (its source files, or the bytes of a compiled tool), the environment values
it reads, its arguments and the bytes of its inputs. Paths and modification times are never part of a key, so a hit
is reused by any build dir, any worktree, another machine sharing the directory, or a change that was reverted. The
output is the same bytes with the cache on or off.

| Namespace | Step | Own switch |
|---|---|---|
| `pvs` | occlusion bake per block (`pvs_cache.py` around `pvs_bake`) | `PVS_CACHE` |
| `simplify` | Python simplifier jobs (`simplify_pool.py`, the fallback when libmapx is off) | `SIMPLIFY_CACHE`, cap `SIMPLIFY_CACHE_GB` (10) |
| `snd` | one sound sample's ADPCM encode (`export_audio.py`) | `SND_CACHE` |
| `tex` | one map texture's palette encode (`texmap.py`) | `TEX_CACHE` |
| `step` | every recipe step but packs, whole (`stepcache.py`, below) | `STEPCACHE`, cap `STEPCACHE_GB` (12) |

- **Location.** `<main tree>/build/cache/<namespace>/<key[:2]>/<key>`, shared by every worktree of the checkout.
  `DSR_CACHE` moves the root; a namespace switch set to a path moves that namespace.
- **Size cap.** `BUILDCACHE_GB` per namespace (default 4 GB). Reads refresh an entry's time; past the cap the least
  recently used entries go until the namespace is at 80 %.
- **Bypass.** `BUILDCACHE=0` turns every namespace off (a namespace switch set to `0` turns off just that one).
- **Verify.** `BUILDCACHE_VERIFY=1` recomputes every hit and compares it with the entry. A difference means a key
  misses an input: the step stops with an error naming the namespace and key, also written to `<root>/verify.log`.
- **Inspect.** `python3 tools/buildcache.py stats` (entries and size per namespace), `trim`, `clear <namespace>`.

**Export steps.** The generated make rules (`tools/recipe.py make`) run every recipe step except the packs through
`tools/stepcache.py`. Its key is make's prerequisites (`$^`: the step's inputs, the code stamps of its `tools`, the knob
and area stamps), the bytes of its `reads`, its command line and environment (the recipe's `env` and `env_line`, the
exported knobs, every registered switch that is set), with the build dir, the repo and the game folder written as
placeholders, plus the game version (`tools/verify_game.py`; a copy that is not a supported version is never cached).
A hit writes the step's `out` and `side` files and replays the tool's output; make then carries on as after a run, so
the steps after it hit too. A fresh worktree or build dir with a warm cache rebuilds without running an exporter.
Each step prints `stepcache: hit|miss|verified|off <name>`, and the progress view counts the hits. A step that reads a
file it does not declare (in `inputs` or `reads`) can be restored stale: run a full build with
`BUILDCACHE_VERIFY=1` after changing what a step reads.

A new cached computation inside a tool builds a `buildcache.Cache(namespace, version, sources=..., extra=...,
where=...)` with every file and environment value that changes its output, then calls `cache.cached(key, compute)`.

## Coverage scan and block reports

**Coverage scan** (`tools/scan/scan.py`, Polaris 3.4). Runs every reader over every file of the game once and lists
what it meets per kind: FLVER versions and vertex layouts, MTD blend modes against the material classes, TAE event
types, Havok classes and quantizations, FFX node / action types, EMEVD instructions, ESD commands and env functions per
character kind, MSB part / event / region types, Lua opcodes, param types and DDS formats. Each variant is `ok`,
`unported` (the reader knows it, the port does not run it: the coverage list), `unknown` (the reader does not know
it) or `error` (a reader failure). Readers raise `souls.UnknownVariant` (an AssertionError and a ValueError, so old
handlers still catch it) where they used to assert or guess, and report what they pass over through `souls.skipped()`
(`SOULS_STRICT=1` makes that raise). Results are cached per game file under `build/scan` (about 2 minutes on four
workers cold, seconds cached); `summary.md` holds ids and counts only. The scan fails on an unknown variant that
`tools/scan/known.tsv` does not list; every row there points at a TECH_DEBT row (scan-1..7).

**Block reports** (`tools/blockreport.py`, Polaris 3.6). One report per world block from the export's own outputs:
triangles and bytes per detail level, the hard tier and the block's `residency.h` row against the visual room (from
`tools/world_budget.py`, run unchanged), VRAM per `gs_budget.h` owner, EMEVD classes and packed Lua opcodes, and the
effect coverage of the block's FFX bundle from the scan. `--strict` fails the build on a miss: world_budget's own
verdict, FX or OBJ textures over their rows, or a Lua opcode `lua0.c` lacks (`BLOCKREPORT_STRICT=0` reports only).

Both are world goals of the default build (recipe steps "coverage scan" and "block report", after "world
budget").

## World manifest

**World manifest** (`tools/worldmanifest.py`, Polaris 4.8). One description of every map of Lordran as world blocks,
from the game's own data: the maps and their links from `tools/worldgraph.py` (load lists, MapOffset events,
ConnectCollision parts), every map's h collision counted per 32 m cell, and the MSB's player starts, bonfires and
enemies. Maps with hand-set cuts in `tools/area.py` BLOCKS keep them (pinned); every other map is cut automatically by
the same cell rules (whole 32 m cells, blocks of one MSB tile it, collision under collision.bin's u16 indices: the
occupied rectangle is split along its longer axis at the line that balances the halves, until each is under 60000
triangles and at most 14 cells across, collision.bin's s16 position range; no cut leaves a strip under 3 cells). Per block: cells, collision, enemy roster against `area.MAX_ENEMIES`, spawn (a player start, else a floor
point), its `residency.h` row (or the default row, flagged) and whether a world build holds it; per seam: the blocks of
one MSB whose cells touch and the ConnectCollision links, with the lead `export_world.py` gives them (one cell). Maps
are listed in DSR's progression order with the cells no pinned block covers yet. `worldmanifest.py <game> --propose
<map>` prints a map's automatic cut as BLOCKS rows, to pin it when the area is added. Recipe step "world manifest"
(world goal, `gen/world_manifest.json` + `.md`); `--check` fails the build when a block of the world build has no
RES_BLOCK row or spawn, pinned blocks overlap, or the world has more than WORLD_MAX blocks.

## Budget fit

**Budget fit** (`tools/budgetfit.py`, Polaris 4.8 step 2). The RES_BLOCK rows of `residency.h` (each block's room and
near share, which `export_map.py`'s spot solve sizes the block's levels against) are fitted from the exported blocks:
`world_budget.py` runs in-process over a world build, and every block's hard tier at its own spots (plus the spots of
other blocks it holds a quarter of) gives the factor its row can grow or must shrink by so at most 0.5 % of those spots
stay over the visual room. Cuts take the whole step, raises half of it, a block with any spot over is never raised, and
a row whose last cut did not shrink its ask is held (the export is at its error caps, WS_PX0 / WS_PX1 in `[mapenv]`).
While the world gate fails, the held block with the most spots over also gets coarser error caps (`RES_CAPS` rows in
`residency.h`, x 1.5 per fit up to 36 / 54 px, which `export_map.py` uses instead of `WS_PX0` / `WS_PX1`).
`budgetfit.py <out.json> <boot gen> <block gens...> [--prev <last out.json>] [--report fit.md] [--write]`; re-export and
run it again until it says settled. The build gate is `world_budget.py --strict`, over every block since this step
(`WORLD_BUDGET_STRICT=m10_01_00_00_` = the Burg blocks only, as before).

## Phase 3 will change this

A single recipe file listing every export step (inputs, outputs, tool) will generate the Makefile rules, a shared cache keyed by input hashes, versioned chunk streams, a coverage scan of every reader over
every game file, and report gates that fail the build on a budget miss.
