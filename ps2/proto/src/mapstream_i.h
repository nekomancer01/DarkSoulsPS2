/* mapstream's own shared state (mapstream.c: plan, reads, API; msarena.c: the arena). Not for other modules:
 * they use mapstream.h. */
#ifndef MAPSTREAM_I_H
#define MAPSTREAM_I_H
#include <kernel.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mapstream.h"
#include "residency.h"
#include "pack.h"
#include "dsrsnd_rpc.h"
#include "vu1.h"
#include "hitch.h"
#include "ticks.h"

/* Radii (box distance, m). Near: map.c draws a cell's near level within NEAR_DRAW_R + NEAR_HYST = 26 m; read 4 m
 * ahead (~1 s of running, ASSUMED), kept 6 m more against reading again on a turn back. Coarse: pinned within
 * MS_R1_PIN (export_map.WS_R1: the spot budget covers it), wanted out to MS_R1_WANT (map.c CULL_FAR 250 m + a box
 * margin), kept to MS_R1_KEEP. */
/* (Burg plan B3, 2026-11-08: the export's hard tier holds the near level within 20 m, the coarse level within 40 m:
 * the radii follow it; 26 / 30 / 36 / 64 m before) */
#define MS_R0_DRAW ((float)RES_NEAR_R)  /* residency.h: the hard tier's near radius */
/* read ahead out to MS_R0_WANT (export_map.WS_R0 budgets 30 m; the plan cut keeps the extra within the arena, C_PREF
 * is the lowest wanted class): at 30 m, 4 m past the draw radius, cells inside it were still coarse when the player
 * walked in (pop-in, ISO 2026-11-06) */
/* (2026-11-08) 30 / 36 m: the project's DVD-9 budget (draw radius + sprint speed x worst DVD read latency; a CD
 * would need 35 / 41). 40 / 46 m (MS_NEAR_FIRST's read-ahead since 2026-11-06) covered ~1.8x the ground of 30 m.
 * make DEFS=-DMS_R0_WANT=40 -DMS_R0_KEEP=46 = the old radii */
/* Per media (budget doc proposal 2): the DVD radii on a DVD (and host: runs), 35 / 41 m on a CD (the read with a seek
 * takes up to 1 s there); the disc server reports the media once it has looked at the disc (pack_disc_media, checked
 * in mapstream_update). MEDIA=cd builds (MEDIA_CD) take the CD radii whatever the disc. DEFS radii stay fixed. */
/* Steady plan (Burg plan B4; built in: the frozen residency design, 2026-11-12): with the export
 * sized to the visual room the plan has nothing to cut, so what is in may stay in. Keep radii rise (a kept item is the
 * first to go when a wanted one needs room, so keeping costs nothing but saves a read when the player turns back: ~25 %
 * of the map KB of a Burg soak was read again within 30 s, world-14); an item that is in (or being read) is measured
 * MS_CLS_HYST m nearer for its class, so a player standing on a class edge does not flip it every plan; and a plan that
 * has to cut a wanted item says so (hitch_cut, the "stream:" line's plan cut: a logged bug, gated by tools/budget.py). */
#define MS_CLS_HYST 2.0f  /* m (ASSUMED: the near switch's own NEAR_HYST in map.c) */
#define MS_KEEP_MORE 10.0f  /* m added to the near, horizon and foliage keep radii */
#define MS_R0_WANT_DVD 24.0f
#define MS_R0_KEEP_DVD (30.0f + MS_KEEP_MORE)
#define MS_R0_WANT_CD 29.0f
#define MS_R0_KEEP_CD (35.0f + MS_KEEP_MORE)
#ifndef MS_R0_WANT
#ifdef MEDIA_CD
extern float ms_r0_want, ms_r0_keep;
#else
extern float ms_r0_want, ms_r0_keep;
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
#define MS_R1_PIN ((float)RES_PIN_R)   /* pinned coarse: residency.h, the hard tier (the ring to MS_R_HZN ranks below the near level) */
#define MS_R0_FIRST 12.0f /* near parts this close rank above coarse cells in view (C_NEAR), the rest of 20 m below (C_NEAR2) */
/* Near level first (built in: the frozen residency design, 2026-11-12): the near parts within MS_R0_FIRST rank
 * above the pinned coarse level out of view (C_PIN0 within MS_R1_FIRST and the sky / backdrop of the player's block >
 * C_VIEW coarse in view > C_NEAR > C_PIN coarse out of view). With every pinned coarse cell of all blocks within 64 m and every one in view within 128 m ahead of
 * them, the near level around the player waited behind megabytes of far coarse cells after each spawn (user report,
 * 2026-11-06). The coarse cells close by still go first: a missing one is a hole at the player's feet. */
