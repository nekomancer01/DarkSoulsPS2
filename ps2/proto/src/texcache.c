/* Map texture cache. VRAM: a pool of 256-byte GS blocks; each texture level takes a power-of-two run of blocks at an
 * offset aligned to its size (a buddy layout), so a level's blocks never straddle another's. Allocation picks the
 * aligned run whose newest occupant was drawn longest ago (free runs first) and evicts what is in it; levels drawn this
 * frame are never evicted, because the EE-clipped triangles (gsKit, end of the frame) still use them.
 *
 * Uploads (R&C 1 / Jak pattern): a texture that is not resident is uploaded in the map's VIF1 chain right before the
 * first batch that draws with it (vu1_upload_tex: queued on PATH3 behind one FLUSHA gate per segment, see vu1.c;
 * -DVU1_TEX_PATH2 builds: PATH2 image transfer straight from map.bin, then TEXFLUSH); resident ones cost nothing (Jak
 * II). The chain may be sent in several segments (vu1.c room()); order is kept either way.
 *
 * Near level (TECH_DEBT map-2 / texstream-5): textures with a 2x level (texmap.add_near: twice mip 0's size, same
 * palette) draw it on chunks close enough for its texels to stay >= 1 / MIP_K px. Near levels live in a pool of their
 * own (TC_NEAR_KB), so they never evict mip 0 / 1; at most TC_NEAR_UP bytes of them are uploaded a frame (no hitch),
 * beyond that or with the pool full the batch takes mip 0. */
#include <kernel.h>
#include <string.h>
#include <stdio.h>
#include "texcache.h"
#include "vu1.h"
#include "render.h"

/* map.bin texture entry (tools/texmap.py) */
typedef struct {
    unsigned short w, h;
    unsigned char psm, cpsm, levels, pad;
    float density;        /* texels per metre at mip 0 */
    unsigned clut, pix[2];
    unsigned short blocks[2];
    unsigned near;  /* near level pixels (flag bit 0 in pad), 0: none; with TE_CHAIN_AT: the mip 2 / 3 chain */
} TexEntry;
/* pad bit 1 (tools/mapsplit.py MIP1_OUT, the Burg plan's lite set diet): mip 1 streams from mapres.bin (pix[1] has
 * TC_RES) and near holds the maptab offset of mip 2 (mip 3 follows it); mip 2 is then the resident level drawn until
 * mip 1 is in */
#define TE_CHAIN_AT 2
/* World blocks of one map share the textures two or more of them have (tools/mapsplit.py TEX_PEERS, maptex.bin): an
 * offset with TC_SHR set is in the map's shared buffer (tc_block_shared), not in the block's maptab */
static const unsigned char *sh_cur;  /* the shared buffer of the slot tc_block_add is setting up */
static const unsigned char *blk_sh[TC_BLOCKS];
static inline const unsigned char *tpix(const unsigned char *base, unsigned off)
{
    return (off & TC_SHR) ? sh_cur + (off & ~TC_SHR) : base + off;
}

typedef struct {
    GSTEXTURE t;
    int off;        /* block offset in the pool, -1: not resident */
    unsigned last;  /* frame it was last drawn */
    int size;       /* blocks */
} Level;

#define TC_BASE 192  /* map textures + tc_add's (bump.c normal maps), then TC_BLOCKS world blocks' ranges */
#define MAX_TEX (TC_BASE + TC_BLOCKS * TC_BLOCK_TEX)
#define POOL_MAX (1024 * 1024 / 256)
#define MIP_K 1.41f  /* mip 1 once a mip 0 texel is below 1 / MIP_K pixels */
#ifndef TC_NEAR_KB
#define TC_NEAR_KB 256  /* near-level pool (only allocated when the map has near levels) */
#endif
/* Upload caps per frame (budget doc proposal 7, 2026-11-08): 512 KB of near and chain levels together over PATH3
 * (~0.5 ms of GS time as CT32 by the SDK's rates, ASSUMED; PCSX2 does not time it), mip 0 / 1 as before uncapped (a
 * miss there is a missing batch). make DEFS=-DTC_UP_OLD = the old 96 + 16 KB. */
#ifdef TC_UP_OLD
#define TC_NEAR_UP (96 * 1024)  /* near-level upload bytes per frame */
#else
#define TC_NEAR_UP (384 * 1024)
#endif

static const TexEntry *te[MAX_TEX];  /* map.bin's table, then tc_add's */
static const unsigned char *bin;
static int ntex, pool_blk;
static u32 pool_vram, clut_vram;
/* tc_add's textures in a pool of their own (when given one): they never evict the map's textures */
static int nmap, pool2_blk;
static u32 pool2_vram;
static int pool3_blk, near_up, near_get, near_want;  /* near levels' pool; near bytes uploaded / drawn / asked this frame */
static u32 pool3_vram;
static GSGLOBAL *gsg;
static int pin_off, pin_blk;  /* tc_pin: the top of the map pool lent to a menu screen, none while pin_blk is 0 */
#define NLV 5
static Level lv[MAX_TEX][NLV];  /* mip 0, mip 1, near, mip 2, mip 3 (the GS mip chain under mip 0 / 1, tc_mip) */
static unsigned frame = 1;
static float mip_d[MAX_TEX];
static float tc_focal = 487;
/* chain level bytes uploaded a frame, at most (no hitch when a teleport or a turn brings many textures at once: those
 * batches draw without a chain until their levels are in; ASSUMED 16 KB, a few textures' mip 1 / 2 / 3) */
