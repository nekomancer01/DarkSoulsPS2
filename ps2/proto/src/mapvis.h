/* Map visibility (src/mapvis.c): the frame's camera, the chunk / box view tests, foliage thinning and the
 * streamer's item visibility. Map files only. */
#ifndef MAPVIS_H
#define MAPVIS_H
#include "map_i.h"

/* camera (mp_cam_set, once per map_draw): eye, right, up, forward; focal length in pixels (x, y) */
extern float mp_cam[12], mp_cam_f, mp_cam_fy;
void mp_cam_set(void);
/* the last chunk mp_vis_test measured and its box's farthest view depth (mp_draw_chunk's edge fog test, same eye) */
extern const Chunk *mp_vis_c;
extern float mp_vis_zmax;
/* box c against the view from e: 0 out, 1 inside the guard band, 2 crossing the near plane / guard band; sky: no
 * distance tests. mp_visible times it (MP_CULL), mp_vis_test does not */
int mp_vis_test(const Chunk *c, const float *e, int sky);
/* (mp_visible: map_i.h) */
/* view depth of box c's nearest (*zmax: farthest) point and its radius squared */
float mp_box_depth(const Chunk *c, const float *e, float *zmax, float *r2);
/* foliage chunk c's triangles to draw at its distance from e (whole tufts) */
unsigned mp_grass_tris(const Chunk *c, const float *e);

#endif
