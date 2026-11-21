/* Map fog and light state (src/maplight.c). Map files only; the rest of the game reads it through map.h
 * (map_char_light, map_char_spec, map_far_dof, ...). */
#ifndef MAPLIGHT_H
#define MAPLIGHT_H
#include "map_i.h"

/* (mp_fog_now, mp_fog_area, mp_pfog_row / rows, mp_fog_ease: map_i.h) */
extern int mp_pinst;  /* the instance the player stands in (sky, fog rows; map_set_floor) */
/* this frame's fog terms F = clamp(a z + b, fmin, 255) of the eased row; sets the edge fog below */
void mp_fog_terms(float *a, float *b, float *fmin);
/* edge fog this frame: slope a2 of b + a2 z and the view depth beyond which it is below the DSR fog */
extern float mp_edge_a2, mp_edge_z;
/* fog terms and FOGCOL of sky row `row` of m (from 1; 0: the header's sky fog) */
u32 mp_sky_fog(const MapInst *m, unsigned row, float *a, float *b, float *fmin);
/* the player's light on the map (MAP_PLIGHT): per frame begin, the chunks within its reach collected, drawn */
void mp_pl_begin(void);
void mp_pl_add(const unsigned char *base, const Chunk *c, unsigned n, int clip, int tex, int level);
void mp_pl_draw(void);
/* the specular pass (MAP_SPEC): opaque chunks with a specular code within SPEC_DIST, drawn with the relief pass */
void mp_spec_add(const MapInst *m, const unsigned char *base, const Chunk *c, float d);
void mp_spec_draw(void);

#endif
