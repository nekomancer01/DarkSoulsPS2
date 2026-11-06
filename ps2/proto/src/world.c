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
 * west and the Parish block) */
#define WORLD_SLOTS 3
/* Every loaded block draws the same way: its map's resident level streams per cell from its mapres.bin and its
 * textures' mip 0 per texture (mapstream.c mapstream_res_open via map.c), in the near arena with the near cells, so
 * there is no full pool and no full swap (Burg 4.9 MB / Firelink 8.5 MB, 5-14 s from cdrom0:, TECH_DEBT world-2 /
 * world-5). Only the normal-map relief (bump.bin) is per block: read for the block the player stood in for
 * WORLD_SWAP_S (no reading back and forth on a boundary; ASSUMED value). */
#define WORLD_SWAP_S 1.0f
/* Largest near arena: what the heap leaves beyond the reserves at boot (the old full pool's 7.8 MB went to it). It
 * took 0.4 MB more than full pool + arena did and route_enemies' RAM low point at the Burg entry fell 1.30 -> 0.60 MB
 * (ISO, 2026-11-05): WORLD_ARENA_LESS more stays outside it */
#define WORLD_ARENA_LESS (1024u << 10)
#define WORLD_POOL (24u << 20)
/* A warp settles once the sky and the resident cells within WORLD_SETTLE_R of the feet are in, those of the block
 * the player is in out to WORLD_SETTLE_FOCUS_R: DSR's loading screen lasts until the area is loaded, and with 40 m
 * Firelink's horizon tree line and the aqueduct's rock wall were still missing 3 s after a warp back from the Burg
 * (ISO world_route, 2026-11-05); 160 m made the far-north and Burg warps 11-14 s from cdrom0:. The rest streams in
 * nearest first. ASSUMED radii. */
#define WORLD_SETTLE_R 40.0f
#define WORLD_SETTLE_FOCUS_R 96.0f
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
/* heap a lite set's entries and texture buffer leave free when they fall back to it (AGENTS.md: 1.5 MB in the
 * heaviest spot) */
#define WORLD_HEAP_FLOOR (1536u << 10)
/* heap a waited-for block's lite entries leave above WORLD_HEAP_FLOOR for the enemy models that stream in once it is
 * on (ASSUMED: two of the Burg's larger model.bin, 578 + 590 KB; tools/export_chr.py outputs, 2026-11-05). Without
 * it the lite sets took the heap the models needed: 10-21 model.bin OUT OF RAM retries in route_enemies.txt */
#define WORLD_HEAP_MODELS (1200u << 10)
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
/* ms a settle goes on while a wanted block finds no room and nothing can be turned off for it (room_stuck) */
#define WORLD_STUCK_MS 2000

/* The lite set of a block, read raw from its pack (tools/pack.py raw: entries, read in place) into its slot.
 * events.bin (A2's tools/export_emevd.py) is optional: a pack without it loads with no events. The block turns on in
 * two steps: once collision, maptab and events are read its collision and map are on (the floor the player walks onto
 * next; turn_on_col), then objects.bin and fx.bin are read for their texture pixels, one after the other into one
 * buffer (tex_total = the larger of the two), uploaded (objects_block_add, fx_block_on; their tables were kept at
 * boot), and the block's enemies and events follow (turn_on_rest). The buffer is taken only after the collision is on
 * and freed after the uploads: allocated with the lite set (one arena block per entry) the Burg west block needed
 * 1508 + 475 KB at once and found no room beside the bonfire block with enemies loaded (2026-11-05, TECH_DEBT
 * world-5). */
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
    int tex_held;  /* the texture buffer (texbuf) is allocated */
    int stage;     /* while W_ON: 1 = collision and map on, the textures / enemies / events still to come; 2 = all on */
    void *texbuf;  /* objects.bin, then fx.bin (tex_total bytes) */
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
static unsigned slot_size;
static float feet_now[3];  /* for start(): mapstream_alloc evicts the cells farthest from here */
/* relief (bump.bin): block it is for (-1 none; its buffer may be NULL: none, or no room), block being read, the read in
 * flight, progress; a dropped buffer waits WORLD_DRAIN frames before it is freed */
