/* A world block's lite set (world_i.h): one arena block per entry, read in the background, turned on in two steps
 * (collision and map, then textures, enemies and events), turned off, let go when only kept (lean) and read back.
 * Split from world.c (Polaris 4.1, code moved as it was). */
#include "world_i.h"
int w_warping;  /* inside settle(): drains and the swap delay are skipped */
/* blocks the player is held for: found by world_hold this frame, latched for step(); blocks a settle could not load
 * (no more holding for them until they turn on); seconds held */
unsigned w_hold_m, w_hold_want, w_hold_gone;
/* WORLD_EV_SHARED: each map's events.bin, kept under its first block's index (map_lead), with the blocks using it */
static struct { void *buf; unsigned n; int refs; } evm[WORLD_MAX], mtx[WORLD_MAX];

void w_step_time(unsigned t0)
{
    unsigned us = (ticks() - t0) / 295;
    if (us > world_stats.on_us_max)
        world_stats.on_us_max = us;
}

/* a started block's entry blobs [e0, e1): arena blocks, or heap for texture entries the arena had no room for */
static void free_ents(BlockRt *r, unsigned e0, unsigned e1)
{
    for (unsigned e = e0; e < e1; e++)
        if (r->blob[e]) {
            if (mapstream_owns(r->blob[e]))
                mapstream_free(r->blob[e], (r->size[e] + 127) & ~127u);
            else
                free(r->blob[e]);
            r->blob[e] = NULL;
        }
}

static void free_tex(BlockRt *r)
{
    if (r->tex_held) {
        if (mapstream_owns(r->texbuf))
            mapstream_free(r->texbuf, r->tex_total);
        else
            free(r->texbuf);
        r->texbuf = NULL;
        r->tex_held = 0;
        world_stats.pool_used_kb -= r->tex_total >> 10;
    }
}

/* A block's lite set is read: hand its blobs to the systems (no copies, no IOP waits) */
/* first step: collision and map (the rest follows in turn_on_rest once the textures are read) */
/* the map's shared textures for block i (its own read becomes the map's copy, or goes when there is one; read now
 * when the copy it counted on went before it turned on) */
static void ev_free(void *p, unsigned n);
static void mtx_on(int i)
{
    BlockRt *r = &w_rt[i];
    int L = map_lead(i);
    if (!r->mtx_full || r->mtx_ref) {
        map_block_shared(i, r->mtx_full ? mtx[L].buf : NULL);
        return;
    }
    if (r->blob[E_MTX]) {
        if (!mtx[L].buf)
            mtx[L].buf = r->blob[E_MTX], mtx[L].n = r->mtx_full;
        else
            ev_free(r->blob[E_MTX], r->mtx_full);
        r->blob[E_MTX] = NULL;
    } else if (!mtx[L].buf) {
        void *b = memalign(128, (r->mtx_full + 127) & ~127u);
        if (b && w_read_sync(&r->pk, r->off[E_MTX], b, r->mtx_full)) {
            mtx[L].buf = b, mtx[L].n = r->mtx_full;
            printf("world: block %d: its map's maptex.bin went before it turned on, read again (heap)\n", i);
        } else
            free(b);
    }
    if (mtx[L].buf)
        mtx[L].refs++, r->mtx_ref = 1;
    map_block_shared(i, mtx[L].buf);
}

/* free_slot: block i's chains are done; the map's shared textures go with its last block */
static void mtx_off(int i)
{
    BlockRt *r = &w_rt[i];
    int L = map_lead(i);
    if (!r->mtx_ref)
        return;
    r->mtx_ref = 0;
    if (--mtx[L].refs <= 0) {
        ev_free(mtx[L].buf, mtx[L].n);
        mtx[L].buf = NULL, mtx[L].refs = 0;
    }
}

