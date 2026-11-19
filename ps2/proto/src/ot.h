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
 *   2D        a g2d stream (Polaris 1.6): WORLD (the EE-path triangles of map and effects, debug lines; BLEND..FX) and UI
 *             (HUD, menus, dialogs, text; UI), each a GIF chain sent at its turn (PATH3) once VU1 / PATH1 are done.
 *   deferred  a function run at the entry's turn, after VU1 / PATH1 are done: the flip with the far depth of field.
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
/* a 2D segment (g2d.h stream, Polaris 1.6): its GIF chain sent at its turn, after VU1 / PATH1, the GIF channel idle */
void ot_g2d(OtBucket first, OtBucket last, int stream);
/* gsKit's draw queue, unused since 1.6: run whatever is in it (TEST warn once); frame.c, before the flip */
void ot_gskit_leftover(void *gs);
/* send every closed entry that may go now, in table order (stops at an open segment) */
void ot_flush(void);
/* wait for VU1 / PATH1 (the last kicked run) */
void ot_wait(void);
#ifdef OT_HASH
/* the draw-trace hashes ("othash:" lines, vu1.c / gstrace.c) of this frame; frozen: a test shot is being taken */
void ot_hash_begin(int frozen);
#endif
/* one "ot:" line (hitch.c, each period) */
void ot_log(void);

#endif