#define MS_R1_FIRST 32.0f
/* Cancelled reads (built in: the frozen residency design, 2026-11-12): a read in flight whose items all fell below C_PREF on a re-plan (the player moved
 * on or warped, a settle began) is cancelled and its pages freed (pack_req_cancel stops it after its current 64 KB
 * page). Without it up to MS_INFLIGHT x MS_RUN = 2 MB of stale reads went on landing ahead of what the new spot
 * needs. */
/* Settle with the near level (built in: the frozen residency design, 2026-11-12): a settle (DSR's loading screen after a warp or
 * spawn) also reads the near parts within MS_R0_DRAW of the feet and waits for them (mapstream_busy), so the player
 * starts with the near level drawn instead of watching it stream in over the coarse one (Ame, 2026-11-06). world.c
 * still ends a settle after its time limit. */
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
/* (Makefile BURG_HZN_NEAR, 2026-11-10, Ame's pick: map.c HZN_R 32 and MS_R_HZN 40: the horizon stands in from 32 m) */
#ifndef MS_R_HZN
#define MS_R_HZN ((float)RES_HZN_R)
#endif
#define MS_R_HZN_KEEP (MS_R_HZN + 8.0f + 2 * MS_KEEP_MORE)
/* Stable ranks (built in: the frozen residency design, 2026-11-12): distance-only ranks inside MS_R1_PIN and a bonus for what is in
 * (MS_HOLD_K x the squared distance, ~16 % nearer), so what fits stays and what does not fit stays out */
#define MS_HOLD_K 0.7f
#define MS_THRASH_TICKS 28u  /* hitch.c: a read again within ~30 s of its drop counts as thrash (ticks of 32 frames) */
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
#define MS_TEX_KEEP 600u  /* (MS_STEADY: 20 s, a mip 0 asked for again after a look around) */
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
enum { C_FREE, C_KEEP, C_PREF, C_FARC, C_VIEW2, C_NEAR2, C_PIN, C_NEAR, C_VIEW, C_PIN0 };
#define C_TOP C_PIN0
/* the class of a streamed mip 1 asked for lately (mapsplit.py MIP1_OUT). C_VIEW2 (the coarse cells in view) took
 * near-class "no room" refusals 12 -> 145 on iso_ame_report3 (one run, 2026-11-08): the 4 KB items spread over the
 * pages the near parts needed. C_PREF: as mip 0, after every near part */
#ifndef MS_MIP1_CLS
#define MS_MIP1_CLS C_PREF
#endif


