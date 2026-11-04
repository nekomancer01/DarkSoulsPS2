#include <kernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "world.h"
#include "loading.h"
#include "pack.h"
#include "collision.h"
#include "map.h"
#include "mapstream.h"
#include "vu1.h"
#include "objects.h"
#include "fx.h"
#include "enemy.h"

/* Which blocks load: the collision triangle under the player (world.bin links, tools/export_world.py). Between two
 * maps that is DSR's own rule, the ConnectCollision part (MSB type 11) under the player; inside one MSB (DSR keeps it
 * whole) a triangle loads the neighbours within one 32 m cut cell and keeps them within two (ASSUMED lead, TECH_DEBT
 * world-3). With no floor under the player (a warp into a block not loaded yet, a fall) the blocks whose bounds hold the
 * feet are wanted too, so a warp settles like DSR's loading screen. */
/* A block the triangle under the player no longer keeps unloads after WORLD_KEEP_S (ASSUMED): Firelink's aqueduct
 * ConnectCollision parts are narrow, and stepping off one between stairs unloaded Burg west and read it again 1.3 s
 * later (world_route.txt 2026-11-01) */
#define WORLD_KEEP_S 5.0f
/* Bytes per background read: one at a time through pack.c's async slot, shared with mapstream.c (a near cell waits for
 * at most one of these) */
#define WORLD_READ (128u << 10)
/* Blocks resident besides the boot block. Their lite sets live in mapstream.c's near-cell arena (mapstream_alloc drops
 * the farthest near cells to make room), so a world build costs no RAM while no neighbour is loaded (TECH_DEBT world-5).
 * 3: the block under the player plus both neighbours at a seam inside the Burg (the bonfire / bridge block keeps Burg
 * west and the Parish block); the Burg blocks' streams are smaller than Firelink's, so the full pool lends the rest */
#define WORLD_SLOTS 3
/* The block the player stands in is "full" (its map's resident streams in the one full pool, sized for the largest
 * block's mapres.bin); every other loaded block is "lite" (near cells only). A new block becomes full after the player
 * stood in it for WORLD_SWAP_S (no swapping back and forth on a boundary); ASSUMED value. */
#define WORLD_SWAP_S 1.0f
/* frames a buffer stays untouched after the map stopped drawing from it (VIF chains of the last two frames) */
#define WORLD_DRAIN 3
/* metres moved in one frame that count as a warp (test script spawns; later bonfire warps / respawns): DSR shows a
 * loading screen there, so world_update then loads and swaps synchronously before the frame is drawn */
#define WORLD_WARP 20.0f
/* metres above / below a block's collision that still count as inside it when there is no floor under the feet
 * (ASSUMED: a fall or a jump out of its range; the blocks of one world are far apart in height or not at all) */
#define WORLD_Y_MARGIN 10.0f
/* seconds before a block whose lite set found no room tries again (ASSUMED: the near cells change over ~1 s of
 * walking; every try may drop drawn cells, which then stream back) */
#define WORLD_RETRY_S 1.0f
/* heap a lite set's texture entries leave free when they fall back to it (AGENTS.md: 1.5 MB in the heaviest spot) */
#define WORLD_HEAP_FLOOR (1536u << 10)
/* heap kept out of the near-cell arena beyond MS_RESERVE, for enemy models: world builds allocate the arena at boot
 * (world_systems) while no enemy model is loaded, so this holds every model streamed in later. Single-area builds
 * allocate it on the first frame after the bonfire's types loaded (3.6 MB in Firelink) and keep 3 MB more. Measured
 * 2026-11-01: an arena created when only 3 types were loaded (spawn at the aqueduct) was 7.7 MB and two blocks'
 * types then ran out of RAM (world_cross.txt); 3.8 MB here gives the 5.5 MB of a bonfire boot wherever the player
 * starts: near cells >= 4.2 MB at the Firelink junction, heap outside the arena >= ~1.6 MB */
#define WORLD_ENEMY_RESERVE (3800u << 10)
/* The player is held at the edge of a block whose collision has not loaded yet (world_hold) instead of walking or
 * falling into it: from cdrom0: the player walked off the Burg bonfire block's floor before the next block's lite set
 * was read, fell to the map's floor far below or out of the map and respawned elsewhere (user report 2026-11-04,
 * TECH_DEBT world-8). DSR has the whole map resident and never needs this. A loaded floor up to WORLD_HOLD_DROP below
 * the feet is the floor of a jump or a short drop, not of another place: no hold (ASSUMED: higher than a running
 * jump, lower than the drops between the Burg's levels). */
