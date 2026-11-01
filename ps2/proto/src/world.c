#include <kernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "world.h"
#include "pack.h"
#include "collision.h"
#include "map.h"
#include "mapstream.h"
#include "vu1.h"
#include "objects.h"
#include "fx.h"
#include "enemy.h"

/* Load / unload radii from a block's xz bounds. ASSUMED (TECH_DEBT world-3): DSR switches its loaded maps by the
 * ConnectCollision part under the player (MSB type 11) and keeps a whole area load list resident (m10 = m10_00 + m10_01 +
 * m10_02); the PS2 cannot hold that, so neighbours load when the player comes within WORLD_LOAD_R of their bounds and
 * leave beyond WORLD_UNLOAD_R (hysteresis against flicker at the edge). 60 m at dash speed (~6 m/s) leaves ~10 s for
 * the reads, the cdrom0: worst case for a ~3 MB block (tools-hwperf: 6.6 s for the whole boot set). */
#define WORLD_LOAD_R 60.0f
#define WORLD_UNLOAD_R 80.0f
/* Bytes per background read: one at a time through pack.c's async slot, shared with mapstream.c (a near cell waits for
 * at most one of these) */
#define WORLD_READ (128u << 10)
/* Blocks resident besides the boot block. Their lite sets live in mapstream.c's near-cell arena (mapstream_alloc drops
 * the farthest near cells to make room), so a world build costs no RAM while no neighbour is loaded (TECH_DEBT world-5) */
#define WORLD_SLOTS 1
/* The block the player stands in is "full" (its map's resident streams in the one full pool, sized for the largest
 * block's mapres.bin); every other loaded block is "lite" (near cells only). A new block becomes full after the player
 * stood in it for WORLD_SWAP_S (no swapping back and forth on a boundary); ASSUMED value. */
#define WORLD_SWAP_S 1.0f
/* frames a buffer stays untouched after the map stopped drawing from it (VIF chains of the last two frames) */
#define WORLD_DRAIN 3
/* metres moved in one frame that count as a warp (test script spawns; later bonfire warps / respawns): DSR shows a
 * loading screen there, so world_update then loads and swaps synchronously before the frame is drawn */
#define WORLD_WARP 20.0f
/* heap kept out of the near-cell arena beyond MS_RESERVE, for enemy models: world builds allocate the arena at boot
 * (world_systems) while no enemy model is loaded, so this holds every model streamed in later. Single-area builds
 * allocate it on the first frame after the bonfire's types loaded (3.6 MB in Firelink) and keep 3 MB more. Measured
 * 2026-11-01: an arena created when only 3 types were loaded (spawn at the aqueduct) was 7.7 MB and two blocks'
 * types then ran out of RAM (world_cross.txt); 3.8 MB here gives the 5.5 MB of a bonfire boot wherever the player
 * starts: near cells >= 4.2 MB at the Firelink junction, heap outside the arena >= ~1.6 MB */
#define WORLD_ENEMY_RESERVE (3800u << 10)

/* The lite set of a block, read raw from its pack (tools/pack.py raw: entries, read in place) into its slot.
 * events.bin (A2's tools/export_emevd.py) is optional: a pack without it loads with no events. */
enum { E_COL, E_TAB, E_EV, NENT };
static const char *const ENTRIES[NENT] = {"collision.bin", "maptab.bin", "events.bin"};
#define E_OPTIONAL(e) ((e) == E_EV)

#include "event.h"  /* event_block_on / _off (A2): the blob stays world.c's (arena) until _off */

typedef struct {
    char pack[24], map[16];
    float mn[2], mx[2];
    unsigned flags, nbr;
} WorldBlock;
_Static_assert(sizeof(WorldBlock) == 64, "WorldBlock");

enum { W_OFF, W_READING, W_ON, W_DRAIN };

typedef struct {
    Pack pk;
    int open, state, slot, drain;
    void *mem;  /* the lite set's arena block (mapstream_alloc) */
    unsigned off[NENT], size[NENT], total, res_off, res_size, bump_off, bump_size;
    void *blob[NENT];
    unsigned ent, done;  /* read progress: entry, bytes of it */
    unsigned t0;
} BlockRt;

