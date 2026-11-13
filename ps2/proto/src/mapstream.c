/* World streaming v2 (2026-11-06): map cells by a residency plan, the API of mapstream.h.
 *
 * The first streamer (v1, removed 2026-11-10; git history has src/mapstream_v1.c) ranked every item by distance times a dozen factors and
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
 * top, clearing the cheapest run first.
 *
 * Files (split 2026-11-12, code moved as it was): this one plans (classes, the cut), reads (queue, issue, landing) and
 * holds the open / close / settle API; msarena.c is the arena (granules, pools, borrowed pages, windows, defrag,
 * mapstream_alloc); mapstream_i.h their shared state (names ms_*), the radii and the item helpers. The numbers the
 * export shares live in residency.h. */
#include "mapstream_i.h"
#include "world.h" /* world_current, world_pack: the neighbour copies (MS_COPIES) */

/* MS_BEHIND_KEEP (default 1, Ame's pick 2026-11-10; 0 = read as before): a horizon-covered coarse item (MS_RES_LOD)
 * out of view and at least MS_BEHIND_R away is kept while it is in but never read: behind the camera nothing draws it,
 * and turning round shows the cell's horizon until it lands (map.c draws a missing coarse cell's horizon). At the Burg
 * spots 96 % of the pinned coarse level 16-48 m was horizon-covered and 270-410 KB of a plan was coarse out of view. */
#ifndef MS_BEHIND_KEEP
#define MS_BEHIND_KEEP 1
#endif
#ifndef MS_BEHIND_R
#define MS_BEHIND_R 32.0f
#endif

#ifndef MS_MIP0
#define MS_MIP0 1  /* 0: no full-size map textures (plan_item) */
#endif

#ifdef MS_MEDIA_RADII  /* the media radii (mapstream_i.h) */
#ifdef MEDIA_CD
float ms_r0_want = MS_R0_WANT_CD, ms_r0_keep = MS_R0_KEEP_CD;
#else
float ms_r0_want = MS_R0_WANT_DVD, ms_r0_keep = MS_R0_KEEP_DVD;
#endif
#endif

MapStreamStats mapstream_stats;
MsInst ms_inst[MS_INST];
static int sel;  /* the instance mapstream_get / mapstream_res answer for */
unsigned ms_frame;
/* the frames of the last two map draws (mapstream_get / mapstream_res calls). An item handed out in either may be read
 * by a VIF chain that is built or still running: it is never freed (a warp settle advances `frame` many times without
 * a draw, and #110 freed cells of the last drawn frame's chain there: hang after a spawn, 2026-11-06) */
unsigned ms_dcur, ms_dprev;
/* (an item that landed after the last draw has used > dcur: no chain has it. Counting those made every item read in
 * a settle undroppable: the near level filled the arena and the next block's lite set found no window, settle at its
 * 30 s cap, ISO 2026-11-06) */
/* mapstream_alloc after vu1_wait: no chain reads the arena (map.c builds the map chain in map_draw and main.c kicks it
 * right after; no allocation runs in between) */
int ms_chains_done;
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

static int focus = -1, hold;
int ms_settling;
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
void (*ms_tex_set)(int k, unsigned tag, const void *p);

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
    MsInst *m = &ms_inst[k];
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

int ms_replan;
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
                if (it_n(&ms_inst[k]) && (ms_inst[k].visn != it_n(&ms_inst[k]) || ms_inst[k].visg != vis_gen(k)))
                    vis_begin(k);
            if (vjob.k < 0)
                break;
        }
        MsInst *m = &ms_inst[vjob.k];
        unsigned n = it_n(m), id = vjob.id;
        if (!m->visv || m->visn != n) {  /* the block closed or grew meanwhile: start it again */
            vjob.k = -1;
            continue;
        }
        unsigned short tick = (unsigned short)(ms_frame >> 2);
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
        ms_replan = 1;
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
    return vis_off(m, id) && (unsigned short)((unsigned short)(ms_frame >> 2) - m->vist[id]) > MS_VIS_HOLD;
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
static unsigned onelod_n, onelod_kb;  /* coarse items the last plan left to their near level (kept or not) */

/* MS_STEADY: an item that is in or on its way is measured MS_CLS_HYST nearer for its class */
static inline float steady_d2(const MsInst *m, unsigned id, float d2)
{
    if (m->state[id] == MS_NONE || m->state[id] == MS_FAILED || d2 <= 0)
        return d2;
    float d = sqrtf(d2) - MS_CLS_HYST;
    return d > 0 ? d * d : 0;
}

