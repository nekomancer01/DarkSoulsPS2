/* World streaming v2 (2026-11-06): map cells by a residency plan, the API of mapstream.h.
 *
 * The first streamer (mapstream_v1.c, `make MAPSTREAM_V1=1`) ranked every item by distance times a dozen factors and
 * let any item evict any item ranked 1.4x farther. Around the Burg entry it wanted 20-27 MB of cells for a 10.8 MB
 * arena, so whatever ranked last was missing, coarse cells included: holes. v2 follows the design of Vice City's
 * streamer (re3 CStreaming: low-detail models always resident, high-detail ones requested around the camera, the least
 * recently used unreferenced model removed to make room, never one in use), with the data sized at export
 * (tools/export_map.py solve_budget) so that what a spot needs fits:
 *
 * Every item gets a class from its distance to the player (box distance) every MS_SCAN_FRAMES or MS_SCAN_D metres,
 * highest first:
 *   C_PIN    the coarse level and kept meshes of cells within MS_R1_PIN, the backdrop and sky of the player's block
 *   C_VIEW   coarse cells within MS_R1_VIEW that map.c drew or asked for lately (in view)
 *   C_NEAR   near parts of cells within MS_R0_FIRST
 *   C_NEAR2  near parts within MS_R0_DRAW (map.c draws them there), foliage within MS_GRASS_WANT
 *   C_VIEW2  coarse cells in view out to MS_R1_WANT, another block's backdrop in view
 *   C_FARC   coarse cells and backdrop not in view (behind the camera)
 *   C_PREF   near parts within MS_R0_WANT (read ahead of walking in), mip 0 textures asked for
 *   C_KEEP   loaded and within its keep radius, or anything ranked past what the arena holds: kept while there is
 *            room, never read
 *   C_FREE   dropped once no VIF chain reads it
 * The plan is cut to fit: wanted items in rank order (class, then distance) until their bytes fill the item pages; the
 * rest become C_KEEP. So the plan always fits and the reader never stalls on items it cannot place.
 * Missing items of class C_PREF and up are read, highest class first, nearest first (in view counts as half the
 * distance), neighbouring resident items together in one request of up to MS_RUN bytes. A read that finds no room may
 * clear one 64 KB page whose items are all of a lower class than its own (lowest class, least recently drawn first),
 * so a coarse cell in view is never dropped for a near cell, and a pinned one only for a block's lite set.
 *
 * Memory: one arena allocated at boot (as v1), managed in 256-byte granules on a grid of 64 KB pages: a read group of
 * up to a page never crosses one (export_map.NEAR_PART, map.c RES_ITEM keep items within a page), so any free page
 * fits any group; mapstream_alloc (lite sets, relief, enemy model buffers) takes runs of whole free pages from the
 * top, clearing the cheapest run first. */
#ifdef MAPSTREAM_V1
#include "mapstream_v1.c"
#else
#include <kernel.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mapstream.h"
#include "pack.h"
#include "vu1.h"

/* Radii (box distance, m). Near: map.c draws a cell's near level within NEAR_DRAW_R + NEAR_HYST = 26 m; read 4 m
 * ahead (~1 s of running, ASSUMED), kept 6 m more against reading again on a turn back. Coarse: pinned within
 * MS_R1_PIN (export_map.WS_R1: the spot budget covers it), wanted out to MS_R1_WANT (map.c CULL_FAR 250 m + a box
 * margin), kept to MS_R1_KEEP. */
#define MS_R0_DRAW 26.0f
/* read ahead out to MS_R0_WANT (export_map.WS_R0 budgets 30 m; the plan cut keeps the extra within the arena, C_PREF
 * is the lowest wanted class): at 30 m, 4 m past the draw radius, cells inside it were still coarse when the player
 * walked in (pop-in, ISO 2026-11-06) */
#ifndef MS_NEAR_FIRST
#define MS_NEAR_FIRST 1
#endif
#if MS_NEAR_FIRST
#define MS_R0_WANT 40.0f
#define MS_R0_KEEP 46.0f
#else
#define MS_R0_WANT 30.0f
#define MS_R0_KEEP 36.0f
#endif
/* near parts rank by the nearer of the feet and a point MS_AHEAD_FRAMES ahead along the walking direction (velocity
 * smoothed over ~10 frames; ASSUMED 1.5 s: about the read time of a near cell group from cdrom0: plus a margin), so
 * the cells the player walks into come before those behind (MS_NEAR_FIRST) */
#define MS_AHEAD_FRAMES 45.0f
#define MS_R1_PIN 64.0f   /* pinned coarse; export_map.WS_R1 (96 m) budgets a little beyond it */
#define MS_R0_FIRST 16.0f /* near parts this close rank above coarse cells in view (C_NEAR), the rest of 26 m below (C_NEAR2) */
/* MS_NEAR_FIRST (default on; make DEFS=-DMS_NEAR_FIRST=0 = the old order): the near parts within MS_R0_FIRST rank
 * above the pinned coarse level out of view (C_PIN0 within MS_R1_FIRST and the sky / backdrop of the player's block >
 * C_VIEW coarse in view > C_NEAR > C_PIN coarse out of view). With every pinned coarse cell of all blocks within 64 m and every one in view within 128 m ahead of
 * them, the near level around the player waited behind megabytes of far coarse cells after each spawn (user report,
 * 2026-11-06). The coarse cells close by still go first: a missing one is a hole at the player's feet. */
#ifndef MS_NEAR_FIRST
#define MS_NEAR_FIRST 1
#endif
#define MS_R1_FIRST 32.0f
/* MS_CANCEL (default on; 0 = off): a read in flight whose items all fell below C_PREF on a re-plan (the player moved
 * on or warped, a settle began) is cancelled and its pages freed (pack_req_cancel stops it after its current 64 KB
 * page). Without it up to MS_INFLIGHT x MS_RUN = 2 MB of stale reads went on landing ahead of what the new spot
 * needs. */
#ifndef MS_CANCEL
#define MS_CANCEL 1
#endif
/* MS_SETTLE_NEAR (default on; 0 = the old settle, coarse level only): a settle (DSR's loading screen after a warp or
 * spawn) also reads the near parts within MS_R0_DRAW of the feet and waits for them (mapstream_busy), so the player
 * starts with the near level drawn instead of watching it stream in over the coarse one (Ame, 2026-11-06). world.c
 * still ends a settle after its time limit. */
#ifndef MS_SETTLE_NEAR
#define MS_SETTLE_NEAR 1
#endif
#define MS_SETTLE_NEAR_MS 10000u  /* ASSUMED cap: past it the settle ends with what is in, the rest streams in */
#define MS_R1_VIEW 128.0f /* coarse cells in view this close: C_VIEW, beyond: C_VIEW2 */
#define MS_R1_WANT 260.0f
#define MS_R1_KEEP 280.0f
/* foliage items (MS_RES_GRASS): map.c draws tufts at (GRASS_D0 10 m / d)^2 of their density, ~6 % at 40 m (ASSUMED
 * radius); kept 8 m more */
#define MS_GRASS_WANT 40.0f
#define MS_GRASS_KEEP 48.0f
/* backdrop chunks (MS_RES_FAR) rank as if no farther than this: they are the horizon wherever the player stands */
#define MS_FAR_D 64.0f
/* an item map.c asked for (drawn, or wanted while missing) within MS_VIEW_FRAMES counts as in view: its rank is
 * MS_VIEW_K x the squared distance (half the distance); ASSUMED 1 s so a look around does not flip classes */
#define MS_VIEW_FRAMES 30u
#define MS_VIEW_K 0.25f
/* re-plan when the player moved this far or this many frames passed (~5000 items over 5 blocks: a scan every frame
 * cost the Burg bonfire 30 -> 22 fps with v1) */
