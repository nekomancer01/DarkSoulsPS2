/* The streamer's page arena (msarena.h; split from mapstream.c 2026-11-12, made a part of its own with hidden state in
 * Polaris 4.1, code moved as it was): 256-byte granules on a grid of 64 KB pages, the pools, pages borrowed from the
 * heap. What lies in the pages (map items, lite sets) and when it may go is msevict.c's. mapstream.c's header comment
 * has the design. */
#include <kernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mapstream.h"
#include "msarena.h"
#include "mem.h"
#include "cvar.h"


/* ---- arena: granule bitmap on a page grid ---- */
static unsigned char *arena;
static unsigned arena_size;
static MemPages gm;                  /* the granules (mem.h pages of MS_GRAN bytes): bit per granule, 1 = used */
static unsigned npages, ipages;      /* ipages: pages map items may use (below MS_OTHER) */
static unsigned short *pfree;        /* free granules per page */
static unsigned char *pkind;         /* MS_LIFE: per page the kind (MS_K_*) of the group that opened it, 0 while whole free */
/* Items over the top (built in: the frozen residency design, 2026-11-12): the pages map items may use now. Once mapstream_alloc holds more than
 * MS_OTHER (two Burg blocks' lite sets: 4.2 MB) the reserve is gone anyway, and the plan's cut already counts every
 * page not held by them as item room (cap = npages - other); items kept out of the reserve's free pages left that room
 * idle while 63 KB coarse cells of the pinned class found no room (384 KB of whole free pages there, ISO A/B of Ame's
 * play report 2026-11-08). mapstream_alloc clears item pages there like any other. */
/* MS_POOLS: bytes asked for the structural / misc pools (mapstream_pools), their first pages once the arena exists
 * (struct: [spage0, npages), misc: [mpage0, spage0), items below mpage0 = ipages) */
static unsigned pool_req[2], spage0, mpage0;
/* MS_POOL_TILE: pages asked (mapstream_tile_pool) and the region's pages now, [spage0, spage0 + tile_pages) */
static unsigned tile_want, tile_pages;
static int pools_on;
static unsigned reserve_extra, pool_max = MS_POOL;
/* the planner's page scores (msevict.c make_window: bits 0-7 highest class + 1 of its clearable groups, 0x200 not
 * clearable; cost; granules of clearable groups) */
static unsigned *pcls, *pcost, *pitem;

/* ---- pages borrowed from the heap (MS_BORROW) ----
 * The arena's size is fixed at boot, but the heap's free RAM is not idle: at the Taurus Demon on the ISO Burg route
 * (2026-11-08) the heap had 5.9 MB free (largest run 1.4 MB) while near and coarse cells found no arena room (plan
 * ~6.7 MB of items against ~3.7 MB of arena left by two lite sets and the relief). Map items are at most a page and
 * any free page fits any read group, so the streamer borrows single 64 KB heap blocks as extra item pages while the
 * heap keeps MS_BORROW_FLOOR free, and gives them back (dropping their items, never one a chain may still read) when
 * it falls below that, or at once when a heap allocation needs the room (mapstream_heap_release, the way re3's
 * CMemoryHeap::Malloc calls CStreaming::MakeSpaceFor before it fails: sdk-ref re3 src/rw/MemoryHeap.cpp). A page
 * fits a hole of the fragmented heap, so free RAM in pieces too small for an enemy model still holds map data.
 * Extra pages get page numbers npages.. npages + MS_XMAX - 1 in the granule map; an absent one is marked used.
 * Built in: the frozen residency design, 2026-11-12. The policy (when to borrow, what to drop) is msevict.c's. */
static unsigned char *xpage[MS_XMAX];
static unsigned char xret[MS_XMAX];  /* being given back: no new items (its drawn ones wait for their chains) */
static unsigned nx;  /* borrowed pages now */

