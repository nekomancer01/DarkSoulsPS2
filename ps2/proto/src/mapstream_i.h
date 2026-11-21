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
#include "msarena.h"
#include "residency.h"
#include "pack.h"
#include "dsrsnd_rpc.h"
#include "ot.h"  /* ot_wait: no VIF chain reads the arena any more */
#include "hitch.h"
#include "ticks.h"
#include "cvar.h"

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
#define MS_R_HZN ((float)RES_HZN_R)
#define MS_R_HZN_KEEP ((float)RES_HZN_R + 8.0f + 2 * MS_KEEP_MORE)
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
/* (the arena's sizes, MS_PAGE / MS_GRAN / MS_OTHER / MS_XMAX: msarena.h) */
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
/* MS_LEND_NEAR_ALL (default 1; -DMS_LEND_NEAR_ALL=0 = at most MS_LEND_NEAR_KB): near parts may take all of the pools'
 * idle room (what their own blocks' lite sets leave, the plan's "lend"), not only 900 KB of it. The long pop-ins at the
 * player's feet (27-80 s at 0-9 m, ISO 2026-11-16/17) were near parts of 31-63 KB refused with the item pages full
 * (~100 KB free in holes under 45 KB) while the misc and structural pools held 300-1100 KB idle, whole pages among it:
 * the near parts' lent room stood at 891-898 of its 900 KB at every such refusal (walls, taurus, report3) */
/* MS_DEFRAG_LEND (default 1; -DMS_DEFRAG_LEND=0 = item pages only): MS_DEFRAG moves a page's groups into the pools'
 * idle room as well when the item pages have no hole for them. With near parts lending the pools' room (above), the
 * refusals left were pinned coarse items of 24-64 KB (class 9) with ~150-200 KB free in the item pages in holes of at
 * most 19-23 KB, while the pools still held 160-450 KB idle (ISO iso_burg_walls 2026-11-17): the groups of a page
 * found no hole to move to, so no page could be emptied */
#ifndef MS_DEFRAG_LEND
#define MS_DEFRAG_LEND 1
#endif
#ifndef MS_LEND_NEAR_ALL
#define MS_LEND_NEAR_ALL 1
#endif
extern unsigned ms_lend_room;  /* the pools' idle room at the last plan (bytes) */
/* MS_LIFE (default 1, runtime switch: option set MS_LIFE 0 = any group in any page, the old placement): items that
 * stay long and items that go soon are kept in different pages (streaming research idea 5; Johnstone and Wilson, ISMM
 * 1998: fragmentation comes from size and lifetime mixes the allocator cannot tell apart). The pinned coarse level
 * (C_PIN0, C_PIN, C_VIEW at the read: kept while the player is within ~48 m) fills item pages from the top down, near
 * parts and read-ahead from the bottom up, small texture items pages of their own; one pinned 4 KB item in a page of
 * near parts kept that page from ever being whole again. A page takes its kind when its first group lands; a group
 * goes to a page of its own kind with a hole first, then to a whole free page while more than MS_TIDY_LO are whole,
 * then to any page with a hole (the old order), so the split never refuses what the old placement placed. (Small items
 * from the top and page-sized groups from the bottom, by size alone, was tried on 2026-11-12 and not kept: no-room
 * refusals 440 -> 515 on taurus; it took whole pages for small items even when the arena was tight.) */
/* (the MS_LIFE default is in msarena.h: the arena places by it) */
#define MS_SMALL_KB 8u  /* texture items up to this are MS_K_SMALL (mip 1 levels: 4 KB) */
/* MS_TIDY (default 1, runtime switch: option set MS_TIDY 0 = pages emptied only when a read waits for one): a reserve
 * of whole free item pages, kept by compaction on quiet frames (streaming research idea 4; GTA's PS2 heap,
 * re3 CMemoryHeap::TidyHeap: a block moves into the smallest free block that fits when that shrinks a hole). When fewer
 * than MS_TIDY_LO item pages are whole, the emptiest page's groups move into holes of the fullest pages (MS_DEFRAG's
 * move, its old copies wait for their chains), one page at a time, until MS_TIDY_HI are whole. A near part that
 * becomes wanted then finds a whole page at once instead of waiting MS_DEFRAG_WAIT frames at the head of the queue or
 * being refused. Never on a frame that planned or already moved groups for a read, never in a settle, at most one
 * page (<= 64 KB of EE memcpy, ~0.5 ms ASSUMED) a frame. */