#define MS_SCAN_D 4.0f
#define MS_SCAN_FRAMES 16u
/* a texture's mip 0 kept while texcache asked for it within this many frames (as v1) */
#define MS_TEX_KEEP 120u
/* an item that found no room is passed over this many frames (the next one reads meanwhile) */
#define MS_BLOCK_FRAMES 30u
#ifndef MS_POOL
#define MS_POOL (8u << 20)
#endif
/* EE heap left outside the arena when it is allocated (as v1): the 1.5 MB reserve + 1.5 MB for what loads later */
#define MS_RESERVE (3u << 20)
#define MS_PAGE (64u << 10)
/* The top of the arena is kept for mapstream_alloc (block lite sets up to 1.7 MB, relief ~0.9 MB, enemy model.bin
 * buffers up to 0.6 MB): map items never go there. Without it the pinned and drawn items filled every page, model
 * buffers fell back to the heap and fragmented it (c2540 model.bin OUT OF RAM in world_cross, host ELF, 2026-11-06);
 * v1 dropped drawn cells for them instead, which hung PCSX2 when a queued chain still read them. Sized for one enemy
 * model.bin (lite sets and relief clear windows below it): 2 MB cost the route's map items 2 MB and its worst frame
 * went from 42 to 219 cells missing (ISO, 2026-11-06). ASSUMED size. */
#define MS_OTHER (768u << 10)
#define MS_GRAN 256u
#define MS_GPP (MS_PAGE / MS_GRAN)  /* granules per page */
#define MS_RUN (256u << 10)          /* bytes of neighbouring resident items per request (v1 step 5) */
#define MS_INFLIGHT 8
#define MS_QUEUE 64
#define MS_BLOCKED 16
/* pages a read may clear per frame (each clearing ranks every loaded item) */
#define MS_EVICT_PASSES 2
#define MS_FAIL_FRAMES 90u
#define MS_FAIL_FRAMES_MAX (16 * MS_FAIL_FRAMES)

enum { MS_NONE, MS_READING, MS_READY, MS_FAILED };
/* classes, lowest first (the header comment lists them). Coarse cells in view within MS_R1_VIEW rank above near
 * parts: a missing near part draws its cell's coarse level, a missing coarse cell is a hole (with near parts first, the
 * route's worst frame had 211 cells missing instead of 42, ISO 2026-11-06) */
#if MS_NEAR_FIRST
enum { C_FREE, C_KEEP, C_PREF, C_FARC, C_VIEW2, C_NEAR2, C_PIN, C_NEAR, C_VIEW, C_PIN0 };
#else
enum { C_FREE, C_KEEP, C_PREF, C_FARC, C_VIEW2, C_NEAR2, C_NEAR, C_VIEW, C_PIN, C_PIN0 = C_PIN };
#endif
#define C_TOP C_PIN0

MapStreamStats mapstream_stats;

typedef struct {
    Pack pk;
    int borrowed;
    unsigned base, total;  /* cells.bin in the pack */
    const MapCell *tab;
    unsigned ncell;
    MapResItem *rit;
    unsigned nres;
    /* per item (ids [0, ncell) near entries, [ncell, ncell + nres) resident items) */
    unsigned char *state, *cls;
    void **buf;
    unsigned *used;        /* frame map.c last asked for it */
    unsigned short *rgrp;  /* resident item: first item (index into rit) of the group it was read with */
    /* visibility (pvs.c): per item 1 while any of its chunks is seen, the tick (frame / 4) it was last seen; the
     * vis_gen value they follow, items they cover */
    unsigned char *visv;
    unsigned short *vist;
    unsigned visg, visn, vish, viskb;  /* + items outside it now, their KB */
} MsInst;
static MsInst ms[MS_INST];
static int sel;  /* the instance mapstream_get / mapstream_res answer for */
static unsigned frame;
/* the frames of the last two map draws (mapstream_get / mapstream_res calls). An item handed out in either may be read
 * by a VIF chain that is built or still running: it is never freed (a warp settle advances `frame` many times without
 * a draw, and #110 freed cells of the last drawn frame's chain there: hang after a spawn, 2026-11-06) */
static unsigned dcur, dprev;
/* (an item that landed after the last draw has used > dcur: no chain has it. Counting those made every item read in
 * a settle undroppable: the near level filled the arena and the next block's lite set found no window, settle at its
 * 30 s cap, ISO 2026-11-06) */
/* mapstream_alloc after vu1_wait: no chain reads the arena (map.c builds the map chain in map_draw and main.c kicks it
 * right after; no allocation runs in between) */
static int chains_done;
static inline int in_chain(unsigned used)
{
    if (chains_done)
        return 0;
    return dprev && (int)(used - dprev) >= 0 && (int)(used - dcur) <= 0;
}
static inline void drawn(void)
{
    if (frame != dcur)
        dprev = dcur, dcur = frame;
}

static inline unsigned it_n(const MsInst *m) { return m->ncell + m->nres; }
static inline int it_res(const MsInst *m, unsigned id) { return id >= m->ncell; }
static inline unsigned it_bytes(const MsInst *m, unsigned id)
{
    return id < m->ncell ? m->tab[id].bytes : m->rit[id - m->ncell].bytes;
}
static inline unsigned it_off(const MsInst *m, unsigned id)
{
    return id < m->ncell ? m->base + m->tab[id].off : m->rit[id - m->ncell].off;
}
static inline int it_tex(const MsInst *m, unsigned id) { return id >= m->ncell && (m->rit[id - m->ncell].tag & MS_RES_TEX); }

/* the items [*first, return) that share id's memory (one read group) */
static unsigned grp_of(const MsInst *m, unsigned id, unsigned *first)
{
    if (!it_res(m, id)) {
        *first = id;
        return id + 1;
    }
    unsigned j0 = m->rgrp[id - m->ncell], e = j0 + 1;
    while (e < m->nres && m->rgrp[e] == j0)
        e++;
    *first = m->ncell + j0;
    return m->ncell + e;
}
static unsigned grp_bytes(const MsInst *m, unsigned a, unsigned e)
{
    if (!it_res(m, a))
        return it_bytes(m, a);
    const MapResItem *r = m->rit;
    return r[e - 1 - m->ncell].rel + r[e - 1 - m->ncell].bytes - r[a - m->ncell].rel;
}

/* ---- arena: granule bitmap on a page grid ---- */
static unsigned char *arena;
static unsigned arena_size, npages, ipages, *gmap;  /* ipages: pages map items may use (below MS_OTHER) */  /* bit per granule, 1 = used */
static unsigned short *pfree;               /* free granules per page */
static unsigned reserve_extra, pool_max = MS_POOL;
/* make_window's scores per page: bits 0-7 highest class + 1 of its clearable groups, 0x200 not clearable; cost;
 * granules of clearable groups (a page whose used granules are not all clearable groups holds a read in flight or a
 * mapstream_alloc block) */
static unsigned *pcls, *pcost, *pitem;

