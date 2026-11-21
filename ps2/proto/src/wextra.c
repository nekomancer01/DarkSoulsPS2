/* What a world block adds besides its lite set (world_i.h): the relief (bump.bin) and the far panorama of the block
 * underfoot, and the boot block letting go of its map tables away from it. Split from world.c (Polaris 4.1, code moved
 * as it was). */
#include "world_i.h"
float w_relief_at;  /* in_cur_s from which the relief of the block underfoot may be tried */
/* relief (bump.bin): block it is for (-1 none; its buffer may be NULL: none, or no room), block being read, the read in
 * flight, progress; a dropped buffer waits WORLD_DRAIN frames before it is freed */
int w_bump_blk = -1, w_bump_rd = -1, w_bump_reading;
/* the ticket of this file's read in flight (pack_req_read): a lite set chunk or a relief chunk, never both
 * (world_stats.reading names its block) */
int w_tk;
unsigned char *w_bump_buf, *w_bump_old;
unsigned w_bump_done, w_bump_t0, w_bump_old_n;
int w_bump_old_drain;
float w_in_cur_s;  /* seconds the player has stood in cur */
/* the far panorama (far.bin of the block underfoot, a heap buffer: map.c map_far_set): block it is loaded for (-1
 * none yet; its buffer may be NULL: the block has none, or no room), block being read, progress; a dropped buffer
 * waits WORLD_DRAIN frames */
int w_far_blk = -1, w_far_rd = -1, w_far_reading;
unsigned char *w_far_buf, *w_far_cur, *w_far_old;  /* being read, drawn (of far_blk), draining */
int w_far_old_drain;
unsigned char *w_far_res;   /* the reserve (NULL: none) */
unsigned w_far_res_size;
static int far_in_res;           /* the panorama being read lies in the reserve */
int w_far_swap;             /* the reserve's panorama drains for the next one: the boot block stays let go */
void w_far_free(unsigned char *p)
{
    if (p && p != w_far_res)
        free(p);
}
static unsigned char *far_alloc(unsigned sz, int *in_res)
{
    *in_res = 0;
    /* the reserve first: a panorama in the heap as well took the Burg's heap minimum from 1.47 to 0.42 MB (ISO
     * iso_burg_walls, 2026-11-17); the heap only for the next one while the reserve's is drawn */
    if (w_far_res && sz <= w_far_res_size && w_far_old != w_far_res && w_far_cur != w_far_res) {
        *in_res = 1;
        return w_far_res;
    }
    if (enemy_ram_free() >= WORLD_HEAP_FLOOR + WORLD_HEAP_MODELS + sz) {
        unsigned char *p = memalign(128, sz);
        if (p)
            return p;
    }
    if (!w_far_res || sz > w_far_res_size || w_far_old == w_far_res)
        return NULL;  /* (none, or still draining: the next try) */
    /* drawn from the reserve and no heap: let it drain, then read into it. (The boot block is not wanted back for
     * that gap: with it, the swap read the boot block's 1.1 MB of tables back into the heap and took the Burg's heap
     * minimum to 0.42 MB, iso_burg_walls 2026-11-17) */
    w_far_swap = 1;
    map_far_set(NULL);
    w_far_old = w_far_cur, w_far_old_drain = WORLD_DRAIN;
    w_far_cur = NULL;
    return NULL;
}
static unsigned far_done;

float w_far_retry_s;
int w_boot_want = 1, w_boot_spots;
int w_lite_first;  /* a wanted block's lite set waits: the relief read and mapstream.c issue no new read (step) */

/* the relief buffer (arena block, or heap when the arena had no room) */
void w_bump_free(void *p, unsigned n)
{
    if (!p)
        return;
    n = (n + 127) & ~127u;
    if (mapstream_owns(p))
        mapstream_free(p, n);
    else
        free(p);
    world_stats.pool_used_kb -= n >> 10;
}