#ifndef MS_TIDY
#define MS_TIDY cv_MS_TIDY  /* runtime switch, default 1 (src/cvar.h) */
#endif
/* WORLD_SOURCES (world_i.h, runtime switch): the other sources' items are planned too, by the target rule
 * (mapstream_sources, ms_settle_plan). 0: the player's spot alone */
#ifndef WORLD_SOURCES
#define WORLD_SOURCES cv_WORLD_SOURCES
#endif
#define MS_NSRC 3  /* sources besides the player at most (world.h WSRC_N - 1) */
extern float ms_src[MS_NSRC][3];
extern int ms_nsrc;
/* (MS_TIDY_LO / MS_TIDY_HI and the MS_K_* page kinds: msarena.h) */
/* MS_QSCALE (default 1, runtime switch: option set MS_QSCALE 0 = fixed radii): the soft tier's radii shrink while the
 * arena is under pressure (streaming research idea 12; UE5 Nanite r.Nanite.Streaming.QualityScale). Pressure: a "no
 * room" refusal since the last plan, or the item pages over MS_QS_FULL % used with nothing whole free. The scale (1 down
 * to MS_QS_MIN) then drops by MS_QS_DOWN a plan, and climbs back by MS_QS_UP a plan once neither holds. It shrinks only
 * what the player cannot see go: the near level's read-ahead beyond the draw radius (C_PREF, MS_R0_WANT), the coarse
 * level wanted out of view beyond the pinned ring (C_FARC) and foliage tufts past MS_QS_GRASS_MIN. The hard tier
 * (near level within the draw radius, pinned coarse ring, coarse in view, textures, the horizon radius) never moves:
 * Ame finds untextured geometry close to the player (the horizon standing in for a near cell) very jarring, so nothing
 * here can bring the horizon or a lower level nearer to the player. */
#ifndef MS_QSCALE
#define MS_QSCALE cv_MS_QSCALE  /* runtime switch, default 1 (src/cvar.h) */
#endif
#define MS_QS_MIN 0.3f
#define MS_QS_DOWN 0.1f
#define MS_QS_UP 0.05f
#define MS_QS_FULL 90u
#define MS_QS_GRASS_MIN 20.0f  /* m: foliage within this stays wanted at any scale */
extern float ms_qs;  /* the scale now (1 = full) */
#ifndef MS_LEND_NEAR_KB
#define MS_LEND_NEAR_KB 900
#endif
#ifndef MS_BORROW_FLOOR
#define MS_BORROW_FLOOR (2816u << 10)
#endif
#define MS_BORROW_HYST (256u << 10)         /* borrow only above floor + this (no borrow / give back each frame) */
#define MS_DEFRAG_PEND 64            /* old copies waiting for their chains */
#define MS_DEFRAG_KB 256u            /* most KB copied for a read (~2 ms of EE memcpy, ASSUMED); 3x for a relief window */
#define MS_DEFRAG_WAIT 8u            /* frames a read waits at the queue head for a page being emptied */
typedef struct { int k; unsigned id, n, bytes, t; int tk, cxl, cls; } MsRead;  /* cxl: cancelled, still landing; cls: the class it went out at (MS_SCAN) */
typedef struct { int k; unsigned id; int cls; float key; } MsQ;
typedef struct { int k, fresh; unsigned id, g, nh, kb; } MsVisJob;  /* the visibility refresh (msplan.c vis_step) */
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
extern unsigned ms_lent_n, ms_items_other;  /* (msevict.c) */
extern unsigned ms_lent_near;
void ms_tidy(void);
extern unsigned ms_tidy_moves, ms_tidy_kb;
void ms_drop(int k, unsigned id);
extern int ms_keep_buf;
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
int ms_defrag_(unsigned need, unsigned lim, unsigned max_kb, int pool);
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