#define WORLD_HOLD_DROP 8.0f
/* seconds of holding before the frame waits for the blocks as on a warp (loading screen; ASSUMED: a Burg lite set
 * alone reads in ~1.5 s from cdrom0:) */
#define WORLD_HOLD_S 3.0f

/* The lite set of a block, read raw from its pack (tools/pack.py raw: entries, read in place) into its slot.
 * events.bin (A2's tools/export_emevd.py) is optional: a pack without it loads with no events. objects.bin and fx.bin
 * (last) are read again for their texture pixels: uploaded when the block turns on (objects_block_add, fx_block_on;
 * their tables were kept at boot). They are read into an arena block of their own, freed once the block is on: in the
 * lite set's block they made it 1.3 MB of one piece, and a warp into the Parish found no room for that (settle loops,
 * 2026-11-01). */
enum { E_COL, E_TAB, E_EV, E_OBJ, E_FX, NENT };
static const char *const ENTRIES[NENT] = {"collision.bin", "maptab.bin", "events.bin", "objects.bin", "fx.bin"};
#define E_OPTIONAL(e) ((e) >= E_EV)
#define E_TEX E_OBJ  /* first of the texture entries (their own arena block) */

#include "event.h"  /* event_block_on / _off (A2): the blob stays world.c's (arena) until _off */

typedef struct {
    char pack[16], map[16];
    float mn[2], mx[2], y0, y1;  /* xz bounds, height range of the block's collision */
    unsigned flags, nbr;
} WorldBlock;
_Static_assert(sizeof(WorldBlock) == 64, "WorldBlock");
/* triangles [first, first + n) of a block's collision.bin want the blocks in load and keep those in keep */
typedef struct {
    unsigned first;
    unsigned short n;
    unsigned char load, keep;
} WorldLink;
_Static_assert(sizeof(WorldLink) == 8, "WorldLink");

enum { W_OFF, W_READING, W_ON, W_DRAIN };

typedef struct {
    Pack pk;
    int open, state, slot, drain;
    int tex_held;  /* the texture entries' blobs are still allocated (until on) */
    float retry_s;  /* start() found no room: seconds until it tries again */
    unsigned off[NENT], size[NENT], total, tex_total, res_off, res_size, bump_off, bump_size;
    void *blob[NENT];
    unsigned ent, done;  /* read progress: entry, bytes of it */
    unsigned t0;
} BlockRt;

WorldStats world_stats = {.reading = -1, .full = -1};
static const WorldBlock *tab;
static const unsigned *lrange;  /* per block: first link, links */
static const WorldLink *links;
static unsigned want_m, keep_m;  /* blocks the triangle under the player loads / keeps */
static float unkept_s[WORLD_MAX];  /* seconds a loaded block has not been in keep_m */
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
static int lite_first;  /* a wanted block's lite set waits: the full swap issues no new read (step) */
/* blocks the player is held for: found by world_hold this frame, latched for step(); blocks a settle could not load
 * (no more holding for them until they turn on); seconds held */
static unsigned hold_m, hold_want, hold_gone;
static float hold_s;

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
    pack_async_free();
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
    if (!h || memcmp(h, "DSW1", 4) || h[1] != 3 || !h[2] || strncmp(((const WorldBlock *)(h + 4))->pack, boot_area, 16)) {
        free(wb);
        printf("world: no world table for %s, single area\n", boot_area);
        return 0;
    }
    nblk = h[2] < WORLD_MAX ? h[2] : WORLD_MAX;
    if (nblk > MAP_INST || nblk > COL_BLOCKS)
        nblk = MAP_INST < COL_BLOCKS ? MAP_INST : COL_BLOCKS;  /* map instance / collision block = block index */
    tab = (const WorldBlock *)(h + 4);
    lrange = (const unsigned *)(tab + h[2]);
    links = (const WorldLink *)(lrange + 2 * h[2]);
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
            if (i && e >= E_TEX)  /* the boot block's collision, objects and effects are main.c's */
                r->tex_total += (r->size[e] + 127) & ~127u;
            else if (i || e == E_TAB)
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
        printf("world: block %d %s (%s) x %.0f..%.0f z %.0f..%.0f nbr %x links %u lite %u KB res %u KB%s\n", i,
               tab[i].pack, tab[i].map, tab[i].mn[0], tab[i].mx[0], tab[i].mn[1], tab[i].mx[1], tab[i].nbr, lrange[2 * i + 1],
               rt[i].total >> 10, rt[i].res_size >> 10, rt[i].open ? "" : " (no pack)");
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

const char *world_map(int block) { return tab && block >= 0 && block < nblk ? tab[block].map : NULL; }

