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
#include "map.h"
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
/* one relief set per map instance (map.c / src/world.c: 0 the boot area, k a world block while it is full) */
typedef struct {
    const unsigned char *bin;
    const BumpHeader *bh;
    const BumpChunk *bc;
    int tex0, on;
} BumpSet;
static BumpSet set[BUMP_INST];
/* One CLUT area for every set (96 KB: 48 clusters x 2 levels; with enemies a world build has ~55 KB of VRAM left when a
 * block goes full, 2026-11-01): it holds the CLUTs of the set of the block the player stands in (bump_owner), uploaded
 * when that changes; the other sets draw no relief meanwhile (TECH_DEBT world-4) */
static u32 clut_vram;
static unsigned clut_kb;
static int owner, owner_sent = -1;
int bump_batches;

typedef struct {
    unsigned key;  /* set << 24 | CLUT << 12 | texture: neighbours share the CLUT (TEX0 CLD 4 reloads on a change) */
    int cull, clip, tris, nv, mip;
    const void *pos, *st, *col, *idx;  /* idx: indexed chunks (nv vertices), else NULL */
    const float *org;
    float st_k, d;
    GSTEXTURE *t;  /* normal map level, fetched nearest first */
} Item;
static Item items[MAX_CAND];
static int nitems;

/* bump.bin of instance k, chunks: its map.bin's resident chunks; with streaming (render-distance, DSM5) bump.bin numbers
 * the resident ones first, then the streamed near chunks (map.c passes their number from flags bits 8-31). The boot
 * area's normal maps are tc_add's (a pool of their own); a world block's join its texcache slot (tc_block_bump), in
 * the same relief pool. */
static int set_init(int k, const void *blob, unsigned chunks)
{
    BumpSet *b = &set[k];
    b->on = 0;
    if (k)
        tc_block_bump(map_tex_slot(k), NULL, NULL, 0);  /* the old set's normal maps (blob gone with the block's streams) */
    b->bin = blob;
    b->bh = blob;
    const BumpHeader *bh = blob;
    if (!blob || memcmp(bh->magic, "DSB1", 4) || !bh->chunks || bh->chunks < chunks || !bh->clusters)
        return 0;
    b->bc = (const BumpChunk *)(b->bin + bh->chunk_off);
    b->tex0 = k ? tc_block_bump(map_tex_slot(k), b->bin, b->bin + bh->tex_off, bh->textures)
                : tc_add(b->bin, b->bin + bh->tex_off, bh->textures, BUMP_POOL_KB);
    unsigned kb = bh->clusters * bh->levels;
    if (b->tex0 >= 0 && !clut_kb && 4 * 1024 * 1024 - gsg->CurrentPointer >= kb * 1024) {  /* the first set sizes it */
        clut_vram = gsKit_vram_alloc(gsg, kb * 1024, GSKIT_ALLOC_USERBUFFER);
        clut_kb = clut_vram == GSKIT_ALLOC_ERROR ? 0 : kb;
    }
    if (b->tex0 < 0 || kb > clut_kb) {
        printf("bump: %d: no relief: textures %d, CLUT VRAM %u of %u KB (VRAM free %u KB)\n", k, b->tex0, clut_kb, kb,
               (4 * 1024 * 1024 - gsg->CurrentPointer) >> 10);
        if (k)
            tc_block_bump(map_tex_slot(k), NULL, NULL, 0);
        return 0;
    }
    if (owner_sent == k)
        owner_sent = -1;  /* a new set in the CLUT area's owner: its CLUTs go up again */
    FlushCache(0);
#ifdef BUMP_OFF  /* A/B builds: same data and VRAM, no relief pass (other systems size themselves to free VRAM) */
    return 0;
#endif
    return b->on = 1;
}

int bump_init(GSGLOBAL *gs, const void *blob, unsigned chunks)
{
    gsg = gs;
    return set_init(0, blob, chunks);
}

int bump_block(int k, const void *blob, unsigned chunks)
{
    if (k <= 0 || k >= BUMP_INST || !gsg)
        return 0;
    nitems = 0;  /* items of this frame may point into the old set (bump_draw runs after the map's batches) */
    return set_init(k, blob, chunks);
}

void bump_owner(int k)
{
    owner = k > 0 && k < BUMP_INST && set[k].on ? k : 0;
}

void bump_add(int k, unsigned chunk, float d, int cull, int clip, int tris, int nv, const void *pos, const void *st,
              const void *col, const void *idx, const float *org, float st_k)
{
    const BumpSet *b = &set[k];
    if (k != owner || !b->on || chunk >= b->bh->chunks || b->bc[chunk].tex == 0xFFFF || d >= BUMP_FAR || nitems >= MAX_CAND)
        return;
    int level = d < BUMP_NEAR ? 0 : 1;
    if (level >= (int)b->bh->levels || b->bc[chunk].spread >> level < BUMP_MIN_SPREAD)
        return;
    Item *it = &items[nitems++];
    int tex = b->tex0 + b->bc[chunk].tex;
    it->key = (unsigned)k << 24 | (unsigned)(b->bc[chunk].cluster * b->bh->levels + level) << 12 | tex;
    /* the half-strength band draws from mip 1 (a quarter of the pool space) */
    it->cull = cull, it->clip = clip, it->tris = tris, it->nv = nv, it->idx = idx, it->mip = level | tc_level(tex, d);
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
    if (!nitems)
        return;
    const BumpSet *b = &set[owner];
    if (owner_sent != owner) {  /* the owner's CLUTs (16x16 CT32 = 4 blocks each), resident until the owner changes */
        const unsigned char *c = b->bin + b->bh->clut_off;
        for (unsigned i = 0; i < b->bh->clusters * b->bh->levels; i++)
            vu1_upload_tex((clut_vram + i * 1024) / 256, GS_PSM_CT32, 16, 16, c + i * 1024, 1024);
        owner_sent = owner;
        vu1_clut_reload();
    }
    int cap = b->bh->cap && b->bh->cap < BUMP_CAP ? (int)b->bh->cap : BUMP_CAP;
    int cand = nitems;
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
        items[i].t = tc_get(items[i].key & 0xFFF, items[i].mip);
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
        tt.VramClut = clut_vram + (it->key >> 12 & 0xFFF) * 1024;
        tt.ClutPSM = GS_PSM_CT32;
        if (it->nv)
            vu1_add_idx(&tt, it->cull, VU1_BLEND | (it->clip & (VU1_CLIP | VU1_PKT)), it->tris, it->nv, it->pos, it->st,
                        it->col, it->idx, it->org, it->st_k);
        else
            vu1_add_ex(&tt, it->cull, VU1_BLEND | (it->clip & (VU1_CLIP | VU1_PKT)), it->tris, it->pos, it->st, it->col,
                       it->org, it->st_k);
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
