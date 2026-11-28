/* World streaming (world.h): the block table, boot, the warp settle and the API. The block lifecycle is split over
 * world.c, wblock.c, wextra.c and wplan.c (Polaris 4.1, code moved as it was); world_i.h has the design notes and the
 * shared state. */
#include "mapfile.h"
#include "world_i.h"
#include "lua0.h"
#include "nav.h"
extern unsigned lua0_malloc_live;  /* lua0.c: the VM's malloc fallback bytes (world_systems' type split) */
WorldStats world_stats = {.reading = -1, .full = -1};
const WorldBlock *w_tab;
const unsigned *w_rrange;  /* WORLD_REACH: per block first reach run, runs (NULL: no reach table) */
const unsigned short *w_rfirst;  /* per run its first triangle; rent: its entry (1-based, 0 none) */
const unsigned char *w_rent;
const WorldReachL *w_reach_tab;
unsigned w_reach_n;
float w_reach_step;
float w_reach_d[WORLD_MAX], w_reach_v[WORLD_MAX];  /* m to each block's seam from the floor; last change's sign */
float w_on_est[WORLD_MAX];  /* ms a block's lite set takes to turn on (see WORLD_REACH) */
BlockRt w_rt[WORLD_MAX];
int w_nblk, w_cur = -1, w_slot_of[WORLD_SLOTS];
static unsigned slot_size;
static float last_feet[3];
static float tile_gone[3];
static int tile_gone_on;
static float hold_s;

/* boot-time read of a raw entry range into dst (waits) */
int w_read_sync(Pack *pk, unsigned off, void *dst, unsigned n)
{
    int r = 0, t;
    while (!(t = pack_req_read(pk, off, dst, n)))
        pack_pump();
    while (!pack_req_done(t, &r))
        ;
    InvalidDCache(dst, (char *)dst + n - 1);
    return r == (int)n;
}

/* WORLD_TAB_SPLIT: E_TAB / E_TABB / E_TABC from maptab.bin's header (map.c Header: chunk_off, tex_off, cell_off at
 * bytes 36 / 40 / 76; tab offset of a map.bin offset X = X - chunk_off + 80). A piece starts on a 128-byte boundary
 * at or before its section (the few bytes before it are read twice), so the sections keep their alignment. Leaves
 * maptab.bin whole when the cuts do not make sense. 0 if the header could not be read. */
static int tab_split(BlockRt *r)
{
    static unsigned hd[32] __attribute__((aligned(64)));
    unsigned full = r->size[E_TAB];
    r->total -= 2 * ((full + 127) & ~127u);  /* the loop counted E_TABB / E_TABC as the whole entry too */
    if (full < 128 || !w_read_sync(&r->pk, r->off[E_TAB], hd, 128))
        return 0;
    if (memcmp(hd, "DSM5", 4))
        return 1;
    unsigned chunk_off = hd[9], tex = hd[10] - chunk_off + 80, cell = hd[19] - chunk_off + 80;
    unsigned b0 = tex & ~127u, c0 = cell & ~127u;
    if (!(tex > 80 && b0 >= 128 && cell > tex && c0 > b0 && cell < full))
        return 1;
    r->total -= (full + 127) & ~127u;
    r->off[E_TABB] = r->off[E_TAB] + b0, r->size[E_TABB] = cell - b0;
    r->off[E_TABC] = r->off[E_TAB] + c0, r->size[E_TABC] = full - c0;
    r->size[E_TAB] = tex, r->cut[0] = b0, r->cut[1] = c0;
    for (int e = E_TAB; e <= E_TABC; e++)
        r->total += (r->size[e] + 127) & ~127u;
    return 1;
}

/* objects.bin (tools/export_objects.py OBJ_TEX_SHARED): the breakable texture model is the file's last chunk (BTEX,
 * its payload offset in OHDR), and the types were loaded at boot (world_systems), so turning a block on reads only that
 * tail (Burg blocks: 272 KB instead of 0.75-1.2 MB of models); objects_block_add gets the tail. A file without BTEX
 * stays whole. */
/* WORLD_FX_LITE: bytes of block r's fx.bin before its first pixel / CLUT (its header and texture table read here,
 * once per block at boot); 0: unreadable, the block's effects then load at boot (world_systems) */
static unsigned fx_head_size(BlockRt *r, unsigned off, unsigned size)
{
    static unsigned hd[16] __attribute__((aligned(64)));
    if (size < 64 || !w_read_sync(&r->pk, off, hd, 64))
        return 0;
    unsigned n_tex = hd[2], off_tex = hd[8], n = (off_tex + n_tex * 16 + 63) & ~63u;  /* (FxHeader: n_tex, off_tex) */
    if (!n_tex || off_tex > size || n > size || n > 64 * 1024)
        return n_tex ? 0 : size;  /* (no textures: the whole file is tables) */
    unsigned char *b = memalign(64, n);
    /* (the header and texture table lie in the n bytes read; the file's size bounds the pixel offsets) */
    unsigned keep = b && w_read_sync(&r->pk, off, b, n) ? fx_head_bytes(b, size) : 0;
    free(b);
    return keep;
}