/* every block's enemy / breakable / effect types, once, at boot before the near-cell arena exists (DSR keeps a whole
 * area's resources loaded; the PS2 keeps the types and places the instances per block): each raw entry read into a
 * temporary buffer, handed over, freed. What the types keep (chr.bin, compacted models, fx tables, Lua) lies below
 * the arena, so the heap above it stays free for enemy models streaming in and out (loading them after the arena
 * fragmented it: Firelink smoke 2026-11-01 02:37, model.bin 379K OUT OF RAM with 3 MB free). The type data stays for
 * the session; textures are in VRAM only while their block is on (breakables, effects: from the entries turn_on reads
 * again) or their characters are near (enemy.c), TECH_DEBT world-4. */
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
        free_ents(r, E_TEX, NENT);
        r->tex_held = 0;
        world_stats.pool_used_kb -= r->tex_total >> 10;
    }
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
        objects_block_add(i, r->size[E_OBJ] ? r->blob[E_OBJ] : NULL);
        fx_block_on(i, 1, r->size[E_FX] ? r->blob[E_FX] : NULL);
        enemy_block_add(i);  /* before event_block_on: its constructors may disable these characters (A2) */
    }
    if (ok)
        event_block_on(i, r->size[E_EV] ? r->blob[E_EV] : NULL);
    free_tex(r);  /* uploaded: back to the near cells */
    r->state = W_ON;
    hold_gone &= ~(1u << i);
    world_stats.loaded++;
    world_stats.loads++;
    world_stats.load_ms = (ticks() - r->t0) / 294912;
    world_stats.read_kb = (r->total + r->tex_total) >> 10;
    step_time(t0);
    printf("world: block %d %s on%s: %u KB in %u ms, collision %d tris, step %u us; lite sets %u KB, lent %u KB\n", i,
           tab[i].pack, ok ? "" : " (FAILED)", r->total >> 10, world_stats.load_ms, col_block_tris(i), (ticks() - t0) / 295,
           world_stats.pool_used_kb, lent >> 10);
}

static void turn_off(int i)
{
    BlockRt *r = &rt[i];
    unsigned t0 = ticks();
    event_block_off(i);  /* before the collision its hit parts point into goes */
    objects_block_remove(i);
    fx_block_on(i, 0, NULL);
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
    free_ents(r, 0, E_TEX);
    world_stats.pool_used_kb -= r->total >> 10;
    free_tex(r);
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
    if (s == WORLD_SLOTS || (r->retry_s > 0 && !warping))
        return 0;
    /* One arena block per entry, largest first: the near cells being drawn split the arena, and the Burg block's
     * 1508 KB in one piece found no hole with 2.1 MB free in it (largest 1068 KB; 2026-11-03, 2.8 MB arena of
     * 9e6dd67). Texture entries live only until on: without room in the arena they take the heap while it keeps
     * WORLD_HEAP_FLOOR. All or nothing; a failed start waits WORLD_RETRY_S, since each try may drop drawn cells. */
    unsigned order[NENT], n = 0;
    for (unsigned e = 0; e < NENT; e++)
        if (r->size[e]) {
            unsigned k = n++;
            while (k && r->size[order[k - 1]] < r->size[e])
                order[k] = order[k - 1], k--;
            order[k] = e;
        }
    memset(r->blob, 0, sizeof r->blob);
    for (unsigned k = 0; k < n; k++) {
        unsigned e = order[k], sz = (r->size[e] + 127) & ~127u;
        r->blob[e] = mapstream_alloc(sz, feet_now);
        if (!r->blob[e] && e >= E_TEX && enemy_ram_free() >= WORLD_HEAP_FLOOR + sz)
            r->blob[e] = memalign(128, sz);
        if (!r->blob[e]) {
            free_ents(r, 0, NENT);
            r->retry_s = WORLD_RETRY_S;
            return 0;  /* near cells still being read or drawn: try again */
        }
    }
    r->tex_held = 1;
    slot_of[s] = i;
    r->slot = s;
    r->ent = r->done = 0;
    while (r->ent < NENT && !r->size[r->ent])
        r->ent++;
    r->state = W_READING;
    r->t0 = ticks();
    world_stats.pool_used_kb += (r->total + r->tex_total) >> 10;
    printf("world: block %d %s loading (%u KB + %u KB textures)\n", i, tab[i].pack, r->total >> 10, r->tex_total >> 10);
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
                printf("world: block %d full: %u KB streams in %u ms; lite sets %u KB, lent %u KB\n", res_blk,
                       r->res_size >> 10, world_stats.full_ms, world_stats.pool_used_kb, lent >> 10);
            }
            step_time(t0);
            res_blk = -1;
            return;
        }
    }
    if (lite_first)
        return;  /* the slot goes to a wanted block's lite set first (step) */
    unsigned n = res_n(r, res_done, &src);
    if (pack_read_async(&r->pk, src, full_pool + res_done, n))
        res_reading = 1, world_stats.reading = res_blk;
}

