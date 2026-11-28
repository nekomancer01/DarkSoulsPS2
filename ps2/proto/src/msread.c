/* The streamer's reads (mapstream_i.h): the queue in rank / disc order, issuing groups into the arena, landing, and
 * the flow / run / thrash logs. Split from mapstream.c (Polaris 4.1, code moved as it was). */
#include "mapstream_i.h"
/* how long a near part waits from the plan that first wants it to its landing (ms: running average and mean
 * deviation; the reads queued and in flight before it count), logged with the plan's class sizes. Budget doc
 * proposal 5 (2026-11-08) set the look-ahead from it (speed x wait, the strip ahead ranked first): on world_sprint
 * near pop-in went 69-71 -> 78-81 against the fixed look-ahead. The pop-ins there wait 3-40 s inside the draw
 * radius, for room in the arena, not for the disc, so the look-ahead stays as it is. */
float ms_lat_avg = 800.0f, ms_lat_dev = 200.0f;
int ms_nrd;
static unsigned land_t;
MsQ ms_q[MS_QUEUE];
int ms_nq;
MsQ ms_blk[MS_BLOCKED];
int ms_nblk;
int ms_room_fail, ms_failed, ms_evict_left, ms_defrag_left;
static int scan_k = -1;
static unsigned scan_end;
static float rd_ms = 80.0f;  /* a read's service time (running average, ms): MS_DEADLINE's estimate */
static unsigned dl_panic, dl_scan, dl_gap, dl_first;  /* picks, logged with the runs line */
static int q_scan_pick(int ra_ok, int *first);
static unsigned flow_rd[3], flow_kb[3];
static unsigned thr_n[3][MS_WHY_N], thr_kb[3], thr_cls[C_TOP + 1];
static int q_before(const MsQ *a, int cls, float key)
{
    int ra = q_rank(a->cls), rc = q_rank(cls);
    return ra > rc || (ra == rc && a->key <= key);
}
/* an entry issue() took at index qi and could not send goes back there: the queue stays in rank order (MS_SCAN picks
 * from the middle; put back at the head, a read-ahead entry led the queue and q_scan_pick held every read, the hard
 * tier's too, until the next plan) */