static void obj_tail(BlockRt *r)
{
    static unsigned hd[32] __attribute__((aligned(64)));
    ChunkFile f;
    const unsigned *oh = NULL;  /* objects.bin: chunk file 'OBJS'; OHDR word 6 = BTEX's payload offset (0: none) */
    if (w_read_sync(&r->pk, r->off[E_OBJ], hd, 64) &&
        chunk_open_part(&f, hd, 64, r->size[E_OBJ], CHUNK_ID('O', 'B', 'J', 'S'), "objects.bin"))
        oh = chunk_get(&f, CHUNK_ID('O', 'H', 'D', 'R'), 2, NULL);
    unsigned at = oh ? oh[6] : 0;
    if (!at || at & 127 || at >= r->size[E_OBJ] || !w_read_sync(&r->pk, r->off[E_OBJ] + at - 64, hd + 16, 64))
        return;
    const unsigned *ch = hd + 16 + 12;  /* BTEX's chunk header: the 16 bytes before its payload */
    if (ch[0] != CHUNK_ID('B', 'T', 'E', 'X') || (ch[1] & 0xFFFF) != 1 || ch[2] > r->size[E_OBJ] - at)
        return;
    /* WORLD_OBJ_LITE: the head (OHDR .. ATEX, up to MODL's chunk header; OHDR word 7 = MODL's payload offset) is the
     * block's E_OBJH; its types are made from it at turn-on (objects_block_types_lite), with this tail as BTEX. Its
     * breakable models stream by their file offset, so OBJ_STREAM must be on */
    if (WORLD_OBJ_LITE && OBJ_STREAM && oh[7] > 16 && oh[7] - 16 <= at) {
        r->off[E_OBJH] = r->off[E_OBJ], r->size[E_OBJH] = oh[7] - 16, r->obj_full = r->size[E_OBJ];
        if (!WORLD_HEAD_TEX)  /* (else it comes in the texture buffer: world_init's tex_total) */
            r->total += (r->size[E_OBJH] + 127) & ~127u;  /* (before world_struct_bytes sums the lite sets) */
    }
    r->off[E_OBJ] += at, r->size[E_OBJ] = ch[2];
}

/* COL_TILES: a paged collision.bin (tools/export_collision.py COL_TILE) reads its index with the lite set and its pages
 * through src/coltile.c; the page table goes to coltile_plan now (the pool's size). An unpaged file stays whole. 0 if
 * the file could not be read. */
static int col_paged(int i, BlockRt *r)
{
    static unsigned hd[48] __attribute__((aligned(64)));
    if (r->size[E_COL] < 192 || !w_read_sync(&r->pk, r->off[E_COL], hd, 128))
        return 0;
    if (memcmp(hd, "DCOL", 4) || hd[1] != 2)
        return 1;
    unsigned at = 0, a0 = 0;
    const unsigned *t = NULL;
    for (int v = 2; v >= 1 && !t; v--) {  /* the page table: CTP2 after the patterns, CTP1 after gid (collision.c) */
        at = col_ptab_at(hd, v), a0 = at & ~63u;
        if (at + 16 > r->size[E_COL] || !w_read_sync(&r->pk, r->off[E_COL] + a0, hd + 32, 64))
            return 0;
        t = (const unsigned *)((const unsigned char *)(hd + 32) + (at - a0));
        t = !memcmp(t, v == 2 ? "CTP2" : "CTP1", 4) ? t : NULL;
    }
    if (!t)
        return 1;
    unsigned npg = t[1], lo = t[2], n = 16 + 32 * npg, n0 = (at - a0 + n + 63) & ~63u;
    unsigned char *pt = memalign(64, n0);
    float os[4];
    memcpy(os, hd + 4, 16);  /* origin[3], scale */
    int ok = pt && lo <= r->size[E_COL] && w_read_sync(&r->pk, r->off[E_COL] + a0, pt, n0) && coltile_plan(i, os, pt + (at - a0));
    free(pt);
    if (ok) {
        r->total -= ((r->size[E_COL] + 127) & ~127u) - ((lo + 127) & ~127u);
        r->col_full = r->size[E_COL], r->size[E_COL] = lo;
    }
    return ok;
}

