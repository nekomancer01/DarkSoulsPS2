/* Hitch monitor: see hitch.h */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "hitch.h"

#define HITCH_PERIOD_S 10.0f
#define TPMS 294912u  /* EE ticks per ms */

unsigned hitch_acc[HS_N];
HitchTotals hitch_total;

static float per_s, run_s;
static unsigned n_fr, o50, o100, o200, worst_ms, worst_sec[HS_N], sum_ms;
static unsigned loads, again, again_kb, drops, evicts, evict_kb, flips, flops, blk_reads, blk_kb, read_kb;
/* the cells that flipped most this period */
#define TOPN 4
static struct { int k; unsigned ci, n; } top[TOPN];

static unsigned heap_free(void)
{
    struct mallinfo mi = mallinfo();
    return 32 * 1024 * 1024 - (unsigned)sbrk(0) + mi.fordblks;
}
/* the largest block malloc finds now (bisection of trial allocations, KB granularity) */
static unsigned largest_block(unsigned hi)
{
    unsigned lo = 0;
    hi >>= 10;
    while (lo < hi) {
        unsigned mid = (lo + hi + 1) / 2;
        void *p = malloc((size_t)mid << 10);
        if (p)
            free(p), lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static unsigned thr, thr_kb;
void hitch_load(int a, int t, unsigned bytes)
{
    loads++, read_kb += bytes >> 10;
    if (a)
        again++, again_kb += bytes >> 10, hitch_total.rereads++;
    if (t)
        thr++, thr_kb += bytes >> 10, hitch_total.thrash++;
}
void hitch_drop(int wanted, unsigned bytes)
{
    drops++;
    if (wanted)
        evicts++, evict_kb += bytes >> 10, hitch_total.evicts++;
}
void hitch_flip(int k, unsigned ci, int flop)
{
    flips++, hitch_total.flips++;
    if (flop)
        flops++, hitch_total.flops++;
    int i, lo = 0;
    for (i = 0; i < TOPN; i++) {
        if (top[i].n && top[i].k == k && top[i].ci == ci) {
            top[i].n++;
            return;
        }
        if (top[i].n < top[lo].n)
            lo = i;
    }
    if (!top[lo].n || top[lo].n <= 1)
        top[lo].k = k, top[lo].ci = ci, top[lo].n = 1;
}
void hitch_block_read(unsigned bytes) { blk_reads++, blk_kb += bytes >> 10; }

static int load_fr;
static unsigned n_load, load_max;
void hitch_loadscreen(void) { load_fr = 1; }

void hitch_frame(float dt_real)
{
    unsigned ms = (unsigned)(dt_real * 1000.0f + 0.5f);
    if (load_fr) {  /* behind a loading screen: its own count */
        load_fr = 0;
        n_load++, hitch_total.loads++;
        load_max = ms > load_max ? ms : load_max;
        hitch_total.load_max_ms = ms > hitch_total.load_max_ms ? ms : hitch_total.load_max_ms;
        memset(hitch_acc, 0, sizeof hitch_acc);
        per_s += 0.25f, run_s += 0.25f;
        return;
    }
    n_fr++, sum_ms += ms;
    hitch_total.frames++;
    o50 += ms > 50, o100 += ms > 100, o200 += ms > 200;
    hitch_total.over50 += ms > 50, hitch_total.over100 += ms > 100, hitch_total.over200 += ms > 200;
    if (ms > hitch_total.max_ms && run_s > 20.0f)  /* (not the boot) */
        hitch_total.max_ms = ms;
    if (ms > worst_ms) {
        worst_ms = ms;
        for (int i = 0; i < HS_N; i++)
            worst_sec[i] = hitch_acc[i] / TPMS;
    }
    memset(hitch_acc, 0, sizeof hitch_acc);
    float dt = dt_real > 0.25f ? 0.25f : dt_real;  /* a loading screen's frame counts as one long frame */
    per_s += dt, run_s += dt;
    if (per_s < HITCH_PERIOD_S)
        return;
    unsigned hf = heap_free(), fkb = 0, rkb = 0, okb = 0;
    unsigned lb = largest_block(hf);
    mapstream_free_info(&fkb, &rkb, &okb);
    printf("hitch: t %u s frames %u avg %u max %u ms over50 %u over100 %u over200 %u | worst pre %u world %u (plan %u win "
           "%u io %u vis %u) snd %u map %u logic %u chr %u wait %u flip %u save %u log %u | heap free %u KB largest %u KB | arena "
           "free %u KB run %u KB other %u KB | reads %u (%u KB) again %u (%u KB) thrash %u (%u KB) drops %u evicted %u (%u KB) | blocks %u "
           "(%u KB) | lod flips %u flops %u | loads %u longest %u ms | top",
           (unsigned)run_s, n_fr, n_fr ? sum_ms / n_fr : 0, worst_ms, o50, o100, o200, worst_sec[HS_PRE],
           worst_sec[HS_WORLD], worst_sec[HS_MSPLAN], worst_sec[HS_MSWIN], worst_sec[HS_MSIO], worst_sec[HS_MSVIS], worst_sec[HS_SND],
           worst_sec[HS_MAP], worst_sec[HS_LOGIC], worst_sec[HS_CHR], worst_sec[HS_WAIT], worst_sec[HS_FLIP],
           worst_sec[HS_SAVE], worst_sec[HS_LOG], hf >> 10, lb, fkb, rkb, okb, loads, read_kb, again, again_kb, thr, thr_kb, drops,
           evicts, evict_kb, blk_reads, blk_kb, flips, flops, n_load, load_max);
    for (int i = 0; i < TOPN; i++)
        if (top[i].n > 1)
            printf(" %d.%u x%u", top[i].k, top[i].ci, top[i].n);
    printf("\n");
    per_s = 0;
    n_fr = o50 = o100 = o200 = worst_ms = sum_ms = n_load = load_max = 0;
    loads = again = again_kb = thr = thr_kb = drops = evicts = evict_kb = flips = flops = blk_reads = blk_kb = read_kb = 0;
    memset(top, 0, sizeof top);
}