/* the item pages now, all page slots (msarena.h) */
static inline unsigned ilim(void) { return msa_ilim(); }
static inline unsigned ntot(void) { return msa_ntot(); }
static inline int lendable(int cls)
{
    /* every class but the near parts (C_NEAR, C_NEAR2): a lent coarse cell dropped for a lite set shows its horizon
     * for a moment, a near part would pop. With only the classes below C_PIN (and C_VIEW) lendable, iso_ame_report3
     * drew the horizon inside HZN_R 99.5 % of the time (main 54 %; all classes lendable: 53 %) */
    return msa_pools_on() && cls > C_KEEP && cls <= C_VIEW2;
}


/* moved from the split files */

#ifndef MS_MIP0
#define MS_MIP0 0  /* no full-size map textures (plan_item); -DMS_MIP0=1: streamed as before 2026-11-12 */
#endif

/* the near level's distance (mapstream.h cell_near_d2: the box clipped to the cell's cube, MS_NEAR_CUBE) */
#define near_d2(m, c, p) cell_near_d2(&(m)->tab[c], (m)->keys ? (m)->keys + 4 * (c) : (const short *)0, p)

/* ---- visibility (src/pvs.c via map.c, 2026-11-06) ----
 * Cells are a spatial index: a cell 20 m away behind a wall still holds parts no camera nearby can see. The vis hook
 * says whether any chunk of an item is in the precomputed visibility of a camera cell within PVS_STREAM_R; an item
 * outside it for MS_VIS_HOLD ticks is planned no higher than C_KEEP: not read, not pinned, kept while there is room
 * (a turn back finds it in). The camera cell changes smoothly; the draw-group cut tried first (floor part, which
 * flips every 1-7 s at the Burg) churned. The design follows Vice City's streamer (re3 CStreaming: entities of
 * another area are neither requested nor kept). `make PVS=0` (-DNO_PVS): every item is planned by distance only. */
#define MS_VIS_HOLD 15u  /* ticks of 4 frames: 2 s, ASSUMED (a look through a doorway and back) */
/* The answers change whenever the camera crosses into another camera cell of a block's grid (every few seconds of
 * walking). Asking every item of all five blocks at once (~45k pvs ranges) cost 0.2-1.1 s in one frame: the walking
 * hitches of the 40 min Burg soak (ISO, 2026-11-07; tools/hitchsum.py). The refresh runs as a job instead, one block
 * at a time (only the block whose grid was crossed), MS_VIS_SLICE_US of EE time per frame (MS_VIS_SETTLE_US in a
 * settle); answers update in place as it goes and the plan is redone once a block is through. An item keeps its old
 * answer until the job reaches it: a few frames late, never wrong for longer. */
#define MS_VIS_SLICE_US 1500u
#define MS_VIS_SETTLE_US 20000u

/* One level per cell (built in: the frozen residency design, 2026-11-12): one level
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
#define MS_COARSE_R (MS_R0_KEEP - MS_KEEP_MORE - 8.0f)  /* (the near keep radius before MS_STEADY, less 8 m) */

/* MS_SCAN (default 1; 0 = strict class then distance order): urgent reads (classes C_NEAR2 and up: near parts within the
 * draw radius, the coarse level in view and pinned) go nearest first; the read-ahead classes below them (C_VIEW2,
 * C_FARC, C_PREF) go in disc order: the queued one at or after the end of the last read in the same pack first, then
 * the lowest offset of that pack (a circular scan), then another pack. The technique of the RW-era PS2 streamers (re3
 * CStreaming: priority requests first, the rest in image order from the head position): ordered by distance, B8b's
 * read log showed ~3100 reads of 1.2 items per 6-min world_route, every one a seek (~8/s, the gate is 2). */
#ifndef MS_SCAN
#define MS_SCAN 1
#endif
/* MS_RA_INFLIGHT (with MS_SCAN): reads of the read-ahead classes in flight at once. The disc server serves reads in
 * order, so a near part wanted at the feet waited behind up to MS_INFLIGHT read-ahead reads: Ame's ISO play
 * (2026-11-10) had cells drawn coarse while due near for up to 169 frames at 0 m with no room refusal. */
