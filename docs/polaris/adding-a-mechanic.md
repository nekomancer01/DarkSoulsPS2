# Adding a mechanic

The path from "DSR does X" to a merged PR that makes the port do X the same way. It applies to any gameplay rule:
an ESD function the player's state machine queries, a TAE event, an EMEVD instruction, a param field a formula
needs, an AI goal. Read `AGENTS.md` first; its legal rules (no game data, no copied decompiled code) hold at every
step.

## 1. Find out what DSR does

Start from the game, not from how it looks.

- **Is it data-driven?** Most mechanics are. Look for the param that holds it: `python3 tools/params.py <Table>
  <row>` prints a row with field names. TAE events come from `tools/souls.py` (`souls.tae`), ESD state machines from
  `tools/esd.py`, event scripts from `tools/emevd.py`, AI scripts from `tools/lua50.py` (disassembly).
- **Is it already partly there?** Search for its ids and names: `grep -rn 'staminaRecoverChangeSpeed' ps2/proto/src
  tools`. Unknown ESD functions return 0 and unknown EMEVD instructions are logged stubs, so a missing mechanic often
  shows up as a stub line in the EE log or in `export_emevd.py --coverage`.
- **What does the exe do with the data?** `notes/EXE_FINDINGS.md` lists what is already known (addresses, offsets,
  constants, formulas). The function database (`fndb/`) and Ghidra scripts (`ghidra/scripts/`) help find more.
  Write findings as facts in your own words: the function's address, the fields it reads, the constants and the
  formula. Add them to `notes/EXE_FINDINGS.md`.
- **Check `notes/TECH_DEBT.md`.** The mechanic may already have a row (`grep -n '^| <area>-' notes/TECH_DEBT.md`)
  with the fix path somebody worked out.

## 2. Decide where it lives

- **Module.** Put the code in the module that owns the DSR manager it belongs to (see
  [subsystems.md](subsystems.md)): a player rule in `player.c`, a damage rule in `combat.c`, a stat formula in
  `chrstat.c`, an EMEVD op in `event.c` or the matching `ev_*.c`, an ESD function in the player's or enemy's hook or
  in `esd_env_move.c` when every character shares it, an AI goal in `ai.c`.