/* the item pages now (all pages without pools while mapstream_alloc holds more than MS_OTHER) */
static inline unsigned ilim(void)
{
    if (pools_on)
        return ipages;
    return (mapstream_stats.other_kb << 10) > MS_OTHER ? npages : ipages;
}
static inline unsigned ntot(void) { return npages + MS_XMAX; }
/* the item pages: those below ilim(), then the borrowed ones (absent ones are marked full) */
#define FOR_IPAGES(p) for (unsigned p##_i = 0, p; p##_i < ilim() + MS_XMAX && (p = p##_i < ilim() ? p##_i : npages + p##_i - ilim(), 1); p##_i++)

unsigned char *msa_ptr(unsigned g)
{
    unsigned p = g / MS_GPP;
    return (p < npages ? arena + (size_t)p * MS_PAGE : xpage[p - npages]) + (g % MS_GPP) * MS_GRAN;
}
unsigned msa_gran(const void *ptr)
{
    const unsigned char *pq = ptr;
    if (pq >= arena && pq < arena + arena_size)
        return (unsigned)(pq - arena) / MS_GRAN;
    for (unsigned i = 0; i < MS_XMAX; i++)
        if (xpage[i] && pq >= xpage[i] && pq < xpage[i] + MS_PAGE)
            return (npages + i) * MS_GPP + (unsigned)(pq - xpage[i]) / MS_GRAN;
    return ~0u;  /* not ours (a caller bug) */
}

int msa_used(unsigned g) { return mem_pages_taken(&gm, g); }
void msa_mark(unsigned g0, unsigned n, int on)
{
    mem_pages_mark(&gm, g0, n, on);
    for (unsigned g = g0; g < g0 + n; g++)
        pfree[g / MS_GPP] += on ? -1 : 1;
    if (!on && pkind && n)
        for (unsigned p = g0 / MS_GPP; p <= (g0 + n - 1) / MS_GPP; p++)
            if (pfree[p] == MS_GPP)
                pkind[p] = MS_K_ANY;
}
/* n bytes (<= a page) inside one page: pages already in use first, so free pages stay whole. (Best fit with small
 * items from the top and page-sized groups from the bottom was tried, 2026-11-12: no-room refusals 440 -> 515 on
 * iso_burg_taurus, 100 -> 162 on iso_ame_report3; not kept) */
/* ng granules in one run inside page p (not a page being given back); NULL: none */
static void *g_in_page(unsigned p, unsigned ng)
{
    if (pfree[p] < ng || (p >= npages && xret[p - npages]))
        return NULL;
    unsigned run = 0, g0 = p * MS_GPP;
    for (unsigned g = g0; g < g0 + MS_GPP; g++) {
        run = msa_used(g) ? 0 : run + 1;
        if (run == ng) {
            msa_mark(g + 1 - ng, ng, 1);
            return msa_ptr(g + 1 - ng);
        }
    }
    return NULL;
}
/* the item pages and the borrowed ones counted i = 0.. (FOR_IPAGES order), or for long-lived groups the item pages
 * from the top down and the borrowed ones last (a borrowed page goes back to the heap, its groups with it) */
static inline unsigned life_page(unsigned i, int kind)
{
    unsigned lim = ilim();
    return i >= lim ? npages + i - lim : kind == MS_K_LONG ? lim - 1 - i : i;
}
unsigned msa_whole_free(void)
{
    unsigned n = 0;
    FOR_IPAGES(p)
        n += pfree[p] == MS_GPP && !(p >= npages && xret[p - npages]);
    return n;
}
unsigned msa_items_free(void)
{
    unsigned fr = 0;
    FOR_IPAGES(p)
        fr += pfree[p];
    return fr;
}
/* MS_LIFE: n bytes (<= a page) for a group of this kind (mapstream_i.h MS_LIFE): a hole in a page of its kind, a whole
 * free page while more than MS_TIDY_LO are whole, then msa_alloc's order */
