/* world_i.h: the block lifecycle's own state and constants (Polaris 4.1 split of world.c, code moved as it was).
 * Only the world part's files include it: world.c (tables, boot, settle, the API), wblock.c (a block's lite set: reads,
 * turn on / off, lean, read back), wextra.c (relief, far panorama, the boot block letting go) and wplan.c (what is
 * wanted: links, reach, slots, the read order). Every other module uses world.h. tools/ci/partown.py checks it. */
#ifndef WORLD_I_H
#define WORLD_I_H
#include <kernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "world.h"
#include "loading.h"
#include "pack.h"
#include "collision.h"
#include "map.h"
#include "mapstream.h"
#include "residency.h"
#include "ot.h"  /* ot_wait: no VIF chain reads the arena any more */
#include "objects.h"
#include "fx.h"
#include "enemy.h"
#include "nav.h"  /* nav_unbind (WORLD_NAV_LITE) */
#include "hitch.h"
#include "coltile.h"
#include "ticks.h"

/* Which blocks load: the collision triangle under the player (world.bin links, tools/export_world.py). Between two
 * maps that is DSR's own rule, the ConnectCollision part (MSB type 11) under the player; inside one MSB (DSR keeps it
 * whole) a triangle loads the neighbours within one 32 m cut cell and keeps them within two (ASSUMED lead, TECH_DEBT
 * world-3). With no floor under the player (a warp into a block not loaded yet, a fall) the blocks whose bounds hold the
 * feet are wanted too, so a warp settles like DSR's loading screen. */
/* A block the triangle under the player no longer keeps unloads after WORLD_KEEP_S (ASSUMED): Firelink's aqueduct
 * ConnectCollision parts are narrow, and stepping off one between stairs unloaded Burg west and read it again 1.3 s
 * later (world_route.txt 2026-11-01) */
#define WORLD_KEEP_S 5.0f
/* A wanted block with no free slot takes the slot of a wanted block farther away (world-10 seam holds, default on;
 * -DWORLD_SLOT_NEAREST=0 = only blocks no triangle wants give up a slot; room_for) */
#ifndef WORLD_SLOT_NEAREST
#define WORLD_SLOT_NEAREST 1
#endif
#define WORLD_SLOT_MARGIN 16.0f
#ifndef WORLD_SLOT_DEFER
#define WORLD_SLOT_DEFER 1  /* (wplan.c w_slot_deferred) */
#endif
/* WORLD_YIELD (default 1; 0 = off): a large temporary that found no window in the arena (an enemy model.bin,
 * enemy.c arena_get) has the blocks whose lite sets spilled into the item pages turn off, never the player's floor
 * or current block, and no lite set spills there for WORLD_YIELD_S (each failure renews it). The item pages are
 * 2.2 MB on Anor Londo's build, a spilled fourth lite set held 0.8 MB of them and the 1.4-1.9 MB model temporaries
 * found no window (the heap's largest hole 0.6-0.8 MB): c3491 never loaded in one of three iso_anor runs */
#ifndef WORLD_YIELD
#define WORLD_YIELD 1
#endif
#define WORLD_YIELD_S 5.0f
/* Bytes per background read: one at a time through pack.c's async slot, shared with mapstream.c (a near cell waits for
 * at most one of these) */
#define WORLD_READ (128u << 10)
/* Blocks resident besides the boot block. Their lite sets live in mapstream.c's near-cell arena (mapstream_alloc drops
 * the farthest near cells to make room), so a world build costs no RAM while no neighbour is loaded (TECH_DEBT world-5).
 * 3: the block under the player plus both neighbours at a seam inside the Burg (the bonfire / bridge block keeps Burg
 * west and the Parish block). 4 since 2026-11-23 (TECH_DEBT world-50): 13583 of the 24 blocks' collision triangles want
 * four blocks besides the boot block (Anor Londo's arrival block by the cathedral 5391, the Burg bonfire block by the
 * Parish steps 3522); with three the fourth was deferred and a warp there left the player over a block that never came
 * (Anor Londo, 3 of 3 runs). The structural pool stays sized for WORLD_POOL_SLOTS lite sets per link: sized for four it
 * grew 4223 -> 4643 KB, taken from every block's visual room; the fourth lite set at those spots spills into the misc
 * pool or the item pages instead (lite_alloc -> mapstream_alloc, world_stats.pool_spills). texcache.h TC_BLOCKS
 * follows (a block's CLUTs, ~40-50 KB of the GS WORLD row each). WORLD_SLOTS itself is defined in world.h */
