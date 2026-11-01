# Tool index (2026-10-31)

**What this is:** a list of every tool and script in the repository, grouped by what it's for, with one line on what
each does. Use it to find the right script; most tools print their usage with `--help` or in the comment at the top
of the file. Paths are relative to the repo root.

You only need a few of these to build and play (see `README.md`). Heavy jobs (exports, builds, emulator tests) should
run through the job queue, `tools/q [-m GB] <command>`, which waits for free RAM. (The maintainer's per-task notes,
`notes/threads/`, hold extra usage details; they're local and not in the public repo.)

## Game data readers (`tools/`)
| Tool | What it does |
|---|---|
| `souls.py` | DCX, BND3, FMG, PARAMDEF/PARAM, TAE |
| `params.py` | Every param table, with Paramdex row names and field descriptions |
| `havok_anim.py` | Havok skeletons + spline-compressed animations |
| `havok_collision.py` | Map collision (hkx) -> world-space triangles |
| `esd.py` | EzState (.esd) reader + expression decompiler |
| `lua50.py`, `lua50_pseudo.py` | Lua 5.0 bytecode disassembler, pseudo-code |
| `emevd.py` | Event scripts (EMEVD), decoded with the game's instruction definitions |
| `worldgraph.py` | Which maps load together / connect (level streaming) |
| `dsr_points.py` | FromSoft autotest point lists |
| `dsr_light.py` | Map lighting, fog, tone map decoded for baking |
| `fev.py`, `fsb4.py` | FMOD event projects (FEV1), sample banks (FSB4) |
| `ffx.py` | DSR effects (FXR1 in sfx/*.ffxbnd): nodes, states, actions, particle data, curves; `--check` |
| `xcheck.py` | Cross-check of the readers against soulstruct |

## Exporters (game files -> PS2 data)
| Tool | Output |
|---|---|
| `export_map.py` (+ `simplify.py`, `texmap.py`) | Map geometry, textures, lighting bake |
| `export_collision.py` | collision.bin |
| `export_model.py`, `export_player.py`, `export_combat.py` | Warrior model + animations, player data, combat data |
| `export_chr.py`, `export_enemies.py` | Enemy / NPC models, placements |
| `export_objects.py` | Breakable objects (pots) |
| `export_esd.py` | ESD runtime data |
| `export_menu.py` | HUD / menu assets |
| `export_audio.py` | Per-area sound banks (prototype) |
| `export_effects.py` | fx.bin: bonfire / MSB SFX particle systems, effect lights, textures; static lights for the map bake |
| `pack.py` | LZ4 area packs (.pak) |
| `area.py`, `gamepath.py`, `verify_game.py` | Per-area selection, game folder, game version check |

## Native libraries (`tools/` subfolders)
| Folder | Contents |
|---|---|
| `meshopt/` | meshoptimizer bindings |
| `liq/` | libimagequant colour quantisation |
| `lz4hc/` | LZ4 high-compression packing |
| `native/` | BC7 decode, LZ4 packer |
| `adpcm/` | PS2 ADPCM (VAG) audio encoder |
| `vgmstream/` | Audio decoding |
| `vu1/` | VU1 microprogram generator (`gen.py`), VU emulator (`vuemu.py`) + unit tests, A/B and reference runs |

## Running and testing the PS2 build
| Tool | What it does |
|---|---|
| `pcsx2-run.sh` | Boot an ELF in PCSX2: headless + screenshot, or a visible window |
| `test-all.sh` | Build + smoke test + render diff + perf check in one run |
| `test-run.py` | Run one test script headless |
| `test-smoke.py` | Autotest smoke test |
| `test-gsdump.py` | Graphics regression diff |
| `test-perf.py` | Perf history |
| `test-report.py` | HTML report of a run |
| `test-pcsx2-ini.py` | PCSX2 config for one headless test instance |
| `pine.py` | EE memory by symbol, savestates, test mailbox |
| `pose_check.py` | Offline reference pose for the character |
| `winshot.py` | Window screenshot |
| `ci/offline.sh` | PR check: VU1 emulator tests, generated `.vsm` up to date, Python compiles (no game data) |
| `ci/hygiene.py` | PR check: no game data, screenshots, binaries or home paths tracked; TECH_DEBT IDs unique |
| `ci/compile.sh` | PR check: compile the engine sources that need no generated header with `-Werror` (ps2dev container) |

## Real hardware and profiling
| Tool | What it does |
|---|---|
| `ps2-iso.sh` | Bootable ISO |
| `ps2-pcsx2.sh` | Debugger / savestate runs |
| `ps2-run.sh` | Real PS2 over ps2link (untested on hardware) |
| `ps2-mc.sh` | Memory card images |
| `ps2-gprof.sh` | gprof profile |

## Reference capture from the real game (duplicate install)
| Tool | What it does |
|---|---|
| `dsr-ref.sh` | Start, load, camera, savestates, screenshots |
| `dsr-ref-cam.py` | Precise camera placement |
| `dsr_mem.py` | Read / write game memory (position, warp, HP) |
| `dsr-ei.c` | Input injection |
| `dsr-rdc.sh` + `dsr_rdc.py`, `dsr_rdc_replay.py` | RenderDoc capture -> named shader constants |

## Exe research
| Tool | What it does |
|---|---|
| `ghidra/scripts/` | 11 Ghidra scripts: DecompileList, DecompileAt, VtableSlots, FindDisp, FindOperand, RefsTo, JpRefs, ExportFunctions, ParamFieldReaders, Peek, DsrFastAnalysis |
| `fndb/fndb.py` | Function database + code search |
| `tools/exe-dearxan/` | Arxan decryption (route blocked) |
| `tools/exe-vtable.py` | MSVC RTTI / vtable lookup |

## Infrastructure
| Tool | What it does |
|---|---|
| `tools/q` | Job queue with memory caps |
| `tools/memwatch.py` | Host memory watchdog (memwatch.service) |
| `tools/py` | Runs the exporters' Python (Pillow + numpy) |
| `progress.sh` | Status of long background jobs |
| `notes/oss/check.sh` | Pre-commit check for game-derived / personal files |
| `tools/techdebt-site/build.py` | Builds the tech-debt web page |