static void turn_on_col(int i)
{
    BlockRt *r = &w_rt[i];
    unsigned t0 = ticks();
    int ok = r->col_full ? col_load_index(i, r->blob[E_COL]) : col_load(i, r->blob[E_COL]), cok = ok;
    if (ok && r->col_full)
        coltile_on(i, &r->pk, r->off[E_COL]);
    if (r->size[E_TABB])
        map_block_split(i, r->blob[E_TABB], r->cut[0], r->blob[E_TABC], r->cut[1]);
    mtx_on(i);
    if (r->size[E_DG] && r->size[E_PVS])
        map_block_dg_set(i, r->blob[E_DG], r->size[E_DG], r->blob[E_PVS], r->size[E_PVS]);  /* WORLD_DG_LITE: lite set */
    ok &= map_block_init(i, r->blob[E_TAB], NULL, i);
    if (!ok)
        printf("world: block %d: collision %s (%.4s), map %.4s\n", i, cok ? "ok" : "FAILED", (const char *)r->blob[E_COL],
               (const char *)r->blob[E_TAB]);
    if (ok)
        map_block_stream_open(i, &r->pk);
    r->state = W_ON;
    r->stage = ok ? 1 : 2;  /* a failed block gets no objects, enemies or events */
    w_hold_gone &= ~(1u << i);
    world_stats.loaded++;
    world_stats.loads++;
    world_stats.load_ms = (ticks() - r->t0) / 294912;
    world_stats.read_kb = r->total >> 10;
    /* WORLD_REACH's turn-on estimate: up at once with a slower load, down by an eighth of the gap (a running max) */
    w_on_est[i] = world_stats.load_ms > w_on_est[i] ? world_stats.load_ms
                                                : w_on_est[i] + ((float)world_stats.load_ms - w_on_est[i]) * 0.125f;
    w_on_s[i] = w_clock_s;
    if (!w_warping && w_want_s[i] > 0)
        printf("world: block %d wanted %.0f ms before on (seam %.1f m, estimate %.0f ms)\n", i,
               (double)((w_clock_s - w_want_s[i]) * 1000), (double)(w_reach_m >> i & 1 ? w_reach_d[i] : -1), (double)w_on_est[i]);
    w_step_time(t0);
    printf("world: block %d %s on%s: %u KB in %u ms, collision %d tris, step %u us; lite sets %u KB | collision %u "
           "chunks %u textures %u cells+horizon %u events %u KB\n", i,
           w_tab[i].pack, ok ? "" : " (FAILED)", r->total >> 10, world_stats.load_ms, col_block_tris(i), (ticks() - t0) / 295,
           world_stats.pool_used_kb, r->size[E_COL] >> 10, r->size[E_TAB] >> 10, r->size[E_TABB] >> 10,
           r->size[E_TABC] >> 10, r->size[E_EV] >> 10);
}

/* objects.bin (e = E_OBJ) or fx.bin (E_FX) is in texbuf, or the pack has none: its textures go up */
static void tex_on(int i, unsigned e)
{
    BlockRt *r = &w_rt[i];
    void *blob = r->size[e] ? r->texbuf : NULL;
    if (e == E_OBJ)
        objects_block_add(i, blob);
    else
        fx_block_on(i, 1, blob);
}

/* second step: the block's enemies and events, after its objects and effects */
static void ev_free(void *p, unsigned n)
{
    if (mapstream_owns(p))
        mapstream_free(p, (n + 127) & ~127u);
    else
        free(p);
}

/* WORLD_EV_SHARED: block i's events on, from its map's copy (its own read becomes it when there is none yet; read now
 * when the copy it counted on went before it turned on) */
static void ev_on(int i)
{
    BlockRt *r = &w_rt[i];
    int L = map_lead(i);
    if (r->blob[E_EVM]) {
        if (!evm[L].buf)
            evm[L].buf = r->blob[E_EVM], evm[L].n = r->ev_full;
        else
            ev_free(r->blob[E_EVM], r->ev_full);
        r->blob[E_EVM] = NULL;
    } else if (!evm[L].buf) {
        void *b = memalign(128, (r->ev_full + 127) & ~127u);
        if (!b || !w_read_sync(&r->pk, r->off[E_EVM], b, r->ev_full)) {
            printf("world: block %d: events.bin not readable\n", i);
            free(b);
            return;
        }
        evm[L].buf = b, evm[L].n = r->ev_full;
        printf("world: block %d: its map's events.bin went before it turned on, read again (heap)\n", i);
    }
    evm[L].refs++, r->ev_ref = 1;
    event_block_on_split(i, evm[L].buf, r->evhdr, r->blob[E_EV], r->ev_mid);
}

static void ev_off(int i)
{
    BlockRt *r = &w_rt[i];
    int L = map_lead(i);
    if (!r->ev_ref)
        return;
    r->ev_ref = 0;
    if (--evm[L].refs <= 0) {
        ev_free(evm[L].buf, evm[L].n);
        evm[L].buf = NULL, evm[L].refs = 0;
    }
}