#ifndef WORLD_POOL_SLOTS
#define WORLD_POOL_SLOTS 3  /* lite sets per link the structural pool is sized for (world_struct_bytes) */
#endif
/* Every loaded block draws the same way: its map's resident level streams per cell from its mapres.bin and its
 * textures' mip 0 per texture (mapstream.c mapstream_res_open via map.c), in the near arena with the near cells, so
 * there is no full pool and no full swap (Burg 4.9 MB / Firelink 8.5 MB, 5-14 s from cdrom0:, TECH_DEBT world-2 /
 * world-5). Only the normal-map relief (bump.bin) is per block: read for the block the player stood in for
 * WORLD_SWAP_S (no reading back and forth on a boundary; ASSUMED value). */
#define WORLD_SWAP_S 1.0f
#define WORLD_RELIEF_RETRY_S 5.0f  /* a relief that found no room tries again after (ASSUMED) */
/* Largest near arena: what the heap leaves beyond the reserves at boot (the old full pool's 7.8 MB went to it). It
 * took 0.4 MB more than full pool + arena did and route_enemies' RAM low point at the Burg entry fell 1.30 -> 0.60 MB
 * (ISO, 2026-11-05): WORLD_ARENA_LESS more stays outside it */
#define WORLD_ARENA_LESS (512u << 10)  /* (Burg plan B3, 2026-11-08: half of it went to the arena's visual room) */
#define WORLD_POOL (24u << 20)
/* MS_POOLS: the arena's size is this build constant, not whatever heap a boot spot leaves (8640 KB booted in the Burg
 * on the ISO, 2026-11-08; booted on world_route's spawn it was ~1.5 MB larger, so the visual room changed with the boot
 * spot). The size is residency.h's RES_ARENA_KB, the table the export and tools/world_budget.py solve against.
 * A boot with less heap gets a smaller arena (logged by mapstream.c; tools/budget.py fails such a run). (9152 KB =
 * 8640 + the 512 KB WORLD_ARENA_LESS gave up in the Burg plan's B3, until 2026-11-12: the ISO's heap gave 9088.) */
#define WORLD_ARENA_KB RES_ARENA_KB
/* A warp settles once the sky and the resident cells within WORLD_SETTLE_R of the feet are in, those of the block
 * the player is in out to WORLD_SETTLE_FOCUS_R: DSR's loading screen lasts until the area is loaded, and with 40 m
 * Firelink's horizon tree line and the aqueduct's rock wall were still missing 3 s after a warp back from the Burg
 * (ISO world_route, 2026-11-05); 160 m made the far-north and Burg warps 11-14 s from cdrom0:. The rest streams in
 * nearest first. ASSUMED radii. */
#define WORLD_SETTLE_R 40.0f
#define WORLD_SETTLE_FOCUS_R 96.0f
/* COL_TILES: a warp also waits for the collision tiles within this of the feet (ASSUMED: the floor around the player
 * and the walls of the next steps; the rest streams in at walking speed well before it is reached) */
#define WORLD_SETTLE_COL_R 16.0f
/* frames a buffer stays untouched after the map stopped drawing from it (VIF chains of the last two frames) */
#define WORLD_DRAIN 3
/* WORLD_LITE_DIET (Makefile, default 1; the Burg plan's step B1): a block that is on only because the triangle under the
 * player keeps it (not wanted, not underfoot) for WORLD_LEAN_S lets go of its map tables (maptab.bin: chunk table,
 * textures, cell table, horizon; map.c map_block_lean), keeping its collision, events, objects and characters. Wanted
 * again, it reads them back (relite) before anything else of the map streams. Two Burg blocks' lite sets took 4.2 of
 * the arena's ~10 MB at the bonfire seam (TECH_DEBT world-14); a kept-only block now holds its ~0.9 MB of collision and
 * events. The other half of the diet is in tools/mapsplit.py (MIP1_OUT: mip 1 streams). 0: today's lite sets. */
#ifndef WORLD_LITE_DIET
#define WORLD_LITE_DIET 1
#endif
/* MS_POOLS (mapstream.h, the Burg plan's step B2): the lite sets live in a structural pool of their own at the
 * arena's top, sized here once from the world table: the most any collision triangle's links can have on at once
 * (world_struct_bytes: wanted blocks whole, kept-only ones lean with WORLD_LITE_DIET, at most WORLD_SLOTS of them);
 * the relief, a block's texture buffer and enemy model temporaries in a misc pool (the largest relief + the largest
 * texture buffer + WORLD_MISC_EXTRA). A wanted lite set that finds the structural pool full first leans the kept-only
 * blocks (they drain in WORLD_DRAIN frames), then turns one off (room_for); it never clears a map item. The visual
 * room left is a constant tools/world_budget.py computes the same way (gen/world_budget.json "structural_kb"). */