#ifdef TC_UP_OLD
#define TC_MIP_UP (16 * 1024)
#else
#define TC_MIP_UP (128 * 1024)
#endif
static int mip_up;
static unsigned mip_short;  /* chains cut short this frame (the "tex:" log) */
/* tc_mip's LOD bias in levels (ASSUMED 1: the next level once a texel is under 2 pixels by view depth; the GS LOD has
 * no slope term, so walls seen at a grazing angle need it. Measured at the user's ring-wall spot, render-12: flicker
 * 17.4k -> 14.7k at 0, 12.8k at 1, 16.1k at -1; near textures as sharp (spawn view gradient 2.75 -> 2.74)) */
#ifndef MIP_BIAS
#define MIP_BIAS 1.0f
#endif
/* world blocks (tc_block_add): CLUT VRAM taken while the block is on, CLUTs uploaded at the next tc_begin */
static int blk_n[TC_BLOCKS], blk_clut_due[TC_BLOCKS];
static int blk_bump[TC_BLOCKS];  /* tc_block_bump: normal maps after the slot's map textures (bump.c relief pool) */

/* pool of a level: 0 map, 1 tc_add's (if it has its own), 2 near levels */
static int pool_of(int i, int l)
{
    if (l == 2)
        return 2;
    if (i >= TC_BASE) {  /* a world block's normal maps share tc_add's pool */
        int b = (i - TC_BASE) / TC_BLOCK_TEX;
        return i - TC_BASE - b * TC_BLOCK_TEX >= blk_n[b] && pool2_blk;
    }
    return i >= nmap && pool2_blk;
}

/* VRAM blocks of a w x h near level, buffer width w (vu1.c TBW): whole pages (PSMT8 128 x 64, PSMT4 128 x 128) */
static int near_blocks(int psm, int w, int h)
{
    int ph = psm == GS_PSM_T4 ? 128 : 64, n = 1, top = (w + 127) / 128 * ((h + ph - 1) / ph) * 32;
    while (n < top)
        n *= 2;
    return n;
}
/* texture i's near level (same CLUT as mip 0, twice the size; pixels from base, in a world block's maptab), if it has
 * one: the first allocates the near pool (a world block's too: its textures draw near levels as the boot map's) */
static void near_level(int i, const unsigned char *base)
{
    const TexEntry *e = te[i];
    Level *v = &lv[i][2];
    v->t = lv[i][0].t;
    v->off = -1, v->last = 0, v->size = 0;
    if (!(e->pad & 1) || !e->near)
        return;
    v->t.Width = e->w * 2, v->t.Height = e->h * 2;
    v->t.Mem = (u32 *)tpix(base, e->near);
    v->size = near_blocks(e->psm, e->w * 2, e->h * 2);
    if (!pool3_blk) {  /* first near level: allocate its pool */
        pool3_blk = TC_NEAR_KB * 4 < POOL_MAX ? TC_NEAR_KB * 4 : POOL_MAX;
        pool3_vram = gsKit_vram_alloc(gsg, pool3_blk * 256, GSKIT_ALLOC_USERBUFFER);
        if (pool3_vram == GSKIT_ALLOC_ERROR)
            pool3_blk = 0;
    }
}

/* level i's slots forgotten (not resident), near / chain levels absent */
static void lv_reset(int i)
{
    for (int l = 0; l < NLV; l++)
        lv[i][l].off = -1, lv[i][l].size = l < 2 ? lv[i][l].size : 0;
}

/* VRAM blocks of a w x h level at buffer width 128 (texmap.blocks): the highest GS block it touches + 1, rounded up to a
 * power of two */