/* WORLD_EV_SHARED: split block r's events.bin at boot (its header, kept for event.c) */
static void ev_split(BlockRt *r)
{
    unsigned full = r->size[E_EV], f128 = (full + 127) & ~127u;
    r->total -= f128;  /* world_init's loop counted the second events.bin entry (E_EVM) whole */
    if (full < 96 || !w_read_sync(&r->pk, r->off[E_EV], r->evhdr, 128) ||
        memcmp(r->evhdr, "EVB1", 4) || r->evhdr[1] != 3)
        return;
    unsigned m0 = r->evhdr[4 + 2 * 3], m1 = r->evhdr[4 + 2 * 6];  /* sections: entities .. colliders, then treasures */
    if (!(96 <= m0 && m0 <= m1 && m1 <= full))
        return;
    r->total -= f128;
    r->ev_split = 1, r->ev_mid = m0, r->ev_full = full;
    r->off[E_EVM] = r->off[E_EV];
    r->off[E_EV] += m0, r->size[E_EV] = m1 - m0;
    r->total += (r->size[E_EV] + 127) & ~127u;
}

int world_init(const char *boot_area)
{
    Pack pk;
    unsigned wsize = 0, nb = 0, nr = 0, nl = 0, nn = 0, nreach = 0;
    const unsigned *rh = NULL;
    void *wb = pack_open(&pk, PACK_ROOT "world.pak") ? pack_load(&pk, "world.bin", &wsize) : NULL;
    pack_close(&pk);
    /* world.bin: a chunk file (tools/export_world.py, src/chunk.h), read in place. The links and reach are the
     * block-local chunks (NBRS v1: up to 15 neighbours, u16 masks); a missing one or another version is refused */
    ChunkFile cf;
    if (chunk_open(&cf, wb, wsize, CHUNK_ID('W', 'R', 'L', 'D'), "world.bin")) {
        w_tab = chunk_get(&cf, CHUNK_ID('B', 'L', 'K', 'S'), 2, &nb);
        w_nbrs = w_tab ? chunk_get(&cf, CHUNK_ID('N', 'B', 'R', 'S'), 1, &nn) : NULL;
        w_lrange = w_nbrs ? chunk_get(&cf, CHUNK_ID('L', 'R', 'N', 'L'), 1, &nr) : NULL;
        w_links = w_lrange ? chunk_get(&cf, CHUNK_ID('L', 'N', 'K', 'L'), 1, &nl) : NULL;
        rh = w_links ? chunk_get(&cf, CHUNK_ID('R', 'C', 'H', 'L'), 1, &nreach) : NULL;
        nb /= sizeof *w_tab, nl /= sizeof *w_links;
    }
    if (!w_tab || !w_nbrs || !w_lrange || !w_links || !rh || !nb || nr != 8 * nb || nn != sizeof *w_nbrs * nb ||
        strncmp(w_tab[0].pack, boot_area, 16)) {
        free(wb);
        w_tab = NULL;
        printf("world: no world table for %s, single area\n", boot_area);
        return 0;
    }
    for (unsigned i = 0; i < nb; i++) {
        const WorldNbrs *n = &w_nbrs[i];
        int bad = n->count > sizeof n->nbr || n->count != w_tab[i].nbr_n;
        for (unsigned k = 0; k < n->count && !bad; k++)
            bad = n->nbr[k] >= nb || n->nbr[k] == i || (k && n->nbr[k] <= n->nbr[k - 1]);
        if (bad || w_lrange[2 * i] > nl || w_lrange[2 * i + 1] > nl - w_lrange[2 * i]) {
            if (bad)
                printf("TEST error: chunk: world.bin: block %u's NBRS (%u of %u neighbours) is not an ascending list of "
                       "other blocks\n", i, n->count, w_tab[i].nbr_n);
            else
                printf("TEST error: chunk: world.bin: block %u's links %u + %u past the %u links\n", i, w_lrange[2 * i],
                       w_lrange[2 * i + 1], nl);
            free(wb);
            w_tab = NULL;
            return 0;
        }
    }
    w_nblk = nb < WORLD_MAX ? nb : WORLD_MAX;
    if (w_nblk > MAP_INST || w_nblk > COL_BLOCKS)
        w_nblk = MAP_INST < COL_BLOCKS ? MAP_INST : COL_BLOCKS;  /* map instance / collision block = block index */
    if (nreach >= 12 + 8 * nb) {  /* the reach table (WORLD_REACH; no entries: the links' masks alone) */
        float st;
        memcpy(&st, rh + 2, 4);
        w_rrange = rh + 3;
        w_rfirst = (const unsigned short *)(w_rrange + 2 * nb);
        w_rent = (const unsigned char *)(w_rfirst + rh[0]);
        w_reach_tab = (const WorldReachL *)((const unsigned char *)w_rfirst + ((3 * rh[0] + 3) & ~3u));
        w_reach_n = rh[1], w_reach_step = st;
        if (!w_reach_n || (const unsigned char *)(w_reach_tab + w_reach_n) > (const unsigned char *)rh + nreach)
            w_rrange = NULL;
    }
    for (int i = 0; i < WORLD_MAX; i++)
        w_on_est[i] = WORLD_ON_SEED_MS, w_reach_d[i] = -1;
    /* open every block's pack now (boot time: a pack_open waits for the IOP) and size the pools */
    for (int i = 0; i < w_nblk; i++) {
        BlockRt *r = &w_rt[i];
        char path[64];
        snprintf(path, sizeof path, PACK_ROOT "%s.pak", w_tab[i].pack);
        r->slot = -1;
        if (!(r->open = pack_open(&r->pk, path))) {
            printf("world: block %d %s: no pack\n", i, path);
            if (!i)
                return 0;
            continue;
        }
        /* map.bin's split files are chunk files: their payload is the base (src/mapfile.h). maptab.bin and cells.bin
         * (the VIF packets: VU1L) are checked here; mapres.bin and maptex.bin come from the same pack step, so they are
         * wrapped alike (each header read is a seek: four per block cost 0.73 s of a 16-block boot on the ISO) */
        if (!mapf_check(&r->pk, "maptab.bin", MAPF_TAB) || !mapf_check(&r->pk, "cells.bin", MAPF_CELLS))
            r->open = 0;
        for (unsigned e = 0; e < NENT; e++) {
            if (e == E_FXH || e == E_OBJH) {  /* (share fx.bin / objects.bin with E_FX / E_OBJ: sized apart) */
                r->off[e] = r->size[e] = 0;
                continue;
            }
            int got = e >= E_TAB && e <= E_TABC ? mapf_entry(&r->pk, ENTRIES[e], MAPF_TAB, &r->off[e], &r->size[e])
                : e == E_MTX ? mapf_entry(&r->pk, ENTRIES[e], MAPF_TEX, &r->off[e], &r->size[e])
                : pack_entry_raw(&r->pk, ENTRIES[e], &r->off[e], &r->size[e]);
            if (!got)
                r->off[e] = r->size[e] = 0;
            if ((e == E_DG || e == E_PVS) && (!WORLD_DG_LITE || !i))  /* (the boot block's are main.c's) */
                r->off[e] = r->size[e] = 0;
            if (e == E_ENM && ((!WORLD_NAV_LITE && !WORLD_CHR_LITE) || !i))  /* (the boot block's: enemy_init) */
                r->off[e] = r->size[e] = 0;
            if (e == E_AI && (!WORLD_AI_UNLOAD || !WORLD_CHR_LITE || !i))  /* (the boot block's: enemy_init, for the run;
                                                                             * without CHR_LITE: enemy_block_types at boot) */
                r->off[e] = r->size[e] = 0;
            if (!r->size[e] && !E_OPTIONAL(e) && (i || e == E_TAB)) {
                printf("world: block %d %s: %s missing or not raw\n", i, w_tab[i].pack, ENTRIES[e]);
                r->open = 0;
            }
            if (i && e == E_OBJ && r->size[e] >= 64)
                r->obj0 = r->off[e], obj_tail(r);
            if (i && e >= E_TEX) {  /* the boot block's collision, objects and effects are main.c's */
                unsigned t = (r->size[e] + 127) & ~127u;
                if (WORLD_HEAD_TEX && e == E_OBJ)  /* (obj_tail sized its head: both in at once, tex_dst) */
                    t += (r->size[E_OBJH] + 127) & ~127u;
                if (t > r->tex_total)
                    r->tex_total = t;
            }
            else if (i || e == E_TAB)
                r->total += (r->size[e] + 127) & ~127u;
        }
        r->size[E_TABB] = r->size[E_TABC] = 0;  /* (the loop above gave them the whole maptab.bin) */
        r->off[E_FXH] = r->size[E_FXH] = 0;  /* WORLD_FX_LITE: fx.bin's tables (before world_struct_bytes sums total) */
        if (WORLD_FX_LITE && i && r->open && r->size[E_FX] &&
            (r->size[E_FXH] = fx_head_size(r, r->off[E_FX], r->size[E_FX])))
            r->off[E_FXH] = r->off[E_FX], r->total += (r->size[E_FXH] + 127) & ~127u;
        if (i && r->open && !tab_split(r)) {
            printf("world: block %d %s: maptab.bin header unreadable\n", i, w_tab[i].pack);
            r->open = 0;
        }
        if (!i && r->open && WORLD_BOOT_LEAN) {  /* the boot block's tables in pieces too (world_boot_map) */
            r->total += 2 * ((r->size[E_TAB] + 127) & ~127u);  /* (tab_split takes the loop's three counts back) */
            if (!tab_split(r))
                r->total = (r->size[E_TAB] + 127) & ~127u;
        }
        if (!pack_entry_raw(&r->pk, "far.bin", &r->far_off, &r->far_size) || r->far_size <= 16)
            r->far_off = r->far_size = 0;  /* (16 bytes: an empty panorama, FARPANO=0) */
        if (i && r->open && COL_TILES && !col_paged(i, r))
            r->open = 0;
        r->off[E_EVM] = r->size[E_EVM] = 0;  /* (the loop above read it as a second events.bin: ev_split) */
        if (i && r->open && (r->mtx_full = r->size[E_MTX]))  /* counted once per map (world_struct_bytes) */
            r->total -= (r->mtx_full + 127) & ~127u;
        if (i && r->open)
            ev_split(r);
        if (!mapf_entry(&r->pk, "mapres.bin", MAPF_RES, &r->res_off, &r->res_size)) {
            printf("world: block %d %s: mapres.bin missing or not raw\n", i, w_tab[i].pack);
            r->open = 0;
        }
        /* a header-only bump.bin (48 bytes: no relief, e.g. BUMP_BAKE exports) is no relief: never read */
        if ((!i && !WORLD_BOOT_RELIEF) || !pack_entry_raw(&r->pk, "bump.bin", &r->bump_off, &r->bump_size) ||
            r->bump_size <= 48)
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
        w_slot_of[s] = -1;
    world_stats.blocks = w_nblk;
    if (w_nblk > 1) {
        unsigned lua = world_lua_bytes();  /* WORLD_AI_UNLOAD: 0 without E_AI entries */
        mapstream_reserve(WORLD_ENEMY_RESERVE + WORLD_ARENA_LESS + lua);
        if (lua)
            printf("world: Lua reserve %u KB (the most a link keeps)\n", lua >> 10);
    }
    mapstream_pool(mapstream_pools_on_build() ? WORLD_ARENA_KB << 10 : WORLD_POOL);
    if (w_nblk > 1) {
        /* MS_POOL_TILE: the tiles' region is whole pages at the structural pool's bottom, the lite sets' room above it */
        unsigned cb = mapstream_tile_pool(coltile_budget()), sb = world_struct_bytes() + cb, mb, tmax = 0;
        for (int i = 0; i < w_nblk; i++)
            w_pool_cb[i] = coltile_budget_block(i);
        for (int i = 0; i < w_nblk; i++)
            tmax = w_rt[i].tex_total > tmax ? w_rt[i].tex_total : tmax;
        mb = tmax + WORLD_MISC_EXTRA;  /* (the relief stays out of the pools) */
        mapstream_pools(sb, mb);
        printf("world: structural pool %u KB (the most one triangle's links keep on, collision tiles %u KB), misc pool "
               "%u KB (textures %u + %u KB)\n", sb >> 10, cb >> 10, mb >> 10, tmax >> 10, WORLD_MISC_EXTRA >> 10);
    }
    printf("world: %d blocks, resident levels per cell, lite sets up to %u KB from the near arena (%d slot%s)\n", w_nblk,
           slot_size >> 10, WORLD_SLOTS, WORLD_SLOTS > 1 ? "s" : "");
    for (int i = 0; i < w_nblk; i++)
        printf("world: block %d %s (%s) x %.0f..%.0f z %.0f..%.0f nbr %u links %u lite %u KB res %u KB%s\n", i,
               w_tab[i].pack, w_tab[i].map, w_tab[i].mn[0], w_tab[i].mx[0], w_tab[i].mn[1], w_tab[i].mx[1], w_tab[i].nbr_n, w_lrange[2 * i + 1],
               w_rt[i].total >> 10, w_rt[i].res_size >> 10, w_rt[i].open ? "" : " (no pack)");
    w_cur = 0;
    return 1;
}

int world_boot_map(void)
{
    BlockRt *r = &w_rt[0];
    if (w_cur < 0)
        return 0;
    /* the boot block: lite set (map tables) in a heap buffer of its own; its resident level streams per cell like every
     * block's (world_stream_open; the first frame's settle waits for the cells around the spawn). Collision and relief
     * are main.c's (collision block 0, bump_init) */
    int ok = 1;
    for (int e = E_TAB; e <= E_TABC && ok; e++)  /* one piece, or three (WORLD_BOOT_LEAN: tab_split at world_init) */
        if (r->size[e])
            ok = (r->blob[e] = memalign(128, (r->size[e] + 127) & ~127u)) && w_read_sync(&r->pk, r->off[e], r->blob[e], r->size[e]);
    if (ok && r->size[E_TABB])
        map_block_split(0, r->blob[E_TABB], r->cut[0], r->blob[E_TABC], r->cut[1]);
    if (!ok || !map_block_init(0, r->blob[E_TAB], NULL, 0)) {
        printf("world: boot block map did not load\n");
        return 0;
    }
    r->state = W_ON;
    w_bump_blk = world_stats.full = WORLD_BOOT_RELIEF ? -1 : 0;  /* (streamed: read once the player stood in it) */
    world_stats.loaded = 1;
    return 1;
}

void world_stream_open(void)
{
    if (w_cur >= 0)  /* instance 0's near cells from the boot pack (opened at world_init, stays open) */
        map_block_stream_open(0, &w_rt[0].pk);
}

int world_current(void) { return w_cur; }
int world_lite_busy(void) { return w_lite_first && !w_warping; }

unsigned world_block_res(int k)
{
    if (w_cur < 0 || k < 0 || k >= w_nblk)
        return 0;
    const BlockRt *r = &w_rt[k];
    unsigned m = w_far_cur ? WB_FAR : 0;
    if (r->state != W_ON)
        return m;
    m |= WB_ON;
    if (!r->lean)
        m |= WB_TABLES | WB_HORIZON;
    else if (k > 0 && LEAN_E1 != E_TABC)
        m |= WB_HORIZON;  /* a kept-only block keeps its cell table and horizon (lean: let go, or being read back) */
    return m;
}

const char *world_map(int block) { return w_tab && block >= 0 && block < w_nblk ? w_tab[block].map : NULL; }

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
    if (!p || !w_read_sync(&r->pk, off, p, *size)) {
        printf("world: %s NOT READ\n", name);
        free(p);
        return NULL;
    }
    return p;
}

