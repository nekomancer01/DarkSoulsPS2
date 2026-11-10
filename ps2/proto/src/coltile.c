#include <kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "coltile.h"
#include "collision.h"
#include "mapstream.h"
#include "pack.h"
#include "dsrsnd_rpc.h"

/* Which pages a block holds: those whose box lies within CT_WANT_R (xz) / CT_WANT_Y (y) of the feet are read; those past
 * CT_KEEP_R / CT_KEEP_Y go. In between they stay while there is room: a wanted page with no room in the structural pool
 * first frees the held ones outside the want window, farthest first. ASSUMED radii: enemies freeze beyond 45 m (enemy.c
 * ACTIVE_DIST), so no character queries collision farther out; the keep ring is half a tile wider so a player walking
 * along a tile edge reads nothing twice. Heights: the Burg's levels lie 8-16 m apart (the cube-grid measurements,
 * 2026-11-09). */
#define CT_WANT_R 48.0f
#define CT_WANT_Y 16.0f
#define CT_KEEP_R 56.0f
#define CT_KEEP_Y 24.0f
#define CT_BUDGET_M 4.0f  /* coltile_budget: the feet are not only at page centres (ASSUMED margin) */
/* coltile_budget: + 1/CT_PACK_K for the pages' packing into the pool's 64 KB pages (mapstream_alloc_pool_small: a page
 * never straddles two; ASSUMED, the log's "coltile:" line prints the pool pages the tiles touch) */
#define CT_PACK_K 8
/* One run: the wanted page nearest the feet and the absent pages after it in the same 32 m map cell, which lie next to
 * each other on the disc, as back-to-back requests (one seek; Sony DevCon 2002 "Why Do My Load Times Suck": a short DVD
 * seek ~40 ms costs about as much as reading 100 KB). Each page has its own pool block, so pages go one by one. */
#define CT_RUN_MAX (128u << 10)
#define CT_INFLIGHT 8
#define CT_RETRY_FRAMES 30   /* a wanted page with no room asks again after (ASSUMED: half a second) */
#define CT_MAX_PAGES 1024    /* pages per block (the Burg blocks have 85-215) */

ColTileStats coltile_stats;

enum { P_OUT, P_READING, P_IN };
typedef struct {
    float lo[3], hi[3];
    unsigned off, bytes;
    short cell[3];  /* the 32 m map cell (tile key >> 1) */
    unsigned char state;
    int tk;         /* P_READING: its ticket */
    void *buf;      /* P_READING / P_IN: its pool block (gran(bytes)) */
} Page;
typedef struct {
    int planned, on;
    Pack *pk;
    unsigned base, npg;
    Page *pg;
} TBlock;

static TBlock tb[COL_BLOCKS];
static struct { short b, p; } fly[CT_INFLIGHT];  /* reads in flight */
static int nfly, noroom_wait;

static inline float box_dxz2(const Page *p, const float *f)
{
    float dx = f[0] < p->lo[0] ? p->lo[0] - f[0] : f[0] > p->hi[0] ? f[0] - p->hi[0] : 0;
    float dz = f[2] < p->lo[2] ? p->lo[2] - f[2] : f[2] > p->hi[2] ? f[2] - p->hi[2] : 0;
    return dx * dx + dz * dz;
}
static inline float box_dy(const Page *p, const float *f)
{
    return f[1] < p->lo[1] ? p->lo[1] - f[1] : f[1] > p->hi[1] ? f[1] - p->hi[1] : 0;
}
static inline int within(const Page *p, const float *f, float r, float y)
{
    return box_dxz2(p, f) <= r * r && box_dy(p, f) <= y;
}
/* ranking: nearest first, a level above or below counts double */
static inline float rank(const Page *p, const float *f) { return box_dxz2(p, f) + 4.0f * box_dy(p, f) * box_dy(p, f); }
/* bytes a page takes in the arena (mapstream's 256-byte granules) */
static inline unsigned gran(unsigned n) { return (n + 255) & ~255u; }

int coltile_plan(int b, const float os[4], const void *ptab)
{
    const unsigned *t = ptab;
    if (b < 0 || b >= COL_BLOCKS || (memcmp(t, "CTP1", 4) && memcmp(t, "CTP2", 4)) || !t[1] || t[1] > CT_MAX_PAGES)
        return 0;
    TBlock *k = &tb[b];
    free(k->pg);
    memset(k, 0, sizeof *k);
    k->npg = t[1];
    k->pg = calloc(k->npg, sizeof *k->pg);
    if (!k->pg)
        return 0;
    const unsigned char *r = (const unsigned char *)(t + 4);
    for (unsigned i = 0; i < k->npg; i++, r += 32) {  /* collision.c ColPage */
        short key[3], lo[3], hi[3];
        unsigned off, bytes;
        memcpy(key, r, 6), memcpy(&off, r + 12, 4), memcpy(&bytes, r + 16, 4), memcpy(lo, r + 20, 6), memcpy(hi, r + 26, 6);
        Page *p = &k->pg[i];
        for (int a = 0; a < 3; a++)
            p->lo[a] = os[a] + lo[a] * os[3], p->hi[a] = os[a] + hi[a] * os[3], p->cell[a] = key[a] >> 1;
        p->off = off, p->bytes = bytes;
    }
    k->planned = 1;
    return 1;
}