static inline int g_used(unsigned g) { return gmap[g >> 5] >> (g & 31) & 1; }
static void g_mark(unsigned g0, unsigned n, int on)
{
    for (unsigned g = g0; g < g0 + n; g++) {
        unsigned b = 1u << (g & 31);
        if (on)
            gmap[g >> 5] |= b;
        else
            gmap[g >> 5] &= ~b;
        pfree[g / MS_GPP] += on ? -1 : 1;
    }
}
/* n bytes (<= a page) inside one page: pages already in use first, so free pages stay whole */
static void *g_alloc(unsigned n)
{
    unsigned ng = (n + MS_GRAN - 1) / MS_GRAN;
    if (!ng || ng > MS_GPP)
        return NULL;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned p = 0; p < ipages; p++) {
            if (pfree[p] < ng || (pass == 0) == (pfree[p] == MS_GPP))
                continue;
            unsigned run = 0, g0 = p * MS_GPP;
            for (unsigned g = g0; g < g0 + MS_GPP; g++) {
                run = g_used(g) ? 0 : run + 1;
                if (run == ng) {
                    g_mark(g + 1 - ng, ng, 1);
                    return arena + (size_t)(g + 1 - ng) * MS_GRAN;
                }
            }
        }
    return NULL;
}
/* n bytes in a run of whole free pages below page `top`, the highest such run, placed at its top */
static void *g_alloc_top(unsigned n, unsigned top)
{
    unsigned np = (n + MS_PAGE - 1) / MS_PAGE, ng = (n + MS_GRAN - 1) / MS_GRAN, run = 0;
    for (unsigned p = top; p-- > 0;) {
        run = pfree[p] == MS_GPP ? run + 1 : 0;
        if (run == np) {
            unsigned end = (p + np) * MS_GPP;
            g_mark(end - ng, ng, 1);
            return arena + (size_t)(end - ng) * MS_GRAN;
        }
    }
    return NULL;
}
static void g_free(void *ptr, unsigned n)
{
    unsigned g0 = (unsigned)((unsigned char *)ptr - arena) / MS_GRAN;
    g_mark(g0, (n + MS_GRAN - 1) / MS_GRAN, 0);
}

static unsigned heap_free(void)
{
    struct mallinfo mi = mallinfo();
    return 32 * 1024 * 1024 - (unsigned)sbrk(0) + mi.fordblks;
}

static inline unsigned ticks(void)
{
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

static int ensure_arena(void)
{
    if (arena)
        return 1;
    unsigned hf = heap_free(), rs = MS_RESERVE + reserve_extra;
    unsigned sz = hf > rs + (1u << 20) ? hf - rs : 0;
    sz = (sz < pool_max ? sz : pool_max) & ~(MS_PAGE - 1);
    arena = sz ? memalign(MS_PAGE, sz) : NULL;
    if (arena) {
        npages = sz / MS_PAGE;
        gmap = calloc((npages * MS_GPP + 31) / 32, 4);
        pfree = malloc(npages * sizeof *pfree);
        pcls = malloc(npages * sizeof *pcls), pcost = malloc(npages * sizeof *pcost);
        pitem = malloc(npages * sizeof *pitem);
        if (!gmap || !pfree || !pcls || !pcost || !pitem) {
            free(arena), free(gmap), free(pfree), free(pcls), free(pcost), free(pitem);
            pcls = pcost = pitem = NULL;
            arena = NULL;
        } else {
            for (unsigned p = 0; p < npages; p++)
                pfree[p] = MS_GPP;
            arena_size = sz;
            ipages = npages > 2 * (MS_OTHER / MS_PAGE) ? npages - MS_OTHER / MS_PAGE : npages / 2;
        }
    }
    printf("mapstream: v2 arena %u KB in %u pages (heap free %u KB)\n", arena ? arena_size >> 10 : 0, npages, hf >> 10);
    return arena != NULL;
}

/* ---- items ---- */
static float box_d2(const MapCell *c, const float *p)
{
    float d2 = 0;
    for (int k = 0; k < 3; k++) {
        float o = p[k] < c->mn[k] ? c->mn[k] - p[k] : p[k] > c->mx[k] ? p[k] - c->mx[k] : 0;
        d2 += o * o;
    }
    return d2;
}

static int focus = -1, settling, hold;
static float ahead[3], vel[3], last_pos[3];  /* the walking look-ahead point (MS_AHEAD_FRAMES) */
static int ahead_on;
static float settle_r, settle_rf;
static unsigned (*tex_age)(int k, unsigned tag);
static void (*tex_set)(int k, unsigned tag, const void *p);

/* ---- visibility (src/pvs.c via map.c, 2026-11-06) ----
 * Cells are a spatial index: a cell 20 m away behind a wall still holds parts no camera nearby can see. The vis hook
 * says whether any chunk of an item is in the precomputed visibility of a camera cell within PVS_STREAM_R; an item
 * outside it for MS_VIS_HOLD ticks is planned no higher than C_KEEP: not read, not pinned, kept while there is room
 * (a turn back finds it in). The camera cell changes smoothly; the draw-group cut tried first (floor part, which
 * flips every 1-7 s at the Burg) churned. The design follows Vice City's streamer (re3 CStreaming: entities of
 * another area are neither requested nor kept). `make PVS=0` (-DNO_PVS): every item is planned by distance only. */
#define MS_VIS_HOLD 15u  /* ticks of 4 frames: 2 s, ASSUMED (a look through a doorway and back) */
static int (*vis_hook)(int k, unsigned id);
static unsigned (*vis_gen)(void);
static unsigned vis_last;

static void vis_refresh(int k)
{
    MsInst *m = &ms[k];
    unsigned n = it_n(m), g = vis_gen ? vis_gen() : 0;
    if (!vis_hook || !n)
        return;
    int fresh = m->visn != n;  /* an item unseen at its first look was never seen: no hold */
    if (fresh) {
        unsigned char *v = realloc(m->visv, n);
        unsigned short *t = v ? (m->visv = v, realloc(m->vist, n * sizeof *t)) : NULL;
        if (!t) {
            free(m->visv), m->visv = NULL, m->visn = 0;
            return;
        }
        m->vist = t;
        memset(m->visv, 1, n);
        memset(m->vist, 0, n * sizeof *t);
        m->visn = n, m->visg = g - 1;
    }
    if (m->visg == g)
        return;
    m->visg = g;
    unsigned short tick = (unsigned short)(frame >> 2);
    unsigned nh = 0, kb = 0;
    for (unsigned id = 0; id < n; id++) {
        int v = it_bytes(m, id) ? vis_hook(k, id) : 1;
        if (!v && m->visv[id])
            m->vist[id] = fresh ? (unsigned short)(tick - MS_VIS_HOLD - 1) : tick;  /* seen until now */
        m->visv[id] = (unsigned char)!!v;
        if (!v)
            nh++, kb += it_bytes(m, id);
    }
    mapstream_stats.vis_items += nh - m->vish, mapstream_stats.vis_kb += (kb >> 10) - m->viskb;
    m->vish = nh, m->viskb = kb >> 10;
    printf("mapstream: %d: pvs leaves out %u of %u items, %u KB\n", k, nh, n, kb >> 10);  /* A/B logs */
}
/* outside the visibility now */
static inline int vis_off(const MsInst *m, unsigned id) { return m->visv && id < m->visn && !m->visv[id]; }
/* out of the plan: outside the visibility now and for MS_VIS_HOLD ticks */
static inline int vis_cut(const MsInst *m, unsigned id)
{
    return vis_off(m, id) && (unsigned short)((unsigned short)(frame >> 2) - m->vist[id]) > MS_VIS_HOLD;
}

void mapstream_vis_hooks(int (*vis)(int k, unsigned id), unsigned (*gen)(void))
{
#ifdef NO_PVS
    (void)vis, (void)gen;
#else
    vis_hook = vis, vis_gen = gen;
    vis_last = gen ? gen() - 1 : 0;
#endif
}

/* every near part of cell c is loaded (or outside the visibility: not read) */
static int near_in(const MsInst *m, unsigned c)
{
    if (!(m->tab[c].near & ~CELL_PART) || (m->tab[c].near & CELL_PART))
        return 0;
    for (unsigned p = c; p < m->ncell && (p == c || (m->tab[p].near & CELL_PART)); p++)
        if (m->state[p] != MS_READY && !vis_off(m, p))  /* such a part is not read (pvs.c) */
            return 0;
    return 1;
}

static inline float sq(float x) { return x * x; }

/* class of item id of instance k with the player at pos; *key ranks it within the class (lower first) */
static int plan_item(int k, const MsInst *m, unsigned id, const float *pos, float *key)
{
    int view = frame - m->used[id] <= MS_VIEW_FRAMES;
    float vk = view ? MS_VIEW_K : 1.0f;
    if (id < m->ncell) {
        if (!m->tab[id].bytes)
            return C_FREE;
        float d2 = box_d2(&m->tab[id], pos);
        *key = d2 * vk;
        /* a settle reads the near level within the draw radius (MS_SETTLE_NEAR) after the coarse level it waits for (as
         * C_NEAR it took the plan from coarse cells inside the settle radius, which the settle then waited for until its
         * 30 s cap: Firelink's near level within 26 m is 4.1 MB, ISO 2026-11-06); the rest streams in */
        if (settling)
            return MS_SETTLE_NEAR && d2 < sq(MS_R0_DRAW) ? C_NEAR2 : d2 < sq(MS_R0_KEEP) ? C_KEEP : C_FREE;
#if MS_NEAR_FIRST
        if (ahead_on) {
            float da = box_d2(&m->tab[id], ahead);
            if (da < d2)
                *key = da * vk;
            if (d2 >= sq(MS_R0_KEEP))
                return da < sq(MS_R0_WANT) ? C_PREF : C_FREE;
            d2 = da < d2 ? da : d2;
        }
#endif
        return d2 < sq(MS_R0_FIRST) ? C_NEAR : d2 < sq(MS_R0_DRAW) ? C_NEAR2 : d2 < sq(MS_R0_WANT) ? C_PREF : d2 < sq(MS_R0_KEEP) ? C_KEEP : C_FREE;
    }
    unsigned tag = m->rit[id - m->ncell].tag;
    if (tag & MS_RES_TEX) {
        unsigned age = tex_age ? tex_age(k, tag & ~MS_RES_TEX) : ~0u;
        *key = 0;
        return age <= 1 ? C_PREF : age <= MS_TEX_KEEP ? C_KEEP : C_FREE;
    }
    if (tag & MS_RES_SKY) {
        *key = 0;
        return k == (focus < 0 ? 0 : focus) ? C_PIN0 : C_KEEP;
    }
    unsigned c = MS_RES_CELL(tag);
    if (c >= m->ncell)
        return C_FREE;
    float d2 = box_d2(&m->tab[c], pos);
    if (tag & MS_RES_FAR) {  /* the backdrop: the horizon of the player's block; another block's while in view */
        *key = (d2 < sq(MS_FAR_D) ? d2 : sq(MS_FAR_D)) * vk;
        return k == (focus < 0 ? 0 : focus) ? C_PIN0 : view ? C_VIEW2 : C_FARC;
    }
    *key = d2 * vk;
    if (tag & MS_RES_GRASS)
        return settling ? C_FREE : d2 < sq(MS_GRASS_WANT) ? C_NEAR2 : d2 < sq(MS_GRASS_KEEP) ? C_KEEP : C_FREE;
    /* no "one level of detail per place" here: a cell's coarse items stay while its near level is drawn. Dropping them
     * once the near parts were in made a hole whenever a near part went next (the near level draws only with every
     * part in): the route's worst frame had ~200 cells missing in the Burg (ISO, 2026-11-06). The coarse level within
     * MS_R1_PIN is in the export budget anyway. */
#if MS_NEAR_FIRST
    /* in view out to MS_R1_VIEW: C_VIEW above the near parts (a missing coarse cell in view is a hole; with C_VIEW
     * below C_NEAR the route had 227 s with over 16 cells missing instead of 144, ISO 2026-11-06); pinned but out of
     * view: C_PIN below them */
    if (d2 >= sq(MS_R1_FIRST) && d2 < sq(MS_R1_VIEW) && view)
        return C_VIEW;
#endif
    return d2 < sq(MS_R1_FIRST) ? C_PIN0 : d2 < sq(MS_R1_PIN) ? C_PIN : d2 < sq(MS_R1_WANT) ? (!view ? C_FARC : d2 < sq(MS_R1_VIEW) ? C_VIEW : C_VIEW2)
                                                           : d2 < sq(MS_R1_KEEP) ? C_KEEP : C_FREE;
}

/* free the group of item id */
static void drop(int k, unsigned id)
{
    MsInst *m = &ms[k];
    unsigned a, e = grp_of(m, id, &a);
    g_free(m->buf[a], grp_bytes(m, a, e));
    for (unsigned j = a; j < e; j++) {
        if (it_tex(m, j) && tex_set)
            tex_set(k, m->rit[j - m->ncell].tag & ~MS_RES_TEX, NULL);
        unsigned b = it_bytes(m, j);
        m->buf[j] = NULL;
        m->state[j] = MS_NONE;
        mapstream_stats.cells--;
        mapstream_stats.bytes -= b;
        if (it_res(m, j)) {
            *(it_tex(m, j) ? &mapstream_stats.tex_kb : &mapstream_stats.res_kb) -= (b + 1023) >> 10;
            m->rgrp[j - m->ncell] = (unsigned short)(j - m->ncell);
        }
    }
}

/* the highest class over id's group (dropping it drops them all) */
static int grp_cls(const MsInst *m, unsigned id)
{
    unsigned a, e = grp_of(m, id, &a);
    int c = C_FREE;
    for (unsigned j = a; j < e; j++)
        c = m->cls[j] > c ? m->cls[j] : c;
    return c;
}
/* any item of id's group handed out in the last two map draws */
static int grp_chain(const MsInst *m, unsigned id)
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
        u = frame - m->used[j] < frame - u ? m->used[j] : u;
    return u;
}

