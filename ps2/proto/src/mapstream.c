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
#include "dsrsnd_rpc.h"
#include "vu1.h"
#include "hitch.h"

/* Radii (box distance, m). Near: map.c draws a cell's near level within NEAR_DRAW_R + NEAR_HYST = 26 m; read 4 m
 * ahead (~1 s of running, ASSUMED), kept 6 m more against reading again on a turn back. Coarse: pinned within
 * MS_R1_PIN (export_map.WS_R1: the spot budget covers it), wanted out to MS_R1_WANT (map.c CULL_FAR 250 m + a box
 * margin), kept to MS_R1_KEEP. */
/* (Burg plan B3, 2026-11-08: the export's hard tier holds the near level within 20 m, the coarse level within 40 m:
 * the radii follow it; 26 / 30 / 36 / 64 m before) */
#define MS_R0_DRAW 20.0f
/* read ahead out to MS_R0_WANT (export_map.WS_R0 budgets 30 m; the plan cut keeps the extra within the arena, C_PREF
 * is the lowest wanted class): at 30 m, 4 m past the draw radius, cells inside it were still coarse when the player
 * walked in (pop-in, ISO 2026-11-06) */
#ifndef MS_NEAR_FIRST
#define MS_NEAR_FIRST 1
#endif
/* (2026-11-08) 30 / 36 m: the project's DVD-9 budget (draw radius + sprint speed x worst DVD read latency; a CD
 * would need 35 / 41). 40 / 46 m (MS_NEAR_FIRST's read-ahead since 2026-11-06) covered ~1.8x the ground of 30 m.
 * make DEFS=-DMS_R0_WANT=40 -DMS_R0_KEEP=46 = the old radii */
/* Per media (budget doc proposal 2): the DVD radii on a DVD (and host: runs), 35 / 41 m on a CD (the read with a seek
 * takes up to 1 s there); the disc server reports the media once it has looked at the disc (pack_disc_media, checked
 * in mapstream_update). MEDIA=cd builds (MEDIA_CD) take the CD radii whatever the disc. DEFS radii stay fixed. */
/* MS_STEADY (Burg plan B4, default on; make DEFS=-DMS_STEADY=0 = the radii and class edges before it): with the export
 * sized to the visual room the plan has nothing to cut, so what is in may stay in. Keep radii rise (a kept item is the
 * first to go when a wanted one needs room, so keeping costs nothing but saves a read when the player turns back: ~25 %
 * of the map KB of a Burg soak was read again within 30 s, world-14); an item that is in (or being read) is measured
 * MS_CLS_HYST m nearer for its class, so a player standing on a class edge does not flip it every plan; and a plan that
 * has to cut a wanted item says so (hitch_cut, the "stream:" line's plan cut: a logged bug, gated by tools/budget.py). */
#ifndef MS_STEADY
#define MS_STEADY 1
#endif
#define MS_CLS_HYST 2.0f  /* m (ASSUMED: the near switch's own NEAR_HYST in map.c) */
#define MS_KEEP_MORE (MS_STEADY ? 10.0f : 0.0f)  /* m added to the near, horizon and foliage keep radii */
#define MS_R0_WANT_DVD 24.0f
#define MS_R0_KEEP_DVD (30.0f + MS_KEEP_MORE)
#define MS_R0_WANT_CD 29.0f
#define MS_R0_KEEP_CD (35.0f + MS_KEEP_MORE)
#ifndef MS_R0_WANT
#ifdef MEDIA_CD
static float ms_r0_want = MS_R0_WANT_CD, ms_r0_keep = MS_R0_KEEP_CD;
#else
static float ms_r0_want = MS_R0_WANT_DVD, ms_r0_keep = MS_R0_KEEP_DVD;
#endif
#define MS_R0_WANT ms_r0_want
#define MS_R0_KEEP ms_r0_keep
#define MS_MEDIA_RADII 1
#elif !defined MS_R0_KEEP
#define MS_R0_KEEP (MS_R0_WANT + 6.0f)
#endif
/* near parts rank by the nearer of the feet and a point MS_AHEAD_FRAMES ahead along the walking direction (velocity
 * smoothed over ~10 frames; ASSUMED 1.5 s: about the read time of a near cell group from cdrom0: plus a margin), so
 * the cells the player walks into come before those behind (MS_NEAR_FIRST) */
#define MS_AHEAD_FRAMES 45.0f
#define MS_R1_PIN 40.0f   /* pinned coarse: export_map.WS_PIN_R, the hard tier (the ring to MS_R_HZN ranks below the near level) */
#define MS_R0_FIRST 12.0f /* near parts this close rank above coarse cells in view (C_NEAR), the rest of 20 m below (C_NEAR2) */
/* MS_NEAR_FIRST (default on; make DEFS=-DMS_NEAR_FIRST=0 = the old order): the near parts within MS_R0_FIRST rank
 * above the pinned coarse level out of view (C_PIN0 within MS_R1_FIRST and the sky / backdrop of the player's block >
 * C_VIEW coarse in view > C_NEAR > C_PIN coarse out of view). With every pinned coarse cell of all blocks within 64 m and every one in view within 128 m ahead of
 * them, the near level around the player waited behind megabytes of far coarse cells after each spawn (user report,
 * 2026-11-06). The coarse cells close by still go first: a missing one is a hole at the player's feet. */
#ifndef MS_NEAR_FIRST
#define MS_NEAR_FIRST 1
#endif
#define MS_R1_FIRST 32.0f
#ifndef MS_NEAR_OVER
#define MS_NEAR_OVER 1
#endif
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
/* Horizon (map.c HZN_R, tools/horizon.py): items tagged MS_RES_LOD (a cell's coarse level and kept opaque meshes, the
 * cell has a horizon) are wanted only within MS_R_HZN and kept to MS_R_HZN_KEEP; beyond, the resident horizon is
 * drawn. That leaves the reader the near detail: ~1000 small coarse items in view out to 260 m, a seek each, were the
 * Burg ISO route's holes and pop-in (2026-11-06). 64 = MS_R1_PIN: the export budget (WS_R1 96 m) covers it. */
/* (2026-11-08: 48 / 56 m with map.c HZN_R 40 and the horizon judged from 38 m freed ~0.55 MB of pinned coarse cells
 * at the Burg spots of Ame's report, but the horizon grew 62 KB per block and the room went down with it: ~0.3 MB net
 * for a closer, coarser horizon; not kept. The "pin0 by distance" log line shows the bands.) */
/* (Burg plan B3, 2026-11-08: 48 / 56 m with map.c HZN_R 40 and the horizon judged from 38 m, the export's hard tier
 * holding the coarse level within 40 m: HZN_R + HZN_HYST + the camera's ~5 m stay inside 48 m) */
#define MS_R_HZN 48.0f
#define MS_R_HZN_KEEP (56.0f + 2 * MS_KEEP_MORE)
/* MS_STABLE (default on; 0 = the old order): distance-only ranks inside MS_R1_PIN and a bonus for what is in
 * (MS_HOLD_K x the squared distance, ~16 % nearer), so what fits stays and what does not fit stays out */
#ifndef MS_STABLE
#define MS_STABLE 1
#endif
#define MS_HOLD_K 0.7f
#define MS_THRASH_TICKS 28u  /* hitch.c: a read again within ~30 s of its drop counts as thrash (ticks of 32 frames) */
#ifndef MS_WHOLE_CELLS
#define MS_WHOLE_CELLS 1  /* the near parts of a cell are planned together (0: one by one, the old cut) */
#endif
#define MS_R1_VIEW 128.0f /* coarse cells in view this close: C_VIEW, beyond: C_VIEW2 */
#define MS_R1_WANT 260.0f
#define MS_R1_KEEP 280.0f
/* foliage items (MS_RES_GRASS): map.c draws tufts at (GRASS_D0 10 m / d)^2 of their density, ~6 % at 40 m (ASSUMED
 * radius); kept 8 m more */
#define MS_GRASS_WANT 40.0f
#define MS_GRASS_KEEP (48.0f + MS_KEEP_MORE)
/* backdrop chunks (MS_RES_FAR) rank as if no farther than this: they are the horizon wherever the player stands */
#define MS_FAR_D 64.0f
/* an item map.c asked for (drawn, or wanted while missing) within MS_VIEW_FRAMES counts as in view: its rank is
 * MS_VIEW_K x the squared distance (half the distance); ASSUMED 1 s so a look around does not flip classes */
#define MS_VIEW_FRAMES 30u
/* MS_STEADY: whether a coarse or backdrop item counts as in view for its class (C_VIEW / C_VIEW2 vs C_FARC) is judged
 * over MS_VIEW_STICKY frames: with the 1 s window another block's backdrop (Firelink's, behind the Burg) dropped to
 * C_FARC on every look away, went first when room was needed and was read again on the next look back: ~180 of 260
 * far seeks on the ISO world_route were Firelink's pack, 60-100 MB from the Burg's on the disc (B8b, 2026-11-08).
 * ASSUMED 10 s (a look around and back) */
#define MS_VIEW_STICKY 300u
#define MS_VIEW_K 0.25f
/* re-plan when the player moved this far or this many frames passed (~5000 items over 5 blocks: a scan every frame
 * cost the Burg bonfire 30 -> 22 fps with v1) */
#define MS_SCAN_D 4.0f
#define MS_SCAN_FRAMES 16u
/* a texture's mip 0 kept while texcache asked for it within this many frames (as v1) */
#define MS_TEX_KEEP (MS_STEADY ? 600u : 120u)  /* (MS_STEADY: 20 s, a mip 0 asked for again after a look around) */
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
/* the class of a streamed mip 1 asked for lately (mapsplit.py MIP1_OUT). C_VIEW2 (the coarse cells in view) took
 * near-class "no room" refusals 12 -> 145 on iso_ame_report3 (one run, 2026-11-08): the 4 KB items spread over the
 * pages the near parts needed. C_PREF: as mip 0, after every near part */
