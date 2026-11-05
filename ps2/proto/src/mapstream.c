#include <kernel.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mapstream.h"
#include "pack.h"
#include "vu1.h"

/* Cells whose box is within MS_LOAD_R m of the player are loaded, nearest first, one read at a time; beyond
 * MS_UNLOAD_R they are freed. The blobs live in one arena allocated at the first mapstream_update, i.e. after
 * enemy_init has preloaded and unloaded every enemy type (an ~8 MB peak) (first fit, coalescing free
 * list), so loading / freeing cells never fragments the heap enemy.c loads models from (a 632 KB model.bin once found
 * no contiguous block with 3.7 MB free). ASSUMED radii (render-distance; DSR keeps whole blocks resident). */
#define MS_LOAD_R 40.0f
#define MS_UNLOAD_R 52.0f
/* World blocks' resident level per cell (mapstream_res_open): loaded out to MS_RES_R, the depth map.c draws opaque
 * boxes to (map.c CULL_FAR = FAR 250 m; a box distance, so a little further), freed beyond MS_RES_UNLOAD_R. Ranked
 * against near cells by distance x MS_RES_K: the resident cell holds the kept meshes the near level does not replace
 * and the coarse level drawn until the near blob is in, so it goes first up to ~1.4x the distance (ASSUMED). */
#define MS_RES_R 260.0f
#define MS_RES_UNLOAD_R 280.0f
#define MS_RES_K 0.5f
/* Resident cells of a block the player is not in (mapstream_focus) rank as if twice as far (x4 on the squared
 * distance, ASSUMED): Firelink + the Burg blocks' resident levels within MS_RES_R exceed the arena, and with equal
 * ranks the Burg's cells kept Firelink's own out after the player came back (holes in its tree line and the aqueduct
 * wall, ISO world_route 2026-11-05) */
#define MS_RES_OTHER 4.0f
/* ... except within MS_OTHER_D: the cells across a block border the player may turn back to (the aqueduct wall's kept
 * meshes 34-70 m off were dropped in the Burg and not read again 1 s after walking back into Firelink, ISO and host:
 * round 10, 2026-11-05); ASSUMED */
#define MS_OTHER_D 100.0f
/* The full want / drop scan over every item runs when a read can start and the player moved MS_SCAN_D m or
 * MS_SCAN_FRAMES passed since the last one, else only every MS_SCAN_FRAMES (frees far items); after an item found no
 * room it waits MS_SCAN_FRAMES (~5000 items over 5 blocks: a scan every frame cost the Burg bonfire 30 -> 22 fps) */
#define MS_SCAN_D 4.0f
/* An item map.c asked for in the last MS_VIEW_FRAMES frames while it was missing (a cell in view: mapstream_get /
 * mapstream_res) ranks as if MS_VIEW_K x as far (squared: 1/4 = half the distance): the aqueduct rock wall and the
 * far north were read after cells behind the camera (ISO rounds 1-7, 2026-11-05). ASSUMED values. */
#define MS_VIEW_K 0.25f  /* loaded items drawn as lately too: else each read in view was evicted for the next one
                          * (0.414 read 18 times in 25 s, the wall never read again: ISO round 11, 2026-11-05) */
/* The backdrop's cells (MS_RES_FAR: far-only silhouettes) rank as if no further than MS_FAR_D: nearest first left the
 * far north's backdrop for last, bare for 30 s after a warp (ISO round 8, 2026-11-05); ASSUMED */
#define MS_FAR_D 64.0f
/* While a settle runs (mapstream_settle) the items it waits for (mapstream_busy's radii) rank x MS_SETTLE_K: with cells
 * in view and the backdrop ranked early the far-north warp settle went from 0.9 to 6.7 s (ISO rounds 8-9, 2026-11-05) */
#define MS_SETTLE_K 1e-4f
#define MS_VIEW_FRAMES 8u
#define MS_SCAN_FRAMES 16u
/* A texture's mip 0 (asked for at mip 0 in the last frame) ranks like a cell MS_TEX_D m away: the texture is in view
 * close up and draws mip 1 until it is in. Kept while asked for within MS_TEX_KEEP frames (ASSUMED: 4 s at 30 fps;
 * texcache keeps a resident mip 0 in VRAM without asking again, so a short keep reads it again off the disc). */
#define MS_TEX_D 16.0f
#define MS_TEX_KEEP 120u
#ifndef MS_POOL
#define MS_POOL (8u << 20)
#endif
/* EE heap left outside the arena when it is allocated: the 1.5 MB reserve (HANDOFF budget) + 1.5 MB for enemy models streamed
 * by enemy.c and objects / effects after enemy_init (smoke 21:15: 0.2 MB used); arena = min(MS_POOL, free - MS_RESERVE) */
#define MS_RESERVE (3u << 20)
static unsigned reserve_extra;  /* mapstream_reserve (world builds) */
static unsigned pool_max = MS_POOL;  /* mapstream_pool */
/* free-list entries of the arena: the resident cells and mip 0 textures are many small blobs (256 was enough for
 * whole near cells alone) */
#define MS_HOLES 1024

enum { MS_NONE, MS_READING, MS_READY, MS_FAILED };

MapStreamStats mapstream_stats;
/* one instance per map block (src/world.c; instance 0 = the boot area), all sharing the arena below. Items of an
 * instance: ids [0, ncell) its near cells (cells.bin), [ncell, ncell + nres) its resident items (mapstream_res_open:
 * cells, the sky, textures) */