#define WORLD_MISC_EXTRA (640u << 10)  /* ASSUMED: one of the Burg's larger enemy model.bin temporaries (578-590 KB) */
/* wanted blocks the structural pool counts whole per link (the rest lean): 3 = every link's worst case (one Burg_w
 * triangle loads w, b and p: 4484 KB, visual 2.1-3.1 MB of the ISO's 8640 KB arena); fewer = a smaller pool, the rare
 * link with more spills its last lite set into the item pages (logged, world_stats.pool_spills) */
#ifndef WORLD_STRUCT_WHOLE
#define WORLD_STRUCT_WHOLE 2  /* (Ame 2026-11-08: two wanted whole + one lean, visual 3.1 -> 3.8 MB) */
#endif
/* The relief (bump.bin) stays out of the pools: it takes item room only where no map item is wanted
 * (mapstream_alloc_soft), never the heap. Sized into the misc pool (tried 2026-11-08, dropped) it left the Burg 2112 KB
 * of visual room in the 8640 KB arena (structural 4544 KB for the link that loads three Burg blocks, misc 1984 KB). */
/* Kept horizon (built in: the frozen residency design, 2026-11-12): a kept-only block keeps its cell table and horizon (maptab's last piece, E_TABC; ~0.45
 * MB in the Burg) and map.c draws its horizon (map_block_lean): its far cells are in view beyond the pinned 64 m, and
 * without the horizon they were holes (planning thread, 2026-11-08). 0: the tables go whole. */
#define LEAN_E1 E_TABB  /* the last maptab piece a lean block lets go */
#define WORLD_LEAN_S 5.0f  /* ASSUMED: as WORLD_KEEP_S, so a seam the player walks back and forth over reads no tables */
/* MS_LEND: a kept, still-off block within this of the feet gets its structural pool room cleared of lent items (not
 * near parts) ahead of being wanted. Without the distance, with B3's radii every kept block cleared the pool all the
 * time: soak2 pop-ins 25 / 37 -> 54 / 54 on the B3 + B4 stack (2026-11-09). ASSUMED: about one 32 m cell */
#define WORLD_PREP_D 32.0f
/* WORLD_REACH (default 1, runtime switch; TECH_DEBT world-3): blocks are wanted by time-to-reach. world.bin's REAC chunk holds,
 * per run of collision triangles, the walking distance along the block's floor to the seam of each neighbour of another
 * MSB within 64 m: the ConnectCollision part that loads it (tools/worldreach.py; seams inside one MSB already load the
 * neighbour within 32 m in x / z). A neighbour is wanted once the player could reach that seam at dash speed sooner
 * than the block takes to turn on (on_est: the block's own lite-set load times, rising at once with a slow one and
 * sinking slowly, seeded with WORLD_ON_SEED_MS) plus WORLD_REACH_MARGIN_S; the links' load masks still want it too, so
 * no block is wanted later than before. Wanted blocks start nearest in time first, those the player walks away from
 * last, and the structural pool is prepared (MS_LEND) WORLD_PREP_S ahead of that instead of within WORLD_PREP_D. A block
 * within reach stays kept (hysteresis). ISO world_route (2026-11-17): Burg west on 37 s before the player crosses from
 * Firelink, was 13 s. 0: the links' masks alone (as a world.bin without REAC). */
#include "cvar.h"
#ifndef WORLD_REACH
#define WORLD_REACH cv_WORLD_REACH  /* runtime switch, default 1 (src/cvar.h) */
#endif
/* WORLD_SOURCES (default 1, runtime switch; Polaris 4.3, world.h): sources besides the player (the respawn point while
 * a death plays) want their blocks and cells too. 0: the player's spot alone */
#ifndef WORLD_SOURCES
#define WORLD_SOURCES cv_WORLD_SOURCES
#endif
typedef struct { float pos[3], vel[3]; int on; } WorldSrc;
#define WORLD_DASH 6.0f            /* m/s: ASSUMED dash speed (the dash loop's root motion is not read here) */
#define WORLD_REACH_MARGIN_S 1.0f  /* ASSUMED: the near cells at the seam stream after the lite set */
#define WORLD_ON_SEED_MS 3000.0f   /* a Burg lite set from cdrom0: 1.4-3.7 s (map streaming research, 2026-11-17) */
#define WORLD_PREP_S 2.0f          /* ASSUMED: lent items drain in WORLD_DRAIN frames; a lead of a few reads */
#define WORLD_AWAY_S 3.0f          /* rank penalty for a block whose seam gets farther (ASSUMED) */
/* WORLD_BOOT_LEAN (world.h, default 1): the boot block lets go of its map tables too. It never unloaded: its tables
 * are a heap buffer of their own and its collision, objects, events and characters are main.c's, so from deep in the
 * Undead Burg Firelink still held its chunk table, cell table and horizon (1365 of its 1555 KB of tables) and drew up
 * to 17k horizon triangles a frame (2026-11-13). With the far panorama of the block underfoot loaded (far_step: it
 * shows the boot block from there), the boot block is neither underfoot nor kept by the triangle under the player, and
 * the player WORLD_BOOT_OFF_D from the last spots where it was, its chunk table (E_TAB) and cell table + horizon
 * (E_TABC) are freed (map.c map_boot_lean; textures, navmesh and fog rows, E_TABB, stay: texcache.c, map_floor and the
 * fog point into them). They are read back when the player is within WORLD_BOOT_BACK_D of that spot again, or the
 * boot block is kept or underfoot (a warp waits for them). ASSUMED distances: the seam's ConnectCollision parts are
 * narrow, so the tables come back one coarse ring (RES_PIN_R) before the player can stand on them. */