#ifndef MS_RA_INFLIGHT
#define MS_RA_INFLIGHT 2
#endif
/* MS_DEADLINE (default 1; 0 = the hard tier nearest first as before): deadline-aware read order, the map research's
 * idea 6 (2026-11-17) after O3DE's IO scheduler (Scheduler::Thread_PrioritizeRequests: a request whose estimated
 * completion falls after its deadline "panics" and goes first, by deadline; the rest go by the shortest seek from the
 * last read's end). Here a hard-tier item's deadline is the time the player needs to come within MS_DL_NEAR_R of a
 * near part (MS_DL_COARSE_R of a coarse item) at the smoothed walking speed (at least MS_DL_VMIN), and its estimated
 * completion is its place in the disc-order scan of the queued hard tier, plus the reads in flight, times the measured
 * time a read takes (rd_ms). Nearest first, every hard read was a seek of its own (B8b: ~8 seeks a second on
 * world_route, ~1.2 items a run); in disc order the ones not late sweep the pack once. A settle keeps its class order. */
#ifndef MS_DEADLINE
#define MS_DEADLINE 1
#endif
#define MS_DL_NEAR_R 8.0f    /* m (ASSUMED: a cell drawn coarse within ~8 m is plain to see; the near level draws to 20 m) */
#define MS_DL_COARSE_R MS_R0_DRAW  /* m: a coarse item stands in from the near level's draw radius out */
#define MS_DL_VMIN 2.0f      /* m/s (ASSUMED: a standing player can be walking within the read; DSR's walk ~1.5-2 m/s) */
#define MS_DL_FPS 30.0f      /* mapstream_update calls a second (vel is per call) */
/* MS_DL_FIRST (default 1, with MS_DEADLINE): a panicking item already late (inside its radius) with two or more reads
 * ahead of it in the disc server goes out ahead of them (pack_req_read_first, as coltile.c's tiles under the feet):
 * the server sweeps its queue by LSN, so a late near part behind the head (or in another pack) could wait for all
 * MS_INFLIGHT reads. One ahead of the head in the same pack goes the normal way: the sweep passes it. */
#ifndef MS_DL_FIRST
#define MS_DL_FIRST 1
#endif
/* MS_GAP_KB (default 96; 0 = off): map reads through small gaps, the research's idea 11, on top of dsrsnd's gap reads
 * (PR #235, GAP_KB 96: a background read at most 96 KB ahead of the head reads the blocks between into the IOP cache
 * instead of seeking). That only pays when the next map read is that close ahead: a queued item that starts within
 * MS_GAP_KB after the end of the last read in the same pack goes next, before the scan's other picks (read-ahead
 * classes too, within MS_RA_INFLIGHT), unless an item panics. ASSUMED 96 KB: a DVD seek of ~100-150 ms is worth
 * ~200-300 KB of reading; matched to GAP_KB so the IOP reads the gap through. */
#ifndef MS_GAP_KB
#define MS_GAP_KB 96
#endif
/* Read order (2026-11-12): the classes rank what stays in memory; the reads of the hard tier (C_NEAR2 and up: the near
 * level within the draw radius, the pinned coarse level, coarse cells in view) go out nearest first whatever their
 * class. By class, a block turning on read its whole pinned coarse ring out to 40 m (C_PIN0, hundreds of small items)
 * before the first near part at the feet: 30-47 near parts within 20 m waited ~20 s with the reader busy (ISO
 * iso_burg_taurus, "flow" log). The classes below keep their order after them. -DMS_READ_BY_CLASS=1: by class as
 * before; a settle too (its plan has its own classes). */
#ifndef MS_READ_BY_CLASS
#define MS_READ_BY_CLASS 0
#endif

/* Burg plan B8b: why each run of resident items stopped, and how the reads it made sit on the disc ("mapstream: runs"
 * line every MS_RUNLOG_FRAMES frames): a read that starts where the last one ended needs no seek. B4's ISO world_route,
 * 2026-11-09: 3089 runs of 1.2 items in 6 min, 170 contiguous; most stops are the next item not wanted (class) or
 * already in (state). Reading the kept neighbours along (up to 64 KB) took 388 more items but no fewer runs, and more
 * "no room" refusals (97 -> 120): dropped */