typedef struct {
    Pack pk;
    unsigned base, total;
    const MapCell *tab;
    unsigned ncell;
    unsigned char *state;
    void **buf;
    unsigned *used;  /* frame the blob was last handed out */
    int borrowed;    /* pk belongs to the caller (mapstream_open_pack): not closed here */
    MapResItem *rit;
    unsigned nres;
    unsigned short *rgrp;  /* per resident item: the first item of the read it came in with (MS_RUN; itself alone) */
} MsInst;
static MsInst ms[MS_INST];
static Pack pk;
static unsigned base, total;
static const MapCell *tab;
static unsigned ncell, frame;
static unsigned char *state;
static void **buf;
static unsigned *used;
static MapResItem *rit;
static unsigned nres;
static unsigned short *rgrp;
/* Reads in flight, oldest first (pack_req_read: the disc server serves them back to back, so a small item no longer
 * costs a frame or two of its own; ISO 2026-11-05: 51-99 ms median for items under 64 KB, one at a time) */
#define MS_INFLIGHT 3
typedef struct { int id, inst, tk; unsigned n, bytes, t; } MsRead;  /* items id .. id + n of instance inst, one block */
static MsRead rd[MS_INFLIGHT];
static int nrd;
static unsigned land_t;  /* when the last read landed (or the first of a busy spell was issued) */
static float settle_r, settle_rf;  /* the radii of the last mapstream_busy */
static int settle_item(int k, unsigned j, const float *pos, float r, float r_focus);
static int sel, hold, room_fail, focus = -1, settling;
static unsigned scan_frame;  /* frame of the last full scan */
static float scan_pos[3];
/* A short ranked list of items: the next reads (lowest key first) or eviction candidates (desc: highest first) */
#define MS_QUEUE 32
typedef struct { int k; unsigned id; float key; } MsQ;
static MsQ q[MS_QUEUE];
static int nq;
/* Items that found no room (key = the frame): passed over for MS_BLOCK_FRAMES while the next ones read, so one big
 * near cell with no hole for it does not stop the reader (22.6 s without a read behind Burg near cell 1.25, 865 KB,
 * largest hole 255 KB; the aqueduct wall waited behind it: ISO round 12, 2026-11-05). ASSUMED: 3 s at 30 fps. */
#define MS_BLOCKED 8
#define MS_BLOCK_FRAMES 90u
static MsQ blk[MS_BLOCKED];
static int nblk;
/* Items whose read came back short (MS_FAILED) are queued again once fail_wait frames passed since the last failure,
 * the wait doubling while reads keep failing (3 s up to ~48 s, ASSUMED: a disc read error is rare and may not repeat)
 * and reset by a good read; before, one bad read left a hole in the cell until its block closed */
#define MS_FAIL_FRAMES 90u
#define MS_FAIL_FRAMES_MAX (16 * MS_FAIL_FRAMES)
static unsigned fail_frame, fail_wait;
static int failed;

static void q_add(MsQ *l, int *n, int cap, int k, unsigned id, float key, int desc)
{
    int i = *n < cap ? (*n)++ : cap;
    if (i == cap && (desc ? key <= l[cap - 1].key : key >= l[cap - 1].key))
        return;  /* full and ranked past the last */
    if (i == cap)
        i = cap - 1;
    while (i > 0 && (desc ? l[i - 1].key < key : l[i - 1].key > key))
        l[i] = l[i - 1], i--;
    l[i] = (MsQ){k, id, key};
}

static unsigned (*tex_age)(int k, unsigned tag);
static void (*tex_set)(int k, unsigned tag, const void *p);

/* the instance the globals above describe */
static void bind(int k)
{
    ms[sel].pk = pk;
    ms[sel].base = base, ms[sel].total = total, ms[sel].tab = tab, ms[sel].ncell = ncell;
    ms[sel].state = state, ms[sel].buf = buf, ms[sel].used = used, ms[sel].rit = rit, ms[sel].nres = nres;
    ms[sel].rgrp = rgrp;
    const MsInst *m = &ms[sel = k];
    pk = m->pk;
    base = m->base, total = m->total, tab = m->tab, ncell = m->ncell;
    state = m->state, buf = m->buf, used = m->used, rit = m->rit, nres = m->nres;
    rgrp = m->rgrp;
}

/* item id of the bound instance: bytes, offset in the pack, kind */
static inline unsigned it_n(void) { return ncell + nres; }
static inline unsigned it_bytes(unsigned id) { return id < ncell ? tab[id].bytes : rit[id - ncell].bytes; }
static inline unsigned it_off(unsigned id) { return id < ncell ? base + tab[id].off : rit[id - ncell].off; }
static inline int it_tex(unsigned id) { return id >= ncell && (rit[id - ncell].tag & MS_RES_TEX); }  /* a mip 0 */
static inline int it_res(unsigned id) { return id >= ncell; }

/* item id of instance k found no room lately (blk): passed over */
static int blocked(int k, unsigned id)
{
    for (int i = 0; i < nblk; i++)
        if (blk[i].k == k && blk[i].id == id)
            return 1;
    return 0;
}

static unsigned char *arena;
static unsigned char *pend_p;  /* mapstream_lend before the arena was allocated */
static unsigned pend_n;
static unsigned arena_size, nholes, hole_off[MS_HOLES], hole_len[MS_HOLES];  /* free list, sorted by offset */

/* main: only the arena's own bytes (not memory lent by mapstream_lend, which its owner takes back) */
static void *arena_alloc(unsigned n, int main)
{
    n = (n + 63) & ~63u;
    for (unsigned i = 0; i < nholes; i++)
        if (hole_len[i] >= n && (!main || hole_off[i] + n <= arena_size)) {
            void *p = arena + hole_off[i];
            hole_off[i] += n, hole_len[i] -= n;
            if (!hole_len[i]) {
                memmove(hole_off + i, hole_off + i + 1, (nholes - i - 1) * sizeof *hole_off);
                memmove(hole_len + i, hole_len + i + 1, (nholes - i - 1) * sizeof *hole_len);
                nholes--;
            }
            return p;
        }
    return NULL;
}