void *msa_alloc_k(unsigned n, int kind)
{
    unsigned ng = (n + MS_GRAN - 1) / MS_GRAN, np = ilim() + MS_XMAX;
    if (!MS_LIFE || kind == MS_K_ANY || !pkind || !ng || ng > MS_GPP)
        return msa_alloc(n);
    void *r;
    for (unsigned i = 0; i < np; i++) {
        unsigned p = life_page(i, kind);
        if (pkind[p] == kind && pfree[p] != MS_GPP && (r = g_in_page(p, ng)))
            return r;
    }
    if (msa_whole_free() > MS_TIDY_LO)
        for (unsigned i = 0; i < np; i++) {
            unsigned p = life_page(i, kind);
            if (pfree[p] == MS_GPP && (r = g_in_page(p, ng))) {
                pkind[p] = (unsigned char)kind;
                return r;
            }
        }
    return msa_alloc(n);
}
void *msa_alloc(unsigned n)
{
    unsigned ng = (n + MS_GRAN - 1) / MS_GRAN;
    if (!ng || ng > MS_GPP)
        return NULL;
    for (int pass = 0; pass < 2; pass++)
        FOR_IPAGES(p) {
            if (pfree[p] < ng || (pass == 0) == (pfree[p] == MS_GPP) || (p >= npages && xret[p - npages]))
                continue;
            unsigned run = 0, g0 = p * MS_GPP;
            for (unsigned g = g0; g < g0 + MS_GPP; g++) {
                run = msa_used(g) ? 0 : run + 1;
                if (run == ng) {
                    msa_mark(g + 1 - ng, ng, 1);
                    return msa_ptr(g + 1 - ng);
                }
            }
        }
    return NULL;
}
void *msa_alloc_range(unsigned n, unsigned bot, unsigned top)
{
    unsigned np = (n + MS_PAGE - 1) / MS_PAGE, ng = (n + MS_GRAN - 1) / MS_GRAN, run = 0;
    for (unsigned p = top; p-- > bot;) {
        run = pfree[p] == MS_GPP ? run + 1 : 0;
        if (run == np) {
            unsigned end = (p + np) * MS_GPP;
            msa_mark(end - ng, ng, 1);
            return arena + (size_t)(end - ng) * MS_GRAN;
        }
    }
    return NULL;
}
void *msa_alloc_lend(unsigned n)
{
    unsigned ng = (n + MS_GRAN - 1) / MS_GRAN;
    if (!ng || ng > MS_GPP)
        return NULL;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned p = mpage0; p < npages; p++) {
            if (pfree[p] < ng || (pass == 0) == (pfree[p] == MS_GPP))
                continue;
            unsigned run = 0, g0 = p * MS_GPP;
            for (unsigned g = g0; g < g0 + MS_GPP; g++) {
                run = msa_used(g) ? 0 : run + 1;
                if (run == ng) {
                    msa_mark(g + 1 - ng, ng, 1);
                    return msa_ptr(g + 1 - ng);
                }
            }
        }
    return NULL;
}
void *msa_alloc_small(unsigned n, unsigned p0, unsigned p1)
{
    unsigned ng = (n + MS_GRAN - 1) / MS_GRAN;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned p = p0; p < p1; p++) {
            if (pfree[p] < ng || (pass == 0) == (pfree[p] == MS_GPP))
                continue;
            unsigned run = 0, g0 = p * MS_GPP;
            for (unsigned g = g0; g < g0 + MS_GPP; g++) {
                run = msa_used(g) ? 0 : run + 1;
                if (run == ng) {
                    msa_mark(g + 1 - ng, ng, 1);
                    return msa_ptr(g + 1 - ng);
                }
            }
        }
    return NULL;
}
void *msa_alloc_ex(unsigned n, unsigned x0, unsigned x1)
{
    unsigned ng = (n + MS_GRAN - 1) / MS_GRAN, bp = ~0u, bg = 0;
    if (!ng || ng > MS_GPP)
        return NULL;
    FOR_IPAGES(p) {
        if ((p >= x0 && p < x1) || (p >= npages && xret[p - npages]) || pfree[p] < ng || (bp != ~0u && pfree[p] >= pfree[bp]))
            continue;
        unsigned run = 0, g0 = p * MS_GPP;
        for (unsigned g = g0; g < g0 + MS_GPP; g++) {
            run = msa_used(g) ? 0 : run + 1;
            if (run == ng) {
                bp = p, bg = g + 1 - ng;
                break;
            }
        }
    }
    if (bp == ~0u)
        return NULL;
    msa_mark(bg, ng, 1);
    return msa_ptr(bg);
}
void msa_free_report(unsigned out[3][3])
{
    memset(out, 0, 9 * sizeof(unsigned));
    unsigned lim = ilim();
    for (unsigned p = 0; p < ntot(); p++) {
        int z = p < lim || p >= npages ? 0 : !pools_on ? 0 : p < spage0 ? 1 : 2;
        if (p < npages && p >= lim && !pools_on)
            continue;  /* (MS_OTHER's top pages: not for items) */
        unsigned run = 0, best = 0, g0 = p * MS_GPP;
        if (pfree[p])
            for (unsigned g = g0; g < g0 + MS_GPP; g++) {
                run = msa_used(g) ? 0 : run + 1;
                best = run > best ? run : best;
            }
        out[z][0] += pfree[p];
        out[z][1] += pfree[p] == MS_GPP;
        out[z][2] = best > out[z][2] ? best : out[z][2];
    }
    for (int z = 0; z < 3; z++)
        out[z][0] = out[z][0] * MS_GRAN >> 10, out[z][2] = out[z][2] * MS_GRAN >> 10;
}
void msa_free(void *ptr, unsigned n)
{
    unsigned g0 = msa_gran(ptr);
    msa_mark(g0, (n + MS_GRAN - 1) / MS_GRAN, 0);
}

