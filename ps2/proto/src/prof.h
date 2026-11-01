/* EE profiling (tools-hwperf; notes/threads/tools-hwperf.md). Only the PROFILE build (make PROFILE=1 -> proto_prof.elf,
 * main.c compiled with -DPROFILE) has these hooks; in every other build the macros are empty and prof.o is not linked.
 * Per pass and frame: EE cycles (COP0 Count, 294.912 MHz), I-cache and D-cache misses (COP0 performance counters
 * PCR0 / PCR1, EE Core User's Manual 5.6: PCCR EVENT0 = 6 I$ miss, EVENT1 = 6 D$ miss), shown on the HUD and logged
 * every PROF_LOG_FRAMES frames with each pass's budget. PCSX2 counts cycles but not cache misses (they read 0 there);
 * the miss columns are real only on a PS2 (tools/ps2-run.sh). make GPROF=1 adds libprofglue's gprof sampling. */
#ifndef PROF_H
#define PROF_H

/* Passes in frame order; a pass runs from its PROF_MARK to the next mark (or the frame's end) */
enum {
    PROF_INPUT,    /* pad, menu, demo / test input */
    PROF_PLAYER,   /* player_update + combat_player_frame */
    PROF_ENEMY,    /* enemy_update: AI Lua, enemyCommon ESD, movement, sweeps */
    PROF_OBJECTS,  /* objects_update: breakables */
    PROF_CAMERA,   /* orbit camera, camera collision ray */
    PROF_MAP,      /* Z clear + map cull + VU1 chain build + kick */
    PROF_CHARS,    /* player / enemies / objects bone matrices + VU1 skin chains */
    PROF_HUD,      /* hud_draw, debug overlay, menu (gsKit queue) */
    PROF_VU1WAIT,  /* vu1_wait: EE idle until VU1 / PATH1 finishes */
    PROF_FLIP,     /* gsKit_queue_exec + flip (vsync wait) */
    PROF_PASSES
};

#ifdef PROFILE
void prof_mark(int pass);  /* end the running pass, start `pass` */
void prof_frame(void);     /* frame boundary: close the frame, update averages / peaks, log, (GPROF) start / stop */
void prof_hud(void);       /* the per-pass table on screen */
#define PROF_MARK(p) prof_mark(p)
#define PROF_FRAME() prof_frame()
#define PROF_DRAW() prof_hud()
#else
#define PROF_MARK(p) ((void)0)
#define PROF_FRAME() ((void)0)
#define PROF_DRAW() ((void)0)
#endif

#endif
