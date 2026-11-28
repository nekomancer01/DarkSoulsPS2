/* The residency plan (mapstream_i.h): every item's class from its distance and view, the visibility refresh, the
 * quality scale and the cut to the room. Split from mapstream.c (Polaris 4.1, code moved as it was). */
#include "mapstream_i.h"
float ms_ahead[3], ms_vel[3], ms_last_pos[3];  /* the walking look-ahead point (MS_AHEAD_FRAMES) */
int ms_ahead_on;
int (*ms_vis_hook)(int k, unsigned id);
unsigned (*ms_vis_gen)(int k);
MsVisJob ms_vjob = {-1};

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
    ms_vjob.k = k, ms_vjob.fresh = fresh, ms_vjob.id = 0, ms_vjob.g = ms_vis_gen(k), ms_vjob.nh = ms_vjob.kb = 0;
}
/* one slice of the visibility job (us of EE time); starts the next block's job when none runs */
void ms_vis_step(unsigned us)
{
    if (!ms_vis_hook || !ms_vis_gen)
        return;
    HITCH_BEGIN(v);
    unsigned t0 = ticks(), lim = us * 295;
    for (int pass = 0; pass < MS_INST; pass++) {
        if (ms_vjob.k < 0) {
            for (int k = 0; k < MS_INST && ms_vjob.k < 0; k++)
                if (it_n(&ms_inst[k]) && (ms_inst[k].visn != it_n(&ms_inst[k]) || ms_inst[k].visg != ms_vis_gen(k)))
                    vis_begin(k);
            if (ms_vjob.k < 0)
                break;
        }
        MsInst *m = &ms_inst[ms_vjob.k];
        unsigned n = it_n(m), id = ms_vjob.id;
        if (!m->visv || m->visn != n) {  /* the block closed or grew meanwhile: start it again */
            ms_vjob.k = -1;
            continue;
        }
        unsigned short tick = (unsigned short)(ms_frame >> 2);
        for (; id < n; id++) {
            if (!(id & 63) && ticks() - t0 > lim)
                break;
            int v = it_bytes(m, id) ? ms_vis_hook(ms_vjob.k, id) : 1;
            if (!v && m->visv[id])
                m->vist[id] = ms_vjob.fresh ? (unsigned short)(tick - MS_VIS_HOLD - 1) : tick;  /* seen until now */
            m->visv[id] = (unsigned char)!!v;
            if (!v)
                ms_vjob.nh++, ms_vjob.kb += it_bytes(m, id);
        }
        ms_vjob.id = id;
        if (id < n)
            break;  /* out of time: on from here next frame */
        mapstream_stats.vis_items += ms_vjob.nh - m->vish, mapstream_stats.vis_kb += (ms_vjob.kb >> 10) - m->viskb;
        m->vish = ms_vjob.nh, m->viskb = ms_vjob.kb >> 10;
        m->visg = ms_vjob.g;  /* (a crossing meanwhile: its gen moved on, the next job takes it) */
        ms_vjob.k = -1;
        ms_replan = 1;
        if (ticks() - t0 > lim)
            break;
    }
    HITCH_END(v, HS_MSVIS);
}

/* every near part of cell c is loaded (or outside the visibility: not read) */
int ms_near_in(const MsInst *m, unsigned c)
{
    if (!(m->tab[c].near & ~CELL_PART) || (m->tab[c].near & CELL_PART))
        return 0;
    for (unsigned p = c; p < m->ncell && (p == c || (m->tab[p].near & CELL_PART)); p++)
        if (m->state[p] != MS_READY && !vis_off(m, p))  /* such a part is not read (pvs.c) */
            return 0;
    return 1;
}
static unsigned onelod_n, onelod_kb;  /* coarse items the last plan left to their near level (kept or not) */

/* MS_STEADY: an item that is in or on its way is measured MS_CLS_HYST nearer for its class */
static inline float steady_d2(const MsInst *m, unsigned id, float d2)
{
    if (m->state[id] == MS_NONE || m->state[id] == MS_FAILED || d2 <= 0)
        return d2;
    float d = sqrtf(d2) - MS_CLS_HYST;
    return d > 0 ? d * d : 0;
}