- **Layer.** Call only down the layers ([architecture.md](architecture.md#layers)). Game code that needs the UI goes
  through `src/ui_req.h`. Don't add a new call from a lower layer into a higher one.
- **Phase.** Game state changes in a simulate phase (`SIM_A` for the player, `SIM_B` for enemies, objects and
  events). Anything drawn is emitted in `EMIT` from state simulate left behind. `FRAME_STRICT` builds catch
  mistakes.

## 3. Get the data to the runtime

The runtime never reads game files directly. Extend the exporter that already reads the source:

- **Values the player or combat needs** go into a game-data record. Add the field to the exporter's declaration
  (`RECORD` in `export_player.py`, `export_combat.py` or `export_progress.py`), regenerate the header and commit it:

  ```bash
  python3 tools/gamedata.py header combat > ps2/proto/src/combat_data.h
  ```

  The layout id changes with the header, so an old pack is refused at boot with a message instead of being misread.
- **Per-character values** go into `chr.bin` (`export_chr.py`), per-area values into the area's own file. Bump the
  format's version field, update the format comment at the top of the exporter, and make the loader refuse versions
  it cannot read.
- **Dependencies.** If the export now reads another exporter's code or output, add that file to its prerequisites in
  `ps2/proto/Makefile` (narrow dependencies: the file it reads, not the other tool's whole stamp).
- **Whole game.** Take the area or character as input. Run the exporter on a second area (Undead Burg,
  `MAP=m10_01_00_00`) before calling it done.

## 4. Write the runtime code

- **Cite sources** in comments: the exe function, param field or measurement each value comes from
  (`FUN_14035ed00`, `SpEffectParam.staminaRecoverChangeSpeed`). Comments give sources and reasons, not a narration
  of the change.
- **Guesses** are marked `ASSUMED` with the reason, and logged as a row in `notes/TECH_DEBT.md` with a unique ID, a
  severity and a fix path.
- **Time** comes from the frame's dt or `frame_clock()`, never `ticks()`, so tests and the Lab stay deterministic. A
  per-frame work budget reads `frame_budget_clock()` and reports its work through `frame_det_work`.
- **Memory.** No `malloc` during play. Boot-time tables go in `mem_static`; per-frame scratch in `mem_frame`; fixed
  sets of objects in a `MemPool`. If you must touch existing legacy heap code, keep it inside its `MEM_LEGACY` tag;
  `MEM_STRICT` builds report anything untagged.
- **VRAM.** A new texture user is charged to its owner (`gs_budget.h`); a new owner's budget comes out of another
  row.
- **Drawing.** Through the order table: a VU1 segment (`ot_open` / `ot_close`) in the right bucket, or the 2D writer
  (`g2d_select`, then the `g2d_prim_*` calls). GS register words come from `gs.h`; never write a raw one.
- **Style.** C99, the surrounding code's naming and comment density.

## 5. On by default, with a way back

Every new behaviour ships on by default in the ISO build. If it replaces an existing path, keep the old one behind
one fallback switch. For example (`STAMINA_SPEFFECT` is an illustration, not a real switch):

```c
#ifndef STAMINA_SPEFFECT
#define STAMINA_SPEFFECT 1  /* SpEffect stamina regen (FUN_14035ed00); 0 = the flat rate, the old path */
#endif
```

- Add a row to `src/switches.h` (default, kind `fallback`, the CI build of the old path such as
  `-DSTAMINA_SPEFFECT=0`, owner module, `new` for the PR number) or `python3 -I tools/ci/switches.py` fails.
- If the switch is read by plain `if`s, make it a runtime variable too: add `X(STAMINA_SPEFFECT, 1)` to `CVAR_LIST`
  in `src/cvar.h` and define the name as `cv_STAMINA_SPEFFECT`, so one ELF runs both sides of an A/B with
  `option set` or `tools/test-run.py --set`.
- A new Makefile or exporter environment variable is a row in `tools/switches.tsv`.

A new mechanic with no old path to fall back to needs no switch.

## 6. Test it

From the cheapest loop to the most expensive:

1. **The Lab** (seconds, no PCSX2), for anything in the simulate phase. Write a test script in `ps2/proto/tests/`
   with `check` lines that prove the mechanic (a flag set, souls changed, the player near a point), then:

   ```bash
   make -C ps2/proto lab
   python3 tools/lab.py run ps2/proto/tests/my_mechanic.txt
   ```

   Or prototype the rule from Python against real data with `Lab(...).cmd()` / `.step()`. If the Lab reports a
   read-back stub, simulate is reading something only presentation provides: fix that before going further.
2. **PCSX2** for the same script, on Firelink and one other area:

   ```bash
   tools/test-run.py build/proto.elf ps2/proto/tests/my_mechanic.txt build/test/my_mechanic
   ```

3. **Determinism.** If the change touches the player, enemies, the world or events, re-run the gate traces
   (`tools/ci/lab.sh`, or `tools/lab.py compare` per trace). A change in behaviour that you intended means the traces
   must be re-recorded with `option record`; say so in the PR.
4. **Drawing unchanged?** If the mechanic should not change what is drawn, compare `othash:` lines of `OT_HASH` builds
   on `ot_check.txt` before and after ([build-and-test.md](build-and-test.md#graphics-gates)).
5. **Against DSR.** Compare with the real game at the same spot and timing. "It looks right" is not evidence; a
   measured value is.
6. **Budgets.** Scoped run: `tools/test-all.sh --no-build --areas <areas> --max 20`. Report fps, frame ms and free EE
   RAM before and after.
7. **CI checks locally**: the commands in [build-and-test.md](build-and-test.md#ci).

## 7. Write it down

- `notes/TECH_DEBT.md`: rows for every `ASSUMED` value and every remaining difference from DSR; move fixed rows to
  "Resolved".
- `notes/EXE_FINDINGS.md`: the exe facts you used.
- `notes/PLAYTHROUGH.md`: the status line of the system, if it changed.
- These pages, if the mechanic adds a subsystem, a data path or a contract.
- The PR (template in `.github/pull_request_template.md`): what changed and why, the verification with numbers and
  commands, and the TECH_DEBT IDs opened or resolved. One logical change per PR.

## Checklist

- [ ] Behaviour taken from DSR's data or the exe, each value cited; guesses marked `ASSUMED` and logged
- [ ] Exporter extended, format version bumped, generated headers regenerated, Makefile prerequisites added
- [ ] Runs on Firelink and a second area
- [ ] State changes in simulate, drawing in emit; no new layer crossing; no `malloc` in play
- [ ] On by default; fallback switch registered (and a cvar if it is `if`-based)
- [ ] Test script with checks, passing in the Lab and in PCSX2; gate traces still agree or re-recorded on purpose
- [ ] Budgets hold (30 fps, 1.5 MB EE RAM free, no VRAM failures); before and after numbers in the PR
- [ ] CI checks pass locally; no game data, screenshots or personal paths in the diff

## Planned: script first

Phase 5 adds a second route for game-layer systems: write the mechanic first as a Lua script on the engine's VM
(hot-reloaded from `host:` without a rebuild, bound to the engine through generated bindings) or as Python in the
Lab, then move it to C once it matches DSR or runs over its budget, with the script kept as the reference the C is
tested against. Until those steps land, mechanics are written in C as above.