static void turn_on_rest(int i)
{
    BlockRt *r = &w_rt[i];
    unsigned t0 = ticks();
    free_tex(r);  /* uploaded: back to the near cells */
    enemy_block_add(i);  /* before event_block_on: its constructors may disable these characters (A2) */
    if (r->ev_split)
        ev_on(i);
    else
        event_block_on(i, r->size[E_EV] ? r->blob[E_EV] : NULL);
    r->stage = 2;
    w_step_time(t0);
    printf("world: block %d objects, enemies, events on: %u KB textures, %u ms after the read began, step %u us\n", i,
           r->tex_total >> 10, (ticks() - r->t0) / 294912, (ticks() - t0) / 295);
}

/* the texture buffer of an on block in stage 1 (one arena block, else the heap while it keeps WORLD_HEAP_FLOOR); 0 =
 * no room yet (retry_s) */
static int tex_alloc(int i)
{
    BlockRt *r = &w_rt[i];
    if (r->tex_held || !r->tex_total)
        return 1;
    if (r->retry_s > 0 && !w_warping)
        return 0;
    r->texbuf = mapstream_alloc_pool(r->tex_total, MS_POOL_MISC);  /* (MS_POOLS) */
    if (!r->texbuf)
        r->texbuf = mapstream_alloc(r->tex_total, w_feet_now);
    if (!r->texbuf && enemy_ram_free() >= WORLD_HEAP_FLOOR + r->tex_total)
        r->texbuf = memalign(128, r->tex_total);
    if (!r->texbuf) {
        r->retry_s = WORLD_RETRY_S;
        return 0;
    }
    r->tex_held = 1;
    world_stats.pool_used_kb += r->tex_total >> 10;
    return 1;
}

void w_turn_off(int i)
{
    BlockRt *r = &w_rt[i];
    unsigned t0 = ticks();
    if (r->stage == 2) {  /* stage 1: no enemies or events yet; the uploads done so far (r->ent) go below */
        event_block_off(i);  /* before the collision its hit parts point into goes */
        ev_off(i);           /* (after: another block of the map may take its running events over, event.c) */
        enemy_block_remove(i);
    }
    if (r->stage == 2 || r->ent > E_OBJ)
        objects_block_remove(i);
    if (r->stage == 2 || r->ent > E_FX)
        fx_block_on(i, 0, NULL);
    r->stage = 0;
    r->lean = r->lean_drain = 0;  /* (free_slot frees whatever tables are still held) */
    coltile_off(i);
    col_unload(i);
    map_block_drop(i);  /* its relief too (bump.c) */
    if (w_bump_blk == i)
        w_bump_drop();
    r->state = W_DRAIN;  /* the slot's memory is reused only after the chains that drew from it are done */
    r->drain = WORLD_DRAIN;
    world_stats.loaded--;
    world_stats.unloads++;
    w_step_time(t0);
    printf("world: block %d %s off\n", i, w_tab[i].pack);
}

static void *lite_alloc(int want, unsigned sz, int *wait);
/* WORLD_LITE_DIET: the map tables of block i (E_TAB .. E_TABC) */
static unsigned tab_kb(const BlockRt *r)
{
    unsigned n = 0;
    for (int e = E_TAB; e <= E_PVS; e++)
        if (LEAN_ENT(e))
            n += (r->size[e] + 127) & ~127u;
    return n >> 10;
}

static void free_lean(BlockRt *r)
{
    for (unsigned e = E_TAB; e <= E_PVS; e++)
        if (LEAN_ENT(e))
            free_ents(r, e, e + 1);
}

void w_free_slot(int i)
{
    BlockRt *r = &w_rt[i];
    if (r->slot >= 0)
        w_slot_of[r->slot] = -1;
    r->slot = -1;
    free_ents(r, 0, E_TEX);
    mtx_off(i);
    world_stats.pool_used_kb -= (r->total >> 10) - (r->tab_gone ? tab_kb(r) : 0);
    r->tab_gone = 0;
    free_tex(r);
    r->state = W_OFF;
}

/* an on block only kept: its drawing goes now, the tables' memory after the chains that drew from them (lean_step) */
void w_lean(int i)
{
    BlockRt *r = &w_rt[i];
    map_block_lean(i, LEAN_E1 != E_TABC);  /* WORLD_KEPT_HZN: its horizon stays drawn */
    r->lean = 1, r->lean_drain = WORLD_DRAIN;
    world_stats.leans++;
    printf("world: block %d %s kept only: map tables let go (%u KB), collision %u KB and events stay\n", i, w_tab[i].pack,
           tab_kb(r), (r->size[E_COL] + 127) >> 10);
}

