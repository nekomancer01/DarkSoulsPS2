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
 * A streaming source besides the player (a respawn point, world.h world_source) classes each item too, by a settle's
 * rule, at most C_VIEW2 and after the player's own C_VIEW2 items; an item takes the highest class any source gives it
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
 * Files (split 2026-11-12, and into the residency planner and the page arena in Polaris 4.1, code moved as it was):
 * this one holds the instances and the open / close / settle / get API and runs a frame (mapstream_update); msplan.c
 * plans (classes, visibility, the quality scale, the cut); msread.c reads (queue, issue, landing, the flow and run
 * logs); msevict.c makes room (drops, windows, defrag, tidy, lending, borrowed pages given back, mapstream_alloc);
 * mapstream_i.h is their shared state (names ms_*), the radii and the item helpers. msarena.c is the page arena
 * (granules, pages, pools, borrowed pages: msarena.h), which knows nothing of items and whose state the others reach
 * only through msarena.h. The numbers the export shares live in residency.h. */
#include "mapfile.h"
#include "mapstream_i.h"
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

int ms_focus = -1, ms_hold;
int ms_settling;
static float settle_r, settle_rf;
unsigned (*ms_tex_age)(int k, unsigned tag);
void (*ms_tex_set)(int k, unsigned tag, const void *p);

int ms_replan;

void mapstream_vis_hooks(int (*vis)(int k, unsigned id), unsigned (*gen)(int k))
{
#ifdef NO_PVS
    (void)vis, (void)gen;
#else
    ms_vis_hook = vis, ms_vis_gen = gen;
#endif
}
/* ---- reads ---- */
MsRead ms_rd[MS_INFLIGHT];
unsigned ms_scan_frame, ms_fail_frame, ms_fail_wait;
static float scan_pos[3];
/* load-time A/B (2026-11-06): a clock in ms over mapstream_update calls (COP0 Count wraps every 14.6 s), the settle
 * that ended last (its start / end on that clock), the first near cell and the full near level within MS_R0_FIRST of
 * the feet after it; KB of reads that landed with no class wanting them, KB of reads cancelled (MS_CANCEL) */
unsigned ms_clk_ms, ms_clk_last, ms_clk_frac, ms_st_t0, ms_st_t1, ms_st_first, ms_st_wait, ms_stale_kb;