/* the same from the top of the highest hole that fits (mapstream_alloc: lite sets, relief, enemy model temporaries
 * stay apart from the items, which fill from the bottom, so a freed group of small items leaves one hole) */
static void *arena_alloc_top(unsigned n)
{
    n = (n + 63) & ~63u;
    for (unsigned i = nholes; i-- > 0;) {
        unsigned top = hole_off[i] + hole_len[i] > arena_size ? arena_size : hole_off[i] + hole_len[i];
        if (hole_off[i] >= arena_size || top - hole_off[i] < n)
            continue;
        unsigned o = top - n;
        if (top == hole_off[i] + hole_len[i]) {
            hole_len[i] -= n;
            if (!hole_len[i]) {
                memmove(hole_off + i, hole_off + i + 1, (nholes - i - 1) * sizeof *hole_off);
                memmove(hole_len + i, hole_len + i + 1, (nholes - i - 1) * sizeof *hole_len);
                nholes--;
            }
        } else {  /* the hole runs on into lent memory beyond the arena: take from the arena's end, split */
            if (nholes == MS_HOLES)
                return NULL;
            memmove(hole_off + i + 2, hole_off + i + 1, (nholes - i - 1) * sizeof *hole_off);
            memmove(hole_len + i + 2, hole_len + i + 1, (nholes - i - 1) * sizeof *hole_len);
            nholes++;
            unsigned end = hole_off[i] + hole_len[i];
            hole_len[i] = o - hole_off[i];
            hole_off[i + 1] = top, hole_len[i + 1] = end - top;
            if (!hole_len[i]) {
                memmove(hole_off + i, hole_off + i + 1, (nholes - i - 1) * sizeof *hole_off);
                memmove(hole_len + i, hole_len + i + 1, (nholes - i - 1) * sizeof *hole_len);
                nholes--;
            }
        }
        return arena + o;
    }
    return NULL;
}

static void arena_free(void *p, unsigned n)
{
    unsigned o = (unsigned char *)p - arena, i = 0;
    n = (n + 63) & ~63u;
    while (i < nholes && hole_off[i] < o)
        i++;
    int left = i > 0 && hole_off[i - 1] + hole_len[i - 1] == o, right = i < nholes && o + n == hole_off[i];
    if (left && right) {
        hole_len[i - 1] += n + hole_len[i];
        memmove(hole_off + i, hole_off + i + 1, (nholes - i - 1) * sizeof *hole_off);
        memmove(hole_len + i, hole_len + i + 1, (nholes - i - 1) * sizeof *hole_len);
        nholes--;
    } else if (left) {
        hole_len[i - 1] += n;
    } else if (right) {
        hole_off[i] = o, hole_len[i] += n;
    } else if (nholes < MS_HOLES) {
        memmove(hole_off + i + 1, hole_off + i, (nholes - i) * sizeof *hole_off);
        memmove(hole_len + i + 1, hole_len + i, (nholes - i) * sizeof *hole_len);
        hole_off[i] = o, hole_len[i] = n, nholes++;
    } else {
        printf("mapstream: free list full, %u bytes lost\n", n);  /* until the next area load */
    }
}
static int ensure_arena(void);
static float box_d2(const MapCell *c, const float *p);

static unsigned heap_free(void)
{
    struct mallinfo mi = mallinfo();
    return 32 * 1024 * 1024 - (unsigned)sbrk(0) + mi.fordblks;
}

