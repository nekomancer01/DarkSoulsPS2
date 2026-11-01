/* Normal-map relief (CLUT dot3). tools/export_normal.py turns every bump texture of the area into an 8-bit index map
 * into one palette of tangent-space normals, and clusters the bumped triangles by how DSR lights them (sun direction
 * and shadow, hemisphere, probe, tone map). Per cluster a CT32 CLUT holds alpha = 0x80 x DSR's output for each palette
 * normal / its output for the flat normal, i.e. what the per-vertex bake lacks. Here the bumped chunks are drawn a
 * second time with their index map and cluster CLUT, blended Cd x As: the relief DSR's pixel shader adds.
 * The depth test (GEQUAL) passes exactly where the opaque pass left the same triangle on top: same streams, same VU1
 * program, so the same depth. */
#include <kernel.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "bump.h"
#include "texcache.h"
#include "vu1.h"

/* bump.bin (tools/export_normal.py) */
typedef struct {
    char magic[4];
    unsigned chunks, textures, clusters, levels;
    unsigned chunk_off, tex_off, clut_off;
    unsigned cap;  /* relief batches per frame (per-area, export_normal BUMP_CAP), 0 = BUMP_CAP */
    unsigned pad[3];
} BumpHeader;
typedef struct { unsigned short tex; unsigned char cluster, spread; } BumpChunk;  /* spread: CLUT std, 0x80 = x1 */

/* ASSUMED (cost, not DSR: DSR shades every distance): full relief within BUMP_NEAR metres of the chunk box, the
 * half-strength CLUT (level 1) up to BUMP_FAR, none beyond (the 128 px maps are at mip 1 and sub-pixel there).
 * TECH_DEBT map-31. (Wider since the 30 fps target, 2026-10-31.) */
#ifndef BUMP_FAR
#define BUMP_NEAR 20.0f  /* 12 / 24 m at the 60 fps target */
#define BUMP_FAR 32.0f
#endif
/* a relief whose texel spread is under one step of the CT16 frame buffer (5 bits: 1/32 of x1 = 4 / 128) does not show:
 * skipped (the half-strength level halves the spread) */
#define BUMP_MIN_SPREAD 4
#define MAX_CAND 1536  /* relief candidates per frame (bumped opaque chunks within BUMP_FAR; Burg spawn: see BUMP_CAP) */
/* Relief batches drawn per frame, nearest first: each costs ~4-5 us EE + a re-sent batch on VU1 / GS. The old
 * first-come limit of 512 dropped whole cells in Undead Burg (the spawn's cobbles got none). */
#ifndef BUMP_CAP
#define BUMP_CAP 1536
#endif
/* VRAM pool of the normal maps (texcache tc_add): their own, so they never evict map textures (Undead Burg's map
 * working set fills the 640 KB map pool: shared, the relief made 30-145 misses a frame and halved the fps) */
#ifndef BUMP_POOL_KB
#define BUMP_POOL_KB 384  /* Firelink at 192 KB: 14-35 relief misses a frame; Burg spawn at 256 KB: 9 of 142 */
#endif

static GSGLOBAL *gsg;
static const unsigned char *bin;
static const BumpHeader *bh;
static const BumpChunk *bc;
static int tex0, on, cluts_sent;
static u32 clut_vram;
int bump_batches;

typedef struct {
    unsigned key;  /* CLUT << 16 | texture: sorted so neighbours share the CLUT (TEX0 CLD 4 reloads on a change) */
    int cull, clip, tris, mip;
    const void *pos, *st, *col;
    const float *org;
    float st_k, d;
    GSTEXTURE *t;  /* normal map level, fetched nearest first */
} Item;
static Item items[MAX_CAND];
static int nitems;

int bump_init(GSGLOBAL *gs, const void *blob, unsigned chunks)
{
    on = 0;
    gsg = gs;
    bin = blob;
    bh = blob;
    /* chunks: map.bin's resident chunks; with streaming (render-distance, DSM5) bump.bin numbers the resident ones first,
     * then the streamed near chunks (map.c passes their number from flags bits 8-31) */
    if (!blob || memcmp(bh->magic, "DSB1", 4) || !bh->chunks || bh->chunks < chunks || !bh->clusters)
        return 0;
    bc = (const BumpChunk *)(bin + bh->chunk_off);
    tex0 = tc_add(bin, bin + bh->tex_off, bh->textures, BUMP_POOL_KB);
    clut_vram = gsKit_vram_alloc(gs, bh->clusters * bh->levels * 1024, GSKIT_ALLOC_USERBUFFER);
    if (tex0 < 0 || clut_vram == GSKIT_ALLOC_ERROR)
        return 0;
    cluts_sent = 0;
    FlushCache(0);
#ifdef BUMP_OFF  /* A/B builds: same data and VRAM, no relief pass (other systems size themselves to free VRAM) */
    return 0;
#endif
    return on = 1;
}

