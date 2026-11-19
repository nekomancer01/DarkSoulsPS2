/* Blob shadows (Polaris 2.2 PR 5): a soft dark disc on the floor under every drawn character. The emit pass queues
 * them (shadow_add, from player_draw / enemy_draw / npc_draw) and frame.c draws them once after the characters
 * (shadow_draw). */
#ifndef SHADOW_H
#define SHADOW_H

#include "render.h"

/* a character with its feet at feet and capsule radius r; fade 0..1 (its fog fade-out) */
void shadow_add(V3 feet, float r, float fade);
/* the frame's queued shadows: floor lookup, then one blended triangle list; clears the queue */
void shadow_draw(void);
extern int shadow_drawn;
/* Polaris 2.2 PR 6, the player's projected silhouette (SHADOW_SIL): shadow_init at boot takes its VRAM (a 128 x 64
 * CT16 target and its Z, 32 KB). player_draw brackets one more draw of the player model with shadow_sil_begin (1 =
 * draw it now: the GS target and a camera at the sun are set) and shadow_sil_end; shadow_draw then lays the
 * silhouette on the floor along the sun instead of the player's blob. */
void shadow_init(GSGLOBAL *gs);
int shadow_sil_begin(V3 feet);
void shadow_sil_end(void);  /* shadows of the last shadow_draw (debugger) */

#endif