int world_obj_read(int block, unsigned rel, void *dst, unsigned bytes)
{
    if (block < 1 || block >= w_nblk || !w_rt[block].open || !w_rt[block].obj0)
        return 0;
    return pack_req_read(&w_rt[block].pk, w_rt[block].obj0 + rel, dst, bytes);
}

void world_systems(GSGLOBAL *gs)
{
    for (int i = 1; i < w_nblk && w_cur >= 0; i++) {
        BlockRt *r = &w_rt[i];
        unsigned n0, n1;
        if (!r->open)
            continue;
        unsigned h0 = enemy_ram_free();
        unsigned char *p = r->size[E_OBJH] ? NULL : read_tmp(r, "objects.bin", &n0);  /* (WORLD_OBJ_LITE: turn-on) */
        int no = p ? objects_block_types(gs, i, p, n0) : -1;
        free(p);
        unsigned h1 = enemy_ram_free();
        p = r->size[E_FXH] ? NULL : read_tmp(r, "fx.bin", &n0);  /* (WORLD_FX_LITE: with the lite set) */
        int nf = p ? fx_block_load(gs, i, p, n0) : -1;
        free(p);
        unsigned h2 = enemy_ram_free();
        p = read_tmp(r, "enemies.bin", &n0);
        unsigned char *lua = r->size[E_AI] ? NULL : read_tmp(r, "ai.lua0", &n1);
        /* (TECH_DEBT world-46 split: the Lua VM's heap and its malloc spill, the navmesh copy; the rest of the enemies
         * figure is the placements and the type heads) */
        unsigned lh0 = lua0_heap, lm0 = lua0_malloc_live, nb0 = nav_bytes();
        if (r->size[E_ENM] && WORLD_NAV_LITE)
            enemy_nav_with_block(i);  /* WORLD_NAV_LITE: its navmesh comes with its lite set (nav_bind) */
        int ne = -1;
        if (WORLD_CHR_LITE && r->size[E_ENM]) {  /* its types at turn-on; its map logic here (or E_AI: at turn-on) */
            if (lua && !r->size[E_AI])
                enemy_block_lua(lua, (int)n1);
        } else if (p) {
            ne = enemy_block_types(gs, i, w_tab[i].pack, p, n0, lua, lua ? (int)n1 : 0);
        }
        unsigned lh1 = lua0_heap, lm1 = lua0_malloc_live, nb1 = nav_bytes();
        free(lua);
        free(p);
        unsigned h3 = enemy_ram_free();
        if (!r->size[E_DG] || !r->size[E_PVS])  /* WORLD_DG_LITE 0, or not raw in the pack: from boot, kept */
            map_block_dg(i, &r->pk);  /* its draw groups (map.c, src/drawgrp.c) */
        printf("world: block %d types: objects %d, effects %d, enemies %d; heap KB objects %d fx %d enemies %d dg %d; "
               "enemies split KB: lua vm %d lua malloc %d nav %d lua0 file %u\n",
               i, no, nf, ne, ((int)h0 - (int)h1) >> 10, ((int)h1 - (int)h2) >> 10, ((int)h2 - (int)h3) >> 10,
               ((int)h3 - (int)enemy_ram_free()) >> 10, ((int)lh1 - (int)lh0) >> 10, ((int)lm1 - (int)lm0) >> 10,
               ((int)nb1 - (int)nb0) >> 10, lua ? n1 >> 10 : 0);
    }
    if (WORLD_FAR_RESERVE && w_nblk > 1) {  /* the panorama reserve, before the arena takes the heap */
        for (int i = 0; i < w_nblk; i++)
            w_far_res_size = w_rt[i].far_size > w_far_res_size ? w_rt[i].far_size : w_far_res_size;
        w_far_res_size = (w_far_res_size + 127) & ~127u;
        w_far_res = w_far_res_size ? memalign(128, w_far_res_size) : NULL;
        printf("world: far panorama reserve %u KB%s\n", w_far_res_size >> 10, w_far_res || !w_far_res_size ? "" : " FAILED");
    }
    if (w_cur >= 0 && w_nblk > 1)
        mapstream_arena();  /* now: no enemy model is loaded yet (enemy_init stripped them all) */
}