static void q_put_back(const MsQ *w, int qi)
{
    memmove(ms_q + qi + 1, ms_q + qi, (ms_nq++ - qi) * sizeof *ms_q);
    ms_q[qi] = *w;
}
void ms_q_add(int k, unsigned id, int cls, float key)
{
    int i = ms_nq < MS_QUEUE ? ms_nq++ : MS_QUEUE;
    if (i == MS_QUEUE) {
        if (q_before(&ms_q[MS_QUEUE - 1], cls, key))
            return;
        i = MS_QUEUE - 1;
    }
    while (i > 0 && !q_before(&ms_q[i - 1], cls, key))
        ms_q[i] = ms_q[i - 1], i--;
    ms_q[i] = (MsQ){k, id, cls, key};
}
int ms_blocked(int k, unsigned id)
{
    for (int i = 0; i < ms_nblk; i++)
        if (ms_blk[i].k == k && ms_blk[i].id == id)
            return 1;
    return 0;
}
void ms_collect(int i, int r)
{
    MsRead w = ms_rd[i];
    memmove(ms_rd + i, ms_rd + i + 1, (--ms_nrd - i) * sizeof *ms_rd);
    mapstream_stats.pending = ms_nrd;
    MsInst *m = &ms_inst[w.k];
    if (w.cxl) {  /* cancelled (MS_CANCEL): the buffer is free once the read stopped */
        ms_why = MS_WHY_CANCEL, ms_drop(w.k, w.id);
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
            ms_stale_kb += w.bytes >> 10;
        if (ms_st_wait && !ms_st_first && !it_res(m, w.id))
            ms_st_first = ms_clk_ms - ms_st_t1 + 1;
        ms_fail_wait = 0;
        mapstream_stats.ms_last = (now - t0) / 294912;
        rd_ms += ((float)(mapstream_stats.ms_last < 2000 ? mapstream_stats.ms_last : 2000) - rd_ms) * 0.125f;
        {  /* a near part's wait from the plan that wanted it to its landing (the reads queued and in
            * flight before it count); others (or one wanted before a settle) from the issue */
            float l = (float)(now - w.t) / 294912.0f;
            if (!it_res(m, w.id) && m->wq && m->wq[w.id] && !ms_settling) {
                l = (float)(ms_clk_ms + 1 - m->wq[w.id]);
                for (unsigned j = w.id; j < w.id + w.n; j++)
                    m->wq[j] = 0;
            }
            l = l > 5000.0f ? 5000.0f : l;
            ms_lat_avg += (l - ms_lat_avg) * 0.125f;
            ms_lat_dev += (fabsf(l - ms_lat_avg) - ms_lat_dev) * 0.125f;
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
        ms_failed = 1, ms_fail_frame = ms_frame;
        ms_fail_wait = !ms_fail_wait ? MS_FAIL_FRAMES : ms_fail_wait < MS_FAIL_FRAMES_MAX ? 2 * ms_fail_wait : ms_fail_wait;
    }
}
static unsigned rs_stop[RS_N], rs_runs, rs_items, rs_reads, rs_contig, rs_kb, rs_frame;
static unsigned rs_last_end, rs_last_k = ~0u;
static unsigned rk[MS_INST][RK_N + 1];
static void rs_log(void)
{
    if (ms_frame - rs_frame < MS_RUNLOG_FRAMES)
        return;
    rs_frame = ms_frame;
    if (!rs_runs)
        return;
    printf("mapstream: runs %u (%u items, %u KB), reads %u, %u start where the last ended; stops", rs_runs, rs_items,
           rs_kb, rs_reads, rs_contig);
    for (int i = 0; i < RS_N; i++)
        printf(" %s %u", rs_name[i], rs_stop[i]);
    printf("; picks panic %u first %u scan %u gap %u (read %u ms)", dl_panic, dl_first, dl_scan, dl_gap, (unsigned)rd_ms);
    dl_panic = dl_first = dl_scan = dl_gap = 0;
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

/* the queue's head into the arena and read (its missing wanted neighbours in mapres.bin along, up to MS_RUN bytes, as
 * groups of at most a page); 0 when nothing more goes out this frame */
int ms_issue(void)
{
    rs_log();
    int ra_ok = 1, first = 0;
    if (MS_SCAN) {  /* read-ahead: at most MS_RA_INFLIGHT of them in flight */
        int ra = 0;
        for (int i = 0; i < ms_nrd; i++)
            ra += !ms_rd[i].cxl && ms_rd[i].cls < C_NEAR2;
        ra_ok = ra < MS_RA_INFLIGHT;
    }
    int qi = MS_SCAN ? q_scan_pick(ra_ok, &first) : 0;
    if (qi < 0)
        return 0;
    MsQ w = ms_q[qi];
    memmove(ms_q + qi, ms_q + qi + 1, (--ms_nq - qi) * sizeof *ms_q);
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
                   m->cls[id] < C_PREF ? RS_CLASS : ms_blocked(w.k, id) ? RS_BLOCKED : -1;
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
        /* MS_LIFE: the page kind it goes to (the pinned coarse level stays, near parts and read-ahead go soon) */
        unsigned gtag = it_res(m, g) ? m->rit[g - m->ncell].tag : 0;
        int kind = (gtag & MS_RES_TEX) && gb <= (MS_SMALL_KB << 10) ? MS_K_SMALL :
                   it_res(m, g) && !(gtag & MS_RES_TEX) && (w.cls == C_PIN0 || w.cls == C_PIN || w.cls == C_VIEW) ? MS_K_LONG : MS_K_SHORT;
        if (nr && msa_pools_on() && gb <= MS_PAGE &&
            !(it_res(m, g) && (m->rit[g - m->ncell].tag & (MS_RES_SKY | MS_RES_FAR))))
            lo = 1;
        void *p = lo && !nr ? msa_alloc_lend(gb) : NULL;
        int lent = 0;  /* 1 lent, 2 lent as a near part: undone when the read cannot go out below */
        if (p)
            ms_lent_n++, lent = 1;
        else
            p = gb <= MS_PAGE ? msa_alloc_k(gb, kind) : msa_alloc_range(gb, 0, ilim());
        if (!p && lo && nr && ms_lent_near + gb <= (MS_LEND_NEAR_ALL ? ms_lend_room : MS_LEND_NEAR_KB << 10) &&
            (p = msa_alloc_lend(gb)))
            ms_lent_n++, ms_lent_near += gb, lent = 2;
        /* MS_AREA_POOL: the pinned and in-view levels in the spare pool pages, after the item pages (the export counts
         * them as room where the blocks wanted need less pool: mapstream.h) */
        if (!p && ms_spare_p && gb <= MS_PAGE && !nr && !lendable(w.cls) &&
            !(it_res(m, g) && (m->rit[g - m->ncell].tag & (MS_RES_SKY | MS_RES_FAR))) &&
            (p = msa_alloc_small(gb, ms_spare0, ms_spare0 + ms_spare_p)))
            ms_lent_n++, lent = 3;
        if (!p && sent)
            break;  /* the rest waits for its turn */
        if (!p && ms_evict_left > 0) {
            ms_evict_left--;
            /* one page (or the pages of a group over a page) of items below this one's class, not drawn this frame
             * or the last */
            if (ms_make_window((gb + MS_PAGE - 1) / MS_PAGE, (gb + MS_GRAN - 1) / MS_GRAN, w.cls, NULL))
                p = gb <= MS_PAGE ? msa_alloc_k(gb, kind) : msa_alloc_range(gb, 0, ilim());
        }
        if (!p && !sent && gb <= MS_PAGE && ms_nq < MS_QUEUE) {
            /* nothing to clear: empty a page by moving its groups (MS_DEFRAG); the read waits at the head of the queue
             * for the old copies to go (two draws), a few frames at most */
            int moving = ms_npend && ms_frame - ms_pend_frame <= MS_DEFRAG_WAIT;
            if (!moving && ms_defrag_left > 0) {
                ms_defrag_left--;
                moving = ms_defrag(1, ilim(), MS_DEFRAG_KB);
            }
            if (moving) {
                q_put_back(&w, qi);
                return 0;
            }
        }
        if (!p) {
            if (ms_evict_left <= 0 && !sent && ms_nq < MS_QUEUE) {  /* no clearing left this frame: first again next frame */
                q_put_back(&w, qi);
                return 0;
            }
            if (!ms_room_fail++) {
                unsigned fr = 0;
                for (unsigned i = 0; i < ntot(); i++)
                    fr += msa_pfree(i);
                /* ", hard": an item of residency.h's hard tier (the near level within RES_NEAR_R, the pinned coarse
                 * level within RES_PIN_R) the export budgeted for: tools/budget.py counts those per run */
                float d = sqrtf(w.key);
                int hard = ((w.cls == C_NEAR || w.cls == C_NEAR2) && d <= MS_R0_DRAW) ||
                           ((w.cls == C_PIN || w.cls == C_PIN0) && d <= MS_R1_PIN);
                printf("mapstream: no room for item %d.%u (%u KB, class %d, %.0f m%s): nothing of a lower class to drop; "
                       "%u KB free, items %u KB (resident %u, mip 0 %u), other %u KB, borrowed %u KB\n", w.k, w.id,
                       gb >> 10, w.cls, d, hard ? ", hard" : "", fr * MS_GRAN >> 10, mapstream_stats.bytes >> 10,
                       mapstream_stats.res_kb, mapstream_stats.tex_kb, mapstream_stats.other_kb, mapstream_stats.lent_kb);
                unsigned fr3[3][3];
                msa_free_report(fr3);
                printf("mapstream: free room (KB, whole pages, largest run in a page KB): items %u %u %u | misc %u %u %u | "
                       "structural %u %u %u; lent near %u KB, inflight %d\n", fr3[0][0], fr3[0][1], fr3[0][2],
                       fr3[1][0], fr3[1][1], fr3[1][2], fr3[2][0], fr3[2][1], fr3[2][2], ms_lent_near >> 10, ms_nrd);
            }
            mapstream_stats.room_fails++;
            hitch_room(w.cls == C_NEAR || w.cls == C_NEAR2, w.k, w.id);
            if (ms_nblk == MS_BLOCKED)
                memmove(ms_blk, ms_blk + 1, --ms_nblk * sizeof *ms_blk);
            ms_blk[ms_nblk++] = (MsQ){w.k, w.id, 0, (float)ms_frame};
            return 1;  /* the next queued one may still fit */
        }
        ms_room_fail = 0;
        int tk = (first && !sent ? pack_req_read_first : pack_req_read)(&m->pk, it_off(m, g), p, gb);
        dl_first += tk && first && !sent;
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
            msa_free(p, gb);
            /* (the lending counts went up with the allocation: left as they were, every full request table while
             * near parts were lent shrank MS_LEND_NEAR_KB's room for good) */
            ms_lent_n -= lent > 0;
            if (lent == 2)
                ms_lent_near = ms_lent_near > gb ? ms_lent_near - gb : 0;
            if (!sent)
                ms_q_add(w.k, w.id, w.cls, w.key);
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

/* MS_SCAN: the queue entry issue() takes (-1: none may go now). The queue is in rank order, so the urgent entries
 * lead it; once the head is a read-ahead class, every queued read-ahead entry (the rest of the queue) is a candidate.
 * ra_ok: a read-ahead entry may go (MS_RA_INFLIGHT). *first: send it ahead of the disc server's queue (MS_DL_FIRST). */
static unsigned scan_key(const MsQ *e, int *rk_out)
{
    const MsInst *m = &ms_inst[e->k];
    if (e->id >= it_n(m))
        return *rk_out = 3, ~0u;
    unsigned o = it_off(m, e->id);
    int rk = e->k != scan_k ? 2 : o >= scan_end ? 0 : 1;  /* ahead of the head, behind it (wrap), other pack */
    if (rk == 2)
        o = (unsigned)e->k << 26 | (o >> 6);  /* other packs: by block, then offset */
    *rk_out = rk;
    return o;
}
static int q_scan_pick(int ra_ok, int *first)
{
    *first = 0;
    int hard = q_rank(ms_q[0].cls) >= C_NEAR2;
    if (!hard && !ra_ok)
        return -1;
    if (scan_k < 0 || (hard && (!MS_DEADLINE || ms_settle_plan() || MS_READ_BY_CLASS)))
        return hard || ra_ok ? 0 : -1;  /* the hard tier at the head: nearest first (q_rank) */
    int nh = 0;
    while (nh < ms_nq && q_rank(ms_q[nh].cls) >= C_NEAR2)
        nh++;
    /* MS_GAP_KB: an entry starting a little after the last read's end in the same pack (the IOP reads the gap) */
    int gi = -1;
    unsigned go = ~0u;
    for (int i = 0; MS_GAP_KB && i < ms_nq; i++) {
        const MsInst *m = &ms_inst[ms_q[i].k];
        if (ms_q[i].k != scan_k || ms_q[i].id >= it_n(m) || (i >= nh && !ra_ok))
            continue;
        unsigned o = it_off(m, ms_q[i].id);
        if (o >= scan_end && o - scan_end <= (unsigned)MS_GAP_KB << 10 && o < go)
            gi = i, go = o;
    }
    if (nh) {
        /* the hard tier in scan order (insertion sort of <= MS_QUEUE indices); each one's estimated completion is its
         * place in that order behind the reads in flight; one that would land after its deadline panics */
        static unsigned char ord[MS_QUEUE];
        static unsigned okey[MS_QUEUE];
        for (int i = 0; i < nh; i++) {
            int rk;
            unsigned o = scan_key(&ms_q[i], &rk), kk = (unsigned)rk << 30 | (o >> 2 & 0x3fffffffu);
            int j = i;
            while (j > 0 && okey[j - 1] > kk)
                ord[j] = ord[j - 1], okey[j] = okey[j - 1], j--;
            ord[j] = (unsigned char)i, okey[j] = kk;
        }
        float v = sqrtf(ms_vel[0] * ms_vel[0] + ms_vel[2] * ms_vel[2]) * MS_DL_FPS;
        v = v > MS_DL_VMIN ? v : MS_DL_VMIN;
        int pi = -1;
        float pdl = 0;
        for (int j = 0; j < nh; j++) {
            const MsQ *e = &ms_q[ord[j]];
            const MsInst *m = &ms_inst[e->k];
            float r = e->id < it_n(m) && it_res(m, e->id) ? MS_DL_COARSE_R : MS_DL_NEAR_R;
            float d = sqrtf(e->key) - r, dl = d > 0 ? d * 1000.0f / v : 0;
            float est = (float)(ms_nrd + j + 1) * rd_ms;
            if (dl < est && (pi < 0 || dl < pdl || (dl == pdl && e->key < ms_q[pi].key)))
                pi = ord[j], pdl = dl;
        }
        if (pi >= 0) {
            dl_panic++;
            /* already late with reads ahead of it in the server: ahead of them, unless it lies ahead of the head in
             * the same pack, where the server's LSN sweep reaches it on the way (jumping the queue there only added a
             * seek out and one back: ISO 2026-11-17, ~100-130 firsts a run = +7-18 % EE seeks on the Burg tests) */
            int prk;
            scan_key(&ms_q[pi], &prk);
            *first = MS_DL_FIRST && pdl <= 0 && ms_nrd >= 2 && prk != 0;
            return pi;
        }
        if (gi >= 0)
            return dl_gap++, gi;
        dl_scan++;
        return ord[0];
    }
    if (gi >= 0)
        return dl_gap++, gi;
    int best = 0, bk = 3;
    unsigned bo = ~0u;
    for (int i = 0; i < ms_nq; i++) {
        const MsInst *m = &ms_inst[ms_q[i].k];
        if (ms_q[i].id >= it_n(m))
            continue;
        int rk;
        unsigned o = scan_key(&ms_q[i], &rk);
        if (rk < bk || (rk == bk && o < bo))
            best = i, bk = rk, bo = o;
    }
    return best;
}
void ms_flow_log(const float *pos)
{
    static unsigned n, held, full, idle;
    if (ms_settling) {
        n = held = full = idle = 0;
        return;
    }
    held += ms_hold && ms_nq, full += ms_nrd == MS_INFLIGHT, idle += !ms_nq && !ms_nrd;
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
            else if (ms_blocked(k, c))
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
               (double)MS_R0_DRAW, cut, ref, vis, disc, kb, ms_nq, ms_nrd, (unsigned)ms_lat_avg, flow_rd[0], flow_kb[0],
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
unsigned ms_pend_frame;  /* frame of the last move */