void w_lean_step(int i)
{
    BlockRt *r = &w_rt[i];
    if (r->lean == 1 && r->lean_drain > 0 && (--r->lean_drain <= 0 || w_warping)) {
        r->lean_drain = 0;
        free_lean(r);
        world_stats.pool_used_kb -= tab_kb(r);
        r->tab_gone = 1;
    }
}

/* a lean block is wanted again: its map tables, one arena block each (largest first, as start()); 0 = no room yet */
int w_relite_start(int i)
{
    BlockRt *r = &w_rt[i];
    if (r->lean_drain > 0 || (r->retry_s > 0 && !w_warping) || world_stats.reading >= 0)
        return 0;
    unsigned order[4] = {E_TAB, E_TABB, E_TABC, E_PVS}, n = LEAN_E1 - E_TAB + 1;
    order[n++] = E_PVS;
    for (unsigned a = 0; a < n; a++)
        for (unsigned b = a + 1; b < n; b++)
            if (r->size[order[b]] > r->size[order[a]]) {
                unsigned t = order[a];
                order[a] = order[b], order[b] = t;
            }
    for (unsigned k = 0; k < n; k++) {
        unsigned e = order[k], sz = (r->size[e] + 127) & ~127u;
        if (!r->size[e])
            continue;
        int wait = 0;
        r->blob[e] = lite_alloc(i, sz, &wait);
        if (!r->blob[e]) {
            free_lean(r);
            r->retry_s = wait ? 0.1f : WORLD_RETRY_S;
            return 0;
        }
    }
    r->lean = 2, r->ent = E_TAB, r->done = 0, r->t0 = ticks();
    world_stats.pool_used_kb += tab_kb(r);
    r->tab_gone = 0;
    return 1;
}

/* read the tables back (one WORLD_READ at a time, as read_step); all in: the drawing is on again */
void w_relite_step(int i)
{
    BlockRt *r = &w_rt[i];
    int res;
    if (world_stats.reading == i) {
        if (!pack_req_done(w_tk, &res))
            return;
        w_tk = 0;
        unsigned n = r->size[r->ent] - r->done < WORLD_READ ? r->size[r->ent] - r->done : WORLD_READ;
        world_stats.reading = -1;
        if (res != (int)n) {
            printf("world: block %d tables read %d of %u bytes\n", i, res, n);
            world_stats.fails++;
            free_lean(r);
            world_stats.pool_used_kb -= tab_kb(r);
            r->lean = 1, r->tab_gone = 1, r->retry_s = WORLD_RETRY_S;
            return;
        }
        InvalidDCache((char *)r->blob[r->ent] + r->done, (char *)r->blob[r->ent] + r->done + n - 1);
        if ((r->done += n) == r->size[r->ent])
            r->ent++, r->done = 0;
    }
    while (r->ent <= E_PVS && (!LEAN_ENT(r->ent) || !r->size[r->ent]))
        r->ent++;
    if (r->ent > E_PVS) {
        unsigned t0 = ticks();
        if (r->size[E_TABB])
            map_block_split(i, r->blob[E_TABB], r->cut[0], r->blob[E_TABC], r->cut[1]);
        if (r->size[E_DG] && r->size[E_PVS])
            map_block_dg_set(i, r->blob[E_DG], r->size[E_DG], r->blob[E_PVS], r->size[E_PVS]);
        int ok = map_block_init(i, r->blob[E_TAB], NULL, i);
        if (ok)
            map_block_stream_open(i, &r->pk);
        r->lean = 0, r->ent = NENT;
        world_stats.relites++;
        w_step_time(t0);
        printf("world: block %d %s wanted again: map tables %u KB in %u ms%s\n", i, w_tab[i].pack, tab_kb(r),
               (ticks() - r->t0) / 294912, ok ? "" : " (map FAILED)");
        return;
    }
    unsigned n = r->size[r->ent] - r->done < WORLD_READ ? r->size[r->ent] - r->done : WORLD_READ;
    if (!w_tk && (w_tk = pack_req_read(&r->pk, r->off[r->ent] + r->done, (char *)r->blob[r->ent] + r->done, n)))
        world_stats.reading = i, hitch_block_read(n);
}