unsigned coltile_budget(void)
{
    unsigned best = 0;
    for (int b = 0; b < COL_BLOCKS; b++)
        for (unsigned i = 0; tb[b].planned && i < tb[b].npg; i++) {
            const Page *c = &tb[b].pg[i];
            float f[3] = {(c->lo[0] + c->hi[0]) * 0.5f, (c->lo[1] + c->hi[1]) * 0.5f, (c->lo[2] + c->hi[2]) * 0.5f};
            unsigned sum = 0;
            for (int o = 0; o < COL_BLOCKS; o++)
                for (unsigned j = 0; tb[o].planned && j < tb[o].npg; j++)
                    if (within(&tb[o].pg[j], f, CT_WANT_R + CT_BUDGET_M, CT_WANT_Y + CT_BUDGET_M))
                        sum += gran(tb[o].pg[j].bytes);
            best = sum > best ? sum : best;
        }
    return best + best / CT_PACK_K;
}

void coltile_on(int b, Pack *pk, unsigned base)
{
    if (b < 0 || b >= COL_BLOCKS || !tb[b].planned || col_pages(b, NULL) != (int)tb[b].npg)
        return;
    tb[b].on = 1, tb[b].pk = pk, tb[b].base = base;
}

static void page_free(int b, int i)
{
    Page *p = &tb[b].pg[i];
    if (p->state == P_IN) {
        col_page_set(b, i, NULL);
        coltile_stats.pages_in--;
    }
    mapstream_free(p->buf, gran(p->bytes));
    coltile_stats.in_kb -= gran(p->bytes) >> 10;
    p->buf = NULL, p->state = P_OUT, p->tk = 0;
}

static void fly_remove(int j)
{
    fly[j] = fly[--nfly];
}

void coltile_off(int b)
{
    if (b < 0 || b >= COL_BLOCKS || !tb[b].on)
        return;
    for (int j = nfly - 1; j >= 0; j--)
        if (fly[j].b == b) {
            pack_req_cancel(tb[b].pg[fly[j].p].tk);  /* returns once nothing more can land in its buffer */
            fly_remove(j);
        }
    for (unsigned i = 0; i < tb[b].npg; i++)
        if (tb[b].pg[i].state != P_OUT)
            page_free(b, (int)i);
    tb[b].on = 0;
}

static void collect(void)
{
    for (int j = nfly - 1; j >= 0; j--) {
        int res, b = fly[j].b, i = fly[j].p;
        Page *p = &tb[b].pg[i];
        if (!pack_req_done(p->tk, &res))
            continue;
        fly_remove(j);
        p->tk = 0;
        if (res != (int)p->bytes) {
            printf("coltile: block %d page %d: read %d of %u bytes\n", b, i, res, p->bytes);
            coltile_stats.failed++;
            page_free(b, i);
            continue;
        }
        InvalidDCache(p->buf, (char *)p->buf + gran(p->bytes) - 1);
        col_page_set(b, i, p->buf);
        p->state = P_IN;
        coltile_stats.pages_in++;
    }
}

/* a wanted page found no room: free the held page outside the want window farthest from the feet; 0 if none */
static int evict(const float *f)
{
    int bb = -1, bi = -1;
    float bd = 0;
    for (int b = 0; b < COL_BLOCKS; b++)
        for (unsigned i = 0; tb[b].on && i < tb[b].npg; i++) {
            const Page *p = &tb[b].pg[i];
            if (p->state != P_IN || within(p, f, CT_WANT_R, CT_WANT_Y))
                continue;
            float d = rank(p, f);
            if (bi < 0 || d > bd)
                bb = b, bi = (int)i, bd = d;
        }
    if (bi < 0)
        return 0;
    page_free(bb, bi);
    coltile_stats.drops++;
    return 1;
}

