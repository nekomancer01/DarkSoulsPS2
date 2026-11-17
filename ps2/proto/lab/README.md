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
the script's input (with `pad` presses through the bonfire's input), the camera (src/cam_input.c, shared with
frame.c), then the simulate phase unless a shot froze the frame, test_frame_end and save_tick. The output is the
console's TEST lines; `lab.py run` reads them and prints per script:

- PASS / FAIL;
- every check that ran: `check` lines and the PASS / FAIL `expect` lines (a script with none says so: it passes by
  running to its end);
- the expects that test what is drawn (`drawn`, `sfx`, `bossbar`, `shines`, `dialog`), listed as not run;
- the commands that are presentation only in the Lab (`shot`, `stats`, `loadscreen`);
- world.c findings (a wanted block that found no room, as on the console);
- the failures: a `TEST error` line, a FAIL expect, a command the Lab cannot run (named, with what it needs: `pickup`,
  `behind`, `pad start`, an enemy query), a warp whose settle ran into world.c's 30 s cap, any read-back stub.

`--state` also compares the run with the console's own run of the script (its `option record` state log): the same
state sequence and the end position within 1 cm (det_walk_open: 0.036 cm). The 102 scripts take 7 s with `-j 4`;
saves (`option save host`) go to build/host/save, emptied once per `run` (pairs such as save_write / save_check run
in order with `-j 1`).

## Stubs: presentation and read-back

Every stub counts its calls, in two lists (lab.h). Presentation stubs (`LAB_COUNT`: sound, render inputs, the loading
screen) are expected: the simulation never reads their result. Read-back stubs (`LAB_READBACK`: the bonfire's respawn
spot, the navmesh floor fallback, the camera basis, a missing pack) return something simulate depends on: they are
the real layer violations of the Polaris 0.4 boundary map, and a script that reaches one fails in the Lab.

What is in it: game data, flags, progress, character stats, items, saves, the player (ESD, combat, physics, status),
the navmesh, the player's model and animations (no VU1 packets: their layout only), and the static world (Polaris
1.7c-2): src/world.c's blocks with their collision, breakables (src/objects.c), events (src/event.c and ev_*.c: fog
walls, doors, levers, treasures, warps), world items (src/pickup.c) and the bonfire (src/bonfire.c). Packs are read
from `build/data` with stdio (lab_pack.c).

The static world: the Lab boots as main.c does (the world table, the boot block's collision, objects and events,
world_systems for the other blocks' types) and runs world.c unchanged, so which blocks are on, and with them which
collision, breakables and events exist, is the console's own rule. What the Lab leaves out is only the wait: every
read completes at once (lab_stubs.c), and each frame steps world_update until `world_settled` holds (the condition a
warp's settle waits for), so nothing the feet want is ever still loading when simulate runs. Collision comes whole
with its block (`COL_TILES=0`: world.c's own fallback reads the whole `collision.bin` with the lite set, every tile
of it), never page by page. A wanted block can still stay off by world.c's own rules (no slot, `world: settle
stopped: no room`); `lab.py run` lists those as findings, since the console does the same.

The console's world holds the player at a block or tile still loading (`world_hold`); the Lab never has to. Recorded
traces carry those frames (flag 4) and a replay holds the player on them too, so a Burg walk compares frame by frame
(`det_burg2`: 92 held frames, 0.01 cm). A script run has no trace, so a Burg script's timing can differ from the
console's by the frames the console was held.

The bonfire's menu and dialogs are bonfire.c's own state and run (script `pad` presses reach them as frame.c routes
them); dialog screens (drb) are ready at once and draw nothing; the Start menu is not in the Lab.

What is not, yet (Polaris 1.7c-3): enemies and their AI. An event that asks about or acts on an enemy is a read-back
stub, except in a script with `option noenemies`, where the console has none either and answers the same. Never:
sound, the map's drawing, menus and dialogs (an event or pickup that opens a dialog the game waits on is a read-back
stub), and the streaming waits themselves: the Lab cannot reproduce a fall through a tile that had not loaded.

32-bit because the loaders keep pointers in 32-bit fields of the loaded blobs (TECH_DEBT core-1); when formats move to
chunk streams with base-plus-offset accessors (Polaris phase 3) the Lab can go 64-bit and load into Python directly.

Floating point: SSE scalar math, no fast-math, no contraction (never x87). sinf / cosf / atan2f come from the host's
libm, not the EE's newlib; the first traces agree within 0.04 cm, so a shared implementation waits until a divergence
needs it.

