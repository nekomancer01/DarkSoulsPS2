/* Frame governor: sheds optional work before the EE busy time reaches a missed vsync, so play stays an even 30 fps.
 * Input: each frame's EE busy time (hitch_busy_ms: the frame minus its vsync waits; the frame time itself is quantised
 * to 33 / 50 ms by the vsync cap). Level 0 = full work, 1-3 = less relief (bump.c), fewer particle spawns (fx.c) and a
 * nearer every-other-frame pose distance (enemy.c). Rules (frame_budget.h, ASSUMED starting points):
 *   up one level   a frame busy over FB_GOV_EDGE_MS, or 4 of the last 8 over FB_GOV_BUSY_MS
 *   down one level 60 frames in a row under FB_GOV_CALM_MS
 *   skipped        frames busy over 100 ms (settles, loading screens): not counted, no change
 * cvars: GOV 0 = off (level 0), GOV_MAX = the highest level, GOV_FORCE 0-3 pins a level (tests), -1 = off. Nothing the
 * governor turns freezes or pops geometry: no ACTIVE_MAX / DRAW_MAX, no map LOD (TECH_DEBT world-4, burg-hitch-soak).
 * Default GOV 0 (off; `option set GOV 1` / test-run.py --set GOV=1 turns it on): the A/B of 2026-11-27 (iso_burg_taurus
 * 9 runs on vs 2 off, iso_ame_report3 2 vs 2) found no fewer missed vsyncs with it on (Taurus: 2.0 % of frames over
 * 34 ms on, 1.5 % off; report3 0.6 / 0.8 %) while it sat at level >= 1 for 80-100 % of the Burg frames (busy p50 is
 * 22-25 ms there, at FB_GOV_CALM_MS). The misses come from logic spikes (ai / move, up to 26 ms a frame), which none of
 * its knobs touch: TECH_DEBT render-5. */
#ifndef GOVERNOR_H
#define GOVERNOR_H

extern int gov_level;  /* 0-3; read-only outside governor.c */
/* once a frame (frame.c, after hitch_frame): the EE busy ms of the frame just done */
void gov_update(float busy_ms);

#endif
