/* mapstream's arena (split from mapstream.c 2026-11-12, moved as it was): 256-byte granules on a grid of 64 KB
 * pages, the pools, pages borrowed from the heap, clearing windows and moving items (defrag), and the
 * mapstream_alloc / _pool / _free API. mapstream.c's header comment has the design. */
#include "mapstream_i.h"

/* ---- arena: granule bitmap on a page grid ---- */
unsigned char *ms_arena;
static unsigned arena_size, *gmap;  /* bit per granule, 1 = used */
unsigned ms_npages, ms_ipages;  /* ipages: pages map items may use (below MS_OTHER) */
unsigned short *ms_pfree;               /* free granules per page */
/* Items over the top (built in: the frozen residency design, 2026-11-12): the pages map items may use now. Once mapstream_alloc holds more than
 * MS_OTHER (two Burg blocks' lite sets: 4.2 MB) the reserve is gone anyway, and the plan's cut already counts every
 * page not held by them as item room (cap = npages - other); items kept out of the reserve's free pages left that room
 * idle while 63 KB coarse cells of the pinned class found no room (384 KB of whole free pages there, ISO A/B of Ame's
 * play report 2026-11-08). mapstream_alloc clears item pages there like any other. */
/* MS_POOLS: bytes asked for the structural / misc pools (mapstream_pools), their first pages once the arena exists
 * (struct: [spage0, npages), misc: [mpage0, spage0), items below mpage0 = ipages), bytes mapstream_alloc placed in
 * item pages (the plan leaves them out) */
static unsigned pool_req[2], spage0;
static int pool_busy;
unsigned ms_mpage0, ms_items_other;
int ms_pools_on;
/* Lending (the planning thread's "drop-only lending", 2026-11-08; built in: the frozen residency design, 2026-11-12): the pools' idle pages hold map items of
 * the lowest classes the plan wants (read-ahead, coarse out of view or beyond the view radius, the pinned coarse ring:
 * MS_LEND_CLS), never a near part. They are never copied: a pool allocation that finds no room drops them (those a
 * chain may still read wait two draws: mapstream_pool_wait). The visual room the export solves against stays the item
 * pages; lent room only fills the horizon ring where fewer blocks are loaded than the pool is sized for. */
unsigned ms_lent_n;  /* items in pool pages now (A/B log) */
/* (a mapstream_pool_prepare does not keep new items out of the structural pool: with ~2 s of that, under B3's radii
 * the near parts lost the lent pages they relied on (soak2 pop-ins 25 / 37 -> 54 / 54, taurus 16 / 12 -> 35 / 35,
 * 2026-11-09); the drop after one VU1 wait in mapstream_alloc_pool is what ended the long seam holds) */
static unsigned early_n;  /* lent items let go ahead of a lite set (mapstream_pool_prepare), for the log */
/* near parts may take idle pool pages too, up to this much (the planning thread: a refused near part is not drawn
 * either; a lent one draws until a lite set needs its page). Their drops are logged and counted (hitch.c stream line) */
/* Only items out of view or ahead of the player are lent (C_PREF, C_FARC, C_VIEW2), so a lite set's drop never
 * removes something drawn (lendable). Lending every class (near parts up to MS_LEND_NEAR_KB): with B3's radii
 * all-class lending cost ~25 world_route pop-ins (36 / 34 vs 10-13 with MS_LEND=0, 2026-11-09); without B3 it brought
 * iso_ame_report3's horizon share back to main's */
/* Near parts lent (built in: the frozen residency design, 2026-11-12): near parts take idle pool pages once the item pages are full, up to
 * MS_LEND_NEAR_KB. Before (2026-11-10) they never could (lendable() is false for them), yet
 * the plan's cap_hi counted that room for them: near cells were planned in and then refused at the player's feet
 * while the structural pool had Firelink's 1.7 MB lite slot idle (Ame's ISO play report 2026-11-10: 252 "no room
 * for item", 167 of them near parts, with 1.4-4.5 MB of the arena free) */
unsigned ms_lent_near;  /* bytes of near parts placed in pool pages (approximate: decremented on drop) */
static unsigned reserve_extra, pool_max = MS_POOL;

/* make_window's scores per page: bits 0-7 highest class + 1 of its clearable groups, 0x200 not clearable; cost;
 * granules of clearable groups (a page whose used granules are not all clearable groups holds a read in flight or a
 * mapstream_alloc block) */
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
 * Built in: the frozen residency design, 2026-11-12 */
/* heap kept free: AGENTS.md's 1.5 MB in the heaviest spot + one Burg enemy type's load (model.bin falls back to the
 * heap, then its skin pages and compact copy; ~1.2 MB, world.c WORLD_HEAP_MODELS) */
static unsigned char *xpage[MS_XMAX];
static unsigned char xret[MS_XMAX];  /* being given back: no new items (its drawn ones wait for their chains) */
unsigned ms_nx;  /* borrowed pages now */
static unsigned x_want;  /* heap free a caller of mapstream_heap_release waits for (x_update drains pages until) */
static unsigned xstat_borrow, xstat_back, xstat_drop_kb;
static inline unsigned char *g_ptr(unsigned g)
{
    unsigned p = g / MS_GPP;
    return (p < ms_npages ? ms_arena + (size_t)p * MS_PAGE : xpage[p - ms_npages]) + (g % MS_GPP) * MS_GRAN;
}
static unsigned g_of(const void *ptr)
{
    const unsigned char *q = ptr;
    if (q >= ms_arena && q < ms_arena + arena_size)
        return (unsigned)(q - ms_arena) / MS_GRAN;
    for (unsigned i = 0; i < MS_XMAX; i++)
        if (xpage[i] && q >= xpage[i] && q < xpage[i] + MS_PAGE)
            return (ms_npages + i) * MS_GPP + (unsigned)(q - xpage[i]) / MS_GRAN;
    return ~0u;  /* not ours (a caller bug) */
}
/* the item pages: those below ilim(), then the borrowed ones (absent ones are marked full) */

