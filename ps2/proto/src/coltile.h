/* Collision tiles (the cube-grid plan's step 1, 2026-11-09): a world block's collision.bin is paged
 * (tools/export_collision.py COL_TILE: one page per 16 m tile, x / y / z), its index stays resident with the block's
 * lite set and the pages within reach of the player stream into mapstream.c's structural pool. src/world.c drives it:
 * coltile_plan at boot (the page table, for the pool's size), coltile_on / _off with the block's collision, coltile_step
 * once a frame. Queries skip absent pages (collision.c), as they skip blocks that are off. */
#ifndef COLTILE_H
#define COLTILE_H

#include "pack.h"

#ifndef COL_TILES
#define COL_TILES 1  /* 0: collision.bin is read whole with the lite set (the old way), even when paged */
#endif

/* boot: block b's page table (the 16-byte 'CTP1' record + its pages, collision.c ColPage) and the file's origin /
 * scale (DCOL header bytes 16..31). Copies what it needs. 0 = not a page table. */
int coltile_plan(int b, const float origin_scale[4], const void *ptab);
/* the structural pool room the pages may take at once: over every page's box centre of every planned block, the
 * pages of all blocks within the keep window (+ one map cell, pages read together stay together) */
unsigned coltile_budget(void);
/* block b's index is loaded (col_load_index): its pages read from pk at file offset base (collision.bin's raw entry) */
void coltile_on(int b, Pack *pk, unsigned base);
/* block b's collision goes: its pages are freed (a read in flight for it is cancelled first) */
void coltile_off(int b);
/* once a frame (and in a settle's loop): collect the read that landed, free the groups no longer kept, start the read of
 * the nearest wanted page (with the wanted pages next to it in the same 32 m cell) */
void coltile_step(const float feet[3]);
/* wanted pages within r (xz) / r (y) of feet that are not in yet and can still come (a settle waits for them; the
 * hold: a page under the feet) */
int coltile_missing(const float feet[3], float r);

typedef struct {
    unsigned reads, read_kb, failed, noroom, drops, in_kb, peak_kb, pages_in;
} ColTileStats;
extern ColTileStats coltile_stats;

#endif
