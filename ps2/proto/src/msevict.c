/* Making room in the arena for map items and for mapstream_alloc (mapstream_i.h): drops, cleared windows, moved
 * groups (defrag, tidy), the pools' lent items, borrowed heap pages given back. Uses the page arena only through
 * msarena.h. Split from msarena.c (Polaris 4.1, code moved as it was). */
#include "mapstream_i.h"
/* MS_POOLS: bytes mapstream_alloc placed in item pages (the plan leaves them out) */
unsigned ms_items_other;
static int pool_busy;
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
unsigned ms_lend_room;
unsigned ms_lent_near;  /* bytes of near parts placed in pool pages (approximate: decremented on drop) */

static unsigned x_want;  /* heap free a caller of mapstream_heap_release waits for (x_update drains pages until) */
static unsigned xstat_borrow, xstat_back, xstat_drop_kb;


/* free the group of item id */
int ms_why;  /* why the next ms_drop drops (MS_WHY_*, set by its caller; back to MS_WHY_FREE after) */
void ms_drop(int k, unsigned id)
{
    MsInst *m = &ms_inst[k];
    unsigned a, e = grp_of(m, id, &a);
    if (msa_pools_on() && ms_lent_n) {  /* (MS_LEND: one in the pools' pages) */
        unsigned g = msa_gran(m->buf[a]);
        int in_pool = g != ~0u && g >= msa_mpage0() * MS_GPP && g < msa_npages() * MS_GPP;
        ms_lent_n -= in_pool;
        if (in_pool && (m->cls[a] == C_NEAR || m->cls[a] == C_NEAR2)) {
            unsigned b = grp_bytes(m, a, e);
            ms_lent_near = ms_lent_near > b ? ms_lent_near - b : 0;
        }
    }
    if (!ms_keep_buf)
        msa_free(m->buf[a], grp_bytes(m, a, e));
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
/* The lent counts from what lies in the pool pages now (once a plan, after its classes). ms_lent_near is added by the
 * class a group has when it is placed and taken off by the class it has when it goes (ms_drop), and the plan changes
 * classes in between: a near part lent as C_NEAR2 and dropped as C_KEEP stayed counted (near lending shut off for
 * good once the count reached the lent room), one lent as C_PREF and dropped as C_NEAR2 was taken off uncounted.
 * Classes change only in the plan, so from here to the next plan the running counts are exact. */
void ms_lent_recount(void)
{
    static unsigned log_frame;
    if (!msa_pools_on())
        return;
    unsigned n = 0, nb = 0, g0 = msa_mpage0() * MS_GPP, g1 = msa_npages() * MS_GPP;
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; m->state && id < it_n(m); id++) {
            unsigned a, e;
            if ((m->state[id] != MS_READY && m->state[id] != MS_READING) || (e = grp_of(m, id, &a), a != id) || !m->buf[id])
                continue;
            unsigned g = msa_gran(m->buf[id]);
            if (g == ~0u || g < g0 || g >= g1)
                continue;
            n++;
            if (m->cls[id] == C_NEAR || m->cls[id] == C_NEAR2)
                nb += grp_bytes(m, a, e);
        }
    }
    unsigned dn = n > ms_lent_n ? n - ms_lent_n : ms_lent_n - n, db = nb > ms_lent_near ? nb - ms_lent_near : ms_lent_near - nb;
    if ((dn > 1 || db > MS_PAGE) && (!log_frame || ms_frame - log_frame >= 300)) {
        log_frame = ms_frame;
        printf("mapstream: lent recount: n %u -> %u, near KB %u -> %u\n", ms_lent_n, n, ms_lent_near >> 10, nb >> 10);
    }
    ms_lent_n = n, ms_lent_near = nb;
}
unsigned ms_rehome_n, ms_rehome_kb;
static int lend_rehome(int k, unsigned a, unsigned e)
{
    MsInst *m = &ms_inst[k];
    unsigned n = grp_bytes(m, a, e);
    if (!MS_LEND_REHOME || n > MS_PAGE)
        return 0;
    unsigned char *src = m->buf[a], *dst = msa_alloc_k(n, MS_K_SHORT);
    if (!dst && ms_make_window(1, (n + MS_GRAN - 1) / MS_GRAN, C_NEAR2, NULL))
        dst = msa_alloc_k(n, MS_K_SHORT);
    if (!dst)
        return 0;
    memcpy(dst, src, n);
    SyncDCache(dst, dst + n - 1);  /* the VIF / GIF DMA reads RAM */
    for (unsigned j = a; j < e; j++) {
        m->buf[j] = dst + ((unsigned char *)m->buf[j] - src);
        if (it_tex(m, j) && ms_tex_set)
            ms_tex_set(k, m->rit[j - m->ncell].tag & ~MS_RES_TEX, m->buf[j]);
    }
    msa_free(src, n);
    ms_lent_n = ms_lent_n ? ms_lent_n - 1 : 0;
    ms_lent_near = ms_lent_near > n ? ms_lent_near - n : 0;
    ms_rehome_n++, ms_rehome_kb += n >> 10;
    printf("mapstream: lent near moved out of a lite set's way: item %d.%u (%u KB)\n", k, a, n >> 10);
    return 1;
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
            unsigned g = msa_gran(m->buf[id]);
            if (g == ~0u || g < p0 * MS_GPP || g >= p1 * MS_GPP)
                continue;
            if (keep_near && (m->cls[id] == C_NEAR || m->cls[id] == C_NEAR2))
                continue;
            if (!ms_settling && ms_grp_chain(m, id))  /* (a settle waited for VU1: no chain reads the arena; without this the
                                                 * lite set waited out the settle: "settle stopped", taurus 2026-11-09) */
                busy++;
            else {
                if (m->cls[id] == C_NEAR || m->cls[id] == C_NEAR2) {
                    if (lend_rehome(k, id, grp_of(m, id, &a)))
                        continue;
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
            unsigned g = msa_gran(m->buf[id]);
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
            ok = msa_pfree(p0 + j) + ig[j] == MS_GPP && !(bz[j] & 2);  /* only lent items and free room, none that stays */
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
            unsigned g = msa_gran(m->buf[id]);
            if (g == ~0u || g < w0 || g >= w1)
                continue;
            if (!ms_settling && ms_grp_chain(m, id)) {
                busy++;
                continue;
            }
            if (m->cls[id] == C_NEAR || m->cls[id] == C_NEAR2) {
                if (lend_rehome(k, id, grp_of(m, id, &a)))
                    continue;
                hitch_lent_near();
                printf("mapstream: lent near dropped for a lite set: item %d.%u (%u KB)\n", k, id,
                       grp_bytes(m, id, grp_of(m, id, &a)) >> 10);
            }
            ms_why = MS_WHY_LENT, ms_drop(k, id);
        }
    }
    return busy;
}
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
    /* scores per page (msarena.c keeps them): bits 0-7 highest class + 1 of its clearable groups, 0x200 not clearable;
     * cost; granules of clearable groups (a page whose used granules are not all clearable groups holds a read in
     * flight or a mapstream_alloc block) */
    unsigned *pcls = msa_scratch(0), *pcost = msa_scratch(1), *pitem = msa_scratch(2);
    unsigned lim = ng_need || msa_pools_on() ? ilim() : msa_npages();  /* a read's window lies where items may go (MS_POOLS: and
                                                             * mapstream_alloc's beyond the pools) */
    if (!need || need > msa_npages() || !pcls || (need == 1 && below >= ms_nowin))
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
            unsigned g0 = msa_gran(m->buf[a]), ng = (n + MS_GRAN - 1) / MS_GRAN;
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
        if (pitem[p] != MS_GPP - msa_pfree(p))
            pcls[p] |= 0x200;
        if (need == 1 && (pcls[p] & 0x200) && pitem[p] && msa_pfree(p) + pitem[p] >= ng_need)
            pcls[p] = (pcls[p] & ~0x200u) | 0x400;  /* partly clearable: after the whole pages */
    }
    int best = -1;
    unsigned long long bestc = ~0ull;
    for (unsigned p = lim >= need ? lim - need + 1 : 0; p-- > 0;) {
        unsigned hi = 0;
        unsigned long long c = 0;
        unsigned pq;
        for (pq = p; pq < p + need; pq++) {
            if (pcls[pq] & 0x200)
                break;
            hi = (pcls[pq] & 0x4ff) > hi ? pcls[pq] & 0x4ff : hi;
            c += pcost[pq];
        }
        if (pq < p + need)
            continue;
        unsigned long long score = (unsigned long long)hi << 40 | c;
        if (score < bestc)
            bestc = score, best = (int)p;
    }
    for (unsigned p = msa_npages(); need == 1 && ng_need && p < ntot(); p++) {  /* a read: borrowed pages too */
        if (!msa_x_on(p - msa_npages()) || msa_x_ret(p - msa_npages()) || (pcls[p] & 0x200))
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
            unsigned o = msa_gran(m->buf[a]);
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
unsigned ms_defrag_moves, ms_defrag_kb;

/* old copies no chain can read any more go back */
void ms_pend_release(void)
{
    for (int i = 0; i < ms_npend; i++)
        if (ms_chains_done || !ms_dprev || (int)(ms_dprev - pend[i].fr) > 0) {
            msa_mark(pend[i].g0, pend[i].ng, 0);
            pend[i--] = pend[--ms_npend];
        }
}

/* Empty `need` whole pages below `lim` by moving their groups elsewhere: the window whose used granules are all
 * movable groups (loaded, inside one page) with the fewest of them. 1 when every group of the window moved (its pages
 * free once the old copies go back, pend_release), 0 when no window qualifies or a group found no hole. */
int ms_defrag(unsigned need, unsigned lim, unsigned max_kb) { return ms_defrag_(need, lim, max_kb, 1); }
/* pool 0: the moved groups stay in the item pages (MS_TIDY: lent pages go back to a lite set, near parts with them) */
int ms_defrag_(unsigned need, unsigned lim, unsigned max_kb, int pool)
{
    unsigned *pitem = msa_scratch(2);
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
            unsigned n = grp_bytes(m, a, e), g0 = msa_gran(m->buf[a]);
            unsigned ng = (n + MS_GRAN - 1) / MS_GRAN;
            if (g0 / MS_GPP == (g0 + ng - 1) / MS_GPP)
                pitem[g0 / MS_GPP] += ng;
        }
    }
    unsigned fr = msa_items_free();
    int to_pool = pool && MS_DEFRAG_LEND && msa_pools_on() && lim == ilim();  /* (an item read's window, not a lite set's) */
    if (to_pool)  /* (the pools' idle pages take moved groups too) */
        for (unsigned p = msa_mpage0(); p < msa_npages(); p++)
            fr += msa_pfree(p);
    int best = -1;
    unsigned bused = ~0u;
    for (unsigned p = lim - need + 1; p-- > 0;) {
        unsigned used = 0, pq;
        for (pq = p; pq < p + need; pq++) {
            unsigned u = MS_GPP - msa_pfree(pq);
            if (pitem[pq] != u)
                break;  /* a read in flight, an old copy or a mapstream_alloc block */
            used += u;
        }
        if (pq < p + need || !used)
            continue;
        unsigned wfree = 0;
        for (pq = p; pq < p + need; pq++)
            wfree += pq < ilim() ? msa_pfree(pq) : 0;
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
                unsigned n = grp_bytes(m, a, e), g0 = msa_gran(src), ng = (n + MS_GRAN - 1) / MS_GRAN;
                if (g0 < w0 || g0 >= w1)
                    continue;
                unsigned char *dst = ms_npend < MS_DEFRAG_PEND ? msa_alloc_ex(n, (unsigned)best, (unsigned)best + need) : NULL;
                if (!dst && to_pool && ms_npend < MS_DEFRAG_PEND && n <= MS_PAGE &&
                    (dst = msa_alloc_lend(n))) {  /* lent from now on: a lite set's room takes it back (ms_drop) */
                    ms_lent_n++;
                    if (m->cls[a] == C_NEAR || m->cls[a] == C_NEAR2)
                        ms_lent_near += n;
                }
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

/* MS_TIDY (mapstream_i.h): one page emptied on a quiet frame while the whole free item pages are under the low mark,
 * on until the high mark */
unsigned ms_tidy_moves, ms_tidy_kb;
void ms_tidy(void)
{
    static int on;
    static unsigned last_log, fail_w = ~0u, fail_f;
    if (!MS_TIDY || !msa_ready() || ms_settling || ms_npend)
        return;
    unsigned w = msa_whole_free();
    if (w == fail_w && ms_frame - fail_f < MS_SCAN_FRAMES)
        return;  /* nothing could move at this count: not every frame (the search ranks every loaded group) */
    on = w < MS_TIDY_LO || (on && w < MS_TIDY_HI);
    if (!on)
        return;
    unsigned m0 = ms_defrag_moves, k0 = ms_defrag_kb;
    if (ms_defrag_(1, ilim(), MS_PAGE >> 10, 0)) {
        ms_tidy_moves += ms_defrag_moves - m0, ms_tidy_kb += ms_defrag_kb - k0;
        if (ms_frame - last_log >= 300) {  /* ~10 s */
            last_log = ms_frame;
            printf("mapstream: tidy: %u whole free item pages, page emptied (%u groups %u KB moved so far)\n", w,
                   ms_tidy_moves, ms_tidy_kb);
        }
    } else {
        fail_w = w, fail_f = ms_frame;  /* no page to empty into the holes there are */
    }
}

/* ---- borrowed pages: give back, borrow (MS_BORROW, comment at xpage) ---- */
/* Give back the borrowed page whose items can all go now (loaded groups no chain reads: no read in flight, no old
 * defrag copy), the emptiest first. 1 when a page went back to the heap. */
static int x_release(int force)
{
    unsigned ok[MS_XMAX];
    if (!msa_nx())
        return 0;
    memset(ok, 0, sizeof ok);
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                continue;
            unsigned g0 = msa_gran(m->buf[a]);
            if (g0 / MS_GPP < msa_npages() || g0 == ~0u || ms_grp_chain(m, id))
                continue;
            ok[g0 / MS_GPP - msa_npages()] += (grp_bytes(m, a, e) + MS_GRAN - 1) / MS_GRAN;
        }
    }
    int best = -1;
    for (unsigned i = 0; i < MS_XMAX; i++) {
        unsigned p = msa_npages() + i;
        if (msa_x_on(i) && ok[i] == MS_GPP - msa_pfree(p) && (best < 0 || msa_pfree(p) > msa_pfree(msa_npages() + best)))
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
            if (msa_x_on(i) && msa_x_ret(i))
                return 0;  /* one at a time: that one is still draining */
        for (unsigned i = 0; i < MS_XMAX; i++)
            if (msa_x_on(i) && (best < 0 || msa_pfree(msa_npages() + i) > msa_pfree(msa_npages() + best)))
                best = (int)i;
        if (best < 0 || ms_npend > MS_DEFRAG_PEND - 16)
            return 0;
        unsigned p = msa_npages() + best, w0 = p * MS_GPP, w1 = w0 + MS_GPP;
        msa_x_set_ret(best, 1);
        for (int k = 0; k < MS_INST; k++) {
            MsInst *m = &ms_inst[k];
            for (unsigned id = 0; id < it_n(m); id++) {
                unsigned a, e;
                if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                    continue;
                unsigned g0 = msa_gran(m->buf[a]);
                if (g0 < w0 || g0 >= w1)
                    continue;
                unsigned n = grp_bytes(m, a, e), ng = (n + MS_GRAN - 1) / MS_GRAN;
                int chain = ms_grp_chain(m, id);
                xstat_drop_kb += n >> 10;
                hitch_drop(ms_grp_cls(m, id) >= C_PREF, n);
                ms_why = MS_WHY_BORROW, ms_drop(k, id);
                if (chain && ms_npend < MS_DEFRAG_PEND) {  /* the granules stay used until no chain reads them */
                    msa_mark(g0, ng, 1);
                    pend[ms_npend++] = (typeof(pend[0])){g0, ng, ms_frame};
                }
            }
        }
        return 0;
    }
    unsigned p = msa_npages() + best, w0 = p * MS_GPP, w1 = w0 + MS_GPP;
    for (int k = 0; k < MS_INST; k++) {
        MsInst *m = &ms_inst[k];
        for (unsigned id = 0; id < it_n(m); id++) {
            unsigned a, e;
            if (m->state[id] != MS_READY || (e = grp_of(m, id, &a), a != id))
                continue;
            unsigned g0 = msa_gran(m->buf[a]);
            if (g0 >= w0 && g0 < w1) {
                xstat_drop_kb += grp_bytes(m, a, e) >> 10;
                hitch_drop(ms_grp_cls(m, id) >= C_PREF, grp_bytes(m, a, e));
                ms_why = MS_WHY_BORROW, ms_drop(k, id);
            }
        }
    }
    if (msa_pfree(p) != MS_GPP)  /* cannot happen (ok[] counted every used granule) */
        return 0;
    msa_x_back(best), xstat_back++;
    mapstream_stats.lent_kb = msa_nx() * (MS_PAGE >> 10);
    return 1;
}