/* class of item id of instance k with the player at pos; *key ranks it within the class (lower first) */
static int plan_item(int k, const MsInst *m, unsigned id, const float *pos, float *key)
{
    int view = ms_frame - m->used[id] <= MS_VIEW_FRAMES;
    float vk = view ? MS_VIEW_K : 1.0f;
    /* (the rank keeps the 1 s view, the class the sticky one) */
    view = ms_frame - m->used[id] <= MS_VIEW_STICKY;
    if (id < m->ncell) {
        if (!m->tab[id].bytes)
            return C_FREE;
        float d2 = steady_d2(m, id, near_d2(m, id, pos));
        /* near parts rank by distance alone (MS_STABLE): with the view factor a look around reordered them against
         * the plan's cut every turn and the arena read the same parts again and again (soak, 2026-11-07) */
        vk = 1.0f;
        *key = d2 * vk;
        /* a settle reads the near level within the draw radius (MS_SETTLE_NEAR) after the coarse level it waits for (as
         * C_NEAR it took the plan from coarse cells inside the settle radius, which the settle then waited for until its
         * 30 s cap: Firelink's near level within 26 m is 4.1 MB, ISO 2026-11-06); the rest streams in */
        if (ms_settling)
            return d2 < sq(MS_R0_DRAW) ? C_NEAR2 : d2 < sq(MS_R0_KEEP) ? C_KEEP : C_FREE;
        if (ahead_on) {
            float da = near_d2(m, id, ahead);
            if (da < d2)
                *key = da * vk;
            if (d2 >= sq(MS_R0_KEEP))
                return da < sq(MS_R0_WANT) ? C_PREF : C_FREE;
            d2 = da < d2 ? da : d2;
        }
        return d2 < sq(MS_R0_FIRST) ? C_NEAR : d2 < sq(MS_R0_DRAW) ? C_NEAR2 : d2 < sq(MS_R0_WANT) ? C_PREF : d2 < sq(MS_R0_KEEP) ? C_KEEP : C_FREE;
    }
    unsigned tag = m->rit[id - m->ncell].tag;
    if (tag & MS_RES_TEX) {
        unsigned age = tex_age ? tex_age(k, tag & ~MS_RES_TEX) : ~0u;
        *key = 0;
        if (tag & MS_RES_MIP1)  /* a streamed mip 1 (mapsplit.py MIP1_OUT): what a cell in view draws beyond the mip 0 range */
            return age <= 1 ? MS_MIP1_CLS : age <= MS_TEX_KEEP ? C_KEEP : C_FREE;
        /* -DMS_MIP0=0: full-size textures are never read (the streamed mip 1 stays the sharpest level): a third fewer
         * map reads on the ISO Taurus route and their ~0.5-0.9 MB of the arena for geometry, for softer surfaces
         * within the 5-10 m where mip 0 shows. A compromise switch, off by default (TECH_DEBT world-16). */
        if (!MS_MIP0)
            return age <= MS_TEX_KEEP ? C_KEEP : C_FREE;
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
    /* Settle by the horizon (Burg plan B9 polish; built in: the frozen residency design, 2026-11-12): in a settle
     * the coarse items the horizon stands for (MS_RES_LOD) beyond MS_SETTLE_HZN_D are not read (kept if in), so the
     * near level within the draw radius gets the room; they come back at their class once the settle ends, the horizon
     * fading over them meanwhile. report3's second warp: every settle plan had near 0, near2 1235 KB, pinned coarse
     * items refused "no room" with 5 MB of the old spot loaded, the settle ended on that refusal with no near cell in
     * and the near cells 0-5 m away popped in 2-7 s later; pinned coarse at 16-48 m was 96 % horizon-covered,
     * ~1.0-1.1 MB a plan (engine-gaps thread, ISO, 2026-11-09) */
#define MS_SETTLE_HZN_D 24.0f  /* m (ASSUMED: past the near level's draw radius + its hysteresis) */
    if (ms_settling && (tag & MS_RES_LOD) && d2 >= sq(MS_SETTLE_HZN_D)) {
        *key = d2 * vk;
        return C_KEEP;
    }
    if (tag & MS_RES_FAR) {  /* the backdrop: the horizon of the player's block; another block's while in view */
        *key = (d2 < sq(MS_FAR_D) ? d2 : sq(MS_FAR_D)) * vk;
        /* (MS_STEADY: another loaded block's backdrop, Firelink's behind the Burg, keeps C_VIEW2 whether in view or
         * not: as C_FARC it went first whenever room was short and came back from across the disc) */
        return k == (focus < 0 ? 0 : focus) ? C_PIN0 : C_VIEW2;
    }
    *key = d2 * vk;
    if (tag & MS_RES_GRASS)
        return ms_settling ? C_FREE : d2 < sq(MS_GRASS_WANT) ? C_NEAR2 : d2 < sq(MS_GRASS_KEEP) ? C_KEEP : C_FREE;
    /* (one level of detail per place: plan_ drops this item while its cell's near level stands for it, MS_ONE_LOD) */
    /* the coarse level the horizon does not stand for yet (within MS_R1_PIN): one order by distance whatever the
     * camera looks at. In view (C_VIEW, above the near parts) or not (C_PIN, below them) flipped those cells between
     * two classes on every look around; with the arena holding ~half of what the Burg pins (two blocks' lite sets
     * beside it) each turn evicted what the last one read: 80 % of all map reads were re-reads, ~1750 swaps between
     * near / coarse / horizon on screen in 40 min (Burg soak on the ISO, 2026-11-07). All of it ranks above the near
     * parts: with near parts first (MS_NEAR_FIRST) the arena never held the Burg's near level anyway (7-37 cells within
     * 24 m waited for it up to 90 s) and 5-17 cells inside the horizon radius drew their untextured horizon instead of
     * their coarse level, the "malformed parts" of the user report. Coarse first, the horizon is a ring at the far
     * edge and the near level fills in nearest cell first with what is left. */
    /* Near over coarse (Burg plan B3; built in: the frozen residency design, 2026-11-12): the coarse items of a cell with a near level
     * within MS_R0_DRAW rank below the near parts (C_VIEW2). The export's hard tier budgets the coarse level from
     * MS_R0_DRAW out and the near level inside it, never both at one cell (export_map.py WS_UNION, world_budget.py);
     * pinned above the near parts, the coarse level of the cells around the player held ~1 MB more than the budget at
     * the median spot until their near parts landed, and the near parts were refused ("no room", Burg soak 2026-11-08).
     * A cell whose near part is not in and whose coarse level is out draws its horizon (map.c), not a hole. */
#if MS_BEHIND_KEEP
    if ((tag & MS_RES_LOD) && !view && !ms_settling && d2 >= sq(MS_BEHIND_R))
        return C_KEEP;
#endif
    if ((tag & MS_RES_COARSE) && (m->tab[c].near & ~CELL_PART) && near_d2(m, c, pos) < sq(MS_R0_DRAW)) {
        *key = d2;
        return C_VIEW2;
    }
    if (d2 < sq(MS_R1_PIN)) {  /* and all of it above the near parts: the near level gets what is left */
        *key = d2;
        return C_PIN0;
    }
    /* in view out to MS_R1_VIEW: C_VIEW above the near parts (a missing coarse cell in view is a hole; with C_VIEW
     * below C_NEAR the route had 227 s with over 16 cells missing instead of 144, ISO 2026-11-06); pinned but out of
     * view: C_PIN below them */
    if (d2 >= sq(MS_R1_FIRST) && d2 < sq(MS_R1_VIEW) && view)
        return C_VIEW;
    return d2 < sq(MS_R1_FIRST) ? C_PIN0 : d2 < sq(MS_R1_PIN) ? C_PIN : d2 < sq(MS_R1_WANT) ? (!view ? C_FARC : d2 < sq(MS_R1_VIEW) ? C_VIEW : C_VIEW2)
                                                           : d2 < sq(MS_R1_KEEP) ? C_KEEP : C_FREE;
}
/* ---- reads ---- */
MsRead ms_rd[MS_INFLIGHT];
int ms_nrd;
static unsigned land_t;
static MsQ q[MS_QUEUE];
static int nq;
static MsQ blk[MS_BLOCKED];
int ms_nblk;
static unsigned scan_frame, fail_frame, fail_wait;
static float scan_pos[3];
static int room_fail, failed, evict_left, defrag_left;
/* load-time A/B (2026-11-06): a clock in ms over mapstream_update calls (COP0 Count wraps every 14.6 s), the settle
 * that ended last (its start / end on that clock), the first near cell and the full near level within MS_R0_FIRST of
 * the feet after it; KB of reads that landed with no class wanting them, KB of reads cancelled (MS_CANCEL) */
static unsigned clk_ms, clk_last, clk_frac, st_t0, st_t1, st_first, st_wait, stale_kb;
unsigned ms_cancel_kb;

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
static int scan_k = -1;
static unsigned scan_end;
static int q_scan_pick(void);
static unsigned flow_rd[3], flow_kb[3];
static unsigned thr_n[3][MS_WHY_N], thr_kb[3], thr_cls[C_TOP + 1];
/* Read order (2026-11-12): the classes rank what stays in memory; the reads of the hard tier (C_NEAR2 and up: the near
 * level within the draw radius, the pinned coarse level, coarse cells in view) go out nearest first whatever their
 * class. By class, a block turning on read its whole pinned coarse ring out to 40 m (C_PIN0, hundreds of small items)
 * before the first near part at the feet: 30-47 near parts within 20 m waited ~20 s with the reader busy (ISO
 * iso_burg_taurus, "flow" log). The classes below keep their order after them. -DMS_READ_BY_CLASS=1: by class as
 * before; a settle too (its plan has its own classes). */
#ifndef MS_READ_BY_CLASS
#define MS_READ_BY_CLASS 0
#endif
static inline int q_rank(int cls) { return !MS_READ_BY_CLASS && !ms_settling && cls >= C_NEAR2 ? C_TOP : cls; }
static int q_before(const MsQ *a, int cls, float key)
{
    int ra = q_rank(a->cls), rc = q_rank(cls);
    return ra > rc || (ra == rc && a->key <= key);
}
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
    for (int i = 0; i < ms_nblk; i++)
        if (blk[i].k == k && blk[i].id == id)
            return 1;
    return 0;
}