static inline int g_used(unsigned g) { return gmap[g >> 5] >> (g & 31) & 1; }
static void g_mark(unsigned g0, unsigned n, int on)
{
    for (unsigned g = g0; g < g0 + n; g++) {
        unsigned b = 1u << (g & 31);
        if (on)
            gmap[g >> 5] |= b;
        else
            gmap[g >> 5] &= ~b;
        ms_pfree[g / MS_GPP] += on ? -1 : 1;
    }
}
/* n bytes (<= a page) inside one page: pages already in use first, so free pages stay whole. (Best fit with small
 * items from the top and page-sized groups from the bottom was tried, 2026-11-12: no-room refusals 440 -> 515 on
 * iso_burg_taurus, 100 -> 162 on iso_ame_report3; not kept) */
void *ms_g_alloc(unsigned n)
{
    unsigned ng = (n + MS_GRAN - 1) / MS_GRAN;
    if (!ng || ng > MS_GPP)
        return NULL;
    for (int pass = 0; pass < 2; pass++)
        FOR_IPAGES(p) {
            if (ms_pfree[p] < ng || (pass == 0) == (ms_pfree[p] == MS_GPP) || (p >= ms_npages && xret[p - ms_npages]))
                continue;
            unsigned run = 0, g0 = p * MS_GPP;
            for (unsigned g = g0; g < g0 + MS_GPP; g++) {
                run = g_used(g) ? 0 : run + 1;
                if (run == ng) {
                    g_mark(g + 1 - ng, ng, 1);
                    return g_ptr(g + 1 - ng);
                }
            }
        }
    return NULL;
}
/* n bytes in a run of whole free pages in [bot, top), the highest such run, placed at its top */
static void *g_alloc_range(unsigned n, unsigned bot, unsigned top)
{
    unsigned np = (n + MS_PAGE - 1) / MS_PAGE, ng = (n + MS_GRAN - 1) / MS_GRAN, run = 0;
    for (unsigned p = top; p-- > bot;) {
        run = ms_pfree[p] == MS_GPP ? run + 1 : 0;
        if (run == np) {
            unsigned end = (p + np) * MS_GPP;
            g_mark(end - ng, ng, 1);
            return ms_arena + (size_t)(end - ng) * MS_GRAN;
        }
    }
    return NULL;
}
void *ms_g_alloc_top(unsigned n, unsigned top) { return g_alloc_range(n, 0, top); }
/* MS_LEND: n bytes (<= a page) inside one page of the pools, pages already in use first */
void *ms_g_alloc_lend(unsigned n)
{
    unsigned ng = (n + MS_GRAN - 1) / MS_GRAN;
    if (!ng || ng > MS_GPP)
        return NULL;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned p = ms_mpage0; p < ms_npages; p++) {
            if (ms_pfree[p] < ng || (pass == 0) == (ms_pfree[p] == MS_GPP))
                continue;
            unsigned run = 0, g0 = p * MS_GPP;
            for (unsigned g = g0; g < g0 + MS_GPP; g++) {
                run = g_used(g) ? 0 : run + 1;
                if (run == ng) {
                    g_mark(g + 1 - ng, ng, 1);
                    return g_ptr(g + 1 - ng);
                }
            }
        }
    return NULL;
}
void ms_g_free(void *ptr, unsigned n)
{
    unsigned g0 = g_of(ptr);
    g_mark(g0, (n + MS_GRAN - 1) / MS_GRAN, 0);
}

static unsigned heap_free(void)
{
    struct mallinfo mi = mallinfo();
    return 32 * 1024 * 1024 - (unsigned)sbrk(0) + mi.fordblks;
}


int ms_ensure_arena(void)
{
    if (ms_arena)
        return 1;
    unsigned hf = heap_free(), rs = MS_RESERVE + reserve_extra;
    unsigned sz = hf > rs + (1u << 20) ? hf - rs : 0;
    sz = (sz < pool_max ? sz : pool_max) & ~(MS_PAGE - 1);
    ms_arena = sz ? memalign(MS_PAGE, sz) : NULL;
    if (ms_arena) {
        ms_npages = sz / MS_PAGE;
        unsigned nt = ntot();  /* + the borrowed pages' slots */
        gmap = calloc((nt * MS_GPP + 31) / 32, 4);
        ms_pfree = malloc(nt * sizeof *ms_pfree);
        pcls = malloc(nt * sizeof *pcls), pcost = malloc(nt * sizeof *pcost);
        pitem = malloc(nt * sizeof *pitem);
        if (!gmap || !ms_pfree || !pcls || !pcost || !pitem) {
            free(ms_arena), free(gmap), free(ms_pfree), free(pcls), free(pcost), free(pitem);
            pcls = pcost = pitem = NULL;
            ms_arena = NULL;
        } else {
            for (unsigned p = 0; p < ms_npages; p++)
                ms_pfree[p] = MS_GPP;
            for (unsigned p = ms_npages; p < ntot(); p++)  /* not borrowed: full */
                ms_pfree[p] = 0, memset(&gmap[p * MS_GPP / 32], 0xff, MS_GPP / 8);
            arena_size = sz;
            ms_ipages = ms_npages > 2 * (MS_OTHER / MS_PAGE) ? ms_npages - MS_OTHER / MS_PAGE : ms_npages / 2;
            unsigned sp = (pool_req[0] + MS_PAGE - 1) / MS_PAGE, mp = (pool_req[1] + MS_PAGE - 1) / MS_PAGE;
            if (sp && sp + mp + 16 < ms_npages) {  /* (16: at least 1 MB of items) */
                ms_pools_on = 1;
                spage0 = ms_npages - sp, ms_mpage0 = spage0 - mp, ms_ipages = ms_mpage0;
            }
        }
    }
    printf("mapstream: v2 arena %u KB in %u pages (heap free %u KB)\n", ms_arena ? arena_size >> 10 : 0, ms_npages, hf >> 10);
    if (ms_pools_on && arena_size < pool_max)
        printf("mapstream: arena %u KB is under the build's %u KB (heap): the visual room is smaller\n",
               arena_size >> 10, pool_max >> 10);
    if (ms_pools_on)
        printf("mapstream: arena structural %u KB, misc %u KB, visual %u KB\n", (ms_npages - spage0) * (MS_PAGE >> 10),
               (spage0 - ms_mpage0) * (MS_PAGE >> 10), ms_ipages * (MS_PAGE >> 10));
    else if (ms_arena && pool_req[0])
        printf("mapstream: arena pools off (structural %u KB + misc %u KB asked of %u KB)\n",
               pool_req[0] >> 10, pool_req[1] >> 10, arena_size >> 10);
    return ms_arena != NULL;
}