/* a wanted block's lite set is being read or could start now (start()'s conditions, room aside) */
static int lite_pending(void)
{
    int free_s = 0;
    for (int s = 0; s < WORLD_SLOTS; s++)
        free_s |= slot_of[s] < 0;
    for (int i = 1; i < nblk; i++)
        if (rt[i].state == W_READING ||
            (free_s && rt[i].open && rt[i].state == W_OFF && (want_m >> i & 1) && (rt[i].retry_s <= 0 || warping)))
            return 1;
    return 0;
}

/* the link record of triangle tri of block b, or NULL (binary search: links are sorted by first triangle) */
static const WorldLink *link_at(int b, unsigned tri)
{
    const WorldLink *l = links + lrange[2 * b];
    int lo = 0, hi = (int)lrange[2 * b + 1] - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        if (tri < l[m].first)
            hi = m - 1;
        else if (tri >= l[m].first + l[m].n)
            lo = m + 1;
        else
            return &l[m];
    }
    return NULL;
}

static void step(const float *feet, float dt)
{
    /* the block under the player: the one whose collision floor is under the feet; its triangle picks the blocks
     * wanted (world.bin links) */
    ColHit h = {.tri = -1};
    int was = cur;
    unsigned wm = want_m, km = keep_m;
    memcpy(feet_now, feet, sizeof feet_now);
    if (col_floor(feet[0], feet[2], feet[1] + 1.0f, feet[1] - 3.0f, COL_SOLID, &h) && h.block < nblk &&
        rt[h.block].state == W_ON) {
        cur = h.block;
        const WorldLink *l = link_at(h.block, h.tri);
        wm = l ? l->load : 0, km = l ? l->keep : 0;
    } else {  /* no floor: the blocks around the feet as well (a warp settles them, as DSR's loading screen). Their
               * height range too: the Asylum lies over the Burg and Firelink in x / z, and loading all three at once
               * left no room in the near arena (settle looped, 2026-11-02) */
        for (int i = 1; i < nblk; i++)
            if (box_d(&tab[i], feet) <= 0.0f && feet[1] >= tab[i].y0 - WORLD_Y_MARGIN &&
                feet[1] <= tab[i].y1 + WORLD_Y_MARGIN)
                wm |= 1u << i, km |= 1u << i;
    }
    wm |= hold_want, km |= hold_want;  /* the blocks the player is held for (world_hold) */
    if (wm != want_m || km != keep_m)
        printf("world: block %d tri %d: want %x keep %x\n", cur, h.tri, wm, km);
    want_m = wm, keep_m = km | wm;
    world_stats.want = want_m;
    for (int i = 0; i < nblk; i++)
        unkept_s[i] = keep_m >> i & 1 ? 0 : unkept_s[i] + dt;
    for (int i = 0; i < nblk; i++)
        rt[i].retry_s = rt[i].retry_s > dt ? rt[i].retry_s - dt : 0;
    in_cur_s = cur == was ? in_cur_s + dt : 0;
    world_stats.current = cur;
    for (int i = 0; i < nblk; i++)  /* slots of unloaded blocks come free once the chains are done */
        if (rt[i].state == W_DRAIN && (--rt[i].drain <= 0 || warping))
            free_slot(i);
    /* one background read at a time. Wanted blocks' lite sets go first: they hold the collision the player walks onto
     * next, while the full pool only adds drawing detail to the block underfoot. With the full swap first, from cdrom0:
     * a Burg swap (4.9 MB, ~5 s at ~1 MB/s) kept the next block's collision from loading and the player walked off
     * the loaded floor (2026-11-04, TECH_DEBT world-8). A swap chunk in flight is collected first; the swap then
     * issues no new chunk while a lite set waits. */
    lite_first = lite_pending();
    if (res_reading || (!lite_first && (res_blk >= 0 || (cur != full_blk && (in_cur_s >= WORLD_SWAP_S || warping) &&
                                                          world_stats.reading < 0)))) {
        if (res_reading || world_stats.reading < 0)
            full_step();
        if (res_reading || !lite_first)
            goto unload;
    }
    for (int i = 0; i < nblk; i++)
        if (rt[i].state == W_READING) {
            if (world_stats.reading < 0 || world_stats.reading == i)
                read_step(i);
            goto unload;
        }
    {   /* start the nearest wanted block */
        int want = -1;
        float wd = 1e30f;
        for (int i = 1; i < nblk; i++)
            if (rt[i].open && rt[i].state == W_OFF && (want_m >> i & 1)) {
                float d = box_d(&tab[i], feet);
                if (d < wd)
                    want = i, wd = d;
            }
        if (want >= 0 && start(want))
            read_step(want);
    }
unload:
    /* unload blocks the triangle under the player has not kept for WORLD_KEEP_S (never the boot block or the one under the player,
     * nor the full one) */
    for (int i = 1; i < nblk; i++)
        if (i != cur && i != full_blk && i != res_blk && rt[i].state == W_ON && !(keep_m >> i & 1) &&
            (unkept_s[i] >= WORLD_KEEP_S || warping))
            turn_off(i);
}

