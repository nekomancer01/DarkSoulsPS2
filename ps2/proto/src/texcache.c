/* Map texture cache. VRAM: a pool of 256-byte GS blocks; each texture level takes a power-of-two run of blocks at an
 * offset aligned to its size (a buddy layout), so a level's blocks never straddle another's. Allocation picks the
 * aligned run whose newest occupant was drawn longest ago (free runs first) and evicts what is in it; levels drawn this
 * frame are never evicted, because the EE-clipped triangles (gsKit, end of the frame) still use them.
 *
 * Uploads (R&C 1 / Jak pattern): a texture that is not resident is uploaded in the map's VIF1 chain right before the
 * first batch that draws with it (vu1_upload_tex: PATH2 image transfer straight from map.bin, then TEXFLUSH); resident
 * ones cost nothing (Jak II). The chain may be sent in several segments (vu1.c room()); order is kept either way.
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
    unsigned near;  /* near level pixels (flag bit 0 in pad), 0: none */
} TexEntry;

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
#define TC_NEAR_UP (96 * 1024)  /* near-level upload bytes per frame */

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
static Level lv[MAX_TEX][3];  /* mip 0, mip 1, near */
static unsigned frame = 1;
static float mip_d[MAX_TEX];
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
    v->t.Mem = (u32 *)(base + e->near);
    v->size = near_blocks(e->psm, e->w * 2, e->h * 2);
    if (!pool3_blk) {  /* first near level: allocate its pool */
        pool3_blk = TC_NEAR_KB * 4 < POOL_MAX ? TC_NEAR_KB * 4 : POOL_MAX;
        pool3_vram = gsKit_vram_alloc(gsg, pool3_blk * 256, GSKIT_ALLOC_USERBUFFER);
        if (pool3_vram == GSKIT_ALLOC_ERROR)
            pool3_blk = 0;
    }
}

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
        lv[i][0].off = lv[i][1].off = lv[i][2].off = -1, lv[i][2].size = 0;
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
            t->Mem = l == 0 && (e->pix[0] & TC_RES) ? NULL : (u32 *)(bin + e->pix[l]);  /* tc_res */
            t->Clut = (u32 *)(bin + e->clut);
            t->VramClut = c;
            lv[i][l].off = -1;
            lv[i][l].last = 0;
            lv[i][l].size = e->blocks[l];
        }
        near_level(i, bin);
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
        lv[ntex][2].off = -1, lv[ntex][2].size = 0;
        if (e->clut)
            cv += clut_blocks(e) * 256;
    }
    return first;
}

/* world block slot b's textures: indices TC_BASE + b x TC_BLOCK_TEX .. + n, CLUTs in VRAM of their own (reserved
 * while the block is on, one allocation each), pixels in the map's pool. base: the block's map.bin (texture offsets
 * are from it), kept until tc_block_drop. Returns the first index, or -1 (too many textures, VRAM full). */
int tc_block_add(int b, const unsigned char *base, const void *table, int n)
{
    if (b < 0 || b >= TC_BLOCKS || n > TC_BLOCK_TEX)
        return -1;
    tc_block_drop(b);
    int first = TC_BASE + b * TC_BLOCK_TEX;
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
            t->Mem = l == 0 && (e->pix[0] & TC_RES) ? NULL : (u32 *)(base + e->pix[l]);  /* tc_res */
            t->Clut = (u32 *)(base + e->clut);
            t->VramClut = c;
            lv[first + i][l].off = -1;
            lv[first + i][l].last = 0;
            lv[first + i][l].size = e->blocks[l];
        }
        near_level(first + i, base);
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
        lv[i][0].off = lv[i][1].off = lv[i][2].off = -1, lv[i][2].size = 0;
    }
    blk_n[b] = blk_clut_due[b] = blk_bump[b] = 0;
}

int tc_block_bump(int b, const unsigned char *base, const void *table, int n)
{
    if (b < 0 || b >= TC_BLOCKS)
        return -1;
    int first = TC_BASE + b * TC_BLOCK_TEX + blk_n[b];
    for (int i = first; i < first + blk_bump[b]; i++) {  /* the old set: forgotten, its pool blocks free */
        te[i] = NULL;
        lv[i][0].off = lv[i][1].off = lv[i][2].off = -1, lv[i][2].size = 0;
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
        lv[first + i][2].off = -1, lv[first + i][2].size = 0;
    }
    blk_bump[b] = n;
    return first;
}

