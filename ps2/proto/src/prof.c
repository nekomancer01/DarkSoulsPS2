/* EE profiling: per-pass cycles and cache misses from COP0 (see prof.h). Linked only into make PROFILE=1 / GPROF=1. */
#include <stdio.h>
#include "prof.h"
#include "render.h"
#ifdef GPROF
#include <ps2prof.h>
#endif

#ifndef PROF_LOG_FRAMES
#define PROF_LOG_FRAMES 300  /* log line every 5 s at 60 fps */
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

static const char *const names[PROF_PASSES] = {"INPUT", "PLAYER", "ENEMY", "OBJ", "CAMERA", "MAP", "CHARS", "HUD",
                                               "VU1WAIT", "FLIP"};
/* Per-pass EE budget at 60 fps (ms; 16.67 per frame). ASSUMED: first split of the frame by the tools-hwperf thread
 * from the PCSX2 HUD timings (MAP / CHR / VUW ms) of 2026-10-31, to be re-cut from real-PS2 numbers (TECH_DEBT hw-3).
 * VU1WAIT and FLIP are idle time, budget 0 = not checked. */
static const float budget_ms[PROF_PASSES] = {0.3f, 1.0f, 3.0f, 0.5f, 0.5f, 4.0f, 3.0f, 1.5f, 0, 0};

typedef struct { unsigned cyc, im, dm; } Sample;
static Sample cur[PROF_PASSES], peak[PROF_PASSES], frame_peak;
static float avg_cyc[PROF_PASSES], avg_im[PROF_PASSES], avg_dm[PROF_PASSES], avg_frame;
static unsigned over[PROF_PASSES], frames, mark_c, mark_i, mark_d, frame_c;
static int running = -1;

static inline unsigned count(void)
{
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
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
    unsigned c = count(), i = pcr0(), d = pcr1();
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
    unsigned now = count();
    if (frames) {
        unsigned fc = now - frame_c;
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
        printf("PROF frame %u: %d.%02d ms avg, %d.%02d peak (budget 16.67)\n", frames + 1, (int)ms(avg_frame),
               (int)(ms(avg_frame) * 100) % 100, (int)ms(frame_peak.cyc), (int)(ms(frame_peak.cyc) * 100) % 100);
        for (int p = 0; p < PROF_PASSES; p++) {
            float a = ms(avg_cyc[p]), k = ms(peak[p].cyc), b = budget_ms[p];
            printf("PROF %-7s avg %2d.%02d ms peak %2d.%02d budget %2d.%01d over %3u/%u  I$miss %6u D$miss %6u%s\n",
                   names[p], (int)a, (int)(a * 100) % 100, (int)k, (int)(k * 100) % 100, (int)b, (int)(b * 10 + 0.5f) % 10,
                   over[p], PROF_LOG_FRAMES, (unsigned)avg_im[p], (unsigned)avg_dm[p],
                   b > 0 && a > b ? "  OVER" : "");
            over[p] = 0;
            peak[p] = (Sample){0, 0, 0};
        }
        frame_peak.cyc = 0;
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
    counters_reset();
    frame_c = count();
    mark_c = frame_c, mark_i = 0, mark_d = 0;
    running = PROF_INPUT;
}

void prof_hud(void)
{
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
}