/* the relief of bump_blk goes (bump.c lets go of it now); its buffer is freed WORLD_DRAIN frames later (VIF chains) */
void w_bump_drop(void)
{
    if (w_bump_blk > 0 || (w_bump_blk == 0 && WORLD_BOOT_RELIEF))
        map_block_bump(w_bump_blk, NULL);
    if (w_bump_buf) {
        if (w_bump_old)  /* the one before is still draining: the chains of two swaps ago are long done */
            w_bump_free(w_bump_old, w_bump_old_n);
        w_bump_old = w_bump_buf, w_bump_old_n = w_rt[w_bump_blk].bump_size, w_bump_old_drain = WORLD_DRAIN;
    }
    w_bump_buf = NULL;
    w_bump_blk = world_stats.full = -1;
}

/* The relief of the block the player stands in: its raw bump.bin read in the background (WORLD_READ at a time, after
 * every waiting lite set) into an arena block of its own, then handed to bump.c (map_block_bump). Relief is drawing
 * detail only (DSR's normal maps): no settle waits for it, and without room it stays off for that block. */
void w_bump_step(void)
{
    int res;
    if (w_bump_rd >= 0) {
        BlockRt *r = &w_rt[w_bump_rd];
        if (w_bump_reading) {
            if (!pack_req_done(w_tk, &res))
                return;
            w_tk = 0;
            unsigned n = r->bump_size - w_bump_done < WORLD_READ ? r->bump_size - w_bump_done : WORLD_READ;
            w_bump_reading = 0;
            world_stats.reading = -1;
            if (res != (int)n) {
                printf("world: block %d relief read %d of %u bytes\n", w_bump_rd, res, n);
                world_stats.fails++;
                w_bump_free(w_bump_buf, r->bump_size);
                w_bump_buf = NULL, w_bump_rd = -1;
                return;
            }
            InvalidDCache(w_bump_buf + w_bump_done, w_bump_buf + w_bump_done + n - 1);
            if ((w_bump_done += n) == r->bump_size) {
                if (r->state == W_ON && w_bump_rd == w_cur) {
                    map_block_bump(w_bump_rd, w_bump_buf);
                    w_bump_blk = world_stats.full = w_bump_rd;
                    world_stats.full_ms = (ticks() - w_bump_t0) / 294912;
                } else {  /* turned off or left while reading */
                    w_bump_free(w_bump_buf, r->bump_size);
                    w_bump_buf = NULL;
                }
                w_bump_rd = -1;
                return;
            }
        }
        if (w_lite_first || w_warping)
            return;  /* a settle waits for no relief */
        unsigned n = r->bump_size - w_bump_done < WORLD_READ ? r->bump_size - w_bump_done : WORLD_READ;
        if (!w_tk && (w_tk = pack_req_read(&r->pk, r->bump_off + w_bump_done, w_bump_buf + w_bump_done, n)))
            w_bump_reading = 1, world_stats.reading = w_bump_rd, hitch_block_read(n);
        return;
    }
    if (w_cur == w_bump_blk || w_in_cur_s < (w_relief_at > WORLD_SWAP_S ? w_relief_at : WORLD_SWAP_S) || w_warping ||
        w_rt[w_cur].state != W_ON || w_lite_first)
        return;
    if (w_bump_blk >= 0)
        w_bump_drop();
    w_bump_blk = w_cur;  /* also for a block without relief or room: no retry until the player changes block */
    BlockRt *r = &w_rt[w_cur];
    unsigned sz = (r->bump_size + 127) & ~127u;
    if (!r->bump_size)
        return;
    /* the relief is detail on top of the map: it takes room no wanted cell needs (WORLD_RELIEF_SOFT). Taking it like a
     * lite set dropped ~700 KB of drawn cells at every block change after 1 s in the Burg, which then read back
     * (soak on the ISO, 2026-11-07); the heap is the enemies' (WORLD_HEAP_MODELS) */
    w_bump_buf = mapstream_alloc_soft(sz);
    /* MS_POOLS: no heap fallback. The relief is optional detail, and in the heap it took RAM min on taurus to
     * 1417-1478 KB under the 1.5 MB floor once the enemies loaded (ISO 2026-11-09) */
    if (!w_bump_buf && !mapstream_pools_on() && enemy_ram_free() >= WORLD_HEAP_FLOOR + WORLD_HEAP_MODELS + sz)
        w_bump_buf = memalign(128, sz);
    if (!w_bump_buf) {
        printf("world: block %d relief: no room for %u KB\n", w_cur, sz >> 10);
        w_bump_blk = -1, w_relief_at = w_in_cur_s + WORLD_RELIEF_RETRY_S;  /* try again while the player stays */
        return;
    }
    w_bump_blk = -1, w_bump_rd = w_cur, w_bump_done = 0, w_bump_t0 = ticks();
    world_stats.pool_used_kb += sz >> 10;
}

