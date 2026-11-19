/* trail.h: the player's weapon trail (Polaris 2.6 PR 3; FFX tracer, action 40). The attack animations' TAE
 * SpawnFFX_Blade events (combat.gd trail windows, tools/export_combat.py) give the time spans and the blade's base / tip
 * dummy polys; combat.c samples both in world space each frame of a window (trail_sample), trail_draw draws the
 * samples younger than the tracer's TrailMaxTime as a strip. */
#ifndef TRAIL_H
#define TRAIL_H

/* the frame's time step (call once a frame, before any trail_sample) */
void trail_tick(float dt);
/* one sample of the blade: base and tip in world space */
void trail_sample(const float base[3], const float tip[3]);
/* the strip, EE path (additive, no Z write) */
void trail_draw(void);
/* samples drawn last frame (TEST line / debug HUD) */
int trail_drawn(void);

#endif
