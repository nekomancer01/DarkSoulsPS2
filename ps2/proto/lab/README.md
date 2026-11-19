# The Lab (Polaris 1.7b)

The player's simulate phase built as a 32-bit host program, driven from Python over a pipe (`tools/lab.py`), so a
gameplay rule or a recorded input trace runs in seconds without PCSX2.

    distrobox-host-exec make -C ps2/proto lab          # build/host/lab (needs a host gcc with -m32)
    python3 tools/lab.py compare build/save/NAME.trace build/save/NAME.trace.state --short

Record a trace on PCSX2 with the test options `fixeddt`, `seed` and `record NAME` (src/frame.h; tests/det_walk*.txt).

## Test scripts in the Lab (Polaris 1.7c)

    python3 tools/lab.py run ps2/proto/tests/save_write.txt ps2/proto/tests/save_check.txt
    python3 tools/lab.py run ps2/proto/tests/det_walk_open.txt --state build/save/det_walk_open.trace.state

`lab ROOT AREA SCRIPT` boots as main.c does with a script (test_init, save_boot, the area's spawn from its map.bin,
save_apply_player) and runs the script through the game's own interpreter, src/testscript.c, in frame.c's order:
the script's input, the camera (src/cam_input.c, shared with frame.c), then the player's simulate phase unless a shot
froze the frame, test_frame_end and save_tick. The output is the console's TEST lines; `lab.py run` reads them and
prints per script: PASS / FAIL, every check that ran (a script with none says so: it passes by running to its end),
the commands that are presentation only in the Lab (`shot`, `stats`, `loadscreen`; `pad` presses other than Start reach
no menu, since none is ever open), and the failures. A script fails on a `TEST error` line, on a command the Lab
cannot run (named, with what it needs: `ev`, `pickup`, `behind`, `approach obj`, `pad start`), on a spawn outside the
area's collision, and on any read-back stub. `--state` also compares the run with the console's own run of the script
(its `option record` state log): the same state sequence and the end position within 1 cm (det_walk_open: 0.036 cm).
All 102 scripts take 13 s; saves (`option save host`) go to build/host/save, emptied once per `run`.

## Stubs: presentation and read-back

Every stub counts its calls, in two lists (lab.h). Presentation stubs (`LAB_COUNT`: sound, render inputs, the loading
screen) are expected: the simulation never reads their result. Read-back stubs (`LAB_READBACK`: the bonfire's respawn
spot, the navmesh floor fallback, the camera basis, a missing pack) return something simulate depends on: they are
the real layer violations of the Polaris 0.4 boundary map, and a script that reaches one fails in the Lab.

What is in it: game data, flags, progress, character stats, items, the player (ESD, combat, physics, status), whole-area
collision (`collision.bin` of an area pack, `COL_TILES=0`), the navmesh, the player's model and animations (no VU1
packets: their layout only). Packs are read from `build/data` with stdio (lab_pack.c).

What is not, yet (Polaris 1.7c-2 / 1.7c-3): enemies and their AI, events (EMEVD: fog walls, warps, bonfires), objects
(breakables: a pot in the way blocks the console's player and not the Lab's), sound, menus. World blocks keep their collision in streamed 16 m tiles, which the Lab does not load: use an area
pack (Firelink's `m10_02_00_00`). The Lab therefore cannot reproduce a fall through a tile that had not loaded.

32-bit because the loaders keep pointers in 32-bit fields of the loaded blobs (TECH_DEBT core-1); when formats move to
chunk streams with base-plus-offset accessors (Polaris phase 3) the Lab can go 64-bit and load into Python directly.

Floating point: SSE scalar math, no fast-math, no contraction (never x87). sinf / cosf / atan2f come from the host's
libm, not the EE's newlib; the first traces agree within 0.04 cm, so a shared implementation waits until a divergence
needs it.

