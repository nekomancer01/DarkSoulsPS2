/* Streamed near cells of a two-level map (map.bin 'DSM5' + the raw pack entry cells.bin, tools/export_map.py STREAM):
 * the coarse level is resident in map.bin; each CHUNK_GRID cell's near level is one blob in cells.bin, read in the
 * background (pack.c pack_read_async: host: or cdrom0:) while the player is within MS_LOAD_R of the cell */
#ifndef MAPSTREAM_H
#define MAPSTREAM_H

/* DSM5 cell table entry (export_map.write_cells) */
typedef struct {
    float mn[3], mx[3];
    unsigned off, bytes, near, first, count, pad;
} MapCell;

/* Open the area pack's cells.bin for the n cells of the table; 0 if the pack has no readable (raw, non-empty) entry */
int mapstream_init(const char *pak_path, const MapCell *cells, unsigned n);
/* World blocks (src/world.c): instance k (0 = the boot area, which mapstream_init opens) of another map block's cell
 * table, all instances sharing the arena; mapstream_get answers for the selected instance */
#define MS_INST 4
int mapstream_open(int k, const char *pak_path, const MapCell *cells, unsigned n);
/* the same with a Pack the caller opened and keeps open (pack.h; no pack_open / close here: no IOP wait) */
int mapstream_open_pack(int k, const void *pack, const MapCell *cells, unsigned n);
void mapstream_close(int k);
void mapstream_select(int k);
/* World blocks' lite sets (src/world.c) share the arena: n bytes (64-byte aligned), dropping the farthest cells no
 * chain reads (not used in the last two frames) until they fit; NULL if they cannot. The arena is allocated on the
 * first call if mapstream_update has not done it yet. */
void *mapstream_alloc(unsigned n, const float pos[3]);
void mapstream_free(void *p, unsigned n);
/* Heap left outside the arena beyond MS_RESERVE (call before the arena exists: world.c, for the enemy models of a
 * second block near a junction) */
void mapstream_reserve(unsigned extra);
/* the arena exists (mapstream_alloc would not create it) */
int mapstream_ready(void);
/* allocate the arena now (world builds, at boot: its size must not depend on which enemy types happen to be loaded) */
int mapstream_arena(void);
int mapstream_owns(const void *p);  /* p lies in the arena (not in memory lent to it) */
/* Memory another system leaves idle for a while (world.c: the full pool's tail behind a smaller block's streams) holds
 * near cells too. mapstream_reclaim takes it back: drops the cells in it and returns 1 once none is left (0: a read
 * into it is in flight, call again after mapstream_update); the caller still waits for the chains that drew them. */
void mapstream_lend(void *p, unsigned n);
int mapstream_reclaim(void *p, unsigned n);
/* Once per frame: finish / start reads, free far cells (blobs drawn in the last two frames are kept: the VIF chains
 * still read them) */
void mapstream_update(const float pos[3]);
/* Cell i's near blob (chunk records + streams) if loaded, else NULL; marks it used this frame */
const void *mapstream_get(unsigned i);

typedef struct { unsigned cells, bytes, loads, fails, pending, ms_last, other_kb, lent_kb; } MapStreamStats;  /* other_kb: mapstream_alloc, lent_kb: mapstream_lend */
extern MapStreamStats mapstream_stats;

#endif