static void orphan_pump(void);

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
    if (!msa_ensure()) {  /* no near level: the coarse one draws everywhere */
        for (int k = 0; k < MS_INST; k++)
            mapstream_close(k);
        return;
    }
    ms_frame++;
    ms_x_update();
    {
        unsigned now = ticks(), dt = now - ms_clk_last + ms_clk_frac;  /* 294912 Count ticks per ms */
        ms_clk_last = now;
        if (ms_frame > 1)
            ms_clk_ms += dt / 294912, ms_clk_frac = dt % 294912;
    }
    int r;
    HITCH_BEGIN(io);
    for (int i = 0; i < ms_nrd; i++)
        if (pack_req_done(ms_rd[i].tk, &r))
            ms_collect(i--, r);
    orphan_pump();
    HITCH_END(io, HS_MSIO);
    {  /* walking velocity per frame (smoothed); a warp / teleport starts it again */
        float d[3] = {pos[0] - ms_last_pos[0], pos[1] - ms_last_pos[1], pos[2] - ms_last_pos[2]};
        int jump = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 4.0f || ms_settling;
        for (int i = 0; i < 3; i++) {
            ms_vel[i] = jump ? 0 : ms_vel[i] + (d[i] - ms_vel[i]) * 0.1f;
            ms_ahead[i] = pos[i] + ms_vel[i] * MS_AHEAD_FRAMES;
            ms_last_pos[i] = pos[i];
        }
        ms_ahead_on = !jump && ms_vel[0] * ms_vel[0] + ms_vel[2] * ms_vel[2] > sq(0.02f);  /* moving faster than 0.6 m/s */
    }
    ms_vis_step(ms_settling ? MS_VIS_SETTLE_US : MS_VIS_SLICE_US);  /* a row changed: the job re-plans once through (pvs.c) */
    float dx = pos[0] - scan_pos[0], dy = pos[1] - scan_pos[1], dz = pos[2] - scan_pos[2];
    int planned = 0;
    if (ms_replan || !ms_scan_frame || ms_frame - ms_scan_frame >= MS_SCAN_FRAMES || dx * dx + dy * dy + dz * dz > sq(MS_SCAN_D)) {
        ms_replan = 0;
        ms_scan_frame = ms_frame;
        memcpy(scan_pos, pos, sizeof scan_pos);
        ms_plan(pos);
        planned = 1;
    }
    ms_evict_left = MS_EVICT_PASSES;
    ms_defrag_left = 1;
    ms_pend_release();
    HITCH_BEGIN(is);
    /* While a block's lite set loads (hold) the map reads go on, MS_HOLD_INFLIGHT at a time: the lite set's own reads
     * are served ahead of them (pack_req_read_first), so it waits for one 32 KB page at most. With no map read at all
     * until the block was fully on, walking into the Burg bonfire block left the reader idle for ~11 s while 10 -> 41
     * near parts within the draw radius waited (1.7 MB; "flow" log, ISO iso_burg_taurus 2026-11-12). */
    while ((!ms_hold || ms_nrd < MS_HOLD_INFLIGHT) && ms_nq && ms_nrd < MS_INFLIGHT && ms_issue())
        ;
    HITCH_END(is, HS_MSIO);
    /* MS_TIDY: a quiet frame (no plan, no move for a read, no near part waiting to be read: a pending tidy move
     * made a refused near read wait MS_DEFRAG_WAIT frames at the queue head, taurus pop-ins 35 -> 47; with no read
     * waiting at all it seldom ran in the Burg and walls refusals rose 102 -> 118, 2026-11-20) keeps whole pages free */
    int near_q = 0;
    for (int i = 0; i < ms_nq && !near_q; i++)
        near_q = ms_q[i].cls == C_NEAR || ms_q[i].cls == C_NEAR2;
    if (!planned && ms_defrag_left > 0 && !near_q)
        ms_tidy();
    {  /* MS_QSCALE: the frames spent below full scale, a line per 300 frames (~10 s) that had any */
        static unsigned qn, qlow;
        static float qmin = 1.0f;
        float qv = ms_qs_now();
        qlow += qv < 1.0f, qmin = qv < qmin ? qv : qmin;
        if (++qn >= 300) {
            if (qlow)
                printf("mapstream: quality scale 300 frames: %u reduced, lowest %.2f\n", qlow, (double)qmin);
            qn = qlow = 0, qmin = 1.0f;
        }
    }
    ms_flow_log(pos);
}

void mapstream_free_info(unsigned *free_kb, unsigned *run_kb, unsigned *other_kb)
{
    unsigned fr = 0, run = 0, best = 0;
    for (unsigned p = 0; msa_ready() && p < msa_npages(); p++) {
        fr += msa_pfree(p);
        run = msa_pfree(p) == MS_GPP ? run + 1 : 0;
        best = run > best ? run : best;
    }
    for (unsigned p = msa_npages(); msa_ready() && p < ntot(); p++)  /* borrowed pages: free room, never part of a run */
        fr += msa_pfree(p);
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
    if (!mapf_entry(&m->pk, "cells.bin", MAPF_CELLS, &m->base, &m->total) || !m->total) {  /* (src/mapfile.h) */
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
    if (!mapf_check(&ms_inst[k].pk, "cells.bin", MAPF_CELLS)) {  /* (world blocks: world.c checked theirs at boot) */
        pack_close(&ms_inst[k].pk);
        return 0;
    }
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
        for (int kq = 0; kq < 5; kq++)
            lo[kq] = ~0u;
        for (unsigned j = 0; j < nitems; j++) {
            unsigned tg = items[j].tag;
            int kq = tg & MS_RES_TEX ? (tg & MS_RES_MIP1 ? 4 : 3) : tg & MS_RES_FAR ? 0 : tg & MS_RES_COARSE ? 1 : 2;
            c[kq]++, kb[kq] += items[j].bytes;
            lo[kq] = items[j].off < lo[kq] ? items[j].off : lo[kq];
            hi[kq] = items[j].off + items[j].bytes > hi[kq] ? items[j].off + items[j].bytes : hi[kq];
        }
        printf("mapstream: instance %d items in the pack:", k);
        for (int kq = 0; kq < 5; kq++)
            if (c[kq])
                printf(" %s %u (%u KB) at %u..%u KB", kn[kq], c[kq], kb[kq] >> 10, lo[kq] >> 10, hi[kq] >> 10);
        printf("\n");
    }
    return 1;
}
static Orphan orphan[MS_INFLIGHT];
static int n_orphan;
/* cancelled reads of closed instances that were still landing: their buffers go back once they have */
static void orphan_pump(void)
{
    int r;
    for (int i = 0; i < n_orphan; i++)
        if (pack_req_done(orphan[i].tk, &r)) {
            msa_free(orphan[i].buf, orphan[i].bytes);
            orphan[i--] = orphan[--n_orphan];
        }
}