#ifndef MS_MIP1_CLS
#define MS_MIP1_CLS C_PREF
#endif

MapStreamStats mapstream_stats;

typedef struct {
    Pack pk;
    int borrowed;
    unsigned base, total;  /* cells.bin in the pack */
    const MapCell *tab;
    const short *keys;  /* cell keys (mapstream.h cell_keys), NULL: none / MS_NEAR_CUBE=0 */
    unsigned ncell;
    MapResItem *rit;
    unsigned nres;
    /* per item (ids [0, ncell) near entries, [ncell, ncell + nres) resident items) */
    unsigned char *state, *cls;
    void **buf;
    unsigned *used;        /* frame map.c last asked for it */
    unsigned short *rgrp;  /* resident item: first item (index into rit) of the group it was read with */
    unsigned short *ld;    /* per item (hitch.c churn): 0 never read, 1 in, else the tick (frame / 32) + 2 it was dropped */
    unsigned *wq;          /* per near part (near wait log): clk_ms + 1 of the plan that first wanted it while not in, 0 */
    unsigned char *cmark;  /* per cell, during the plan's cut: 1 its near parts are in the plan, 2 they are not */
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
/* MS_ITEMS_TOP (default on; 0 = the old limit): the pages map items may use now. Once mapstream_alloc holds more than
 * MS_OTHER (two Burg blocks' lite sets: 4.2 MB) the reserve is gone anyway, and the plan's cut already counts every
 * page not held by them as item room (cap = npages - other); items kept out of the reserve's free pages left that room
 * idle while 63 KB coarse cells of the pinned class found no room (384 KB of whole free pages there, ISO A/B of Ame's
 * play report 2026-11-08). mapstream_alloc clears item pages there like any other. */
#ifndef MS_POOLS
#define MS_POOLS 1
#endif
/* MS_POOLS: bytes asked for the structural / misc pools (mapstream_pools), their first pages once the arena exists
 * (struct: [spage0, npages), misc: [mpage0, spage0), items below mpage0 = ipages), bytes mapstream_alloc placed in
 * item pages (the plan leaves them out) */
static unsigned pool_req[2], spage0, mpage0, items_other;
static int pools_on, pool_busy;
/* MS_LEND (default 1; the planning thread's "drop-only lending", 2026-11-08): the pools' idle pages hold map items of
 * the lowest classes the plan wants (read-ahead, coarse out of view or beyond the view radius, the pinned coarse ring:
 * MS_LEND_CLS), never a near part. They are never copied: a pool allocation that finds no room drops them (those a
 * chain may still read wait two draws: mapstream_pool_wait). The visual room the export solves against stays the item
 * pages; lent room only fills the horizon ring where fewer blocks are loaded than the pool is sized for. */
#ifndef MS_LEND
#define MS_LEND 1
#endif
static unsigned lent_n;  /* items in pool pages now (A/B log) */
/* frames a mapstream_pool_prepare keeps new items out of the structural pool. 0 (default): with ~2 s, under B3's
 * radii the near parts lost the lent pages they relied on (soak2 pop-ins 25 / 37 -> 54 / 54, taurus 16 / 12 -> 35 / 35,
 * 2026-11-09); the drop after one VU1 wait in mapstream_alloc_pool is what ended the long seam holds */
#ifndef MS_PREP_NOLEND
#define MS_PREP_NOLEND 0
#endif
static unsigned lend_struct_off;  /* frame until which nothing new is lent the structural pool (a lite set is due) */
static unsigned early_n;  /* lent items let go ahead of a lite set (mapstream_pool_prepare), for the log */
/* near parts may take idle pool pages too, up to this much (the planning thread: a refused near part is not drawn
 * either; a lent one draws until a lite set needs its page). Their drops are logged and counted (hitch.c stream line) */
/* MS_LEND_ALL 0 (default): only items out of view or ahead of the player are lent (C_PREF, C_FARC, C_VIEW2), so a
 * lite set's drop never removes something drawn. 1: every class (near parts up to MS_LEND_NEAR_KB). With B3's radii
 * all-class lending cost ~25 world_route pop-ins (36 / 34 vs 10-13 with MS_LEND=0, 2026-11-09); without B3 it brought
 * iso_ame_report3's horizon share back to main's */
#ifndef MS_LEND_ALL
#define MS_LEND_ALL 0
#endif
#ifndef MS_LEND_NEAR_KB
#define MS_LEND_NEAR_KB 900
#endif
static unsigned lent_near;  /* bytes of near parts placed in pool pages (approximate: decremented on drop) */
#ifndef MS_ITEMS_TOP
#define MS_ITEMS_TOP 1
#endif
static inline unsigned ilim(void)
{
    if (pools_on)
        return ipages;
    return MS_ITEMS_TOP && (mapstream_stats.other_kb << 10) > MS_OTHER ? npages : ipages;
}
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
 * Default on; make DEFS=-DMS_BORROW=0 = the old fixed arena. */
#ifndef MS_BORROW
#define MS_BORROW 1
#endif
#define MS_XMAX 96                          /* pages that may be borrowed (6 MB) */
/* heap kept free: AGENTS.md's 1.5 MB in the heaviest spot + one Burg enemy type's load (model.bin falls back to the
 * heap, then its skin pages and compact copy; ~1.2 MB, world.c WORLD_HEAP_MODELS) */
#ifndef MS_BORROW_FLOOR
#define MS_BORROW_FLOOR (2816u << 10)
#endif
#define MS_BORROW_HYST (256u << 10)         /* borrow only above floor + this (no borrow / give back each frame) */
static unsigned char *xpage[MS_XMAX];
static unsigned char xret[MS_XMAX];  /* being given back: no new items (its drawn ones wait for their chains) */
static unsigned nx;  /* borrowed pages now */
static unsigned x_want;  /* heap free a caller of mapstream_heap_release waits for (x_update drains pages until) */
static unsigned xstat_borrow, xstat_back, xstat_drop_kb;
static inline unsigned ntot(void) { return npages + MS_XMAX; }
static inline unsigned char *g_ptr(unsigned g)
{
    unsigned p = g / MS_GPP;
    return (p < npages ? arena + (size_t)p * MS_PAGE : xpage[p - npages]) + (g % MS_GPP) * MS_GRAN;
}
static unsigned g_of(const void *ptr)
{
    const unsigned char *q = ptr;
    if (q >= arena && q < arena + arena_size)
        return (unsigned)(q - arena) / MS_GRAN;
    for (unsigned i = 0; i < MS_XMAX; i++)
        if (xpage[i] && q >= xpage[i] && q < xpage[i] + MS_PAGE)
            return (npages + i) * MS_GPP + (unsigned)(q - xpage[i]) / MS_GRAN;
    return ~0u;  /* not ours (a caller bug) */
}
/* the item pages: those below ilim(), then the borrowed ones (absent ones are marked full) */
#define FOR_IPAGES(p) for (unsigned p##_i = 0, p; p##_i < ilim() + MS_XMAX && (p = p##_i < ilim() ? p##_i : npages + p##_i - ilim(), 1); p##_i++)

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
        FOR_IPAGES(p) {
            if (pfree[p] < ng || (pass == 0) == (pfree[p] == MS_GPP) || (p >= npages && xret[p - npages]))
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
        run = pfree[p] == MS_GPP ? run + 1 : 0;
        if (run == np) {
            unsigned end = (p + np) * MS_GPP;
            g_mark(end - ng, ng, 1);
            return arena + (size_t)(end - ng) * MS_GRAN;
        }
    }
    return NULL;
}
static void *g_alloc_top(unsigned n, unsigned top) { return g_alloc_range(n, 0, top); }
/* MS_LEND: n bytes (<= a page) inside one page of the pools, pages already in use first */
static void *g_alloc_lend(unsigned n)
{
    unsigned ng = (n + MS_GRAN - 1) / MS_GRAN;
    if (!ng || ng > MS_GPP)
        return NULL;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned p = mpage0; p < (frame < lend_struct_off ? spage0 : npages); p++) {
            if (pfree[p] < ng || (pass == 0) == (pfree[p] == MS_GPP))
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
static void g_free(void *ptr, unsigned n)
{
    unsigned g0 = g_of(ptr);
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
        unsigned nt = ntot();  /* + the borrowed pages' slots */
        gmap = calloc((nt * MS_GPP + 31) / 32, 4);
        pfree = malloc(nt * sizeof *pfree);
        pcls = malloc(nt * sizeof *pcls), pcost = malloc(nt * sizeof *pcost);
        pitem = malloc(nt * sizeof *pitem);
        if (!gmap || !pfree || !pcls || !pcost || !pitem) {
            free(arena), free(gmap), free(pfree), free(pcls), free(pcost), free(pitem);
            pcls = pcost = pitem = NULL;
            arena = NULL;
        } else {
            for (unsigned p = 0; p < npages; p++)
                pfree[p] = MS_GPP;
            for (unsigned p = npages; p < ntot(); p++)  /* not borrowed: full */
                pfree[p] = 0, memset(&gmap[p * MS_GPP / 32], 0xff, MS_GPP / 8);
            arena_size = sz;
            ipages = npages > 2 * (MS_OTHER / MS_PAGE) ? npages - MS_OTHER / MS_PAGE : npages / 2;
            unsigned sp = (pool_req[0] + MS_PAGE - 1) / MS_PAGE, mp = (pool_req[1] + MS_PAGE - 1) / MS_PAGE;
            if (MS_POOLS && sp && sp + mp + 16 < npages) {  /* (16: at least 1 MB of items) */
                pools_on = 1;
                spage0 = npages - sp, mpage0 = spage0 - mp, ipages = mpage0;
            }
        }
    }
    printf("mapstream: v2 arena %u KB in %u pages (heap free %u KB)\n", arena ? arena_size >> 10 : 0, npages, hf >> 10);
    if (pools_on && arena_size < pool_max)
        printf("mapstream: arena %u KB is under the build's %u KB (heap): the visual room is smaller\n",
               arena_size >> 10, pool_max >> 10);
    if (pools_on)
        printf("mapstream: arena structural %u KB, misc %u KB, visual %u KB\n", (npages - spage0) * (MS_PAGE >> 10),
               (spage0 - mpage0) * (MS_PAGE >> 10), ipages * (MS_PAGE >> 10));
    else if (arena && pool_req[0])
        printf("mapstream: arena pools off (MS_POOLS %d, structural %u KB + misc %u KB asked of %u KB)\n", MS_POOLS,
               pool_req[0] >> 10, pool_req[1] >> 10, arena_size >> 10);
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

/* the near level's distance (mapstream.h cell_near_d2: the box clipped to the cell's cube, MS_NEAR_CUBE) */
#define near_d2(m, c, p) cell_near_d2(&(m)->tab[c], (m)->keys ? (m)->keys + 4 * (c) : (const short *)0, p)

static int focus = -1, settling, hold;
static float ahead[3], vel[3], last_pos[3];  /* the walking look-ahead point (MS_AHEAD_FRAMES) */
static int ahead_on;
/* how long a near part waits from the plan that first wants it to its landing (ms: running average and mean
 * deviation; the reads queued and in flight before it count), logged with the plan's class sizes. Budget doc
 * proposal 5 (2026-11-08) set the look-ahead from it (speed x wait, the strip ahead ranked first): on world_sprint
 * near pop-in went 69-71 -> 78-81 against the fixed look-ahead. The pop-ins there wait 3-40 s inside the draw
 * radius, for room in the arena, not for the disc, so the look-ahead stays as it is. */
static float lat_avg = 800.0f, lat_dev = 200.0f;
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
static unsigned (*vis_gen)(int k);
/* The answers change whenever the camera crosses into another camera cell of a block's grid (every few seconds of
 * walking). Asking every item of all five blocks at once (~45k pvs ranges) cost 0.2-1.1 s in one frame: the walking
 * hitches of the 40 min Burg soak (ISO, 2026-11-07; tools/hitchsum.py). The refresh runs as a job instead, one block
 * at a time (only the block whose grid was crossed), MS_VIS_SLICE_US of EE time per frame (MS_VIS_SETTLE_US in a
 * settle); answers update in place as it goes and the plan is redone once a block is through. An item keeps its old
 * answer until the job reaches it: a few frames late, never wrong for longer. */
#define MS_VIS_SLICE_US 1500u
#define MS_VIS_SETTLE_US 20000u
static struct { int k, fresh; unsigned id, g, nh, kb; } vjob = {-1};

static void vis_begin(int k)
{
    MsInst *m = &ms[k];
    unsigned n = it_n(m);
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
        m->visn = n;
    }
    vjob.k = k, vjob.fresh = fresh, vjob.id = 0, vjob.g = vis_gen(k), vjob.nh = vjob.kb = 0;
}