/* settled: nothing reading, every wanted neighbour on, the resident cells around the feet in (mapstream_busy) */
static int settled(const float *feet)
{
    if (world_stats.reading >= 0 || mapstream_busy(feet, WORLD_SETTLE_R, WORLD_SETTLE_FOCUS_R) ||
        coltile_missing(feet, WORLD_SETTLE_COL_R))
        return 0;
    if ((w_rt[0].lean && w_boot_want && w_rt[0].retry_s <= 0) || w_far_rd >= 0 ||
        (w_cur >= 0 && w_cur != w_far_blk && w_rt[w_cur].far_size && w_far_retry_s <= 0))
        return 0;  /* the boot block's tables, the far panorama of the block warped into */
    for (int i = 1; i < w_nblk; i++)
        if (w_rt[i].state == W_READING || (w_rt[i].state == W_ON && w_rt[i].stage == 1) ||
            (w_rt[i].open && w_rt[i].state == W_OFF && (w_want_m >> i & 1) && !w_slot_deferred(i, feet) && !w_src_only(i)) ||
            (w_rt[i].state == W_ON && w_rt[i].lean && (w_want_m >> i & 1 || i == w_cur)))
            return 0;
    return 1;
}

int world_settled(const float *feet) { return w_cur < 0 || settled(feet); }