/* start page i of block b's read; 0: no room or no request slot */
static int start(int b, int i, const float *f, int wanted)
{
    Page *p = &tb[b].pg[i];
    void *buf;
    while (!(buf = mapstream_alloc_pool_small(gran(p->bytes), MS_POOL_STRUCT)))
        if (!wanted || !evict(f)) {
            if (wanted) {
                if (!coltile_stats.noroom++ || !(coltile_stats.noroom & 63))
                    printf("coltile: no room for block %d page %d (%u bytes), %u KB of tiles in, %d pages (%u refusals)\n",
                           b, i, p->bytes, coltile_stats.in_kb, coltile_stats.pages_in, coltile_stats.noroom);
                noroom_wait = CT_RETRY_FRAMES;
            }
            return 0;
        }
    int t = pack_req_read_q(tb[b].pk, tb[b].base + p->off, buf, p->bytes, SNDD_PRIO_HIT);  /* DSR: BackRead_MapHit */
    if (!t) {
        mapstream_free(buf, gran(p->bytes));
        return 0;
    }
    p->buf = buf, p->tk = t, p->state = P_READING;
    fly[nfly].b = b, fly[nfly].p = i, nfly++;
    coltile_stats.reads++;
    coltile_stats.read_kb += p->bytes >> 10;
    coltile_stats.in_kb += gran(p->bytes) >> 10;
    coltile_stats.peak_kb = coltile_stats.in_kb > coltile_stats.peak_kb ? coltile_stats.in_kb : coltile_stats.peak_kb;
    return 1;
}

static void issue(const float *f)
{
    int bb = -1, bp = -1;
    float bd = 0;
    for (int b = 0; b < COL_BLOCKS; b++)
        for (unsigned i = 0; tb[b].on && i < tb[b].npg; i++) {
            const Page *p = &tb[b].pg[i];
            if (p->state != P_OUT || !within(p, f, CT_WANT_R, CT_WANT_Y))
                continue;
            float d = rank(p, f);
            if (bp < 0 || d < bd)
                bb = b, bp = (int)i, bd = d;
        }
    if (bp < 0 || !start(bb, bp, f, 1))
        return;
    /* the absent pages after it in the same cell, within the keep window: they follow on the disc */
    const TBlock *k = &tb[bb];
    for (int i = bp + 1; i < (int)k->npg && nfly < CT_INFLIGHT; i++) {
        const Page *p = &k->pg[i], *q = &k->pg[bp];
        if (p->state != P_OUT || memcmp(p->cell, q->cell, sizeof p->cell) || p->off + p->bytes - q->off > CT_RUN_MAX)
            break;
        int w = within(p, f, CT_WANT_R, CT_WANT_Y);
        if ((!w && !within(p, f, CT_KEEP_R, CT_KEEP_Y)) || !start(bb, i, f, w))
            break;
    }
}

void coltile_step(const float feet[3])
{
    static unsigned fr;
    if (!(++fr % 600) && coltile_stats.reads)  /* every ~20 s of play */
        printf("coltile: %u KB in (%d pages, peak %u KB), %u reads %u KB, %u drops, %u no room, %u failed, %d in flight\n",
               coltile_stats.in_kb, coltile_stats.pages_in, coltile_stats.peak_kb, coltile_stats.reads,
               coltile_stats.read_kb, coltile_stats.drops, coltile_stats.noroom, coltile_stats.failed, nfly);
    collect();
    for (int b = 0; b < COL_BLOCKS; b++)  /* pages past the keep window go (one still reading goes once it lands) */
        for (unsigned i = 0; tb[b].on && i < tb[b].npg; i++)
            if (tb[b].pg[i].state == P_IN && !within(&tb[b].pg[i], feet, CT_KEEP_R, CT_KEEP_Y)) {
                page_free(b, (int)i);
                coltile_stats.drops++;
            }
    if (noroom_wait > 0)
        noroom_wait--;
    else if (!nfly)
        issue(feet);
}

int coltile_under(const float feet[3])
{
    int n = 0;
    for (int b = 0; b < COL_BLOCKS; b++)
        for (unsigned i = 0; tb[b].on && i < tb[b].npg; i++) {
            const Page *p = &tb[b].pg[i];
            n += p->state != P_IN && box_dxz2(p, feet) <= 1.0f && feet[1] >= p->lo[1] - 1.0f &&
                 feet[1] <= p->hi[1] + CT_WANT_Y;
        }
    return n;
}

int coltile_missing(const float feet[3], float r)
{
    int n = 0;
    for (int b = 0; b < COL_BLOCKS; b++)
        for (unsigned i = 0; tb[b].on && i < tb[b].npg; i++)
            n += tb[b].pg[i].state != P_IN && within(&tb[b].pg[i], feet, r, r);
    return noroom_wait > 0 && !nfly ? 0 : n;  /* no room: nothing more comes until something goes */
}