void mapstream_close(int k)
{
    if (k < 0 || k >= MS_INST || !ms_inst[k].ncell)
        return;
    MsInst *m = &ms_inst[k];
    for (int i = 0; i < ms_nrd; i++)  /* stop its reads (after their current page) before the buffers go back */
        if (ms_rd[i].k == k) {
            /* without waiting (MS_ORPHAN): pack_req_cancel held the frame until the read in flight stopped, 117-418 ms
             * of the world step whenever a block let go of its map tables (iso_neighbours, 2026-11-15); such a read
             * keeps its buffer (an orphan) until it lands, collected by orphan_pump */
            unsigned a, e = grp_of(m, ms_rd[i].id, &a);
            if (!MS_ORPHAN) {
                pack_req_cancel(ms_rd[i].tk);  /* the fallback waits for the read to stop (it did not cancel at all:
                                                * the read went on landing in buffers freed below) */
            } else if (pack_req_cancel_async(ms_rd[i].tk)) {
                /* never sent: gone now, its items go below */
            } else if (n_orphan == MS_INFLIGHT) {
                pack_req_cancel(ms_rd[i].tk);
            } else {
                orphan[n_orphan++] = (Orphan){ms_rd[i].tk, m->buf[a], grp_bytes(m, a, e)};
                ms_keep_buf = 1, ms_drop(k, a), ms_keep_buf = 0;  /* its items go; the buffer stays the read's */
            }
            memmove(ms_rd + i, ms_rd + i + 1, (--ms_nrd - i) * sizeof *ms_rd);
            i--;
        }
    mapstream_stats.pending = ms_nrd;
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
    if (ms_vjob.k == k)
        ms_vjob.k = -1;
    ms_nq = 0, ms_replan = 1;
    for (int i = 0; i < ms_nblk; i++)
        if (ms_blk[i].k == k)
            ms_blk[i--] = ms_blk[--ms_nblk];
    if (sel == k)
        sel = 0;
}

int mapstream_reading(int k)
{
    int n = 0;
    for (int i = 0; i < ms_nrd; i++)
        n += ms_rd[i].k == k;
    return n;
}

/* Why cell c of the selected instance (and its parts) is not drawn at its near level yet (map.c's pop-in line): 0 in,
 * 1 no camera cell sees it (visibility cut), 2 refused (blocked after "no room"), 3 left out by the plan's cut,
 * 4 queued / reading (the disc), 5 settling */
int mapstream_settling(void) { return ms_settling; }

int mapstream_why(unsigned c)
{
    const MsInst *m = &ms_inst[sel];
    if (ms_settling)
        return 5;
    int why = 0;
    for (unsigned p = c; p < m->ncell && (p == c || (m->tab[p].near & CELL_PART)); p++) {
        if (!m->tab[p].bytes || m->state[p] == MS_READY)
            continue;
        int w = vis_cut(m, p) ? 1 : ms_blocked(sel, p) ? 2 : m->cls[p] < C_PREF ? 3 : 4;
        why = w > why ? w : why;  /* (the slowest reason of the cell's parts) */
    }
    return why;
}

void mapstream_select(int k)
{
    if (k >= 0 && k < MS_INST)
        sel = k;
}