WorldStats world_stats = {.reading = -1, .full = -1};
static const WorldBlock *tab;
static BlockRt rt[WORLD_MAX];
static int nblk, cur = -1, slot_of[WORLD_SLOTS];
static unsigned char *full_pool;
static unsigned slot_size, full_size;
static float feet_now[3];  /* for start(): mapstream_alloc evicts the cells farthest from here */
/* full pool state: block whose streams it holds (-1 none), block being read into it, drain frames, read progress */
static int full_blk = -1, res_blk = -1, res_drain, res_reading;
static unsigned lent;
static int res_reclaim;  /* bytes of the full pool behind the full block's streams lent to mapstream.c for near cells */
static unsigned res_done, res_t0;
static float in_cur_s;  /* seconds the player has stood in cur */
static float last_feet[3];
static int warping;  /* inside settle(): drains and the swap delay are skipped */

static inline unsigned ticks(void)
{
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

static float box_d(const WorldBlock *b, const float *p)
{
    float dx = p[0] < b->mn[0] ? b->mn[0] - p[0] : p[0] > b->mx[0] ? p[0] - b->mx[0] : 0;
    float dz = p[2] < b->mn[1] ? b->mn[1] - p[2] : p[2] > b->mx[1] ? p[2] - b->mx[1] : 0;
    return dx > dz ? dx + dz * 0.5f : dz + dx * 0.5f;  /* within 12 % of the distance, no sqrt */
}

/* boot-time read of a raw entry range into dst (waits) */
static int read_sync(Pack *pk, unsigned off, void *dst, unsigned n)
{
    int r = 0;
    if (!pack_read_async(pk, off, dst, n))
        return 0;
    while (!pack_async_done(&r))
        ;
    InvalidDCache(dst, (char *)dst + n - 1);
    return r == (int)n;
}

int world_init(const char *boot_area)
{
    Pack pk;
    void *wb = pack_open(&pk, PACK_ROOT "world.pak") ? pack_load(&pk, "world.bin", 0) : NULL;
    pack_close(&pk);
    const unsigned *h = wb;
    if (!h || memcmp(h, "DSW1", 4) || h[1] != 1 || !h[2] || strncmp(((const WorldBlock *)(h + 4))->pack, boot_area, 24)) {
        free(wb);
        printf("world: no world table for %s, single area\n", boot_area);
        return 0;
    }
    nblk = h[2] < WORLD_MAX ? h[2] : WORLD_MAX;
    if (nblk > MAP_INST || nblk > COL_BLOCKS)
        nblk = MAP_INST < COL_BLOCKS ? MAP_INST : COL_BLOCKS;  /* map instance / collision block = block index */
    tab = (const WorldBlock *)(h + 4);
    /* open every block's pack now (boot time: a pack_open waits for the IOP) and size the pools */
    for (int i = 0; i < nblk; i++) {
        BlockRt *r = &rt[i];
        char path[64];
        snprintf(path, sizeof path, PACK_ROOT "%s.pak", tab[i].pack);
        r->slot = -1;
        if (!(r->open = pack_open(&r->pk, path))) {
            printf("world: block %d %s: no pack\n", i, path);
            if (!i)
                return 0;
            continue;
        }
        for (unsigned e = 0; e < NENT; e++) {
            if (!pack_entry_raw(&r->pk, ENTRIES[e], &r->off[e], &r->size[e]))
                r->off[e] = r->size[e] = 0;
            if (!r->size[e] && !E_OPTIONAL(e) && (i || e == E_TAB)) {
                printf("world: block %d %s: %s missing or not raw\n", i, tab[i].pack, ENTRIES[e]);
                r->open = 0;
            }
            if (i || e == E_TAB)  /* the boot block's collision is main.c's */
                r->total += (r->size[e] + 127) & ~127u;
        }
        if (!pack_entry_raw(&r->pk, "mapres.bin", &r->res_off, &r->res_size)) {
            printf("world: block %d %s: mapres.bin missing or not raw\n", i, tab[i].pack);
            r->open = 0;
        }
        if (!i || !pack_entry_raw(&r->pk, "bump.bin", &r->bump_off, &r->bump_size))  /* the boot area's: main.c */
            r->bump_off = r->bump_size = 0;
        if (!r->open) {
            pack_close(&r->pk);
            if (!i)
                return 0;
            continue;
        }
        if (i && r->total > slot_size)
            slot_size = r->total;
        if (r->res_size > full_size)
            full_size = r->res_size;
    }
    for (int s = 0; s < WORLD_SLOTS; s++)
        slot_of[s] = -1;
    full_size = (full_size + 127) & ~127u;
    full_pool = memalign(128, full_size);
    if (!full_pool) {
        printf("world: OUT OF RAM for the full pool (%u KB)\n", full_size >> 10);
        return 0;
    }
    world_stats.blocks = nblk;
    world_stats.pool_kb = full_size >> 10;
    if (nblk > 1)
        mapstream_reserve(WORLD_ENEMY_RESERVE);
    printf("world: %d blocks, full pool %u KB, lite sets up to %u KB from the near arena (%d slot%s)\n", nblk,
           full_size >> 10, slot_size >> 10, WORLD_SLOTS, WORLD_SLOTS > 1 ? "s" : "");
    for (int i = 0; i < nblk; i++)
        printf("world: block %d %s (%s) x %.0f..%.0f z %.0f..%.0f nbr %x lite %u KB res %u KB%s\n", i, tab[i].pack,
               tab[i].map, tab[i].mn[0], tab[i].mx[0], tab[i].mn[1], tab[i].mx[1], tab[i].nbr, rt[i].total >> 10,
               rt[i].res_size >> 10, rt[i].open ? "" : " (no pack)");
    cur = 0;
    return 1;
}

/* the full pool's bytes behind a smaller block's streams hold near cells until the next swap */
static void lend_tail(unsigned used)
{
    used = (used + 127) & ~127u;
    lent = full_size - used >= (256u << 10) ? full_size - used : 0;
    if (lent) {
        mapstream_lend(full_pool + used, lent);
        printf("world: %u KB of the full pool lent to the near cells\n", lent >> 10);
    }
}

int world_boot_map(void)
{
    BlockRt *r = &rt[0];
    if (cur < 0)
        return 0;
    /* the boot block: lite set (map tables) in a heap buffer of its own, streams into the full pool; collision is
     * main.c's (collision block 0) */
    r->blob[E_TAB] = memalign(128, (r->size[E_TAB] + 127) & ~127u);
    if (!r->blob[E_TAB] || !read_sync(&r->pk, r->off[E_TAB], r->blob[E_TAB], r->size[E_TAB]) ||
        !read_sync(&r->pk, r->res_off, full_pool, r->res_size) || !map_block_init(0, r->blob[E_TAB], full_pool, 0)) {
        printf("world: boot block map did not load\n");
        return 0;
    }
    r->state = W_ON;
    full_blk = world_stats.full = 0;
    lend_tail(r->res_size);
    world_stats.loaded = 1;
    world_stats.pool_used_kb = r->res_size >> 10;
    return 1;
}

void world_stream_open(void)
{
    if (cur >= 0)  /* instance 0's near cells from the boot pack (opened at world_init, stays open) */
        map_block_stream_open(0, &rt[0].pk);
}

int world_current(void) { return cur; }

/* every block's enemy / breakable / effect types, once, at boot before the near-cell arena exists (DSR keeps a whole
 * area's resources loaded; the PS2 keeps the types and places the instances per block): each raw entry read into a
 * temporary buffer, handed over, freed. What the types keep (chr.bin, compacted models, fx tables, Lua) lies below
 * the arena, so the heap above it stays free for enemy models streaming in and out (loading them after the arena
 * fragmented it: Firelink smoke 2026-11-01 02:37, model.bin 379K OUT OF RAM with 3 MB free). VRAM and the type data
 * stay for the session (TECH_DEBT world-4). */
static unsigned char *read_tmp(BlockRt *r, const char *name, unsigned *size)
{
    unsigned off;
    if (!pack_entry_raw(&r->pk, name, &off, size) || !*size)
        return NULL;
    unsigned char *p = memalign(128, *size);
    if (!p || !read_sync(&r->pk, off, p, *size)) {
        printf("world: %s NOT READ\n", name);
        free(p);
        return NULL;
    }
    return p;
}

void world_systems(GSGLOBAL *gs)
{
    for (int i = 1; i < nblk && cur >= 0; i++) {
        BlockRt *r = &rt[i];
        unsigned n0, n1;
        if (!r->open)
            continue;
        unsigned char *p = read_tmp(r, "objects.bin", &n0);
        int no = p ? objects_block_types(gs, i, p) : -1;
        free(p);
        p = read_tmp(r, "fx.bin", &n0);
        int nf = p ? fx_block_load(gs, i, p, n0) : -1;
        free(p);
        p = read_tmp(r, "enemies.bin", &n0);
        unsigned char *lua = read_tmp(r, "ai.lua0", &n1);
        int ne = p ? enemy_block_types(gs, i, tab[i].pack, p, lua, lua ? (int)n1 : 0) : -1;
        free(lua);
        free(p);
        printf("world: block %d types: objects %d, effects %d, enemies %d\n", i, no, nf, ne);
        map_block_dg(i, &r->pk);  /* its draw groups (map.c, src/drawgrp.c), kept from boot */
    }
    if (cur >= 0 && nblk > 1)
        mapstream_arena();  /* now: no enemy model is loaded yet (enemy_init stripped them all) */
}

static void step_time(unsigned t0)
{
    unsigned us = (ticks() - t0) / 295;
    if (us > world_stats.on_us_max)
        world_stats.on_us_max = us;
}

/* A block's lite set is read: hand its blobs to the systems (no copies, no IOP waits) */
static void turn_on(int i)
{
    BlockRt *r = &rt[i];
    unsigned t0 = ticks();
    int ok = col_load(i, r->blob[E_COL]), cok = ok;
    ok &= map_block_init(i, r->blob[E_TAB], NULL, i);
    if (!ok)
        printf("world: block %d: collision %s (%.4s), map %.4s\n", i, cok ? "ok" : "FAILED", (const char *)r->blob[E_COL],
               (const char *)r->blob[E_TAB]);
    if (ok) {
        map_block_stream_open(i, &r->pk);
        objects_block_add(i);
        fx_block_on(i, 1);
        enemy_block_add(i);  /* before event_block_on: its constructors may disable these characters (A2) */
    }
    if (ok)
        event_block_on(i, r->size[E_EV] ? r->blob[E_EV] : NULL);
    r->state = W_ON;
    world_stats.loaded++;
    world_stats.loads++;
    world_stats.load_ms = (ticks() - r->t0) / 294912;
    world_stats.read_kb = r->total >> 10;
    step_time(t0);
    printf("world: block %d %s on%s: %u KB in %u ms, collision %d tris, step %u us\n", i, tab[i].pack,
           ok ? "" : " (FAILED)", r->total >> 10, world_stats.load_ms, col_block_tris(i), (ticks() - t0) / 295);
}

static void turn_off(int i)
{
    BlockRt *r = &rt[i];
    unsigned t0 = ticks();
    event_block_off(i);  /* before the collision its hit parts point into goes */
    objects_block_remove(i);
    fx_block_on(i, 0);
    enemy_block_remove(i);
    col_unload(i);
    map_block_drop(i);
    r->state = W_DRAIN;  /* the slot's memory is reused only after the chains that drew from it are done */
    r->drain = WORLD_DRAIN;
    world_stats.loaded--;
    world_stats.unloads++;
    step_time(t0);
    printf("world: block %d %s off\n", i, tab[i].pack);
}

static void free_slot(int i)
{
    BlockRt *r = &rt[i];
    if (r->slot >= 0)
        slot_of[r->slot] = -1;
    r->slot = -1;
    mapstream_free(r->mem, r->total);
    r->mem = NULL;
    world_stats.pool_used_kb -= r->total >> 10;
    r->state = W_OFF;
}

/* Continue the background read of block i's lite set */
static void read_step(int i)
{
    BlockRt *r = &rt[i];
    int res;
    if (world_stats.reading == i) {
        if (!pack_async_done(&res))
            return;  /* still in flight */
        unsigned n = r->size[r->ent] - r->done < WORLD_READ ? r->size[r->ent] - r->done : WORLD_READ;
        world_stats.reading = -1;
        if (res != (int)n) {
            printf("world: block %d read %d of %u bytes\n", i, res, n);
            world_stats.fails++;
            free_slot(i);
            return;
        }
        InvalidDCache((char *)r->blob[r->ent] + r->done, (char *)r->blob[r->ent] + r->done + n - 1);
        if ((r->done += n) == r->size[r->ent])
            r->ent++, r->done = 0;
        while (r->ent < NENT && !r->size[r->ent])  /* optional entry the pack does not have */
            r->ent++;
        if (r->ent == NENT) {
            turn_on(i);
            return;
        }
    }
    unsigned n = r->size[r->ent] - r->done < WORLD_READ ? r->size[r->ent] - r->done : WORLD_READ;
    if (pack_read_async(&r->pk, r->off[r->ent] + r->done, (char *)r->blob[r->ent] + r->done, n))
        world_stats.reading = i;  /* else mapstream's read is in flight: try next frame */
}

static int start(int i)
{
    BlockRt *r = &rt[i];
    int s = 0;
    while (s < WORLD_SLOTS && slot_of[s] >= 0)
        s++;
    if (s == WORLD_SLOTS)
        return 0;
    unsigned char *p = mapstream_alloc(r->total, feet_now);
    if (!p)
        return 0;  /* near cells still being read or drawn: next frame */
    r->mem = p;
    slot_of[s] = i;
    r->slot = s;
    for (unsigned e = 0; e < NENT; e++) {
        r->blob[e] = p;
        p += (r->size[e] + 127) & ~127u;
    }
    r->ent = r->done = 0;
    while (r->ent < NENT && !r->size[r->ent])
        r->ent++;
    r->state = W_READING;
    r->t0 = ticks();
    world_stats.pool_used_kb += r->total >> 10;
    printf("world: block %d %s loading (%u KB)\n", i, tab[i].pack, r->total >> 10);
    return 1;
}

/* A full swap reads mapres.bin into the pool, then the block's raw bump.bin behind it when both fit (normal-map relief
 * while the block is full: map_block_bump); the lent tail starts after them. Read offsets are pool offsets. */
static unsigned bump_at(const BlockRt *r)
{
    return (r->res_size + 127) & ~127u;
}

static unsigned res_total(const BlockRt *r)
{
    return r->bump_size && bump_at(r) + r->bump_size <= full_size ? bump_at(r) + r->bump_size : r->res_size;
}

/* the next read at pool offset done: bytes, and its offset in the pack */
static unsigned res_n(const BlockRt *r, unsigned done, unsigned *src)
{
    unsigned end = done < r->res_size ? r->res_size : res_total(r);
    *src = done < r->res_size ? r->res_off + done : r->bump_off + done - bump_at(r);
    return end - done < WORLD_READ ? end - done : WORLD_READ;
}

/* The full pool follows the player: the old full block goes lite, WORLD_DRAIN frames later the new one's streams are
 * read into the pool (background, WORLD_READ at a time), then it draws them */
static void full_step(void)
{
    if (res_blk < 0) {
        if (cur == full_blk || (in_cur_s < WORLD_SWAP_S && !warping) || rt[cur].state != W_ON)
            return;
        if (full_blk >= 0) {
            map_block_streams(full_blk, NULL);
            printf("world: block %d lite\n", full_blk);
        }
        full_blk = world_stats.full = -1;
        res_blk = cur, res_reclaim = 1, res_drain = warping ? 0 : WORLD_DRAIN, res_done = 0, res_reading = 0, res_t0 = ticks();
        return;
    }
    if (res_reclaim) {  /* the near cells in the lent tail go first, then the chains drain */
        if (lent && !mapstream_reclaim(full_pool + full_size - lent, lent))
            return;
        lent = 0, res_reclaim = 0;
    }
    if (res_drain > 0) {
        res_drain--;
        return;
    }
    BlockRt *r = &rt[res_blk];
    int res;
    unsigned src;
    if (res_reading) {
        if (!pack_async_done(&res))
            return;
        unsigned n = res_n(r, res_done, &src);
        res_reading = 0;
        world_stats.reading = -1;
        if (res != (int)n) {
            printf("world: block %d streams read %d of %u bytes at %u + %u (fd %d)\n", res_blk, res, n, r->res_off, res_done,
                   r->pk.fd);
            world_stats.fails++;
            res_blk = -1;
            return;
        }
        InvalidDCache(full_pool + res_done, full_pool + res_done + n - 1);
        if ((res_done += n) == r->res_size && res_total(r) > r->res_size)
            res_done = bump_at(r);
        if (res_done == res_total(r)) {
            unsigned t0 = ticks();
            if (rt[res_blk].state == W_ON) {
                map_block_streams(res_blk, full_pool);
                if (res_total(r) > r->res_size)
                    map_block_bump(res_blk, full_pool + bump_at(r));
                full_blk = world_stats.full = res_blk;
                lend_tail(res_total(r));
                world_stats.full_ms = (ticks() - res_t0) / 294912;
                printf("world: block %d full: %u KB streams in %u ms\n", res_blk, r->res_size >> 10, world_stats.full_ms);
            }
            step_time(t0);
            res_blk = -1;
            return;
        }
    }
    unsigned n = res_n(r, res_done, &src);
    if (pack_read_async(&r->pk, src, full_pool + res_done, n))
        res_reading = 1, world_stats.reading = res_blk;
}

static void step(const float *feet, float dt)
{
    /* the block under the player: the one whose collision floor is under the feet */
    ColHit h;
    int was = cur;
    memcpy(feet_now, feet, sizeof feet_now);
    if (col_floor(feet[0], feet[2], feet[1] + 1.0f, feet[1] - 3.0f, COL_SOLID, &h) && h.block < nblk &&
        rt[h.block].state == W_ON)
        cur = h.block;
    in_cur_s = cur == was ? in_cur_s + dt : 0;
    world_stats.current = cur;
    for (int i = 0; i < nblk; i++)  /* slots of unloaded blocks come free once the chains are done */
        if (rt[i].state == W_DRAIN && (--rt[i].drain <= 0 || warping))
            free_slot(i);
    /* one background read at a time: the full pool first (the player's own block), then neighbours' lite sets */
    if (res_blk >= 0 || (cur != full_blk && (in_cur_s >= WORLD_SWAP_S || warping) && world_stats.reading < 0)) {
        if (res_reading || world_stats.reading < 0)
            full_step();
        if (res_blk >= 0)
            goto unload;
    }
    for (int i = 0; i < nblk; i++)
        if (rt[i].state == W_READING) {
            read_step(i);
            goto unload;
        }
    {   /* start the nearest wanted neighbour of the current block */
        int want = -1;
        float wd = WORLD_LOAD_R;
        for (int i = 1; i < nblk; i++)
            if (rt[i].open && rt[i].state == W_OFF && (tab[cur].nbr >> i & 1)) {
                float d = box_d(&tab[i], feet);
                if (d < wd)
                    want = i, wd = d;
            }
        if (want >= 0 && start(want))
            read_step(want);
    }
unload:
    /* unload far blocks (never the boot block or the one under the player, nor the full one) */
    for (int i = 1; i < nblk; i++)
        if (i != cur && i != full_blk && i != res_blk && rt[i].state == W_ON && box_d(&tab[i], feet) > WORLD_UNLOAD_R)
            turn_off(i);
}

/* settled: nothing reading, no swap pending, the block under the player full, every wanted neighbour on */
static int settled(const float *feet)
{
    if (world_stats.reading >= 0 || res_blk >= 0 || (rt[cur].state == W_ON && cur != full_blk))
        return 0;
    for (int i = 1; i < nblk; i++)
        if (rt[i].state == W_READING || (rt[i].open && rt[i].state == W_OFF && (tab[cur].nbr >> i & 1) &&
                                         box_d(&tab[i], feet) < WORLD_LOAD_R))
            return 0;
    return 1;
}

/* A warp: finish every load / unload / swap the new position needs now (the frame waits, as DSR's loading screen) */
static void settle(const float *feet)
{
    unsigned t0 = ticks();
    vu1_wait();  /* the last frame's VIF chain no longer reads the pools */
    warping = 1;
    int n = 0;
    while (!settled(feet) && (ticks() - t0) / 294912 < 30000) {
        mapstream_update(feet);  /* collects a cell read in flight so the async slot comes free */
        step(feet, 0);
        n++;
    }
    warping = 0;
    in_cur_s = WORLD_SWAP_S;
    printf("world: warp to %.1f %.1f %.1f settled in %u ms (%d steps): block %d, full %d, %d loaded\n", feet[0], feet[1],
           feet[2], (ticks() - t0) / 294912, n, cur, full_blk, world_stats.loaded);
}

void world_update(const float *feet, float dt)
{
    if (cur < 0)
        return;
    float dx = feet[0] - last_feet[0], dy = feet[1] - last_feet[1], dz = feet[2] - last_feet[2];
    int warp = dx * dx + dy * dy + dz * dz > WORLD_WARP * WORLD_WARP;  /* also the first frame: boot settles */
    memcpy(last_feet, feet, sizeof last_feet);
    if (warp) {
        step(feet, 0);  /* the block under the new position (its collision may need loading first: settle loops) */
        settle(feet);
        return;
    }
    step(feet, dt);
}