void tc_res(int slot, const unsigned char *res)
{
    int a = slot < 0 ? 0 : TC_BASE + slot * TC_BLOCK_TEX, n = slot < 0 ? nmap : slot < TC_BLOCKS ? blk_n[slot] : 0;
    for (int i = a; i < a + n; i++)
        if (te[i] && (te[i]->pix[0] & TC_RES))
            lv[i][0].t.Mem = res ? (u32 *)(res + (te[i]->pix[0] & ~TC_RES)) : NULL;
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
static int alloc(int s, int p)
{
    static unsigned score[POOL_MAX];  /* per run: 0 free, else newest occupant's frame + 1, ~0 drawn this frame */
    if (s <= 0)
        return -1;
    int nrun = (p == 2 ? pool3_blk : p ? pool2_blk : pool_blk) / s;
    if (!nrun)
        return -1;
    memset(score, 0, nrun * sizeof *score);
    int nall = tex_end();
    for (int i = 0; i < nall; i++)
        for (int l = 0; l < 3; l++) {
            const Level *v = &lv[i][l];
            if (v->off < 0 || pool_of(i, l) != p)
                continue;
            unsigned sc = v->last == frame ? ~0u : v->last + 1;
            for (int r = v->off / s; r < nrun && r * s < v->off + v->size; r++)
                score[r] = sc > score[r] ? sc : score[r];
        }
    int best = -1;
    unsigned long long bk = ~0ull;
    for (int r = 0; r < nrun; r++) {
        /* among free runs prefer one whose buddy is in use (keeps large free runs whole) */
        unsigned long long k = (unsigned long long)score[r] * 2 + (!score[r] && ((r ^ 1) >= nrun || !score[r ^ 1]));
        if (k < bk)
            bk = k, best = r;
    }
    if (best < 0 || score[best] == ~0u)
        return -1;
    int off = best * s;
    for (int i = 0; i < nall; i++)
        for (int l = 0; l < 3; l++) {
            Level *v = &lv[i][l];
            if (v->off >= 0 && pool_of(i, l) == p && v->off < off + s && v->off + v->size > off)
                v->off = -1;
        }
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
    return 1;
}

void tc_begin(float focal)
{
    frame++;
    tc_stats.uploads = tc_stats.bytes = tc_stats.fails = 0;
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

int tc_level(int tex, float d)
{
    if (tex < 0 || tex >= MAX_TEX || !te[tex])
        return 0;
#ifndef TC_NO_NEAR  /* A/B builds: near levels never drawn */
    if (lv[tex][2].size && pool3_blk && d < mip_d[tex] * 0.5f)
        return 2;  /* near level texels (half mip 0's) still >= 1 / MIP_K px */
#endif
    return d > mip_d[tex];
}

GSTEXTURE *tc_get(int tex, int level)
{
    if (tex < 0 || tex >= MAX_TEX || !te[tex] || !in_frame)
        return NULL;
    if (level == 2 ? !lv[tex][2].size : level >= te[tex]->levels)
        level = 0;
    near_want += level == 2;
    Level *v = &lv[tex][level];
    if (v->off < 0 && !load(tex, level)) {
        int o = level == 2 ? 0 : level ^ 1;
        v = &lv[tex][o];
        if (o >= te[tex]->levels || (v->off < 0 && !load(tex, o))) {
            tc_stats.fails++;
            return NULL;
        }
    }
    v->last = frame;
    near_get += v == &lv[tex][2];
    return &v->t;
}

void tc_end(void)
{
    in_frame = 0;
    if (frame == 2)
        tc_stats.first_bytes = tc_stats.bytes;
    else if (tc_stats.bytes > tc_stats.max_bytes)
        tc_stats.max_bytes = tc_stats.bytes;
    int used = 0, now = 0, nall = tex_end();
    for (int i = 0; i < nall; i++)
        for (int l = 0; l < 3; l++) {
            used += lv[i][l].off >= 0 ? lv[i][l].size : 0;
            now += lv[i][l].off >= 0 && lv[i][l].last == frame ? lv[i][l].size : 0;
        }
    tc_stats.resident = used / 4;
    tc_stats.frame_kb = now / 4;
    tc_stats.max_frame_kb = tc_stats.frame_kb > tc_stats.max_frame_kb ? tc_stats.frame_kb : tc_stats.max_frame_kb;
#ifdef TC_HUD  /* measurement builds: upload numbers on screen and in the EE log */
    char buf[100];
    snprintf(buf, sizeof buf, "TEX UP %dK MAX %dK 1ST %dK SET %d/%dK RES %d/%dK F%d N%d/%d", tc_stats.bytes / 1024,
             tc_stats.max_bytes / 1024, tc_stats.first_bytes / 1024, tc_stats.frame_kb, tc_stats.max_frame_kb,
             tc_stats.resident, tc_stats.pool, tc_stats.fails, near_get, near_want);
    r_text_outlined(30, 230, 2, buf);
    if (!(frame & 63))
        printf("TEXC frame %u: %s\n", frame, buf);
#endif
}