#define WORLD_BOOT_OFF_D 64.0f
#define WORLD_BOOT_BACK_D 48.0f
#define WORLD_BOOT_SPOTS 4      /* seams remembered (Firelink: the aqueduct to the Burg, the Parish elevator, ...) */
#define WORLD_BOOT_SPOT_D 96.0f /* ASSUMED: farther than this from every remembered spot is another seam */
/* metres moved in one frame that count as a warp (test script spawns; later bonfire warps / respawns): DSR shows a
 * loading screen there, so world_update then loads and swaps synchronously before the frame is drawn */
#define WORLD_WARP 20.0f
/* metres above / below a block's collision that still count as inside it when there is no floor under the feet
 * (ASSUMED: a fall or a jump out of its range; the blocks of one world are far apart in height or not at all) */
#define WORLD_Y_MARGIN 10.0f
/* seconds before a block whose lite set found no room tries again (ASSUMED: the near cells change over ~1 s of
 * walking; every try may drop drawn cells, which then stream back) */
#define WORLD_RETRY_S 1.0f
/* heap a lite set's entries and texture buffer leave free when they fall back to it (AGENTS.md: 1.5 MB in the
 * heaviest spot) */
#define WORLD_HEAP_FLOOR (1536u << 10)
/* heap a waited-for block's lite entries leave above WORLD_HEAP_FLOOR for the enemy models that stream in once it is
 * on (ASSUMED: two of the Burg's larger model.bin, 578 + 590 KB; tools/export_chr.py outputs, 2026-11-05). Without
 * it the lite sets took the heap the models needed: 10-21 model.bin OUT OF RAM retries in route_enemies.txt */
#define WORLD_HEAP_MODELS (1200u << 10)
/* heap kept out of the near-cell arena beyond MS_RESERVE, for enemy models: world builds allocate the arena at boot
 * (world_systems) while no enemy model is loaded, so this holds every model streamed in later. Single-area builds
 * allocate it on the first frame after the bonfire's types loaded (3.6 MB in Firelink) and keep 3 MB more. Measured
 * 2026-11-01: an arena created when only 3 types were loaded (spawn at the aqueduct) was 7.7 MB and two blocks'
 * types then ran out of RAM (world_cross.txt); 3.8 MB here gives the 5.5 MB of a bonfire boot wherever the player
 * starts: near cells >= 4.2 MB at the Firelink junction, heap outside the arena >= ~1.6 MB */
#define WORLD_ENEMY_RESERVE (3800u << 10)
/* The player is held at the edge of a block whose collision has not loaded yet (world_hold) instead of walking or
 * falling into it: from cdrom0: the player walked off the Burg bonfire block's floor before the next block's lite set
 * was read, fell to the map's floor far below or out of the map and respawned elsewhere (user report 2026-11-04,
 * TECH_DEBT world-8). DSR has the whole map resident and never needs this. A loaded floor up to WORLD_HOLD_DROP below
 * the feet is the floor of a jump or a short drop, not of another place: no hold (ASSUMED: higher than a running
 * jump, lower than the drops between the Burg's levels). */
#define WORLD_HOLD_DROP 8.0f
/* seconds of holding before the frame waits for the blocks as on a warp (loading screen; ASSUMED: a Burg lite set
 * alone reads in ~1.5 s from cdrom0:) */
#define WORLD_HOLD_S 3.0f
/* ms a settle goes on while a wanted block finds no room and nothing can be turned off for it (room_stuck) */
#define WORLD_STUCK_MS 2000

/* The lite set of a block, read raw from its pack (tools/pack.py raw: entries, read in place) into its slot.
 * events.bin (A2's tools/export_emevd.py) is optional: a pack without it loads with no events. The block turns on in
 * two steps: once collision, maptab and events are read its collision and map are on (the floor the player walks onto
 * next; turn_on_col), then objects.bin and fx.bin are read for their texture pixels, one after the other into one
 * buffer (tex_total = the larger of the two), uploaded (objects_block_add, fx_block_on; their tables were kept at
 * boot), and the block's enemies and events follow (turn_on_rest). The buffer is taken only after the collision is on
 * and freed after the uploads: allocated with the lite set (one arena block per entry) the Burg west block needed
 * 1508 + 475 KB at once and found no room beside the bonfire block with enemies loaded (2026-11-05, TECH_DEBT
 * world-5). */
