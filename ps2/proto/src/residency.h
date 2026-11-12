/* The residency design's numbers (frozen 2026-11-12, TECH_DEBT world-14): what map data the streamer keeps in the
 * arena around the player, and the per-block budgets the map export is solved against. One table for the runtime
 * (mapstream.c, map.c, world.c), tools/export_map.py and the budget checks (tools/world_budget.py at build time,
 * tools/budget.py on test runs); tools/residency.py reads this file, so keep every value a plain number on its own
 * #define or RES_BLOCK line.
 *
 * The design (mapstream.c's header has the classes):
 *   hard tier  near parts within RES_NEAR_R of the player and the coarse level and kept meshes within RES_PIN_R, of
 *              every block loaded at the spot. It must fit the visual room at every navmesh spot: the export solves
 *              each block against its RES_BLOCK budget (export_map.py solve_budget, the block's share of a seam spot
 *              by area), tools/world_budget.py --strict fails the build when the sum over the blocks at a spot of a
 *              gated block is over the room, and tools/budget.py fails a test run whose arena came out smaller than
 *              RES_ARENA_KB or whose visual room is under the largest RES_BLOCK room.
 *   soft tier  the rest (near read-ahead to the media's want radius, the coarse ring to RES_HZN_R, coarse cells in view
 *              out to 260 m, another block's backdrop, mip 0 / mip 1 textures, foliage): ranked below the hard tier,
 *              fills what the hard tier leaves, cut by the plan when it does not fit (the horizon stands in).
 * The visual room = (RES_ARENA_KB - structural pool - misc pool, each rounded up to whole RES_PAGE_KB pages, as
 * mapstream.c ensure_arena splits them) x RES_FILL_PCT %. */
#ifndef RESIDENCY_H
#define RESIDENCY_H

#define RES_NEAR_R 20      /* m: near level drawn and budgeted within (box clipped to the cell's cube) */
#define RES_PIN_R 40       /* m: coarse level and kept meshes pinned within (map.c HZN_R: the horizon stands in beyond) */
#define RES_HZN_R 48       /* m: coarse cells with a horizon wanted within (soft), the horizon drawn beyond */
#define RES_ARENA_KB 9088  /* the streamer's arena (world.c), what the heap gives at boot (9152 asked before 2026-11-12) */
#define RES_PAGE_KB 64     /* arena page (mapstream.c MS_PAGE) */
#define RES_FILL_PCT 92    /* share of the item pages the plan fills (granule rounding, groups not filling a page) */
#define RES_ALLOW_PCT 1    /* % of a world's gated spots that may be over the room or on the seam allowance */
#define RES_ALLOW_BLOCK_PCT 5  /* % of one block's own spots that may be over its budget after the solve */

/* Per-block budgets (KB): RES_BLOCK(block, room, near). room: the hard tier of a spot of the block (all blocks
 * loaded there together, the block's share at a seam); near: of it for the near level, the rest for the coarse level
 * and kept meshes. Every streamed block needs a row (the export stops otherwise); `default` covers single-area test
 * maps. room is at most the visual room world_budget.py computes for RES_ARENA_KB (4239 KB on main 0f087d7 +
 * this table: 9088 - 3520 structural - 960 misc, x 92 %), less RES_PAGE_KB of margin for a lite set growing a page.
 * (Before 2026-11-12 every block took export_map.py's WS_ROOM 4317 / WS_NEAR_KB 2590, solved against a room
 * world_budget.py printed without the pools' page rounding: 4403 KB against 4239 KB on the ISO.)
 * Since 2026-11-13 (TECH_DEBT world-17) the blocks of one MSB share a seam spot's room by demand (export_map.py
 * WS_SHARE_TRIS) and their seam cells came back from the allowance; the lite sets grew with the chunks and the visual
 * room is 4180 KB. 4110 / 2466 (the room less a page) for the blocks alone in their MSB; Parish 4050 / 2430, Burg west
 * 3650 / 2190 and the bonfire block 3900 / 2340: with the neighbours' cells and ~620 KB of kept meshes the spots around
 * the Burg bonfire held up to 4416 KB at 4050 (1.06 % of the gated spots over the room; 0.05 % now, max 4236 KB). */
#ifdef RES_BLOCK
RES_BLOCK(m10_02_00_00, 4110, 2466)
RES_BLOCK(m10_01_00_00_w, 3650, 2190)
RES_BLOCK(m10_01_00_00_b, 3900, 2340)
RES_BLOCK(m10_01_00_00_p, 4050, 2430)
RES_BLOCK(m18_01_00_00_a, 4110, 2466)
RES_BLOCK(default, 4110, 2466)
#endif

#endif