static inline unsigned ticks(void)
{
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

int mapstream_init(const char *pak_path, const MapCell *cells, unsigned n)
{
    memset(&mapstream_stats, 0, sizeof mapstream_stats);
    return mapstream_open(0, pak_path, cells, n);
}

static int open_cells(int k, const MapCell *cells, unsigned n);

int mapstream_open(int k, const char *pak_path, const MapCell *cells, unsigned n)
{
    if (k < 0 || k >= MS_INST)
        return 0;
    mapstream_close(k);
    bind(k);
    ms[k].borrowed = 0;
    if (!n || !pack_open(&pk, pak_path))
        return 0;
    return open_cells(k, cells, n);
}

int mapstream_open_pack(int k, const void *pack, const MapCell *cells, unsigned n)
{
    if (k < 0 || k >= MS_INST || !n)
        return 0;
    mapstream_close(k);
    bind(k);
    pk = *(const Pack *)pack;
    ms[k].borrowed = 1;
    return open_cells(k, cells, n);
}

static int open_cells(int k, const MapCell *cells, unsigned n)
{
    if (!pack_entry_raw(&pk, "cells.bin", &base, &total) || !total) {
        if (!ms[k].borrowed)
            pack_close(&pk);
        return 0;
    }
    tab = cells, ncell = n;
    state = calloc(n, 1);
    buf = calloc(n, sizeof *buf);
    used = calloc(n, sizeof *used);
    rit = NULL, nres = 0, rgrp = NULL;
    if (!state || !buf || !used) {
        if (!ms[k].borrowed)
            pack_close(&pk);
        free(state), free(buf), free(used);
        state = NULL, buf = NULL, used = NULL;
        ncell = 0;
        return 0;
    }
    printf("mapstream: %u cells, cells.bin %u KB at %u (instance %d)\n", n, total >> 10, base, k);
    return 1;
}

int mapstream_res_open(int k, const MapResItem *items, unsigned n, unsigned nitems)
{
    if (k < 0 || k >= MS_INST || !nitems)
        return 0;
    int was = sel;
    bind(k);
    if (!ncell || n != ncell || rit) {
        bind(was);
        return 0;
    }
    /* the item arrays grow to ncell + nitems: state / buf / used are indexed by item id */
    unsigned char *s2 = realloc(state, ncell + nitems);
    if (s2)
        state = s2;
    void **b2 = s2 ? realloc(buf, (ncell + nitems) * sizeof *buf) : NULL;
    if (b2)
        buf = b2;
    unsigned *u2 = b2 ? realloc(used, (ncell + nitems) * sizeof *used) : NULL;
    if (u2)
        used = u2;
    rit = u2 ? malloc(nitems * sizeof *rit) : NULL;
    rgrp = rit && nitems <= 65536 ? malloc(nitems * sizeof *rgrp) : NULL;
    if (!rgrp) {
        free(rit);
        rit = NULL;
        bind(was);
        return 0;
    }
    for (unsigned j = 0; j < nitems; j++)
        rgrp[j] = (unsigned short)j;
    memcpy(rit, items, nitems * sizeof *rit);
    memset(state + ncell, MS_NONE, nitems);
    memset(buf + ncell, 0, nitems * sizeof *buf);
    memset(used + ncell, 0, nitems * sizeof *used);
    nres = nitems;
    scan_frame = 0;
    unsigned cb = 0, tb = 0, nt = 0;
    for (unsigned j = 0; j < nitems; j++)
        *(items[j].tag & MS_RES_TEX ? &tb : &cb) += items[j].bytes, nt += !!(items[j].tag & MS_RES_TEX);
    printf("mapstream: instance %d resident level per cell: %u cells + sky in %u items %u KB, %u mip 0 textures %u KB\n",
           k, n, nitems - nt, cb >> 10, nt, tb >> 10);
    bind(was);
    return 1;
}

void mapstream_tex_hooks(unsigned (*age)(int k, unsigned tag), void (*set)(int k, unsigned tag, const void *p))
{
    tex_age = age, tex_set = set;
}

void mapstream_hold(int on)
{
    hold = on;
}

void mapstream_settle(int on)
{
    if (on != settling)
        scan_frame = 0;  /* requeue */
    settling = on;
}

void mapstream_focus(int k)
{
    if (k != focus)
        scan_frame = 0;  /* ranks changed */
    focus = k;
}

void mapstream_pool(unsigned max)
{
    pool_max = max;
}

static void drop(unsigned id);

void mapstream_close(int k)
{
    if (k < 0 || k >= MS_INST || !(k == sel ? ncell : ms[k].ncell))
        return;
    int was = sel;
    bind(k);
    for (int i = 0; i < nrd; i++)  /* stop its reads (after their current page) before the buffers go back */
        if (rd[i].inst == k) {
            pack_req_cancel(rd[i].tk);
            memmove(rd + i, rd + i + 1, (--nrd - i) * sizeof *rd);
            i--;
        }
    mapstream_stats.pending = nrd;
    for (unsigned id = 0; id < it_n(); id++)
        if (state[id] == MS_READY || state[id] == MS_READING)
            drop(id);
    if (!ms[k].borrowed)
        pack_close(&pk);
    free(state), free(buf), free(used), free(rit), free(rgrp);
    state = NULL, buf = NULL, used = NULL, ncell = 0, tab = NULL, rit = NULL, nres = 0, rgrp = NULL;
    nq = 0, scan_frame = 0;  /* queued ids may name this instance's items */
    for (int i = 0; i < nblk; i++)
        if (blk[i].k == k)
            blk[i--] = blk[--nblk];
    bind(was == k ? 0 : was);
}

void mapstream_select(int k)
{
    if (k >= 0 && k < MS_INST && k != sel)
        bind(k);
}

static float box_d2(const MapCell *c, const float *p)
{
    float d2 = 0;
    for (int k = 0; k < 3; k++) {
        float o = p[k] < c->mn[k] ? c->mn[k] - p[k] : p[k] > c->mx[k] ? p[k] - c->mx[k] : 0;
        d2 += o * o;
    }
    return d2;
}

/* What the bound instance k wants of item id with the player at pos: 2 load, 1 keep, 0 free; *key ranks it (lower
 * first: squared metres) */
static int it_want(int k, unsigned id, const float *pos, float *key)
{
    if (id < ncell) {
        float d2 = box_d2(&tab[id], pos);
        *key = d2 * (frame - used[id] <= MS_VIEW_FRAMES ? MS_VIEW_K : 1.0f);
        if (settling)  /* a settle waits for the resident level only: near cells stream in behind the game */
            return d2 <= MS_UNLOAD_R * MS_UNLOAD_R;
        return d2 < MS_LOAD_R * MS_LOAD_R ? 2 : d2 <= MS_UNLOAD_R * MS_UNLOAD_R;
    }
    unsigned tag = rit[id - ncell].tag;
    if (!(tag & (MS_RES_SKY | MS_RES_TEX)) && (tag & ~MS_RES_FAR) < ncell) {  /* a cell's resident chunks (or a part) */
        float d2 = box_d2(&tab[tag & ~MS_RES_FAR], pos);
        if ((tag & MS_RES_FAR) && d2 > MS_FAR_D * MS_FAR_D)
            d2 = MS_FAR_D * MS_FAR_D;
        *key = d2 * (focus < 0 || k == focus || d2 < MS_OTHER_D * MS_OTHER_D ? MS_RES_K : MS_RES_K * MS_RES_OTHER) *
               (frame - used[id] <= MS_VIEW_FRAMES ? MS_VIEW_K : 1.0f);
        if (settling && settle_r > 0 && settle_item(k, id - ncell, pos, settle_r, settle_rf))
            *key *= MS_SETTLE_K;  /* what the settle waits for before anything else in view or the backdrop */
        if (k == focus)  /* the player's block: its whole resident level, as the old full pool held it, but nearest
                          * first and with no swap (beyond MS_RES_R: Firelink's horizon tree line and the far north
                          * went missing behind the Burg's cells, ISO rounds 3-6, 2026-11-05) */
            return 2;
        return d2 < MS_RES_R * MS_RES_R ? 2 : d2 <= MS_RES_UNLOAD_R * MS_RES_UNLOAD_R;
    }
    if (!(tag & MS_RES_TEX)) {  /* the sky */
        *key = 0;
        return 2;
    }
    unsigned age = tex_age ? tex_age(k, tag & ~MS_RES_TEX) : ~0u;
    if (age <= 1)
        used[id] = frame;  /* asked for: no eviction under it */
    *key = age <= MS_TEX_KEEP ? MS_TEX_D * MS_TEX_D : 1e30f;
    return age <= 1 ? 2 : age <= MS_TEX_KEEP;
}

/* the items [*first, return) of the bound instance that came in one read with item id (MS_RUN) */
static unsigned run_of(unsigned id, unsigned *first)
{
    if (!it_res(id) || !rgrp) {
        *first = id;
        return id + 1;
    }
    unsigned j0 = rgrp[id - ncell], e = j0 + 1;
    while (e < nres && rgrp[e] == j0)
        e++;
    *first = ncell + j0;
    return ncell + e;
}

/* some item of id's run was handed out within the last f frames */
static int run_used(unsigned id, unsigned f)
{
    unsigned a, e = run_of(id, &a);
    for (unsigned m = a; m < e; m++)
        if (frame - used[m] <= f)
            return 1;
    return 0;
}

/* the lowest rank over id's run (dropping it drops them all) */
static float run_key(int k, unsigned id, const float *pos)
{
    unsigned a, e = run_of(id, &a);
    float best = 1e30f, key;
    for (unsigned m = a; m < e; m++) {
        it_want(k, m, pos, &key);
        best = key < best ? key : best;
    }
    return best;
}

/* free item id, and with it the items read in the same block (they share its memory) */
static void drop(unsigned id)
{
    unsigned a, e = run_of(id, &a), n = it_res(a) ? rit[e - 1 - ncell].rel + rit[e - 1 - ncell].bytes - rit[a - ncell].rel
                                                 : it_bytes(a);
    arena_free(buf[a], n);
    for (unsigned m = a; m < e; m++) {
        if (it_tex(m) && tex_set)
            tex_set(sel, rit[m - ncell].tag & ~MS_RES_TEX, NULL);
        unsigned b = it_bytes(m);
        buf[m] = NULL;
        state[m] = MS_NONE;
        used[m] = 0;  /* not "asked for" (MS_VIEW_K) by its last hand-out */
        mapstream_stats.cells--;
        mapstream_stats.bytes -= b;
        if (it_res(m)) {
            *(it_tex(m) ? &mapstream_stats.tex_kb : &mapstream_stats.res_kb) -= (b + 1023) >> 10;
            rgrp[m - ncell] = (unsigned short)(m - ncell);
        }
    }
}

static int ensure_arena(void)
{
    if (!arena) {
        unsigned hf = heap_free();
        unsigned rs = MS_RESERVE + reserve_extra;
        arena_size = hf > rs + (1u << 20) ? hf - rs : 0;
        arena_size = (arena_size < pool_max ? arena_size : pool_max) & ~63u;
        arena = arena_size ? memalign(64, arena_size) : NULL;
        printf("mapstream: arena %u KB (heap free %u KB)\n", arena ? arena_size >> 10 : 0, hf >> 10);
        if (!arena)
            return 0;
        hole_off[0] = 0, hole_len[0] = arena_size, nholes = 1;
        if (pend_n) {
            arena_free(pend_p, pend_n);
            mapstream_stats.lent_kb += pend_n >> 10;
            pend_p = NULL, pend_n = 0;
        }
    }
    return 1;
}

void *mapstream_alloc(unsigned n, const float pos[3])
{
    if (!ensure_arena())
        return NULL;
    int was = sel, forced = 0;
    unsigned forced_kb = 0;
    void *p;
    /* no room even with every item not drawn this frame gone: keep them all (a block lite set retried each frame
     * dropped 1.4 MB of drawn cells per try and never fit; 2026-11-03, ISO of 9e6dd67) */
    unsigned room = 0;
    for (unsigned i = 0; i < nholes; i++)
        if (hole_off[i] < arena_size)
            room += hole_off[i] + hole_len[i] > arena_size ? arena_size - hole_off[i] : hole_len[i];
    for (int k = 0; k < MS_INST && room < n; k++) {
        bind(k);
        for (unsigned id = 0; id < it_n(); id++)
            if (state[id] == MS_READY && !run_used(id, 0))
                room += (it_bytes(id) + 63) & ~63u;
    }
    bind(was);
    if (room < n)
        return NULL;
    /* drop the farthest item no chain reads (any distance; resident cells ranked as mapstream_update ranks them)
     * until n bytes fit. When every item left was drawn in the last frames (the player stands among them), drop
     * drawn ones too: callers run in the update phase (enemy.c stream_types, world.c), after main.c's end-of-frame
     * vu1_wait and before this frame's map_draw, so once VIF1 is idle no chain reads an item not handed out this
     * frame. Without it a 579 KB c2540 model.bin found no room in the Burg and the heap fallback failed on
     * fragmentation (40-209 OUT OF RAM per 30-minute soak, 2026-11-01). Dropped items stream back. */
    while (!(p = arena_alloc_top(n))) {
        int far = -1, far_k = 0;
        float fd = -1, key;
        for (int k = 0; k < MS_INST; k++) {
            bind(k);
            for (unsigned id = 0; id < it_n(); id++)
                if (state[id] == MS_READY && !run_used(id, forced ? 0 : 2)) {
                    it_want(k, id, pos, &key);
                    if (key > fd)
                        far = id, far_k = k, fd = key;
                }
        }
        if (far < 0 && !forced) {
            vu1_wait();
            forced = 1;
            continue;
        }
        if (far < 0)
            break;
        bind(far_k);
        forced_kb += forced ? it_bytes(far) >> 10 : 0;
        drop(far);
    }
    bind(was);
    if (forced_kb)  /* the caller falls back to the heap on NULL */
        printf("mapstream: alloc %u KB %s, %u KB of drawn cells dropped\n", n >> 10, p ? "ok" : "FAILED", forced_kb);
    if (p)
        mapstream_stats.other_kb += (n + 63) >> 10;
    return p;
}

int mapstream_ready(void)
{
    return arena != NULL;
}

int mapstream_arena(void)
{
    return ensure_arena();
}

int mapstream_owns(const void *p)
{
    return arena && (const unsigned char *)p >= arena && (const unsigned char *)p < arena + arena_size;
}

void mapstream_reserve(unsigned extra)
{
    reserve_extra = extra;
}

void mapstream_free(void *p, unsigned n)
{
    if (!p)
        return;
    arena_free(p, n);
    mapstream_stats.other_kb -= (n + 63) >> 10;
}

/* Lent memory is addressed like the arena (offsets from arena, mod 2^32: a region below the arena sorts after it) */
void mapstream_lend(void *p, unsigned n)
{
    unsigned char *a = (unsigned char *)(((unsigned)p + 63) & ~63u);
    n = n > (unsigned)(a - (unsigned char *)p) ? (n - (a - (unsigned char *)p)) & ~63u : 0;
    if (!n)
        return;
    if (!arena) {  /* before the arena exists (its size depends on the heap left after the area load): add it then */
        pend_p = a, pend_n = n;
        return;
    }
    arena_free(a, n);
    mapstream_stats.lent_kb += n >> 10;
}

int mapstream_reclaim(void *p, unsigned n)
{
    unsigned char *a = (unsigned char *)(((unsigned)p + 63) & ~63u);
    n = n > (unsigned)(a - (unsigned char *)p) ? (n - (a - (unsigned char *)p)) & ~63u : 0;
    if (!n)
        return 1;
    if (a == pend_p) {
        pend_p = NULL, pend_n = 0;
        return 1;
    }
    if (!arena)
        return 1;
    unsigned long long o = (unsigned)(a - arena), e = o + n;
    int was = sel;
    for (int k = 0; k < MS_INST; k++) {  /* the items in it go (the caller drains the chains before reusing it) */
        bind(k);
        for (unsigned id = 0; id < it_n(); id++) {
            unsigned long long b = (unsigned)((unsigned char *)buf[id] - arena);
            if (buf[id] && b < e && b + it_bytes(id) > o) {
                if (state[id] == MS_READING) {  /* mapstream_update collects it first */
                    bind(was);
                    return 0;
                }
                drop(id);
            }
        }
    }
    bind(was);
    unsigned long long got = 0;
    for (unsigned i = 0; i < nholes; i++) {
        unsigned long long hs = hole_off[i], he = hs + hole_len[i];
        if (hs < e && he > o)
            got += (he < e ? he : e) - (hs > o ? hs : o);
    }
    if (got != n)
        return 0;  /* something else lives in it (mapstream_alloc never places there) */
    for (unsigned i = 0; i < nholes; i++) {  /* carve [o, e) out of the free list */
        unsigned long long hs = hole_off[i], he = hs + hole_len[i];
        if (hs >= e || he <= o)
            continue;
        int left = hs < o, right = he > e;
        if (left && right) {
            if (nholes == MS_HOLES)
                return 0;
            memmove(hole_off + i + 2, hole_off + i + 1, (nholes - i - 1) * sizeof *hole_off);
            memmove(hole_len + i + 2, hole_len + i + 1, (nholes - i - 1) * sizeof *hole_len);
            nholes++;
            hole_len[i] = o - hs;
            hole_off[i + 1] = (unsigned)e, hole_len[i + 1] = he - e;
            i++;
        } else if (left) {
            hole_len[i] = o - hs;
        } else if (right) {
            hole_off[i] = (unsigned)e, hole_len[i] = he - e;
        } else {
            memmove(hole_off + i, hole_off + i + 1, (nholes - i - 1) * sizeof *hole_off);
            memmove(hole_len + i, hole_len + i + 1, (nholes - i - 1) * sizeof *hole_len);
            nholes--, i--;
        }
    }
    mapstream_stats.lent_kb -= n >> 10;
    return 1;
}

/* An item's blob: large ones (near cells up to 2.2 MB in the Burg) from the top with mapstream_alloc's lite sets, small
 * ones (resident items, mip 0) from the bottom, so a big near cell does not wait for one hole among small items (ISO
 * round 5, 2026-11-05: 158 of 181 refusals had the room but no hole) */
#define MS_BIG (256u << 10)
#define MS_RUN (128u << 10)
/* a loaded item gives way only to one ranked MS_EVICT_M x nearer (squared: ~1.4x the distance; ASSUMED hysteresis
 * against read / evict loops between items of about the same rank) */
#define MS_EVICT_M 2.0f  /* ASSUMED: RES_ITEM (map.c), ~0.2 s from the disc */
static void *item_alloc(unsigned n)
{
    return n >= MS_BIG ? arena_alloc_top(n) : arena_alloc(n, 0);
}

/* read i landed (r bytes) */
static void collect(int i, int r)
{
    MsRead w = rd[i];
    memmove(rd + i, rd + i + 1, (--nrd - i) * sizeof *rd);
    mapstream_stats.pending = nrd;
    int reading_inst = w.inst;
    bind(reading_inst);
    unsigned id = w.id, e = w.id + w.n, n = w.bytes, now = ticks();
    /* its own time: from when the read before it landed if it waited behind it */
    unsigned t0 = (int)(w.t - land_t) > 0 ? w.t : land_t;
    land_t = now;
    if (r == (int)n) {
        InvalidDCache(buf[id], (char *)buf[id] + n - 1);  /* drop stale lines over the DMA'd data */
        fail_wait = 0;
        mapstream_stats.ms_last = (now - t0) / 294912;
        for (unsigned m = id; m < e; m++) {
            state[m] = MS_READY;
            used[m] = frame;  /* ranks as in view (MS_VIEW_K) until map.c draws it or not */
            mapstream_stats.loads++;
            if (it_tex(m) && tex_set)
                tex_set(reading_inst, rit[m - ncell].tag & ~MS_RES_TEX, buf[m]);
        }
        if (!it_tex(id))
            printf("mapstream: %s %d.%u (%u items) %u KB in %u ms, %u items %u KB loaded (resident %u KB, mip 0 %u KB)\n",
                   it_res(id) ? "resident cell" : "cell", reading_inst, it_res(id) ? id - ncell : id, e - id, n >> 10,
                   mapstream_stats.ms_last, mapstream_stats.cells, mapstream_stats.bytes >> 10, mapstream_stats.res_kb,
                   mapstream_stats.tex_kb);
    } else {
        printf("mapstream: item %d.%u (%u items) read %d of %u bytes\n", reading_inst, id, e - id, r, n);
        drop(id);
        for (unsigned m = id; m < e; m++)
            state[m] = MS_FAILED;
        mapstream_stats.fails++;
        failed = 1, fail_frame = frame;
        fail_wait = !fail_wait ? MS_FAIL_FRAMES : fail_wait < MS_FAIL_FRAMES_MAX ? 2 * fail_wait : fail_wait;
    }
}

static int issue(const float *pos);

void mapstream_update(const float pos[3])
{
    int any = 0, was = sel;
    for (int k = 0; k < MS_INST; k++)
        any |= ms[k].ncell || (k == sel && ncell);
    if (!any)
        return;
    if (!ensure_arena()) {  /* no near level: the coarse one draws everywhere */
        for (int k = 0; k < MS_INST; k++)
            mapstream_close(k);
        return;
    }
    frame++;
    int r;
    for (int i = 0; i < nrd; i++)
        if (pack_req_done(rd[i].tk, &r))
            collect(i--, r);
    /* free far items no chain still reads; queue the most wanted missing items over every instance (MS_SCAN_*) */
    float dx = pos[0] - scan_pos[0], dy = pos[1] - scan_pos[1], dz = pos[2] - scan_pos[2];
    int moved = dx * dx + dy * dy + dz * dz > MS_SCAN_D * MS_SCAN_D;
    int due = frame - scan_frame >= MS_SCAN_FRAMES || !scan_frame || (moved && !room_fail);
    int can = nrd < MS_INFLIGHT && !hold;
    if (due) {
        scan_frame = frame;
        memcpy(scan_pos, pos, sizeof scan_pos);
        nq = 0;
        float key;
        for (int i = 0; i < nblk; i++)
            if (frame - (unsigned)blk[i].key >= MS_BLOCK_FRAMES)
                blk[i--] = blk[--nblk];
        int retry = failed && frame - fail_frame >= fail_wait;
        failed &= !retry;
        for (int k = 0; k < MS_INST; k++) {
            bind(k);
            for (unsigned id = 0; id < it_n(); id++) {
                if (state[id] == MS_FAILED && retry)
                    state[id] = MS_NONE;
                if (!it_bytes(id) || state[id] == MS_READING || state[id] == MS_FAILED)
                    continue;
                int w = it_want(k, id, pos, &key);
                if (state[id] == MS_READY && !w && !run_used(id, 2))
                    drop(id);
                else if (state[id] == MS_NONE && w == 2 && !blocked(k, id))
                    q_add(q, &nq, MS_QUEUE, k, id, key, 0);
            }
        }
    }
    while (can && nq && nrd < MS_INFLIGHT && issue(pos))
        ;
    bind(was);
}

/* the next queued item still missing (reads between scans take the queue in rank order) into the arena and read;
 * 0 when nothing more goes out this frame */
static int issue(const float *pos)
{
    int was = sel;
    MsQ w = q[0];
    memmove(q, q + 1, --nq * sizeof *q);
    bind(w.k);
    if (state[w.id] != MS_NONE) {
        bind(was);
        return 1;  /* loaded or reading since: the next one */
    }
    /* a small resident item takes the missing wanted ones after it in mapres.bin along, up to MS_RUN bytes in one
     * read (one block, freed together): Firelink's 815 items took ~30 s at one a read when each read cost a frame or
     * two of its own (ISO round 8, 2026-11-05); fewer, larger reads still save seeks */
    unsigned wn = 1, wbytes = it_bytes(w.id);
    if (it_res(w.id)) {
        unsigned j = w.id - ncell, end = rit[j].rel + rit[j].bytes, tex = !!(rit[j].tag & MS_RES_TEX);
        float key;
        while (j + wn < nres) {
            const MapResItem *c = &rit[j + wn];
            if (state[ncell + j + wn] != MS_NONE || !c->bytes || c->rel < end || c->rel - end > 64 ||
                c->off - c->rel != rit[j].off - rit[j].rel || c->rel + c->bytes - rit[j].rel > MS_RUN ||
                !!(c->tag & MS_RES_TEX) != tex || it_want(w.k, ncell + j + wn, pos, &key) != 2)
                break;
            end = c->rel + c->bytes, wn++;
        }
        wbytes = end - rit[j].rel;
        if (wn > 1 && wbytes >= MS_BIG)
            wn = 1, wbytes = it_bytes(w.id);
    }
    void *p = item_alloc(wbytes);
    if (!p && wn > 1)  /* no room for the run as it is: the item alone, making room for it below */
        wn = 1, wbytes = it_bytes(w.id);
    /* make room in the arena: drop the least wanted loaded items ranked below the one wanted until its blob fits
     * (one pass ranks up to MS_QUEUE of them, least wanted first) */
    while (!p && !(p = item_alloc(wbytes))) {
        MsQ d[MS_QUEUE];
        int nd = 0;
        float key;
        for (int k = 0; k < MS_INST; k++) {
            bind(k);
            for (unsigned id = 0; id < it_n(); id++)
                if (state[id] == MS_READY && !run_used(id, 0)) {  /* drawn last frame is fine: mapstream_update
                                                                      * runs after main.c's end-of-frame vu1_wait */
                    key = run_key(k, id, pos);
                    if (key > w.key * MS_EVICT_M)
                        q_add(d, &nd, MS_QUEUE, k, id, key, 1);
                }
        }
        if (!nd) {
            if (!room_fail++) {
                unsigned fr = 0, big = 0;
                for (unsigned i = 0; i < nholes; i++)
                    fr += hole_len[i], big = hole_len[i] > big ? hole_len[i] : big;
                printf("mapstream: no room for item %d.%u (%u KB, rank %.0f m): arena full of nearer items; %u KB free, "
                       "largest hole %u KB, items %u KB (resident %u, mip 0 %u), other %u KB\n", w.k, w.id, wbytes >> 10,
                       sqrtf(w.key), fr >> 10, big >> 10, mapstream_stats.bytes >> 10, mapstream_stats.res_kb,
                       mapstream_stats.tex_kb, mapstream_stats.other_kb);
            }
            mapstream_stats.room_fails++;
            if (nblk == MS_BLOCKED)
                blk[0] = blk[--nblk];
            blk[nblk++] = (MsQ){w.k, w.id, (float)frame};  /* the next queued one reads next frame */
            bind(was);
            return 0;  /* arena full of items ranked above it: the coarse level (or nothing) stays for this one */
        }
        for (int j = 0; j < nd && !p; j++) {
            bind(d[j].k);
            if (state[d[j].id] == MS_READY)  /* not gone with an earlier one's run */
                drop(d[j].id);
            bind(w.k);
            p = item_alloc(wbytes);
        }
        if (p)
            break;
    }
    room_fail = 0;
    bind(w.k);
    int tk = pack_req_read(&pk, it_off(w.id), p, wbytes);
    if (!tk) {
        arena_free(p, wbytes);
        q_add(q, &nq, MS_QUEUE, w.k, w.id, w.key, 0);  /* every request slot is taken: again next frame */
        bind(was);
        return 0;
    }
    for (unsigned m = w.id; m < w.id + wn; m++) {
        unsigned b = it_bytes(m);
        buf[m] = (unsigned char *)p + (it_res(m) ? rit[m - ncell].rel - rit[w.id - ncell].rel : 0);
        state[m] = MS_READING;
        if (it_res(m)) {
            rgrp[m - ncell] = (unsigned short)(w.id - ncell);
            *(it_tex(m) ? &mapstream_stats.tex_kb : &mapstream_stats.res_kb) += (b + 1023) >> 10;
        }
        mapstream_stats.cells++;
        mapstream_stats.bytes += b;
    }
    if (!nrd)
        land_t = ticks();
    rd[nrd++] = (MsRead){.id = w.id, .inst = w.k, .tk = tk, .n = wn, .bytes = wbytes, .t = ticks()};
    mapstream_stats.pending = nrd;
    bind(was);
    return 1;
}

/* resident item j of the bound instance k counts for a settle: the sky, a cell's within r, or within r_focus for the
 * block the player is in (no mip 0 texture) */
static int settle_item(int k, unsigned j, const float *pos, float r, float r_focus)
{
    unsigned tag = rit[j].tag;
    if (tag & MS_RES_TEX)
        return 0;
    if (tag & MS_RES_SKY)
        return 1;
    float rr = k == focus && r_focus > r ? r_focus : r;
    return (tag & ~MS_RES_FAR) < ncell && box_d2(&tab[tag & ~MS_RES_FAR], pos) < rr * rr;
}

int mapstream_busy(const float pos[3], float r, float r_focus)
{
    int was = sel, busy = 0;
    if (r != settle_r || r_focus != settle_rf)
        scan_frame = 0;  /* requeue: the settle's items first */
    settle_r = r, settle_rf = r_focus;
    for (int i = 0; i < nrd && !busy; i++) {  /* only reads the settle waits for: further ones stream on behind the
                                               * game (a settle that waited for every read in flight went on over the
                                               * whole 260 m queue: 10-14 s, ISO) */
        bind(rd[i].inst);
        busy = it_res(rd[i].id) && !it_tex(rd[i].id) && settle_item(rd[i].inst, rd[i].id - ncell, pos, r, r_focus);
        bind(was);
    }
    if (busy)
        return 1;
    if (room_fail)
        return 0;
    for (int k = 0; k < MS_INST && !busy; k++) {
        bind(k);
        for (unsigned j = 0; j < nres && !busy; j++)
            busy = state[ncell + j] == MS_NONE && rit[j].bytes && settle_item(k, j, pos, r, r_focus) &&
                   !blocked(k, ncell + j);
    }
    bind(was);
    return busy;
}

const void *mapstream_get(unsigned i)
{
    if (i < ncell && state[i] == MS_NONE)
        used[i] = frame;  /* asked for while missing: in view (MS_VIEW_K) */
    if (i >= ncell || state[i] != MS_READY)
        return NULL;
    used[i] = frame;
    return buf[i];
}

const unsigned char *mapstream_res(unsigned i)
{
    if (i < nres && state[ncell + i] == MS_NONE)
        used[ncell + i] = frame;  /* asked for while missing: in view (MS_VIEW_K) */
    if (i >= nres || state[ncell + i] != MS_READY)
        return NULL;
    used[ncell + i] = frame;
    return (const unsigned char *)buf[ncell + i] - rit[i].rel;
}