/* The far panorama of the block underfoot (tools/farpano.py: the blocks of its area that are off, as seen from it):
 * its raw far.bin read in the background (WORLD_READ at a time, after every waiting lite set) into a heap buffer while
 * the heap keeps WORLD_HEAP_FLOOR + WORLD_HEAP_MODELS, then handed to map.c (map_far_set), which draws the parts whose
 * block is not drawn. The one of the block before stays drawn until the new one is in. */
void w_far_step(void)
{
    int res;
    if (w_far_rd >= 0) {
        BlockRt *r = &w_rt[w_far_rd];
        if (w_far_reading) {
            if (!pack_req_done(w_tk, &res))
                return;
            w_tk = 0;
            unsigned n = r->far_size - far_done < WORLD_READ ? r->far_size - far_done : WORLD_READ;
            w_far_reading = 0;
            world_stats.reading = -1;
            if (res != (int)n) {
                printf("world: block %d far panorama read %d of %u bytes\n", w_far_rd, res, n);
                world_stats.fails++;
                w_far_free(w_far_buf);
                w_far_buf = NULL, w_far_rd = -1, w_far_retry_s = WORLD_RETRY_S, w_far_swap = 0;
                return;
            }
            InvalidDCache(w_far_buf + far_done, w_far_buf + far_done + n - 1);
            if ((far_done += n) == r->far_size) {
                unsigned char *was = map_far_set(w_far_buf) ? NULL : w_far_buf;  /* (not a panorama: freed below) */
                if (w_far_old)  /* the one before the last is still draining: its chains are long done */
                    w_far_free(w_far_old);
                w_far_old = w_far_cur, w_far_old_drain = WORLD_DRAIN;
                printf("world: block %d far panorama %s: %u KB (%s, RAM free %u KB)\n", w_far_rd, was ? "NOT VALID" : "on",
                       r->far_size >> 10, far_in_res ? "reserve" : "heap", enemy_ram_free() >> 10);
                w_far_free(was);
                w_far_blk = w_far_rd, w_far_rd = -1, w_far_swap = 0;
                w_far_cur = was ? NULL : w_far_buf;
                w_far_buf = NULL;
                return;
            }
        }
        unsigned n = r->far_size - far_done < WORLD_READ ? r->far_size - far_done : WORLD_READ;
        if (!w_tk && world_stats.reading < 0 && (w_tk = pack_req_read(&r->pk, r->far_off + far_done, w_far_buf + far_done, n)))
            w_far_reading = 1, world_stats.reading = w_far_rd, hitch_block_read(n);
        return;
    }
    if (w_cur < 0 || w_cur == w_far_blk || (w_in_cur_s < WORLD_SWAP_S && !w_warping) || w_far_retry_s > 0 || w_lite_first)
        return;
    BlockRt *r = &w_rt[w_cur];
    unsigned sz = (r->far_size + 127) & ~127u;
    if (!r->far_size) {  /* this block has none: the last one's goes (it shows the world from another place) */
        map_far_set(NULL);
        if (w_far_cur) {
            if (w_far_old)
                w_far_free(w_far_old);
            w_far_old = w_far_cur, w_far_old_drain = WORLD_DRAIN;
            w_far_cur = NULL;
        }
        w_far_blk = w_cur;
        return;
    }
    if (!(w_far_buf = far_alloc(sz, &far_in_res))) {
        if (w_far_swap) {  /* the reserve drains (WORLD_DRAIN draws): read into it right after */
            w_far_retry_s = 0.2f;  /* (ASSUMED: a few draws) */
            return;
        }
        w_far_retry_s = WORLD_RELIEF_RETRY_S;
        printf("world: block %d far panorama: no room for %u KB (RAM free %u KB)\n", w_cur, sz >> 10, enemy_ram_free() >> 10);
        return;
    }
    w_far_rd = w_cur, far_done = 0;
}

