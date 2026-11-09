/* Streamed near cells of a two-level map (map.bin 'DSM5' + the raw pack entry cells.bin, tools/export_map.py STREAM):
 * the coarse level is resident in map.bin; each CHUNK_GRID cell's near level is one blob in cells.bin, read in the
 * background (pack.c pack_read_async: host: or cdrom0:) while the player is within MS_LOAD_R of the cell */
#ifndef MAPSTREAM_H
#define MAPSTREAM_H

/* DSM5 cell table entry (export_map.write_cells). A near blob over export_map.NEAR_PART (64 KB, one arena page) comes in parts: the
 * cell's entry holds the first, the entries right after it the others (near | CELL_PART, no resident chunks, the same
 * box). Each part is an item of its own here; map.c draws the cell's near level once every part is in. */
typedef struct {
    float mn[3], mx[3];
    unsigned off, bytes, near, first, count, pad;
} MapCell;
#define CELL_PART 0x80000000u
/* Cell keys (2026-11-08, TECH_DEBT world-10; export_map.py write_cells, table header flag CELLS_KEYS): after the
 * entries, per entry s16 x, y, z, 0: the CELL_GRID m cube (export_map.CHUNK_GRID) its triangles were grouped into by
 * centroid. Triangles are not cut at the cube, so a cell's box reaches past it (Undead Burg near cells: 12 m at the
 * median, up to 74 m: big wall / ground triangles); measured to the box, the near level of cells up to ~26 + 12 m away
 * was wanted and drawn, ~1.3x the near bytes the export budgets (tools/world_budget.py). MS_NEAR_CUBE (default on;
 * make DEFS=-DMS_NEAR_CUBE=0 = the box as before): the near level's distance (map.c's switch, mapstream's near ranks)
 * is measured to the box clipped to the cube. Both levels lock the cell's cuts, so the switch stays
 * watertight; frustum and horizon tests keep the whole box. */
#define CELLS_KEYS 2u
#define CELL_GRID 32.0f
#ifndef MS_NEAR_CUBE
#define MS_NEAR_CUBE 1
#endif
static inline const short *cell_keys(const MapCell *tab, unsigned n)
{
    return MS_NEAR_CUBE && tab && (((const unsigned *)tab)[-3] & CELLS_KEYS) ? (const short *)(tab + n) : (const short *)0;
}
/* squared distance from p to entry c's near level: its box, clipped to its cube when key (4 shorts) is given */
static inline float cell_near_d2(const MapCell *c, const short *key, const float *p)
{
    float d2 = 0;
    for (int a = 0; a < 3; a++) {
        float lo = c->mn[a], hi = c->mx[a];
        if (key) {
            float k0 = key[a] * CELL_GRID, k1 = k0 + CELL_GRID;
            lo = lo > k0 ? lo : k0, hi = hi < k1 ? hi : k1;
            hi = hi < lo ? lo : hi;
        }
        float o = p[a] < lo ? lo - p[a] : p[a] > hi ? p[a] - hi : 0;
        d2 += o * o;
    }
    return d2;
}
#define NEAR_PARTS 64  /* parts per cell at most (export_map.NEAR_PARTS_MAX) */

/* Open the area pack's cells.bin for the n cells of the table; 0 if the pack has no readable (raw, non-empty) entry */
int mapstream_init(const char *pak_path, const MapCell *cells, unsigned n);
/* World blocks (src/world.c): instance k (0 = the boot area, which mapstream_init opens) of another map block's cell
 * table, all instances sharing the arena; mapstream_get answers for the selected instance */
#define MS_INST 5  /* = MAP_INST (map.h): the Asylum is the fifth block */
int mapstream_open(int k, const char *pak_path, const MapCell *cells, unsigned n);
/* the same with a Pack the caller opened and keeps open (pack.h; no pack_open / close here: no IOP wait) */
int mapstream_open_pack(int k, const void *pack, const MapCell *cells, unsigned n);
void mapstream_close(int k);
void mapstream_select(int k);
/* World blocks' lite sets (src/world.c) share the arena: n bytes (64-byte aligned), dropping the farthest cells no
 * chain reads (not used in the last two frames) until they fit; NULL if they cannot. The arena is allocated on the
 * first call if mapstream_update has not done it yet. */
void *mapstream_alloc(unsigned n, const float pos[3]);
/* as mapstream_alloc, but clears only items no class wants (kept / cut): never a cell the plan wants or one drawn.
 * For optional data (world.c relief). NULL when there is no such room. */
