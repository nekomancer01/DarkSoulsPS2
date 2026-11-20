/* GS memory (VRAM) budget per owner, in KB: one table for the runtime (gs.c, the "gsvram:" log lines and the
 * HEAPMAP "vrammap:" census) and tools/gs_budget.py (tools/budget.py's check of test runs). tools/gs_budget.py reads
 * this file, so keep every value a plain number on its own #define.
 *
 * Sizes are from the Polaris phase 0 census (main f3689ac, 2026-11-17, ISO routes iso_burg_walls / iso_burg_taurus /
 * iso_ame_report3 / world_walk): the peak per owner rounded up to whole 8 KB GS pages, with a little room above it.
 * The rows sum to the GS's 4096 KB together with GSB_HOLES_KB (blocks free inside pages already taken) and
 * GSB_FREE_KB (pages never taken), so a new owner's budget has to come out of another row.
 *
 * An owner going over its row logs a "TEST warn" line, which tools/budget.py counts. Since Polaris 2.7 the owners
 * that come and go with the world (CHR, OBJ, FX) are refused an allocation past their row (gs.c gs_vram_admit, "TEST
 * warn: gs vram refused"; the caller drops that texture set as when VRAM is full); the rest still only report.
 * GS_VRAM_ENFORCE=0 builds report only. Running out of the 4096 KB stays a hard failure as before ("Not enough VRAM",
 * the GS draws nothing new). */
#ifndef GS_BUDGET_H
#define GS_BUDGET_H

#define GSB_FRAME_KB 1152      /* draw buffer 448 + Z 448 (512 x 448, 16-bit) + display buffer 256 (512 x 224) */
#define GSB_WORLD_KB 1144      /* map pixel pool 896 + its CLUTs 72 + block textures / CLUTs 149 + relief / near pools; peak 1137 (iso_burg_taurus, 2.7 census) */
#define GSB_CHR_PLAYER_KB 496  /* the player's textures 485: never evicted */
#define GSB_CHR_KB 192         /* enemies and NPCs: 58 at the VRAM low, about 150 at the heap low, 180-184 at Firelink (2.7) */
#define GSB_OBJ_KB 544         /* objects and breakables: 528 at the VRAM low, 362 at the heap low */
#define GSB_FX_KB 352          /* effect textures: 311 / 285; 345 on the golden taurus route (2.7 census: 16 from WORLD, 16 from FREE) */
#define GSB_UI_KB 216          /* HUD atlases, menu DRB, font: peak 209 (2.7 census; 8 to WORLD) */
#define GSB_HOLES_KB 0         /* free blocks inside pages already taken (r_vram's first fit fills them) */
#define GSB_FREE_KB 0          /* pages never taken: 233 / 360 measured; the slack after the rows above (to CHR and FX, 2.7) */
#define GSB_LOAD_KB 336        /* the loading screen: borrowed from the map pool (or the top of free VRAM at boot),
                                * not part of the sum; it must come back to 0 after every loading screen */

#endif
