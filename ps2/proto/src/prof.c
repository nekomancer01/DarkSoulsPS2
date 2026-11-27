/* EE profiling: per-pass cycles and cache misses from COP0 (see prof.h). Linked only into make PROFILE=1 / GPROF=1. */
#include <stdio.h>
#include <string.h>
#include "prof.h"
#include "frame_budget.h"
#include "render.h"
#include "ticks.h"
#include "lua0.h"
#include "sound.h"
#include "loading.h"
#ifdef GPROF
#include <ps2prof.h>
#endif

#include "dsrsnd_rpc.h"
#define SND_STREAMS_SPU_BYTES (SND_STREAMS * 2 * 2 * SND_STREAM_CHUNK_MAX)  /* sound.c SPU_STREAMS0: the rings */
void ai_prof_log(void);  /* ai.c (PROFILE): lua0 instructions per battle goal script this interval */

#ifndef PROF_LOG_FRAMES
#define PROF_LOG_FRAMES 300  /* log line every 10 s at 30 fps */
#endif
#ifndef PROF_GPROF_START
#define PROF_GPROF_START 120  /* GPROF: frames after the first loop frame before sampling starts (load excluded) */
#endif
#ifndef PROF_GPROF_FRAMES
#define PROF_GPROF_FRAMES 1800  /* GPROF: frames sampled, then host:gmon.out is written */
#endif

/* PCCR: count in user, supervisor, kernel and EXL mode (bits 1-4 / 11-14), EVENT0 bits 5-9, EVENT1 bits 15-19, CTE bit 31 */
#define PCCR_EV0(e) (0x1Eu | (e) << 5)
#define PCCR_EV1(e) (0x1Eu << 10 | (e) << 15)
#define EV0_ICACHE_MISS 6
#define EV1_DCACHE_MISS 6

static const char *const names[PROF_PASSES + 1] = {"INPUT", "PLAYER", "ENEMY", "OBJ", "CAMERA", "STREAM", "SOUND", "MAP",
                                                   "CHARS", "HUD", "VU1WAIT", "FLIP", "FRAME"};
/* Per-pass EE budget at 30 fps (ms; frame_budget.h FB_PROF_*: the gated passes of tools/budget.py, logic split over
 * INPUT..CAMERA), PCSX2 emulated time, to be re-cut from real-PS2 numbers (TECH_DEBT hw-3). STREAM / SOUND are part
 * of the world pass (not split here), VU1WAIT and FLIP are idle time: budget 0 = not checked. */
static const float budget_ms[PROF_PASSES] = {FB_PROF_INPUT, FB_PROF_PLAYER, FB_PROF_ENEMY, FB_PROF_OBJ,
                                             FB_PROF_CAMERA, 0, 0, FB_PROF_MAP, FB_PROF_CHARS, FB_PROF_HUD, 0, 0};

typedef struct { unsigned cyc, im, dm; } Sample;
static Sample cur[PROF_PASSES], peak[PROF_PASSES], frame_peak;
static float avg_cyc[PROF_PASSES], avg_im[PROF_PASSES], avg_dm[PROF_PASSES], avg_frame;
static unsigned over[PROF_PASSES], frames, mark_c, mark_i, mark_d, frame_c;
static int running = -1;

/* Polaris census: per-pass and whole-frame time histograms (0.1 ms bins up to HB_MS, one overflow bin; exact maxima),
 * per log interval and over the counted frames of a test run (prof_gate), and lua0 instructions per frame (LB_STEP
 * bins). Percentiles are the bin's upper edge: p50 0.4 = half the frames took at most 0.4 ms. Frames with a loading
 * screen (loading_stats moved) stay out of the run's totals; prof_hud's own drawing is taken out of the HUD pass. */
#define HB_MS 50
#define HB_N (HB_MS * 10 + 1)
#define LB_STEP 10
#define LB_N 1001
#define PROF_SUM_LOGS 6  /* a cumulative PROFSUM block every 6 log lines (30 s), and at the script's end */
enum { PH_FRAME = PROF_PASSES, PH_N };
static unsigned short hi_iv[PH_N][HB_N], lh_iv[LB_N];
static unsigned hi_tot[PH_N][HB_N], lh_tot[LB_N], max_iv[PH_N], max_tot[PH_N], lmax_iv, lmax_tot, n_iv, n_tot, logs;
static unsigned long long sum_tot[PH_N], lsum_tot;
static unsigned lua_last, load_last, hud_self;
static int gate_on, gate_state;

