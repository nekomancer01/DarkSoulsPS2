#include <kernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mapstream.h"
#include "pack.h"

/* Cells whose box is within MS_LOAD_R m of the player are loaded, nearest first, one read at a time; beyond
 * MS_UNLOAD_R they are freed. The blobs live in one arena allocated at the first mapstream_update, i.e. after
 * enemy_init has preloaded and unloaded every enemy type (an ~8 MB peak) (first fit, coalescing free
 * list), so loading / freeing cells never fragments the heap enemy.c loads models from (a 632 KB model.bin once found
 * no contiguous block with 3.7 MB free). ASSUMED radii (render-distance; DSR keeps whole blocks resident). */
#define MS_LOAD_R 40.0f
#define MS_UNLOAD_R 52.0f
#ifndef MS_POOL
#define MS_POOL (8u << 20)
#endif
/* EE heap left outside the arena when it is allocated: the 1.5 MB reserve (HANDOFF budget) + 1.5 MB for enemy models streamed
 * by enemy.c and objects / effects after enemy_init (smoke 21:15: 0.2 MB used); arena = min(MS_POOL, free - MS_RESERVE) */
#define MS_RESERVE (3u << 20)
static unsigned reserve_extra;  /* mapstream_reserve (world builds) */
#define MS_HOLES 256  /* free-list entries of the arena */

enum { MS_NONE, MS_READING, MS_READY, MS_FAILED };

MapStreamStats mapstream_stats;
/* one instance per map block (src/world.c; instance 0 = the boot area), all sharing the arena below */
typedef struct {
    Pack pk;
    unsigned base, total;
    const MapCell *tab;
    unsigned ncell;
    unsigned char *state;
    void **buf;
    unsigned *used;  /* frame the blob was last handed out */
    int borrowed;    /* pk belongs to the caller (mapstream_open_pack): not closed here */
} MsInst;
static MsInst ms[MS_INST];
static Pack pk;
static unsigned base, total;
static const MapCell *tab;
static unsigned ncell, frame;
static unsigned char *state;
static void **buf;
static unsigned *used;
static int sel, reading = -1, reading_inst;

/* the instance the globals above describe */
static void bind(int k)
{
    ms[sel].pk = pk;
    ms[sel].base = base, ms[sel].total = total, ms[sel].tab = tab, ms[sel].ncell = ncell;
    ms[sel].state = state, ms[sel].buf = buf, ms[sel].used = used;
    const MsInst *m = &ms[sel = k];
    pk = m->pk;
    base = m->base, total = m->total, tab = m->tab, ncell = m->ncell;
    state = m->state, buf = m->buf, used = m->used;
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
    }  /* else: the bytes are lost until the next area load (256 holes: not reached with whole-cell blobs) */
}
static unsigned t_read;
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
    if (!state || !buf || !used) {
        if (!ms[k].borrowed)
            pack_close(&pk);
        ncell = 0;
        return 0;
    }
    printf("mapstream: %u cells, cells.bin %u KB at %u (instance %d)\n", n, total >> 10, base, k);
    return 1;
}

static void drop(unsigned i);