/* ---- the rest of the API ---- */
void mapstream_tex_hooks(unsigned (*age)(int k, unsigned tag), void (*set)(int k, unsigned tag, const void *p))
{
    ms_tex_age = age, ms_tex_set = set;
}
void mapstream_hold(int on) { ms_hold = on; }
float ms_src[MS_NSRC][3];
int ms_nsrc;
void mapstream_sources(const float *pos, int n)
{
    n = n < 0 ? 0 : n > MS_NSRC ? MS_NSRC : n;
    ms_replan |= n != ms_nsrc || (n && memcmp(ms_src, pos, n * sizeof ms_src[0]));
    if (n)
        memcpy(ms_src, pos, n * sizeof ms_src[0]);
    ms_nsrc = n;
}
void mapstream_settle(int on)
{
    if (on && !ms_settling)
        ms_st_t0 = ms_clk_ms, ms_st_wait = 0;
    if (!on && ms_settling)
        ms_st_t1 = ms_clk_ms, ms_st_first = 0, ms_st_wait = 1;
    ms_replan |= on != ms_settling;
    ms_settling = on;
}
void mapstream_focus(int k)
{
    ms_replan |= k != ms_focus;
    ms_focus = k;
}
/* resident item j of instance k counts for a settle: the sky of the player's block, or a cell's coarse / kept items
 * within r (r_focus for that block) unless its near level stands for them */
static int settle_item(int k, const MsInst *m, unsigned j, const float *pos, float r, float r_focus)
{
    unsigned tag = m->rit[j].tag;
    if (tag & (MS_RES_TEX | MS_RES_GRASS))  /* foliage is not read in a settle (plan_item) */
        return 0;
    if (tag & MS_RES_SKY)
        return k == (ms_focus < 0 ? 0 : ms_focus);
    unsigned c = MS_RES_CELL(tag);
    float rr = k == ms_focus && r_focus > r ? r_focus : r;
    if (vis_off(m, m->ncell + j))  /* no camera cell near the camera sees it (pvs.c) */
        return 0;
    if (c >= m->ncell || ((tag & MS_RES_COARSE) && ms_near_in(m, c)))
        return 0;
    return box_d2(&m->tab[c], pos) < rr * rr;
}

int mapstream_busy(const float pos[3], float r, float r_focus)
{
    if (r != settle_r || r_focus != settle_rf)
        ms_replan = 1;
    settle_r = r, settle_rf = r_focus;
    if (msa_ready() && (ms_replan || !ms_scan_frame))
        return 1;  /* classes not planned yet (all C_FREE): the check below would find nothing to wait for (the boot
                    * settle ended after 0 ms with 51 cells missing, ISO 2026-11-06) */
    for (int i = 0; i < ms_nrd; i++) {  /* only the reads the settle waits for: the rest streams on behind the game */
        const MsInst *m = &ms_inst[ms_rd[i].k];
        if (it_res(m, ms_rd[i].id) && settle_item(ms_rd[i].k, m, ms_rd[i].id - m->ncell, pos, r, r_focus))
            return 1;
    }
    if (ms_room_fail)
        return 0;  /* nothing more fits: go on with what is in */
    /* the near level within the draw radius (the plan made it C_NEAR2 in the settle; one the cut left out is C_KEEP),
     * for at most MS_SETTLE_NEAR_MS of the settle */
    if (ms_clk_ms - ms_st_t0 < MS_SETTLE_NEAR_MS)
        for (int k = 0; k < MS_INST; k++) {
            const MsInst *m = &ms_inst[k];
            for (unsigned c = 0; c < m->ncell; c++)
                if (m->tab[c].bytes && m->cls[c] >= C_PREF && (m->state[c] == MS_READING ||
                    (m->state[c] == MS_NONE && !ms_blocked(k, c))) && !vis_off(m, c) &&
                    near_d2(m, c, pos) < sq(MS_R0_DRAW))
                    return 1;
        }
    for (int k = 0; k < MS_INST; k++) {
        const MsInst *m = &ms_inst[k];
        for (unsigned j = 0; j < m->nres; j++)
            if (m->state[m->ncell + j] == MS_NONE && m->rit[j].bytes && m->cls[m->ncell + j] >= C_PREF &&
                settle_item(k, m, j, pos, r, r_focus) && !ms_blocked(k, m->ncell + j))  /* (not what the cut left out) */
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
int ms_keep_buf;  /* ms_drop leaves the buffer allocated (mapstream_close: a cancelled read still lands in it) */
