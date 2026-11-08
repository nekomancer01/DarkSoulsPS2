/* Hitch monitor: see hitch.h */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "hitch.h"
#include "mapstream.h"
#include "pack.h"
#include "dsrsnd_rpc.h"

#define HITCH_PERIOD_S 10.0f
#define TPMS 294912u  /* EE ticks per ms */
#define HITCH_SPIKE_MS 150   /* a single frame this long (outside a loading screen) gets its own "spike:" line */
#define HITCH_SPIKES_MAX 6   /* "spike:" lines a period at most (the rest only counted) */

/* the real allocators (-Wl,--wrap=malloc,...: the wrappers below count the failures) */
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t s);
void *__real_realloc(void *p, size_t n);
void *__real_memalign(size_t a, size_t n);

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
        void *p = __real_malloc((size_t)mid << 10);  /* probes: not counted as failures */
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

static float where[3];
static int where_blk = -1;
void hitch_where(const float *pos, int block) { where[0] = pos[0], where[1] = pos[1], where[2] = pos[2], where_blk = block; }

/* allocation failures: count, the largest request, the first one from each caller logged */
#define AF_CALLERS 32
static unsigned af_n, af_kb, af_caller[AF_CALLERS], af_ncaller;
static int af_busy;
static void alloc_failed(const char *fn, unsigned n, void *caller)
{
    af_n++, hitch_total.allocfail++;
    af_kb = (n >> 10) > af_kb ? n >> 10 : af_kb;
    if (af_busy)  /* (a failure inside the printf below) */
        return;
    unsigned c = (unsigned)caller, i;
    for (i = 0; i < af_ncaller && af_caller[i] != c; i++)
        ;
    if (i < af_ncaller || af_ncaller == AF_CALLERS)  /* seen (or the table is full): counted only */
        return;
    af_caller[af_ncaller++] = c;
    af_busy = 1;
    unsigned hf = heap_free();
    printf("alloc: %s %u B FAILED, caller 0x%08x, heap free %u KB, at %.1f %.1f %.1f block %d, t %u s (first from this "
           "caller; later ones counted in hitch lines)\n", fn, n, c, hf >> 10, where[0], where[1], where[2], where_blk,
           (unsigned)run_s);
    af_busy = 0;
}
void *__wrap_malloc(size_t n)
{
    void *p = __real_malloc(n);
    if (!p && n)
        alloc_failed("malloc", n, __builtin_return_address(0));
    return p;
}
void *__wrap_calloc(size_t n, size_t s)
{
    void *p = __real_calloc(n, s);
    if (!p && n && s)
        alloc_failed("calloc", n * s, __builtin_return_address(0));
    return p;
}
void *__wrap_realloc(void *q, size_t n)
{
    void *p = __real_realloc(q, n);
    if (!p && n)
        alloc_failed("realloc", n, __builtin_return_address(0));
    return p;
}
void *__wrap_memalign(size_t a, size_t n)
{
    void *p = __real_memalign(a, n);
    if (!p && n)
        alloc_failed("memalign", n, __builtin_return_address(0));
    return p;
}

/* the drive over the period (dsrsnd's counters, ISO runs only): read rate, seeks, EE read latency, layer changes, page
 * cache. A layer change in a period with a loading screen counts for the load (budget doc: none during play) */
static const char *media_name(unsigned m) { return m == SNDM_DVD9 ? "dvd9" : m == SNDM_DVD ? "dvd" : m == SNDM_CD ? "cd" : "-"; }
static void disc_line(float secs, int loading)
{
    static SndDiscStat prev;
    static int have;
    SndDiscStat d;
    int ok = pack_disc_stats(&d);
    pack_disc_poll();  /* fresh counters for the next period */
    if (!ok)
        return;
    if (have && secs > 0) {
        unsigned lc = d.layer_changes - prev.layer_changes, n = d.lat_n - prev.lat_n;
        if (!loading)
            hitch_total.layer_play += lc;
        printf("disc: t %u s media %s kbps %u seeks %u far %u (%.2f/s) layer %u%s | ee reads %u wait avg %u max %u ms | "
               "stream wait max %u ms over100 %u | cache %u KB hits %u miss %u | busy %u %% | iop free %u KB\n",
               (unsigned)run_s, media_name(d.media), (unsigned)((d.drive_kb - prev.drive_kb) / secs),
               d.seeks - prev.seeks, d.far_seeks - prev.far_seeks, (d.seeks - prev.seeks) / secs, lc,
               loading ? " (load)" : "", n, n ? (d.lat_ms - prev.lat_ms) / n : 0, d.lat_max, d.wait_max_ms, d.waits_100,
               d.cache_kb, d.cache_hits - prev.cache_hits, d.cache_miss - prev.cache_miss,
               (unsigned)((d.drive_ms - prev.drive_ms) / (10.0f * secs)), d.iop_free >> 10);
    }
    prev = d, have = 1;
}