/* Split map tables (built in: the frozen residency design, 2026-11-12): a neighbour block's maptab.bin is read as three arena
 * blocks, cut at its section starts (map.c map_block_split): header + chunk table (E_TAB), textures + navmesh + fog
 * rows (E_TABB), cell table + horizon (E_TABC). In one piece the Burg bonfire block's 1185 KB needed a run of 19 free
 * arena pages, the largest the lite sets ask for; as pieces the largest is 549 KB. Most "no room" refusals had the
 * bytes free but no hole (TECH_DEBT world-5: 53 of 58 in world_route). The pieces' offsets come from the 80-byte
 * header, read once per block at boot. */
/* Shared events (the cube-grid plan's step 2, built in: the frozen residency design, 2026-11-12): the blocks of
 * one MSB carry the same events.bin but for their entities, hit ranges and colliders (export_emevd.py writes the map's
 * events, instrs, substs, treasures, objacts and lots alike; 67 of each Burg block's 68 KB). A block reads its own
 * middle part (E_EV: entities .. colliders, ~18 KB) and the whole file (E_EVM) only when no block of its map has one
 * in yet; the first copy is the map's (evm[], freed with the map's last block). event.c's split sets point there. */
/* maptex.bin (tools/mapsplit.py TEX_PEERS, the cube-grid plan): the textures two or more blocks of one MSB have,
 * shared by its blocks (texcache.c TC_SHR). Read (E_MTX) only when no block of the map has it in; the first copy is
 * the map's (mtx[]), freed once the map's last block has drained (free_slot). */
/* E_DG / E_PVS (WORLD_DG_LITE, default 1; 2026-11-15): the block's draw groups and visibility (dg.bin, pvs.bin) come
 * with its lite set and stay while it is on (a lean block keeps them: its collision parts' groups), instead of being
 * read for every block at boot and kept: with 14 blocks they took 3.3 MB of the heap the near-cell arena is cut from
 * (arena 10752 -> 6592 KB). 0: read at boot as before (a pack whose dg.bin / pvs.bin are not raw does that too). */
#ifndef WORLD_DG_LITE
#define WORLD_DG_LITE 1
#endif
/* E_ENM (WORLD_NAV_LITE, default 1; TECH_DEBT world-46 item 3, phase 1): the block's enemies.bin (placements + navmesh)
 * comes with its lite set and its navmesh is used in place while the block's enemies are on (nav_bind at turn_on_rest,
 * nav_unbind at turn-off), instead of a heap copy of every block's navmesh made at boot and kept (772 KB of the boot
 * heap at 27 blocks, 2026-11-25). DSR prepares a block's navmesh with the block (WorldBlockNvmPrepare in FUN_1403dac50,
 * notes/EXE_FINDINGS.md). It sits after the map tables in the read order, so collision and maps come first; it is not
 * a lean entry (a kept-only block keeps its enemies on). 0: copied at boot as before. */
#ifndef WORLD_NAV_LITE
#define WORLD_NAV_LITE 1
#endif
/* WORLD_CHR_LITE (default 1; world-46 phase 4): the block's placements and enemy types from E_ENM at turn-on
 * (enemy_block_types_lite; a new type's chr.bin head read in the background, its placements waiting for it, as DSR's
 * WorldBlockChrPrepare loads a block's characters with the block), let go at turn-off (enemy_block_untypes), instead
 * of every block's types and placements made at boot with synchronous pack reads. 0: at boot as before. */
#ifndef WORLD_CHR_LITE
#define WORLD_CHR_LITE 1
#endif
/* E_FXH (WORLD_FX_LITE, default 1; world-46 phase 2): the head of the block's fx.bin, everything before its first pixel /
 * CLUT (the effect tables, 1-20 KB), comes with its lite set and is used in place from turn_on_col (fx_block_tables) to
 * the turn-off (fx_block_drop), instead of a heap copy of every block's tables and runtime arrays made at boot (339 KB at
 * 27 blocks). The pixels still come through the texture buffer (E_FX). The entry shares fx.bin with E_FX: world_init
 * sizes it after its entry loop (fx_head_size), before world_struct_bytes. 0: tables at boot as before. */
