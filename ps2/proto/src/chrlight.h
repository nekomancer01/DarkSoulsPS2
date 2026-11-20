/* Character lights (Polaris 2.2 PR 3): the light of a skinned character as 1 ambient + 1 directional (the area's sun)
 * + up to 2 point lights with a direction. chrl_frame gathers the frame's point lights once (the player-attached
 * light, the lit effect lights); chrl_pick picks a character's two strongest and folds the rest into its ambient. */
#ifndef CHRLIGHT_H
#define CHRLIGHT_H

#include "render.h"

/* once a frame, before the characters draw (frame.c phase_emit) */
void chrl_frame(void);
/* the light of a character at pos (feet), in map_char_light's units (0x80 = 1.0): amb, the sun's colour and world
 * direction (towards it), and np (0..2) point lights: their world direction from the character's centre (pos + 1 m)
 * towards them and their colour after falloff. CHR_LIGHTS 0: map_char_light, np = 0. */
void chrl_pick(V3 pos, float amb[3], float sun[3], float dir[3], int *np, float pdir[2][3], float pcol[2][3]);

#endif