static unsigned spikes, spikes_cut;
static void spike(unsigned ms)
{
    if (spikes >= HITCH_SPIKES_MAX) {
        spikes_cut++;
        return;
    }
    spikes++;
    printf("spike: t %u s %u ms | pre %u world %u (plan %u win %u io %u vis %u) snd %u map %u logic %u chr %u wait %u "
           "flip %u save %u log %u | at %.1f %.1f %.1f block %d\n",
           (unsigned)run_s, ms, hitch_acc[HS_PRE] / TPMS, hitch_acc[HS_WORLD] / TPMS, hitch_acc[HS_MSPLAN] / TPMS,
           hitch_acc[HS_MSWIN] / TPMS, hitch_acc[HS_MSIO] / TPMS, hitch_acc[HS_MSVIS] / TPMS, hitch_acc[HS_SND] / TPMS,
           hitch_acc[HS_MAP] / TPMS, hitch_acc[HS_LOGIC] / TPMS, hitch_acc[HS_CHR] / TPMS, hitch_acc[HS_WAIT] / TPMS,
           hitch_acc[HS_FLIP] / TPMS, hitch_acc[HS_SAVE] / TPMS, hitch_acc[HS_LOG] / TPMS, where[0], where[1], where[2],
           where_blk);
}

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
    if (ms >= HITCH_SPIKE_MS && run_s > 20.0f)  /* (not the boot) */
        spike(ms);
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
           "free %u KB run %u KB other %u KB lent %u KB | reads %u (%u KB) again %u (%u KB) thrash %u (%u KB) drops %u evicted %u (%u KB) | blocks %u "
           "(%u KB) | lod flips %u flops %u | loads %u longest %u ms | spikes %u (+%u) | allocfail %u (max %u KB) | at %.0f %.0f "
           "%.0f block %d | top",
           (unsigned)run_s, n_fr, n_fr ? sum_ms / n_fr : 0, worst_ms, o50, o100, o200, worst_sec[HS_PRE],
           worst_sec[HS_WORLD], worst_sec[HS_MSPLAN], worst_sec[HS_MSWIN], worst_sec[HS_MSIO], worst_sec[HS_MSVIS], worst_sec[HS_SND],
           worst_sec[HS_MAP], worst_sec[HS_LOGIC], worst_sec[HS_CHR], worst_sec[HS_WAIT], worst_sec[HS_FLIP],
           worst_sec[HS_SAVE], worst_sec[HS_LOG], hf >> 10, lb, fkb, rkb, okb, mapstream_stats.lent_kb, loads, read_kb, again, again_kb, thr, thr_kb, drops,
           evicts, evict_kb, blk_reads, blk_kb, flips, flops, n_load, load_max, spikes, spikes_cut, af_n, af_kb, where[0], where[1], where[2],
           where_blk);
    for (int i = 0; i < TOPN; i++)
        if (top[i].n > 1)
            printf(" %d.%u x%u", top[i].k, top[i].ci, top[i].n);
    printf("\n");
    disc_line(per_s, n_load > 0);
    per_s = 0;
    spikes = spikes_cut = af_n = af_kb = 0;
    n_fr = o50 = o100 = o200 = worst_ms = sum_ms = n_load = load_max = 0;
    loads = again = again_kb = thr = thr_kb = drops = evicts = evict_kb = flips = flops = blk_reads = blk_kb = read_kb = 0;
    memset(top, 0, sizeof top);
}