/* ---- making room ----
 * Clear `need` whole pages (a window; need = 1 for a read group): every group in the window must be loaded, of a class
 * below `below`, and not handed out in the last two map draws (in_chain). Cost per page: (highest class in it, then
 * the bytes and recency of its groups); the cheapest window from the top goes. 1 when cleared. */
static int nowin = C_TOP + 2;  /* a window of 1 page was not found for this class or above since the last change */
static int make_window(unsigned need, unsigned ng_need, int below, unsigned *kb)
{
    unsigned lim = ng_need ? ipages : npages;  /* a read's window lies where items may go */
    if (!need || need > npages || !pcls || (need == 1 && below >= nowin))
        return 0;
    memset(pcls, 0, npages * sizeof *pcls);
    memset(pcost, 0, npages * sizeof *pcost);
    memset(pitem, 0, npages * sizeof *pitem);
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                continue;
            int c = grp_cls(m, id);
            unsigned u = grp_used(m, id), n = grp_bytes(m, a, e);
            unsigned g0 = (unsigned)((unsigned char *)m->buf[a] - arena) / MS_GRAN, ng = (n + MS_GRAN - 1) / MS_GRAN;
            int ok = c < below && !grp_chain(m, id);
            unsigned age = frame - u > 1000 ? 1000 : frame - u;  /* cost: bytes, older groups cheaper */
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
    for (unsigned p = 0; p < npages; p++) {
        if (pitem[p] != MS_GPP - pfree[p])
            pcls[p] |= 0x200;
        if (need == 1 && (pcls[p] & 0x200) && pitem[p] && pfree[p] + pitem[p] >= ng_need)
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
    if (best < 0) {
        if (need == 1 && below < nowin)
            nowin = below;
        return 0;
    }
    unsigned w0 = best * MS_PAGE, w1 = (best + need) * MS_PAGE;
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                continue;
            unsigned o = (unsigned)((unsigned char *)m->buf[a] - arena);
            if (o < w1 && o + grp_bytes(m, a, e) > w0 && grp_cls(m, id) < below && !grp_chain(m, id)) {
                if (kb)
                    *kb += grp_bytes(m, a, e) >> 10;
                drop(k, id);
            }
        }
    }
    return 1;
}