#ifndef WORLD_FX_LITE
#define WORLD_FX_LITE 1
#endif
/* E_OBJH (WORLD_OBJ_LITE, default 1; world-46 phase 3): the head of the block's objects.bin (OHDR .. ATEX: the type,
 * piece and placement tables, the animated models and their textures; export_objects.py OHDR v2) comes with its lite
 * set; its breakable types are made from it when the objects.bin tail is in (tex_on: objects_block_types_lite), shared
 * with the map's other on blocks and counted, and let go after its instances (objects_block_untypes). Their models
 * stream by file offset (OBJ_STREAM). Animated types are made at the block's first turn-on and stay (as at boot). Until
 * now every block's types were made at boot from a whole-file read (0.9-1.5 MB in the Burg; 696 KB of the boot heap at 27
 * blocks, 2026-11-25). world_init sizes it in obj_tail. 0 (or OBJ_STREAM 0): at boot as before. */
#ifndef WORLD_OBJ_LITE
#define WORLD_OBJ_LITE 1
#endif
/* E_AI (WORLD_AI_UNLOAD, default 1; world-46 phase 4b): the block's map AI (ai.lua0) comes with its lite set; its chunks
 * are counted into the Lua VM when it is in (late_on: enemy_block_ai) and out at the turn-off, freed once no block holds
 * them (lua0.c). Until now every block's scripts ran into the VM at boot and stayed (1044 KB of the boot heap at 27
 * blocks, 2026-11-25). The Lua of the blocks a link keeps is reserved beside the arena (world_init: w_lua_res).
 * 0: every block's at boot as before. */
#ifndef WORLD_AI_UNLOAD
#define WORLD_AI_UNLOAD 1
#endif
/* WORLD_HEAD_TEX (default 1; world-46 Task 10): E_AI and E_OBJH are read into the block's texture buffer (the misc pool)
 * at turn-on instead of the structural pool. Both are used only there: lua0_load copies the protos to the Lua heap
 * (reserved apart), objects_block_types_lite copies the placements and makes the types and animated models; after that
 * their bytes were dead pool room for as long as the block was on (Burg's four lite sets carried 770 KB of them: the
 * visual room went 5122 -> 4180 KB, 2026-11-25). The texture buffer then holds the object head and its BTEX tail at
 * once (tex_total). 0: both in the structural pool as before. */
#ifndef WORLD_HEAD_TEX
#define WORLD_HEAD_TEX 1
#endif
enum { E_COL, E_TAB, E_TABB, E_TABC, E_MTX, E_EV, E_EVM, E_DG, E_PVS, E_ENM, E_FXH, E_AI, E_OBJH, E_OBJ, E_FX, NENT };
static const char *const ENTRIES[NENT] = {"collision.bin", "maptab.bin", "maptab.bin", "maptab.bin", "maptex.bin",
                                          "events.bin", "events.bin", "dg.bin", "pvs.bin", "enemies.bin", "fx.bin",
                                          "ai.lua0", "objects.bin", "objects.bin", "fx.bin"};
#define E_OPTIONAL(e) ((e) >= E_MTX)
/* what a lean block lets go (WORLD_LITE_DIET): its map tables up to LEAN_E1 and its visibility (E_PVS: pvs.c is only
 * read while the block draws; the draw groups stay, its collision parts' groups) */
#define LEAN_ENT(e) (((e) >= E_TAB && (e) <= LEAN_E1) || (e) == E_PVS)
#define E_TEX (WORLD_HEAD_TEX ? E_AI : E_OBJ)  /* first of the texture entries (the texture buffer, misc pool) */
/* first of the entries read after the floor is on (stage 1, before the textures): world-46's enemies, effect tables,
 * object head and AI; a block's turn-on waits only for collision and maps (streamer thread, 2026-11-25: read before
 * turn_on_col they made the Burg bonfire block's turn-on 0.5-1 s slower on the ISO) */
#define E_LATE E_ENM

#include "chunk.h"
#include "event.h"  /* event_block_on / _off (A2): the blob stays world.c's (arena) until _off */

typedef struct {
    char pack[16], map[16];
    float mn[2], mx[2], y0, y1;  /* xz bounds, height range of the block's collision */
    unsigned flags, nbr_n;  /* BLKS v2: neighbour count (the list is NBRS); v1's u32 mask cannot name block 32+ */
} WorldBlock;
_Static_assert(sizeof(WorldBlock) == 64, "WorldBlock");
/* world.bin NBRS v1: a block's neighbours (global block numbers, ascending). The link and reach masks below are
 * block-local: bit k = nbr[k] (wplan.c w_expand) */