void mapstream_close(int k)
{
    if (k < 0 || k >= MS_INST || !(k == sel ? ncell : ms[k].ncell))
        return;
    int was = sel;
    bind(k);
    if (reading >= 0 && reading_inst == k) {  /* let the read land before its buffer goes back to the arena */
        int r;
        while (!pack_async_done(&r))
            ;
        reading = -1;
        mapstream_stats.pending = 0;
    }
    for (unsigned i = 0; i < ncell; i++)
        if (state[i] == MS_READY || state[i] == MS_READING)
            drop(i);
    if (!ms[k].borrowed)
        pack_close(&pk);
    free(state), free(buf), free(used);
    state = NULL, buf = NULL, used = NULL, ncell = 0, tab = NULL;
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

static void drop(unsigned i)
{
    arena_free(buf[i], tab[i].bytes);
    buf[i] = NULL;
    state[i] = MS_NONE;
    mapstream_stats.cells--;
    mapstream_stats.bytes -= tab[i].bytes;
}

static int ensure_arena(void)
{
    if (!arena) {
        unsigned hf = heap_free();
        unsigned rs = MS_RESERVE + reserve_extra;
        arena_size = hf > rs + (1u << 20) ? hf - rs : 0;
        arena_size = (arena_size < MS_POOL ? arena_size : MS_POOL) & ~63u;
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
    int was = sel;
    void *p;
    while (!(p = arena_alloc(n, 1))) {  /* drop the farthest cell no chain reads (any distance) until n bytes fit */
        int far = -1, far_k = 0;
        float fd = -1;
        for (int k = 0; k < MS_INST; k++) {
            bind(k);
            for (unsigned i = 0; i < ncell; i++)
                if (state[i] == MS_READY && frame - used[i] > 2) {
                    float d2 = box_d2(&tab[i], pos);
                    if (d2 > fd)
                        far = i, far_k = k, fd = d2;
                }
        }
        if (far < 0)
            break;
        bind(far_k);
        drop(far);
    }
    bind(was);
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
    for (int k = 0; k < MS_INST; k++) {  /* the cells in it go (the caller drains the chains before reusing it) */
        bind(k);
        for (unsigned i = 0; i < ncell; i++) {
            unsigned long long b = (unsigned)((unsigned char *)buf[i] - arena);
            if (buf[i] && b < e && b + tab[i].bytes > o) {
                if (state[i] == MS_READING) {  /* mapstream_update collects it first */
                    bind(was);
                    return 0;
                }
                drop(i);
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
    if (reading >= 0 && pack_async_done(&r)) {
        bind(reading_inst);
        unsigned i = reading;
        reading = -1;
        mapstream_stats.pending = 0;
        if (r == (int)tab[i].bytes) {
            InvalidDCache(buf[i], (char *)buf[i] + tab[i].bytes - 1);  /* drop stale lines over the DMA'd data */
            state[i] = MS_READY;
            used[i] = 0;
            mapstream_stats.loads++;
            mapstream_stats.ms_last = (ticks() - t_read) / 294912;
            printf("mapstream: cell %d.%u %u KB in %u ms, %u cells %u KB loaded\n", reading_inst, i, tab[i].bytes >> 10,
                   mapstream_stats.ms_last, mapstream_stats.cells, mapstream_stats.bytes >> 10);
        } else {
            printf("mapstream: cell %d.%u read %d of %u bytes\n", reading_inst, i, r, tab[i].bytes);
            arena_free(buf[i], tab[i].bytes);
            buf[i] = NULL;
            state[i] = MS_FAILED;
            mapstream_stats.fails++;
            mapstream_stats.cells--;
            mapstream_stats.bytes -= tab[i].bytes;
        }
    }
    /* free far cells no chain still reads; pick the nearest missing cell in range over every instance */
    int want = -1, want_k = 0;
    float wd = MS_LOAD_R * MS_LOAD_R;
    for (int k = 0; k < MS_INST; k++) {
        bind(k);
        for (unsigned i = 0; i < ncell; i++) {
            if (!tab[i].bytes)
                continue;
            float d2 = box_d2(&tab[i], pos);
            if (state[i] == MS_READY && d2 > MS_UNLOAD_R * MS_UNLOAD_R && frame - used[i] > 2)
                drop(i);
            else if (state[i] == MS_NONE && d2 < wd)
                want = i, want_k = k, wd = d2;
        }
    }
    if (want < 0 || reading >= 0) {
        bind(was);
        return;
    }
    /* make room in the arena: drop the farthest loaded cell outside the load range until the blob fits */
    bind(want_k);
    unsigned wbytes = tab[want].bytes;
    void *p;
    while (!(p = arena_alloc(wbytes, 0))) {
        int far = -1, far_k = 0;
        float fd = MS_LOAD_R * MS_LOAD_R;
        for (int k = 0; k < MS_INST; k++) {
            bind(k);
            for (unsigned i = 0; i < ncell; i++)
                if (state[i] == MS_READY && frame - used[i] > 2) {
                    float d2 = box_d2(&tab[i], pos);
                    if (d2 > fd)
                        far = i, far_k = k, fd = d2;
                }
        }
        if (far < 0) {
            bind(was);
            return;  /* arena full of cells in range: the coarse level stays for this one */
        }
        bind(far_k);
        drop(far);
    }
    bind(want_k);
    buf[want] = p;
    if (!pack_read_async(&pk, base + tab[want].off, buf[want], tab[want].bytes)) {
        arena_free(buf[want], tab[want].bytes);
        buf[want] = NULL;
        bind(was);
        return;
    }
    state[want] = MS_READING;
    reading = want;
    reading_inst = want_k;
    t_read = ticks();
    mapstream_stats.cells++;
    mapstream_stats.bytes += tab[want].bytes;
    mapstream_stats.pending = 1;
    bind(was);
}

const void *mapstream_get(unsigned i)
{
    if (i >= ncell || state[i] != MS_READY)
        return NULL;
    used[i] = frame;
    return buf[i];
}