/* ---- reads ---- */
typedef struct { int k; unsigned id, n, bytes, t; int tk; } MsRead;
static MsRead rd[MS_INFLIGHT];
static int nrd;
static unsigned land_t;
typedef struct { int k; unsigned id; int cls; float key; } MsQ;
static MsQ q[MS_QUEUE];
static int nq;
static MsQ blk[MS_BLOCKED];
static int nblk;
static unsigned scan_frame, fail_frame, fail_wait;
static float scan_pos[3];
static int room_fail, failed, evict_left, replan;
/* load-time A/B (2026-11-06): a clock in ms over mapstream_update calls (COP0 Count wraps every 14.6 s), the settle
 * that ended last (its start / end on that clock), the first near cell and the full near level within MS_R0_FIRST of
 * the feet after it; KB of reads that landed with no class wanting them, KB of reads cancelled (MS_CANCEL) */
static unsigned clk_ms, clk_last, clk_frac, st_t0, st_t1, st_first, st_wait, stale_kb, cancel_kb;

static int q_before(const MsQ *a, int cls, float key) { return a->cls > cls || (a->cls == cls && a->key <= key); }
static void q_add(int k, unsigned id, int cls, float key)
{
    int i = nq < MS_QUEUE ? nq++ : MS_QUEUE;
    if (i == MS_QUEUE) {
        if (q_before(&q[MS_QUEUE - 1], cls, key))
            return;
        i = MS_QUEUE - 1;
    }
    while (i > 0 && !q_before(&q[i - 1], cls, key))
        q[i] = q[i - 1], i--;
    q[i] = (MsQ){k, id, cls, key};
}
static int blocked(int k, unsigned id)
{
    for (int i = 0; i < nblk; i++)
        if (blk[i].k == k && blk[i].id == id)
            return 1;
    return 0;
}

static void collect(int i, int r)
{
    MsRead w = rd[i];
    memmove(rd + i, rd + i + 1, (--nrd - i) * sizeof *rd);
    mapstream_stats.pending = nrd;
    MsInst *m = &ms[w.k];
    unsigned now = ticks(), t0 = (int)(w.t - land_t) > 0 ? w.t : land_t;
    land_t = now;
    if (r == (int)w.bytes) {
        InvalidDCache(m->buf[w.id], (char *)m->buf[w.id] + w.bytes - 1);
        int want = 0;
        for (unsigned j = w.id; j < w.id + w.n; j++)
            want |= m->cls[j] >= C_PREF;
        if (!want)
            stale_kb += w.bytes >> 10;
        if (st_wait && !st_first && !it_res(m, w.id))
            st_first = clk_ms - st_t1 + 1;
        fail_wait = 0;
        mapstream_stats.ms_last = (now - t0) / 294912;
        for (unsigned j = w.id; j < w.id + w.n; j++) {
            m->state[j] = MS_READY;
            m->used[j] = frame;
            mapstream_stats.loads++;
            if (it_tex(m, j) && tex_set)
                tex_set(w.k, m->rit[j - m->ncell].tag & ~MS_RES_TEX, m->buf[j]);
        }
        if (!it_tex(m, w.id))
            printf("mapstream: %s %d.%u (%u items) %u KB in %u ms, %u items %u KB loaded (resident %u KB, mip 0 %u KB)\n",
                   it_res(m, w.id) ? "resident cell" : "cell", w.k, it_res(m, w.id) ? w.id - m->ncell : w.id, w.n,
                   w.bytes >> 10, mapstream_stats.ms_last, mapstream_stats.cells, mapstream_stats.bytes >> 10,
                   mapstream_stats.res_kb, mapstream_stats.tex_kb);
    } else {
        printf("mapstream: item %d.%u (%u items) read %d of %u bytes\n", w.k, w.id, w.n, r, w.bytes);
        drop(w.k, w.id);
        for (unsigned j = w.id; j < w.id + w.n; j++)
            m->state[j] = MS_FAILED;
        mapstream_stats.fails++;
        failed = 1, fail_frame = frame;
        fail_wait = !fail_wait ? MS_FAIL_FRAMES : fail_wait < MS_FAIL_FRAMES_MAX ? 2 * fail_wait : fail_wait;
    }
}

/* the queue's head into the arena and read (its missing wanted neighbours in mapres.bin along, up to MS_RUN bytes, as
 * groups of at most a page); 0 when nothing more goes out this frame */
static int issue(void)
{
    MsQ w = q[0];
    memmove(q, q + 1, --nq * sizeof *q);
    MsInst *m = &ms[w.k];
    if (w.id >= it_n(m) || m->state[w.id] != MS_NONE)
        return 1;
    unsigned wn = 1;
    if (it_res(m, w.id)) {
        unsigned j = w.id - m->ncell, end = m->rit[j].rel + m->rit[j].bytes, tex = !!(m->rit[j].tag & MS_RES_TEX);
        while (j + wn < m->nres) {
            const MapResItem *c = &m->rit[j + wn];
            unsigned id = m->ncell + j + wn;
            if (m->state[id] != MS_NONE || !c->bytes || c->rel < end || c->rel - end > 64 ||
                c->off - c->rel != m->rit[j].off - m->rit[j].rel || c->rel + c->bytes - m->rit[j].rel > MS_RUN ||
                !!(c->tag & MS_RES_TEX) != tex || m->cls[id] < C_PREF || blocked(w.k, id))
                break;
            end = c->rel + c->bytes, wn++;
        }
    }
    int sent = 0;
    for (unsigned g = w.id; g < w.id + wn && nrd < MS_INFLIGHT;) {
        unsigned e = g + 1, gb = it_bytes(m, g);
        if (it_res(m, g)) {
            unsigned r0 = m->rit[g - m->ncell].rel;
            /* coarse items in groups of their own (they go first once the near level is in), except in a settle,
             * where split groups took 2.5x the reads (v1, ISO boot 6.0 -> 7.8 s) */
            while (e < w.id + wn && m->rit[e - m->ncell].rel + m->rit[e - m->ncell].bytes - r0 <= MS_PAGE &&
                   (settling || !(m->rit[e - m->ncell].tag & MS_RES_COARSE) == !(m->rit[g - m->ncell].tag & MS_RES_COARSE)))
                e++;
            gb = grp_bytes(m, g, e);
        }
        void *p = gb <= MS_PAGE ? g_alloc(gb) : g_alloc_top(gb, ipages);
        if (!p && sent)
            break;  /* the rest waits for its turn */
        if (!p && evict_left > 0) {
            evict_left--;
            /* one page (or the pages of a group over a page) of items below this one's class, not drawn this frame
             * or the last */
            if (make_window((gb + MS_PAGE - 1) / MS_PAGE, (gb + MS_GRAN - 1) / MS_GRAN, w.cls, NULL))
                p = gb <= MS_PAGE ? g_alloc(gb) : g_alloc_top(gb, ipages);
        }
        if (!p) {
            if (evict_left <= 0 && !sent && nq < MS_QUEUE) {  /* no clearing left this frame: first again next frame */
                memmove(q + 1, q, nq++ * sizeof *q);
                q[0] = w;
                return 0;
            }
            if (!room_fail++) {
                unsigned fr = 0;
                for (unsigned i = 0; i < npages; i++)
                    fr += pfree[i];
                printf("mapstream: no room for item %d.%u (%u KB, class %d, %.0f m): nothing of a lower class to drop; "
                       "%u KB free, items %u KB (resident %u, mip 0 %u), other %u KB\n", w.k, w.id, gb >> 10, w.cls,
                       sqrtf(w.key), fr * MS_GRAN >> 10, mapstream_stats.bytes >> 10, mapstream_stats.res_kb,
                       mapstream_stats.tex_kb, mapstream_stats.other_kb);
            }
            mapstream_stats.room_fails++;
            if (nblk == MS_BLOCKED)
                memmove(blk, blk + 1, --nblk * sizeof *blk);
            blk[nblk++] = (MsQ){w.k, w.id, 0, (float)frame};
            return 1;  /* the next queued one may still fit */
        }
        room_fail = 0;
        int tk = pack_req_read(&m->pk, it_off(m, g), p, gb);
        if (!tk) {
            g_free(p, gb);
            if (!sent)
                q_add(w.k, w.id, w.cls, w.key);
            return 0;
        }
        for (unsigned j = g; j < e; j++) {
            unsigned b = it_bytes(m, j);
            m->buf[j] = (unsigned char *)p + (it_res(m, j) ? m->rit[j - m->ncell].rel - m->rit[g - m->ncell].rel : 0);
            m->state[j] = MS_READING;
            if (it_res(m, j)) {
                m->rgrp[j - m->ncell] = (unsigned short)(g - m->ncell);
                *(it_tex(m, j) ? &mapstream_stats.tex_kb : &mapstream_stats.res_kb) += (b + 1023) >> 10;
            }
            mapstream_stats.cells++;
            mapstream_stats.bytes += b;
        }
        if (!nrd)
            land_t = ticks();
        rd[nrd++] = (MsRead){w.k, g, e - g, gb, ticks(), tk};
        mapstream_stats.pending = nrd;
        sent++;
        g = e;
    }
    return 1;
}