void *mapstream_alloc_soft(unsigned n);
void mapstream_free(void *p, unsigned n);
/* MS_POOLS (default 1; the Burg plan's step B2): the arena's top is split off for what is not map items, sized once
 * before the arena exists (world.c world_init, from the world table): a structural pool for the lite sets of the
 * WORLD_SLOTS blocks (MS_POOL_STRUCT) and a misc pool for the relief, a block's texture buffer and enemy model
 * temporaries (MS_POOL_MISC). Map items never use either, so the visual room (the pages below) is the same at every
 * spot; the boot log prints "mapstream: arena structural X KB, misc Y KB, visual Z KB". mapstream_alloc_pool takes
 * from one pool only and never drops a map item (NULL: the pool is full); mapstream_alloc / _soft try the misc pool,
 * then clear item pages as before. MS_POOLS=0: one arena (mapstream_pools does nothing). */
enum { MS_POOL_STRUCT, MS_POOL_MISC };
void mapstream_pools(unsigned struct_bytes, unsigned misc_bytes);
void *mapstream_alloc_pool(unsigned n, int pool);
/* the visual room in bytes (the item pages; 0 before the arena exists) */
unsigned mapstream_visual(void);
int mapstream_pools_on(void);
/* MS_POOLS is built in (before the arena exists; mapstream_pools_on says whether the arena has them) */
int mapstream_pools_on_build(void);
/* the last mapstream_alloc_pool that failed found lent items a chain may still read (MS_LEND): ask again in a frame or two */
int mapstream_pool_wait(void);
/* MS_LEND: make n bytes of the pool free ahead of time (world.c: a block the player's triangle keeps while it is still
 * off, i.e. one step from wanted), dropping the lent items no chain reads; 1 = the room is there now */
int mapstream_pool_prepare(int pool, unsigned n);
/* lent items mapstream_pool_prepare let go since the last call (the hold log) */
unsigned mapstream_lend_early(void);
/* Heap left outside the arena beyond MS_RESERVE (call before the arena exists: world.c, for the enemy models of a
 * second block near a junction) */
void mapstream_reserve(unsigned extra);
/* the arena exists (mapstream_alloc would not create it) */
int mapstream_ready(void);
/* allocate the arena now (world builds, at boot: its size must not depend on which enemy types happen to be loaded) */
int mapstream_arena(void);
int mapstream_owns(const void *p);  /* p lies in the arena (not in memory lent to it) */
/* Give pages the streamer borrowed from the heap (mapstream.c MS_BORROW) back, dropping their items, until about
 * `bytes` more are free (each a 64 KB block); call when a heap allocation failed, then try it again. Returns the pages
 * given back (0: none borrowed, or every one still read by a chain). */
unsigned mapstream_heap_release(unsigned bytes);
/* Memory another system leaves idle for a while (world.c: the full pool's tail behind a smaller block's streams) holds
 * near cells too. mapstream_reclaim takes it back: drops the cells in it and returns 1 once none is left (0: a read
 * into it is in flight, call again after mapstream_update); the caller still waits for the chains that drew them. */
void mapstream_lend(void *p, unsigned n);
int mapstream_reclaim(void *p, unsigned n);
/* World blocks: the map's resident level per cell instead of one piece (TECH_DEBT world-2 / world-5). Items of
 * instance k, each a byte range of the block's raw mapres.bin (tools/mapsplit.py): the resident chunks' streams of
 * cell `tag` (| MS_RES_FAR for backdrop chunks; export_map.py writes them grouped by cell; a large cell comes as several items of whole chunks, so no
 * item needs one big arena hole), the sky chunks' (tag MS_RES_SKY), and mip 0 pixels of single textures (MS_RES_TEX |
 * the texture's index). off = offset in the pack, rel = offset in mapres.bin (chunk records address the streams from
 * there). Cells load within MS_RES_R of the player, nearest first; the sky always; a texture while the tex_age hook
 * says it was asked for at mip 0 lately (map.c / texcache.c tc_res_age), handed over with tex_set. Call after
 * mapstream_open_pack (cell boxes from its table); n must equal its cell count. */