static int bump_blk = -1, bump_rd = -1, bump_reading;
/* the ticket of this file's read in flight (pack_req_read): a lite set chunk or a relief chunk, never both
 * (world_stats.reading names its block) */
static int w_tk;
static unsigned char *bump_buf, *bump_old;
static unsigned bump_done, bump_t0, bump_old_n;
static int bump_old_drain;
static float in_cur_s;  /* seconds the player has stood in cur */
static float last_feet[3];
static int warping;  /* inside settle(): drains and the swap delay are skipped */
static int lite_first;  /* a wanted block's lite set waits: the relief read and mapstream.c issue no new read (step) */
static int room_stuck;  /* this step a wanted block found no room and no kept block could make it some (room_for) */
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
    int r = 0, t;
    while (!(t = pack_req_read(pk, off, dst, n)))
        pack_pump();
    while (!pack_req_done(t, &r))
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
            if (i && e >= E_TEX) {  /* the boot block's collision, objects and effects are main.c's */
                if (((r->size[e] + 127) & ~127u) > r->tex_total)
                    r->tex_total = (r->size[e] + 127) & ~127u;
            }
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
    }
    for (int s = 0; s < WORLD_SLOTS; s++)
        slot_of[s] = -1;
    world_stats.blocks = nblk;
    if (nblk > 1)
        mapstream_reserve(WORLD_ENEMY_RESERVE + WORLD_ARENA_LESS);
    mapstream_pool(WORLD_POOL);
    printf("world: %d blocks, resident levels per cell, lite sets up to %u KB from the near arena (%d slot%s)\n", nblk,
           slot_size >> 10, WORLD_SLOTS, WORLD_SLOTS > 1 ? "s" : "");
    for (int i = 0; i < nblk; i++)
        printf("world: block %d %s (%s) x %.0f..%.0f z %.0f..%.0f nbr %x links %u lite %u KB res %u KB%s\n", i,
               tab[i].pack, tab[i].map, tab[i].mn[0], tab[i].mx[0], tab[i].mn[1], tab[i].mx[1], tab[i].nbr, lrange[2 * i + 1],
               rt[i].total >> 10, rt[i].res_size >> 10, rt[i].open ? "" : " (no pack)");
    cur = 0;
    return 1;
}

int world_boot_map(void)
{
    BlockRt *r = &rt[0];
    if (cur < 0)
        return 0;
    /* the boot block: lite set (map tables) in a heap buffer of its own; its resident level streams per cell like every
     * block's (world_stream_open; the first frame's settle waits for the cells around the spawn). Collision and relief
     * are main.c's (collision block 0, bump_init) */
    r->blob[E_TAB] = memalign(128, (r->size[E_TAB] + 127) & ~127u);
    if (!r->blob[E_TAB] || !read_sync(&r->pk, r->off[E_TAB], r->blob[E_TAB], r->size[E_TAB]) ||
        !map_block_init(0, r->blob[E_TAB], NULL, 0)) {
        printf("world: boot block map did not load\n");
        return 0;
    }
    r->state = W_ON;
    bump_blk = world_stats.full = 0;
    world_stats.loaded = 1;
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
        if (mapstream_owns(r->texbuf))
            mapstream_free(r->texbuf, r->tex_total);
        else
            free(r->texbuf);
        r->texbuf = NULL;
        r->tex_held = 0;
        world_stats.pool_used_kb -= r->tex_total >> 10;
    }
}

