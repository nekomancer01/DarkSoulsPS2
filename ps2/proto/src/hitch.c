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


#ifdef MEMPROF
/* Memory-handling profile (make MEMPROF=1, measurement builds only): EE ticks spent in the allocator, mallinfo, the
 * cache maintenance calls and large memcpy / memmove / memset, per frame (loading-screen frames left out), printed
 * as one "mem:" line per hitch period. The linker wraps the functions (Makefile); each timed call costs two mfc0
 * reads (~0.3 us in PCSX2), listed as calls so the overhead can be subtracted. */
enum { MP_ALLOC, MP_FREE, MP_MINFO, MP_PROBE, MP_SYNC, MP_INVAL, MP_FLUSH, MP_CPY, MP_SET, MP_N };
static const char *const mp_name[MP_N] = {"alloc", "free", "mallinfo", "probe", "sync", "inval", "flush", "cpy", "set"};
static unsigned mp_fr[MP_N], mp_frn[MP_N], mp_sum[MP_N], mp_n[MP_N], mp_max1[MP_N], mp_maxfr[MP_N], mp_frames, mp_skip;
static unsigned long long mp_bytes[MP_N];
static inline unsigned mp_t(void) { unsigned c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }
/* per-caller totals over the whole run ("memcall:" lines each period: addr2line on proto.sym.elf) */
#define MP_CALLERS 1024
static struct { unsigned b, ra, n; unsigned long long t, bytes; unsigned max1; } mp_c[MP_CALLERS];
static void mp_caller(int b, unsigned ra, unsigned d, unsigned bytes)
{
    int i;
    for (i = 0; i < MP_CALLERS && mp_c[i].n && !(mp_c[i].b == (unsigned)b && mp_c[i].ra == ra); i++)
        ;
    if (i == MP_CALLERS)
        return;
    mp_c[i].b = b, mp_c[i].ra = ra, mp_c[i].n++, mp_c[i].t += d, mp_c[i].bytes += bytes;
    mp_c[i].max1 = d > mp_c[i].max1 ? d : mp_c[i].max1;
}
#define mp_add(b, t0, bytes) mp_add_(b, t0, bytes, (unsigned)__builtin_return_address(0))
static inline void mp_add_(int b, unsigned t0, unsigned bytes, unsigned ra)
{
    if (mp_skip)
        return;
    unsigned d = mp_t() - t0;
    mp_caller(b, ra, d, bytes);
    mp_fr[b] += d, mp_frn[b]++, mp_bytes[b] += bytes;
    mp_max1[b] = d > mp_max1[b] ? d : mp_max1[b];
}
void __real_free(void *p);
void __wrap_free(void *p) { unsigned t = mp_t(); __real_free(p); mp_add(MP_FREE, t, 0); }
struct mallinfo __real_mallinfo(void);
struct mallinfo __wrap_mallinfo(void) { unsigned t = mp_t(); struct mallinfo m = __real_mallinfo(); mp_add(MP_MINFO, t, 0); return m; }
void __real_SyncDCache(void *a, void *b);
void __wrap_SyncDCache(void *a, void *b) { unsigned t = mp_t(); __real_SyncDCache(a, b); mp_add(MP_SYNC, t, (char *)b - (char *)a + 1); }
void __real_InvalidDCache(void *a, void *b);
void __wrap_InvalidDCache(void *a, void *b) { unsigned t = mp_t(); __real_InvalidDCache(a, b); mp_add(MP_INVAL, t, (char *)b - (char *)a + 1); }
void __real_FlushCache(int op);
void __wrap_FlushCache(int op) { unsigned t = mp_t(); __real_FlushCache(op); mp_add(MP_FLUSH, t, 0); }
/* only the calls of 1 KB or more are timed (the small ones would be mostly timer overhead) */
void *__real_memcpy(void *d, const void *s, size_t n);
void *__wrap_memcpy(void *d, const void *s, size_t n)
{
    if (n < 1024)
        return __real_memcpy(d, s, n);
    unsigned t = mp_t(); void *r = __real_memcpy(d, s, n); mp_add(MP_CPY, t, n); return r;
}
void *__real_memmove(void *d, const void *s, size_t n);
void *__wrap_memmove(void *d, const void *s, size_t n)
{
    if (n < 1024)
        return __real_memmove(d, s, n);
    unsigned t = mp_t(); void *r = __real_memmove(d, s, n); mp_add(MP_CPY, t, n); return r;
}
void *__real_memset(void *d, int c, size_t n);
void *__wrap_memset(void *d, int c, size_t n)
{
    if (n < 1024)
        return __real_memset(d, c, n);
    unsigned t = mp_t(); void *r = __real_memset(d, c, n); mp_add(MP_SET, t, n); return r;
}
#define MP_BEGIN unsigned mp_t0 = mp_t()
#define MP_END(b) mp_add(b, mp_t0, 0)
static void mp_frame(int loading)
{
    for (int b = 0; b < MP_N; b++) {
        if (!loading) {
            mp_sum[b] += mp_fr[b], mp_n[b] += mp_frn[b];
            mp_maxfr[b] = mp_fr[b] > mp_maxfr[b] ? mp_fr[b] : mp_maxfr[b];
        }
        mp_fr[b] = mp_frn[b] = 0;
    }
    mp_frames += !loading;
}
static void mp_line(void)
{
    struct mallinfo mi = __real_mallinfo();
    unsigned tot = 0, totmax = 0;
    printf("mem: t %u s frames %u | free chunks %u heap free %u KB |", (unsigned)run_s, mp_frames, (unsigned)mi.ordblks,
           (unsigned)mi.fordblks >> 10);
    for (int b = 0; b < MP_N; b++) {
        tot += mp_sum[b], totmax += mp_maxfr[b];
        printf(" %s %u us/fr %u calls/fr max1 %u us maxfr %u us %u KB/fr |", mp_name[b],
               mp_frames ? (unsigned)(mp_sum[b] / 294.912f / mp_frames) : 0, mp_frames ? (mp_n[b] + mp_frames / 2) / mp_frames : 0,
               (unsigned)(mp_max1[b] / 294.912f), (unsigned)(mp_maxfr[b] / 294.912f),
               mp_frames ? (unsigned)(mp_bytes[b] / 1024 / mp_frames) : 0);
    }
    printf(" all %u us/fr\n", mp_frames ? (unsigned)(tot / 294.912f / mp_frames) : 0);
    for (int i = 0; i < MP_CALLERS && mp_c[i].n; i++)
        if (mp_c[i].t > 294912u / 10)  /* > 0.1 ms over the run */
            printf("memcall: %s 0x%08x calls %u total %u us max1 %u us %u KB\n", mp_name[mp_c[i].b], mp_c[i].ra, mp_c[i].n,
                   (unsigned)(mp_c[i].t / 295), (unsigned)(mp_c[i].max1 / 295), (unsigned)(mp_c[i].bytes >> 10));
    memset(mp_sum, 0, sizeof mp_sum), memset(mp_n, 0, sizeof mp_n), memset(mp_max1, 0, sizeof mp_max1);
    memset(mp_maxfr, 0, sizeof mp_maxfr), memset(mp_bytes, 0, sizeof mp_bytes);
    mp_frames = 0;
}
#else
#define MP_BEGIN
#define MP_END(b)
#endif

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
/* the streamer's Burg gates (plan step B0): refusals, near due but coarse, horizon inside its radius, pop-ins */
#define DUE_BINS 64
static unsigned lent_near_n;
void hitch_lent_near(void) { lent_near_n++, hitch_total.lent_near++; }
static unsigned room_n, room_near, pop_n, due_hist[DUE_BINS], due_max, due_fr, map_due, map_hz, cut_n, cut_near;
static float hz_s;
/* near items refused this run, distinct (an item is refused again at each retry, 2-3 times in 50 ms: the attempt
 * count swung 2x between identical runs; the B0 gate counts items). Open addressing, (k << 20 | id) + 1; full: counted
 * as new */