/* ---- plan ---- */
/* the plan's wanted items, sorted by rank; MS_FILL_PCT: the share (%) of the item pages the plan may fill (granule rounding,
 * read groups not filling their page, ASSUMED) */
#define MS_WANTED 4096  /* wanted items of all blocks at once (~2000 seen at the Burg entry); 32 KB */
#define MS_FILL_PCT 92u
typedef struct { unsigned char k, cls; unsigned short id; float key; } MsW;
static MsW want[MS_WANTED];
static int w_cmp(const void *a, const void *b)
{
    const MsW *x = a, *y = b;
    return x->cls != y->cls ? y->cls - x->cls : x->key < y->key ? -1 : x->key > y->key;
}
static void plan(const float *pos)
{
    nq = 0;
    nowin = C_TOP + 2;
    for (int i = 0; i < nblk; i++)
        if (frame - (unsigned)blk[i].key >= MS_BLOCK_FRAMES)
            blk[i--] = blk[--nblk];
    int retry = failed && frame - fail_frame >= fail_wait;
    failed &= !retry;
    /* classes, then the cut: wanted items in rank order (class, then key) until their bytes fill the item pages; the
     * rest are only kept (never read, first to go). Without the cut the pinned coarse cells of Firelink + three Burg
     * blocks (7 MB within 96 m, 1.9 MB of backdrop) outgrew the arena: no near cell could find room and the Burg
     * stayed at its coarse level, with cells going missing (user report, 2026-11-06) */
    unsigned nw = 0;
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms[k];
        vis_refresh(k);
        for (unsigned id = 0; id < it_n(m); id++) {
            float key = 0;
            int c = it_bytes(m, id) ? plan_item(k, m, id, pos, &key) : C_FREE;
            if (c > C_KEEP && vis_cut(m, id))
                c = C_KEEP;  /* no camera cell near the camera sees it (pvs.c) */
            m->cls[id] = (unsigned char)c;
            if (m->state[id] == MS_FAILED && retry)
                m->state[id] = MS_NONE;
            if (c >= C_PREF && nw < MS_WANTED)
                want[nw++] = (MsW){(unsigned char)k, (unsigned char)c, (unsigned short)id, key};
        }
    }
    qsort(want, nw, sizeof *want, w_cmp);
    /* what mapstream_alloc holds beyond its MS_OTHER top lies in item pages: the plan leaves it out */
    unsigned other = mapstream_stats.other_kb << 10, over = other > MS_OTHER ? other - MS_OTHER : 0;
    unsigned cap = ipages * MS_PAGE > over ? (ipages * MS_PAGE - over) / 100 * MS_FILL_PCT : 0, sum = 0;
    for (unsigned i = 0; i < nw; i++) {
        MsInst *m = &ms[want[i].k];
        sum += (it_bytes(m, want[i].id) + MS_GRAN - 1) & ~(MS_GRAN - 1);
        if (sum > cap) {
            m->cls[want[i].id] = C_KEEP;
            continue;
        }
        if (m->state[want[i].id] == MS_NONE && !blocked(want[i].k, want[i].id))
            q_add(want[i].k, want[i].id, want[i].cls, want[i].key);
    }
#if MS_CANCEL
    for (int i = 0; i < nrd; i++) {  /* reads in flight that nothing wants any more */
        MsInst *m = &ms[rd[i].k];
        int want = 0;
        for (unsigned j = rd[i].id; j < rd[i].id + rd[i].n; j++)
            want |= m->cls[j] >= C_PREF;
        if (want)
            continue;
        pack_req_cancel(rd[i].tk);  /* returns once nothing more lands in the buffer */
        cancel_kb += rd[i].bytes >> 10;
        drop(rd[i].k, rd[i].id);
        memmove(rd + i, rd + i + 1, (--nrd - i) * sizeof *rd);
        i--;
    }
    mapstream_stats.pending = nrd;
#endif
    {  /* what the plan wants per class (KB) against the room, every ~10 s of plans (tuning the classes) */
        static unsigned np;
        if (!(np++ % 40)) {
            unsigned kb[C_TOP + 1] = {0};
            for (unsigned i = 0; i < nw; i++)
                kb[want[i].cls] += it_bytes(&ms[want[i].k], want[i].id) >> 10;
            printf("mapstream: plan %u KB room: pin %u view %u near %u near2 %u view2 %u farc %u pref %u KB; stale reads "
                   "landed %u KB, cancelled %u KB\n", cap >> 10, kb[C_PIN] + (C_PIN0 != C_PIN ? kb[C_PIN0] : 0),
                   kb[C_VIEW], kb[C_NEAR], kb[C_NEAR2], kb[C_VIEW2], kb[C_FARC], kb[C_PREF], stale_kb, cancel_kb);
        }
    }
    if (st_wait) {  /* after a settle: is the near level within MS_R0_FIRST of the feet in? */
        unsigned left = 0;
        for (int k = 0; k < MS_INST; k++) {
            const MsInst *m = &ms[k];
            for (unsigned c = 0; c < m->ncell; c++)
                if ((m->tab[c].near & ~CELL_PART) && !(m->tab[c].near & CELL_PART) && m->tab[c].bytes &&
                    box_d2(&m->tab[c], pos) < sq(MS_R0_FIRST) && !vis_cut(m, c) && !near_in(m, c))
                    left++;
        }
        unsigned t = clk_ms - st_t1;
        if (!left || t > 60000) {
            printf("mapstream: after the settle (%u ms): first near cell %d ms, near level within %.0f m %s %u ms "
                   "(%u cells left)\n", st_t1 - st_t0, st_first ? (int)st_first - 1 : -1, (double)MS_R0_FIRST,
                   left ? "still missing after" : "in at", t, left);
            st_wait = 0;
        }
    }
    /* free what no class wants and no VIF chain reads (not asked for this frame or the last) */
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a;
            if (m->state[id] == MS_READY && (grp_of(m, id, &a), a == id) && grp_cls(m, id) == C_FREE &&
                !grp_chain(m, id))
                drop(k, id);
        }
    }
}