typedef struct {
    unsigned char count, nbr[15];
} WorldNbrs;
_Static_assert(sizeof(WorldNbrs) == 16, "world.bin NBRS v1");
/* LNKL v1: triangles [first, first + n) of a block's collision.bin want the blocks in load and keep those in keep */
typedef struct {
    unsigned first;
    unsigned short n, load, keep, pad;  /* local masks */
} WorldLinkL;
_Static_assert(sizeof(WorldLinkL) == 12, "world.bin LNKL v1");
/* world.bin RCHL v1 (WORLD_REACH): reach runs (u16 first triangle, u8 entry) and entries, the distances to the seams */
typedef struct {
    unsigned short mask;  /* local */
    unsigned char d[6];   /* per set bit of mask, low bit first: distance in reach_step metres (rounded down) */
} WorldReachL;
_Static_assert(sizeof(WorldReachL) == 8, "world.bin RCHL v1");
/* the planner's block sets in RAM (want / keep / hold / yield): u64; the stored masks above are local (w_expand) */
typedef unsigned long long WMask;
#define WBIT(b) ((WMask)1 << (b))
_Static_assert(WORLD_MAX <= 64, "planner sets are u64");

enum { W_OFF, W_READING, W_ON, W_DRAIN };

typedef struct {
    Pack pk;
    int open, state, slot, drain;
    int tex_held;  /* the texture buffer (texbuf) is allocated */
    int stage;     /* while W_ON: 1 = collision and map on, the textures / enemies / events still to come; 2 = all on */
    void *texbuf;  /* objects.bin, then fx.bin (tex_total bytes) */
    float retry_s;  /* start() found no room: seconds until it tries again */
    unsigned off[NENT], size[NENT], total, tex_total, res_off, res_size, bump_off, bump_size;
    unsigned far_off, far_size;  /* far.bin (tools/farpano.py): its far panorama of the blocks that are off; 0 none */
    unsigned obj0;  /* objects.bin's own offset in the pack (obj_tail moves off[E_OBJ] to its texture tail) */
    unsigned obj_full;  /* WORLD_OBJ_LITE: objects.bin's whole size (E_OBJH holds its head, E_OBJ its tail) */
    unsigned char ai_on;  /* WORLD_AI_UNLOAD: its E_AI is counted into the VM (turn-on .. w_turn_off) */
    unsigned cut[2];  /* WORLD_TAB_SPLIT: where E_TABB / E_TABC start in maptab.bin */
    unsigned col_full;  /* COL_TILES: collision.bin's whole size when it is paged (size[E_COL] = its index), else 0 */
    /* WORLD_EV_SHARED: the events.bin is split (E_EV = its middle from ev_mid, E_EVM = the whole file, read only when
     * the map has no copy in: ev_full bytes); its own header; it holds a reference on its map's copy */
    int ev_split, ev_ref;
    unsigned mtx_full;  /* maptex.bin's size (0: none); the block holds a reference on its map's copy (mtx_ref) */
    int mtx_ref;
    unsigned ev_mid, ev_full;
    unsigned evhdr[24] __attribute__((aligned(64)));
    void *blob[NENT];
    unsigned ent, done;  /* read progress: entry, bytes of it */
    unsigned t0;
    int lean;          /* WORLD_LITE_DIET: 1 map tables let go (draining: lean_drain frames), 2 being read back */
    int lean_drain, tab_gone;  /* tab_gone: the tables' memory is freed (counted out of pool_used_kb) */
    float unwanted_s;  /* seconds an on block has not been wanted */
    int why;           /* the last reason start() / relite_start() returned 0 (WHY_*, the hold log) */
    unsigned want_t;   /* ticks when it was first wanted while off (0: not waiting) */
} BlockRt;
enum { WHY_NONE, WHY_SLOT, WHY_RETRY, WHY_POOLWAIT, WHY_LEANWAIT, WHY_NOROOM };
static const char *const WHY[] = {"-", "no slot", "retry wait", "lent pages draining", "a lean draining", "no room"};
/* WORLD_FAR_RESERVE (default 1; -DWORLD_FAR_RESERVE=0 = heap only): one buffer of the largest panorama is taken from
 * the heap at boot, before the streaming arena sizes itself, and a panorama that finds no heap room is read into it.
 * In the Burg the heap sits at 1.6-2.7 MB, under WORLD_HEAP_FLOOR + WORLD_HEAP_MODELS + the panorama, so Burg west's
 * panorama never came in from 180 to 250 s of Ame's play report (2026-11-17) and blocks not loaded drew nothing:
 * white voids past the tunnels, "deloaded" map and missing chunks. Arena pages instead (tried): the 3 contiguous pages
 * were found 2 times in 58 (only what no class wanted) and 0 in 46 (clearing items below the near classes). While the
 * reserve still holds the panorama being drawn, it drains first (no panorama for WORLD_DRAIN draws) */
#ifndef WORLD_WANT_3D  /* (wanted blocks nearest first, the height range counted: step) */
#define WORLD_WANT_3D 1
#endif
#ifndef WORLD_FAR_RESERVE
#define WORLD_FAR_RESERVE 1
#endif
#define WORLD_TILE_GONE_R 8.0f  /* ASSUMED: half a collision tile */

