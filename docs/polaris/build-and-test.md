# Build and test

How to build Polaris, run it, and prove a change works: in PCSX2 with scripted tests, in the Lab on the host, and in
the CI checks every pull request runs. `README.md` covers first-time setup (toolchain, game folder, Python packages,
PCSX2 HostFs); this page assumes that is done.

Heavy jobs (make, exporters, PCSX2 runs) go through the job queue when several agents share a machine:
`tools/q [-m GB] <command>` (AGENTS.md section 5).

## Building

```bash
source ps2/env.sh
make -C ps2/proto                        # exports what changed, builds build/proto.elf (boots at Firelink)
make -C ps2/proto MAP=m10_01_00_00       # another boot area: build/proto_m10_01_00_00.elf
make -C ps2/proto iso                    # build/proto.iso
./build-iso.sh                           # everything in one command, with a progress view; --help for options
```

`build/` holds `gen/` (exports and generated headers), `obj/`, `data/` (the packs) and the ELFs. Builds are
incremental: exports re-run only when their tool's code or their inputs change (see
[data-pipeline.md](data-pipeline.md#incremental-builds)). A new tree starts from an existing build with
`tools/newbuild "$PWD/build" --from <other tree>/build`.

Build variants:

| Command | What it adds |
|---|---|
| `make PROFILE=1` | per-pass EE cycles and cache misses on the HUD and in the log (`proto_prof.elf`) |
| `make HEAPMAP=1` | live heap by caller in the log, for `tools/heapmap.py` |
| `make DEFS='-DMEM_STRICT'` | `TEST error` on any untagged heap allocation during play |
| `make DEFS='-DFRAME_STRICT'` | `TEST error` on a draw call in a simulate phase or an update call in an emit phase |
| `make DEFS='-DOT_HASH'` | `othash:` lines: hashes of everything sent to the GS on frozen test frames |
| `make DEFS='-DNAME=0'` | the old path of a fallback switch (`src/switches.h`) |
| `make sym` | `proto.sym.elf` with symbols, for `addr2line` on `alloc:` and `spike:` lines |

## Running

```bash
tools/pcsx2-run.sh build/proto.elf               # play in PCSX2 (HostFs)
tools/pcsx2-run.sh build/proto.elf 8 shot.png    # headless: run 8 s, save a screenshot
./play-report.sh                                 # play the ISO and record a report zip (log, shots, marks)
```

Headless runs use their own virtual display and data directory and kill only their own PCSX2 instance.

## Test scripts

A test is a plain-text script in `ps2/proto/tests/` run by the ELF's own interpreter (`src/testscript.c`), so a new
test needs no rebuild. The header of `testscript.h` lists every command. The essentials:

```
# what this test checks, and why these spots
option noenemies                # boot options first: noenemies, hud, holes, god, demo, save host, set NAME VALUE
option fixeddt 33.333           # determinism (below)
wait 3
spawn -47.41 -59.82 53.3 5.2185 # onto the collision floor under x y z, facing yaw
cam 5.2185 0.1                  # camera yaw and pitch held
stick 0 1                       # hold the left stick forward
wait 2
pad circle                      # one button press
check near -40 -59 60 2         # fails the run unless the player is within 2 m
check flag 11010000 1
shot bonfire_fx                 # freeze, screenshot, log the segment's stats
end
```

Run one script headless and collect its results:

```bash
tools/test-run.py build/proto.elf ps2/proto/tests/ev_door.txt build/test/ev_door
tools/test-run.py build/proto.elf ps2/proto/tests/iso_burg_taurus.txt out --iso build/proto.iso
tools/test-run.py build/proto.elf tests/x.txt out --set WORLD_REACH=0     # A/B a runtime switch, same ELF
```

`result.json` holds every shot's stats (fps, frame ms, RAM and VRAM free, chain peaks), errors and the log lines;
`emulog.txt` is the EE log. The run fails on a `TEST error`, a failed check, a hang or a crash, and stops shortly
after the first error unless `--keep-going`. Runs are unthrottled by default; ISO runs warm-start from a cached
savestate parked at the script's first spawn (`--cold` boots every time). `--hitch` gates on the hitch monitor.

Test names say what they cover: `ev_*` events, `world_*` streaming and the world, `iso_*` routes booted from the disc,
`det_*` deterministic traces, `ot*_check` graphics gates, `save_*` saves, `fx_*` effects, `block_*` world blocks.

## Suites

```bash
tools/test-all.sh --no-build --areas m10_02_00_00,m10_01_00_00 --max 20   # the scoped PR run
tools/test-all.sh                                                          # build + full suite, every area
tools/test-all.sh --iso --burg                                             # the Burg gate on the ISO
```

`test-all.sh` builds, runs the smoke test (DSR's own autotest points per area: crash, hang, holes, low fps), compares
renders with the previous run and the accepted baseline (`--accept` sets a new baseline), appends perf history and
checks budgets (`tools/budget.py`). The `--iso --burg` run boots the ISO on the Burg routes and prints one budget
table: passes per shot, frames over 34 ms, RAM and VRAM, disc and streaming gates. Results land in
`build/test/runs/<time>/` with a `report.html`.

Budgets every change must hold: 30 fps in every area, at least 1.5 MB of EE RAM free at the heaviest spot, no
"Not enough VRAM" in the log. Report numbers from before and after. PCSX2 does not time VU1, GS fill or the EE cache,
so say when a gain can only be confirmed on a PS2.

Map changes can be judged without PCSX2: `tools/mapshot.py` draws exported blocks from the port's camera with the
levels the runtime would show (AGENTS.md section 5).

## Deterministic traces

Three script options make a run repeatable (details in [architecture.md](architecture.md#determinism)):
`option fixeddt 33.333`, `option seed N`, and `option record NAME`, which writes the input trace to
`build/save/NAME` and the state log to `NAME.state` (and `NAME.enemies` with enemies on).

The gate traces live in `ps2/proto/tests/det_*.txt`:

| Script | Covers |
|---|---|
| `det_walk.txt`, `det_walk_open.txt` | Firelink walks with a roll and turns: past breakables, and across open ground |
| `det_burg.txt` | the Burg's west stairs with enemies (two runs agree unless an enemy's model arrives later) |
| `det_burg2.txt` | the Burg's west stairs without enemies, with frames where the world held the player while a tile loaded |
| `det_burg3.txt` | Burg enemies below the Taurus tower: ESD, AI, action and HP sequences per enemy |
| `det_seam.txt`, `det_seam_chr.txt` | the Firelink aqueduct into the Burg: draw-group state per frame, without and with enemies |

Two runs of one build must give the same state log line for line. Record traces at spots where everything needed is
resident, after a settle wait: disc timing is not deterministic.

## The Lab

The Lab (`ps2/proto/lab/`, full reference in its `README.md`) is the simulate phase built as a 32-bit host program
and driven from Python (`tools/lab.py`). It runs the game's own code for game data, flags, progress, stats, items,
saves, the player (ESD, combat, physics, status), the navmesh, the static world (blocks, collision, breakables,
events, pickups, the bonfire), enemies with their Lua AI, and draw groups. It leaves out sound, drawing, menus and
the streaming waits: every read completes at once.

```bash
make -C ps2/proto lab                     # build/host/lab; needs a host gcc with -m32
python3 tools/lab.py compare build/save/det_walk.trace build/save/det_walk.trace.state --short
python3 tools/lab.py run ps2/proto/tests/save_write.txt ps2/proto/tests/save_check.txt
python3 tools/lab.py run ps2/proto/tests/det_walk_open.txt --state build/save/det_walk_open.trace.state
```

- **`compare`** replays a PCSX2 trace in the Lab and compares each frame with the console's state log: the same
  state sequence and every frame within 1 cm for short traces; for version 3 traces, the same ESD, AI, action and HP
  sequence per enemy and the same draw-group state hash per frame.
- **`run`** runs test scripts through the game's own interpreter and prints pass or fail, the checks that ran, the
  checks it cannot run (what is drawn), and any stub whose result the simulation reads back (those mark places where
  simulate still depends on presentation). About a hundred scripts take seconds with `-j 4`.
- From Python: `Lab("build", "m10_02_00_00")` gives `cmd()` (test-script commands) and `step(n)` (frames, returning
  position, yaw and state), for prototyping a rule against real exported data.

`tools/ci/lab.sh` builds the Lab and runs its gate traces and scripts where exported data exists. Floating point is
SSE scalar without fast-math on the host; the first traces agree with the console within 0.04 cm.

## Graphics gates

Behaviour-preserving changes to drawing (order, packet layout, file splits) gate on identical GS output:

1. Build both versions with `make DEFS='-DOT_HASH'`.
2. Run `ps2/proto/tests/ot_check.txt` (VU1 streams on frozen shots: the bonfire's particles, the Start menu over the
   HUD, far walls past the depth split, a block turning on) and `ot2d_check.txt` (the 2D layer: HUD, debug overlay,
   Start and bonfire menus, the item-get dialog) with `tools/test-run.py`.
3. Compare the `othash:` lines of the two EE logs. They hash the VIF1 stream per kick and the decoded 2D draws
   (`src/gstrace.c`), so equal hashes mean the GS received the same draws.

Visual changes compare against the real game: same spot, same camera, side by side (`tools/dsr-ref*`); render diffs
with `tools/test-gsdump.py`.

## CI

Every pull request runs `.github/workflows/pr-checks.yml`, with no game data. Run these locally before pushing:

| Check | Command | Fails on |
|---|---|---|
| Hygiene | `python3 -I tools/ci/hygiene.py` | game data or derived files, images outside the allowed list, personal paths, duplicate TECH_DEBT IDs |
| Switch registry | `python3 -I tools/ci/switches.py` | a `#if` on a name with no row in `src/switches.h`, or a stale row |
| GS ownership | `python3 -I tools/ci/gsown.py` | a raw GS register write outside `gs.h` / `gs.c`, a VU1 kick or wait outside `ot.c`, VRAM rows not summing to 4096 KB |
| Part ownership | `python3 -I tools/ci/partown.py` | a world streaming part's private header (`world_i.h`, `mapstream_i.h`, `msarena.h`) included outside the part, the page arena seeing map items, an `extern` of a part's shared name elsewhere |
| Memory regions | `sh tools/ci/test_mem.sh` | a host test of the region allocators (`mem.c` built with `MEM_HOST_TEST`) |
| Offline | `tools/ci/offline.sh` | VU1 emulator tests, generated `.vsm` / `vu1_layout.h` / `*_data.h` out of date, Python that does not compile |
| Compile | `tools/ci/compile.sh` | a warning or error in the data-free ELF build (ps2dev v2.0.0, `-Werror`) |
| Switch builds | `tools/ci/switches.sh` | any fallback or debug switch build that no longer compiles and links |

After each merge to `main` and nightly, `.github/workflows/pcsx2-smoke.yml` runs the Firelink and Burg smoke test on
the maintainer's PC (`tools/ci/pcsx2-smoke.sh`, self-hosted runner); only the step summary and budget table are
published. The PR template's Verification section takes the PCSX2 results of a scoped run.
