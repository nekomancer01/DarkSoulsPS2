/* camera_i.h: the view's constants, shared by render.c (drawing) and camera.c (the camera the simulation also reads:
 * enemy pose updates and omission levels, NPC visibility). Internal to those two files. */
#ifndef CAMERA_I_H
#define CAMERA_I_H
#include "render.h"
#define SCREEN_W ((float)R_SCREEN_W)
#define SCREEN_H ((float)R_SCREEN_H)
#define NEAR R_NEAR
/* LockCamParam row 0: camFovY 43 degrees. W x 448 is shown 4:3, so a pixel is (W / 448) / (4 / 3) times wider than
 * tall: FOCAL_Y = 224 / tan(21.5 deg), FOCAL_X = FOCAL_Y * (W / 448) / (4 / 3) (609 at 640, 487.2 at 512). */
#ifndef PANO_ZOOM
#define PANO_ZOOM 1.0f  /* tools/pano.py capture builds only (-DPANO_ZOOM=2.0f): narrower view, more texels per degree */
#endif
#define FOCAL_Y (568.4f * PANO_ZOOM)
#define FOCAL_X (609.0f * R_SCREEN_W / 640.0f * PANO_ZOOM)
#define FOCAL FOCAL_X  /* guard-band tests use the larger one, which is the stricter limit on both axes */
#define ZMAX 65535.0f  /* 16-bit Z (PSMZ16S, matching the CT16 colour buffer); larger = nearer, tested GEQUAL */
#define GUARD R_GUARD
#endif