/* Continue the background read of block i's lite set */
void w_read_step(int i, int issue)
{
    BlockRt *r = &w_rt[i];
    int res;
    if (world_stats.reading == i) {
        if (!pack_req_done(w_tk, &res))
            return;  /* still in flight */
        w_tk = 0;
        unsigned n = r->size[r->ent] - r->done < WORLD_READ ? r->size[r->ent] - r->done : WORLD_READ;
        world_stats.reading = -1;
        if (res != (int)n) {
            printf("world: block %d read %d of %u bytes\n", i, res, n);
            world_stats.fails++;
            if (r->state == W_ON) {  /* stage 1 (a texture entry): its collision and map tables are in use; freeing the
                                       * slot here left them pointing at freed memory */
                w_turn_off(i);
                r->retry_s = WORLD_RETRY_S;
            } else
                w_free_slot(i);
            return;
        }
        char *dst = r->ent >= E_TEX ? r->texbuf : r->blob[r->ent];
        InvalidDCache(dst + r->done, dst + r->done + n - 1);
        if ((r->done += n) == r->size[r->ent]) {
            if (r->ent >= E_TEX)
                tex_on(i, r->ent);
            r->ent++, r->done = 0;
        }
    }
    for (;;) {
        while (r->ent < NENT && !r->size[r->ent]) {  /* optional entry the pack does not have */
            if (r->state == W_ON && r->ent >= E_TEX)
                tex_on(i, r->ent);
            r->ent++;
        }
        if (r->state == W_READING && (r->ent >= E_TEX || r->ent == NENT)) {
            turn_on_col(i);  /* collision, maptab, events read: the floor is on */
            if (r->stage == 2) {
                free_tex(r);
                return;
            }
            issue = 0;  /* objects.bin / fx.bin from a later step (this one stays short) */
            continue;
        }
        if (r->ent == NENT) {
            turn_on_rest(i);
            return;
        }
        break;
    }
    if (r->state == W_ON && (!issue || world_stats.reading >= 0 || !tex_alloc(i)))
        return;  /* stage 1: the next texture entry once a read is free and the buffer has room */
    char *dst = r->ent >= E_TEX ? r->texbuf : r->blob[r->ent];
    unsigned n = r->size[r->ent] - r->done < WORLD_READ ? r->size[r->ent] - r->done : WORLD_READ;
    if (!w_tk && (w_tk = (r->state == W_READING ? pack_req_read_first : pack_req_read)(&r->pk, r->off[r->ent] + r->done, dst + r->done, n)))
        world_stats.reading = i, hitch_block_read(n);  /* else every request slot is taken: try next frame */
}

/* MS_POOLS: a lite-set entry from the structural pool. Full: a kept-only block that still holds its tables lets go of
 * them (lean, freed WORLD_DRAIN frames later: *wait = 1, try again soon); none left to lean: as before, from the item
 * pages (mapstream_alloc clears map items), which the pool's size should make rare (logged). Without pools: as before */
static void *lite_alloc(int want, unsigned sz, int *wait)
{
    if (!mapstream_pools_on())
        return mapstream_alloc(sz, w_feet_now);
    void *p = mapstream_alloc_pool(sz, MS_POOL_STRUCT);
    if (p)
        return p;
    if (mapstream_pool_wait()) {  /* MS_LEND: map items lent the pool are in a chain for two more draws */
        *wait = 1;
        return NULL;
    }
    for (int i = 1; i < w_nblk; i++)
        if (w_rt[i].state == W_ON && w_rt[i].lean == 1 && w_rt[i].lean_drain > 0) {
            *wait = 1;  /* one is draining already */
            return NULL;
        }
    int b = -1;
    float bd = -1;
    for (int i = 1; WORLD_LITE_DIET && i < w_nblk; i++)
        if (i != want && i != w_cur && w_rt[i].state == W_ON && w_rt[i].stage == 2 && !w_rt[i].lean && !(w_want_m >> i & 1) &&
            world_stats.reading != i) {
            float d = box_d(&w_tab[i], w_feet_now);
            if (d > bd)
                b = i, bd = d;
        }
    if (b >= 0) {
        w_lean(b);
        *wait = 1;
        return NULL;
    }
    if (w_ronly_m >> want & 1)
        return NULL;  /* (wanted by the reach alone: no item pages; it tries again in WORLD_RETRY_S) */
    p = mapstream_alloc(sz, w_feet_now);
    if (p)
        world_stats.pool_spills++;
    printf("world: structural pool full for %u KB of block %d: %s the item pages\n", sz >> 10, want,
           p ? "taken from" : "nothing in");
    return p;
}