/* the relief buffer (arena block, or heap when the arena had no room) */
static void bump_free(void *p, unsigned n)
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
static void bump_drop(void)
{
    if (bump_blk > 0)
        map_block_bump(bump_blk, NULL);
    if (bump_buf) {
        if (bump_old)  /* the one before is still draining: the chains of two swaps ago are long done */
            bump_free(bump_old, bump_old_n);
        bump_old = bump_buf, bump_old_n = rt[bump_blk].bump_size, bump_old_drain = WORLD_DRAIN;
    }
    bump_buf = NULL;
    bump_blk = world_stats.full = -1;
}

/* A block's lite set is read: hand its blobs to the systems (no copies, no IOP waits) */
/* first step: collision and map (the rest follows in turn_on_rest once the textures are read) */
static void turn_on_col(int i)
{
    BlockRt *r = &rt[i];
    unsigned t0 = ticks();
    int ok = col_load(i, r->blob[E_COL]), cok = ok;
    ok &= map_block_init(i, r->blob[E_TAB], NULL, i);
    if (!ok)
        printf("world: block %d: collision %s (%.4s), map %.4s\n", i, cok ? "ok" : "FAILED", (const char *)r->blob[E_COL],
               (const char *)r->blob[E_TAB]);
    if (ok)
        map_block_stream_open(i, &r->pk);
    r->state = W_ON;
    r->stage = ok ? 1 : 2;  /* a failed block gets no objects, enemies or events */
    hold_gone &= ~(1u << i);
    world_stats.loaded++;
    world_stats.loads++;
    world_stats.load_ms = (ticks() - r->t0) / 294912;
    world_stats.read_kb = r->total >> 10;
    step_time(t0);
    printf("world: block %d %s on%s: %u KB in %u ms, collision %d tris, step %u us; lite sets %u KB\n", i,
           tab[i].pack, ok ? "" : " (FAILED)", r->total >> 10, world_stats.load_ms, col_block_tris(i), (ticks() - t0) / 295,
           world_stats.pool_used_kb);
}

/* objects.bin (e = E_OBJ) or fx.bin (E_FX) is in texbuf, or the pack has none: its textures go up */
static void tex_on(int i, unsigned e)
{
    BlockRt *r = &rt[i];
    void *blob = r->size[e] ? r->texbuf : NULL;
    if (e == E_OBJ)
        objects_block_add(i, blob);
    else
        fx_block_on(i, 1, blob);
}

/* second step: the block's enemies and events, after its objects and effects */
static void turn_on_rest(int i)
{
    BlockRt *r = &rt[i];
    unsigned t0 = ticks();
    free_tex(r);  /* uploaded: back to the near cells */
    enemy_block_add(i);  /* before event_block_on: its constructors may disable these characters (A2) */
    event_block_on(i, r->size[E_EV] ? r->blob[E_EV] : NULL);
    r->stage = 2;
    step_time(t0);
    printf("world: block %d objects, enemies, events on: %u KB textures, %u ms after the read began, step %u us\n", i,
           r->tex_total >> 10, (ticks() - r->t0) / 294912, (ticks() - t0) / 295);
}

/* the texture buffer of an on block in stage 1 (one arena block, else the heap while it keeps WORLD_HEAP_FLOOR); 0 =
 * no room yet (retry_s) */