void mapstream_update(const float pos[3])
{
    int any = 0;
    for (int k = 0; k < MS_INST; k++)
        any |= ms[k].ncell != 0;
    if (!any)
        return;
    if (!ensure_arena()) {  /* no near level: the coarse one draws everywhere */
        for (int k = 0; k < MS_INST; k++)
            mapstream_close(k);
        return;
    }
    frame++;
    {
        unsigned now = ticks(), dt = now - clk_last + clk_frac;  /* 294912 Count ticks per ms */
        clk_last = now;
        if (frame > 1)
            clk_ms += dt / 294912, clk_frac = dt % 294912;
    }
    int r;
    for (int i = 0; i < nrd; i++)
        if (pack_req_done(rd[i].tk, &r))
            collect(i--, r);
    {  /* walking velocity per frame (smoothed); a warp / teleport starts it again */
        float d[3] = {pos[0] - last_pos[0], pos[1] - last_pos[1], pos[2] - last_pos[2]};
        int jump = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 4.0f || settling;
        for (int i = 0; i < 3; i++) {
            vel[i] = jump ? 0 : vel[i] + (d[i] - vel[i]) * 0.1f;
            ahead[i] = pos[i] + vel[i] * MS_AHEAD_FRAMES;
            last_pos[i] = pos[i];
        }
        ahead_on = !jump && vel[0] * vel[0] + vel[2] * vel[2] > sq(0.02f);  /* moving faster than 0.6 m/s */
    }
    if (vis_gen && vis_gen() != vis_last)  /* a row changed: what is read changes too (pvs.c) */
        vis_last = vis_gen(), replan = 1;
    float dx = pos[0] - scan_pos[0], dy = pos[1] - scan_pos[1], dz = pos[2] - scan_pos[2];
    if (replan || !scan_frame || frame - scan_frame >= MS_SCAN_FRAMES || dx * dx + dy * dy + dz * dz > sq(MS_SCAN_D)) {
        replan = 0;
        scan_frame = frame;
        memcpy(scan_pos, pos, sizeof scan_pos);
        plan(pos);
    }
    evict_left = MS_EVICT_PASSES;
    while (!hold && nq && nrd < MS_INFLIGHT && issue())
        ;
}

/* ---- opening and closing ---- */
int mapstream_init(const char *pak_path, const MapCell *cells, unsigned n)
{
    memset(&mapstream_stats, 0, sizeof mapstream_stats);
    return mapstream_open(0, pak_path, cells, n);
}

static int open_cells(int k, const MapCell *cells, unsigned n)
{
    MsInst *m = &ms[k];
    if (!pack_entry_raw(&m->pk, "cells.bin", &m->base, &m->total) || !m->total) {
        if (!m->borrowed)
            pack_close(&m->pk);
        return 0;
    }
    m->tab = cells, m->ncell = n;
    m->state = calloc(n, 1), m->cls = calloc(n, 1);
    m->buf = calloc(n, sizeof *m->buf), m->used = calloc(n, sizeof *m->used);
    m->rit = NULL, m->nres = 0, m->rgrp = NULL;
    if (!m->state || !m->cls || !m->buf || !m->used) {
        if (!m->borrowed)
            pack_close(&m->pk);
        free(m->state), free(m->cls), free(m->buf), free(m->used);
        memset(m, 0, sizeof *m);
        return 0;
    }
    replan = 1;
    printf("mapstream: %u cells, cells.bin %u KB at %u (instance %d)\n", n, m->total >> 10, m->base, k);
    return 1;
}

int mapstream_open(int k, const char *pak_path, const MapCell *cells, unsigned n)
{
    if (k < 0 || k >= MS_INST)
        return 0;
    mapstream_close(k);
    sel = k;
    ms[k].borrowed = 0;
    if (!n || !pack_open(&ms[k].pk, pak_path))
        return 0;
    return open_cells(k, cells, n);
}

int mapstream_open_pack(int k, const void *pack, const MapCell *cells, unsigned n)
{
    if (k < 0 || k >= MS_INST || !n)
        return 0;
    mapstream_close(k);
    sel = k;
    ms[k].pk = *(const Pack *)pack;
    ms[k].borrowed = 1;
    return open_cells(k, cells, n);
}

int mapstream_res_open(int k, const MapResItem *items, unsigned n, unsigned nitems)
{
    if (k < 0 || k >= MS_INST || !nitems)
        return 0;
    MsInst *m = &ms[k];
    if (!m->ncell || n != m->ncell || m->rit || m->ncell + nitems > 65535)  /* ids are u16 in the plan */
        return 0;
    unsigned t = m->ncell + nitems;
    unsigned char *s = realloc(m->state, t), *c = s ? (m->state = s, realloc(m->cls, t)) : NULL;
    void **b = c ? (m->cls = c, realloc(m->buf, t * sizeof *m->buf)) : NULL;
    unsigned *u = b ? (m->buf = b, realloc(m->used, t * sizeof *m->used)) : NULL;
    if (u)
        m->used = u;
    m->rit = u ? malloc(nitems * sizeof *m->rit) : NULL;
    m->rgrp = m->rit ? malloc(nitems * sizeof *m->rgrp) : NULL;
    if (!m->rgrp) {
        free(m->rit);
        m->rit = NULL;
        return 0;
    }
    memcpy(m->rit, items, nitems * sizeof *m->rit);
    for (unsigned j = 0; j < nitems; j++)
        m->rgrp[j] = (unsigned short)j;
    memset(m->state + m->ncell, MS_NONE, nitems);
    memset(m->cls + m->ncell, C_FREE, nitems);
    memset(m->buf + m->ncell, 0, nitems * sizeof *m->buf);
    memset(m->used + m->ncell, 0, nitems * sizeof *m->used);
    m->nres = nitems;
    replan = 1;
    unsigned cb = 0, tb = 0, nt = 0;
    for (unsigned j = 0; j < nitems; j++)
        *(items[j].tag & MS_RES_TEX ? &tb : &cb) += items[j].bytes, nt += !!(items[j].tag & MS_RES_TEX);
    printf("mapstream: instance %d resident level per cell: %u cells + sky in %u items %u KB, %u mip 0 textures %u KB\n",
           k, n, nitems - nt, cb >> 10, nt, tb >> 10);
    return 1;
}

void mapstream_close(int k)
{
    if (k < 0 || k >= MS_INST || !ms[k].ncell)
        return;
    MsInst *m = &ms[k];
    for (int i = 0; i < nrd; i++)  /* stop its reads (after their current page) before the buffers go back */
        if (rd[i].k == k) {
            pack_req_cancel(rd[i].tk);
            memmove(rd + i, rd + i + 1, (--nrd - i) * sizeof *rd);
            i--;
        }
    mapstream_stats.pending = nrd;
    for (unsigned id = 0; id < it_n(m); id++) {
        unsigned a;
        if ((m->state[id] == MS_READY || m->state[id] == MS_READING) && (grp_of(m, id, &a), a == id))
            drop(k, id);
    }
    if (!m->borrowed)
        pack_close(&m->pk);
    free(m->state), free(m->cls), free(m->buf), free(m->used), free(m->rit), free(m->rgrp);
    free(m->visv), free(m->vist);
    mapstream_stats.vis_items -= m->vish, mapstream_stats.vis_kb -= m->viskb;
    memset(m, 0, sizeof *m);
    nq = 0, replan = 1;
    for (int i = 0; i < nblk; i++)
        if (blk[i].k == k)
            blk[i--] = blk[--nblk];
    if (sel == k)
        sel = 0;
}

void mapstream_select(int k)
{
    if (k >= 0 && k < MS_INST)
        sel = k;
}