/* A warp: finish every load / unload / swap the new position needs now (the frame waits, as DSR's loading screen) */
static void settle(const float *feet)
{
    unsigned t0 = ticks();
    ot_wait();  /* the last frame's VIF chain no longer reads the pools */
    loading_begin();  /* DSR's loading screen while the frame waits (src/loading.c) */
    w_warping = 1;
    mapstream_settle(1);
    int n = 0;
    unsigned stuck_t = 0, el_ms = 0, last = t0, frac = 0;
    /* elapsed ms summed per step: COP0 Count wraps every 14.6 s, so (ticks() - t0) never reached the 30 s cap */
    while (!settled(feet) && el_ms < 30000) {
        unsigned now = ticks(), d = now - last + frac;
        last = now, el_ms += d / 294912, frac = d % 294912;
        if (!w_room_stuck)
            stuck_t = 0;
        else if (!stuck_t)
            stuck_t = ticks();
        else if ((ticks() - stuck_t) / 294912 >= WORLD_STUCK_MS) {
            /* no room for a wanted block and nothing to turn off: mapstream would read and drop the same cells
             * until the 30 s cap (the frame never advances) */
            printf("world: settle stopped: no room for the wanted blocks %llx\n", w_want_m);
            break;
        }
        mapstream_update(feet);  /* collects the cell reads that landed, queues the next ones */
        w_step(feet, 0);
        loading_tick(1);
        n++;
    }
    w_warping = 0;
    mapstream_settle(0);
    w_in_cur_s = WORLD_SWAP_S;
    event_flush();  /* the new blocks' constructors finish behind the loading screen (A2 event-14) */
    loading_end();
    {
        unsigned d = ticks() - last + frac;
        el_ms += d / 294912;
    }
    printf("world: warp to %.1f %.1f %.1f settled in %u ms (%d steps): block %d, %d loaded; resident %u KB, mip 0 %u KB\n",
           feet[0], feet[1], feet[2], el_ms, n, w_cur, world_stats.loaded, mapstream_stats.res_kb,
           mapstream_stats.tex_kb);
}