static int tex_alloc(int i)
{
    BlockRt *r = &rt[i];
    if (r->tex_held || !r->tex_total)
        return 1;
    if (r->retry_s > 0 && !warping)
        return 0;
    r->texbuf = mapstream_alloc(r->tex_total, feet_now);
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

static void turn_off(int i)
{
    BlockRt *r = &rt[i];
    unsigned t0 = ticks();
    if (r->stage == 2) {  /* stage 1: no enemies or events yet; the uploads done so far (r->ent) go below */
        event_block_off(i);  /* before the collision its hit parts point into goes */
        enemy_block_remove(i);
    }
    if (r->stage == 2 || r->ent > E_OBJ)
        objects_block_remove(i);
    if (r->stage == 2 || r->ent > E_FX)
        fx_block_on(i, 0, NULL);
    r->stage = 0;
    col_unload(i);
    map_block_drop(i);  /* its relief too (bump.c) */
    if (bump_blk == i)
        bump_drop();
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
static void read_step(int i, int issue)
{
    BlockRt *r = &rt[i];
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
            free_slot(i);
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
    if (!w_tk && (w_tk = pack_req_read(&r->pk, r->off[r->ent] + r->done, dst + r->done, n)))
        world_stats.reading = i;  /* else every request slot is taken: try next frame */
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
     * 9e6dd67). Without room in the arena an entry takes the heap while it keeps WORLD_HEAP_FLOOR. All or nothing;
     * a failed start waits WORLD_RETRY_S, since each try may drop drawn cells. */
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
        r->blob[e] = mapstream_alloc(sz, feet_now);
        /* no hole in the near arena, and the player waits for this block (a warp, or held at its edge): the heap,
         * while it keeps WORLD_HEAP_FLOOR + WORLD_HEAP_MODELS for the enemy types. At the top of the Burg ladders
         * with enemies the bonfire block's maptab.bin (592 KB) found none even after 886 KB of drawn cells went,
         * with 3.5 MB of heap free (route_enemies.txt, 2026-11-05). Not for every block: lite sets in the heap while
         * walking left the enemy types without room (70 model.bin OUT OF RAM in one run, RAM min 639 KB) */
        if (!r->blob[e] && (warping || (hold_want >> i & 1)) &&
            enemy_ram_free() >= WORLD_HEAP_FLOOR + WORLD_HEAP_MODELS + sz && (r->blob[e] = memalign(128, sz)))
            printf("world: block %d %s %u KB in the heap (RAM free %u KB)\n", i, ENTRIES[e], sz >> 10,
                   enemy_ram_free() >> 10);
        if (!r->blob[e]) {
            free_ents(r, 0, NENT);
            r->retry_s = WORLD_RETRY_S;
            return 0;  /* near cells still being read or drawn: try again */
        }
    }
    r->tex_held = 0, r->stage = 0;
    slot_of[s] = i;
    r->slot = s;
    r->ent = r->done = 0;
    while (r->ent < NENT && !r->size[r->ent])
        r->ent++;
    r->state = W_READING;
    r->t0 = ticks();
    world_stats.pool_used_kb += r->total >> 10;
    printf("world: block %d %s loading (%u KB + %u KB textures)\n", i, tab[i].pack, r->total >> 10, r->tex_total >> 10);
    return 1;
}

/* The relief of the block the player stands in: its raw bump.bin read in the background (WORLD_READ at a time, after
 * every waiting lite set) into an arena block of its own, then handed to bump.c (map_block_bump). Relief is drawing
 * detail only (DSR's normal maps): no settle waits for it, and without room it stays off for that block. */