void bump_add(unsigned chunk, float d, int cull, int clip, int tris, const void *pos, const void *st, const void *col,
              const float *org, float st_k)
{
    if (!on || chunk >= bh->chunks || bc[chunk].tex == 0xFFFF || d >= BUMP_FAR || nitems >= MAX_CAND)
        return;
    int level = d < BUMP_NEAR ? 0 : 1;
    if (level >= (int)bh->levels || bc[chunk].spread >> level < BUMP_MIN_SPREAD)
        return;
    Item *it = &items[nitems++];
    it->key = (unsigned)(bc[chunk].cluster * bh->levels + level) << 16 | bc[chunk].tex;
    /* the half-strength band draws from mip 1 (a quarter of the pool space) */
    it->cull = cull, it->clip = clip, it->tris = tris, it->mip = level | tc_level(tex0 + bc[chunk].tex, d);
    it->pos = pos, it->st = st, it->col = col, it->org = org, it->st_k = st_k, it->d = d;
}

static int dist_cmp(const void *a, const void *b)
{
    float x = ((const Item *)a)->d, y = ((const Item *)b)->d;
    return x < y ? -1 : x > y;
}

static int item_cmp(const void *a, const void *b)
{
    unsigned x = ((const Item *)a)->key, y = ((const Item *)b)->key;
    return x < y ? -1 : x > y;
}

#ifdef BUMP_LOG
static inline unsigned ticks(void) { unsigned c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }
#endif

void bump_draw(void)
{
#ifdef BUMP_LOG
    unsigned t_start = ticks();
#endif
    bump_batches = 0;
    if (!on || !nitems) {
        nitems = 0;
        return;
    }
    if (!cluts_sent) {  /* first frame: every CLUT (16x16 CT32 = 4 blocks each), resident from then on */
        const unsigned char *c = bin + bh->clut_off;
        for (unsigned i = 0; i < bh->clusters * bh->levels; i++)
            vu1_upload_tex((clut_vram + i * 1024) / 256, GS_PSM_CT32, 16, 16, c + i * 1024, 1024);
        cluts_sent = 1;
        vu1_clut_reload();
    }
    int cand = nitems, cap = bh->cap && bh->cap < BUMP_CAP ? (int)bh->cap : BUMP_CAP;
    if (nitems > cap) {  /* keep the cap nearest: distance histogram, cut inside the bin that crosses the cap */
        enum { BINS = 64 };
        int hist[BINS] = {0}, cut = BINS, room = cap;
        for (int i = 0; i < nitems; i++)
            hist[(int)(items[i].d * (BINS / BUMP_FAR))]++;
        for (int b = 0; b < BINS; b++) {
            if (hist[b] > room) {
                cut = b;
                break;
            }
            room -= hist[b];
        }
        int n = 0;
        for (int i = 0; i < nitems; i++) {
            int b = (int)(items[i].d * (BINS / BUMP_FAR));
            if (b < cut || (b == cut && room-- > 0))
                items[n++] = items[i];
        }
        nitems = n;
    }
    /* normal maps nearest first: when the pool runs short the far chunks go without relief (or fall back to mip 1),
     * never the floor under the camera (Burg spawn in CLUT order: 5 of 142 missed, the cobbles among them) */
    qsort(items, nitems, sizeof *items, dist_cmp);
    for (int i = 0; i < nitems; i++)
        items[i].t = tc_get(tex0 + (items[i].key & 0xFFFF), items[i].mip);
    qsort(items, nitems, sizeof *items, item_cmp);
    GSGLOBAL *gs = gsg;
    /* Cd x As (As = CLUT alpha x vertex alpha 0x80; 0x80 = x1, up to ~x2), Z tested, not written */
#ifdef BUMP_COVER  /* debug: every pixel the relief pass passes the depth test on turns black */
    u64 r[4] = {GS_SETREG_ALPHA(1, 2, 2, 2, 0), 0x42, GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 1), 0x4E};
#else
    u64 r[4] = {GS_SETREG_ALPHA(1, 2, 0, 2, 0), 0x42, GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 1), 0x4E};
#endif
    vu1_gs_regs(2, r);
    for (int i = 0; i < nitems; i++) {
        const Item *it = &items[i];
        if (!it->t)
            continue;
        GSTEXTURE tt = *it->t;
        tt.VramClut = clut_vram + (it->key >> 16) * 1024;
        tt.ClutPSM = GS_PSM_CT32;
        vu1_add_ex(&tt, it->cull, VU1_BLEND | (it->clip ? VU1_CLIP : 0), it->tris, it->pos, it->st, it->col, it->org,
                   it->st_k);
        bump_batches++;
    }
    u64 rr[4] = {gs->PrimAlpha, 0x42, GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0), 0x4E};
    vu1_gs_regs(2, rr);
#ifdef BUMP_LOG
    static unsigned frame, acc;
    acc += ticks() - t_start;
    if (!(++frame & 63))
        printf("BUMP cand %d drawn %d, bump_draw %u us / frame\n", cand, bump_batches, acc / 64 / 295), acc = 0;
#else
    (void)cand;
#endif
    nitems = 0;
}