static void collect(int i, int r)
{
    MsRead w = ms_rd[i];
    memmove(ms_rd + i, ms_rd + i + 1, (--ms_nrd - i) * sizeof *ms_rd);
    mapstream_stats.pending = ms_nrd;
    MsInst *m = &ms_inst[w.k];
    if (w.cxl) {  /* cancelled (MS_CANCEL): the buffer is free once the read stopped */
        ms_drop(w.k, w.id);
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
            if (!it_res(m, w.id) && m->wq && m->wq[w.id] && !ms_settling) {
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
            int th = l >= 2 && (unsigned short)((ms_frame >> 5) + 2 - l) <= MS_THRASH_TICKS;
            hitch_load(l != 0, th, w.bytes);
            if (th && m->dw) {  /* the thrash log: by kind, by why it had gone and the class it had then */
                int kd = !it_res(m, w.id) ? 0 : it_tex(m, w.id) ? 2 : 1, y = m->dw[w.id] & 15, c = m->dw[w.id] >> 4;
                thr_n[kd][y < MS_WHY_N ? y : 0]++, thr_kb[kd] += w.bytes >> 10, thr_cls[c <= C_TOP ? c : 0]++;
            }
        }
        for (unsigned j = w.id; j < w.id + w.n; j++) {
            if (m->ld)
                m->ld[j] = 1;
            m->state[j] = MS_READY;
            m->used[j] = ms_frame;
            mapstream_stats.loads++;
            if (it_tex(m, j) && ms_tex_set)
                ms_tex_set(w.k, m->rit[j - m->ncell].tag & ~MS_RES_TEX, m->buf[j]);
        }
        if (!it_tex(m, w.id))
            printf("mapstream: %s %d.%u (%u items) %u KB in %u ms, %u items %u KB loaded (resident %u KB, mip 0 %u KB)\n",
                   it_res(m, w.id) ? "resident cell" : "cell", w.k, it_res(m, w.id) ? w.id - m->ncell : w.id, w.n,
                   w.bytes >> 10, mapstream_stats.ms_last, mapstream_stats.cells, mapstream_stats.bytes >> 10,
                   mapstream_stats.res_kb, mapstream_stats.tex_kb);
    } else {
        printf("mapstream: item %d.%u (%u items) read %d of %u bytes\n", w.k, w.id, w.n, r, w.bytes);
        ms_drop(w.k, w.id);
        for (unsigned j = w.id; j < w.id + w.n; j++)
            m->state[j] = MS_FAILED;
        mapstream_stats.fails++;
        failed = 1, fail_frame = ms_frame;
        fail_wait = !fail_wait ? MS_FAIL_FRAMES : fail_wait < MS_FAIL_FRAMES_MAX ? 2 * fail_wait : fail_wait;
    }
}

/* Burg plan B8b: why each run of resident items stopped, and how the reads it made sit on the disc ("mapstream: runs"
 * line every MS_RUNLOG_FRAMES frames): a read that starts where the last one ended needs no seek. B4's ISO world_route,
 * 2026-11-09: 3089 runs of 1.2 items in 6 min, 170 contiguous; most stops are the next item not wanted (class) or
 * already in (state). Reading the kept neighbours along (up to 64 KB) took 388 more items but no fewer runs, and more
 * "no room" refusals (97 -> 120): dropped */
enum { RS_END, RS_STATE, RS_GAP, RS_BASE, RS_CAP, RS_TEX, RS_CLASS, RS_BLOCKED, RS_N };
static const char *const rs_name[RS_N] = {"end", "state", "gap", "base", "cap", "tex", "class", "blocked"};
static unsigned rs_stop[RS_N], rs_runs, rs_items, rs_reads, rs_contig, rs_kb, rs_frame;
static unsigned rs_last_end, rs_last_k = ~0u;
static unsigned cpy_reads;  /* reads from a neighbour copy (MS_COPIES) since the last runs line */
/* per instance (world block): reads, and of them far backdrop / coarse / sky / texture / mip 1 / other items */
enum { RK_FAR, RK_COARSE, RK_SKY, RK_TEX, RK_MIP1, RK_OTHER, RK_N };
static unsigned rk[MS_INST][RK_N + 1];
#define MS_RUNLOG_FRAMES 300
static void rs_log(void)
{
    if (ms_frame - rs_frame < MS_RUNLOG_FRAMES)
        return;
    rs_frame = ms_frame;
    if (!rs_runs)
        return;
    printf("mapstream: runs %u (%u items, %u KB), reads %u (%u from a neighbour copy), %u start where the last ended; "
           "stops", rs_runs, rs_items, rs_kb, rs_reads, cpy_reads, rs_contig);
    cpy_reads = 0;
    for (int i = 0; i < RS_N; i++)
        printf(" %s %u", rs_name[i], rs_stop[i]);
    printf("; by block");
    for (int k = 0; k < MS_INST; k++)
        if (rk[k][RK_N])
            printf(" %d: %u (far %u coarse %u sky %u tex %u mip1 %u other %u)", k, rk[k][RK_N], rk[k][RK_FAR],
                   rk[k][RK_COARSE], rk[k][RK_SKY], rk[k][RK_TEX], rk[k][RK_MIP1], rk[k][RK_OTHER]);
    printf("\n");
    memset(rk, 0, sizeof rk);
    memset(rs_stop, 0, sizeof rs_stop);
    rs_runs = rs_items = rs_reads = rs_contig = rs_kb = 0;
}

/* Burg plan B8a (default on; DEFS=-DMS_COPIES=0 = always the block's own pack): tools/iso9660.py writes each world
 * block's resident items (its mapres.bin entry, the bytes after the block table) once more beside every other block
 * of its walk region, as <that block>/<this block>.pak. A read of block k's items while the player is in block j
 * comes from j's copy, a short seek from what j itself reads, instead of from k's pack 60-100 MB away (ISO 2026-11-09:
 * Burg <-> Firelink reads were ~half the far seeks in play). Opened on first use; host builds have no copies */