/* free the group of item id */
int ms_why;  /* why the next ms_drop drops (MS_WHY_*, set by its caller; back to MS_WHY_FREE after) */
void ms_drop(int k, unsigned id)
{
    MsInst *m = &ms_inst[k];
    unsigned a, e = grp_of(m, id, &a);
    if (ms_pools_on && ms_lent_n) {  /* (MS_LEND: one in the pools' pages) */
        unsigned g = g_of(m->buf[a]);
        int in_pool = g != ~0u && g >= ms_mpage0 * MS_GPP && g < ms_npages * MS_GPP;
        ms_lent_n -= in_pool;
        if (in_pool && (m->cls[a] == C_NEAR || m->cls[a] == C_NEAR2)) {
            unsigned b = grp_bytes(m, a, e);
            ms_lent_near = ms_lent_near > b ? ms_lent_near - b : 0;
        }
    }
    ms_g_free(m->buf[a], grp_bytes(m, a, e));
    for (unsigned j = a; j < e; j++) {
        if (it_tex(m, j) && ms_tex_set)
            ms_tex_set(k, m->rit[j - m->ncell].tag & ~MS_RES_TEX, NULL);
        unsigned b = it_bytes(m, j);
        if (m->ld && m->ld[j])
            m->ld[j] = (unsigned short)((ms_frame >> 5) + 2);
        if (m->dw)  /* why it went and the class it had (the thrash log) */
            m->dw[j] = (unsigned char)(ms_why | m->cls[j] << 4);
        m->buf[j] = NULL;
        m->state[j] = MS_NONE;
        mapstream_stats.cells--;
        mapstream_stats.bytes -= b;
        if (it_res(m, j)) {
            *(it_tex(m, j) ? &mapstream_stats.tex_kb : &mapstream_stats.res_kb) -= (b + 1023) >> 10;
            m->rgrp[j - m->ncell] = (unsigned short)(j - m->ncell);
        }
    }
    ms_why = MS_WHY_FREE;
}

/* the highest class over id's group (dropping it drops them all) */
int ms_grp_cls(const MsInst *m, unsigned id)
{
    unsigned a, e = grp_of(m, id, &a);
    int c = C_FREE;
    for (unsigned j = a; j < e; j++)
        c = m->cls[j] > c ? m->cls[j] : c;
    return c;
}
/* any item of id's group handed out in the last two map draws */
int ms_grp_chain(const MsInst *m, unsigned id)
{
    unsigned a, e = grp_of(m, id, &a);
    for (unsigned j = a; j < e; j++)
        if (in_chain(m->used[j]))
            return 1;
    return 0;
}
static unsigned grp_used(const MsInst *m, unsigned id)
{
    unsigned a, e = grp_of(m, id, &a), u = 0;
    for (unsigned j = a; j < e; j++)
        u = ms_frame - m->used[j] < ms_frame - u ? m->used[j] : u;
    return u;
}

/* MS_LEND: drop the lent items in pool pages [p0, p1) that no chain reads; the number a chain may still read */
static unsigned lend_reclaim_(unsigned p0, unsigned p1, int keep_near);
/* keep_near: near parts stay (mapstream_pool_prepare: ahead of time, they are what the player is looking at) */
static unsigned lend_reclaim_(unsigned p0, unsigned p1, int keep_near)
{
    unsigned busy = 0;
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; m->state && id < it_n(m); id++) {
            unsigned a;
            if (m->state[id] != MS_READY || (grp_of(m, id, &a), a != id) || !m->buf[id])
                continue;
            unsigned g = g_of(m->buf[id]);
            if (g == ~0u || g < p0 * MS_GPP || g >= p1 * MS_GPP)
                continue;
            if (keep_near && (m->cls[id] == C_NEAR || m->cls[id] == C_NEAR2))
                continue;
            if (!ms_settling && ms_grp_chain(m, id))  /* (a settle waited for VU1: no chain reads the arena; without this the
                                                 * lite set waited out the settle: "settle stopped", taurus 2026-11-09) */
                busy++;
            else {
                if (m->cls[id] == C_NEAR || m->cls[id] == C_NEAR2) {
                    hitch_lent_near();
                    printf("mapstream: lent near dropped for a lite set: item %d.%u (%u KB)\n", k, id,
                           grp_bytes(m, id, grp_of(m, id, &a)) >> 10);
                }
                ms_why = MS_WHY_LENT, ms_drop(k, id);
            }
        }
    }
    return busy;
}

/* Lent items go only where the room is needed (2026-11-12): n bytes of whole pool pages in [p0, p1), the cheapest run
 * of pages that hold nothing but lent items and free granules (fewest item bytes; a run a chain still reads only when
 * no other does). lend_reclaim dropped every lent item of the pool whenever one allocation found no hole, a 5 KB
 * collision tile included: ~250 of ~600 items read again within 30 s of their drop on the ISO Taurus route had gone
 * that way (the "thrash" log), most of them textures. Returns the items of the run a chain may still read (they stay:
 * the caller waits for VU1 and asks again); *found 0: no such run, nothing dropped. */
