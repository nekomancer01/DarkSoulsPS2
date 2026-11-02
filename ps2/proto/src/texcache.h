/* Map texture streaming: a VRAM pool holding the textures drawn recently (LRU), filled by uploads in the map's VIF1
 * chain (PATH2, right before the first batch drawing with them) and skipped when the texture is already resident.
 * Textures come from map.bin's texture section (tools/texmap.py: PSMT4 / PSMT8, mip 0 + 1); their CLUTs stay resident. */
#ifndef TEXCACHE_H
#define TEXCACHE_H

#include <gsKit.h>

/* base: map.bin, table: its texture section (ntex entries). Allocates the pool (pool_kb) and one block for all CLUTs.
 * Returns 0, or -1 if VRAM ran out. */
int tc_init(GSGLOBAL *gs, const unsigned char *base, const void *table, int ntex, int pool_kb);
/* More textures, after tc_init (same entry format; pixel offsets from base, which stays loaded): no CLUT of their own
 * (clut offset 0), the caller sets GSTEXTURE.VramClut per draw. pool_kb > 0: they get a VRAM pool of their own (the
 * first call allocates it), so they never evict the map's textures; 0: they share the map's pool. Returns the first new
 * index, or -1 when the table or VRAM is full. */
int tc_add(const unsigned char *base, const void *table, int n, int pool_kb);
/* World blocks (src/world.c via map.c): a further map's textures in index range TC_BASE + slot x TC_BLOCK_TEX, CLUTs
 * in VRAM taken at tc_block_add and given back at tc_block_drop (one allocation each), pixels share the pool.
 * base = that map.bin (texture offsets from it). Returns the first index or -1. */
#define TC_BLOCKS 3
#define TC_BLOCK_TEX 192
int tc_block_add(int slot, const unsigned char *base, const void *table, int n);
void tc_block_drop(int slot);
/* A world block's normal maps (bump.c, while the block is full): entries as tc_add's (no CLUT), placed after the slot's
 * map textures, pixels in tc_add's relief pool. n = 0 / table NULL forgets the slot's set. Returns the first index or
 * -1 (block off, no room, no relief pool). tc_block_drop forgets them too. */
int tc_block_bump(int slot, const unsigned char *base, const void *table, int n);
/* Split maps (tools/mapsplit.py): an entry whose pix[0] has TC_RES set keeps its mip 0 in the block's mapres.bin at
 * that offset, present only while the block is full. tc_res gives the textures of slot (-1: the boot map's, else a
 * block slot) that buffer, or NULL: mip 0 is then drawn only while still in VRAM, else mip 1. The caller keeps the old
 * buffer until the chains that uploaded from it are done. */
#define TC_RES 0x80000000u
void tc_res(int slot, const unsigned char *res);
/* Once per map chain, after vu1_begin, before any map batch (sets TEXA for the CT16 CLUTs; the first call also uploads
 * the CLUTs). focal: screen focal length in pixels (mip choice). */
void tc_begin(float focal);
/* Mip level for a texture seen from d metres (nearest point of the batch): 1 once a mip 0 texel gets smaller than
 * ~0.7 pixel */
int tc_level(int tex, float d);
/* The texture to draw with (between tc_begin and tc_end): resident until the end of the frame, uploaded first if
 * needed. Falls back to the other mip level when the pool has no room for this one; NULL if neither fits. */
GSTEXTURE *tc_get(int tex, int level);
/* GS mip chain under t (what tc_get returned for tex: mip 0 or 1; mips 2 and 3 from texmap.py's MIP_CHAIN), resident
 * for the frame; returns MXL (0: none) and fills MIPTBP1 and TEX1.K for a batch with st scale st_k (see texcache.c) */
int tc_mip(int tex, const GSTEXTURE *t, float st_k, u64 *miptbp1, int *k);
/* End of the map's batches (stats) */
void tc_end(void);

typedef struct {
    int uploads, bytes;  /* this frame: textures uploaded, bytes (CLUTs at the start included) */
    int first_bytes;     /* the first frame (all CLUTs + every texture in view) */
    int max_bytes;       /* largest frame since */
    int frame_kb;        /* pool KB drawn from this frame (the working set) */
    int max_frame_kb;    /* largest working set so far */
    int fails;           /* tc_get calls that returned NULL, this frame */
    int resident;        /* KB of the pool in use */
    int pool;            /* pool KB */
    int mip_batches;     /* this frame: batches drawn with a GS mip chain (tc_mip) */
} TcStats;
extern TcStats tc_stats;

#endif