/* WORLD_BOOT_LEAN: the boot block's chunk table and cell table + horizon go (see WORLD_BOOT_OFF_D) */
void w_boot_lean(void)
{
    BlockRt *r = &w_rt[0];
    unsigned t0 = ticks();
    map_boot_lean();
    r->lean = 1, r->lean_drain = WORLD_DRAIN;
    world_stats.leans++;
    w_step_time(t0);
    printf("world: boot block %s let go: chunk table %u KB and cells + horizon %u KB freed (far panorama of block %d "
           "stands in), step %u us\n", w_tab[0].pack, r->size[E_TAB] >> 10, r->size[E_TABC] >> 10, w_far_blk,
           (ticks() - t0) / 295);
}

/* a let-go boot block: its buffers are freed once the chains that drew from them are done; wanted back (boot_want),
 * they are read again (E_TAB, then E_TABC; WORLD_READ at a time) and the map is on as at boot. issue 0: only a read
 * in flight is collected */
void w_boot_step(int issue)
{
    BlockRt *r = &w_rt[0];
    int res;
    if (r->lean == 1 && r->lean_drain > 0 && (--r->lean_drain <= 0 || w_warping)) {
        r->lean_drain = 0;
        free(r->blob[E_TAB]), free(r->blob[E_TABC]);
        r->blob[E_TAB] = r->blob[E_TABC] = NULL;
    }
    if (r->lean == 1 && !r->lean_drain && w_boot_want && issue && world_stats.reading < 0 && (r->retry_s <= 0 || w_warping)) {
        r->blob[E_TAB] = memalign(128, (r->size[E_TAB] + 127) & ~127u);
        r->blob[E_TABC] = memalign(128, (r->size[E_TABC] + 127) & ~127u);
        if (!r->blob[E_TAB] || !r->blob[E_TABC]) {
            printf("world: boot block tables: no heap (RAM free %u KB)\n", enemy_ram_free() >> 10);
            free(r->blob[E_TAB]), free(r->blob[E_TABC]);
            r->blob[E_TAB] = r->blob[E_TABC] = NULL;
            r->retry_s = WORLD_RETRY_S;
            return;
        }
        r->lean = 2, r->ent = E_TAB, r->done = 0, r->t0 = ticks();
    }
    if (r->lean != 2)
        return;
    if (world_stats.reading == 0) {
        if (!pack_req_done(w_tk, &res))
            return;
        w_tk = 0;
        unsigned n = r->size[r->ent] - r->done < WORLD_READ ? r->size[r->ent] - r->done : WORLD_READ;
        world_stats.reading = -1;
        if (res != (int)n) {
            printf("world: boot block tables read %d of %u bytes\n", res, n);
            world_stats.fails++;
            free(r->blob[E_TAB]), free(r->blob[E_TABC]);
            r->blob[E_TAB] = r->blob[E_TABC] = NULL;
            r->lean = 1, r->retry_s = WORLD_RETRY_S;
            return;
        }
        InvalidDCache((char *)r->blob[r->ent] + r->done, (char *)r->blob[r->ent] + r->done + n - 1);
        if ((r->done += n) == r->size[r->ent])
            r->ent = r->ent == E_TAB ? E_TABC : NENT, r->done = 0;
    }
    if (r->ent == NENT) {
        unsigned t0 = ticks();
        map_block_split(0, r->blob[E_TABB], r->cut[0], r->blob[E_TABC], r->cut[1]);
        int ok = map_block_init(0, r->blob[E_TAB], NULL, 0);
        if (ok)
            map_block_stream_open(0, &r->pk);
        r->lean = 0;
        world_stats.relites++;
        w_step_time(t0);
        printf("world: boot block %s back: tables %u KB in %u ms%s\n", w_tab[0].pack,
               (r->size[E_TAB] + r->size[E_TABC]) >> 10, (ticks() - r->t0) / 294912, ok ? "" : " (map FAILED)");
        return;
    }
    if (!issue || world_stats.reading >= 0)
        return;
    unsigned n = r->size[r->ent] - r->done < WORLD_READ ? r->size[r->ent] - r->done : WORLD_READ;
    if (!w_tk && (w_tk = pack_req_read(&r->pk, r->off[r->ent] + r->done, (char *)r->blob[r->ent] + r->done, n)))
        world_stats.reading = 0, hitch_block_read(n);
}