static void bump_step(void)
{
    int res;
    if (bump_rd >= 0) {
        BlockRt *r = &rt[bump_rd];
        if (bump_reading) {
            if (!pack_req_done(w_tk, &res))
                return;
            w_tk = 0;
            unsigned n = r->bump_size - bump_done < WORLD_READ ? r->bump_size - bump_done : WORLD_READ;
            bump_reading = 0;
            world_stats.reading = -1;
            if (res != (int)n) {
                printf("world: block %d relief read %d of %u bytes\n", bump_rd, res, n);
                world_stats.fails++;
                bump_free(bump_buf, r->bump_size);
                bump_buf = NULL, bump_rd = -1;
                return;
            }
            InvalidDCache(bump_buf + bump_done, bump_buf + bump_done + n - 1);
            if ((bump_done += n) == r->bump_size) {
                if (r->state == W_ON && bump_rd == cur) {
                    map_block_bump(bump_rd, bump_buf);
                    bump_blk = world_stats.full = bump_rd;
                    world_stats.full_ms = (ticks() - bump_t0) / 294912;
                } else {  /* turned off or left while reading */
                    bump_free(bump_buf, r->bump_size);
                    bump_buf = NULL;
                }
                bump_rd = -1;
                return;
            }
        }
        if (lite_first || warping)
            return;  /* a settle waits for no relief */
        unsigned n = r->bump_size - bump_done < WORLD_READ ? r->bump_size - bump_done : WORLD_READ;
        if (!w_tk && (w_tk = pack_req_read(&r->pk, r->bump_off + bump_done, bump_buf + bump_done, n)))
            bump_reading = 1, world_stats.reading = bump_rd;
        return;
    }
    if (cur == bump_blk || in_cur_s < WORLD_SWAP_S || warping || rt[cur].state != W_ON || lite_first)
        return;
    if (bump_blk >= 0)
        bump_drop();
    bump_blk = cur;  /* also for a block without relief or room: no retry until the player changes block */
    BlockRt *r = &rt[cur];
    unsigned sz = (r->bump_size + 127) & ~127u;
    if (!r->bump_size)
        return;
    bump_buf = mapstream_alloc(sz, feet_now);
    if (!bump_buf && enemy_ram_free() >= WORLD_HEAP_FLOOR + sz)
        bump_buf = memalign(128, sz);
    if (!bump_buf) {
        printf("world: block %d relief: no room for %u KB\n", cur, sz >> 10);
        return;
    }
    bump_blk = -1, bump_rd = cur, bump_done = 0, bump_t0 = ticks();
    world_stats.pool_used_kb += sz >> 10;
}

/* a wanted block's lite set is being read or could start now (start()'s conditions, room aside) */
static int lite_pending(void)
{
    int free_s = 0;
    for (int s = 0; s < WORLD_SLOTS; s++)
        free_s |= slot_of[s] < 0;
    for (int i = 1; i < nblk; i++)
        if (rt[i].state == W_READING || (rt[i].state == W_ON && rt[i].stage == 1) ||
            (free_s && rt[i].open && rt[i].state == W_OFF && (want_m >> i & 1) && (rt[i].retry_s <= 0 || warping)))
            return 1;
    return 0;
}

/* A wanted block's lite set found no room in the near arena (or no free slot): turns off the farthest block that
 * is on only because the triangle under the player keeps it (not wanted, not underfoot). From cdrom0: the
 * Parish block (1072 KB) found no room at the courtyard steps while the Burg west block stayed kept from the bonfire
 * block with the bonfire block full, never loaded, and the player walked off the loaded floor (2026-11-05, TECH_DEBT
 * world-8). 0 if no block could go. */
static int slots_full(void)
{
    for (int s = 0; s < WORLD_SLOTS; s++)
        if (slot_of[s] < 0)
            return 0;
    return 1;
}