static int hbin(unsigned cyc)
{
    unsigned b = cyc / 29491u;  /* 0.1 ms */
    return b < HB_N - 1 ? (int)b : HB_N - 1;
}
static void hadd(int h, unsigned cyc, int tot)
{
    int b = hbin(cyc);
    hi_iv[h][b]++;
    max_iv[h] = cyc > max_iv[h] ? cyc : max_iv[h];
    if (tot)
        hi_tot[h][b]++, sum_tot[h] += cyc, max_tot[h] = cyc > max_tot[h] ? cyc : max_tot[h];
}
/* the bin holding the q-th fraction of n samples (q in 1/100), as its upper edge in bin units */
static unsigned pct_s(const unsigned short *hh, int nb, unsigned n, unsigned q)
{
    unsigned want = (n * q + 99) / 100, acc = 0;
    int b = 0;
    for (; b < nb - 1 && (acc += hh[b]) < want; b++)
        ;
    return (unsigned)b + 1;
}
static unsigned pct_u(const unsigned *hh, int nb, unsigned n, unsigned q)
{
    unsigned want = (n * q + 99) / 100, acc = 0;
    int b = 0;
    for (; b < nb - 1 && (acc += hh[b]) < want; b++)
        ;
    return (unsigned)b + 1;
}
static void sum_block(const char *why)
{
    extern unsigned lua0_pool_used, lua0_malloc_live;
    printf("PROFSUM %s: %u frames counted (test script running, not frozen)\n", why, n_tot);
    if (!n_tot)
        return;
    for (int h = 0; h < PH_N; h++) {
        unsigned p50 = pct_u(hi_tot[h], HB_N, n_tot, 50), p90 = pct_u(hi_tot[h], HB_N, n_tot, 90);
        unsigned p99 = pct_u(hi_tot[h], HB_N, n_tot, 99);
        unsigned mx = max_tot[h] / 2949u, av = (unsigned)(sum_tot[h] / n_tot / 295u);  /* 0.01 ms, 0.001 ms */
        printf("PROFSUM %-7s p50 %2u.%u p90 %2u.%u p99 %2u.%u max %3u.%02u avg %2u.%03u ms%s\n", names[h], p50 / 10, p50 % 10,
               p90 / 10, p90 % 10, p99 / 10, p99 % 10, mx / 100, mx % 100, av / 1000, av % 1000,
               p99 >= HB_N ? " (p99 past the 50 ms bins)" : "");
    }
    unsigned l50 = pct_u(lh_tot, LB_N, n_tot, 50), l90 = pct_u(lh_tot, LB_N, n_tot, 90);
    printf("PROFSUM lua0 instr/frame p50 <=%u p90 <=%u max %u avg %u | lua heap %u KB pool %u KB malloc %u KB | spu2 "
           "banks %u KB used %u KB free (hole %u KB), streams %u KB, %u evict %u noroom %u retry\n", l50 * LB_STEP, l90 * LB_STEP, lmax_tot,
           (unsigned)(lsum_tot / n_tot), lua0_heap >> 10, lua0_pool_used >> 10, lua0_malloc_live >> 10,
           snd_stats.spu_used >> 10, snd_stats.spu_free >> 10, snd_stats.spu_hole >> 10, (SND_STREAMS_SPU_BYTES) >> 10,
           snd_stats.evicted, snd_stats.no_room, snd_stats.retried);
}

void prof_gate(int state, int frozen)
{
    if (gate_state != 2 && state == 2)
        sum_block("end");
    gate_state = state;
    gate_on = state == 1 && !frozen;
}

static inline unsigned pcr0(void)
{
    unsigned c;
    __asm__ volatile("mfpc %0, 0" : "=r"(c));
    return c;
}
static inline unsigned pcr1(void)
{
    unsigned c;
    __asm__ volatile("mfpc %0, 1" : "=r"(c));
    return c;
}
/* Stop, zero and restart both counters (they are 31 bits wide and raise a level-2 exception on bit 31; a frame's
 * misses stay far below that) */