static int replan;
/* one slice of the visibility job (us of EE time); starts the next block's job when none runs */
static void vis_step(unsigned us)
{
    if (!vis_hook || !vis_gen)
        return;
    HITCH_BEGIN(v);
    unsigned t0 = ticks(), lim = us * 295;
    for (int pass = 0; pass < MS_INST; pass++) {
        if (vjob.k < 0) {
            for (int k = 0; k < MS_INST && vjob.k < 0; k++)
                if (it_n(&ms[k]) && (ms[k].visn != it_n(&ms[k]) || ms[k].visg != vis_gen(k)))
                    vis_begin(k);
            if (vjob.k < 0)
                break;
        }
        MsInst *m = &ms[vjob.k];
        unsigned n = it_n(m), id = vjob.id;
        if (!m->visv || m->visn != n) {  /* the block closed or grew meanwhile: start it again */
            vjob.k = -1;
            continue;
        }
        unsigned short tick = (unsigned short)(frame >> 2);
        for (; id < n; id++) {
            if (!(id & 63) && ticks() - t0 > lim)
                break;
            int v = it_bytes(m, id) ? vis_hook(vjob.k, id) : 1;
            if (!v && m->visv[id])
                m->vist[id] = vjob.fresh ? (unsigned short)(tick - MS_VIS_HOLD - 1) : tick;  /* seen until now */
            m->visv[id] = (unsigned char)!!v;
            if (!v)
                vjob.nh++, vjob.kb += it_bytes(m, id);
        }
        vjob.id = id;
        if (id < n)
            break;  /* out of time: on from here next frame */
        mapstream_stats.vis_items += vjob.nh - m->vish, mapstream_stats.vis_kb += (vjob.kb >> 10) - m->viskb;
        m->vish = vjob.nh, m->viskb = vjob.kb >> 10;
        m->visg = vjob.g;  /* (a crossing meanwhile: its gen moved on, the next job takes it) */
        vjob.k = -1;
        replan = 1;
        if (ticks() - t0 > lim)
            break;
    }
    HITCH_END(v, HS_MSVIS);
}
/* outside the visibility now */
static inline int vis_off(const MsInst *m, unsigned id) { return m->visv && id < m->visn && !m->visv[id]; }
/* out of the plan: outside the visibility now and for MS_VIS_HOLD ticks */
static inline int vis_cut(const MsInst *m, unsigned id)
{
    return vis_off(m, id) && (unsigned short)((unsigned short)(frame >> 2) - m->vist[id]) > MS_VIS_HOLD;
}

