/* GS memory (VRAM) budget per owner, in KB: one table for the runtime (gs.c, the "gsvram:" log lines and the
 * HEAPMAP "vrammap:" census) and tools/gs_budget.py (tools/budget.py's check of test runs). tools/gs_budget.py reads
 * this file, so keep every value a plain number on its own #define.
 *
 * Sizes are from the Polaris phase 0 census (main f3689ac, 2026-11-17, ISO routes iso_burg_walls / iso_burg_taurus /
 * iso_ame_report3 / world_walk): the peak per owner rounded up to whole 8 KB GS pages, with a little room above it.
 * The rows sum to the GS's 4096 KB together with GSB_HOLES_KB (blocks free inside pages already taken) and
 * GSB_FREE_KB (pages never taken), so a new owner's budget has to come out of another row.
 *
 * In this step (Polaris 1.1) the budgets only report: an owner going over its row logs a "TEST warn" line, which
 * tools/budget.py counts. Enforcement (an allocation over its owner's budget refused) is Polaris 2.7. Running out of
 * the 4096 KB stays a hard failure as before ("Not enough VRAM", the GS draws nothing new). */
#ifndef GS_BUDGET_H
#define GS_BUDGET_H

#define GSB_FRAME_KB 1152      /* draw buffer 448 + Z 448 (512 x 448, 16-bit) + display buffer 256 (512 x 224) */
#define GSB_WORLD_KB 1152      /* map pixel pool 896 + its CLUTs 72 + block textures / CLUTs 149 + relief / near pools */
#define GSB_CHR_PLAYER_KB 520  /* the player's textures 485 (488 in pages): never evicted; + its silhouette target and Z 32 (shadow.c) */
#define GSB_CHR_KB 176         /* enemies and NPCs: 58 at the VRAM low, about 150 at the heap low */
#define GSB_OBJ_KB 528         /* objects and breakables: 528 at the VRAM low, 362 at the heap low (16 to the silhouette) */
#define GSB_FX_KB 320          /* effect textures: 311 / 285 */
#define GSB_UI_KB 216          /* HUD atlases 209, menu DRB, font (8 to the silhouette) */
#define GSB_HOLES_KB 0         /* free blocks inside pages already taken (r_vram's first fit fills them) */
#define GSB_FREE_KB 32         /* pages never taken: 233 / 360 measured; the slack after the rows above */
#define GSB_LOAD_KB 336        /* the loading screen: borrowed from the map pool (or the top of free VRAM at boot),
                                * not part of the sum; it must come back to 0 after every loading screen */
/* The player's silhouette shadow (Polaris 2.2 PR 6) pays its 32 KB from CHR_PLAYER's page slack (8), OBJ (16) and UI
 * (8) and leaves FREE alone: Polaris 2.7's rebalance changes only WORLD, CHR, FX and FREE, so the two merge to 4096 by
 * taking each side's rows (1152 + 1144 + 520 + 184 + 528 + 352 + 216 + 0). */

#endif