/* MS_QSCALE (mapstream_i.h): the soft radii at the scale now; the hard tier's radii never change */
float ms_qs = 1.0f;
int ms_target;
float ms_qs_now(void) { return MS_QSCALE && !ms_settling ? ms_qs : 1.0f; }
static inline float qs_r0_want(void) { return MS_R0_DRAW + (MS_R0_WANT - MS_R0_DRAW) * ms_qs_now(); }
static inline float qs_grass_want(void)
{
    float r = MS_GRASS_WANT * ms_qs_now();
    return r > MS_QS_GRASS_MIN ? r : MS_QS_GRASS_MIN;
}
/* coarse cells out of view: wanted out to MS_R1_WANT at full scale, down to MS_R1_VIEW (the in-view radius) */
static inline float qs_farc_r(void) { return MS_R1_VIEW + (MS_R1_WANT - MS_R1_VIEW) * ms_qs_now(); }
/* once a plan: pressure lowers the scale, none raises it back */
static void qs_step(void)
{
    static unsigned fails0;
    static int nlog;
    unsigned f = mapstream_stats.room_fails, fr = 0, tot = ilim() * MS_GPP;  /* (the item pages below ilim) */
    for (unsigned p = 0; p < ilim(); p++)
        fr += msa_pfree(p);
    tot = tot ? tot : 1;
    int press = f != fails0 || ((tot - fr) * 100u / tot > MS_QS_FULL && !msa_whole_free());
    fails0 = f;
    float qv = ms_qs;
    if (!MS_QSCALE)
        qv = 1.0f;
    else if (press)
        qv = qv - MS_QS_DOWN > MS_QS_MIN ? qv - MS_QS_DOWN : MS_QS_MIN;
    else
        qv = qv + MS_QS_UP < 1.0f ? qv + MS_QS_UP : 1.0f;
    if ((qv < 1.0f || ms_qs < 1.0f) && (qv == MS_QS_MIN || qv == 1.0f) && qv != ms_qs && nlog++ < 200)
        printf("mapstream: quality scale %.2f (%s): near read-ahead to %.0f m, foliage %.0f m, coarse out of view %.0f m\n",
               (double)qv, press ? "pressure" : "room", (double)(MS_R0_DRAW + (MS_R0_WANT - MS_R0_DRAW) * qv),
               (double)(MS_GRASS_WANT * qv > MS_QS_GRASS_MIN ? MS_GRASS_WANT * qv : MS_QS_GRASS_MIN),
               (double)(MS_R1_VIEW + (MS_R1_WANT - MS_R1_VIEW) * qv));
    ms_qs = qv;
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
        if (ms_settle_plan())
            return d2 < sq(MS_R0_DRAW) ? C_NEAR2 : d2 < sq(MS_R0_KEEP) ? C_KEEP : C_FREE;
        if (ms_ahead_on) {
            float da = near_d2(m, id, ms_ahead);
            if (da < d2)
                *key = da * vk;
            if (d2 >= sq(MS_R0_KEEP))
                return da < sq(qs_r0_want()) ? C_PREF : C_FREE;
            d2 = da < d2 ? da : d2;
        }
        return d2 < sq(MS_R0_FIRST) ? C_NEAR : d2 < sq(MS_R0_DRAW) ? C_NEAR2 : d2 < sq(qs_r0_want()) ? C_PREF : d2 < sq(MS_R0_KEEP) ? C_KEEP : C_FREE;
    }
    unsigned tag = m->rit[id - m->ncell].tag;
    if (tag & MS_RES_TEX) {
        unsigned age = ms_tex_age ? ms_tex_age(k, tag & ~MS_RES_TEX) : ~0u;
        *key = 0;
        if (tag & MS_RES_MIP1)  /* a streamed mip 1 (mapsplit.py MIP1_OUT): what a cell in view draws beyond the mip 0 range */
            return age <= 1 ? MS_MIP1_CLS : age <= MS_TEX_KEEP ? C_KEEP : C_FREE;
        /* MS_MIP0 0 (the default since 2026-11-12, Ame: geometry before textures): full-size textures are never read,
         * the streamed mip 1 stays the sharpest level. Their reads and their 0.5-0.9 MB of the arena go to geometry:
         * ISO iso_burg_taurus, on -> off: map reads 1440 -> 1196, read again within 30 s 510 -> 312, hard-tier "no
         * room" refusals 50-57 -> 26, drive busy 58 -> 50 %, free EE RAM at the shots +0.7 MB, for softer surfaces
         * within the 5-10 m where mip 0 showed (TECH_DEBT world-16). -DMS_MIP0=1: streamed as before. */
        if (!MS_MIP0)
            return age <= MS_TEX_KEEP ? C_KEEP : C_FREE;
        return age <= 1 ? C_PREF : age <= MS_TEX_KEEP ? C_KEEP : C_FREE;
    }
    if (tag & MS_RES_SKY) {
        *key = 0;
        return k == (ms_focus < 0 ? 0 : ms_focus) ? C_PIN0 : C_KEEP;
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
    if (ms_settle_plan() && (tag & MS_RES_LOD) && d2 >= sq(MS_SETTLE_HZN_D)) {
        *key = d2 * vk;
        return C_KEEP;
    }
    if (tag & MS_RES_FAR) {  /* the backdrop: the horizon of the player's block; another block's while in view */
        *key = (d2 < sq(MS_FAR_D) ? d2 : sq(MS_FAR_D)) * vk;
        /* (MS_STEADY: another loaded block's backdrop, Firelink's behind the Burg, keeps C_VIEW2 whether in view or
         * not: as C_FARC it went first whenever room was short and came back from across the disc) */
        return k == (ms_focus < 0 ? 0 : ms_focus) ? C_PIN0 : C_VIEW2;
    }
    *key = d2 * vk;
    if (tag & MS_RES_GRASS)
        return ms_settle_plan() ? C_FREE : d2 < sq(qs_grass_want()) ? C_NEAR2 : d2 < sq(MS_GRASS_KEEP) ? C_KEEP : C_FREE;
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
    return d2 < sq(MS_R1_FIRST) ? C_PIN0 : d2 < sq(MS_R1_PIN) ? C_PIN : d2 < sq(MS_R1_WANT) ? (!view ? (d2 < sq(qs_farc_r()) ? C_FARC : C_KEEP) : d2 < sq(MS_R1_VIEW) ? C_VIEW : C_VIEW2)
                                                           : d2 < sq(MS_R1_KEEP) ? C_KEEP : C_FREE;
}
unsigned ms_cancel_kb;