static int room_for(int want, const float *feet)
{
    int b = -1;
    float bd = -1;
    for (int i = 1; i < nblk; i++)
        if (i != want && i != cur && rt[i].state == W_ON && !(want_m >> i & 1) && world_stats.reading != i) {
            float d = box_d(&tab[i], feet);
            if (d > bd)
                b = i, bd = d;
        }
    if (b < 0)
        return 0;
    printf("world: block %d off to make room for block %d\n", b, want);
    turn_off(b);
    return 1;
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
    room_stuck = 0;
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
    if (bump_old && (--bump_old_drain <= 0 || warping)) {
        bump_free(bump_old, bump_old_n);
        bump_old = NULL;
    }
    /* one background read at a time. Wanted blocks' lite sets go first: they hold the collision the player walks onto
     * next, while the resident cells (mapstream.c) and the relief only add drawing detail. With a full swap first, from
     * cdrom0: a Burg swap (4.9 MB, ~5 s at ~1 MB/s) kept the next block's collision from loading and the player walked
     * off the loaded floor (2026-11-04, TECH_DEBT world-8). A relief chunk in flight is collected first; while a lite
     * set waits neither the relief nor mapstream.c issue a new read. */
    lite_first = lite_pending();
    mapstream_hold(lite_first);
    mapstream_focus(cur);
    if (bump_reading) {
        bump_step();
        if (bump_reading)
            goto unload;
    }
    if (world_stats.reading > 0 && !bump_reading && rt[world_stats.reading].state == W_ON) {
        read_step(world_stats.reading, 0);  /* a texture chunk in flight is collected; the next one waits its turn */
        goto unload;
    }
    for (int i = 0; i < nblk; i++)
        if (rt[i].state == W_READING) {
            if (world_stats.reading < 0 || world_stats.reading == i)
                read_step(i, 1);
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
        if (want >= 0 && start(want)) {
            read_step(want, 1);
            goto unload;
        }
        if (want >= 0 && (rt[want].retry_s >= WORLD_RETRY_S || warping || slots_full()))
            room_stuck = !room_for(want, feet);  /* a fresh failure for room, or no slot */
        else if (world_stats.reading < 0)
            bump_step();  /* nothing for the lite sets: the relief of the block underfoot */
    }
    for (int i = 1; i < nblk; i++)  /* then the textures, enemies and events of blocks whose collision is on */
        if (rt[i].state == W_ON && rt[i].stage == 1 && world_stats.reading < 0) {
            read_step(i, 1);
            if (world_stats.reading == i || rt[i].stage == 2)
                break;
            room_stuck |= !rt[i].tex_held;  /* no room for its texture buffer (settle stops on it) */
        }
unload:
    /* unload blocks the triangle under the player has not kept for WORLD_KEEP_S (never the boot block or the one under
     * the player) */
    for (int i = 1; i < nblk; i++)
        if (i != cur && rt[i].state == W_ON && !(keep_m >> i & 1) && world_stats.reading != i &&
            (unkept_s[i] >= WORLD_KEEP_S || warping))
            turn_off(i);
}

/* settled: nothing reading, every wanted neighbour on, the resident cells around the feet in (mapstream_busy) */
static int settled(const float *feet)
{
    if (world_stats.reading >= 0 || mapstream_busy(feet, WORLD_SETTLE_R, WORLD_SETTLE_FOCUS_R))
        return 0;
    for (int i = 1; i < nblk; i++)
        if (rt[i].state == W_READING || (rt[i].state == W_ON && rt[i].stage == 1) ||
            (rt[i].open && rt[i].state == W_OFF && (want_m >> i & 1)))
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
    mapstream_settle(1);
    int n = 0;
    unsigned stuck_t = 0, el_ms = 0, last = t0, frac = 0;
    /* elapsed ms summed per step: COP0 Count wraps every 14.6 s, so (ticks() - t0) never reached the 30 s cap */
    while (!settled(feet) && el_ms < 30000) {
        unsigned now = ticks(), d = now - last + frac;
        last = now, el_ms += d / 294912, frac = d % 294912;
        if (!room_stuck)
            stuck_t = 0;
        else if (!stuck_t)
            stuck_t = ticks();
        else if ((ticks() - stuck_t) / 294912 >= WORLD_STUCK_MS) {
            /* no room for a wanted block and nothing to turn off: mapstream would read and drop the same cells
             * until the 30 s cap (the frame never advances) */
            printf("world: settle stopped: no room for the wanted blocks %x\n", want_m);
            break;
        }
        mapstream_update(feet);  /* collects the cell reads that landed, queues the next ones */
        step(feet, 0);
        loading_tick(1);
        n++;
    }
    warping = 0;
    mapstream_settle(0);
    in_cur_s = WORLD_SWAP_S;
    event_flush();  /* the new blocks' constructors finish behind the loading screen (A2 event-14) */
    loading_end();
    {
        unsigned d = ticks() - last + frac;
        el_ms += d / 294912;
    }
    printf("world: warp to %.1f %.1f %.1f settled in %u ms (%d steps): block %d, %d loaded; resident %u KB, mip 0 %u KB\n",
           feet[0], feet[1], feet[2], el_ms, n, cur, world_stats.loaded, mapstream_stats.res_kb,
           mapstream_stats.tex_kb);
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