/* settled: nothing reading, no swap pending, the block under the player full, every wanted neighbour on */
static int settled(const float *feet)
{
    if (world_stats.reading >= 0 || res_blk >= 0 || (rt[cur].state == W_ON && cur != full_blk))
        return 0;
    for (int i = 1; i < nblk; i++)
        if (rt[i].state == W_READING || (rt[i].open && rt[i].state == W_OFF && (want_m >> i & 1)))
            return 0;
    return 1;
}

/* A warp: finish every load / unload / swap the new position needs now (the frame waits, as DSR's loading screen) */
static void settle(const float *feet)
{
    unsigned t0 = ticks();
    vu1_wait();  /* the last frame's VIF chain no longer reads the pools */
    loading_begin();  /* DSR's loading screen while the frame waits (src/loading.c) */
    warping = 1;
    int n = 0;
    while (!settled(feet) && (ticks() - t0) / 294912 < 30000) {
        pack_async_free();  /* a background enemy model load (enemy.c) holds the slot no longer */
        mapstream_update(feet);  /* collects a cell read in flight so the async slot comes free */
        step(feet, 0);
        loading_tick(1);
        n++;
    }
    warping = 0;
    in_cur_s = WORLD_SWAP_S;
    event_flush();  /* the new blocks' constructors finish behind the loading screen (A2 event-14) */
    loading_end();
    printf("world: warp to %.1f %.1f %.1f settled in %u ms (%d steps): block %d, full %d, %d loaded\n", feet[0], feet[1],
           feet[2], (ticks() - t0) / 294912, n, cur, full_blk, world_stats.loaded);
}

int world_hold(const float *feet)
{
    if (cur < 0)
        return 0;
    ColHit h;
    if (col_floor(feet[0], feet[2], feet[1] + 1.0f, feet[1] - WORLD_HOLD_DROP, COL_SOLID, &h))
        return 0;
    unsigned m = 0;
    for (int i = 1; i < nblk; i++)
        if (rt[i].open && rt[i].state != W_ON && !(hold_gone >> i & 1) && box_d(&tab[i], feet) <= 0.0f &&
            feet[1] >= tab[i].y0 - WORLD_Y_MARGIN && feet[1] <= tab[i].y1 + WORLD_Y_MARGIN)
            m |= 1u << i;
    hold_m |= m;
    return m != 0;
}

void world_update(const float *feet, float dt)
{
    if (cur < 0)
        return;
    float dx = feet[0] - last_feet[0], dy = feet[1] - last_feet[1], dz = feet[2] - last_feet[2];
    int warp = dx * dx + dy * dy + dz * dz > WORLD_WARP * WORLD_WARP;  /* also the first frame: boot settles */
    memcpy(last_feet, feet, sizeof last_feet);
    if (hold_m && !hold_want)
        printf("world: player held at %.1f %.1f %.1f for blocks %x\n", feet[0], feet[1], feet[2], hold_m);
    hold_want = hold_m, hold_m = 0;
    hold_s = hold_want && !warp ? hold_s + dt : 0;
    world_stats.hold_ms += hold_s > 0 ? (unsigned)(dt * 1000) : 0;
    if (warp || hold_s >= WORLD_HOLD_S) {
        /* a warp, or held too long: the frame waits for the blocks (loading screen) */
        step(feet, 0);  /* the block under the new position (its collision may need loading first: settle loops) */
        settle(feet);
        if (hold_s > 0) {
            world_stats.hold_settles++;
            for (int i = 1; i < nblk; i++)
                if ((hold_want >> i & 1) && rt[i].state != W_ON) {
                    hold_gone |= 1u << i;  /* could not load: the player walks / falls on as before */
                    printf("world: block %d did not load under the player: no more holding for it\n", i);
                }
            hold_want = 0, hold_s = 0;
        }
        return;
    }
    step(feet, dt);
}
