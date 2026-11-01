/* Normal-map relief for the map (TECH_DEBT map-23): a second pass over the bumped chunks that multiplies the opaque
 * pass by DSR's per-pixel light ratio (CLUT dot3; data from tools/export_normal.py in bump.bin) */
#ifndef BUMP_H
#define BUMP_H

#include <gsKit.h>

/* blob: bump.bin (kept loaded), after map_upload (its normal maps join the texture pool); chunks: map.bin's resident
 * chunk count (bump.bin covers at least those; streamed chunks follow them). Returns 1 when the relief pass is on, 0 otherwise (no blob, empty
 * bump.bin, VRAM or texture table full). */
int bump_init(GSGLOBAL *gs, const void *blob, unsigned chunks);
/* One opaque map chunk that was just queued (draw_chunk): kept for the relief pass when bump.bin marks it and it lies
 * within BUMP_FAR. d: distance from the eye to the chunk's box. Same streams / origin / st scale as the opaque batch,
 * so VU1 writes the same depth. */
void bump_add(unsigned chunk, float d, int cull, int clip, int tris, const void *pos, const void *st, const void *col,
              const float *org, float st_k);
/* Draw the kept chunks (after the opaque map, before the blended effects; between tc_begin and tc_end): ALPHA
 * Cd x As, Z tested GEQUAL, not written, no fog; restores ALPHA / ZBUF. */
void bump_draw(void);
extern int bump_batches;  /* relief batches last frame (HUD / tests) */

#endif
