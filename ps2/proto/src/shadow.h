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
extern int shadow_drawn;  /* shadows of the last shadow_draw (debugger) */

#endif
