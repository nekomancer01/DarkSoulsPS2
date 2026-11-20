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
data) serves every player's own packs; the in-browser converter (`tools/pages`, `tools/convert.py`) relies on this
and only produces the packs.

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

The layout of each chunk is documented at the top of the exporter that writes it. Formats not yet moved (map.bin,
cells.bin, model.bin, chr.bin and the others) keep a magic and a version field and relocate pointers in place; they
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

## Phase 3 will change this

A single recipe file listing every export step (inputs, outputs, tool) will generate the Makefile rules and drive the
browser converter, a shared cache keyed by input hashes, versioned chunk streams, a coverage scan of every reader over
every game file, and report gates that fail the build on a budget miss.
