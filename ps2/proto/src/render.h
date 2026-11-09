/* Minimal CPU-transformed 3D on gsKit: camera, near-plane clipping, flat-shaded boxes, lines, HUD text. */
#ifndef RENDER_H
#define RENDER_H

#include <gsKit.h>
#include "vmath.h"

#ifndef R_NEAR
#define R_NEAR 0.25f    /* near plane, metres (16-bit depth: Z = 65535 * near / z) */
#endif
#define R_GUARD 1700.0f /* px from the screen centre that GS coordinates still hold safely */
/* V1c: frame buffer width (DISPLAY MAGH stretches it to the full 4:3 picture). 2D code keeps 640-wide logical
 * coordinates; R_HX scales them to buffer pixels at the draw call. */
#define R_SCREEN_W 512
#define R_SCREEN_H 448
#define R_HX(x) ((x) * (R_SCREEN_W / 640.0f))

/* 3x3 rotation, row-major; a local frame for boxes and bones */
typedef struct { float m[9]; } M3;
M3 m3_identity(void);
M3 m3_rot_y(float a);
M3 m3_rot_x(float a);
M3 m3_rot_z(float a);
M3 m3_mul(M3 a, M3 b);
V3 m3_apply(M3 m, V3 v);

void r_init(GSGLOBAL *gs);
/* Current camera (for the VU1 renderer): eye, view basis, focal lengths in px (x, y), Z scale
 * (z_gs = zscale / view_z) */
void r_camera_basis(V3 *eye, V3 *right, V3 *up, V3 *fwd, float *focal, float *focal_y, float *zscale);
/* 0 = outside the frustum, 1 = entirely inside the near plane and guard band (VU1-safe), 2 = needs clipping */
int r_box_classify(const float *mn, const float *mx);
/* Camera at eye looking at target (y up) */
void r_camera(V3 eye, V3 target);
void r_begin(void);
/* Box of half-extents h, centred at local offset c inside the frame (pos, rot); colour rgb 0-255 */
void r_box(V3 pos, M3 rot, V3 c, V3 h, int r, int g, int b);
void r_line(V3 a, V3 b, int r, int g, int bl);
/* World point -> screen x, y and GS z in out[0..2]; out[2] < 0 when it is behind the near plane */
void r_to_screen(V3 p, float out[3]);
/* True if screen triangle a, b, c (y down) faces away: DS front faces give a negative (b - a) x (c - a) here.
 * (Verified in PCSX2: the opposite sign removed the floor, walls and the outside of the armour.) */
static inline int r_backface(const float *a, const float *b, const float *c)
{
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) >= 0;
}
/* Gouraud triangle from r_to_screen outputs */
void r_tri_screen(const float *a, const float *b, const float *c, u64 ca, u64 cb, u64 cc);
/* Textured (modulated) gouraud triangle; uv = 3 (u, v) pairs in texels */
void r_tri_screen_tex(GSTEXTURE *t, const float *a, const float *b, const float *c, const float *uv, u64 ca, u64 cb,
                      u64 cc);
/* View-space vertex for clipped drawing */
typedef struct { float x, y, z, u, v, r, g, b, a; } ClipVert;
/* World point -> view space (x right, y up, z forward) */
V3 r_view(V3 p);
/* True if a view-space point is in front of the near plane and inside the guard band */
float r_guard_k(void);  /* guard band half width / focal length: view-space limit per metre of depth */
int r_point_inside(const V3 *v);
/* Stricter variant matching VU1's skip test: a triangle with any vertex failing it is drawn by the EE */
int r_point_vu_safe(const V3 *v);
/* View-space point -> screen x, y, z (only for points r_point_inside accepts) */
void r_view_to_screen(const V3 *v, float out[3]);
/* True if all 3 view-space points are inside the near plane and the screen guard band (no clipping needed) */
int r_inside(const V3 *a, const V3 *b, const V3 *c);
/* Clip a view-space triangle against the near plane and guard band, then draw it (textured if t != NULL; mode: R_FOG,
 * R_BLEND (alpha blend, no Z write), R_ADD (with R_BLEND: additive); untextured ones are fogged too) */
#define R_FOG 1
#define R_BLEND 2
#define R_ADD 4
void r_tri_clip(GSTEXTURE *t, const ClipVert *a, const ClipVert *b, const ClipVert *c, int cull, int mode);
/* Unclipped triangle list in one GS packet (same registers as r_tri_clip): nv vertices (multiple of 3) whose view-space
 * points r_point_inside accepts, s = their r_view_to_screen outputs (fx.c batches particles through it) */
void r_tri_list(GSTEXTURE *t, int mode, int nv, const ClipVert *v, const float (*s)[3]);
/* Fog for r_tri_clip: factor F = clamp(a * view z + b, fmin, fmax) (255 = none), FOGCOL rgb while drawing,
 * base_rgb restored after each packet (the map's normal fog colour, which the VU1 path also uses) */
void r_set_fog(float a, float b, float fmin, float fmax, u32 rgb, u32 base_rgb);
/* edge fog slope for the EE path (map.c fog_terms): F = max(min(F, b + a2 z), 0); 0 = none */
void r_set_fog_edge(float a2);
/* the EE path's fog terms as set above (fx.c hands them to its VU1 particle chain): a, b, fmin, fmax, edge a2 */
void r_get_fog(float f[5], u64 *rgb, u64 *base_rgb);
/* View-space axis-aligned box test against the frustum: 0 if certainly outside */
int r_box_visible(const float *mn, const float *mx);
void r_ground(float size, float cell);
/* V6: block-granular VRAM for textures and CLUTs (gsKit_vram_alloc rounds every allocation up, and a sub-page PSMT8
 * texture's blocks are not contiguous). Each allocation takes the blocks its real footprint covers (PSMT8 with
 * vu1.c's TBW, CT32 / CT16 16x16 CLUTs), first fit in the free blocks of pages taken earlier, else new 8 KB pages
 * at gs->CurrentPointer (which stays page aligned for gsKit's own allocations after it). Byte address or
 * GSKIT_ALLOC_ERROR (0). r_vram_lin: n consecutive blocks (CLUT tables). The r_vram_free_* calls give an
 * allocation's blocks back (the same w / h / psm / n it was made with) once the GS no longer reads them (callers free
 * between frames: the last frame's chains and gsKit queue are done before the next frame's updates run). */
u32 r_vram_tex(GSGLOBAL *gs, int w, int h, int psm);
u32 r_vram_clut(GSGLOBAL *gs, int clut_psm);
u32 r_vram_lin(GSGLOBAL *gs, int n);
void r_vram_free_tex(u32 vram, int w, int h, int psm);
void r_vram_free_clut(u32 vram, int clut_psm);
void r_vram_free_lin(u32 vram, int n);
/* VRAM bytes free: after gs->CurrentPointer plus the free blocks of r_vram's pages */
int r_vram_avail(const GSGLOBAL *gs);
/* blocks taken / pages taken (stats) / free blocks inside those pages */
extern int r_vram_used, r_vram_pages, r_vram_gskit, r_vram_holes;  /* + blocks gsKit_vram_alloc would have taken */
/* Screen-space HUD (640x448 logical, x scaled by R_HX) */
void r_rect(float x, float y, float w, float h, int r, int g, int b);
void r_text(float x, float y, float scale, const char *s, int r, int g, int b);
/* Light text on a dark box, readable on any background */
void r_text_outlined(float x, float y, float scale, const char *s);

#endif
