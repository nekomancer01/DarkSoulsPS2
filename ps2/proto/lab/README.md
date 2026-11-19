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
of it), never page by page. A warp's settle takes at least one step, as on the console where the near cells at the
new spot always need reading (the Lab's `mapstream_busy` says busy once per settle): kept-only blocks lean and go in
that step. A wanted block can still stay off by world.c's own rules (no slot, `world: settle
stopped: no room`); `lab.py run` lists those as findings, since the console does the same.

Time: `ticks()` (the EE counter) moves 1 ms per read in the Lab (ticks.h `LAB`, lab_stubs.c lab_ticks):
deterministic, so world.c's settle loop reaches its stuck timer and 30 s cap as on the console instead of spinning.
Nothing in simulate measures gameplay time with it (1.7a moved that to `frame_clock`): in the Lab's modules it feeds
world.c's settle / stuck timers and load-time log lines, the save writer's and objects' / events' timing logs, and
model.c's skin-build budget (render).

The console's world holds the player at a block or tile still loading (`world_hold`); the Lab never has to. Recorded
traces carry those frames (flag 4) and a replay holds the player on them too, so a Burg walk compares frame by frame
(`det_burg2`: 92 held frames, 0.01 cm; `lab.py compare` prints `held_frames`: a recording with many is a poor
reference). The replay's hold is a link wrap (`--wrap=world_hold`, the Lab's build only). A script run has no trace, so a Burg script's timing can differ from the
console's by the frames the console was held.

The bonfire's menu and dialogs are bonfire.c's own state and run (script `pad` presses reach them as frame.c routes
them); dialog screens (drb) are ready at once and draw nothing; the Start menu is not in the Lab.

Enemies and AI (Polaris 1.7c-3): enemy.c, ai.c (lua0 VM), npc.c and the ragdoll run unchanged, with the camera
(src/camera.c, src/cam_input.c cam_follow: moved out of render.c / frame.c) as the console's simulate reads it.
Placement by distance, the active cap, the view filters and the pose pool are the console's own; only the wait is left
out: each frame steps enemy_stream until `enemy_stream_settled` (no type load in flight or waiting), so a type never
arrives late. In a replay of a version 3 trace the residency itself comes from the recording (`enemy_load_gate`: a
type is in exactly on the frames the console had it, whose loads also wait for RAM room) and so does the camera (eye
and target per frame). Draw groups (Polaris 1.7c-4): drawgrp.c runs as on the console, dg_player (moved out of map.c's
map_set_player) once a frame after the world update, each block's dg.bin set up when it turns on (lab_world.c
map_block_init, as mapblock.c); version 3 records carry the draw-group state hash (dg_state_hash), which lab.py compare
matches frame by frame (det_seam: the aqueduct seam, 7 states, 357 frames identical). A script run (no recording) has every type at once, so a type
the console loads seconds late (RAM room, the disc) can act earlier in the Lab: tests/ev_chr.txt's c3510 at Firelink
notices the player from the boot spot in the Lab only, and one of its expects differs for that reason. Never: sound, the map's drawing, menus, and the streaming waits.

Trace version 3 (frame.c): 96-byte records add the camera's eye and target, a 6-word header adds flags (1 = option
noenemies: a replay then boots without enemies), and NAME.enemies holds per frame the loaded types ("FRAME T
NAME@DIR...") and a row per loaded enemy (enemy.c enemy_trace_lines: entity, position, yaw, ESD states, AI state,
action, HP, omission level, draw group shown, placement). `lab.py compare` gates on the same (ESD, AI, action) and HP
sequence per enemy from the first input (the wait before it lets the console's types arrive; the Lab has them at
once) and reports positions against --enemy-cm (det_burg3: 29 enemies, 0.0 cm). The recorder gathers its output in
RAM and writes it after the streamer's reads (host writes beside async reads cut those short).

32-bit because the loaders keep pointers in 32-bit fields of the loaded blobs (TECH_DEBT core-1); when formats move to
chunk streams with base-plus-offset accessors (Polaris phase 3) the Lab can go 64-bit and load into Python directly.

Floating point: SSE scalar math, no fast-math, no contraction (never x87). sinf / cosf / atan2f come from the host's
libm, not the EE's newlib; the first traces agree within 0.04 cm, so a shared implementation waits until a divergence
needs it.