int w_start(int i)
{
    BlockRt *r = &w_rt[i];
    int s = 0;
    while (s < WORLD_SLOTS && w_slot_of[s] >= 0)
        s++;
    if (s == WORLD_SLOTS || (r->retry_s > 0 && !w_warping)) {
        if (s == WORLD_SLOTS)
            r->why = WHY_SLOT;
        return 0;
    }
    /* One arena block per entry, largest first: the near cells being drawn split the arena, and the Burg block's
     * 1508 KB in one piece found no hole with 2.1 MB free in it (largest 1068 KB; 2026-11-03, 2.8 MB arena of
     * 9e6dd67). Without room in the arena an entry takes the heap while it keeps WORLD_HEAP_FLOOR. All or nothing;
     * a failed start waits WORLD_RETRY_S, since each try may drop drawn cells. */
    if (r->ev_split)  /* the map's events.bin is in: only this block's middle part */
        r->size[E_EVM] = evm[map_lead(i)].buf ? 0 : r->ev_full;
    r->size[E_MTX] = mtx[map_lead(i)].buf ? 0 : r->mtx_full;  /* the same for its shared textures */
    unsigned order[NENT], n = 0;
    for (unsigned e = 0; e < E_TEX; e++)  /* the texture buffer comes once the collision is on (tex_alloc) */
        if (r->size[e]) {
            unsigned k = n++;
            while (k && r->size[order[k - 1]] < r->size[e])
                order[k] = order[k - 1], k--;
            order[k] = e;
        }
    memset(r->blob, 0, sizeof r->blob);
    for (unsigned k = 0; k < n; k++) {
        unsigned e = order[k], sz = (r->size[e] + 127) & ~127u;
        int wait = 0;
        r->blob[e] = lite_alloc(i, sz, &wait);
        if (wait) {
            free_ents(r, 0, NENT);
            r->retry_s = 0.1f;  /* (a lean drains in WORLD_DRAIN frames) */
            r->why = mapstream_pool_wait() ? WHY_POOLWAIT : WHY_LEANWAIT;
            return 0;
        }
        /* no hole in the near arena, and the player waits for this block (a warp, or held at its edge): the heap,
         * while it keeps WORLD_HEAP_FLOOR + WORLD_HEAP_MODELS for the enemy types. At the top of the Burg ladders
         * with enemies the bonfire block's maptab.bin (592 KB) found none even after 886 KB of drawn cells went,
         * with 3.5 MB of heap free (route_enemies.txt, 2026-11-05). Not for every block: lite sets in the heap while
         * walking left the enemy types without room (70 model.bin OUT OF RAM in one run, RAM min 639 KB) */
        if (!r->blob[e] && (w_warping || (w_hold_want >> i & 1)) &&
            enemy_ram_free() >= WORLD_HEAP_FLOOR + WORLD_HEAP_MODELS + sz && (r->blob[e] = memalign(128, sz)))
            printf("world: block %d %s %u KB in the heap (RAM free %u KB)\n", i, ENTRIES[e], sz >> 10,
                   enemy_ram_free() >> 10);
        if (!r->blob[e]) {
            free_ents(r, 0, NENT);
            r->retry_s = WORLD_RETRY_S;
            r->why = WHY_NOROOM;
            return 0;  /* near cells still being read or drawn: try again */
        }
    }
    r->tex_held = 0, r->stage = 0;
    r->lean = r->lean_drain = r->tab_gone = 0, r->unwanted_s = 0;
    w_slot_of[s] = i;
    r->slot = s;
    r->ent = r->done = 0;
    while (r->ent < NENT && !r->size[r->ent])
        r->ent++;
    r->state = W_READING;
    r->t0 = ticks();
    world_stats.pool_used_kb += r->total >> 10;
    printf("world: block %d %s loading (%u KB + %u KB textures)%s\n", i, w_tab[i].pack, r->total >> 10, r->tex_total >> 10,
           w_hold_want >> i & 1 ? " for the held player" : "");
    if (r->want_t && (w_hold_want >> i & 1))
        printf("world: block %d started %u ms after it was wanted (last wait: %s)\n", i, (ticks() - r->want_t) / 294912,
               WHY[r->why]);
    r->why = WHY_NONE;
    return 1;
}