#define MS_POOL_PAGES_MAX 160
static unsigned lend_reclaim_n(unsigned n, unsigned p0, unsigned p1, int keep_near, int *found)
{
    static unsigned short ig[MS_POOL_PAGES_MAX];
    static unsigned char bz[MS_POOL_PAGES_MAX];
    unsigned np = (n + MS_PAGE - 1) / MS_PAGE, npool = p1 - p0;
    *found = 0;
    if (!np || np > npool || npool > MS_POOL_PAGES_MAX)
        return 0;
    memset(ig, 0, npool * sizeof *ig), memset(bz, 0, npool);
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; m->state && id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id) || !m->buf[id])
                continue;
            unsigned g = g_of(m->buf[id]);
            if (g == ~0u || g < p0 * MS_GPP || g >= p1 * MS_GPP)
                continue;
            unsigned p = g / MS_GPP - p0;
            ig[p] += (unsigned short)((grp_bytes(m, a, e) + MS_GRAN - 1) / MS_GRAN);
            if (keep_near && (m->cls[id] == C_NEAR || m->cls[id] == C_NEAR2))
                bz[p] |= 2;  /* stays */
            else if (!ms_settling && ms_grp_chain(m, id))
                bz[p] |= 1;
        }
    }
    int best = -1;
    unsigned bc = 0;
    for (unsigned p = 0; p + np <= npool; p++) {
        unsigned c = 0, ok = 1;
        for (unsigned j = p; j < p + np && ok; j++) {
            ok = ms_pfree[p0 + j] + ig[j] == MS_GPP && !(bz[j] & 2);  /* only lent items and free room, none that stays */
            c += ig[j] + (bz[j] & 1 ? 4 * MS_GPP : 0);
        }
        if (ok && (best < 0 || c < bc || (c == bc && np > 1)))  /* ties: page-sized ones low (tiles), runs high (lite sets) */
            best = (int)p, bc = c;
    }
    if (best < 0)
        return 0;
    *found = 1;
    unsigned busy = 0, w0 = (p0 + (unsigned)best) * MS_GPP, w1 = w0 + np * MS_GPP;
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; m->state && id < it_n(m); id++) {
            unsigned a;
            if (m->state[id] != MS_READY || (grp_of(m, id, &a), a != id) || !m->buf[id])
                continue;
            unsigned g = g_of(m->buf[id]);
            if (g == ~0u || g < w0 || g >= w1)
                continue;
            if (!ms_settling && ms_grp_chain(m, id)) {
                busy++;
                continue;
            }
            if (m->cls[id] == C_NEAR || m->cls[id] == C_NEAR2) {
                hitch_lent_near();
                printf("mapstream: lent near dropped for a lite set: item %d.%u (%u KB)\n", k, id,
                       grp_bytes(m, id, grp_of(m, id, &a)) >> 10);
            }
            ms_why = MS_WHY_LENT, ms_drop(k, id);
        }
    }
    return busy;
}
/* the lent items in the way of n bytes of [p0, p1): the cheapest run's (MS_LEND_ALL_DROP 0, the default), else all */
#ifndef MS_LEND_ALL_DROP
#define MS_LEND_ALL_DROP 0
#endif
static unsigned lend_reclaim_for(unsigned n, unsigned p0, unsigned p1, int keep_near)
{
    int found = 0;
    unsigned busy = MS_LEND_ALL_DROP ? 0 : lend_reclaim_n(n, p0, p1, keep_near, &found);
    /* no run of lent-only pages: the pool's own blocks are in the way and dropping items cannot help, except in a
     * settle or with MS_LEND_ALL_DROP, where the pool is cleared as before */
    return found ? busy : MS_LEND_ALL_DROP || ms_settling ? lend_reclaim_(p0, p1, keep_near) : 0;
}

/* ---- making room ----
 * Clear `need` whole pages (a window; need = 1 for a read group): every group in the window must be loaded, of a class
 * below `below`, and not handed out in the last two map draws (in_chain). Cost per page: (highest class in it, then
 * the bytes and recency of its groups); the cheapest window from the top goes. 1 when cleared. */
int ms_nowin = C_TOP + 2;  /* a window of 1 page was not found for this class or above since the last change */
static int make_window_(unsigned need, unsigned ng_need, int below, unsigned *kb);
int ms_make_window(unsigned need, unsigned ng_need, int below, unsigned *kb)
{
    HITCH_BEGIN(w);
    int r = make_window_(need, ng_need, below, kb);
    HITCH_END(w, HS_MSWIN);
    return r;
}
static int make_window_(unsigned need, unsigned ng_need, int below, unsigned *kb)
{
    unsigned lim = ng_need || ms_pools_on ? ilim() : ms_npages;  /* a read's window lies where items may go (MS_POOLS: and
                                                             * mapstream_alloc's beyond the pools) */
    if (!need || need > ms_npages || !pcls || (need == 1 && below >= ms_nowin))
        return 0;
    memset(pcls, 0, ntot() * sizeof *pcls);
    memset(pcost, 0, ntot() * sizeof *pcost);
    memset(pitem, 0, ntot() * sizeof *pitem);
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                continue;
            int c = ms_grp_cls(m, id);
            unsigned u = grp_used(m, id), n = grp_bytes(m, a, e);
            unsigned g0 = g_of(m->buf[a]), ng = (n + MS_GRAN - 1) / MS_GRAN;
            int ok = c < below && !ms_grp_chain(m, id);
            unsigned age = ms_frame - u > 1000 ? 1000 : ms_frame - u;  /* cost: bytes, older groups cheaper */
            for (unsigned p = g0 / MS_GPP; p <= (g0 + ng - 1) / MS_GPP; p++) {
                unsigned lo = p * MS_GPP > g0 ? p * MS_GPP : g0, hi = (p + 1) * MS_GPP < g0 + ng ? (p + 1) * MS_GPP : g0 + ng;
                if (ok) {
                    pitem[p] += hi - lo;
                    if ((unsigned)c + 1 > (pcls[p] & 0xff))
                        pcls[p] = (pcls[p] & ~0xffu) | (c + 1);
                    pcost[p] += (n >> 8) + (1000 - age) / 10;
                } else {
                    pcls[p] |= 0x200;
                }
            }
        }
    }
    /* a page is clearable when its used granules are all clearable groups; for one group of ng_need granules, also
     * when its free granules and those of its clearable groups add up to that (the pinned and drawn groups of mixed
     * pages stay; whether the hole is contiguous shows when the group is placed) */
    for (unsigned p = 0; p < ntot(); p++) {
        if (pitem[p] != MS_GPP - ms_pfree[p])
            pcls[p] |= 0x200;
        if (need == 1 && (pcls[p] & 0x200) && pitem[p] && ms_pfree[p] + pitem[p] >= ng_need)
            pcls[p] = (pcls[p] & ~0x200u) | 0x400;  /* partly clearable: after the whole pages */
    }
    int best = -1;
    unsigned long long bestc = ~0ull;
    for (unsigned p = lim >= need ? lim - need + 1 : 0; p-- > 0;) {
        unsigned hi = 0;
        unsigned long long c = 0;
        unsigned q;
        for (q = p; q < p + need; q++) {
            if (pcls[q] & 0x200)
                break;
            hi = (pcls[q] & 0x4ff) > hi ? pcls[q] & 0x4ff : hi;
            c += pcost[q];
        }
        if (q < p + need)
            continue;
        unsigned long long score = (unsigned long long)hi << 40 | c;
        if (score < bestc)
            bestc = score, best = (int)p;
    }
    for (unsigned p = ms_npages; need == 1 && ng_need && p < ntot(); p++) {  /* a read: borrowed pages too */
        if (!xpage[p - ms_npages] || xret[p - ms_npages] || (pcls[p] & 0x200))
            continue;
        unsigned long long score = (unsigned long long)(pcls[p] & 0x4ff) << 40 | pcost[p];
        if (score < bestc)
            bestc = score, best = (int)p;
    }
    if (best < 0) {
        if (need == 1 && below < ms_nowin)
            ms_nowin = below;
        return 0;
    }
    unsigned w0 = best * MS_GPP, w1 = (best + need) * MS_GPP;  /* granules */
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                continue;
            unsigned o = g_of(m->buf[a]);
            if (o < w1 && o + (grp_bytes(m, a, e) + MS_GRAN - 1) / MS_GRAN > w0 && ms_grp_cls(m, id) < below && !ms_grp_chain(m, id)) {
                if (kb)
                    *kb += grp_bytes(m, a, e) >> 10;
                hitch_drop(ms_grp_cls(m, id) >= C_PREF, grp_bytes(m, a, e));
                ms_why = MS_WHY_WINDOW, ms_drop(k, id);
            }
        }
    }
    return 1;
}

