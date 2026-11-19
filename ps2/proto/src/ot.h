/* ot.h: the order table (Polaris 1.4). Everything that reaches the GS in a frame is an entry here, in bucket order,
 * and the table alone starts the transfers: the VU1 chain kicks, the wait for VU1 / PATH1, gsKit's queue and the
 * flip. tools/ci/gsown.py fails on a vu1_begin / vu1_kick / vu1_wait / r_queue_exec call outside ot.c (legacy
 * exceptions listed there).
 *
 * Buckets, in the order the frame should reach the GS:
 *   SKY PANO HORIZON WORLD   the map pass (map.c, one segment for all of SKY..BLEND until Polaris 4.1 splits map.c)
 *   CHR OBJ                  characters (player, enemies, NPCs), objects (breakables)
 *   ALPHA_TEST BLEND         (no entries of their own yet: the map's are in its segment, the EE path's in gsKit's queue)
 *   FX                       VU1 particles
 *   POST                     the far depth-of-field bands and the flip (main.c flip)
 *   UI                       HUD, menus, the loading overlay
 *
 * Entry kinds:
 *   segment   a piece of a VU1 chain buffer (map 0, characters 1, effects 2), opened with ot_open, closed with ot_close.
 *             Segments of one buffer that are built back to back are contiguous in it and go out in one kick, with no
 *             tag between them: the bytes VIF1 receives are the ones the chain held before the table existed. A run
 *             never spans two buffers today (each buffer is kicked before the next one is opened), so no END tag is
 *             ever rewritten; when a later step needs a run across buffers, it is joined by rewriting each END into a
 *             NEXT to the following segment (no CALL / RET: the VIF1 call stack stays free for prebuilt packets).
 *   deferred  a function run at the entry's turn, after VU1 / PATH1 are done: gsKit's queue (the EE-path triangles,
 *             the effects' gsKit packets, HUD, menus) and the flip. Polaris 1.6 replaces gsKit's queue with segments.
 * Each entry records the buckets it spans. The table keeps today's order: an entry whose first bucket is lower than the
 * one before it is counted ("ot:" line), which is where today's order and the bucket order disagree (gsKit's queue
 * spans BLEND..UI but runs after FX: TECH_DEBT render-18).
 *
 * Segment rules:
 *   1. A segment leaves the paths balanced: no PATH3 mask set, no flush pending, VIF STCYCL / BASE / OFFSET as it
 *      found them or set by the next segment's preamble (vu1_begin writes one at each buffer's start).
 *   2. A segment opened in a buffer that is already open (OBJ after CHR) has no preamble of its own: it inherits the
 *      loaded microprogram, GS registers and constants from the segment before it. These are counted per bucket
 *      ("inherits" in the "ot:" line); the PR lists what each segment assumes on entry.
 *   3. Segment memory follows frame.h's rule: a chain buffer is rebuilt only after the run sent from it was waited on.
 *   4. The table is a fixed array (OT_ENTRIES a frame); one more is a TEST error. */
#ifndef OT_H
#define OT_H

typedef enum {
    OT_SKY, OT_PANO, OT_HORIZON, OT_WORLD, OT_CHR, OT_OBJ, OT_ALPHA_TEST, OT_BLEND, OT_FX, OT_POST, OT_UI, OT_N
} OtBucket;

void ot_frame_begin(void);
/* a VU1 segment spanning buckets first..last, in the chain buffer of `first` */
void ot_open(OtBucket first, OtBucket last);
void ot_close(void);
/* a deferred entry: run(arg) at its turn, once VU1 / PATH1 are done */
void ot_deferred(OtBucket first, OtBucket last, void (*run)(void *), void *arg);
/* a deferred entry running gsKit's queue (r_queue_exec), then (wait_after) the GIF DMA's end */
void ot_queue(OtBucket first, OtBucket last, void *gs, int wait_after);
/* send every closed entry that may go now, in table order (stops at an open segment) */
void ot_flush(void);
/* wait for VU1 / PATH1 (the last kicked run) */
void ot_wait(void);
#ifdef OT_HASH
/* Polaris 1.4 gate (debug builds only): on frozen test frames every other frame goes through the old sequence (the
 * begin / kick / wait calls main.c made before the table), so two frames of the same state compare old and new in one
 * run. Removed with Polaris 1.6 */
extern int ot_ab_old;
void ot_ab_frame(int frozen);
void ot_old_begin(int buffer);
void ot_old_kick(void);
void ot_old_wait(void);
void ot_old_queue(void *gs);
#endif
/* one "ot:" line (hitch.c, each period) */
void ot_log(void);

#endif
