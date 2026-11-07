# Tool index (2026-10-31)

**What this is:** a list of every tool and script in the repository, grouped by what it's for, with one line on what
each does. Use it to find the right script; most tools print their usage with `--help` or in the comment at the top
of the file. Paths are relative to the repo root.

You only need a few of these to build and play (see `README.md`). Heavy jobs (exports, builds, emulator tests) should
run through the job queue, `tools/q [-m GB] <command>`, which waits for free RAM. (The maintainer's per-task notes,
`notes/threads/`, hold extra usage details; they live in the same checkout but `.gitignore` keeps everything in
`notes/` except the public docs out of git.)

## Game data readers (`tools/`)
| Tool | What it does |
|---|---|
| `souls.py` | DCX, BND3, FMG, PARAMDEF/PARAM, TAE |
| `params.py` | Every param table, with Paramdex row names and field descriptions |
| `havok_anim.py` | Havok skeletons + spline-compressed animations |
| `havok_ragdoll.py` | A character's Havok ragdoll (chrbnd hkx): capsule bodies, joints, the bone each follows |
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
| `simplify_pool.py`, `simplify_native.py` (+ `native/simplify.c`) | export_map's simplify jobs: result cache, worker pool, the C simplifier (same output as simplify.py, `--test`) |
| `export_collision.py` | collision.bin |
| `navwalls.py` | Walls of one collision part that seal a doorway another part leaves open (navmesh crosses them onto another part's floor); export_collision.py drops them (NAVWALLS=0 keeps them). `navwalls.py <map>` lists them |
| `export_model.py`, `export_player.py`, `export_combat.py` | Warrior model + animations, player data, combat data |
| `export_chr.py`, `export_enemies.py` | Enemy / NPC models, placements |
| `export_objects.py` | Breakable objects (pots) |
| `export_esd.py` | ESD runtime data |
| `export_menu.py` | HUD / menu assets |
| `export_items.py` | Item database for the menus: stack sizes / kinds (items.bin), names, stats, reinforcement (itemx.bin), descriptions (desc.bin), 48 px icons (icons.bin) |
| `export_screens.py` | DSR menu screens (scr_<name>.bin): menu.drb dialog trees at their own layout + a sprite atlas each (R00 bar, I02 Equipment, I01 Inventory, C01 Status, F04 System) |
| `export_audio.py` | Sound packs data/snd/{main,mAA,cXXXX}.pak: FMOD banks as SPU2 ADPCM (resident / streamed), TAE sound cues, MSB sound emitters (src/sound.c); encoded samples cached in build/cache/snd (SND_CACHE=0 = off) |
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
| `hostexec` | Runs a command on the host: through `distrobox-host-exec` inside a distrobox, directly otherwise (the PCSX2 and reference-game scripts use it) |
| `test-all.sh` | Build + smoke test + render diff + perf check in one run |
| `test-run.py` | Run one test script headless; stops 5 s after the first EE log error (`--keep-going` runs on), progress line every minute (`--turbo`: no speed limit, for functional runs) |
| `test-smoke.py` | Autotest smoke test |
| `test-gsdump.py` | Graphics regression diff |
| `test-perf.py` | Perf history |
| `test-report.py` | HTML report of a run |
| `test-pcsx2-ini.py` | PCSX2 config for one headless test instance |
| `pine.py` | EE memory by symbol, savestates, test mailbox |
| `pose_check.py` | Offline reference pose for the character |
| `ci/offline.sh` | PR check: VU1 emulator tests, generated `.vsm` up to date, Python compiles (no game data) |
| `ci/hygiene.py` | PR check: no game data, screenshots, binaries or home paths tracked; TECH_DEBT IDs unique |
| `ci/compile.sh` | PR check: compile the engine sources that need no generated header with `-Werror` (ps2dev container) |

## Real hardware and profiling
| Tool | What it does |
|---|---|
| `ps2-iso.sh` | Bootable ISO |
| `build-iso.sh` | One command from the game folder to a playable ISO: downloads the source (or builds this checkout), pulls the ps2dev image and builds in a container on it (`iso-builder/Dockerfile`, Docker or Podman) or with `--engine native`, writes `DarkSoulsPS2.iso` |
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
| `tools/q` | Job queue: `-m` address-space cap, `-r` RAM reservation against a 20 GB budget (default: the measured peak of earlier runs of the same kind of job + 10 %, from notes/jobq-peaks.tsv, which `tools/qpeak.py` fills; without history builds and test-all reserve 8 GB); builds lock their own build dir, so builds of different `BUILD=` dirs run side by side; notes/jobq.log has wait and run time per job |
| `tools/qd` | Delegator: same arguments as `tools/q`, runs the job on whichever machine in `tools/hosts.local` (git-ignored) has room, copying the tree there over rsync for another machine; `--fetch PATH` copies results back, `-H NAME` forces a machine |
| `tools/qstat.py` | This machine's queue state as JSON (budget, reserved, free, MemAvailable, swap, memwatch level, running and waiting jobs); `--fits GB` exits 0 when a job fits now. Used by `tools/qd` |
| `tools/newbuild` | Seed a new build dir (`make BUILD=...`) or a new clone's `build/` (`--from` another tree's build) from an existing build, stamps included, so only what changed re-exports |
| `tools/srcsum.py` | The Makefile's tool stamps: hash of a Python tool's syntax tree without comments / docstrings, so comment edits re-export nothing |
| `tools/later` | Night queue: `later add <cmd>` keeps long jobs (soaks, routes, sweeps) for the idle hours, `later run` starts them through `tools/qd` (exports to the VPS, PCSX2 runs here); `later timer` prints a systemd timer |
| `tools/worktree.sh` | One git worktree per work segment (`wt/<name>`): add, land the changes back, remove |
| `tools/runstamp.py` | Git revision, uncommitted-change hash and ELF / script hashes of a test run (result.json, report) |
| `tools/memwatch.py` | Host memory watchdog (memwatch.service): under memory pressure (PSI, disk swap-ins; zram and page cache don't count) pauses, then kills, the largest job of its own checkout (plus folder names in `DSR_MEMWATCH_TREES`), never other apps; state in `/tmp/memwatch.json` |
| `tools/py` | Runs the exporters' Python (Pillow + numpy) |
| `tools/techdebt-site/build.py` | Builds the tech-debt web page |
| `tools/convert.py` | The default build's packs without make (every export in one Python process; what the in-browser converter runs). `--list` shows the steps, `--compare <dir> build` checks them byte for byte against make's. Mirrors the Makefile's recipes: change it with them. `--lean` removes each intermediate once no later step needs it (the browser's run) |
| `tools/pages/convert-node.mjs` | The in-browser converter's run under Node (same Pyodide, 32-bit numpy, Pillow and wasm C helpers as the page): `node tools/pages/convert-node.mjs <game> <build dir>`, then `tools/convert.py --compare <build dir> build` shows whether the browser's packs are the same bytes as a native build (TECH_DEBT build-7). `--prof F.tsv`: seconds and wasm heap per step (export_map prints the heap per stage); it restarts Pyodide as worker.js does (`--old-rule` for the previous rule) |
| `tools/resample.py` | `resize(img, size, resample)`: Image.resize with the two passes in a fixed order (horizontal, then vertical, premultiplied), the pixels of Pillow <= 12 on every Pillow version (13 orders them per shape) and in the browser. Every export's resize goes through it |
| `tools/octree.py` | Pillow's FASTOCTREE quantizer in numpy with a stable bucket sort: the same palettes natively and in the browser (Pillow's own depends on the platform's qsort). Used by texmap, export_map, export_effects, export_menu |
| `tools/iso9660.py` | Bootable disc image in pure Python, same layout and pack order as `ps2-iso.sh` (no xorriso) |
| `tools/gamedata.py` | Layout of the game values the ELF loads at boot (player.gd / combat.gd / progress.gd) and the generator of `src/*_data.h` |
| `tools/native/build-wasm.sh` | The C helpers as WebAssembly side modules for Pyodide (clang + wasm-ld) |
| `tools/pages/build.py` | Builds the GitHub Pages site (`.github/workflows/pages.yml`): checks the player's game copy in the browser, hands out `build-iso.sh` pinned to the deployed main commit |