enum { RS_END, RS_STATE, RS_GAP, RS_BASE, RS_CAP, RS_TEX, RS_CLASS, RS_BLOCKED, RS_N };
static const char *const rs_name[RS_N] = {"end", "state", "gap", "base", "cap", "tex", "class", "blocked"};
/* per instance (world block): reads, and of them far backdrop / coarse / sky / texture / mip 1 / other items */
enum { RK_FAR, RK_COARSE, RK_SKY, RK_TEX, RK_MIP1, RK_OTHER, RK_N };
#define MS_RUNLOG_FRAMES 300


/* Why the near level is late (the "flow" line, every MS_FLOW_FRAMES outside settles): frames the reader was held for
 * a lite set with reads queued, frames with every read slot busy, frames with nothing queued, and the near cells
 * within the draw radius that are not in, by what keeps them out: the plan's cut (no room by its count), a refusal
 * (blocked after "no room"), the visibility cut, or simply the disc (queued / reading). */
#define MS_FLOW_FRAMES 60u
/* WORLD_SEAM_BUNDLE (Makefile, default 1; TECH_DEBT world-16): a block pack holds the entries world.c reads to turn it
 * on first and in read order, so a lite set is one run of the disc, and no map read goes out while it loads: a map read
 * in between took the head away from that run and back (two seeks per read). ISO world_route (2026-11-17): block turn-on
 * median 1.86 -> 1.0-1.14 s, far seeks 483 / 472 -> 394 / 361. 0: the old pack order, map reads go on MS_HOLD_INFLIGHT
 * at a time (the lite set was spread over its pack: four far seeks per turn-on whatever the map reader did) */
#ifndef WORLD_SEAM_BUNDLE
#define WORLD_SEAM_BUNDLE 1
#endif
#if WORLD_SEAM_BUNDLE
#define MS_HOLD_DEFAULT 0
#else
#define MS_HOLD_DEFAULT 3  /* ASSUMED */
#endif
#ifndef MS_HOLD_INFLIGHT
#define MS_HOLD_INFLIGHT MS_HOLD_DEFAULT  /* map reads in flight while a lite set loads */
#endif

/* MS_ORPHAN (default on; 0 = mapstream_close waits for its reads in flight as before) */
#ifndef MS_ORPHAN
#define MS_ORPHAN 1
#endif
typedef struct { int tk; void *buf; unsigned bytes; } Orphan;
/* MS_LEND_REHOME (default on; 0 = the old drop): a lent near group a lite set needs the room of moves into the item
 * pages instead of going: dropped, the cell had nothing to draw until it was read again (its coarse level had made
 * way for it), and stairs and floors vanished for a moment at every block turn-on (Ame's report 2026-11-17 21:31,
 * t 75.9 / 97.3 / 114.3: each right after "lent near dropped for a lite set"). Room: a hole in the item pages, else
 * a page cleared of groups below the near classes. Called only for groups no chain reads (the old copy frees now).
 * 1 when moved. */
#ifndef MS_LEND_REHOME
#define MS_LEND_REHOME 1
#endif

/* Lent items go only where the room is needed (2026-11-12): n bytes of whole pool pages in [p0, p1), the cheapest run
 * of pages that hold nothing but lent items and free granules (fewest item bytes; a run a chain still reads only when
 * no other does). lend_reclaim dropped every lent item of the pool whenever one allocation found no hole, a 5 KB
 * collision tile included: ~250 of ~600 items read again within 30 s of their drop on the ISO Taurus route had gone
 * that way (the "thrash" log), most of them textures. Returns the items of the run a chain may still read (they stay:
 * the caller waits for VU1 and asks again); *found 0: no such run, nothing dropped. */
#define MS_POOL_PAGES_MAX 160
/* the lent items in the way of n bytes of [p0, p1): the cheapest run's (MS_LEND_ALL_DROP 0, the default), else all */
#ifndef MS_LEND_ALL_DROP
#define MS_LEND_ALL_DROP 0
#endif