/* ---- moving items (MS_DEFRAG) ----
 * Ame's ISO play report (2026-11-08): 101 "no room" in 200 s with ~1 MB of the arena free. The free granules lay in
 * holes of ~10 KB over every page, and every page held a group drawn this frame or of a higher class, so make_window
 * found nothing to clear: near cells stayed at the coarse level or the horizon (holes, "never loads the higher LOD",
 * untextured stand-ins) and the relief window (732 KB) never came (normal maps). A group moves instead: copied into a
 * hole of another page, its pointers (and its texture's mip 0, tex_set) follow at once, and the old copy stays used
 * until no VIF chain built before the move can still read it (the in_chain rule: two map draws later), then frees.
 * Built in: the frozen residency design, 2026-11-12 */
static struct { unsigned g0, ng, fr; } pend[MS_DEFRAG_PEND];
int ms_npend;
unsigned ms_pend_frame;  /* frame of the last move */
unsigned ms_defrag_moves, ms_defrag_kb;

/* old copies no chain can read any more go back */
void ms_pend_release(void)
{
    for (int i = 0; i < ms_npend; i++)
        if (ms_chains_done || !ms_dprev || (int)(ms_dprev - pend[i].fr) > 0) {
            g_mark(pend[i].g0, pend[i].ng, 0);
            pend[i--] = pend[--ms_npend];
        }
}

/* n bytes inside one page outside pages [x0, x1) (below ilim): fullest page with a fitting hole first */
static void *g_alloc_ex(unsigned n, unsigned x0, unsigned x1)
{
    unsigned ng = (n + MS_GRAN - 1) / MS_GRAN, bp = ~0u, bg = 0;
    if (!ng || ng > MS_GPP)
        return NULL;
    FOR_IPAGES(p) {
        if ((p >= x0 && p < x1) || (p >= ms_npages && xret[p - ms_npages]) || ms_pfree[p] < ng || (bp != ~0u && ms_pfree[p] >= ms_pfree[bp]))
            continue;
        unsigned run = 0, g0 = p * MS_GPP;
        for (unsigned g = g0; g < g0 + MS_GPP; g++) {
            run = g_used(g) ? 0 : run + 1;
            if (run == ng) {
                bp = p, bg = g + 1 - ng;
                break;
            }
        }
    }
    if (bp == ~0u)
        return NULL;
    g_mark(bg, ng, 1);
    return g_ptr(bg);
}

/* Empty `need` whole pages below `lim` by moving their groups elsewhere: the window whose used granules are all
 * movable groups (loaded, inside one page) with the fewest of them. 1 when every group of the window moved (its pages
 * free once the old copies go back, pend_release), 0 when no window qualifies or a group found no hole. */