#ifndef MS_COPIES
#define MS_COPIES 1
#endif
static struct { signed char st; Pack pk; unsigned d0, len; } cpy[MS_INST][MS_INST];  /* [j][k]; st 0 ?, 1 open, -1 none */
static Pack *copy_for(int k, unsigned *off, unsigned bytes)
{
    int j = world_current();
    if (!MS_COPIES || j < 0 || j >= MS_INST || j == k)
        return NULL;
    if (!cpy[j][k].st) {
        const char *pj = world_pack(j), *pk = world_pack(k);
        unsigned d0, len;
        char path[96];
        cpy[j][k].st = -1;
        if (pj && pk && pack_entry_raw(&ms_inst[k].pk, "mapres.bin", &d0, &len)) {
            snprintf(path, sizeof path, PACK_ROOT "%s/%s.pak", pj, pk);
            if (pack_open_raw(&cpy[j][k].pk, path))
                cpy[j][k].st = 1, cpy[j][k].d0 = d0, cpy[j][k].len = len;
        }
    }
    if (cpy[j][k].st < 0 || *off < cpy[j][k].d0 || *off + bytes > cpy[j][k].d0 + cpy[j][k].len)
        return NULL;
    *off -= cpy[j][k].d0;
    cpy_reads++;
    return &cpy[j][k].pk;
}
static void copies_close(int k)  /* block k's instance closed, its reads cancelled: the copies of its items go */
{
    for (int i = 0; i < MS_INST; i++) {
        if (cpy[i][k].st > 0)
            pack_close(&cpy[i][k].pk);  /* (io_close queues behind the handle's reads) */
        cpy[i][k].st = 0;
    }
}

/* the queue's head into the arena and read (its missing wanted neighbours in mapres.bin along, up to MS_RUN bytes, as
 * groups of at most a page); 0 when nothing more goes out this frame */
