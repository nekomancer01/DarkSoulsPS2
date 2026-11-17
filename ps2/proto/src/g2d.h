/* g2d.h: the 2D packet writer (Polaris 1.6), in place of gsKit's draw queue. Same calls and the same conventions as
 * the gsKit primitives it replaces (texel-coordinate sprites, x / y * 16 + the window offset clamped to 0..4095.9375,
 * u / v * 16 clamped to the texture, PRIM's ABE / AA1 from gs->PrimAlphaEnable / PrimAAEnable, TEX1 from the
 * texture's filter before each textured sprite), so the GS draws the same thing (the Polaris 1.6 gate compares the
 * decoded draws, src/gstrace.c).
 *
 * Streams: WORLD (the EE-path triangles of the map and effects, debug world lines; order-table bucket BLEND..FX) and
 * UI (HUD, menus, dialogs, text, the debug overlay; bucket UI) are built each frame in a slice of the frame region
 * (mem.h), the order table sends each at its turn (ot_g2d). LOAD is the loading screen's own small pair (static: the
 * frame swap does not run during a load); loading.c flushes it itself (legacy present in tools/ci/gsown.py).
 * The writer's register cache (TEST / ALPHA / CLAMP / TEX1 skipped when unchanged) is cleared at every stream start
 * and after every raw reservation: VU1 segments and r_tri_list's own packets change those registers in between.
 * A stream that fills up cuts the rest of its primitives (TEST error with the stream and the caller, cut count). */
#ifndef G2D_H
#define G2D_H

#include <gsKit.h>
#include "ot.h"

typedef enum { G2D_WORLD, G2D_UI, G2D_LOAD, G2D_N } G2dStream;

void g2d_init(GSGLOBAL *gs);
void g2d_frame_begin(void);    /* the frame's WORLD / UI buffers (frame region) */
void g2d_select(G2dStream s);  /* where the calls below go */
G2dStream g2d_current(void);
/* the order table sends a stream (ot.c): its packets closed, ready as a GIF DMA chain; NULL when empty */
void *g2d_close(G2dStream s);
/* the loading screen's present: send LOAD now and wait (loading.c) */
void g2d_flush_load(void);

/* gsKit's queued state calls */
void g2d_set_test(GSGLOBAL *gs, u64 which);
void g2d_set_primalpha(GSGLOBAL *gs, u64 alpha);
void g2d_set_clamp(GSGLOBAL *gs, u8 mode);
/* gsKit's primitives (same arguments) */
void g2d_prim_sprite(GSGLOBAL *gs, float x1, float y1, float x2, float y2, int z, u64 color);
void g2d_prim_sprite_texture(GSGLOBAL *gs, const GSTEXTURE *t, float x1, float y1, float u1, float v1, float x2,
                             float y2, float u2, float v2, int z, u64 color);
void g2d_prim_triangle(GSGLOBAL *gs, float x1, float y1, float x2, float y2, float x3, float y3, int z, u64 color);
void g2d_prim_quad(GSGLOBAL *gs, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, int z,
                   u64 color);
void g2d_prim_quad_gouraud(GSGLOBAL *gs, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4,
                           int z, u64 c1, u64 c2, u64 c3, u64 c4);
void g2d_prim_triangle_gouraud_3d(GSGLOBAL *gs, float x1, float y1, int z1, float x2, float y2, int z2, float x3,
                                  float y3, int z3, u64 c1, u64 c2, u64 c3);
void g2d_prim_line_3d(GSGLOBAL *gs, float x1, float y1, int z1, float x2, float y2, int z2, u64 color);
void g2d_clear(GSGLOBAL *gs, u64 color);
/* qw quadwords of raw GIF packets (the caller writes its own tags; render.c r_tri_list); NULL when full */
u64 *g2d_raw(int qw);

/* gsKit_texture_upload (an immediate GIF transfer the order table does not own): in a load, or with the GIF channel idle
 * (TEST error otherwise: that path hung PATH3 before, drb.c) */
void g2d_tex_upload(GSGLOBAL *gs, GSTEXTURE *t);

/* one "g2d:" log line (hitch.c): peak bytes per stream, cut primitives */
void g2d_log(void);

#endif