void mapstream_vis_hooks(int (*vis)(int k, unsigned id), unsigned (*gen)(int k))
{
#ifdef NO_PVS
    (void)vis, (void)gen;
#else
    vis_hook = vis, vis_gen = gen;
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

/* MS_ONE_LOD (default on; make DEFS=-DMS_ONE_LOD=0 = the old plan, coarse items kept under drawn near cells): one level
 * of detail in memory per place (the SDK / re3 rule). 70-90 % of each block's resident level is coarse chunks (Burg
 * west 3.4 of 3.8 MB, Parish 3.1 of 3.3 MB) that map.c skips wherever the cell's near level is drawn; kept in the arena
 * beside those near cells they ranked above them (C_PIN0 within MS_R1_PIN) and the near level found no room. A cell's
 * coarse items are planned C_KEEP while all its near parts are in and the cell is within MS_COARSE_R: never read, the
 * first to go when a wanted item needs room, kept while nothing does (as C_FREE they went at once and were read again
 * on the way out: the ISO world_route read 58 -> 69 MB, 780 -> 1110 re-reads, 2026-11-08); beyond it they are wanted
 * again at their own class. While any coarse item of the cell is not in, its loaded near parts take that
 * class (map.c draws the near level wherever the coarse level misses, at any distance), so a near part never goes
 * before the coarse level is back: #110's first try (c24273d, reverted) left the near parts at their own class, a near
 * part went next and the cell was a hole (~200 cells missing in the Burg, ISO 2026-11-06). Its teleport hang was the
 * settle freeing items of the last drawn frame's chain (fixed by the dcur / dprev in_chain rule). Not in a settle: a
 * settle reads no coarse item whose near level is in (settle_item). */
#ifndef MS_ONE_LOD
#define MS_ONE_LOD 1
#endif
#define MS_COARSE_R (MS_R0_KEEP - MS_KEEP_MORE - 8.0f)  /* (the near keep radius before MS_STEADY, less 8 m) */
static unsigned onelod_n, onelod_kb;  /* coarse items the last plan left to their near level (kept or not) */

/* MS_STEADY: an item that is in or on its way is measured MS_CLS_HYST nearer for its class */
static inline float steady_d2(const MsInst *m, unsigned id, float d2)
{
    if (!MS_STEADY || m->state[id] == MS_NONE || m->state[id] == MS_FAILED || d2 <= 0)
        return d2;
    float d = sqrtf(d2) - MS_CLS_HYST;
    return d > 0 ? d * d : 0;
}

/* class of item id of instance k with the player at pos; *key ranks it within the class (lower first) */
static int plan_item(int k, const MsInst *m, unsigned id, const float *pos, float *key)
{
    int view = frame - m->used[id] <= MS_VIEW_FRAMES;
    float vk = view ? MS_VIEW_K : 1.0f;
    if (MS_STEADY)  /* (the rank keeps the 1 s view, the class the sticky one) */
        view = frame - m->used[id] <= MS_VIEW_STICKY;
    if (id < m->ncell) {
        if (!m->tab[id].bytes)
            return C_FREE;
        float d2 = steady_d2(m, id, near_d2(m, id, pos));
        /* near parts rank by distance alone (MS_STABLE): with the view factor a look around reordered them against
         * the plan's cut every turn and the arena read the same parts again and again (soak, 2026-11-07) */
        vk = MS_STABLE ? 1.0f : vk;
        *key = d2 * vk;
        /* a settle reads the near level within the draw radius (MS_SETTLE_NEAR) after the coarse level it waits for (as
         * C_NEAR it took the plan from coarse cells inside the settle radius, which the settle then waited for until its
         * 30 s cap: Firelink's near level within 26 m is 4.1 MB, ISO 2026-11-06); the rest streams in */
        if (settling)
            return MS_SETTLE_NEAR && d2 < sq(MS_R0_DRAW) ? C_NEAR2 : d2 < sq(MS_R0_KEEP) ? C_KEEP : C_FREE;
#if MS_NEAR_FIRST
        if (ahead_on) {
            float da = near_d2(m, id, ahead);
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
        if (tag & MS_RES_MIP1)  /* a streamed mip 1 (mapsplit.py MIP1_OUT): what a cell in view draws beyond the mip 0 range */
            return age <= 1 ? MS_MIP1_CLS : age <= MS_TEX_KEEP ? C_KEEP : C_FREE;
        return age <= 1 ? C_PREF : age <= MS_TEX_KEEP ? C_KEEP : C_FREE;
    }
    if (tag & MS_RES_SKY) {
        *key = 0;
        return k == (focus < 0 ? 0 : focus) ? C_PIN0 : C_KEEP;
    }
    unsigned c = MS_RES_CELL(tag);
    if (c >= m->ncell)
        return C_FREE;
    float d2 = steady_d2(m, id, box_d2(&m->tab[c], pos));
    /* a cell the horizon stands for beyond MS_R_HZN (map.c HZN_R): never read there, kept a little while (its
     * backdrop too: the horizon has it) */
    if ((tag & MS_RES_LOD) && d2 >= sq(MS_R_HZN)) {
        *key = d2 * vk;
        return d2 < sq(MS_R_HZN_KEEP) ? C_KEEP : C_FREE;
    }
    if (tag & MS_RES_FAR) {  /* the backdrop: the horizon of the player's block; another block's while in view */
        *key = (d2 < sq(MS_FAR_D) ? d2 : sq(MS_FAR_D)) * vk;
        /* (MS_STEADY: another loaded block's backdrop, Firelink's behind the Burg, keeps C_VIEW2 whether in view or
         * not: as C_FARC it went first whenever room was short and came back from across the disc) */
        return k == (focus < 0 ? 0 : focus) ? C_PIN0 : view || MS_STEADY ? C_VIEW2 : C_FARC;
    }
    *key = d2 * vk;
    if (tag & MS_RES_GRASS)
        return settling ? C_FREE : d2 < sq(MS_GRASS_WANT) ? C_NEAR2 : d2 < sq(MS_GRASS_KEEP) ? C_KEEP : C_FREE;
    /* (one level of detail per place: plan_ drops this item while its cell's near level stands for it, MS_ONE_LOD) */
#if MS_STABLE
    /* the coarse level the horizon does not stand for yet (within MS_R1_PIN): one order by distance whatever the
     * camera looks at. In view (C_VIEW, above the near parts) or not (C_PIN, below them) flipped those cells between
     * two classes on every look around; with the arena holding ~half of what the Burg pins (two blocks' lite sets
     * beside it) each turn evicted what the last one read: 80 % of all map reads were re-reads, ~1750 swaps between
     * near / coarse / horizon on screen in 40 min (Burg soak on the ISO, 2026-11-07). All of it ranks above the near
     * parts: with near parts first (MS_NEAR_FIRST) the arena never held the Burg's near level anyway (7-37 cells within
     * 24 m waited for it up to 90 s) and 5-17 cells inside the horizon radius drew their untextured horizon instead of
     * their coarse level, the "malformed parts" of the user report. Coarse first, the horizon is a ring at the far
     * edge and the near level fills in nearest cell first with what is left. */
    /* MS_NEAR_OVER (Burg plan B3, default on; 0 = pinned as below): the coarse items of a cell with a near level
     * within MS_R0_DRAW rank below the near parts (C_VIEW2). The export's hard tier budgets the coarse level from
     * MS_R0_DRAW out and the near level inside it, never both at one cell (export_map.py WS_UNION, world_budget.py);
     * pinned above the near parts, the coarse level of the cells around the player held ~1 MB more than the budget at
     * the median spot until their near parts landed, and the near parts were refused ("no room", Burg soak 2026-11-08).
     * A cell whose near part is not in and whose coarse level is out draws its horizon (map.c), not a hole. */
#if MS_NEAR_OVER
    if ((tag & MS_RES_COARSE) && (m->tab[c].near & ~CELL_PART) && near_d2(m, c, pos) < sq(MS_R0_DRAW)) {
        *key = d2;
        return C_VIEW2;
    }
#endif
    if (d2 < sq(MS_R1_PIN)) {  /* and all of it above the near parts: the near level gets what is left */
        *key = d2;
        return C_PIN0;
    }
#endif
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
    if (pools_on && lent_n) {  /* (MS_LEND: one in the pools' pages) */
        unsigned g = g_of(m->buf[a]);
        int in_pool = g != ~0u && g >= mpage0 * MS_GPP && g < npages * MS_GPP;
        lent_n -= in_pool;
        if (in_pool && (m->cls[a] == C_NEAR || m->cls[a] == C_NEAR2)) {
            unsigned b = grp_bytes(m, a, e);
            lent_near = lent_near > b ? lent_near - b : 0;
        }
    }
    g_free(m->buf[a], grp_bytes(m, a, e));
    for (unsigned j = a; j < e; j++) {
        if (it_tex(m, j) && tex_set)
            tex_set(k, m->rit[j - m->ncell].tag & ~MS_RES_TEX, NULL);
        unsigned b = it_bytes(m, j);
        if (m->ld && m->ld[j])
            m->ld[j] = (unsigned short)((frame >> 5) + 2);
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

static inline int lendable(int cls)
{
    /* every class but the near parts (C_NEAR, C_NEAR2): a lent coarse cell dropped for a lite set shows its horizon
     * for a moment, a near part would pop. With only the classes below C_PIN (and C_VIEW) lendable, iso_ame_report3
     * drew the horizon inside HZN_R 99.5 % of the time (main 54 %; all classes lendable: 53 %) */
    return MS_LEND && pools_on && cls > C_KEEP && (MS_LEND_ALL || cls <= C_VIEW2);
}
/* MS_LEND: drop the lent items in pool pages [p0, p1) that no chain reads; the number a chain may still read */
static unsigned lend_reclaim_(unsigned p0, unsigned p1, int keep_near);
static unsigned lend_reclaim(unsigned p0, unsigned p1) { return lend_reclaim_(p0, p1, 0); }
/* keep_near: near parts stay (mapstream_pool_prepare: ahead of time, they are what the player is looking at) */
static unsigned lend_reclaim_(unsigned p0, unsigned p1, int keep_near)
{
    unsigned busy = 0;
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms[k];
        for (unsigned id = 0; m->state && id < it_n(m); id++) {
            unsigned a;
            if (m->state[id] != MS_READY || (grp_of(m, id, &a), a != id) || !m->buf[id])
                continue;
            unsigned g = g_of(m->buf[id]);
            if (g == ~0u || g < p0 * MS_GPP || g >= p1 * MS_GPP)
                continue;
            if (keep_near && (m->cls[id] == C_NEAR || m->cls[id] == C_NEAR2))
                continue;
            if (!settling && grp_chain(m, id))  /* (a settle waited for VU1: no chain reads the arena; without this the
                                                 * lite set waited out the settle: "settle stopped", taurus 2026-11-09) */
                busy++;
            else {
                if (m->cls[id] == C_NEAR || m->cls[id] == C_NEAR2) {
                    hitch_lent_near();
                    printf("mapstream: lent near dropped for a lite set: item %d.%u (%u KB)\n", k, id,
                           grp_bytes(m, id, grp_of(m, id, &a)) >> 10);
                }
                drop(k, id);
            }
        }
    }
    return busy;
}

/* ---- making room ----
 * Clear `need` whole pages (a window; need = 1 for a read group): every group in the window must be loaded, of a class
 * below `below`, and not handed out in the last two map draws (in_chain). Cost per page: (highest class in it, then
 * the bytes and recency of its groups); the cheapest window from the top goes. 1 when cleared. */
static int nowin = C_TOP + 2;  /* a window of 1 page was not found for this class or above since the last change */
static int make_window_(unsigned need, unsigned ng_need, int below, unsigned *kb);
static int make_window(unsigned need, unsigned ng_need, int below, unsigned *kb)
{
    HITCH_BEGIN(w);
    int r = make_window_(need, ng_need, below, kb);
    HITCH_END(w, HS_MSWIN);
    return r;
}
static int make_window_(unsigned need, unsigned ng_need, int below, unsigned *kb)
{
    unsigned lim = ng_need || pools_on ? ilim() : npages;  /* a read's window lies where items may go (MS_POOLS: and
                                                             * mapstream_alloc's beyond the pools) */
    if (!need || need > npages || !pcls || (need == 1 && below >= nowin))
        return 0;
    memset(pcls, 0, ntot() * sizeof *pcls);
    memset(pcost, 0, ntot() * sizeof *pcost);
    memset(pitem, 0, ntot() * sizeof *pitem);
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                continue;
            int c = grp_cls(m, id);
            unsigned u = grp_used(m, id), n = grp_bytes(m, a, e);
            unsigned g0 = g_of(m->buf[a]), ng = (n + MS_GRAN - 1) / MS_GRAN;
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
    for (unsigned p = 0; p < ntot(); p++) {
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
    for (unsigned p = npages; need == 1 && ng_need && p < ntot(); p++) {  /* a read: borrowed pages too */
        if (!xpage[p - npages] || xret[p - npages] || (pcls[p] & 0x200))
            continue;
        unsigned long long score = (unsigned long long)(pcls[p] & 0x4ff) << 40 | pcost[p];
        if (score < bestc)
            bestc = score, best = (int)p;
    }
    if (best < 0) {
        if (need == 1 && below < nowin)
            nowin = below;
        return 0;
    }
    unsigned w0 = best * MS_GPP, w1 = (best + need) * MS_GPP;  /* granules */
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                continue;
            unsigned o = g_of(m->buf[a]);
            if (o < w1 && o + (grp_bytes(m, a, e) + MS_GRAN - 1) / MS_GRAN > w0 && grp_cls(m, id) < below && !grp_chain(m, id)) {
                if (kb)
                    *kb += grp_bytes(m, a, e) >> 10;
                hitch_drop(grp_cls(m, id) >= C_PREF, grp_bytes(m, a, e));
                drop(k, id);
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
 * Default on; make DEFS=-DMS_DEFRAG=0 = the old path (never move). */
#ifndef MS_DEFRAG
#define MS_DEFRAG 1
#endif
#define MS_DEFRAG_PEND 64            /* old copies waiting for their chains */
#define MS_DEFRAG_KB 256u            /* most KB copied for a read (~2 ms of EE memcpy, ASSUMED); 3x for a relief window */
#define MS_DEFRAG_WAIT 8u            /* frames a read waits at the queue head for a page being emptied */
static struct { unsigned g0, ng, fr; } pend[MS_DEFRAG_PEND];
static int npend;
static unsigned pend_frame;  /* frame of the last move */
static unsigned defrag_moves, defrag_kb;

/* old copies no chain can read any more go back */
static void pend_release(void)
{
    for (int i = 0; i < npend; i++)
        if (chains_done || !dprev || (int)(dprev - pend[i].fr) > 0) {
            g_mark(pend[i].g0, pend[i].ng, 0);
            pend[i--] = pend[--npend];
        }
}

/* n bytes inside one page outside pages [x0, x1) (below ilim): fullest page with a fitting hole first */
static void *g_alloc_ex(unsigned n, unsigned x0, unsigned x1)
{
    unsigned ng = (n + MS_GRAN - 1) / MS_GRAN, bp = ~0u, bg = 0;
    if (!ng || ng > MS_GPP)
        return NULL;
    FOR_IPAGES(p) {
        if ((p >= x0 && p < x1) || (p >= npages && xret[p - npages]) || pfree[p] < ng || (bp != ~0u && pfree[p] >= pfree[bp]))
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
static int defrag(unsigned need, unsigned lim, unsigned max_kb)
{
    if (!MS_DEFRAG || !pitem || npend > MS_DEFRAG_PEND - 16 || need > lim)
        return 0;
    HITCH_BEGIN(d);
    memset(pitem, 0, ntot() * sizeof *pitem);
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms[k];
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
        fr += pfree[p];
    int best = -1;
    unsigned bused = ~0u;
    for (unsigned p = lim - need + 1; p-- > 0;) {
        unsigned used = 0, q;
        for (q = p; q < p + need; q++) {
            unsigned u = MS_GPP - pfree[q];
            if (pitem[q] != u)
                break;  /* a read in flight, an old copy or a mapstream_alloc block */
            used += u;
        }
        if (q < p + need || !used)
            continue;
        unsigned wfree = 0;
        for (q = p; q < p + need; q++)
            wfree += q < ilim() ? pfree[q] : 0;
        if (used < bused && used <= max_kb * 1024 / MS_GRAN && fr - wfree >= used + used / 4)
            bused = used, best = (int)p;
    }
    int ok = best >= 0;
    if (ok) {
        unsigned w0 = (unsigned)best * MS_GPP, w1 = w0 + need * MS_GPP, kb = 0;
        for (int k = 0; k < MS_INST && ok; k++) {
            MsInst *m = &ms[k];
            for (unsigned id = 0; id < it_n(m) && ok; id++) {
                unsigned a, e;
                if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                    continue;
                unsigned char *src = m->buf[a];
                unsigned n = grp_bytes(m, a, e), g0 = g_of(src), ng = (n + MS_GRAN - 1) / MS_GRAN;
                if (g0 < w0 || g0 >= w1)
                    continue;
                unsigned char *dst = npend < MS_DEFRAG_PEND ? g_alloc_ex(n, (unsigned)best, (unsigned)best + need) : NULL;
                if (!dst) {
                    ok = 0;
                    break;
                }
                memcpy(dst, src, n);
                SyncDCache(dst, dst + n - 1);  /* the VIF / GIF DMA reads RAM */
                for (unsigned j = a; j < e; j++) {
                    m->buf[j] = dst + ((unsigned char *)m->buf[j] - src);
                    if (it_tex(m, j) && tex_set)
                        tex_set(k, m->rit[j - m->ncell].tag & ~MS_RES_TEX, m->buf[j]);
                }
                pend[npend++] = (typeof(pend[0])){g0, ng, frame};
                kb += n;
                defrag_moves++;
            }
        }
        defrag_kb += kb >> 10;
        pend_frame = frame;
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
    if (!nx)
        return 0;
    memset(ok, 0, sizeof ok);
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                continue;
            unsigned g0 = g_of(m->buf[a]);
            if (g0 / MS_GPP < npages || g0 == ~0u || grp_chain(m, id))
                continue;
            ok[g0 / MS_GPP - npages] += (grp_bytes(m, a, e) + MS_GRAN - 1) / MS_GRAN;
        }
    }
    int best = -1;
    for (unsigned i = 0; i < MS_XMAX; i++) {
        unsigned p = npages + i;
        if (xpage[i] && ok[i] == MS_GPP - pfree[p] && (best < 0 || pfree[p] > pfree[npages + best]))
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
            if (xpage[i] && (best < 0 || pfree[npages + i] > pfree[npages + best]))
                best = (int)i;
        if (best < 0 || npend > MS_DEFRAG_PEND - 16)
            return 0;
        unsigned p = npages + best, w0 = p * MS_GPP, w1 = w0 + MS_GPP;
        xret[best] = 1;
        for (int k = 0; k < MS_INST; k++) {
            MsInst *m = &ms[k];
            for (unsigned id = 0; id < it_n(m); id++) {
                unsigned a, e;
                if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                    continue;
                unsigned g0 = g_of(m->buf[a]);
                if (g0 < w0 || g0 >= w1)
                    continue;
                unsigned n = grp_bytes(m, a, e), ng = (n + MS_GRAN - 1) / MS_GRAN;
                int chain = grp_chain(m, id);
                xstat_drop_kb += n >> 10;
                hitch_drop(grp_cls(m, id) >= C_PREF, n);
                drop(k, id);
                if (chain && npend < MS_DEFRAG_PEND) {  /* the granules stay used until no chain reads them */
                    g_mark(g0, ng, 1);
                    pend[npend++] = (typeof(pend[0])){g0, ng, frame};
                }
            }
        }
        return 0;
    }
    unsigned p = npages + best, w0 = p * MS_GPP, w1 = w0 + MS_GPP;
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                continue;
            unsigned g0 = g_of(m->buf[a]);
            if (g0 >= w0 && g0 < w1) {
                xstat_drop_kb += grp_bytes(m, a, e) >> 10;
                hitch_drop(grp_cls(m, id) >= C_PREF, grp_bytes(m, a, e));
                drop(k, id);
            }
        }
    }
    if (pfree[p] != MS_GPP)  /* cannot happen (ok[] counted every used granule) */
        return 0;
    pfree[p] = 0;
    memset(&gmap[w0 / 32], 0xff, MS_GPP / 8);
    free(xpage[best]), xpage[best] = NULL, xret[best] = 0, nx--, xstat_back++;
    mapstream_stats.lent_kb = nx * (MS_PAGE >> 10);
    return 1;
}

/* Once per frame: borrow a page while the item pages are (nearly) full and the heap has room over the floor; give
 * one back while the heap is under it */
static void x_update(void)
{
    if (!MS_BORROW || !arena)
        return;
    unsigned hf = heap_free(), floor = x_want > MS_BORROW_FLOOR ? x_want : MS_BORROW_FLOOR;
    if (!nx || hf >= x_want)
        x_want = 0;
    if (nx && hf < floor) {  /* up to 4 pages a frame: back over the floor within a few frames */
        for (int i = 0; i < 4 && hf < floor && x_release(1); i++)
            hf = heap_free();
        return;
    }
    unsigned fr = 0;
    FOR_IPAGES(p)
        fr += pfree[p];
    if (nx >= MS_XMAX || fr >= 2 * MS_GPP || hf < MS_BORROW_FLOOR + MS_BORROW_HYST + MS_PAGE)
        return;
    for (unsigned j = 0; j < MS_XMAX; j++)
        if (xret[j])
            return;  /* one is being given back: none borrowed meanwhile */
    unsigned i = 0;
    while (xpage[i])
        i++;
    if (!(xpage[i] = memalign(MS_GRAN, MS_PAGE)))
        return;
    unsigned p = npages + i;
    memset(&gmap[p * MS_GPP / 32], 0, MS_GPP / 8);
    pfree[p] = MS_GPP;
    nx++, xstat_borrow++;
    mapstream_stats.lent_kb = nx * (MS_PAGE >> 10);
    nowin = C_TOP + 2;  /* new room: windows may be found again */
}

unsigned mapstream_heap_release(unsigned bytes)
{
    unsigned n = 0, want = (bytes + MS_PAGE - 1) / MS_PAGE + 1;
    while (n < want && x_release(0))
        n++;
    if (n < want && nx) {  /* the rest drains over the next frames (pages with drawn cells: x_release) */
        unsigned t = heap_free() + (want - n) * MS_PAGE;
        x_want = t > x_want ? t : x_want;
        x_release(1);
    }
    if (n)
        printf("mapstream: gave %u borrowed pages back to the heap for %u KB (%u left)\n", n, bytes >> 10, nx);
    return n;
}

/* ---- reads ---- */
typedef struct { int k; unsigned id, n, bytes, t; int tk, cxl; } MsRead;  /* cxl: cancelled, still landing */
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
static int room_fail, failed, evict_left, defrag_left, replan;
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
    if (w.cxl) {  /* cancelled (MS_CANCEL): the buffer is free once the read stopped */
        drop(w.k, w.id);
        return;
    }
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
        {  /* a near part's wait from the plan that wanted it to its landing (the reads queued and in
            * flight before it count); others (or one wanted before a settle) from the issue */
            float l = (float)(now - w.t) / 294912.0f;
            if (!it_res(m, w.id) && m->wq && m->wq[w.id] && !settling) {
                l = (float)(clk_ms + 1 - m->wq[w.id]);
                for (unsigned j = w.id; j < w.id + w.n; j++)
                    m->wq[j] = 0;
            }
            l = l > 5000.0f ? 5000.0f : l;
            lat_avg += (l - lat_avg) * 0.125f;
            lat_dev += (fabsf(l - lat_avg) - lat_dev) * 0.125f;
        }
        {
            unsigned l = m->ld ? m->ld[w.id] : 0;  /* read again within MS_THRASH_TICKS of its drop: thrash */
            hitch_load(l != 0, l >= 2 && (unsigned short)((frame >> 5) + 2 - l) <= MS_THRASH_TICKS, w.bytes);
        }
        for (unsigned j = w.id; j < w.id + w.n; j++) {
            if (m->ld)
                m->ld[j] = 1;
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
        /* MS_LEND: a lendable item goes to an idle pool page first, so the item pages stay for the near parts (in
         * the item pages first, coarse cells of classes above them took the room the near parts could not clear:
         * taurus near refusals 165 -> 876) */
        /* (never the sky or the far backdrop (MS_RES_FAR): dropped for a lite set they are read again from across the
         * disc, ~180 of 260 far seeks on world_route were Firelink's backdrop re-read while in the Burg) */
        /* (of the boot block, instance 0, only coarse cells out of view: its textures and in-view cells were dropped and
         * read again from 60-100 MB away while the player was in the Burg, B8b's per-read log 2026-11-09; with none of
         * it lent, taurus near refusals went back to the no-lending level, 376-474) */
        int lo = gb <= MS_PAGE && lendable(w.cls) &&
                 (w.k != 0 || (w.cls == C_FARC && !(it_res(m, g) && (m->rit[g - m->ncell].tag & MS_RES_TEX)))) &&
                 !(it_res(m, g) && (m->rit[g - m->ncell].tag & (MS_RES_SKY | MS_RES_FAR)));
        int nr = w.cls == C_NEAR || w.cls == C_NEAR2;  /* near parts: the item pages first, lent pages after */
        void *p = lo && !nr ? g_alloc_lend(gb) : NULL;
        if (p)
            lent_n++;
        else
            p = gb <= MS_PAGE ? g_alloc(gb) : g_alloc_top(gb, ilim());
        if (!p && lo && nr && lent_near + gb <= (MS_LEND_NEAR_KB << 10) && (p = g_alloc_lend(gb)))
            lent_n++, lent_near += gb;
        if (!p && sent)
            break;  /* the rest waits for its turn */
        if (!p && evict_left > 0) {
            evict_left--;
            /* one page (or the pages of a group over a page) of items below this one's class, not drawn this frame
             * or the last */
            if (make_window((gb + MS_PAGE - 1) / MS_PAGE, (gb + MS_GRAN - 1) / MS_GRAN, w.cls, NULL))
                p = gb <= MS_PAGE ? g_alloc(gb) : g_alloc_top(gb, ilim());
        }
        if (!p && !sent && MS_DEFRAG && gb <= MS_PAGE && nq < MS_QUEUE) {
            /* nothing to clear: empty a page by moving its groups (MS_DEFRAG); the read waits at the head of the queue
             * for the old copies to go (two draws), a few frames at most */
            int moving = npend && frame - pend_frame <= MS_DEFRAG_WAIT;
            if (!moving && defrag_left > 0) {
                defrag_left--;
                moving = defrag(1, ilim(), MS_DEFRAG_KB);
            }
            if (moving) {
                memmove(q + 1, q, nq++ * sizeof *q);
                q[0] = w;
                return 0;
            }
        }
        if (!p) {
            if (evict_left <= 0 && !sent && nq < MS_QUEUE) {  /* no clearing left this frame: first again next frame */
                memmove(q + 1, q, nq++ * sizeof *q);
                q[0] = w;
                return 0;
            }
            if (!room_fail++) {
                unsigned fr = 0;
                for (unsigned i = 0; i < ntot(); i++)
                    fr += pfree[i];
                printf("mapstream: no room for item %d.%u (%u KB, class %d, %.0f m): nothing of a lower class to drop; "
                       "%u KB free, items %u KB (resident %u, mip 0 %u), other %u KB, borrowed %u KB\n", w.k, w.id,
                       gb >> 10, w.cls, sqrtf(w.key), fr * MS_GRAN >> 10, mapstream_stats.bytes >> 10,
                       mapstream_stats.res_kb, mapstream_stats.tex_kb, mapstream_stats.other_kb, mapstream_stats.lent_kb);
            }
            mapstream_stats.room_fails++;
            hitch_room(w.cls == C_NEAR || w.cls == C_NEAR2, w.k, w.id);
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
        rd[nrd++] = (MsRead){w.k, g, e - g, gb, ticks(), tk, 0};
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
static void plan_(const float *pos);
static void plan(const float *pos)
{
    HITCH_BEGIN(p);
    plan_(pos);
    HITCH_END(p, HS_MSPLAN);
}
static void plan_(const float *pos)
{
    unsigned pt[8], np_ = 0;  /* section times (PLAN_PROF) */
    pt[np_++] = ticks();
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
        for (unsigned id = 0; id < it_n(m); id++) {
            float key = 0;
            int c = it_bytes(m, id) ? plan_item(k, m, id, pos, &key) : C_FREE;
            if (c > C_KEEP && vis_cut(m, id))
                c = C_KEEP;  /* no camera cell near the camera sees it (pvs.c) */
            if (MS_STABLE && m->state[id] != MS_NONE && m->state[id] != MS_FAILED)
                key *= MS_HOLD_K;  /* in (or on its way): ranks as if nearer, so the cut does not flip at its edge */
            m->cls[id] = (unsigned char)c;
            if (m->state[id] == MS_FAILED && retry)
                m->state[id] = MS_NONE;
            if (c >= C_PREF && nw < MS_WANTED)
                want[nw++] = (MsW){(unsigned char)k, (unsigned char)c, (unsigned short)id, key};
        }
    }
#if MS_ONE_LOD
    onelod_n = onelod_kb = 0;
    for (int k = 0; !settling && k < MS_INST; k++) {
        MsInst *m = &ms[k];
        for (unsigned j = 0; j < m->nres; j++) {
            unsigned tag = m->rit[j].tag, id = m->ncell + j, c = MS_RES_CELL(tag);
            if (!(tag & MS_RES_COARSE) || (tag & (MS_RES_SKY | MS_RES_TEX)) || c >= m->ncell || !near_in(m, c))
                continue;
            int cc = m->cls[id];
            /* (the whole box, not near_d2's cube: map.c draws the near level wherever the coarse level misses, and
             * by the cube ~0.4 MB more of the Burg's coarse level stayed in beside its near level at the Taurus
             * tower, where near parts at the feet then found no room; ISO 2026-11-08) */
            if (box_d2(&m->tab[c], pos) < sq(MS_COARSE_R)) {  /* the near level stands for it */
                if (m->cls[id] > C_KEEP)
                    m->cls[id] = C_KEEP;
                onelod_n++, onelod_kb += it_bytes(m, id) >> 10;
            } else if (m->state[id] == MS_READY) {
                continue;  /* back in: the near parts go by their own class */
            }
            /* the near parts are the cell's only level in memory: they keep the coarse level's class */
            for (unsigned p = c; p < m->ncell && (p == c || (m->tab[p].near & CELL_PART)); p++) {
                if (m->state[p] != MS_READY || m->cls[p] >= cc)
                    continue;
                if (m->cls[p] < C_PREF && cc >= C_PREF && nw < MS_WANTED)
                    want[nw++] = (MsW){(unsigned char)k, (unsigned char)cc, (unsigned short)p,
                                       near_d2(m, p, pos) * (MS_STABLE ? MS_HOLD_K : 1.0f)};
                m->cls[p] = (unsigned char)cc;
            }
        }
    }
    {  /* the classes of the wanted list after that */
        unsigned o = 0;
        for (unsigned i = 0; i < nw; i++)
            if ((want[i].cls = ms[want[i].k].cls[want[i].id]) >= C_PREF)
                want[o++] = want[i];
        nw = o;
    }
#endif
    pt[np_++] = ticks();
    qsort(want, nw, sizeof *want, w_cmp);
    /* what mapstream_alloc holds beyond its MS_OTHER top lies in item pages: the plan leaves it out */
    unsigned other = mapstream_stats.other_kb << 10, over = pools_on ? items_other : other > MS_OTHER ? other - MS_OTHER : 0;
    unsigned room = (ipages + nx) * MS_PAGE;  /* + the pages borrowed from the heap (MS_BORROW) */
    unsigned lend = 0;
    if (pools_on && MS_LEND) {  /* the pools' room their own blocks leave: only the lowest classes may use it (lendable) */
        unsigned own = other > items_other ? other - items_other : 0, pb = (npages - mpage0) * MS_PAGE;
        lend = pb > own ? pb - own : 0;
    }
    /* cap_hi: what the item pages hold, for every class; cap: + the lent room, which only lendable classes reach (with
     * one cap the near parts were planned into room they could never use: near refusals 165 -> 1117 on taurus) */
    unsigned cap_hi = room > over ? (room - over) / 100 * MS_FILL_PCT : 0, cap = cap_hi + lend / 100 * MS_FILL_PCT;
    {  /* near parts may use lent room up to MS_LEND_NEAR_KB */
        unsigned ln = lend < (MS_LEND_NEAR_KB << 10) ? lend : MS_LEND_NEAR_KB << 10;
        cap_hi += ln / 100 * MS_FILL_PCT;
    }
    unsigned sum = 0, sum_hi = 0;
    /* a planned near part not in yet: no lent room for the lendable classes this plan (their reads would queue beside
     * the near parts' and the near level wait longer: world_route pop-ins 10-13 -> 26-28 with lending under B3's
     * radii, 2026-11-09) */
    int near_missing = 0;
    pt[np_++] = ticks();
    for (int k = 0; MS_WHOLE_CELLS && k < MS_INST; k++)
        if (ms[k].cmark)
            memset(ms[k].cmark, 0, ms[k].ncell);
    unsigned cut = 0, cut_near = 0;  /* wanted items the cut left out (MS_STEADY: hitch_cut) */
    for (unsigned i = 0; i < nw; i++) {
        MsInst *m = &ms[want[i].k];
        unsigned id = want[i].id;
        if (MS_WHOLE_CELLS && id < m->ncell && m->cmark) {
            /* a cell's near level draws only with all its (seen) parts in: the parts go in together or not at all,
             * so the room is not spent on parts of cells that cannot be drawn (MS_WHOLE_CELLS) */
            unsigned h = id;
            while (h > 0 && (m->tab[h].near & CELL_PART))
                h--;
            if (!m->cmark[h]) {
                unsigned need = 0;
                for (unsigned p = h; p < m->ncell && (p == h || (m->tab[p].near & CELL_PART)); p++)
                    if (m->cls[p] >= C_PREF)
                        need += (it_bytes(m, p) + MS_GRAN - 1) & ~(MS_GRAN - 1);
                m->cmark[h] = sum + need > cap || sum_hi + need > cap_hi ? 2 : 1;  /* (near parts: never lent) */
                if (m->cmark[h] == 1) {
                    sum += need, sum_hi += need;
                    for (unsigned p = h; !near_missing && p < m->ncell && (p == h || (m->tab[p].near & CELL_PART)); p++)
                        near_missing = m->cls[p] >= C_PREF && m->state[p] != MS_READY;
                }
            }
            if (m->cmark[h] == 2) {
                m->cls[id] = C_KEEP;
                cut++, cut_near += want[i].cls == C_NEAR || want[i].cls == C_NEAR2;
                continue;
            }
        } else {
            unsigned b = (it_bytes(m, id) + MS_GRAN - 1) & ~(MS_GRAN - 1);
            int lo = lendable(want[i].cls) && want[i].cls != C_NEAR && want[i].cls != C_NEAR2;
            sum += b, sum_hi += lo ? 0 : b;  /* (counted when cut too, as before: past the cap, the rest is cut) */
            if (sum > cap || (!lo && sum_hi > cap_hi) || (lo && near_missing && sum > cap_hi)) {
                m->cls[id] = C_KEEP;
                cut++, cut_near += id < m->ncell && (want[i].cls == C_NEAR || want[i].cls == C_NEAR2);
                continue;
            }
        }
        if (m->state[want[i].id] == MS_NONE && !blocked(want[i].k, want[i].id))
            q_add(want[i].k, want[i].id, want[i].cls, want[i].key);
    }
    if (MS_STEADY && !settling)
        hitch_cut(cut, cut_near);
    pt[np_++] = ticks();
#if MS_CANCEL
    for (int i = 0; i < nrd; i++) {  /* reads in flight that nothing wants any more */
        MsInst *m = &ms[rd[i].k];
        if (rd[i].cxl)
            continue;
        int want = 0;
        for (unsigned j = rd[i].id; j < rd[i].id + rd[i].n; j++)
            want |= m->cls[j] >= C_PREF;
        if (want)
            continue;
        cancel_kb += rd[i].bytes >> 10;
        if (!pack_req_cancel_async(rd[i].tk)) {  /* in flight: it stops after its page; collect() frees it then */
            rd[i].cxl = 1;
            continue;
        }
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
                   "landed %u KB, cancelled %u KB; moved %u groups %u KB; one lod %u coarse items %u KB\n", cap >> 10, kb[C_PIN] + (C_PIN0 != C_PIN ? kb[C_PIN0] : 0),
                   kb[C_VIEW], kb[C_NEAR], kb[C_NEAR2], kb[C_VIEW2], kb[C_FARC], kb[C_PREF], stale_kb, cancel_kb,
                   defrag_moves, defrag_kb, onelod_n, onelod_kb);
            printf("mapstream: near wait %u ms (dev %u) from wanted to landed\n", (unsigned)lat_avg, (unsigned)lat_dev);
            /* the pinned coarse level by box distance of its cell (0-16-32-48-64 m, beyond), the backdrop and sky
             * apart, and what of it the horizon could stand for (MS_RES_LOD): sizing the pin radius (2026-11-08) */
            unsigned band[6] = {0}, lod[6] = {0}, far = 0, sky = 0;
            for (unsigned i = 0; i < nw; i++) {
                const MsInst *m = &ms[want[i].k];
                unsigned id = want[i].id;
                if (want[i].cls != C_PIN0 || id < m->ncell)
                    continue;
                unsigned tag = m->rit[id - m->ncell].tag, kb = it_bytes(m, id);
                if (tag & MS_RES_SKY) {
                    sky += kb;
                    continue;
                }
                if (tag & MS_RES_FAR) {
                    far += kb;
                    continue;
                }
                unsigned c = MS_RES_CELL(tag);
                if (c >= m->ncell)
                    continue;
                float d = sqrtf(box_d2(&m->tab[c], pos));
                int b = d < 16 ? 0 : d < 32 ? 1 : d < 48 ? 2 : d < 64 ? 3 : d < 96 ? 4 : 5;
                band[b] += kb;
                if (tag & MS_RES_LOD)
                    lod[b] += kb;
            }
            printf("mapstream: pin0 by distance KB (of it horizon-covered): 0-16 %u (%u) 16-32 %u (%u) 32-48 %u (%u) "
                   "48-64 %u (%u) 64-96 %u (%u) more %u (%u); backdrop %u sky %u\n", band[0] >> 10, lod[0] >> 10,
                   band[1] >> 10, lod[1] >> 10, band[2] >> 10, lod[2] >> 10, band[3] >> 10, lod[3] >> 10, band[4] >> 10,
                   lod[4] >> 10, band[5] >> 10, lod[5] >> 10, far >> 10, sky >> 10);
            /* the plan by height (Ame, 2026-11-09: "does the LOD take vertical elements into account"): KB of near
             * parts / coarse cells whose box (the near level's clipped to its cube) lies wholly more than 8 m below
             * the feet, within 8 m of them, or wholly more than 8 m above, of the classes at or above C_NEAR2 */
            unsigned vk_[2][3] = {{0}};
            for (unsigned i = 0; i < nw; i++) {
                const MsInst *m = &ms[want[i].k];
                unsigned id = want[i].id, c, nr = id < m->ncell;
                if (want[i].cls < C_NEAR2)
                    continue;
                if (nr)
                    c = id;
                else if ((c = MS_RES_CELL(m->rit[id - m->ncell].tag)) >= m->ncell ||
                         (m->rit[id - m->ncell].tag & (MS_RES_SKY | MS_RES_FAR | MS_RES_TEX)))
                    continue;
                float lo = m->tab[c].mn[1], hi = m->tab[c].mx[1];
                if (nr && m->keys) {
                    float k0 = m->keys[4 * c + 1] * CELL_GRID, k1 = k0 + CELL_GRID;
                    lo = lo > k0 ? lo : k0, hi = hi < k1 ? hi : k1;
                }
                vk_[!nr][hi < pos[1] - 8 ? 0 : lo > pos[1] + 8 ? 2 : 1] += it_bytes(m, id) >> 10;
            }
            printf("mapstream: plan by height KB (below 8 m / level / above 8 m): near %u / %u / %u, coarse %u / %u / %u\n",
                   vk_[0][0], vk_[0][1], vk_[0][2], vk_[1][0], vk_[1][1], vk_[1][2]);
        }
    }
    pt[np_++] = ticks();
    if (st_wait) {  /* after a settle: is the near level within MS_R0_FIRST of the feet in? */
        unsigned left = 0;
        for (int k = 0; k < MS_INST; k++) {
            const MsInst *m = &ms[k];
            for (unsigned c = 0; c < m->ncell; c++)
                if ((m->tab[c].near & ~CELL_PART) && !(m->tab[c].near & CELL_PART) && m->tab[c].bytes &&
                    near_d2(m, c, pos) < sq(MS_R0_FIRST) && !vis_cut(m, c) && !near_in(m, c))
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
    pt[np_++] = ticks();
    /* free what no class wants and no VIF chain reads (not asked for this frame or the last) */
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a;
            if (m->state[id] == MS_READY && (grp_of(m, id, &a), a == id) && grp_cls(m, id) == C_FREE &&
                !grp_chain(m, id))
                hitch_drop(0, 0), drop(k, id);
        }
    }
    for (int k = 0; !settling && k < MS_INST; k++) {  /* near parts the cut left wanted and not in: their wait starts */
        MsInst *m = &ms[k];
        for (unsigned id = 0; m->wq && id < m->ncell; id++)
            if (m->cls[id] < C_PREF || m->state[id] == MS_READY)
                m->wq[id] = 0;
            else if (!m->wq[id])
                m->wq[id] = clk_ms + 1;
    }
    pt[np_++] = ticks();
    if ((pt[np_ - 1] - pt[0]) / 294912 >= 40)  /* a slow plan: where it went (classify, sort, cut, cancel+log, settle check, free) */
        printf("mapstream: slow plan %u ms: %u %u %u %u %u %u ms, %u wanted\n", (pt[np_ - 1] - pt[0]) / 294912,
               (pt[1] - pt[0]) / 294912, (pt[2] - pt[1]) / 294912, (pt[3] - pt[2]) / 294912, (pt[4] - pt[3]) / 294912,
               (pt[5] - pt[4]) / 294912, (pt[6] - pt[5]) / 294912, nw);
}


void mapstream_update(const float pos[3])
{
#if defined MS_MEDIA_RADII && !defined MEDIA_CD
    static unsigned media;
    if (pack_disc_media() != media) {  /* the disc server looked at the disc: its radii (budget doc "Disc and read-ahead") */
        media = pack_disc_media();
        ms_r0_want = media == SNDM_CD ? MS_R0_WANT_CD : MS_R0_WANT_DVD;
        ms_r0_keep = media == SNDM_CD ? MS_R0_KEEP_CD : MS_R0_KEEP_DVD;
        printf("mapstream: media %u, near parts wanted to %.0f m, kept to %.0f m\n", media, ms_r0_want, ms_r0_keep);
    }
#endif
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
    x_update();
    {
        unsigned now = ticks(), dt = now - clk_last + clk_frac;  /* 294912 Count ticks per ms */
        clk_last = now;
        if (frame > 1)
            clk_ms += dt / 294912, clk_frac = dt % 294912;
    }
    int r;
    HITCH_BEGIN(io);
    for (int i = 0; i < nrd; i++)
        if (pack_req_done(rd[i].tk, &r))
            collect(i--, r);
    HITCH_END(io, HS_MSIO);
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
    vis_step(settling ? MS_VIS_SETTLE_US : MS_VIS_SLICE_US);  /* a row changed: the job re-plans once through (pvs.c) */
    float dx = pos[0] - scan_pos[0], dy = pos[1] - scan_pos[1], dz = pos[2] - scan_pos[2];
    if (replan || !scan_frame || frame - scan_frame >= MS_SCAN_FRAMES || dx * dx + dy * dy + dz * dz > sq(MS_SCAN_D)) {
        replan = 0;
        scan_frame = frame;
        memcpy(scan_pos, pos, sizeof scan_pos);
        plan(pos);
    }
    evict_left = MS_EVICT_PASSES;
    defrag_left = 1;
    pend_release();
    HITCH_BEGIN(is);
    while (!hold && nq && nrd < MS_INFLIGHT && issue())
        ;
    HITCH_END(is, HS_MSIO);
}

void mapstream_free_info(unsigned *free_kb, unsigned *run_kb, unsigned *other_kb)
{
    unsigned fr = 0, run = 0, best = 0;
    for (unsigned p = 0; pfree && p < npages; p++) {
        fr += pfree[p];
        run = pfree[p] == MS_GPP ? run + 1 : 0;
        best = run > best ? run : best;
    }
    for (unsigned p = npages; pfree && p < ntot(); p++)  /* borrowed pages: free room, never part of a run */
        fr += pfree[p];
    *free_kb = fr * MS_GRAN >> 10, *run_kb = best * (MS_PAGE >> 10), *other_kb = mapstream_stats.other_kb;
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
    m->tab = cells, m->ncell = n, m->keys = cell_keys(cells, n);
    m->state = calloc(n, 1), m->cls = calloc(n, 1);
    m->buf = calloc(n, sizeof *m->buf), m->used = calloc(n, sizeof *m->used);
    m->ld = calloc(n, sizeof *m->ld);  /* (NULL: no churn count) */
    m->wq = calloc(n, sizeof *m->wq);  /* (NULL: no wait measured) */
    m->cmark = calloc(n, 1);  /* (NULL: parts planned one by one) */
    m->rit = NULL, m->nres = 0, m->rgrp = NULL;
    if (!m->state || !m->cls || !m->buf || !m->used) {
        if (!m->borrowed)
            pack_close(&m->pk);
        free(m->state), free(m->cls), free(m->buf), free(m->used), free(m->ld), free(m->cmark), free(m->wq);
        memset(m, 0, sizeof *m);
        return 0;
    }
    replan = 1;
    printf("mapstream: %u cells, cells.bin %u KB at %u (instance %d)%s\n", n, m->total >> 10, m->base, k,
           m->keys ? ", near level by cell cube" : "");
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
    if (u) {
        m->used = u;
        unsigned short *l = realloc(m->ld, t * sizeof *l);
        if (l)
            memset(l + m->ncell, 0, nitems * sizeof *l);
        else
            free(m->ld);
        m->ld = l;
    }
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
    free(m->state), free(m->cls), free(m->buf), free(m->used), free(m->rit), free(m->rgrp), free(m->ld), free(m->cmark);
    free(m->wq);
    free(m->visv), free(m->vist);
    mapstream_stats.vis_items -= m->vish, mapstream_stats.vis_kb -= m->viskb;
    memset(m, 0, sizeof *m);
    if (vjob.k == k)
        vjob.k = -1;
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

void mapstream_pools(unsigned struct_bytes, unsigned misc_bytes)
{
    if (!arena)
        pool_req[0] = struct_bytes, pool_req[1] = misc_bytes;
}

unsigned mapstream_visual(void) { return arena ? ipages * MS_PAGE : 0; }
int mapstream_pools_on(void) { return pools_on; }
int mapstream_pools_on_build(void) { return MS_POOLS; }
int mapstream_pool_wait(void) { return pool_busy; }
unsigned mapstream_lend_early(void) { unsigned n = early_n; early_n = 0; return n; }

int mapstream_pool_prepare(int pool, unsigned n)
{
    if (!pools_on || !lent_n)
        return 1;
    unsigned p0 = pool == MS_POOL_STRUCT ? spage0 : mpage0, p1 = pool == MS_POOL_STRUCT ? npages : spage0;
    void *p = g_alloc_range(n, p0, p1);
    if (p) {
        g_free(p, n);
        return 1;
    }
    unsigned before = lent_n;
    if (pool == MS_POOL_STRUCT && MS_PREP_NOLEND)
        lend_struct_off = frame + MS_PREP_NOLEND;  /* nothing new lent there meanwhile */
    lend_reclaim_(p0, p1, 1);
    early_n += before - lent_n;
    return 0;
}

void *mapstream_alloc_pool(unsigned n, int pool)
{
    if (!mapstream_arena() || !pools_on)
        return NULL;
    unsigned p0 = pool == MS_POOL_STRUCT ? spage0 : mpage0, p1 = pool == MS_POOL_STRUCT ? npages : spage0;
    void *p = g_alloc_range(n, p0, p1);
    pool_busy = 0;
    if (!p && lent_n) {  /* MS_LEND: the lent items go */
        if (lend_reclaim(p0, p1) && !(p = g_alloc_range(n, p0, p1))) {
            /* some are in a chain: cells drawn every frame never leave it (a lite set waited 13.8 s behind them,
             * taurus 2026-11-09), so wait for VU1 once and drop them all */
            vu1_wait();
            chains_done = 1;
            pool_busy = lend_reclaim(p0, p1) > 0;
            chains_done = 0;
        }
        if (!p)
            p = g_alloc_range(n, p0, p1);
    }
    if (p)
        mapstream_stats.other_kb += (n + 1023) >> 10;
    return p;
}

/* the top of the item pages mapstream_alloc may clear (all pages without pools) */
static inline unsigned alloc_top(void) { return pools_on ? ipages : npages; }
static void *alloc_items(unsigned n)
{
    void *p = g_alloc_top(n, alloc_top());
    if (p && pools_on)
        items_other += (n + MS_GRAN - 1) / MS_GRAN * MS_GRAN;
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
        chains_done = 1;
        p = mapstream_alloc_pool(n, MS_POOL_MISC);
        chains_done = 0;
    }
    if (p)
        return p;
    p = alloc_items(n);
    unsigned need = (n + MS_PAGE - 1) / MS_PAGE, kb = 0;
    /* clear a window: of items below the near class first, then of anything but the pinned level, last of anything
     * (a lite set holds the collision the player walks onto next); never of items a VIF chain may read (in_chain) */
    if (!p && (make_window(need, 0, C_NEAR2, &kb) || make_window(need, 0, C_PIN, &kb) || make_window(need, 0, C_TOP + 1, &kb)))
        p = alloc_items(n);
    if (!p) {
        /* every window holds a drawn item: the lite set of the block the player walks onto next (983 KB of the
         * bonfire block's collision) found none for minutes and the player walked off the loaded floor (ISO
         * 2026-11-06). Once VU1 is idle no chain of an earlier frame reads the arena: clear the cheapest window, drawn
         * items too (they read back; a frame or two of coarse level or hole beats no floor) */
        vu1_wait();
        chains_done = 1;
        nowin = C_TOP + 2;
        if (make_window(need, 0, C_TOP + 1, &kb))
            p = alloc_items(n);
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
                p = alloc_items(n);
        }
        chains_done = 0;
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
    if (!p && make_window((n + MS_PAGE - 1) / MS_PAGE, 0, C_PREF, &kb))  /* only what no class wants */
        p = alloc_items(n);
    if (p)
        mapstream_stats.other_kb += (n + 1023) >> 10;
    else {
        /* the room is there but in holes (MS_DEFRAG): empty a window for the next call (world.c asks again) when
         * the arena's free granules cover it with MS_DEFRAG_KB to spare */
        unsigned fr = 0;
        for (unsigned i = 0; i < npages; i++)
            fr += pfree[i];
        int mv = MS_DEFRAG && !npend && fr * MS_GRAN >= n + (MS_DEFRAG_KB << 10) &&
                 defrag((n + MS_PAGE - 1) / MS_PAGE, alloc_top(), 3 * MS_DEFRAG_KB);
        printf("mapstream: soft alloc %u KB: no room beside the wanted cells%s (%u KB free)\n", n >> 10,
               mv ? ", window being emptied" : "", fr * MS_GRAN >> 10);
    }
    return p;
}

void mapstream_free(void *p, unsigned n)
{
    if (!p)
        return;
    if (pools_on && g_of(p) < ipages * MS_GPP)
        items_other -= (n + MS_GRAN - 1) / MS_GRAN * MS_GRAN;
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
                    near_d2(m, c, pos) < sq(MS_R0_DRAW))
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