/* ---- the rest of the API ---- */
void mapstream_tex_hooks(unsigned (*age)(int k, unsigned tag), void (*set)(int k, unsigned tag, const void *p))
{
    tex_age = age, tex_set = set;
}
void mapstream_hold(int on) { hold = on; }
void mapstream_settle(int on)
{
    if (on && !settling)
        st_t0 = clk_ms, st_wait = 0;
    if (!on && settling)
        st_t1 = clk_ms, st_first = 0, st_wait = 1;
    replan |= on != settling;
    settling = on;
}
void mapstream_focus(int k)
{
    replan |= k != focus;
    focus = k;
}
void mapstream_pool(unsigned max) { pool_max = max; }
void mapstream_reserve(unsigned extra) { reserve_extra = extra; }
int mapstream_ready(void) { return arena != NULL; }
int mapstream_arena(void)
{
    return ensure_arena();
}
int mapstream_owns(const void *p)
{
    return arena && (const unsigned char *)p >= arena && (const unsigned char *)p < arena + arena_size;
}
/* v1 lent idle memory of the old full pool to the arena; nothing lends it any more (world.c, 2026-11-05) */
void mapstream_lend(void *p, unsigned n) { (void)p, (void)n; }
int mapstream_reclaim(void *p, unsigned n) { (void)p, (void)n; return 1; }

void *mapstream_alloc(unsigned n, const float pos[3])
{
    (void)pos;  /* ranks come from the last plan */
    if (!mapstream_arena())
        return NULL;
    void *p = g_alloc_top(n, npages);
    unsigned need = (n + MS_PAGE - 1) / MS_PAGE, kb = 0;
    /* clear a window: of items below the near class first, then of anything but the pinned level, last of anything
     * (a lite set holds the collision the player walks onto next); never of items a VIF chain may read (in_chain) */
    if (!p && (make_window(need, 0, C_NEAR2, &kb) || make_window(need, 0, C_PIN, &kb) || make_window(need, 0, C_TOP + 1, &kb)))
        p = g_alloc_top(n, npages);
    if (!p) {
        /* every window holds a drawn item: the lite set of the block the player walks onto next (983 KB of the
         * bonfire block's collision) found none for minutes and the player walked off the loaded floor (ISO
         * 2026-11-06). Once VU1 is idle no chain of an earlier frame reads the arena: clear the cheapest window, drawn
         * items too (they read back; a frame or two of coarse level or hole beats no floor) */
        vu1_wait();
        chains_done = 1;
        nowin = C_TOP + 2;
        if (make_window(need, 0, C_TOP + 1, &kb))
            p = g_alloc_top(n, npages);
        if (!p && nrd) {
            /* still none: the pages reads in flight land in cannot be cleared, and up to MS_INFLIGHT small reads are
             * spread over the arena (indexed map data: more, smaller items; a 1.3 MB enemy model temporary at the
             * Taurus tower warp fell back to the heap, RAM free 1.66 MB, ISO 2026-11-07). Stop them all (the plan asks
             * again) and try once more */
            unsigned c = 0;
            for (int i = 0; i < nrd; i++) {
                pack_req_cancel(rd[i].tk);  /* returns once nothing more lands in the buffer */
                c += rd[i].bytes >> 10;
                drop(rd[i].k, rd[i].id);
            }
            printf("mapstream: alloc %u KB: %d reads in flight (%u KB) cancelled for a window\n", n >> 10, nrd, c);
            cancel_kb += c;
            nrd = 0;
            mapstream_stats.pending = 0;
            if (make_window(need, 0, C_TOP + 1, &kb))
                p = g_alloc_top(n, npages);
        }
        chains_done = 0;
    }
    if (kb || !p)  /* the caller falls back to the heap on NULL */
        printf("mapstream: alloc %u KB %s, %u KB of cells dropped\n", n >> 10, p ? "ok" : "FAILED", kb);
    if (p)
        mapstream_stats.other_kb += (n + 1023) >> 10;
    return p;
}

void mapstream_free(void *p, unsigned n)
{
    if (!p)
        return;
    g_free(p, n);
    mapstream_stats.other_kb -= (n + 1023) >> 10;
    replan = 1;  /* room for what was refused */
    nblk = 0, nowin = C_TOP + 2;
}

/* resident item j of instance k counts for a settle: the sky of the player's block, or a cell's coarse / kept items
 * within r (r_focus for that block) unless its near level stands for them */
static int settle_item(int k, const MsInst *m, unsigned j, const float *pos, float r, float r_focus)
{
    unsigned tag = m->rit[j].tag;
    if (tag & (MS_RES_TEX | MS_RES_GRASS))  /* foliage is not read in a settle (plan_item) */
        return 0;
    if (tag & MS_RES_SKY)
        return k == (focus < 0 ? 0 : focus);
    unsigned c = MS_RES_CELL(tag);
    float rr = k == focus && r_focus > r ? r_focus : r;
    if (vis_off(m, m->ncell + j))  /* no camera cell near the camera sees it (pvs.c) */
        return 0;
    if (c >= m->ncell || ((tag & MS_RES_COARSE) && near_in(m, c)))
        return 0;
    return box_d2(&m->tab[c], pos) < rr * rr;
}

int mapstream_busy(const float pos[3], float r, float r_focus)
{
    if (r != settle_r || r_focus != settle_rf)
        replan = 1;
    settle_r = r, settle_rf = r_focus;
    if (arena && (replan || !scan_frame))
        return 1;  /* classes not planned yet (all C_FREE): the check below would find nothing to wait for (the boot
                    * settle ended after 0 ms with 51 cells missing, ISO 2026-11-06) */
    for (int i = 0; i < nrd; i++) {  /* only the reads the settle waits for: the rest streams on behind the game */
        const MsInst *m = &ms[rd[i].k];
        if (it_res(m, rd[i].id) && settle_item(rd[i].k, m, rd[i].id - m->ncell, pos, r, r_focus))
            return 1;
    }
    if (room_fail)
        return 0;  /* nothing more fits: go on with what is in */
#if MS_SETTLE_NEAR
    /* the near level within the draw radius (the plan made it C_NEAR2 in the settle; one the cut left out is C_KEEP),
     * for at most MS_SETTLE_NEAR_MS of the settle */
    if (clk_ms - st_t0 < MS_SETTLE_NEAR_MS)
        for (int k = 0; k < MS_INST; k++) {
            const MsInst *m = &ms[k];
            for (unsigned c = 0; c < m->ncell; c++)
                if (m->tab[c].bytes && m->cls[c] >= C_PREF && (m->state[c] == MS_READING ||
                    (m->state[c] == MS_NONE && !blocked(k, c))) && !vis_off(m, c) &&
                    box_d2(&m->tab[c], pos) < sq(MS_R0_DRAW))
                    return 1;
        }
#endif
    for (int k = 0; k < MS_INST; k++) {
        const MsInst *m = &ms[k];
        for (unsigned j = 0; j < m->nres; j++)
            if (m->state[m->ncell + j] == MS_NONE && m->rit[j].bytes && m->cls[m->ncell + j] >= C_PREF &&
                settle_item(k, m, j, pos, r, r_focus) && !blocked(k, m->ncell + j))  /* (not what the cut left out) */
                return 1;
    }
    return 0;
}

const void *mapstream_get(unsigned i)
{
    MsInst *m = &ms[sel];
    if (i >= m->ncell)
        return NULL;
    drawn();
    m->used[i] = frame;  /* asked for: in view (a missing one too) */
    return m->state[i] == MS_READY ? m->buf[i] : NULL;
}

const unsigned char *mapstream_res(unsigned i)
{
    MsInst *m = &ms[sel];
    if (i >= m->nres)
        return NULL;
    drawn();
    m->used[m->ncell + i] = frame;
    return m->state[m->ncell + i] == MS_READY ? (const unsigned char *)m->buf[m->ncell + i] - m->rit[i].rel : NULL;
}
#endif