static int issue(void)
{
    rs_log();
    if (MS_SCAN && q_rank(q[0].cls) < C_NEAR2) {  /* read-ahead: at most MS_RA_INFLIGHT of them in flight */
        int ra = 0;
        for (int i = 0; i < ms_nrd; i++)
            ra += !ms_rd[i].cxl && ms_rd[i].cls < C_NEAR2;
        if (ra >= MS_RA_INFLIGHT)
            return 0;
    }
    int qi = MS_SCAN ? q_scan_pick() : 0;
    MsQ w = q[qi];
    memmove(q + qi, q + qi + 1, (--nq - qi) * sizeof *q);
    MsInst *m = &ms_inst[w.k];
    if (w.id >= it_n(m) || m->state[w.id] != MS_NONE)
        return 1;
    unsigned wn = 1;
    int stop = RS_END;
    if (it_res(m, w.id)) {
        unsigned j = w.id - m->ncell, end = m->rit[j].rel + m->rit[j].bytes, tex = !!(m->rit[j].tag & MS_RES_TEX);
        while (j + wn < m->nres) {
            const MapResItem *c = &m->rit[j + wn];
            unsigned id = m->ncell + j + wn;
            stop = m->state[id] != MS_NONE ? RS_STATE : !c->bytes || c->rel < end || c->rel - end > 64 ? RS_GAP :
                   c->off - c->rel != m->rit[j].off - m->rit[j].rel ? RS_BASE :
                   c->rel + c->bytes - m->rit[j].rel > MS_RUN ? RS_CAP : !!(c->tag & MS_RES_TEX) != tex ? RS_TEX :
                   m->cls[id] < C_PREF ? RS_CLASS : blocked(w.k, id) ? RS_BLOCKED : -1;
            if (stop >= 0)
                break;
            stop = RS_END;
            end = c->rel + c->bytes, wn++;
        }
    }
    rs_runs++, rs_items += wn, rs_stop[stop]++;
    int sent = 0;
    for (unsigned g = w.id; g < w.id + wn && ms_nrd < MS_INFLIGHT;) {
        unsigned e = g + 1, gb = it_bytes(m, g);
        if (it_res(m, g)) {
            unsigned r0 = m->rit[g - m->ncell].rel;
            /* coarse items in groups of their own (they go first once the near level is in), except in a settle,
             * where split groups took 2.5x the reads (v1, ISO boot 6.0 -> 7.8 s) */
            while (e < w.id + wn && m->rit[e - m->ncell].rel + m->rit[e - m->ncell].bytes - r0 <= MS_PAGE &&
                   (ms_settling || !(m->rit[e - m->ncell].tag & MS_RES_COARSE) == !(m->rit[g - m->ncell].tag & MS_RES_COARSE)))
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
        if (nr && ms_pools_on && gb <= MS_PAGE &&
            !(it_res(m, g) && (m->rit[g - m->ncell].tag & (MS_RES_SKY | MS_RES_FAR))))
            lo = 1;
        void *p = lo && !nr ? ms_g_alloc_lend(gb) : NULL;
        if (p)
            ms_lent_n++;
        else
            p = gb <= MS_PAGE ? ms_g_alloc(gb) : ms_g_alloc_top(gb, ilim());
        if (!p && lo && nr && ms_lent_near + gb <= (MS_LEND_NEAR_KB << 10) && (p = ms_g_alloc_lend(gb)))
            ms_lent_n++, ms_lent_near += gb;
        if (!p && sent)
            break;  /* the rest waits for its turn */
        if (!p && evict_left > 0) {
            evict_left--;
            /* one page (or the pages of a group over a page) of items below this one's class, not drawn this frame
             * or the last */
            if (ms_make_window((gb + MS_PAGE - 1) / MS_PAGE, (gb + MS_GRAN - 1) / MS_GRAN, w.cls, NULL))
                p = gb <= MS_PAGE ? ms_g_alloc(gb) : ms_g_alloc_top(gb, ilim());
        }
        if (!p && !sent && gb <= MS_PAGE && nq < MS_QUEUE) {
            /* nothing to clear: empty a page by moving its groups (MS_DEFRAG); the read waits at the head of the queue
             * for the old copies to go (two draws), a few frames at most */
            int moving = ms_npend && ms_frame - ms_pend_frame <= MS_DEFRAG_WAIT;
            if (!moving && defrag_left > 0) {
                defrag_left--;
                moving = ms_defrag(1, ilim(), MS_DEFRAG_KB);
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
                    fr += ms_pfree[i];
                /* ", hard": an item of residency.h's hard tier (the near level within RES_NEAR_R, the pinned coarse
                 * level within RES_PIN_R) the export budgeted for: tools/budget.py counts those per run */
                float d = sqrtf(w.key);
                int hard = ((w.cls == C_NEAR || w.cls == C_NEAR2) && d <= MS_R0_DRAW) ||
                           ((w.cls == C_PIN || w.cls == C_PIN0) && d <= MS_R1_PIN);
                printf("mapstream: no room for item %d.%u (%u KB, class %d, %.0f m%s): nothing of a lower class to drop; "
                       "%u KB free, items %u KB (resident %u, mip 0 %u), other %u KB, borrowed %u KB\n", w.k, w.id,
                       gb >> 10, w.cls, d, hard ? ", hard" : "", fr * MS_GRAN >> 10, mapstream_stats.bytes >> 10,
                       mapstream_stats.res_kb, mapstream_stats.tex_kb, mapstream_stats.other_kb, mapstream_stats.lent_kb);
            }
            mapstream_stats.room_fails++;
            hitch_room(w.cls == C_NEAR || w.cls == C_NEAR2, w.k, w.id);
            if (ms_nblk == MS_BLOCKED)
                memmove(blk, blk + 1, --ms_nblk * sizeof *blk);
            blk[ms_nblk++] = (MsQ){w.k, w.id, 0, (float)ms_frame};
            return 1;  /* the next queued one may still fit */
        }
        room_fail = 0;
        unsigned roff = it_off(m, g);
        Pack *src = copy_for(w.k, &roff, gb);
        int tk = pack_req_read(src ? src : &m->pk, roff, p, gb);
        if (tk) {
            unsigned o = it_off(m, g);
            rs_reads++, rs_kb += gb >> 10, rs_contig += rs_last_k == w.k && o >= rs_last_end && o - rs_last_end < 2048;
            rs_last_end = o + gb, rs_last_k = w.k;
            unsigned tg = it_res(m, g) ? m->rit[g - m->ncell].tag : 0;
            rk[w.k][RK_N]++;
            rk[w.k][tg & MS_RES_TEX ? (tg & MS_RES_MIP1 ? RK_MIP1 : RK_TEX) : tg & MS_RES_SKY ? RK_SKY :
                    tg & MS_RES_FAR ? RK_FAR : tg & MS_RES_COARSE ? RK_COARSE : RK_OTHER]++;
        }
        if (!tk) {
            ms_g_free(p, gb);
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
        {  /* the flow line's reads by kind: near parts, resident geometry, textures */
            int kd = !it_res(m, g) ? 0 : it_tex(m, g) ? 2 : 1;
            flow_rd[kd]++, flow_kb[kd] += gb >> 10;
        }
        if (!ms_nrd)
            land_t = ticks();
        ms_rd[ms_nrd++] = (MsRead){w.k, g, e - g, gb, ticks(), tk, 0, w.cls};
        scan_k = w.k, scan_end = it_off(m, g) + gb;  /* MS_SCAN: the head after this read */
        mapstream_stats.pending = ms_nrd;
        sent++;
        g = e;
    }
    return 1;
}

/* MS_SCAN: the queue entry issue() takes. The queue is in rank order, so the urgent entries lead it; once the head is
 * a read-ahead class, every queued read-ahead entry (the rest of the queue) is a candidate. */
static int q_scan_pick(void)
{
    if (q_rank(q[0].cls) >= C_NEAR2 || scan_k < 0)  /* the hard tier at the head: nearest first (q_rank) */
        return 0;
    int best = 0, bk = 3;
    unsigned bo = ~0u;
    for (int i = 0; i < nq; i++) {
        const MsInst *m = &ms_inst[q[i].k];
        if (q[i].id >= it_n(m))
            continue;
        unsigned o = it_off(m, q[i].id);
        int rk = q[i].k != scan_k ? 2 : o >= scan_end ? 0 : 1;  /* ahead of the head, behind it (wrap), other pack */
        if (rk == 2)
            o = (unsigned)q[i].k << 26 | (o >> 6);  /* other packs: by block, then offset */
        if (rk < bk || (rk == bk && o < bo))
            best = i, bk = rk, bo = o;
    }
    return best;
}

/* ---- plan ---- */
/* the plan's wanted items, sorted by rank; MS_FILL_PCT: the share (%) of the item pages the plan may fill (granule rounding,
 * read groups not filling their page, ASSUMED) */
MsW ms_want[MS_WANTED];
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
    ms_nowin = C_TOP + 2;
    for (int i = 0; i < ms_nblk; i++)
        if (ms_frame - (unsigned)blk[i].key >= MS_BLOCK_FRAMES)
            blk[i--] = blk[--ms_nblk];
    int retry = failed && ms_frame - fail_frame >= fail_wait;
    failed &= !retry;
    /* classes, then the cut: wanted items in rank order (class, then key) until their bytes fill the item pages; the
     * rest are only kept (never read, first to go). Without the cut the pinned coarse cells of Firelink + three Burg
     * blocks (7 MB within 96 m, 1.9 MB of backdrop) outgrew the arena: no near cell could find room and the Burg
     * stayed at its coarse level, with cells going missing (user report, 2026-11-06) */
    unsigned nw = 0;
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            float key = 0;
            int c = it_bytes(m, id) ? plan_item(k, m, id, pos, &key) : C_FREE;
            if (c > C_KEEP && vis_cut(m, id))
                c = C_KEEP;  /* no camera cell near the camera sees it (pvs.c) */
            if (m->state[id] != MS_NONE && m->state[id] != MS_FAILED)
                key *= MS_HOLD_K;  /* in (or on its way): ranks as if nearer, so the cut does not flip at its edge */
            m->cls[id] = (unsigned char)c;
            if (m->state[id] == MS_FAILED && retry)
                m->state[id] = MS_NONE;
            if (c >= C_PREF && nw < MS_WANTED)
                ms_want[nw++] = (MsW){(unsigned char)k, (unsigned char)c, (unsigned short)id, key};
        }
    }
    onelod_n = onelod_kb = 0;
    for (int k = 0; !ms_settling && k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
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
                    ms_want[nw++] = (MsW){(unsigned char)k, (unsigned char)cc, (unsigned short)p,
                                       near_d2(m, p, pos) * MS_HOLD_K};
                m->cls[p] = (unsigned char)cc;
            }
        }
    }
    {  /* the classes of the wanted list after that */
        unsigned o = 0;
        for (unsigned i = 0; i < nw; i++)
            if ((ms_want[i].cls = ms_inst[ms_want[i].k].cls[ms_want[i].id]) >= C_PREF)
                ms_want[o++] = ms_want[i];
        nw = o;
    }
    pt[np_++] = ticks();
    qsort(ms_want, nw, sizeof *ms_want, w_cmp);
    /* what mapstream_alloc holds beyond its MS_OTHER top lies in item pages: the plan leaves it out */
    unsigned other = mapstream_stats.other_kb << 10, over = ms_pools_on ? ms_items_other : other > MS_OTHER ? other - MS_OTHER : 0;
    unsigned room = (ms_ipages + ms_nx) * MS_PAGE;  /* + the pages borrowed from the heap (MS_BORROW) */
    unsigned lend = 0;
    if (ms_pools_on) {  /* the pools' room their own blocks leave: only the lowest classes may use it (lendable) */
        unsigned own = other > ms_items_other ? other - ms_items_other : 0, pb = (ms_npages - ms_mpage0) * MS_PAGE;
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
    for (int k = 0; k < MS_INST; k++)
        if (ms_inst[k].cmark)
            memset(ms_inst[k].cmark, 0, ms_inst[k].ncell);
    unsigned cut = 0, cut_near = 0;  /* wanted items the cut left out (MS_STEADY: hitch_cut) */
    for (unsigned i = 0; i < nw; i++) {
        MsInst *m = &ms_inst[ms_want[i].k];
        unsigned id = ms_want[i].id;
        if (id < m->ncell && m->cmark) {
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
                cut++, cut_near += ms_want[i].cls == C_NEAR || ms_want[i].cls == C_NEAR2;
                continue;
            }
        } else {
            unsigned b = (it_bytes(m, id) + MS_GRAN - 1) & ~(MS_GRAN - 1);
            int lo = lendable(ms_want[i].cls) && ms_want[i].cls != C_NEAR && ms_want[i].cls != C_NEAR2;
            sum += b, sum_hi += lo ? 0 : b;  /* (counted when cut too, as before: past the cap, the rest is cut) */
            if (sum > cap || (!lo && sum_hi > cap_hi) || (lo && near_missing && sum > cap_hi)) {
                m->cls[id] = C_KEEP;
                cut++, cut_near += id < m->ncell && (ms_want[i].cls == C_NEAR || ms_want[i].cls == C_NEAR2);
                continue;
            }
        }
        if (m->state[ms_want[i].id] == MS_NONE && !blocked(ms_want[i].k, ms_want[i].id))
            q_add(ms_want[i].k, ms_want[i].id, ms_want[i].cls, ms_want[i].key);
    }
    if (!ms_settling)
        hitch_cut(cut, cut_near);
    pt[np_++] = ticks();
    for (int i = 0; i < ms_nrd; i++) {  /* reads in flight that nothing wants any more */
        MsInst *m = &ms_inst[ms_rd[i].k];
        if (ms_rd[i].cxl)
            continue;
        int want = 0;
        for (unsigned j = ms_rd[i].id; j < ms_rd[i].id + ms_rd[i].n; j++)
            want |= m->cls[j] >= C_PREF;
        if (want)
            continue;
        ms_cancel_kb += ms_rd[i].bytes >> 10;
        if (!pack_req_cancel_async(ms_rd[i].tk)) {  /* in flight: it stops after its page; collect() frees it then */
            ms_rd[i].cxl = 1;
            continue;
        }
        ms_why = MS_WHY_CANCEL, ms_drop(ms_rd[i].k, ms_rd[i].id);
        memmove(ms_rd + i, ms_rd + i + 1, (--ms_nrd - i) * sizeof *ms_rd);
        i--;
    }
    mapstream_stats.pending = ms_nrd;
    {  /* what the plan wants per class (KB) against the room, every ~10 s of plans (tuning the classes) */
        static unsigned np;
        if (!(np++ % 40)) {
            unsigned kb[C_TOP + 1] = {0};
            for (unsigned i = 0; i < nw; i++)
                kb[ms_want[i].cls] += it_bytes(&ms_inst[ms_want[i].k], ms_want[i].id) >> 10;
            printf("mapstream: plan %u KB room: pin %u view %u near %u near2 %u view2 %u farc %u pref %u KB; stale reads "
                   "landed %u KB, cancelled %u KB; moved %u groups %u KB; one lod %u coarse items %u KB\n", cap >> 10, kb[C_PIN] + (C_PIN0 != C_PIN ? kb[C_PIN0] : 0),
                   kb[C_VIEW], kb[C_NEAR], kb[C_NEAR2], kb[C_VIEW2], kb[C_FARC], kb[C_PREF], stale_kb, ms_cancel_kb,
                   ms_defrag_moves, ms_defrag_kb, onelod_n, onelod_kb);
            printf("mapstream: near wait %u ms (dev %u) from wanted to landed\n", (unsigned)lat_avg, (unsigned)lat_dev);
            /* the pinned coarse level by box distance of its cell (0-16-32-48-64 m, beyond), the backdrop and sky
             * apart, and what of it the horizon could stand for (MS_RES_LOD): sizing the pin radius (2026-11-08) */
            unsigned band[6] = {0}, lod[6] = {0}, far = 0, sky = 0;
            for (unsigned i = 0; i < nw; i++) {
                const MsInst *m = &ms_inst[ms_want[i].k];
                unsigned id = ms_want[i].id;
                if (ms_want[i].cls != C_PIN0 || id < m->ncell)
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
                const MsInst *m = &ms_inst[ms_want[i].k];
                unsigned id = ms_want[i].id, c, nr = id < m->ncell;
                if (ms_want[i].cls < C_NEAR2)
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
            const MsInst *m = &ms_inst[k];
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
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a;
            if (m->state[id] == MS_READY && (grp_of(m, id, &a), a == id) && ms_grp_cls(m, id) == C_FREE &&
                !ms_grp_chain(m, id))
                hitch_drop(0, 0), ms_drop(k, id);
        }
    }
    for (int k = 0; !ms_settling && k < MS_INST; k++) {  /* near parts the cut left wanted and not in: their wait starts */
        MsInst *m = &ms_inst[k];
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


/* Why the near level is late (the "flow" line, every MS_FLOW_FRAMES outside settles): frames the reader was held for
 * a lite set with reads queued, frames with every read slot busy, frames with nothing queued, and the near cells
 * within the draw radius that are not in, by what keeps them out: the plan's cut (no room by its count), a refusal
 * (blocked after "no room"), the visibility cut, or simply the disc (queued / reading). */
#define MS_FLOW_FRAMES 60u
#ifndef MS_HOLD_INFLIGHT
#define MS_HOLD_INFLIGHT 3  /* ASSUMED; -DMS_HOLD_INFLIGHT=0: no map read while a lite set loads, as before */
#endif
static void flow_log(const float *pos)
{
    static unsigned n, held, full, idle;
    if (ms_settling) {
        n = held = full = idle = 0;
        return;
    }
    held += hold && nq, full += ms_nrd == MS_INFLIGHT, idle += !nq && !ms_nrd;
    if (++n < MS_FLOW_FRAMES)
        return;
    unsigned cut = 0, ref = 0, vis = 0, disc = 0, kb = 0;
    for (int k = 0; k < MS_INST; k++) {
        const MsInst *m = &ms_inst[k];
        for (unsigned c = 0; c < m->ncell; c++) {
            if (!m->tab[c].bytes || m->state[c] == MS_READY || near_d2(m, c, pos) >= sq(MS_R0_DRAW))
                continue;
            kb += m->tab[c].bytes >> 10;
            if (vis_cut(m, c))
                vis++;
            else if (blocked(k, c))
                ref++;
            else if (m->cls[c] < C_PREF)
                cut++;
            else
                disc++;
        }
    }
    if (cut + ref + vis + disc + held + flow_rd[0] + flow_rd[1] + flow_rd[2])
        printf("mapstream: flow %u frames: held %u full %u idle %u | near parts within %.0f m not in: cut %u refused %u "
               "unseen %u disc %u (%u KB) | queue %d in flight %d, near wait %u ms | reads near %u (%u KB) resident %u (%u KB) "
               "textures %u (%u KB)\n", n, held, full, idle,
               (double)MS_R0_DRAW, cut, ref, vis, disc, kb, nq, ms_nrd, (unsigned)lat_avg, flow_rd[0], flow_kb[0],
               flow_rd[1], flow_kb[1], flow_rd[2], flow_kb[2]);
    n = held = full = idle = 0;
    memset(flow_rd, 0, sizeof flow_rd), memset(flow_kb, 0, sizeof flow_kb);
    static unsigned tn;
    if (!(++tn % 5)) {  /* every 10 s: what was read again within ~30 s of its drop */
        unsigned any = 0;
        for (int i = 0; i < 3; i++)
            for (int y = 0; y < MS_WHY_N; y++)
                any += thr_n[i][y];
        if (any)
            printf("mapstream: thrash 10 s (freed / window / lent / borrow / cancel): near %u %u %u %u %u (%u KB) resident "
                   "%u %u %u %u %u (%u KB) textures %u %u %u %u %u (%u KB) | class when dropped free %u keep %u pref %u "
                   "farc %u view2 %u near2 %u pin %u near %u view %u pin0 %u\n",
                   thr_n[0][0], thr_n[0][1], thr_n[0][2], thr_n[0][3], thr_n[0][4], thr_kb[0],
                   thr_n[1][0], thr_n[1][1], thr_n[1][2], thr_n[1][3], thr_n[1][4], thr_kb[1],
                   thr_n[2][0], thr_n[2][1], thr_n[2][2], thr_n[2][3], thr_n[2][4], thr_kb[2],
                   thr_cls[C_FREE], thr_cls[C_KEEP], thr_cls[C_PREF], thr_cls[C_FARC], thr_cls[C_VIEW2], thr_cls[C_NEAR2],
                   thr_cls[C_PIN], thr_cls[C_NEAR], thr_cls[C_VIEW], thr_cls[C_PIN0]);
        memset(thr_n, 0, sizeof thr_n), memset(thr_kb, 0, sizeof thr_kb), memset(thr_cls, 0, sizeof thr_cls);
    }
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
        any |= ms_inst[k].ncell != 0;
    if (!any)
        return;
    if (!ms_ensure_arena()) {  /* no near level: the coarse one draws everywhere */
        for (int k = 0; k < MS_INST; k++)
            mapstream_close(k);
        return;
    }
    ms_frame++;
    ms_x_update();
    {
        unsigned now = ticks(), dt = now - clk_last + clk_frac;  /* 294912 Count ticks per ms */
        clk_last = now;
        if (ms_frame > 1)
            clk_ms += dt / 294912, clk_frac = dt % 294912;
    }
    int r;
    HITCH_BEGIN(io);
    for (int i = 0; i < ms_nrd; i++)
        if (pack_req_done(ms_rd[i].tk, &r))
            collect(i--, r);
    HITCH_END(io, HS_MSIO);
    {  /* walking velocity per frame (smoothed); a warp / teleport starts it again */
        float d[3] = {pos[0] - last_pos[0], pos[1] - last_pos[1], pos[2] - last_pos[2]};
        int jump = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 4.0f || ms_settling;
        for (int i = 0; i < 3; i++) {
            vel[i] = jump ? 0 : vel[i] + (d[i] - vel[i]) * 0.1f;
            ahead[i] = pos[i] + vel[i] * MS_AHEAD_FRAMES;
            last_pos[i] = pos[i];
        }
        ahead_on = !jump && vel[0] * vel[0] + vel[2] * vel[2] > sq(0.02f);  /* moving faster than 0.6 m/s */
    }
    vis_step(ms_settling ? MS_VIS_SETTLE_US : MS_VIS_SLICE_US);  /* a row changed: the job re-plans once through (pvs.c) */
    float dx = pos[0] - scan_pos[0], dy = pos[1] - scan_pos[1], dz = pos[2] - scan_pos[2];
    if (ms_replan || !scan_frame || ms_frame - scan_frame >= MS_SCAN_FRAMES || dx * dx + dy * dy + dz * dz > sq(MS_SCAN_D)) {
        ms_replan = 0;
        scan_frame = ms_frame;
        memcpy(scan_pos, pos, sizeof scan_pos);
        plan(pos);
    }
    evict_left = MS_EVICT_PASSES;
    defrag_left = 1;
    ms_pend_release();
    HITCH_BEGIN(is);
    /* While a block's lite set loads (hold) the map reads go on, MS_HOLD_INFLIGHT at a time: the lite set's own reads
     * are served ahead of them (pack_req_read_first), so it waits for one 32 KB page at most. With no map read at all
     * until the block was fully on, walking into the Burg bonfire block left the reader idle for ~11 s while 10 -> 41
     * near parts within the draw radius waited (1.7 MB; "flow" log, ISO iso_burg_taurus 2026-11-12). */
    while ((!hold || ms_nrd < MS_HOLD_INFLIGHT) && nq && ms_nrd < MS_INFLIGHT && issue())
        ;
    HITCH_END(is, HS_MSIO);
    flow_log(pos);
}

void mapstream_free_info(unsigned *free_kb, unsigned *run_kb, unsigned *other_kb)
{
    unsigned fr = 0, run = 0, best = 0;
    for (unsigned p = 0; ms_pfree && p < ms_npages; p++) {
        fr += ms_pfree[p];
        run = ms_pfree[p] == MS_GPP ? run + 1 : 0;
        best = run > best ? run : best;
    }
    for (unsigned p = ms_npages; ms_pfree && p < ntot(); p++)  /* borrowed pages: free room, never part of a run */
        fr += ms_pfree[p];
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
    MsInst *m = &ms_inst[k];
    if (!pack_entry_raw(&m->pk, "cells.bin", &m->base, &m->total) || !m->total) {
        if (!m->borrowed)
            pack_close(&m->pk);
        return 0;
    }
    m->tab = cells, m->ncell = n, m->keys = cell_keys(cells, n);
    m->state = calloc(n, 1), m->cls = calloc(n, 1);
    m->buf = calloc(n, sizeof *m->buf), m->used = calloc(n, sizeof *m->used);
    m->ld = calloc(n, sizeof *m->ld);  /* (NULL: no churn count) */
    m->dw = calloc(n, 1);
    m->wq = calloc(n, sizeof *m->wq);  /* (NULL: no wait measured) */
    m->cmark = calloc(n, 1);  /* (NULL: parts planned one by one) */
    m->rit = NULL, m->nres = 0, m->rgrp = NULL;
    if (!m->state || !m->cls || !m->buf || !m->used) {
        if (!m->borrowed)
            pack_close(&m->pk);
        free(m->state), free(m->cls), free(m->buf), free(m->used), free(m->ld), free(m->dw), free(m->cmark), free(m->wq);
        memset(m, 0, sizeof *m);
        return 0;
    }
    ms_replan = 1;
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
    ms_inst[k].borrowed = 0;
    if (!n || !pack_open(&ms_inst[k].pk, pak_path))
        return 0;
    return open_cells(k, cells, n);
}

int mapstream_open_pack(int k, const void *pack, const MapCell *cells, unsigned n)
{
    if (k < 0 || k >= MS_INST || !n)
        return 0;
    mapstream_close(k);
    sel = k;
    ms_inst[k].pk = *(const Pack *)pack;
    ms_inst[k].borrowed = 1;
    return open_cells(k, cells, n);
}

int mapstream_res_open(int k, const MapResItem *items, unsigned n, unsigned nitems)
{
    if (k < 0 || k >= MS_INST || !nitems)
        return 0;
    MsInst *m = &ms_inst[k];
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
        unsigned char *w = realloc(m->dw, t);
        if (w)
            memset(w + m->ncell, 0, nitems);
        else
            free(m->dw);
        m->dw = w;
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
    ms_replan = 1;
    unsigned cb = 0, tb = 0, nt = 0;
    for (unsigned j = 0; j < nitems; j++)
        *(items[j].tag & MS_RES_TEX ? &tb : &cb) += items[j].bytes, nt += !!(items[j].tag & MS_RES_TEX);
    printf("mapstream: instance %d resident level per cell: %u cells + sky in %u items %u KB, %u mip 0 textures %u KB\n",
           k, n, nitems - nt, cb >> 10, nt, tb >> 10);
    {  /* Burg plan B8a: where each kind of item lies in the pack (a neighbour's backdrop read from far away) */
        static const char *const kn[5] = {"far", "coarse", "near", "tex", "mip1"};
        unsigned c[5] = {0}, kb[5] = {0}, lo[5], hi[5] = {0};
        for (int q = 0; q < 5; q++)
            lo[q] = ~0u;
        for (unsigned j = 0; j < nitems; j++) {
            unsigned tg = items[j].tag;
            int q = tg & MS_RES_TEX ? (tg & MS_RES_MIP1 ? 4 : 3) : tg & MS_RES_FAR ? 0 : tg & MS_RES_COARSE ? 1 : 2;
            c[q]++, kb[q] += items[j].bytes;
            lo[q] = items[j].off < lo[q] ? items[j].off : lo[q];
            hi[q] = items[j].off + items[j].bytes > hi[q] ? items[j].off + items[j].bytes : hi[q];
        }
        printf("mapstream: instance %d items in the pack:", k);
        for (int q = 0; q < 5; q++)
            if (c[q])
                printf(" %s %u (%u KB) at %u..%u KB", kn[q], c[q], kb[q] >> 10, lo[q] >> 10, hi[q] >> 10);
        printf("\n");
    }
    return 1;
}

void mapstream_close(int k)
{
    if (k < 0 || k >= MS_INST || !ms_inst[k].ncell)
        return;
    MsInst *m = &ms_inst[k];
    for (int i = 0; i < ms_nrd; i++)  /* stop its reads (after their current page) before the buffers go back */
        if (ms_rd[i].k == k) {
            pack_req_cancel(ms_rd[i].tk);
            memmove(ms_rd + i, ms_rd + i + 1, (--ms_nrd - i) * sizeof *ms_rd);
            i--;
        }
    mapstream_stats.pending = ms_nrd;
    copies_close(k);
    for (unsigned id = 0; id < it_n(m); id++) {
        unsigned a;
        if ((m->state[id] == MS_READY || m->state[id] == MS_READING) && (grp_of(m, id, &a), a == id))
            ms_drop(k, id);
    }
    if (!m->borrowed)
        pack_close(&m->pk);
    free(m->state), free(m->cls), free(m->buf), free(m->used), free(m->rit), free(m->rgrp), free(m->ld), free(m->dw), free(m->cmark);
    free(m->wq);
    free(m->visv), free(m->vist);
    mapstream_stats.vis_items -= m->vish, mapstream_stats.vis_kb -= m->viskb;
    memset(m, 0, sizeof *m);
    if (vjob.k == k)
        vjob.k = -1;
    nq = 0, ms_replan = 1;
    for (int i = 0; i < ms_nblk; i++)
        if (blk[i].k == k)
            blk[i--] = blk[--ms_nblk];
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
    tex_age = age, ms_tex_set = set;
}
void mapstream_hold(int on) { hold = on; }
void mapstream_settle(int on)
{
    if (on && !ms_settling)
        st_t0 = clk_ms, st_wait = 0;
    if (!on && ms_settling)
        st_t1 = clk_ms, st_first = 0, st_wait = 1;
    ms_replan |= on != ms_settling;
    ms_settling = on;
}
void mapstream_focus(int k)
{
    ms_replan |= k != focus;
    focus = k;
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
        ms_replan = 1;
    settle_r = r, settle_rf = r_focus;
    if (ms_arena && (ms_replan || !scan_frame))
        return 1;  /* classes not planned yet (all C_FREE): the check below would find nothing to wait for (the boot
                    * settle ended after 0 ms with 51 cells missing, ISO 2026-11-06) */
    for (int i = 0; i < ms_nrd; i++) {  /* only the reads the settle waits for: the rest streams on behind the game */
        const MsInst *m = &ms_inst[ms_rd[i].k];
        if (it_res(m, ms_rd[i].id) && settle_item(ms_rd[i].k, m, ms_rd[i].id - m->ncell, pos, r, r_focus))
            return 1;
    }
    if (room_fail)
        return 0;  /* nothing more fits: go on with what is in */
    /* the near level within the draw radius (the plan made it C_NEAR2 in the settle; one the cut left out is C_KEEP),
     * for at most MS_SETTLE_NEAR_MS of the settle */
    if (clk_ms - st_t0 < MS_SETTLE_NEAR_MS)
        for (int k = 0; k < MS_INST; k++) {
            const MsInst *m = &ms_inst[k];
            for (unsigned c = 0; c < m->ncell; c++)
                if (m->tab[c].bytes && m->cls[c] >= C_PREF && (m->state[c] == MS_READING ||
                    (m->state[c] == MS_NONE && !blocked(k, c))) && !vis_off(m, c) &&
                    near_d2(m, c, pos) < sq(MS_R0_DRAW))
                    return 1;
        }
    for (int k = 0; k < MS_INST; k++) {
        const MsInst *m = &ms_inst[k];
        for (unsigned j = 0; j < m->nres; j++)
            if (m->state[m->ncell + j] == MS_NONE && m->rit[j].bytes && m->cls[m->ncell + j] >= C_PREF &&
                settle_item(k, m, j, pos, r, r_focus) && !blocked(k, m->ncell + j))  /* (not what the cut left out) */
                return 1;
    }
    return 0;
}

const void *mapstream_get(unsigned i)
{
    MsInst *m = &ms_inst[sel];
    if (i >= m->ncell)
        return NULL;
    drawn();
    m->used[i] = ms_frame;  /* asked for: in view (a missing one too) */
    return m->state[i] == MS_READY ? m->buf[i] : NULL;
}

const unsigned char *mapstream_res(unsigned i)
{
    MsInst *m = &ms_inst[sel];
    if (i >= m->nres)
        return NULL;
    drawn();
    m->used[m->ncell + i] = ms_frame;
    return m->state[m->ncell + i] == MS_READY ? (const unsigned char *)m->buf[m->ncell + i] - m->rit[i].rel : NULL;
}