#define ROOM_SEEN 4096
static unsigned room_seen[ROOM_SEEN], room_new;
void hitch_room(int near, int k, unsigned id)
{
    room_n++, hitch_total.room++;
    if (!near)
        return;
    room_near++, hitch_total.room_near++;
    unsigned key = ((unsigned)k << 20 | (id & 0xFFFFF)) + 1, h = key * 2654435761u >> 20, n;
    for (n = 0; n < ROOM_SEEN && room_seen[h % ROOM_SEEN] && room_seen[h % ROOM_SEEN] != key; n++, h++)
        ;
    if (n < ROOM_SEEN && room_seen[h % ROOM_SEEN] == key)
        return;
    if (n < ROOM_SEEN)
        room_seen[h % ROOM_SEEN] = key;
    room_new++, hitch_total.room_items++;
}
void hitch_cut(unsigned items, unsigned near)
{
    /* the most one plan of the period cut (plans run every few frames: a sum would count one item many times) */
    if (items > cut_n)
        cut_n = items, cut_near = near;
    hitch_total.cut = items > hitch_total.cut ? items : hitch_total.cut;
}
void hitch_map(unsigned due, unsigned hz_fill, unsigned pops)
{
    map_due = due, map_hz = hz_fill;
    pop_n += pops, hitch_total.popins += pops;
}
static void stream_frame(float dt)
{
    due_hist[map_due < DUE_BINS ? map_due : DUE_BINS - 1]++, due_fr++;
    due_max = map_due > due_max ? map_due : due_max;
    if (map_hz)
        hz_s += dt, hitch_total.hzn_s += dt;
    map_due = map_hz = 0;  /* (a frame without a map draw counts 0) */
}
static void stream_line(void)
{
    unsigned med = 0, acc = 0;
    while (med < DUE_BINS - 1 && (acc += due_hist[med]) * 2 < due_fr)
        med++;
    printf("stream: t %u s room %u near %u items %u | due near drawn coarse median %u max %u | hzn inside %.1f s | "
           "pop-ins %u | lent near dropped %u | plan cut %u near %u\n", (unsigned)run_s, room_n, room_near, room_new,
           due_fr ? med : 0, due_max, (double)hz_s, pop_n, lent_near_n, cut_n, cut_near);
    room_new = 0;
    room_n = room_near = pop_n = due_max = due_fr = lent_near_n = cut_n = cut_near = 0, hz_s = 0;
    memset(due_hist, 0, sizeof due_hist);
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
    MP_BEGIN;
    void *p = __real_malloc(n);
    MP_END(MP_ALLOC);
    if (!p && n)
        alloc_failed("malloc", n, __builtin_return_address(0));
    return p;
}
void *__wrap_calloc(size_t n, size_t s)
{
    MP_BEGIN;
    void *p = __real_calloc(n, s);
    MP_END(MP_ALLOC);
    if (!p && n && s)
        alloc_failed("calloc", n * s, __builtin_return_address(0));
    return p;
}
void *__wrap_realloc(void *q, size_t n)
{
    MP_BEGIN;
    void *p = __real_realloc(q, n);
    MP_END(MP_ALLOC);
    if (!p && n)
        alloc_failed("realloc", n, __builtin_return_address(0));
    return p;
}
void *__wrap_memalign(size_t a, size_t n)
{
    MP_BEGIN;
    void *p = __real_memalign(a, n);
    MP_END(MP_ALLOC);
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
#ifdef MEMPROF
        mp_frame(1);
#endif
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
    stream_frame(dt);
    per_s += dt, run_s += dt;
    if (per_s < HITCH_PERIOD_S) {
#ifdef MEMPROF
        mp_frame(0);
#endif
        return;
    }
    unsigned hf = heap_free(), fkb = 0, rkb = 0, okb = 0;
#ifdef MEMPROF
    unsigned pt0 = mp_t();
    mp_skip = 1;
#endif
    unsigned lb = largest_block(hf);
#ifdef MEMPROF
    mp_skip = 0;
    mp_add(MP_PROBE, pt0, 0);
    mp_frame(0);  /* the probe counted in the period's last frame */
    mp_line();
#endif
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
    stream_line();
    disc_line(per_s, n_load > 0);
    per_s = 0;
    spikes = spikes_cut = af_n = af_kb = 0;
    n_fr = o50 = o100 = o200 = worst_ms = sum_ms = n_load = load_max = 0;
    loads = again = again_kb = thr = thr_kb = drops = evicts = evict_kb = flips = flops = blk_reads = blk_kb = read_kb = 0;
    memset(top, 0, sizeof top);
}