int ms_defrag(unsigned need, unsigned lim, unsigned max_kb)
{
    if (!pitem || ms_npend > MS_DEFRAG_PEND - 16 || need > lim)
        return 0;
    HITCH_BEGIN(d);
    memset(pitem, 0, ntot() * sizeof *pitem);
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                continue;
            unsigned n = grp_bytes(m, a, e), g0 = g_of(m->buf[a]);
            unsigned ng = (n + MS_GRAN - 1) / MS_GRAN;
            if (g0 / MS_GPP == (g0 + ng - 1) / MS_GPP)
                pitem[g0 / MS_GPP] += ng;
        }
    }
    unsigned fr = 0;
    FOR_IPAGES(p)
        fr += ms_pfree[p];
    int best = -1;
    unsigned bused = ~0u;
    for (unsigned p = lim - need + 1; p-- > 0;) {
        unsigned used = 0, q;
        for (q = p; q < p + need; q++) {
            unsigned u = MS_GPP - ms_pfree[q];
            if (pitem[q] != u)
                break;  /* a read in flight, an old copy or a mapstream_alloc block */
            used += u;
        }
        if (q < p + need || !used)
            continue;
        unsigned wfree = 0;
        for (q = p; q < p + need; q++)
            wfree += q < ilim() ? ms_pfree[q] : 0;
        if (used < bused && used <= max_kb * 1024 / MS_GRAN && fr - wfree >= used + used / 4)
            bused = used, best = (int)p;
    }
    int ok = best >= 0;
    if (ok) {
        unsigned w0 = (unsigned)best * MS_GPP, w1 = w0 + need * MS_GPP, kb = 0;
        for (int k = 0; k < MS_INST && ok; k++) {
            MsInst *m = &ms_inst[k];
            for (unsigned id = 0; id < it_n(m) && ok; id++) {
                unsigned a, e;
                if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                    continue;
                unsigned char *src = m->buf[a];
                unsigned n = grp_bytes(m, a, e), g0 = g_of(src), ng = (n + MS_GRAN - 1) / MS_GRAN;
                if (g0 < w0 || g0 >= w1)
                    continue;
                unsigned char *dst = ms_npend < MS_DEFRAG_PEND ? g_alloc_ex(n, (unsigned)best, (unsigned)best + need) : NULL;
                if (!dst) {
                    ok = 0;
                    break;
                }
                memcpy(dst, src, n);
                SyncDCache(dst, dst + n - 1);  /* the VIF / GIF DMA reads RAM */
                for (unsigned j = a; j < e; j++) {
                    m->buf[j] = dst + ((unsigned char *)m->buf[j] - src);
                    if (it_tex(m, j) && ms_tex_set)
                        ms_tex_set(k, m->rit[j - m->ncell].tag & ~MS_RES_TEX, m->buf[j]);
                }
                pend[ms_npend++] = (typeof(pend[0])){g0, ng, ms_frame};
                kb += n;
                ms_defrag_moves++;
            }
        }
        ms_defrag_kb += kb >> 10;
        ms_pend_frame = ms_frame;
    }
    HITCH_END(d, HS_MSWIN);
    return ok;
}

/* ---- borrowed pages: give back, borrow (MS_BORROW, comment at xpage) ---- */
/* Give back the borrowed page whose items can all go now (loaded groups no chain reads: no read in flight, no old
 * defrag copy), the emptiest first. 1 when a page went back to the heap. */
static int x_release(int force)
{
    unsigned ok[MS_XMAX];
    if (!ms_nx)
        return 0;
    memset(ok, 0, sizeof ok);
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                continue;
            unsigned g0 = g_of(m->buf[a]);
            if (g0 / MS_GPP < ms_npages || g0 == ~0u || ms_grp_chain(m, id))
                continue;
            ok[g0 / MS_GPP - ms_npages] += (grp_bytes(m, a, e) + MS_GRAN - 1) / MS_GRAN;
        }
    }
    int best = -1;
    for (unsigned i = 0; i < MS_XMAX; i++) {
        unsigned p = ms_npages + i;
        if (xpage[i] && ok[i] == MS_GPP - ms_pfree[p] && (best < 0 || ms_pfree[p] > ms_pfree[ms_npages + best]))
            best = (int)i;
    }
    if (best < 0) {
        /* every borrowed page holds a group a chain may still read (cells drawn each frame stay in the last two
         * draws: the heap sat at 1.48 MB with 448 KB lent at the Firelink cave, ISO 2026-11-08). Empty the
         * fullest-free one now: its groups go (drawn ones keep their granules until their chains are done, as
         * defrag's old copies, pend_release) and no new item goes in; the next call gives it back. */
        if (!force)
            return 0;
        for (unsigned i = 0; i < MS_XMAX; i++)
            if (xpage[i] && xret[i])
                return 0;  /* one at a time: that one is still draining */
        for (unsigned i = 0; i < MS_XMAX; i++)
            if (xpage[i] && (best < 0 || ms_pfree[ms_npages + i] > ms_pfree[ms_npages + best]))
                best = (int)i;
        if (best < 0 || ms_npend > MS_DEFRAG_PEND - 16)
            return 0;
        unsigned p = ms_npages + best, w0 = p * MS_GPP, w1 = w0 + MS_GPP;
        xret[best] = 1;
        for (int k = 0; k < MS_INST; k++) {
            MsInst *m = &ms_inst[k];
            for (unsigned id = 0; id < it_n(m); id++) {
                unsigned a, e;
                if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                    continue;
                unsigned g0 = g_of(m->buf[a]);
                if (g0 < w0 || g0 >= w1)
                    continue;
                unsigned n = grp_bytes(m, a, e), ng = (n + MS_GRAN - 1) / MS_GRAN;
                int chain = ms_grp_chain(m, id);
                xstat_drop_kb += n >> 10;
                hitch_drop(ms_grp_cls(m, id) >= C_PREF, n);
                ms_why = MS_WHY_BORROW, ms_drop(k, id);
                if (chain && ms_npend < MS_DEFRAG_PEND) {  /* the granules stay used until no chain reads them */
                    g_mark(g0, ng, 1);
                    pend[ms_npend++] = (typeof(pend[0])){g0, ng, ms_frame};
                }
            }
        }
        return 0;
    }
    unsigned p = ms_npages + best, w0 = p * MS_GPP, w1 = w0 + MS_GPP;
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                continue;
            unsigned g0 = g_of(m->buf[a]);
            if (g0 >= w0 && g0 < w1) {
                xstat_drop_kb += grp_bytes(m, a, e) >> 10;
                hitch_drop(ms_grp_cls(m, id) >= C_PREF, grp_bytes(m, a, e));
                ms_why = MS_WHY_BORROW, ms_drop(k, id);
            }
        }
    }
    if (ms_pfree[p] != MS_GPP)  /* cannot happen (ok[] counted every used granule) */
        return 0;
    ms_pfree[p] = 0;
    memset(&gmap[w0 / 32], 0xff, MS_GPP / 8);
    free(xpage[best]), xpage[best] = NULL, xret[best] = 0, ms_nx--, xstat_back++;
    mapstream_stats.lent_kb = ms_nx * (MS_PAGE >> 10);
    return 1;
}

