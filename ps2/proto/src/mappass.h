/* The map's passes around the cells (src/mappass.c): sky, void ring, backdrop pictures, fades, blended effects.
 * Map files only. */
#ifndef MAPPASS_H
#define MAPPASS_H
#include "map_i.h"

#ifndef MAP_BLEND_LATE
#define MAP_BLEND_LATE 1  /* make DEFS=-DMAP_BLEND_LATE=0: the blended map inside the map segment, before the characters */
#endif
/* map_draw, before the cells: the lists below emptied */
void mp_pass_reset(void);
/* the sky of block sk, the void ring and the backdrop pictures, then the opaque map's fog (a, b, fmin, colour col)
 * and test registers. Returns triangles. */
int mp_draw_sky(int sk, const float e[3], u32 col, float a, float b, float fmin);
/* kept while map_draw walks the cells: a blended chunk (sorted far to near, drawn after the opaque map), a horizon
 * fading in over its cell's coarse level (HZN_FADE, alpha al of 128), a coarse level fading out over its near level
 * (NEAR_FADE); full lists drop the rest */
void mp_fx_add(const Chunk *c, const float e[3], int k, int cls, const unsigned char *base, unsigned no);
void mp_hzfade_add(int k, int ci, int al);
void mp_nrfade_add(int k, int ci, int al);
/* the kept fades and effects. Return triangles. mp_draw_effects which: 0 all, 1 the map segment's share (the rest
 * kept for map_draw_blend), 2 the kept ones */
int mp_draw_hz_fades(const float e[3], float a, float b, float fmin);
int mp_draw_near_fades(const float e[3]);
int mp_draw_effects(int fog, int which, u32 col);
unsigned mp_pano_drawn(void);  /* backdrop picture triangles this frame (log) */

#endif