unsigned msa_heap_free(void)
{
    struct mallinfo mi = mallinfo();
    return 32 * 1024 * 1024 - (unsigned)sbrk(0) + mi.fordblks;
}

int msa_ensure(void)
{
    if (arena)
        return 1;
    unsigned hf = msa_heap_free(), rs = MS_RESERVE + reserve_extra;
    unsigned sz = hf > rs + (1u << 20) ? hf - rs : 0;
    sz = (sz < pool_max ? sz : pool_max) & ~(MS_PAGE - 1);
    arena = sz ? memalign(MS_PAGE, sz) : NULL;
    if (arena) {
        npages = sz / MS_PAGE;
        unsigned nt = ntot();  /* + the borrowed pages' slots */
        unsigned *gmap = calloc((nt * MS_GPP + 31) / 32, 4);
        pfree = malloc(nt * sizeof *pfree);
        pcls = malloc(nt * sizeof *pcls), pcost = malloc(nt * sizeof *pcost);
        pitem = malloc(nt * sizeof *pitem);
        pkind = calloc(nt, 1);  /* (NULL: MS_LIFE places as before) */
        if (!gmap || !pfree || !pcls || !pcost || !pitem) {
            free(arena), free(gmap), free(pfree), free(pcls), free(pcost), free(pitem), free(pkind);
            pcls = pcost = pitem = NULL, pkind = NULL;
            arena = NULL;
        } else {
            mem_pages_init(&gm, arena, nt * MS_PAGE, MS_GRAN, gmap);
            for (unsigned p = 0; p < npages; p++)
                pfree[p] = MS_GPP;
            for (unsigned p = npages; p < ntot(); p++)  /* not borrowed: full */
                pfree[p] = 0, memset(&gm.bits[p * MS_GPP / 32], 0xff, MS_GPP / 8);
            arena_size = sz;
            ipages = npages > 2 * (MS_OTHER / MS_PAGE) ? npages - MS_OTHER / MS_PAGE : npages / 2;
            unsigned sp = (pool_req[0] + MS_PAGE - 1) / MS_PAGE, mp = (pool_req[1] + MS_PAGE - 1) / MS_PAGE;
            if (sp && sp + mp + 16 < npages) {  /* (16: at least 1 MB of items) */
                pools_on = 1;
                spage0 = npages - sp, mpage0 = spage0 - mp, ipages = mpage0;
                tile_pages = tile_want < sp ? tile_want : sp;
            }
        }
    }
    printf("mapstream: v2 arena %u KB in %u pages (heap free %u KB)\n", arena ? arena_size >> 10 : 0, npages, hf >> 10);
    if (pools_on && arena_size < pool_max)
        printf("mapstream: arena %u KB is under the build's %u KB (heap): the visual room is smaller\n",
               arena_size >> 10, pool_max >> 10);
    if (pools_on)
        printf("mapstream: arena structural %u KB (collision tiles %u KB), misc %u KB, visual %u KB\n",
               (npages - spage0) * (MS_PAGE >> 10), tile_pages * (MS_PAGE >> 10), (spage0 - mpage0) * (MS_PAGE >> 10),
               ipages * (MS_PAGE >> 10));
    else if (arena && pool_req[0])
        printf("mapstream: arena pools off (structural %u KB + misc %u KB asked of %u KB)\n",
               pool_req[0] >> 10, pool_req[1] >> 10, arena_size >> 10);
    return arena != NULL;
}
int msa_ready(void) { return arena != NULL; }