/* Once per frame: borrow a page while the item pages are (nearly) full and the heap has room over the floor; give
 * one back while the heap is under it */
void ms_x_update(void)
{
    if (!msa_ready())
        return;
    unsigned hf = msa_heap_free(), floor = x_want > MS_BORROW_FLOOR ? x_want : MS_BORROW_FLOOR;
    if (!msa_nx() || hf >= x_want)
        x_want = 0;
    if (msa_nx() && hf < floor) {  /* up to 4 pages a frame: back over the floor within a few frames */
        for (int i = 0; i < 4 && hf < floor && x_release(1); i++)
            hf = msa_heap_free();
        return;
    }
    unsigned fr = msa_items_free();
    if (msa_nx() >= MS_XMAX || fr >= 2 * MS_GPP || hf < MS_BORROW_FLOOR + MS_BORROW_HYST + MS_PAGE)
        return;
    for (unsigned j = 0; j < MS_XMAX; j++)
        if (msa_x_ret(j))
            return;  /* one is being given back: none borrowed meanwhile */
    if (msa_x_borrow() < 0)
        return;
    xstat_borrow++;
    mapstream_stats.lent_kb = msa_nx() * (MS_PAGE >> 10);
    ms_nowin = C_TOP + 2;  /* new room: windows may be found again */
}