typedef struct {
    Pack pk;
    int borrowed;
    unsigned base, total;  /* cells.bin in the pack */
    const MapCell *tab;
    const short *keys;  /* cell keys (mapstream.h cell_keys), NULL: none */
    unsigned ncell;
    MapResItem *rit;
    unsigned nres;
    /* per item (ids [0, ncell) near entries, [ncell, ncell + nres) resident items) */
    unsigned char *state, *cls;
    void **buf;
    unsigned *used;        /* frame map.c last asked for it */
    unsigned short *rgrp;  /* resident item: first item (index into rit) of the group it was read with */
    unsigned char *dw;     /* per item: why it was last dropped (MS_WHY_*) | its class then << 4 */
    unsigned short *ld;    /* per item (hitch.c churn): 0 never read, 1 in, else the tick (frame / 32) + 2 it was dropped */
    unsigned *wq;          /* per near part (near wait log): clk_ms + 1 of the plan that first wanted it while not in, 0 */
    unsigned char *cmark;  /* per cell, during the plan's cut: 1 its near parts are in the plan, 2 they are not */
    /* visibility (pvs.c): per item 1 while any of its chunks is seen, the tick (frame / 4) it was last seen; the
     * vis_gen value they follow, items they cover */
    unsigned char *visv;
    unsigned short *vist;
    unsigned visg, visn, vish, viskb;  /* + items outside it now, their KB */
} MsInst;
#ifndef MS_LEND_NEAR_KB
#define MS_LEND_NEAR_KB 900
#endif
#define MS_XMAX 96                          /* pages that may be borrowed (6 MB) */
#ifndef MS_BORROW_FLOOR
#define MS_BORROW_FLOOR (2816u << 10)
#endif
#define MS_BORROW_HYST (256u << 10)         /* borrow only above floor + this (no borrow / give back each frame) */
#define FOR_IPAGES(p) for (unsigned p##_i = 0, p; p##_i < ilim() + MS_XMAX && (p = p##_i < ilim() ? p##_i : ms_npages + p##_i - ilim(), 1); p##_i++)
#define MS_DEFRAG_PEND 64            /* old copies waiting for their chains */
#define MS_DEFRAG_KB 256u            /* most KB copied for a read (~2 ms of EE memcpy, ASSUMED); 3x for a relief window */
#define MS_DEFRAG_WAIT 8u            /* frames a read waits at the queue head for a page being emptied */
typedef struct { int k; unsigned id, n, bytes, t; int tk, cxl, cls; } MsRead;  /* cxl: cancelled, still landing; cls: the class it went out at (MS_SCAN) */
typedef struct { int k; unsigned id; int cls; float key; } MsQ;
#define MS_WANTED 4096  /* wanted items of all blocks at once (~2000 seen at the Burg entry); 32 KB */
#define MS_FILL_PCT ((unsigned)RES_FILL_PCT)  /* residency.h */
typedef struct { unsigned char k, cls; unsigned short id; float key; } MsW;
extern MsInst ms_inst[MS_INST];
extern unsigned ms_frame;
extern unsigned ms_dcur, ms_dprev;
extern int ms_chains_done;
extern int ms_settling;
extern void (*ms_tex_set)(int k, unsigned tag, const void *p);
extern int ms_replan;
extern MsRead ms_rd[MS_INFLIGHT];
extern int ms_nrd;
extern int ms_nblk;
extern unsigned ms_cancel_kb;
extern MsW ms_want[MS_WANTED];
extern unsigned char *ms_arena;
extern unsigned ms_npages, ms_ipages;
extern unsigned short *ms_pfree;
extern unsigned ms_mpage0, ms_items_other;
extern int ms_pools_on;
extern unsigned ms_lent_n;
extern unsigned ms_lent_near;
extern unsigned ms_nx;
void *ms_g_alloc(unsigned n);
void *ms_g_alloc_top(unsigned n, unsigned top);
void *ms_g_alloc_lend(unsigned n);
void ms_g_free(void *ptr, unsigned n);
int ms_ensure_arena(void);
void ms_drop(int k, unsigned id);
/* why an item was dropped: the plan freed it (class C_FREE), a read / allocation cleared its page, a lite set took
 * its lent pool page, a borrowed heap page went back, its read was cancelled */
enum { MS_WHY_FREE, MS_WHY_WINDOW, MS_WHY_LENT, MS_WHY_BORROW, MS_WHY_CANCEL, MS_WHY_N };
extern int ms_why;
int ms_grp_cls(const MsInst *m, unsigned id);
int ms_grp_chain(const MsInst *m, unsigned id);
extern int ms_nowin;
int ms_make_window(unsigned need, unsigned ng_need, int below, unsigned *kb);
extern int ms_npend;
extern unsigned ms_pend_frame;
extern unsigned ms_defrag_moves, ms_defrag_kb;
void ms_pend_release(void);
int ms_defrag(unsigned need, unsigned lim, unsigned max_kb);
void ms_x_update(void);

static inline int in_chain(unsigned used)
{
    if (ms_chains_done)
        return 0;
    return ms_dprev && (int)(used - ms_dprev) >= 0 && (int)(used - ms_dcur) <= 0;
}
static inline void drawn(void)
{
    if (ms_frame != ms_dcur)
        ms_dprev = ms_dcur, ms_dcur = ms_frame;
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
static inline unsigned grp_of(const MsInst *m, unsigned id, unsigned *first)
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
static inline unsigned grp_bytes(const MsInst *m, unsigned a, unsigned e)
{
    if (!it_res(m, a))
        return it_bytes(m, a);
    const MapResItem *r = m->rit;
    return r[e - 1 - m->ncell].rel + r[e - 1 - m->ncell].bytes - r[a - m->ncell].rel;
}

static inline unsigned ilim(void)
{
    if (ms_pools_on)
        return ms_ipages;
    return (mapstream_stats.other_kb << 10) > MS_OTHER ? ms_npages : ms_ipages;
}
static inline unsigned ntot(void) { return ms_npages + MS_XMAX; }
static inline int lendable(int cls)
{
    /* every class but the near parts (C_NEAR, C_NEAR2): a lent coarse cell dropped for a lite set shows its horizon
     * for a moment, a near part would pop. With only the classes below C_PIN (and C_VIEW) lendable, iso_ame_report3
     * drew the horizon inside HZN_R 99.5 % of the time (main 54 %; all classes lendable: 53 %) */
    return ms_pools_on && cls > C_KEEP && cls <= C_VIEW2;
}

#endif