int world_hold(const float *feet)
{
    if (w_cur < 0)
        return 0;
    ColHit h;
    if (col_floor(feet[0], feet[2], feet[1] + 1.0f, feet[1] - WORLD_HOLD_DROP, COL_SOLID, &h))
        return 0;
    WMask m = 0;
    /* COL_TILES: no floor and a collision tile around the feet still to come: held as for a block (its block is on) */
    if (tile_gone_on) {  /* (a tile that did not come in a hold's loading screen: no second hold at that spot) */
        float dx = feet[0] - tile_gone[0], dy = feet[1] - tile_gone[1], dz = feet[2] - tile_gone[2];
        if (dx * dx + dy * dy + dz * dz > WORLD_TILE_GONE_R * WORLD_TILE_GONE_R)
            tile_gone_on = 0;
    }
    if (!tile_gone_on && coltile_under(feet))
        for (int i = 1; i < w_nblk; i++)
            if (w_rt[i].state == W_ON && w_rt[i].col_full && box_d(&w_tab[i], feet) <= 0.0f)
                m |= WBIT(i);
    for (int i = 1; i < w_nblk; i++)
        if (w_rt[i].open && w_rt[i].state != W_ON && !(w_hold_gone >> i & 1) && box_d(&w_tab[i], feet) <= 0.0f &&
            feet[1] >= w_tab[i].y0 - WORLD_Y_MARGIN && feet[1] <= w_tab[i].y1 + WORLD_Y_MARGIN)
            m |= WBIT(i);
    w_hold_m |= m;
    world_stats.holds += m != 0;
    return m != 0;
}