/* Once per frame: borrow a page while the item pages are (nearly) full and the heap has room over the floor; give
 * one back while the heap is under it */
void ms_x_update(void)
{
    if (!ms_arena)
        return;
    unsigned hf = heap_free(), floor = x_want > MS_BORROW_FLOOR ? x_want : MS_BORROW_FLOOR;
    if (!ms_nx || hf >= x_want)
        x_want = 0;
    if (ms_nx && hf < floor) {  /* up to 4 pages a frame: back over the floor within a few frames */
        for (int i = 0; i < 4 && hf < floor && x_release(1); i++)
            hf = heap_free();
        return;
    }
    unsigned fr = 0;
    FOR_IPAGES(p)
        fr += ms_pfree[p];
    if (ms_nx >= MS_XMAX || fr >= 2 * MS_GPP || hf < MS_BORROW_FLOOR + MS_BORROW_HYST + MS_PAGE)
        return;
    for (unsigned j = 0; j < MS_XMAX; j++)
        if (xret[j])
            return;  /* one is being given back: none borrowed meanwhile */
    unsigned i = 0;
    while (xpage[i])
        i++;
    if (!(xpage[i] = memalign(MS_GRAN, MS_PAGE)))
        return;
    unsigned p = ms_npages + i;
    memset(&gmap[p * MS_GPP / 32], 0, MS_GPP / 8);
    ms_pfree[p] = MS_GPP;
    ms_nx++, xstat_borrow++;
    mapstream_stats.lent_kb = ms_nx * (MS_PAGE >> 10);
    ms_nowin = C_TOP + 2;  /* new room: windows may be found again */
}

unsigned mapstream_heap_release(unsigned bytes)
{
    unsigned n = 0, ms_want = (bytes + MS_PAGE - 1) / MS_PAGE + 1;
    while (n < ms_want && x_release(0))
        n++;
    if (n < ms_want && ms_nx) {  /* the rest drains over the next frames (pages with drawn cells: x_release) */
        unsigned t = heap_free() + (ms_want - n) * MS_PAGE;
        x_want = t > x_want ? t : x_want;
        x_release(1);
    }
    if (n)
        printf("mapstream: gave %u borrowed pages back to the heap for %u KB (%u left)\n", n, bytes >> 10, ms_nx);
    return n;
}

void mapstream_pool(unsigned max) { pool_max = max; }
void mapstream_reserve(unsigned extra) { reserve_extra = extra; }
int mapstream_ready(void) { return ms_arena != NULL; }
int mapstream_arena(void)
{
    return ms_ensure_arena();
}
int mapstream_owns(const void *p)
{
    return ms_arena && (const unsigned char *)p >= ms_arena && (const unsigned char *)p < ms_arena + arena_size;
}

void mapstream_pools(unsigned struct_bytes, unsigned misc_bytes)
{
    if (!ms_arena)
        pool_req[0] = struct_bytes, pool_req[1] = misc_bytes;
}

int mapstream_pools_on(void) { return ms_pools_on; }
int mapstream_pools_on_build(void) { return 1; }
int mapstream_pool_wait(void) { return pool_busy; }
unsigned mapstream_lend_early(void) { unsigned n = early_n; early_n = 0; return n; }

int mapstream_pool_prepare(int pool, unsigned n)
{
    if (!ms_pools_on || !ms_lent_n)
        return 1;
    unsigned p0 = pool == MS_POOL_STRUCT ? spage0 : ms_mpage0, p1 = pool == MS_POOL_STRUCT ? ms_npages : spage0;
    void *p = g_alloc_range(n, p0, p1);
    if (p) {
        ms_g_free(p, n);
        return 1;
    }
    unsigned before = ms_lent_n;
    lend_reclaim_for(n, p0, p1, 1);
    early_n += before - ms_lent_n;
    return 0;
}

void *mapstream_alloc_pool(unsigned n, int pool)
{
    if (!mapstream_arena() || !ms_pools_on)
        return NULL;
    unsigned p0 = pool == MS_POOL_STRUCT ? spage0 : ms_mpage0, p1 = pool == MS_POOL_STRUCT ? ms_npages : spage0;
    void *p = g_alloc_range(n, p0, p1);
    pool_busy = 0;
    if (!p && ms_lent_n) {  /* MS_LEND: the lent items go */
        if (lend_reclaim_for(n, p0, p1, 0) && !(p = g_alloc_range(n, p0, p1))) {
            /* some are in a chain: cells drawn every frame never leave it (a lite set waited 13.8 s behind them,
             * taurus 2026-11-09), so wait for VU1 once and drop them all */
            vu1_wait();
            ms_chains_done = 1;
            pool_busy = lend_reclaim_for(n, p0, p1, 0) > 0;
            ms_chains_done = 0;
        }
        if (!p)
            p = g_alloc_range(n, p0, p1);
    }
    if (p)
        mapstream_stats.other_kb += (n + 1023) >> 10;
    return p;
}

/* mapstream_alloc_pool for blocks of at most a page (collision tiles, src/coltile.c: 1-55 KB each): granules inside
 * one page of the pool, pages already in use from the pool's bottom first, so whole pages stay free at its top for the
 * lite sets (g_alloc_range takes whole pages: a 3 KB tile there held a 64 KB page) */
void *mapstream_alloc_pool_small(unsigned n, int pool)
{
    unsigned ng = (n + MS_GRAN - 1) / MS_GRAN;
    if (!mapstream_arena() || !ms_pools_on || !ng || ng > MS_GPP)
        return mapstream_alloc_pool(n, pool);
    unsigned p0 = pool == MS_POOL_STRUCT ? spage0 : ms_mpage0, p1 = pool == MS_POOL_STRUCT ? ms_npages : spage0;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned p = p0; p < p1; p++) {
            if (ms_pfree[p] < ng || (pass == 0) == (ms_pfree[p] == MS_GPP))
                continue;
            unsigned run = 0, g0 = p * MS_GPP;
            for (unsigned g = g0; g < g0 + MS_GPP; g++) {
                run = g_used(g) ? 0 : run + 1;
                if (run == ng) {
                    g_mark(g + 1 - ng, ng, 1);
                    mapstream_stats.other_kb += (n + 1023) >> 10;
                    return g_ptr(g + 1 - ng);
                }
            }
        }
    return mapstream_alloc_pool(n, pool);  /* (no hole: lent items may go) */
}