unsigned mapstream_heap_release(unsigned bytes)
{
    unsigned n = 0, ms_want = (bytes + MS_PAGE - 1) / MS_PAGE + 1;
    while (n < ms_want && x_release(0))
        n++;
    if (n < ms_want && msa_nx()) {  /* the rest drains over the next frames (pages with drawn cells: x_release) */
        unsigned t = msa_heap_free() + (ms_want - n) * MS_PAGE;
        x_want = t > x_want ? t : x_want;
        x_release(1);
    }
    if (n)
        printf("mapstream: gave %u borrowed pages back to the heap for %u KB (%u left)\n", n, bytes >> 10, msa_nx());
    return n;
}
int mapstream_pool_wait(void) { return pool_busy; }
unsigned mapstream_lend_early(void) { unsigned n = early_n; early_n = 0; return n; }

int mapstream_pool_prepare(int pool, unsigned n)
{
    if (!msa_pools_on() || !ms_lent_n)
        return 1;
    unsigned p0 = pool == MS_POOL_STRUCT ? msa_spage0() : msa_mpage0(), p1 = pool == MS_POOL_STRUCT ? msa_npages() : msa_spage0();
    void *p = msa_alloc_range(n, p0, p1);
    if (p) {
        msa_free(p, n);
        return 1;
    }
    unsigned before = ms_lent_n;
    lend_reclaim_for(n, p0, p1, 1);
    early_n += before - ms_lent_n;
    return 0;
}