void world_update(const float *feet, float dt)
{
    if (w_cur < 0)
        return;
    world_source(WSRC_PLAYER, feet);
    float dx = feet[0] - last_feet[0], dy = feet[1] - last_feet[1], dz = feet[2] - last_feet[2];
    int warp = dx * dx + dy * dy + dz * dz > WORLD_WARP * WORLD_WARP;  /* also the first frame: boot settles */
    memcpy(last_feet, feet, sizeof last_feet);
    if (w_hold_m && !w_hold_want) {
        printf("world: player held at %.1f %.1f %.1f for blocks %llx\n", feet[0], feet[1], feet[2], w_hold_m);
        for (int i = 1; i < w_nblk; i++)  /* what each waits on (B2 hold check): slot / pool / lean / room, or the disc */
            if (w_hold_m >> i & 1)
                printf("world: hold block %d: %s, wanted %u ms ago, last wait %s, retry %.2f s, reading %d (%u of %u KB), "
                       "near reads in flight %u, lent items let go early %u\n", i,
                       w_rt[i].state == W_OFF ? "off" : w_rt[i].state == W_READING ? "reading its lite set" : w_rt[i].state == W_ON
                       ? "on" : "draining", w_rt[i].want_t ? (ticks() - w_rt[i].want_t) / 294912 : 0, WHY[w_rt[i].why],
                       (double)w_rt[i].retry_s, world_stats.reading, w_rt[i].state == W_READING ? w_rt[i].ent : 0,
                       w_rt[i].total >> 10, mapstream_stats.pending, mapstream_lend_early());
    }
    if (!w_hold_m && w_hold_want)
        printf("world: hold released after %.0f ms\n", (double)(hold_s * 1000));
    w_hold_want = w_hold_m, w_hold_m = 0;
    hold_s = w_hold_want && !warp ? hold_s + dt : 0;
    world_stats.hold_ms += hold_s > 0 ? (unsigned)(dt * 1000) : 0;
    if (warp || hold_s >= WORLD_HOLD_S) {
        /* a warp, or held too long: the frame waits for the blocks (loading screen) */
        w_step(feet, 0);  /* the block under the new position (its collision may need loading first: settle loops) */
        settle(feet);
        if (hold_s > 0) {
            world_stats.hold_settles++;
            for (int i = 1; i < w_nblk; i++)
                if ((w_hold_want >> i & 1) && w_rt[i].state != W_ON) {
                    w_hold_gone |= WBIT(i);  /* could not load: the player walks / falls on as before */
                    printf("world: block %d did not load under the player: no more holding for it\n", i);
                }
            w_hold_want = 0, hold_s = 0;
            if (coltile_under(feet)) {
                /* the loading screen ended and a tile around the feet is still out (no room, a read error): holding
                 * again would loop hold / loading screen for good; the player goes on as if it were open air */
                memcpy(tile_gone, feet, sizeof tile_gone), tile_gone_on = 1;
                printf("world: a collision tile at %.1f %.1f %.1f did not load: no more holding within %.0f m\n",
                       feet[0], feet[1], feet[2], (double)WORLD_TILE_GONE_R);
            }
        }
        return;
    }
    w_step(feet, dt);
}