/* the top of the item pages mapstream_alloc may clear (all pages without pools) */
static inline unsigned alloc_top(void) { return ms_pools_on ? ms_ipages : ms_npages; }
static void *alloc_items(unsigned n)
{
    void *p = ms_g_alloc_top(n, alloc_top());
    if (p && ms_pools_on)
        ms_items_other += (n + MS_GRAN - 1) / MS_GRAN * MS_GRAN;
    return p;
}

void *mapstream_alloc(unsigned n, const float pos[3])
{
    (void)pos;  /* ranks come from the last plan */
    if (!mapstream_arena())
        return NULL;
    void *p = mapstream_alloc_pool(n, MS_POOL_MISC);  /* (MS_POOLS: enemy model temporaries and the like) */
    if (!p && pool_busy) {
        /* MS_LEND: map items lent the misc pool are in this or the last frame's chain. Wait for VU1 rather than clear
         * item pages: enemy model temporaries took 300-600 KB of drawn cells from the visual room at each load, so the
         * room the export was solved to was not a constant (taurus / report3 with enemies, 2026-11-09) */
        vu1_wait();
        ms_chains_done = 1;
        p = mapstream_alloc_pool(n, MS_POOL_MISC);
        ms_chains_done = 0;
    }
    if (p)
        return p;
    p = alloc_items(n);
    unsigned need = (n + MS_PAGE - 1) / MS_PAGE, kb = 0;
    /* clear a window: of items below the near class first, then of anything but the pinned level, last of anything
     * (a lite set holds the collision the player walks onto next); never of items a VIF chain may read (in_chain) */
    if (!p && (ms_make_window(need, 0, C_NEAR2, &kb) || ms_make_window(need, 0, C_PIN, &kb) || ms_make_window(need, 0, C_TOP + 1, &kb)))
        p = alloc_items(n);
    if (!p) {
        /* every window holds a drawn item: the lite set of the block the player walks onto next (983 KB of the
         * bonfire block's collision) found none for minutes and the player walked off the loaded floor (ISO
         * 2026-11-06). Once VU1 is idle no chain of an earlier frame reads the arena: clear the cheapest window, drawn
         * items too (they read back; a frame or two of coarse level or hole beats no floor) */
        vu1_wait();
        ms_chains_done = 1;
        ms_nowin = C_TOP + 2;
        if (ms_make_window(need, 0, C_TOP + 1, &kb))
            p = alloc_items(n);
        if (!p && ms_nrd) {
            /* still none: the pages reads in flight land in cannot be cleared, and up to MS_INFLIGHT small reads are
             * spread over the arena (indexed map data: more, smaller items; a 1.3 MB enemy model temporary at the
             * Taurus tower warp fell back to the heap, RAM free 1.66 MB, ISO 2026-11-07). Stop them all (the plan asks
             * again) and try once more */
            unsigned c = 0;
            for (int i = 0; i < ms_nrd; i++) {
                pack_req_cancel(ms_rd[i].tk);  /* returns once nothing more lands in the buffer */
                c += ms_rd[i].bytes >> 10;
                ms_drop(ms_rd[i].k, ms_rd[i].id);
            }
            printf("mapstream: alloc %u KB: %d reads in flight (%u KB) cancelled for a window\n", n >> 10, ms_nrd, c);
            ms_cancel_kb += c;
            ms_nrd = 0;
            mapstream_stats.pending = 0;
            if (ms_make_window(need, 0, C_TOP + 1, &kb))
                p = alloc_items(n);
        }
        ms_chains_done = 0;
    }
    if (kb || !p)  /* the caller falls back to the heap on NULL */
        printf("mapstream: alloc %u KB %s, %u KB of cells dropped\n", n >> 10, p ? "ok" : "FAILED", kb);
    if (p)
        mapstream_stats.other_kb += (n + 1023) >> 10;
    return p;
}

void *mapstream_alloc_soft(unsigned n)
{
    if (!mapstream_arena())
        return NULL;
    void *p = alloc_items(n);
    unsigned kb = 0;
    if (!p && ms_make_window((n + MS_PAGE - 1) / MS_PAGE, 0, C_PREF, &kb))  /* only what no class wants */
        p = alloc_items(n);
    if (p)
        mapstream_stats.other_kb += (n + 1023) >> 10;
    else {
        /* the room is there but in holes (MS_DEFRAG): empty a window for the next call (world.c asks again) when
         * the arena's free granules cover it with MS_DEFRAG_KB to spare */
        unsigned fr = 0;
        for (unsigned i = 0; i < ms_npages; i++)
            fr += ms_pfree[i];
        int mv = !ms_npend && fr * MS_GRAN >= n + (MS_DEFRAG_KB << 10) &&
                 ms_defrag((n + MS_PAGE - 1) / MS_PAGE, alloc_top(), 3 * MS_DEFRAG_KB);
        printf("mapstream: soft alloc %u KB: no room beside the wanted cells%s (%u KB free)\n", n >> 10,
               mv ? ", window being emptied" : "", fr * MS_GRAN >> 10);
    }
    return p;
}

void mapstream_free(void *p, unsigned n)
{
    if (!p)
        return;
    if (ms_pools_on && g_of(p) < ms_ipages * MS_GPP)
        ms_items_other -= (n + MS_GRAN - 1) / MS_GRAN * MS_GRAN;
    ms_g_free(p, n);
    mapstream_stats.other_kb -= (n + 1023) >> 10;
    ms_replan = 1;  /* room for what was refused */
    ms_nblk = 0, ms_nowin = C_TOP + 2;
}