/* ---- plan ---- */
/* the plan's wanted items, sorted by rank; MS_FILL_PCT: the share (%) of the item pages the plan may fill (granule rounding,
 * read groups not filling their page, ASSUMED) */
MsW ms_want[MS_WANTED];
/* a soft want (lendable, not near / pinned) goes into the plan only if its bytes fit the room the classes before it left
 * (free): a soft want that does not fit is cut here, as a plan cut (hitch_cut), not refused later by the reader as "no
 * room" (zero refusals) */
static inline int ms_plan_soft_fit(unsigned bytes, unsigned free) { return bytes <= free; }
static int w_cmp(const void *a, const void *b)
{
    const MsW *x = a, *y = b;
    return x->cls != y->cls ? y->cls - x->cls : x->key < y->key ? -1 : x->key > y->key;
}
static void plan_(const float *pos);
void ms_plan(const float *pos)
{
    HITCH_BEGIN(p);
    plan_(pos);
    HITCH_END(p, HS_MSPLAN);
}
static void plan_(const float *pos)
{
    unsigned pt[8], np_ = 0;  /* section times (PLAN_PROF) */
    pt[np_++] = ticks();
    ms_nq = 0;
    ms_nowin = C_TOP + 2;
    for (int i = 0; i < ms_nblk; i++)
        if (ms_frame - (unsigned)ms_blk[i].key >= MS_BLOCK_FRAMES)
            ms_blk[i--] = ms_blk[--ms_nblk];
    if (!ms_settling)
        qs_step();
    int retry = ms_failed && ms_frame - ms_fail_frame >= ms_fail_wait;
    ms_failed &= !retry;
    /* classes, then the cut: wanted items in rank order (class, then key) until their bytes fill the item pages; the
     * rest are only kept (never read, first to go). Without the cut the pinned coarse cells of Firelink + three Burg
     * blocks (7 MB within 96 m, 1.9 MB of backdrop) outgrew the arena: no near cell could find room and the Burg
     * stayed at its coarse level, with cells going missing (user report, 2026-11-06) */
    unsigned nw = 0;
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            float key = 0;
            int c = it_bytes(m, id) ? plan_item(k, m, id, pos, &key) : C_FREE, from_src = 0;
            for (int s = 0; WORLD_SOURCES && s < ms_nsrc && c < C_TOP && it_bytes(m, id); s++) {
                /* another streaming source (a respawn point): the highest class any source gives the item, the key of
                 * that source, by the target rule (ms_settle_plan; no walking look-ahead) */
                float k2 = 0;
                int ao = ms_ahead_on;
                ms_ahead_on = 0, ms_target = 1;
                int c2 = plan_item(k, m, id, ms_src[s], &k2);
                ms_ahead_on = ao, ms_target = 0;
                /* at most C_VIEW2, after all of the player's own C_VIEW2 items (MS_SRC_KEY on the key, below): the preload
                 * never outranks the player's hard tier or what is in view; it goes before the player's coarse level
                 * out of view and near read-ahead (the respawn is seconds away, those may never be needed). (C_FARC
                 * was tried: instance 0's C_FARC items are lent to the pools, so a lite set, the respawn's own,
                 * dropped Firelink's respawn cells again. Other blocks' C_VIEW2 items are lendable still: a lite set
                 * may drop those, and they are read again) */
                if (c2 > C_VIEW2)
                    c2 = C_VIEW2;

                if (c2 > c || (c2 == c && k2 < key))
                    c = c2, key = k2, from_src = 1;
            }
            /* no camera cell near the camera sees it (pvs.c); not for a source's item: the camera is not there (with
             * the filter most of a respawn point's preload was cut by the death camera's view) */
            if (c > C_KEEP && !from_src && vis_cut(m, id))
                c = C_KEEP;
            if (m->state[id] != MS_NONE && m->state[id] != MS_FAILED)
                key *= MS_HOLD_K;  /* in (or on its way): ranks as if nearer, so the cut does not flip at its edge */
            if (from_src && c == C_VIEW2)
                key += MS_SRC_KEY;  /* (after the hold factor: in or not, every source item ranks after the player's) */
            m->cls[id] = (unsigned char)c;
            if (m->state[id] == MS_FAILED && retry)
                m->state[id] = MS_NONE;
            if (c >= C_PREF && nw < MS_WANTED)
                ms_want[nw++] = (MsW){(unsigned char)k, (unsigned char)c, (unsigned short)id, key};
        }
    }
    onelod_n = onelod_kb = 0;
    for (int k = 0; !ms_settle_plan() && k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned j = 0; j < m->nres; j++) {
            unsigned tag = m->rit[j].tag, id = m->ncell + j, c = MS_RES_CELL(tag);
            if (!(tag & MS_RES_COARSE) || (tag & (MS_RES_SKY | MS_RES_TEX)) || c >= m->ncell || !ms_near_in(m, c))
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
    unsigned other = mapstream_stats.other_kb << 10, over = msa_pools_on() ? ms_items_other : other > MS_OTHER ? other - MS_OTHER : 0;
    unsigned room = (msa_ipages() + msa_nx()) * MS_PAGE;  /* + the pages borrowed from the heap (MS_BORROW) */
    unsigned lend = 0;
    if (msa_pools_on()) {  /* the pools' room their own blocks leave: only the lowest classes may use it (lendable) */
        unsigned own = other > ms_items_other ? other - ms_items_other : 0, pb = (msa_npages() - msa_mpage0()) * MS_PAGE;
        lend = pb > own ? pb - own : 0;
    }
    /* cap_hi: what the item pages hold, for every class; cap: + the lent room, which only lendable classes reach (with
     * one cap the near parts were planned into room they could never use: near refusals 165 -> 1117 on taurus) */
    unsigned cap_hi = room > over ? (room - over) / 100 * MS_FILL_PCT : 0, cap = cap_hi + lend / 100 * MS_FILL_PCT;
    ms_lend_room = lend;
    {  /* near parts may use lent room: all of it (MS_LEND_NEAR_ALL), else up to MS_LEND_NEAR_KB */
        unsigned ln = MS_LEND_NEAR_ALL || lend < (MS_LEND_NEAR_KB << 10) ? lend : MS_LEND_NEAR_KB << 10;
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
            unsigned lim = lo && near_missing ? cap_hi : cap, fit = lo ? ms_plan_soft_fit(b, lim > sum ? lim - sum : 0) : 1;
            sum += b, sum_hi += lo ? 0 : b;  /* (counted when cut too, as before: past the cap, the rest is cut) */
            if ((!lo && (sum > cap || sum_hi > cap_hi)) || (lo && (sum > cap || !fit))) {
                m->cls[id] = C_KEEP;
                cut++, cut_near += id < m->ncell && (ms_want[i].cls == C_NEAR || ms_want[i].cls == C_NEAR2);
                continue;
            }
        }
        if (m->state[ms_want[i].id] == MS_NONE && !ms_blocked(ms_want[i].k, ms_want[i].id))
            ms_q_add(ms_want[i].k, ms_want[i].id, ms_want[i].cls, ms_want[i].key);
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
                   kb[C_VIEW], kb[C_NEAR], kb[C_NEAR2], kb[C_VIEW2], kb[C_FARC], kb[C_PREF], ms_stale_kb, ms_cancel_kb,
                   ms_defrag_moves, ms_defrag_kb, onelod_n, onelod_kb);
            printf("mapstream: near wait %u ms (dev %u) from wanted to landed\n", (unsigned)ms_lat_avg, (unsigned)ms_lat_dev);
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
    if (ms_st_wait) {  /* after a settle: is the near level within MS_R0_FIRST of the feet in? */
        unsigned left = 0;
        for (int k = 0; k < MS_INST; k++) {
            const MsInst *m = &ms_inst[k];
            for (unsigned c = 0; c < m->ncell; c++)
                if ((m->tab[c].near & ~CELL_PART) && !(m->tab[c].near & CELL_PART) && m->tab[c].bytes &&
                    near_d2(m, c, pos) < sq(MS_R0_FIRST) && !vis_cut(m, c) && !ms_near_in(m, c))
                    left++;
        }
        unsigned t = ms_clk_ms - ms_st_t1;
        if (!left || t > 60000) {
            printf("mapstream: after the settle (%u ms): first near cell %d ms, near level within %.0f m %s %u ms "
                   "(%u cells left)\n", ms_st_t1 - ms_st_t0, ms_st_first ? (int)ms_st_first - 1 : -1, (double)MS_R0_FIRST,
                   left ? "still missing after" : "in at", t, left);
            ms_st_wait = 0;
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
    ms_lent_recount();  /* classes are fixed until the next plan: the lent counts match ms_drop's from here */
    for (int k = 0; !ms_settling && k < MS_INST; k++) {  /* near parts the cut left wanted and not in: their wait starts */
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; m->wq && id < m->ncell; id++)
            if (m->cls[id] < C_PREF || m->state[id] == MS_READY)
                m->wq[id] = 0;
            else if (!m->wq[id])
                m->wq[id] = ms_clk_ms + 1;
    }
    pt[np_++] = ticks();
    if ((pt[np_ - 1] - pt[0]) / 294912 >= 40)  /* a slow plan: where it went (classify, sort, cut, cancel+log, settle check, free) */
        printf("mapstream: slow plan %u ms: %u %u %u %u %u %u ms, %u wanted\n", (pt[np_ - 1] - pt[0]) / 294912,
               (pt[1] - pt[0]) / 294912, (pt[2] - pt[1]) / 294912, (pt[3] - pt[2]) / 294912, (pt[4] - pt[3]) / 294912,
               (pt[5] - pt[4]) / 294912, (pt[6] - pt[5]) / 294912, nw);
}