unsigned msa_npages(void) { return npages; }
unsigned msa_ipages(void) { return ipages; }
unsigned msa_ntot(void) { return ntot(); }
unsigned msa_ilim(void) { return ilim(); }
unsigned msa_mpage0(void) { return mpage0; }
unsigned msa_spage0(void) { return spage0; }
unsigned msa_tpage0(void) { return spage0; }
unsigned msa_tile_pages(void) { return tile_pages; }
/* The region's top moves only over pages with nothing in them: a tile above a new, lower top or a lite-set entry
 * below a new, higher one would sit in the other's range (still freed by mapstream_free wherever it lies, but the
 * range would no longer be one user's). A page in use stops the move; the next call goes on. */
void msa_tile_settle(void)
{
    if (!arena || !pools_on)
        return;
    unsigned want = tile_want < npages - spage0 ? tile_want : npages - spage0;
    while (tile_pages < want && pfree[spage0 + tile_pages] == MS_GPP)
        tile_pages++;
    while (tile_pages > want && pfree[spage0 + tile_pages - 1] == MS_GPP)
        tile_pages--;
}
int msa_pools_on(void) { return pools_on; }
unsigned msa_pfree(unsigned p) { return pfree[p]; }
unsigned *msa_scratch(int which) { return which == 0 ? pcls : which == 1 ? pcost : pitem; }

unsigned msa_nx(void) { return nx; }
int msa_x_on(unsigned i) { return xpage[i] != NULL; }
int msa_x_ret(unsigned i) { return xret[i]; }
void msa_x_set_ret(unsigned i, int on) { xret[i] = (unsigned char)on; }
int msa_x_borrow(void)
{
    unsigned i = 0;
    while (xpage[i])
        i++;
    if (!(xpage[i] = memalign(MS_GRAN, MS_PAGE)))
        return -1;
    unsigned p = npages + i;
    memset(&gm.bits[p * MS_GPP / 32], 0, MS_GPP / 8);
    pfree[p] = MS_GPP;
    nx++;
    return (int)i;
}
void msa_x_back(unsigned i)
{
    unsigned p = npages + i, w0 = p * MS_GPP;
    pfree[p] = 0;
    memset(&gm.bits[w0 / 32], 0xff, MS_GPP / 8);
    free(xpage[i]), xpage[i] = NULL, xret[i] = 0, nx--;
}

/* ---- mapstream.h's arena calls ---- */
void mapstream_pool(unsigned max) { pool_max = max; }
void mapstream_reserve(unsigned extra) { reserve_extra = extra; }
int mapstream_ready(void) { return arena != NULL; }
int mapstream_arena(void)
{
    return msa_ensure();
}
int mapstream_owns(const void *p)
{
    return arena && (const unsigned char *)p >= arena && (const unsigned char *)p < arena + arena_size;
}
int mapstream_in_items(const void *p)
{
    return mapstream_owns(p) && msa_pools_on() && msa_gran(p) / MS_GPP < msa_ipages();
}

void mapstream_pools(unsigned struct_bytes, unsigned misc_bytes)
{
    if (!arena)
        pool_req[0] = struct_bytes, pool_req[1] = misc_bytes;
}

unsigned mapstream_tile_pool(unsigned bytes)
{
    tile_want = (bytes + MS_PAGE - 1) / MS_PAGE;
    msa_tile_settle();  /* (before the arena: msa_ensure sizes it) */
    return tile_want * MS_PAGE;
}

int mapstream_pools_on(void) { return pools_on; }
int mapstream_pools_on_build(void) { return 1; }