/* shared between the part's files */
extern float w_relief_at;
extern WorldStats world_stats;
extern const WorldBlock *w_tab;
extern const unsigned *w_lrange;
extern const WorldLinkL *w_links;
extern const WorldNbrs *w_nbrs;
extern const unsigned *w_rrange;
extern const unsigned short *w_rfirst;
extern const unsigned char *w_rent;
extern const WorldReachL *w_reach_tab;
extern unsigned w_reach_n;
extern float w_reach_step;
extern float w_reach_d[WORLD_MAX], w_reach_v[WORLD_MAX];
extern WMask w_reach_m;
extern WMask w_ronly_m;
extern float w_on_est[WORLD_MAX];
extern float w_clock_s;
extern float w_want_s[WORLD_MAX], w_on_s[WORLD_MAX];
extern WMask w_want_m, w_keep_m;
extern BlockRt w_rt[WORLD_MAX];
extern int w_nblk, w_cur, w_slot_of[WORLD_SLOTS];
extern float w_feet_now[3];
extern int w_bump_blk, w_bump_rd, w_bump_reading;
extern int w_tk;
extern unsigned char *w_bump_buf, *w_bump_old;
extern unsigned w_bump_done, w_bump_t0, w_bump_old_n;
extern int w_bump_old_drain;
extern float w_in_cur_s;
extern int w_far_blk, w_far_rd, w_far_reading;
extern unsigned char *w_far_buf, *w_far_cur, *w_far_old;
extern int w_far_old_drain;
extern unsigned char *w_far_res;
extern unsigned w_far_res_size;
extern int w_far_swap;
void w_far_free(unsigned char *p);
extern float w_far_retry_s;
extern float w_yield_s;  /* WORLD_YIELD: seconds no lite set spills into the item pages (wplan.c) */
int w_under(int i);      /* the player's floor (or a block a hold waits for) is on block i (wplan.c) */
extern int w_boot_want, w_boot_spots;
extern int w_warping;
extern int w_lite_first;
extern int w_room_stuck;
extern WMask w_hold_m, w_hold_want, w_hold_gone;
extern WorldSrc w_src[WSRC_N];
int w_read_sync(Pack *pk, unsigned off, void *dst, unsigned n);
unsigned world_struct_bytes(void);
unsigned world_lua_bytes(void);  /* wplan.c: WORLD_AI_UNLOAD reserve */
void w_step_time(unsigned t0);
void w_bump_free(void *p, unsigned n);
void w_bump_drop(void);
void w_turn_off(int i);
void w_free_slot(int i);
void w_lean(int i);
void w_lean_step(int i);
int w_relite_start(int i);
void w_relite_step(int i);
void w_read_step(int i, int issue);
int w_start(int i);
void w_bump_step(void);
void w_far_step(void);
void w_boot_lean(void);
void w_boot_step(int issue);
void w_step(const float *feet, float dt);
int w_slot_deferred(int i, const float *feet);
/* block i is wanted by a source besides the player alone (a settle does not wait for it: the player is not there) */
int w_src_only(int i);


static inline float box_d(const WorldBlock *b, const float *p)
{
    float dx = p[0] < b->mn[0] ? b->mn[0] - p[0] : p[0] > b->mx[0] ? p[0] - b->mx[0] : 0;
    float dz = p[2] < b->mn[1] ? b->mn[1] - p[2] : p[2] > b->mx[1] ? p[2] - b->mx[1] : 0;
    return dx > dz ? dx + dz * 0.5f : dz + dx * 0.5f;  /* within 12 % of the distance, no sqrt */
}

/* the first block of block i's map (evm[]'s index) */
static inline int map_lead(int i)
{
    for (int j = 1; j < i; j++)
        if (!strncmp(w_tab[j].map, w_tab[i].map, 16))
            return j;
    return i;
}

/* A wanted block's lite set found no room in the near arena (or no free slot): turns off the farthest block that
 * is on only because the triangle under the player keeps it (not wanted, not underfoot). From cdrom0: the
 * Parish block (1072 KB) found no room at the courtyard steps while the Burg west block stayed kept from the bonfire
 * block with the bonfire block full, never loaded, and the player walked off the loaded floor (2026-11-05, TECH_DEBT
 * world-8). 0 if no block could go. */
static inline int slots_full(void)
{
    for (int s = 0; s < WORLD_SLOTS; s++)
        if (w_slot_of[s] < 0)
            return 0;
    return 1;
}

/* WORLD_REACH: seconds until the player could stand on block i's seam (dash speed), -1 if unknown */
static inline float reach_t(int i)
{
    return w_reach_m >> i & 1 ? w_reach_d[i] / WORLD_DASH : -1.0f;
}

#endif
