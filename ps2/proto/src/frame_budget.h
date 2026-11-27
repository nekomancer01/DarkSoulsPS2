/* frame_budget.h: the 30 fps frame budget, the one place its numbers live (PCSX2 emulated time; re-cut on hardware:
 * TECH_DEBT hw-3). Read by prof.c (the HUD's "!" marks), testscript.c (the "TEST passes" line), governor.c and, through
 * a regex over the #define lines, tools/budget.py (the test and CI gate). Keep each value a plain float literal on its
 * own #define line so budget.py can parse it. Pass budgets: the budget doc "Frame time budget (final)", 2026-11-08
 * (map 14 + chr 5 + logic 5 + world 2 + fxhud 1.5 + vuw 0.5 = 28 ms of the 33.37 ms frame: the rest is the GS / DMA
 * tail and timing noise). */
#ifndef FRAME_BUDGET_H
#define FRAME_BUDGET_H

#define FB_FRAME_MS      33.37f  /* 2 NTSC fields (main.c FPS_CAP_FIELDS 2) */
/* a frame at or over this missed its vsync: 2.5 fields, halfway between an on-time frame (2 fields) and one that waited
 * a third. Was 34 ms (budget doc): measured 2026-11-27, on-time frames read 33.4-34.2 ms (the frame is timed at the
 * loop's start, not at the flip), so single 34.1 ms frames failed 42-frame smoke shots at random; frames that missed a
 * vsync read 50.0-50.9 ms and nothing falls between (reports/frametime/baseline.md) */
#define FB_OVER_MS       41.7f
#define FB_OVER_PCT      1.0f    /* most frames over FB_OVER_MS per shot, % */
#define FB_P99_MS        41.7f   /* the frame's p99 per shot: never a second vsync (FB_OVER_MS) */
#define FB_MISS_RUN      1       /* most missed vsyncs in a row per shot */
#define FB_FRAME_MAX_MS  67.0f   /* worst frame outside settles: under 4 fields */
#define FB_GOV_BUSY_MS   28.0f   /* governor: 4 of 8 frames busy over this (= sum of pass budgets) steps up; ASSUMED */
#define FB_GOV_EDGE_MS   31.0f   /* governor: one frame busy over this steps up (2.4 ms short of a miss); ASSUMED */
#define FB_GOV_CALM_MS   24.0f   /* governor: 60 frames under this steps down; ASSUMED */

/* the gated passes (testscript.c pb_name, tools/budget.py PASS_MS): name, ms at p90 per shot */
#define FB_PASSES(X) X(map, 14.0f) X(chr, 5.0f) X(fxhud, 1.5f) X(logic, 5.0f) X(world, 2.0f) X(vuw, 0.5f)

/* prof.c HUD passes (INPUT..CAMERA sum to the logic pass's 5.0 ms) */
#define FB_PROF_INPUT    0.3f
#define FB_PROF_PLAYER   1.2f
#define FB_PROF_ENEMY    2.5f
#define FB_PROF_OBJ      0.5f
#define FB_PROF_CAMERA   0.5f
#define FB_PROF_MAP      14.0f
#define FB_PROF_CHARS    5.0f
#define FB_PROF_HUD      1.5f

#endif