void *mapstream_alloc_pool(unsigned n, int pool)
{
    if (!mapstream_arena() || !msa_pools_on())
        return NULL;
    unsigned p0 = pool == MS_POOL_STRUCT ? msa_spage0() : msa_mpage0(), p1 = pool == MS_POOL_STRUCT ? msa_npages() : msa_spage0();
    void *p = msa_alloc_range(n, p0, p1);
    pool_busy = 0;
    if (!p && ms_lent_n) {  /* MS_LEND: the lent items go */
        if (lend_reclaim_for(n, p0, p1, 0) && !(p = msa_alloc_range(n, p0, p1))) {
            /* some are in a chain: cells drawn every frame never leave it (a lite set waited 13.8 s behind them,
             * taurus 2026-11-09), so wait for VU1 once and drop them all */
            ot_wait();
            ms_chains_done = 1;
            pool_busy = lend_reclaim_for(n, p0, p1, 0) > 0;
            ms_chains_done = 0;
        }
        if (!p)
            p = msa_alloc_range(n, p0, p1);
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
    if (!mapstream_arena() || !msa_pools_on() || !ng || ng > MS_GPP)
        return mapstream_alloc_pool(n, pool);
    unsigned p0 = pool == MS_POOL_STRUCT ? msa_spage0() : msa_mpage0(), p1 = pool == MS_POOL_STRUCT ? msa_npages() : msa_spage0();
    void *p = msa_alloc_small(n, p0, p1);
    if (p) {
        mapstream_stats.other_kb += (n + 1023) >> 10;
        return p;
    }
    return mapstream_alloc_pool(n, pool);  /* (no hole: lent items may go) */
}

/* the top of the item pages mapstream_alloc may clear (all pages without pools) */
static inline unsigned alloc_top(void) { return msa_pools_on() ? msa_ipages() : msa_npages(); }
static void *alloc_items(unsigned n)
{
    void *p = msa_alloc_range(n, 0, alloc_top());
    if (p && msa_pools_on())
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
        ot_wait();
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
        ot_wait();
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
                ms_why = MS_WHY_CANCEL, ms_drop(ms_rd[i].k, ms_rd[i].id);
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

/* mapstream_alloc without touching what the player sees: the misc pool, free item pages, windows of items below the
 * pinned level; NULL rather than clear near, view or pinned cells (enemy model temporaries: ENEMY_TMP_GENTLE) */
void *mapstream_alloc_gentle(unsigned n)
{
    if (!mapstream_arena())
        return NULL;
    void *p = mapstream_alloc_pool(n, MS_POOL_MISC);
    if (p)
        return p;
    unsigned need = (n + MS_PAGE - 1) / MS_PAGE, kb = 0;
    if (!(p = alloc_items(n)) && (ms_make_window(need, 0, C_NEAR2, &kb) || ms_make_window(need, 0, C_PIN, &kb)))
        p = alloc_items(n);
    if (kb)
        printf("mapstream: gentle alloc %u KB %s, %u KB of cells dropped\n", n >> 10, p ? "ok" : "FAILED", kb);
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
        for (unsigned i = 0; i < msa_npages(); i++)
            fr += msa_pfree(i);
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
    if (msa_pools_on() && msa_gran(p) < msa_ipages() * MS_GPP)
        ms_items_other -= (n + MS_GRAN - 1) / MS_GRAN * MS_GRAN;
    msa_free(p, n);
    mapstream_stats.other_kb -= (n + 1023) >> 10;
    ms_replan = 1;  /* room for what was refused */
    ms_nblk = 0, ms_nowin = C_TOP + 2;
}