static int level_blocks(int psm, int w, int h)
{
    static const unsigned char b8[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
                                           {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
    static const unsigned char b4[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
                                           {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
    int t4 = psm == GS_PSM_T4, bw = t4 ? 32 : 16, ph = t4 ? 128 : 64, top = 0, n = 1;
    for (int y = 0; y < h; y += 16)
        for (int x = 0; x < w; x += bw) {
            int b = y / ph * 32 + (t4 ? b4[y % ph / 16][x % 128 / bw] : b8[y % ph / 16][x % 128 / bw]);
            top = b + 1 > top ? b + 1 : top;
        }
    while (n < top)
        n *= 2;
    return n;
}

/* texture i's mip 2 and 3 (texmap.py MIP_CHAIN: after mip 1, each 16-byte aligned), when its entry has them */
static void chain_levels(int i, const unsigned char *base)
{
    const TexEntry *e = te[i];
    unsigned off = e->pix[1];
    for (int m = 2; m < 4; m++) {
        Level *v = &lv[i][m + 1];
        v->off = -1, v->last = 0, v->size = 0;
        if (e->levels <= m)
            continue;
        int w = e->w >> (m - 1), h = e->h >> (m - 1);
        if (m == 2 && (e->pad & TE_CHAIN_AT))
            off = e->near;  /* mip 1 streams (mapsplit.py MIP1_OUT): the chain's own offset */
        else
            off += ((w * h >> (e->psm == GS_PSM_T4)) + 15) & ~15;  /* past the level before */
        v->t = lv[i][1].t;
        v->t.Width = e->w >> m, v->t.Height = e->h >> m;
        v->t.Mem = (u32 *)tpix(base, off);
        v->size = level_blocks(e->psm, v->t.Width, v->t.Height);
    }
}

/* resident levels (TC_RLIST, default on; make DEFS=-DTC_RLIST=0 = the old walk over every texture's levels): alloc and
 * tc_end walk this list instead of all MAX_TEX x NLV levels (3840: a texture miss cost ~0.1 ms, a few hundred of
 * them after a turn or warp up to 10 ms of map pass, rd_firelink 2026-11-08). Entries are added by load and dropped
 * lazily once their level is no longer resident (lv_reset, eviction, tc_pin set off = -1 anywhere). */
#ifndef TC_RLIST
#define TC_RLIST 1
#endif
static unsigned short rl[MAX_TEX * NLV];
static int rl_n;
static unsigned char rl_in[MAX_TEX * NLV];
static void rl_add(int i, int l)
{
    unsigned id = (unsigned)i * NLV + (unsigned)l;
    if (!rl_in[id])
        rl_in[id] = 1, rl[rl_n++] = (unsigned short)id;
}
#if TC_RLIST
/* the list without entries no longer resident */
static void rl_trim(void)
{
    for (int j = 0; j < rl_n;) {
        unsigned id = rl[j];
        if (lv[id / NLV][id % NLV].off < 0)
            rl_in[id] = 0, rl[j] = rl[--rl_n];
        else
            j++;
    }
}
#endif

static int cluts_sent;
static int in_frame;
TcStats tc_stats;

/* one GS image transfer into the chain; counted in tc_stats */
static int upload(u32 tbp, int psm, int w, int h, const void *pix, int bytes)
{
    if (!vu1_upload_tex(tbp, psm, w, h, pix, bytes))
        return 0;
    tc_stats.uploads++;
    tc_stats.bytes += bytes;
    return 1;
}

static int clut_blocks(const TexEntry *e)
{
    return e->psm == GS_PSM_T4 ? 1 : e->cpsm == GS_PSM_CT32 ? 4 : 2;  /* 8x2 (one block), 16x16 CT32 / CT16 */
}

int tc_init(GSGLOBAL *gs, const unsigned char *base, const void *table, int n, int pool_kb)
{
    bin = base;
    ntex = n < TC_BASE ? n : TC_BASE;
    for (int i = 0; i < MAX_TEX; i++)
        lv_reset(i);
    for (int i = 0; i < ntex; i++)
        te[i] = (const TexEntry *)table + i;
    pool_blk = pool_kb * 4 < POOL_MAX ? pool_kb * 4 : POOL_MAX;
    nmap = ntex, pool2_blk = 0, pool3_blk = 0;
    gsg = gs;
    int cb = 0;
    for (int i = 0; i < ntex; i++)
        cb += clut_blocks(te[i]);
    clut_vram = gsKit_vram_alloc(gs, cb * 256, GSKIT_ALLOC_USERBUFFER);
    pool_vram = gsKit_vram_alloc(gs, pool_blk * 256, GSKIT_ALLOC_USERBUFFER);
    if (clut_vram == GSKIT_ALLOC_ERROR || pool_vram == GSKIT_ALLOC_ERROR)
        return -1;
    u32 c = clut_vram;
    for (int i = 0; i < ntex; i++) {
        const TexEntry *e = te[i];
        for (int l = 0; l < 2; l++) {
            GSTEXTURE *t = &lv[i][l].t;
            memset(t, 0, sizeof *t);
            t->Width = e->w >> l;
            t->Height = e->h >> l;
            t->PSM = e->psm;
            t->ClutPSM = e->cpsm;
            t->ClutStorageMode = GS_CLUT_STORAGE_CSM1;
            t->Filter = GS_FILTER_LINEAR;
            t->Mem = (e->pix[l] & TC_RES) ? NULL : (u32 *)(bin + e->pix[l]);  /* tc_res */
            t->Clut = (u32 *)(bin + e->clut);
            t->VramClut = c;
            lv[i][l].off = -1;
            lv[i][l].last = 0;
            lv[i][l].size = e->blocks[l];
        }
        near_level(i, bin);
        chain_levels(i, bin);
        c += clut_blocks(e) * 256;
    }
    cluts_sent = 0;
    tc_stats.pool = pool_blk / 4;
    tc_stats.max_bytes = 0;
    FlushCache(0);
    return 0;
}

int tc_add(const unsigned char *base, const void *table, int n, int pool_kb)
{
    int first = ntex;
    if (ntex + n > TC_BASE)
        return -1;
    if (pool_kb > 0 && !pool2_blk) {
        pool2_blk = pool_kb * 4 < POOL_MAX ? pool_kb * 4 : POOL_MAX;
        pool2_vram = gsKit_vram_alloc(gsg, pool2_blk * 256, GSKIT_ALLOC_USERBUFFER);
        if (pool2_vram == GSKIT_ALLOC_ERROR)
            return pool2_blk = 0, -1;
    }
    /* entries with their own CLUT (map.c's far panorama): CLUT VRAM of their own, sent with the others on the first
     * frame (so tc_add them before it) */
    int cb = 0;
    for (int i = 0; i < n; i++)
        cb += ((const TexEntry *)table)[i].clut ? clut_blocks((const TexEntry *)table + i) : 0;
    u32 cv = cb ? gsKit_vram_alloc(gsg, cb * 256, GSKIT_ALLOC_USERBUFFER) : 0;
    if (cb && cv == GSKIT_ALLOC_ERROR)  /* GSKIT_ALLOC_ERROR is 0: without CLUTs (bump.c's normal maps) cv = 0 is no error */
        return -1;
    for (int i = 0; i < n; i++, ntex++) {
        const TexEntry *e = te[ntex] = (const TexEntry *)table + i;
        for (int l = 0; l < 2; l++) {
            GSTEXTURE *t = &lv[ntex][l].t;
            memset(t, 0, sizeof *t);
            t->Width = e->w >> l;
            t->Height = e->h >> l;
            t->PSM = e->psm;
            t->ClutPSM = e->cpsm;
            t->ClutStorageMode = GS_CLUT_STORAGE_CSM1;
            t->Filter = GS_FILTER_LINEAR;
            t->Mem = (u32 *)(base + e->pix[l]);
            if (e->clut)
                t->Clut = (u32 *)(base + e->clut), t->VramClut = cv;
            lv[ntex][l].off = -1;
            lv[ntex][l].last = 0;
            lv[ntex][l].size = e->blocks[l];
        }
        lv_reset(ntex);
        if (e->clut)
            cv += clut_blocks(e) * 256;
    }
    return first;
}

/* world block slot b's textures: indices TC_BASE + b x TC_BLOCK_TEX .. + n, CLUTs in VRAM of their own (reserved
 * while the block is on, one allocation each), pixels in the map's pool. base: the block's map.bin (texture offsets
 * are from it), kept until tc_block_drop. Returns the first index, or -1 (too many textures, VRAM full). */
void tc_block_shared(int b, const void *buf)
{
    if (b >= 0 && b < TC_BLOCKS)
        blk_sh[b] = buf;
}

int tc_block_add(int b, const unsigned char *base, const void *table, int n)
{
    if (b < 0 || b >= TC_BLOCKS || n > TC_BLOCK_TEX)
        return -1;
    tc_block_drop(b);
    int first = TC_BASE + b * TC_BLOCK_TEX;
    sh_cur = blk_sh[b];
    for (int i = 0; i < n; i++) {
        const TexEntry *e = te[first + i] = (const TexEntry *)table + i;
        /* its CLUT's VRAM, one allocation each (fills the holes characters and blocks leave), until tc_block_drop */
        u32 c = e->psm == GS_PSM_T4 ? r_vram_lin(gsg, 1) : r_vram_clut(gsg, e->cpsm);
        if (c == GSKIT_ALLOC_ERROR) {
            te[first + i] = NULL;
            blk_n[b] = i;
            tc_block_drop(b);
            return -1;
        }
        for (int l = 0; l < 2; l++) {
            GSTEXTURE *t = &lv[first + i][l].t;
            memset(t, 0, sizeof *t);
            t->Width = e->w >> l;
            t->Height = e->h >> l;
            t->PSM = e->psm;
            t->ClutPSM = e->cpsm;
            t->ClutStorageMode = GS_CLUT_STORAGE_CSM1;
            t->Filter = GS_FILTER_LINEAR;
            t->Mem = (e->pix[l] & TC_RES) ? NULL : (u32 *)tpix(base, e->pix[l]);  /* tc_res */
            t->Clut = (u32 *)tpix(base, e->clut);
            t->VramClut = c;
            lv[first + i][l].off = -1;
            lv[first + i][l].last = 0;
            lv[first + i][l].size = e->blocks[l];
        }
        near_level(first + i, base);
        chain_levels(first + i, base);
    }
    blk_n[b] = n;
    blk_clut_due[b] = 1;
    return first;
}

/* Forget slot b's textures (their pool blocks become free); the caller keeps the pixels until the chains that used
 * them are done (two frames) */
void tc_block_drop(int b)
{
    if (b < 0 || b >= TC_BLOCKS)
        return;
    for (int i = TC_BASE + b * TC_BLOCK_TEX; i < TC_BASE + b * TC_BLOCK_TEX + blk_n[b] + blk_bump[b]; i++) {
        if (i < TC_BASE + b * TC_BLOCK_TEX + blk_n[b]) {  /* the block is off: its CLUTs' VRAM goes back */
            const GSTEXTURE *t = &lv[i][0].t;
            if (t->PSM == GS_PSM_T4)
                r_vram_free_lin(t->VramClut, 1);
            else
                r_vram_free_clut(t->VramClut, t->ClutPSM);
        }
        te[i] = NULL;
        lv_reset(i);
    }
    blk_n[b] = blk_clut_due[b] = blk_bump[b] = 0;
}

int tc_relief_pool(int pool_kb)
{
    if (!pool2_blk && pool_kb > 0) {
        pool2_blk = pool_kb * 4 < POOL_MAX ? pool_kb * 4 : POOL_MAX;
        pool2_vram = gsKit_vram_alloc(gsg, pool2_blk * 256, GSKIT_ALLOC_USERBUFFER);
        if (pool2_vram == GSKIT_ALLOC_ERROR)
            pool2_blk = 0;
    }
    return pool2_blk != 0;
}

int tc_base_bump(const unsigned char *base, const void *table, int n, int pool_kb)
{
    static int first = -1, nb;
    if (first >= 0)
        for (int i = first; i < first + nb; i++)  /* the old buffer's: forgotten, their pool blocks free */
            te[i] = NULL, lv_reset(i);
    if (!table || n <= 0)
        return -1;
    if (first < 0) {
        first = tc_add(base, table, n, pool_kb);
        nb = first >= 0 ? n : 0;
        return first;
    }
    if (n != nb)
        return -1;
    for (int i = 0; i < n; i++) {  /* as tc_add's entries (normal maps have no CLUT of their own) */
        const TexEntry *e = te[first + i] = (const TexEntry *)table + i;
        for (int l = 0; l < 2; l++) {
            GSTEXTURE *t = &lv[first + i][l].t;
            memset(t, 0, sizeof *t);
            t->Width = e->w >> l;
            t->Height = e->h >> l;
            t->PSM = e->psm;
            t->ClutPSM = e->cpsm;
            t->ClutStorageMode = GS_CLUT_STORAGE_CSM1;
            t->Filter = GS_FILTER_LINEAR;
            t->Mem = (u32 *)(base + e->pix[l]);
            lv[first + i][l].off = -1;
            lv[first + i][l].last = 0;
            lv[first + i][l].size = e->blocks[l];
        }
        lv_reset(first + i);
    }
    return first;
}

int tc_block_bump(int b, const unsigned char *base, const void *table, int n)
{
    if (b < 0 || b >= TC_BLOCKS)
        return -1;
    int first = TC_BASE + b * TC_BLOCK_TEX + blk_n[b];
    for (int i = first; i < first + blk_bump[b]; i++) {  /* the old set: forgotten, its pool blocks free */
        te[i] = NULL;
        lv_reset(i);
    }
    blk_bump[b] = 0;
    if (!table || !n)
        return -1;
    if (!blk_n[b] || blk_n[b] + n > TC_BLOCK_TEX || !pool2_blk)
        return -1;  /* no map textures in the slot (block off), no room, or no relief pool (the boot area has none) */
    for (int i = 0; i < n; i++) {  /* as tc_add's entries: no CLUT of their own */
        const TexEntry *e = te[first + i] = (const TexEntry *)table + i;
        for (int l = 0; l < 2; l++) {
            GSTEXTURE *t = &lv[first + i][l].t;
            memset(t, 0, sizeof *t);
            t->Width = e->w >> l;
            t->Height = e->h >> l;
            t->PSM = e->psm;
            t->ClutPSM = e->cpsm;
            t->ClutStorageMode = GS_CLUT_STORAGE_CSM1;
            t->Filter = GS_FILTER_LINEAR;
            t->Mem = (u32 *)(base + e->pix[l]);
            lv[first + i][l].off = -1;
            lv[first + i][l].last = 0;
            lv[first + i][l].size = e->blocks[l];
        }
        lv_reset(first + i);
    }
    blk_bump[b] = n;
    return first;
}

void tc_res(int slot, const unsigned char *res)
{
    int a = slot < 0 ? 0 : TC_BASE + slot * TC_BLOCK_TEX, n = slot < 0 ? nmap : slot < TC_BLOCKS ? blk_n[slot] : 0;
    for (int i = a; i < a + n; i++)
        for (int l = 0; l < 2; l++)
            if (te[i] && (te[i]->pix[l] & TC_RES))
                lv[i][l].t.Mem = res ? (u32 *)(res + (te[i]->pix[l] & ~TC_RES)) : NULL;
}

static unsigned res_asked[MAX_TEX][2];  /* frame + 1 of the last tc_get asking for a TC_RES mip 0 / 1 (tc_res_age) */

static int res_index(int slot, int i, int l)
{
    int a = slot < 0 ? 0 : TC_BASE + slot * TC_BLOCK_TEX, n = slot < 0 ? nmap : slot < TC_BLOCKS ? blk_n[slot] : 0;
    return l >= 0 && l < 2 && i >= 0 && i < n && te[a + i] && (te[a + i]->pix[l] & TC_RES) ? a + i : -1;
}

void tc_res_one(int slot, int i, int l, const void *pix)
{
    int t = res_index(slot, i, l);
    if (t >= 0)
        lv[t][l].t.Mem = (u32 *)pix;
    if (t >= 0 && !pix)
        res_asked[t][l] = 0;
}

unsigned tc_res_age(int slot, int i, int l)
{
    int t = res_index(slot, i, l);
    return t < 0 || !res_asked[t][l] ? ~0u : frame - (res_asked[t][l] - 1);
}

/* textures to walk: the boot map's and tc_add's, then the world block ranges in use */
static int tex_end(void)
{
    for (int b = TC_BLOCKS - 1; b >= 0; b--)
        if (blk_n[b])
            return TC_BASE + b * TC_BLOCK_TEX + blk_n[b] + blk_bump[b];
    return ntex;
}

/* aligned run of s blocks in pool p (pool_of): free, else the one drawn longest ago (not this frame); evicts its
 * occupants */
static unsigned fail_fr;
static int fail_s[3];
static int alloc(int s, int p)
{
    static unsigned score[POOL_MAX];  /* per run: 0 free, else newest occupant's frame + 1, ~0 drawn this frame */
    if (s <= 0)
        return -1;
    /* this frame a run of fail_s[p] or fewer blocks found nothing to evict in pool p: larger ones cannot either (runs
     * only fill up within a frame) */
    if (fail_fr == frame && fail_s[p] && s >= fail_s[p])
        return -1;
    int nrun = (p == 2 ? pool3_blk : p ? pool2_blk : pool_blk) / s;
    if (!nrun)
        return -1;
    memset(score, 0, nrun * sizeof *score);
    int nall = tex_end();
#if TC_RLIST
    rl_trim();
    for (int j = 0; j < rl_n; j++) {
        int i = rl[j] / NLV, l = rl[j] % NLV;
        const Level *v = &lv[i][l];
        if (pool_of(i, l) != p)
            continue;
        unsigned sc = v->last == frame ? ~0u : v->last + 1;
        for (int r = v->off / s; r < nrun && r * s < v->off + v->size; r++)
            score[r] = sc > score[r] ? sc : score[r];
    }
#else
    for (int i = 0; i < nall; i++)
        for (int l = 0; l < NLV; l++) {
            const Level *v = &lv[i][l];
            if (v->off < 0 || pool_of(i, l) != p)
                continue;
            unsigned sc = v->last == frame ? ~0u : v->last + 1;
            for (int r = v->off / s; r < nrun && r * s < v->off + v->size; r++)
                score[r] = sc > score[r] ? sc : score[r];
        }
#endif
    if (p == 0 && pin_blk)  /* runs in the lent top of the pool are taken */
        for (int r = pin_off / s; r < nrun; r++)
            score[r] = ~0u;
    int best = -1;
    unsigned long long bk = ~0ull;
    for (int r = 0; r < nrun; r++) {
        /* among free runs prefer one whose buddy is in use (keeps large free runs whole) */
        unsigned long long k = (unsigned long long)score[r] * 2 + (!score[r] && ((r ^ 1) >= nrun || !score[r ^ 1]));
        if (k < bk)
            bk = k, best = r;
    }
    if (best < 0 || score[best] == ~0u) {
        if (fail_fr != frame)
            fail_fr = frame, fail_s[0] = fail_s[1] = fail_s[2] = 0;
        fail_s[p] = !fail_s[p] || s < fail_s[p] ? s : fail_s[p];
        return -1;
    }
    int off = best * s;
#if TC_RLIST
    (void)nall;
    for (int j = 0; j < rl_n; j++) {
        int i = rl[j] / NLV, l = rl[j] % NLV;
        Level *v = &lv[i][l];
        if (v->off >= 0 && pool_of(i, l) == p && v->off < off + s && v->off + v->size > off)
            v->off = -1;
    }
#else
    for (int i = 0; i < nall; i++)
        for (int l = 0; l < NLV; l++) {
            Level *v = &lv[i][l];
            if (v->off >= 0 && pool_of(i, l) == p && v->off < off + s && v->off + v->size > off)
                v->off = -1;
        }
#endif
    return off;
}

static int load(int tex, int l)
{
    Level *v = &lv[tex][l];
    const TexEntry *e = te[tex];
    int p = pool_of(tex, l);
    int bytes = (v->t.Width * v->t.Height) >> (e->psm == GS_PSM_T4);
    if (!v->t.Mem)
        return 0;  /* mip 0 of a lite block (tc_res) */
    if (p == 2 && near_up + bytes > TC_NEAR_UP)
        return 0;  /* this frame's near uploads are spent: mip 0 */
    int off = alloc(v->size, p);
    if (off < 0)
        return 0;
    u32 tbp = ((p == 2 ? pool3_vram : p ? pool2_vram : pool_vram) + off * 256) / 256;
    if (p == 2)
        near_up += bytes;
    if (!upload(tbp, e->psm, v->t.Width, v->t.Height, v->t.Mem, bytes))
        return 0;
    v->off = off;
    v->t.Vram = tbp * 256;
    v->last = frame;
    rl_add(tex, l);
    return 1;
}

void tc_begin(float focal)
{
    frame++;
    tc_focal = focal, mip_up = 0;
    tc_stats.uploads = tc_stats.bytes = tc_stats.fails = tc_stats.mip_batches = 0;
    near_up = near_get = near_want = 0;
    in_frame = 1;
    /* CT16 CLUT alpha bit 0 -> 0 (cut-out texels, alpha test drops them), 1 -> 0x80 */
    u64 texa[2] = {GS_SETREG_TEXA(0, 0, 0x80), GS_TEXA};
    vu1_gs_regs(1, texa);
    int nall = tex_end();
    for (int i = 0; i < nall; i++)
        mip_d[i] = te[i] && te[i]->levels > 1 && te[i]->density > 0 ? MIP_K * focal / te[i]->density : 1e9f;
    if (!cluts_sent) {  /* first frame: every CLUT (they stay resident) */
        for (int i = 0; i < ntex; i++) {
            const TexEntry *e = te[i];
            if (!e->clut)  /* tc_add entries: CLUT chosen per batch by their user */
                continue;
            int w = e->psm == GS_PSM_T8 ? 16 : 8, h = e->psm == GS_PSM_T8 ? 16 : 2;
            upload(lv[i][0].t.VramClut / 256, e->cpsm, w, h, (const unsigned char *)lv[i][0].t.Clut,
                   w * h * (e->cpsm == GS_PSM_CT32 ? 4 : 2));
        }
        cluts_sent = 1;
        vu1_clut_reload();  /* TEX0 CLD 4 compares CBP only: force a load after writing CLUT memory */
    }
    for (int b = 0; b < TC_BLOCKS; b++)  /* a world block's CLUTs, the frame after tc_block_add */
        if (blk_clut_due[b]) {
            for (int i = TC_BASE + b * TC_BLOCK_TEX; i < TC_BASE + b * TC_BLOCK_TEX + blk_n[b]; i++) {
                const TexEntry *e = te[i];
                int w = e->psm == GS_PSM_T8 ? 16 : 8, h = e->psm == GS_PSM_T8 ? 16 : 2;
                upload(lv[i][0].t.VramClut / 256, e->cpsm, w, h, lv[i][0].t.Clut, w * h * (e->cpsm == GS_PSM_CT32 ? 4 : 2));
            }
            blk_clut_due[b] = 0;
            vu1_clut_reload();
        }
}

/* a streamed mip 0 is asked for from MIP0_AHEAD x its switch distance on (Burg plan B9, TECH_DEBT texstream-5): asked
 * only where drawn, it arrived a few frames after the switch and the texture sharpened on screen (the mip 1 -> mip 0
 * step); asked ahead it is in by the time the camera crosses mip_d. ASSUMED 1.3 (~1-2 s at walking speed for the
 * Burg's 5-10 m switch distances). make DEFS=-DMIP0_AHEAD=0: asked where drawn */
#ifndef MIP0_AHEAD
#define MIP0_AHEAD 1.3f
#endif

int tc_level(int tex, float d)
{
    if (tex < 0 || tex >= MAX_TEX || !te[tex])
        return 0;
#ifndef TC_NO_NEAR  /* A/B builds: near levels never drawn */
    if (lv[tex][2].size && pool3_blk && d < mip_d[tex] * 0.5f)
        return 2;  /* near level texels (half mip 0's) still >= 1 / MIP_K px */
#endif
    if (MIP0_AHEAD > 0 && d > mip_d[tex] && d < mip_d[tex] * MIP0_AHEAD && (te[tex]->pix[0] & TC_RES) && in_frame)
        res_asked[tex][0] = frame + 1;  /* mip 1 drawn, mip 0 read ahead */
    return d > mip_d[tex];
}

GSTEXTURE *tc_get(int tex, int level)
{
    if (tex < 0 || tex >= MAX_TEX || !te[tex] || !in_frame)
        return NULL;
    if (level == 2 ? !lv[tex][2].size : level >= te[tex]->levels)
        level = 0;
    near_want += level == 2;
    if (level < 2 && (te[tex]->pix[level] & TC_RES))
        res_asked[tex][level] = frame + 1;  /* a streamed mip 0 / 1 (tc_res_one) is wanted */
    Level *v = &lv[tex][level];
    if (v->off < 0 && !load(tex, level)) {
        int o = level == 2 ? 0 : level ^ 1;
        v = &lv[tex][o];
        if (o == 1 && o < te[tex]->levels && (te[tex]->pix[1] & TC_RES))
            res_asked[tex][1] = frame + 1;  /* the fallback streams too */
        if (o >= te[tex]->levels || (v->off < 0 && !load(tex, o))) {
            /* a streamed mip 1 not in yet (mapsplit.py MIP1_OUT): the resident chain's mip 2, then mip 3 */
            for (o = 3; o < 5 && (te[tex]->pad & TE_CHAIN_AT); o++)
                if (lv[tex][o].size && (lv[tex][o].off >= 0 || load(tex, o))) {
                    lv[tex][o].last = frame;
                    return &lv[tex][o].t;
                }
            tc_stats.fails++;
            return NULL;
        }
    }
    v->last = frame;
    near_get += v == &lv[tex][2];
    return &v->t;
}

/* GS mip chain under t = tc_get's mip 0 / 1 of tex, drawn from batches whose st scale is st_k (Q = st_k / z): mip levels
 * level + 1 .. 3 made resident (uploaded if needed) for the frame, at most 3. Returns MXL (0: no chain, draw as
 * before) with *miptbp1 their addresses (buffer width 128 like TEX0) and *k TEX1.K (7.4 fixed): LOD = log2(z) - log2(st_k)
 * + K picks the level whose texels are ~1 pixel at view depth z, from the texture's area-weighted density (a batch's
 * own scale differs; the GS LOD has no slope term, so walls seen at a grazing angle stay under-filtered) */
static float fast_log2(float x)
{
    union { float f; unsigned u; } v = {x};
    float e = (float)((int)(v.u >> 23 & 255) - 128);
    v.u = (v.u & 0x007FFFFF) | 0x3F800000;  /* mantissa m in [1, 2): log2(m) ~ (-1/3 m + 2) m - 2/3 */
    return e + (-0.34484843f * v.f + 2.02466578f) * v.f - 0.67487759f;
}

int tc_mip(int tex, const GSTEXTURE *t, float st_k, u64 *miptbp1, int *k)
{
    /* per texture and base level, once a frame (chunks come grouped by texture): the chain made resident, its MIPTBP1
     * and K at st_k 4096 */
    static struct { unsigned frame; u64 tbp; short k; unsigned char n; } memo[MAX_TEX][2];
    if (tex < 0 || tex >= MAX_TEX || !te[tex] || te[tex]->levels <= 2 || te[tex]->density <= 0 || !in_frame)
        return 0;
    int level = t == &lv[tex][0].t ? 0 : t == &lv[tex][1].t ? 1 : -1;  /* what tc_get gave (near level: no chain) */
    if (level < 0)
        return 0;
    typeof(memo[0][0]) *mo = &memo[tex][level];
    if (mo->frame != frame) {
        u32 tbp[3] = {0, 0, 0};
        int n = 0;
        for (int m = level + 1; m < 4 && m < te[tex]->levels; m++) {
            Level *v = &lv[tex][m < 2 ? m : m + 1];
            if (!v->size)
                break;
            if (v->off < 0) {  /* not resident: uploaded unless this frame's chain uploads are spent (a shorter chain) */
                int bytes = (v->t.Width * v->t.Height) >> (te[tex]->psm == GS_PSM_T4);
                if (mip_up + bytes > TC_MIP_UP || !load(tex, m < 2 ? m : m + 1)) {
                    mip_short++;  /* a shorter chain this frame (cap or no room) */
                    break;
                }
                mip_up += bytes;
            }
            v->last = frame;
            tbp[n++] = ((pool_of(tex, 0) ? pool2_vram : pool_vram) + v->off * 256) / 256;
        }
        /* log2(density x 4096 / focal) - level + bias; log2 from the float's exponent and a quadratic on its mantissa
         * (error < 0.005 level) */
        float kf = fast_log2(te[tex]->density * 4096.0f / tc_focal) - level + MIP_BIAS;
        mo->frame = frame, mo->n = n, mo->tbp = GS_SETREG_MIPTBP1(tbp[0], 2, tbp[1], 2, tbp[2], 2);
        mo->k = (short)(kf * 16 + (kf < 0 ? -0.5f : 0.5f));
    }
    if (!mo->n)
        return 0;
    *miptbp1 = mo->tbp;
    *k = (mo->k - (st_k < 2048 ? 32 : 0)) & 0xFFF;  /* st_k 1024: log2 two lower */
    tc_stats.mip_batches++;
    return mo->n;
}

u32 tc_pin(int bytes)
{
    int n = ((bytes + 255) / 256 + 31) & ~31;  /* whole 8 KB pages */
    if (!pool_blk || n <= 0 || n > pool_blk / 2 || in_frame)
        return 0;
    if (pin_blk)
        return n <= pin_blk ? pool_vram + (pool_blk - pin_blk) * 256 : 0;
    int off = (pool_blk - n) & ~31;
    int nall = tex_end();
    /* update phase: the last frame's chain finished (main.c vu1_wait), so its textures there can go; they upload
     * again below the lent range when next drawn */
    for (int i = 0; i < nall; i++)
        for (int l = 0; l < NLV; l++)
            if (lv[i][l].off >= 0 && pool_of(i, l) == 0 && lv[i][l].off + lv[i][l].size > off)
                lv[i][l].off = -1;
    pin_off = off, pin_blk = pool_blk - off;
    return pool_vram + off * 256;
}

void tc_unpin(void) { pin_blk = 0; }

void tc_end(void)
{
    in_frame = 0;
    if (frame == 2)
        tc_stats.first_bytes = tc_stats.bytes;
    else if (tc_stats.bytes > tc_stats.max_bytes)
        tc_stats.max_bytes = tc_stats.bytes;
    int used = 0, now = 0;
#if TC_RLIST
    rl_trim();
    for (int j = 0; j < rl_n; j++) {
        const Level *v = &lv[rl[j] / NLV][rl[j] % NLV];
        used += v->size;
        now += v->last == frame ? v->size : 0;
    }
#else
    int nall = tex_end();
    for (int i = 0; i < nall; i++)
        for (int l = 0; l < NLV; l++) {
            used += lv[i][l].off >= 0 ? lv[i][l].size : 0;
            now += lv[i][l].off >= 0 && lv[i][l].last == frame ? lv[i][l].size : 0;
        }
#endif
    tc_stats.resident = used / 4;
    tc_stats.frame_kb = now / 4;
    tc_stats.max_frame_kb = tc_stats.frame_kb > tc_stats.max_frame_kb ? tc_stats.frame_kb : tc_stats.max_frame_kb;
    {  /* budget doc proposal 7: uploads and misses every 120 frames (fails = a batch not drawn: no level resident or
        * uploadable; short = a mip chain cut by the per-frame cap or the pool) */
        static unsigned fr, up, kb, fails, shorts, kbmax;
        up += tc_stats.uploads, kb += tc_stats.bytes >> 10, fails += tc_stats.fails, shorts += mip_short;
        kbmax = (unsigned)(tc_stats.bytes >> 10) > kbmax ? (unsigned)(tc_stats.bytes >> 10) : kbmax;
        mip_short = 0;
        if (!(++fr % 120)) {
            printf("tex: %u uploads %u KB (most in a frame %u KB), %u fails, %u chains cut short over 120 frames; "
                   "resident %d KB of %d\n", up, kb, kbmax, fails, shorts, tc_stats.resident, tc_stats.pool);
            up = kb = fails = shorts = kbmax = 0;
        }
    }
#ifdef TC_HUD  /* measurement builds: upload numbers on screen and in the EE log */
    char buf[100];
    snprintf(buf, sizeof buf, "TEX UP %dK MAX %dK 1ST %dK SET %d/%dK RES %d/%dK F%d N%d/%d M%d", tc_stats.bytes / 1024,
             tc_stats.max_bytes / 1024, tc_stats.first_bytes / 1024, tc_stats.frame_kb, tc_stats.max_frame_kb,
             tc_stats.resident, tc_stats.pool, tc_stats.fails, near_get, near_want, tc_stats.mip_batches);
    r_text_outlined(30, 230, 2, buf);
    if (!(frame & 63))
        printf("TEXC frame %u: %s\n", frame, buf);
#endif
}
