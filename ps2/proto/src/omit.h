/* Character update omission (CHR_OMIT): characters that matter less this frame update every few frames, with the
 * skipped frames' time handed to the update that runs. DSR does this per ChrIns (FUN_14036e8d0, once a frame over the
 * active characters; level at ChrIns +0x164, bit 0 of +0x2a5 = forced by the event command ForceSetOmissionLevel,
 * FUN_1404af3b0):
 *   -1   inactive (not drawn / not loaded)
 *    0   full: in view and among the nearest OMIT_FULL_N in view (the camera's sort, FUN_140373ba0), or a character
 *        the exe marks through ChrIns vtable +0x150 (always 0; ASSUMED here: battle with the player, attacking, hit)
 *    2   in view, past the nearest OMIT_FULL_N; kept at 2 past OMIT_MID_N when inside the near sphere (flag 0x10)
 *    5   in view past the nearest OMIT_MID_N; out of view but within OMIT_NEAR_M of the camera, or marked through
 *        vtable +0x148 (ASSUMED here: AI caution / find)
 *   30   out of view and far
 * The level is the frames left out between two updates (update every level + 1 frames). The three numbers the exe
 * reads from its settings object (DAT_141c77e88 +0x2c, +0x30, +0x34) sit in Arxan-encrypted code (constructor
 * FUN_140374230): the values below are ASSUMED (TECH_DEBT enemy-24), sized to this port's ACTIVE_MAX / DRAW_MAX 16. */
#ifndef OMIT_H
#define OMIT_H

#ifndef CHR_OMIT
#define CHR_OMIT 1  /* 0 = every active character updates every frame (the old path) */
#endif
#ifndef OMIT_FULL_N
#define OMIT_FULL_N 6   /* in-view characters at level 0 (exe +0x2c) */
#endif
#ifndef OMIT_MID_N
#define OMIT_MID_N 10   /* in-view characters at level 2 or better (exe +0x30) */
#endif
#ifndef OMIT_NEAR_M
#define OMIT_NEAR_M 12.0f  /* out of view within this: level 5, not 30 (exe +0x34); also the near sphere for in view */
#endif
#ifndef OMIT_DT_MAX
#define OMIT_DT_MAX 0.25f  /* longest step handed to one update: the rest of a level-30 wait is dropped (out of view,
                            * far: its clock runs slow instead of one 1 s step through collision and falls) */
#endif

enum { OMIT_OFF = -1, OMIT_FULL = 0 };

typedef struct {
    signed char level;  /* this frame's level */
    unsigned char now;  /* moved to a better level: updates this frame */
    float dt;           /* time gathered since the last update */
} Omit;

/* what omit_rank needs to know about one active character this frame */
typedef struct {
    Omit *o;
    float cam_d2;      /* squared distance to the camera */
    unsigned char in_view, force_full, alert;
} OmitIn;

/* Levels for this frame's active characters (n of them; in[] is reordered) */
void omit_rank(OmitIn *in, int n);
/* Gathers dt; 1 when the character updates this frame, with the time to step in *run_dt. A character at level L
 * updates on the frames where (frame + phase) is a multiple of L + 1: phase (its slot index) spreads characters at
 * one level over different frames. */
int omit_step(Omit *o, float dt, unsigned frame, unsigned phase, float *run_dt);
/* A character that left the active set: its gathered time is dropped, the next update is at once */
void omit_reset(Omit *o);

/* Totals for the "omit:" log line (enemy.c): character updates run / left out since the last line */
extern unsigned omit_ran, omit_skipped, omit_lv[4];

#endif