#define MS_RES_SKY 0x40000000u
#define MS_RES_FAR 0x20000000u  /* | cell: chunks of the far backdrop (map.c CHUNK_BACKDROP), read early */
/* | cell: only coarse chunks (map.c CHUNK_COARSE), which map.c skips while the cell's near level is drawn: not read,
 * and the first to go, while the near level is in and the cell within MS_COARSE_R (mapstream.c MS_ONE_LOD) */
#define MS_RES_COARSE 0x10000000u
/* | cell: only foliage chunks (map.c CHUNK_GRASS), which map.c thins to nothing with distance: resident only near the
 * player (world streaming v2) */
#define MS_RES_GRASS 0x08000000u
/* | cell: only chunks the cell's horizon stands for (map.c hz_covers, tools/horizon.py): map.c draws them only within
 * HZN_R of the camera, so they are read and kept only within MS_R_HZN of the player (the horizon covers the rest) */
#define MS_RES_LOD 0x04000000u
#define MS_RES_CELL(tag) ((tag) & ~(MS_RES_FAR | MS_RES_COARSE | MS_RES_GRASS | MS_RES_LOD))  /* the cell of a cell item's tag */
#define MS_RES_TEX 0x80000000u
/* | texture index (with MS_RES_TEX): its mip 1 (tools/mapsplit.py MIP1_OUT, the lite set diet): wanted while asked for
 * as mip 1 lately, ranked with the coarse cells in view (map.c draws the resident mip 2 until it is in) */
#define MS_RES_MIP1 0x00800000u
typedef struct { unsigned off, rel, bytes, tag; } MapResItem;
int mapstream_res_open(int k, const MapResItem *items, unsigned n, unsigned nitems);
/* the selected instance's item i as a base to add its chunk records' stream offsets to, or NULL while it is not
 * loaded; marks it used this frame */
const unsigned char *mapstream_res(unsigned i);
/* frames since texture tag of instance k was last asked for at mip 0 (~0u never); its mip 0 pixels, or NULL when
 * they go */
void mapstream_tex_hooks(unsigned (*tex_age)(int k, unsigned tag), void (*tex_set)(int k, unsigned tag, const void *p));
/* Visibility (mapstream.c, src/pvs.c): vis(k, id) = 1 while any chunk of instance k's item id (ids: cells [0, n), then
 * the mapstream_res_open items) is seen from a camera cell near the camera, gen(k) changes whenever an answer for
 * instance k may change (map.c ms_pvs_vis / pvs_gen). Items unseen a while are neither read nor pinned. The answers
 * are refreshed a slice per frame (mapstream.c MS_VIS_SLICE_US). */
void mapstream_vis_hooks(int (*vis)(int k, unsigned id), unsigned (*gen)(int k));
/* world.c: while on, no new reads start (a block's lite set holds the async slot back to back) */
void mapstream_hold(int on);
/* world.c: the instance the player stands in; other instances' resident cells rank lower (mapstream.c MS_RES_OTHER) */
void mapstream_focus(int k);
/* world.c settle: while on, near cells are not read (the settle waits for the resident level; ISO boot 2026-11-05:
 * 5.5 MB of near cells read inside a 14 s boot settle) */
void mapstream_settle(int on);
/* settle (world.c, a warp): 1 while the sky or a resident cell within r m of pos of any instance (r_focus for the
 * mapstream_focus instance) is still to load or being read (0 once the arena had no room for the next one: nothing
 * more can come) */
int mapstream_busy(const float pos[3], float r, float r_focus);
/* largest arena (default MS_POOL; world builds: the full pool's RAM went to it), before it is allocated */
void mapstream_pool(unsigned max);
/* Once per frame: finish / start reads, free far cells (blobs drawn in the last two frames are kept: the VIF chains
 * still read them) */
void mapstream_update(const float pos[3]);
/* Cell i's near blob (chunk records + streams) if loaded, else NULL; marks it used this frame */
const void *mapstream_get(unsigned i);

/* cells / bytes: every loaded item (near cells, resident cells, textures); res_kb / tex_kb: the resident cells' and
 * mip 0 textures' share; other_kb: mapstream_alloc, lent_kb: mapstream_lend; room_fails: a wanted item found no room;
 * vis_items / vis_kb: items (loaded or not) the visibility leaves out now (pvs.c) */
typedef struct {
    unsigned cells, bytes, loads, fails, pending, ms_last, other_kb, lent_kb, res_kb, tex_kb, room_fails, vis_items, vis_kb;
} MapStreamStats;
extern MapStreamStats mapstream_stats;

#endif