extern MapStreamStats mapstream_stats;
extern MsInst ms_inst[MS_INST];
extern unsigned ms_frame;
extern unsigned ms_dcur, ms_dprev;
extern int ms_chains_done;
extern int ms_focus, ms_hold;
extern int ms_settling;
extern float ms_ahead[3], ms_vel[3], ms_last_pos[3];
extern int ms_ahead_on;
extern float ms_lat_avg, ms_lat_dev;
extern unsigned (*ms_tex_age)(int k, unsigned tag);
extern void (*ms_tex_set)(int k, unsigned tag, const void *p);
extern int (*ms_vis_hook)(int k, unsigned id);
extern unsigned (*ms_vis_gen)(int k);
extern MsVisJob ms_vjob;
extern int ms_replan;
void ms_vis_step(unsigned us);
int ms_near_in(const MsInst *m, unsigned c);
float ms_qs_now(void);
extern MsRead ms_rd[MS_INFLIGHT];
extern int ms_nrd;
extern MsQ ms_q[MS_QUEUE];
extern int ms_nq;
extern MsQ ms_blk[MS_BLOCKED];
extern int ms_nblk;
extern unsigned ms_scan_frame, ms_fail_frame, ms_fail_wait;
extern int ms_room_fail, ms_failed, ms_evict_left, ms_defrag_left;
extern unsigned ms_clk_ms, ms_clk_last, ms_clk_frac, ms_st_t0, ms_st_t1, ms_st_first, ms_st_wait, ms_stale_kb;
extern unsigned ms_cancel_kb;
void ms_q_add(int k, unsigned id, int cls, float key);
int ms_blocked(int k, unsigned id);
void ms_collect(int i, int r);
int ms_issue(void);
extern MsW ms_want[MS_WANTED];
void ms_plan(const float *pos);
void ms_flow_log(const float *pos);
extern int ms_why;
extern int ms_keep_buf;
void ms_drop(int k, unsigned id);
int ms_grp_cls(const MsInst *m, unsigned id);
int ms_grp_chain(const MsInst *m, unsigned id);
extern int ms_nowin;
int ms_make_window(unsigned need, unsigned ng_need, int below, unsigned *kb);
extern int ms_npend;
extern unsigned ms_pend_frame;
extern unsigned ms_defrag_moves, ms_defrag_kb;
void ms_pend_release(void);
int ms_defrag(unsigned need, unsigned lim, unsigned max_kb);
void ms_tidy(void);
void ms_x_update(void);
/* ---- items ---- */
static inline float box_d2(const MapCell *c, const float *p)
{
    float d2 = 0;
    for (int k = 0; k < 3; k++) {
        float o = p[k] < c->mn[k] ? c->mn[k] - p[k] : p[k] > c->mx[k] ? p[k] - c->mx[k] : 0;
        d2 += o * o;
    }
    return d2;
}
/* outside the visibility now */
static inline int vis_off(const MsInst *m, unsigned id) { return m->visv && id < m->visn && !m->visv[id]; }
/* out of the plan: outside the visibility now and for MS_VIS_HOLD ticks */
static inline int vis_cut(const MsInst *m, unsigned id)
{
    return vis_off(m, id) && (unsigned short)((unsigned short)(ms_frame >> 2) - m->vist[id]) > MS_VIS_HOLD;
}

static inline float sq(float x) { return x * x; }
/* the target rule (Polaris 4.3): a place being loaded rather than walked in (a settle's spot, a respawn point while a
 * death plays) is planned with the hard tier it is waited for in (plan_item: the near level within the draw radius,
 * no read-ahead or foliage, the horizon standing in from MS_SETTLE_HZN_D). The read order (q_rank, q_scan_pick) is a
 * settle's (by class) only while a settle runs: a source's items queue with the player's in the walking order */
extern int ms_target;  /* plan_item is classifying for a target source (msplan.c) */
static inline int ms_settle_plan(void) { return ms_settling || ms_target; }
static inline int q_rank(int cls) { return !MS_READ_BY_CLASS && !ms_settle_plan() && cls >= C_NEAR2 ? C_TOP : cls; }

#endif