static void counters_reset(void)
{
    unsigned z = 0, on = 0x80000000u | PCCR_EV0(EV0_ICACHE_MISS) | PCCR_EV1(EV1_DCACHE_MISS);
    __asm__ volatile("mtps %0, 0\n\tsync.p\n\tmtpc %0, 0\n\tmtpc %0, 1\n\tsync.p\n\tmtps %1, 0\n\tsync.p"
                     :: "r"(z), "r"(on));
}

void prof_mark(int pass)
{
    unsigned c = ticks(), i = pcr0(), d = pcr1();
    if (running >= 0) {
        cur[running].cyc += c - mark_c;
        cur[running].im += i - mark_i;
        cur[running].dm += d - mark_d;
    }
    running = pass, mark_c = c, mark_i = i, mark_d = d;
}

static float ms(float cyc) { return cyc / 294912.0f; }

void prof_frame(void)
{
    prof_mark(-1);
    unsigned now = ticks(), ld = loading_stats.frames + loading_stats.opens;
    int count = gate_on && ld == load_last;  /* (not a loading-screen frame) */
    load_last = ld;
    cur[PROF_HUD].cyc -= hud_self < cur[PROF_HUD].cyc ? hud_self : cur[PROF_HUD].cyc;  /* the PROF table itself */
    hud_self = 0;
    if (frames) {
        unsigned fc = now - frame_c, li = lua0_instructions - lua_last, lb = li / LB_STEP;
        lb = lb < LB_N - 1 ? lb : LB_N - 1;
        lh_iv[lb]++, lmax_iv = li > lmax_iv ? li : lmax_iv, n_iv++;
        if (count)
            lh_tot[lb]++, lsum_tot += li, lmax_tot = li > lmax_tot ? li : lmax_tot, n_tot++;
        hadd(PH_FRAME, fc, count);
        for (int p = 0; p < PROF_PASSES; p++)
            hadd(p, cur[p].cyc, count);
        avg_frame += (fc - avg_frame) * 0.05f;
        if (fc > frame_peak.cyc)
            frame_peak.cyc = fc;
        for (int p = 0; p < PROF_PASSES; p++) {
            avg_cyc[p] += (cur[p].cyc - avg_cyc[p]) * 0.05f;
            avg_im[p] += (cur[p].im - avg_im[p]) * 0.05f;
            avg_dm[p] += (cur[p].dm - avg_dm[p]) * 0.05f;
            if (cur[p].cyc > peak[p].cyc)
                peak[p] = cur[p];
            if (budget_ms[p] > 0 && ms(cur[p].cyc) > budget_ms[p])
                over[p]++;
        }
    }
    if (frames % PROF_LOG_FRAMES == PROF_LOG_FRAMES - 1) {
        printf("PROF frame %u: %d.%02d ms avg, %d.%02d peak (budget %d.%02d)\n", frames + 1, (int)ms(avg_frame),
               (int)(ms(avg_frame) * 100) % 100, (int)ms(frame_peak.cyc), (int)(ms(frame_peak.cyc) * 100) % 100,
               (int)FB_FRAME_MS, (int)(FB_FRAME_MS * 100 + 0.5f) % 100);
        for (int p = 0; p < PROF_PASSES; p++) {
            float a = ms(avg_cyc[p]), k = ms(peak[p].cyc), b = budget_ms[p];
            printf("PROF %-7s avg %2d.%02d ms peak %2d.%02d budget %2d.%01d over %3u/%u  I$miss %6u D$miss %6u%s\n",
                   names[p], (int)a, (int)(a * 100) % 100, (int)k, (int)(k * 100) % 100, (int)b, (int)(b * 10 + 0.5f) % 10,
                   over[p], PROF_LOG_FRAMES, (unsigned)avg_im[p], (unsigned)avg_dm[p],
                   b > 0 && a > b ? "  OVER" : "");
            over[p] = 0;
            peak[p] = (Sample){0, 0, 0};
        }
        {   /* Polaris 1.3: the frame phases (src/frame.h) as sums of the passes above, averages in ms */
            float ph[6];
            ph[0] = ms(avg_cyc[PROF_INPUT] + avg_cyc[PROF_PLAYER] + avg_cyc[PROF_CAMERA]);  /* input + simulate A */
            ph[1] = ms(avg_cyc[PROF_STREAM] + avg_cyc[PROF_SOUND]);                      /* world */
            ph[2] = ms(avg_cyc[PROF_MAP]);                                               /* emit map */
            ph[3] = ms(avg_cyc[PROF_ENEMY] + avg_cyc[PROF_OBJECTS]);  /* simulate B (+ enemy type streaming) */
            ph[4] = ms(avg_cyc[PROF_CHARS] + avg_cyc[PROF_HUD]);                         /* emit */
            ph[5] = ms(avg_cyc[PROF_VU1WAIT] + avg_cyc[PROF_FLIP]);                      /* present */
            printf("PROF phases avg ms: sim_a %d.%02d world %d.%02d emit_map %d.%02d sim_b %d.%02d emit %d.%02d present "
                   "%d.%02d\n", (int)ph[0], (int)(ph[0] * 100) % 100, (int)ph[1], (int)(ph[1] * 100) % 100, (int)ph[2],
                   (int)(ph[2] * 100) % 100, (int)ph[3], (int)(ph[3] * 100) % 100, (int)ph[4], (int)(ph[4] * 100) % 100,
                   (int)ph[5], (int)(ph[5] * 100) % 100);
        }
        frame_peak.cyc = 0;
        /* Polaris census: this interval's percentiles per pass and for the whole frame, lua0, SPU2 */
        if (n_iv) {
            for (int h = 0; h < PH_N; h++) {
                unsigned p50 = pct_s(hi_iv[h], HB_N, n_iv, 50), p90 = pct_s(hi_iv[h], HB_N, n_iv, 90);
                unsigned mx = max_iv[h] / 2949u;  /* 0.01 ms */
                printf("PROFQ %-7s p50 %2u.%u p90 %2u.%u max %3u.%02u ms\n", names[h], p50 / 10, p50 % 10, p90 / 10, p90 % 10,
                       mx / 100, mx % 100);
            }
            unsigned l50 = pct_s(lh_iv, LB_N, n_iv, 50), l90 = pct_s(lh_iv, LB_N, n_iv, 90);
            printf("PROFQ lua0 instr/frame p50 <=%u p90 <=%u max %u | lua heap %u KB | spu2 banks %u KB used %u KB free, "
                   "hole %u KB, %u banks, %u streams, %u evict %u noroom\n", l50 * LB_STEP, l90 * LB_STEP, lmax_iv, lua0_heap >> 10,
                   snd_stats.spu_used >> 10, snd_stats.spu_free >> 10, snd_stats.spu_hole >> 10, snd_stats.banks,
                   snd_stats.streams, snd_stats.evicted, snd_stats.no_room);
            ai_prof_log();
        }
        memset(hi_iv, 0, sizeof hi_iv), memset(max_iv, 0, sizeof max_iv), memset(lh_iv, 0, sizeof lh_iv);
        lmax_iv = n_iv = 0;
        if (++logs % PROF_SUM_LOGS == 0)
            sum_block("so far");
    }
#ifdef GPROF
    if (frames == PROF_GPROF_START) {
        gprof_start();
        printf("PROF gprof: sampling %d frames\n", PROF_GPROF_FRAMES);
    } else if (frames == PROF_GPROF_START + PROF_GPROF_FRAMES) {
        gprof_stop("host:gmon.out", 1);
        printf("PROF gprof: wrote host:gmon.out\n");
    }
#endif
    for (int p = 0; p < PROF_PASSES; p++)
        cur[p] = (Sample){0, 0, 0};
    frames++;
    lua_last = lua0_instructions;
    counters_reset();
    frame_c = ticks();
    mark_c = frame_c, mark_i = 0, mark_d = 0;
    running = PROF_INPUT;
}

void prof_hud(void)
{
    unsigned t0 = ticks();
    char buf[48];
    float y = 40;
    snprintf(buf, sizeof buf, "PROF %d.%02d MS", (int)ms(avg_frame), (int)(ms(avg_frame) * 100) % 100);
    r_text_outlined(380, y, 1.5f, buf);
    for (int p = 0; p < PROF_PASSES; p++) {
        float a = ms(avg_cyc[p]);
        y += 13;
        snprintf(buf, sizeof buf, "%-7s %2d.%02d I%uK D%uK%s", names[p], (int)a, (int)(a * 100) % 100,
                 (unsigned)avg_im[p] >> 10, (unsigned)avg_dm[p] >> 10, budget_ms[p] > 0 && a > budget_ms[p] ? " !" : "");
        r_text_outlined(380, y, 1.5f, buf);
    }
    hud_self += ticks() - t0;
}
