/* VU1 batch renderer for static, pre-chunked geometry (map chunks from map.bin) */
#ifndef VU1_H
#define VU1_H

#include <gsKit.h>

/* Upload the microprogram, set up double buffering; call once after gsKit/dmaKit init */
void vu1_init(GSGLOBAL *gs);
/* Start a chain in buffer 0 or 1 (two chains can be built while the other one is drawing): camera constants
 * (view matrix from render.c's camera) */
void vu1_begin(int buffer);
/* Queue one map batch: tris unindexed triangles from 16-byte aligned, zero-padded quantised streams (map.bin R3):
 * pos s16x3 (world = org[0..2] + n x org[3]), st u16x2 (normalized = n / st_k), colour u8x4. cull: drop back faces.
 * mode bits: VU1_FOG (GS fog from the per-vertex factor), VU1_BLEND (alpha blending on, alpha test off; the ALPHA /
 * ZBUF registers themselves are set with vu1_gs_regs) */
#define VU1_FOG 1
#define VU1_BLEND 2
/* VU1_CLIP: the batch crosses the near plane or the guard band: drawn by the clipping program (vu1clip.vsm, F4),
 * which clips against both on VU1 (batches over VU1_CLIP_TRIS are split) */
#define VU1_CLIP 4
#define VU1_CLIP_TRIS 32
/* VU1_EDGE: fog = max(min(fog, b + a2 z), 0) on top of the map fog (b = vu1_map_fog's b, a2 = vu1_map_edge): far
 * batches fade into the fog colour by map.c's cull distance (vu1map2 edge loop, 91 instead of 79 VU1 cycles per
 * triangle; ignored with VU1_CLIP) */
#define VU1_EDGE 8
void vu1_add_ex(GSTEXTURE *tex, int cull, int mode, int tris, const void *pos, const void *st, const void *col,
                const float *org, float st_k);
/* GS mip chain for the next vu1_add_ex only (opaque batches; texcache.c tc_mip): TEX1 MXL mxl, LINEAR_MIPMAP_NEAREST,
 * K k (7.4 fixed), MIPTBP1 miptbp1. The batch then sends MIPTBP1 in TEST's A+D slot: the opaque TEST must be set
 * already (vu1_gs_regs; map.c sets it at the start of its opaque pass) */
void vu1_mip(int mxl, int k, u64 miptbp1);
/* Map program fog for the following batches: F = clamp(a * view z + b, fmin, fmax), 255 = no fog. Call after
 * vu1_begin before the first map batch (the map program writes XYZF2 and needs these constants). */
void vu1_map_fog(float a, float b, float fmin, float fmax);
/* Edge fog slope a2 (fog units per metre of view z, <= 0) for the following VU1_EDGE batches */
void vu1_map_edge(float a2);
/* Set n GS registers (pairs {value, register address}) between batches, in draw order (FLUSH + DIRECT) */
void vu1_gs_regs(int n, const u64 *regs);
/* Upload a texture (w x h pixels of psm, bytes from 16-byte aligned RAM, written back from the data cache) to GS block
 * tbp in draw order (FLUSH + DIRECT image transfer + TEXFLUSH; buffer width = w rounded up to 64, at least 128 for
 * PSMT8 / PSMT4). Returns 0 if the chain had no room. */
int vu1_upload_tex(u32 tbp, int psm, int w, int h, const void *pix, int bytes);
/* Skinned character (vu1skin.vsm), after vu1_begin in a chain of its own (call once per character, any number per
 * chain): switch to the skin memory layout and upload
 * the bone palette (bones <= 61, 4 qwords each: columns of view * world, col0-2 .w = bone-space light vector) */
void vu1_skin_begin(const void *palette, int bones);
/* Fog cap (255 = none, 0 = all fog colour) for the next vu1_skin_begin only: a character fading out at its draw
 * distance (enemy.c); the map's edge fog at the character's depth is applied on top */
void vu1_skin_fade(float fmax);
/* Light for the following textured skin batches: colour = amb + sun x max(n . L, 0) per channel, 0x80 = 1.0 */
void vu1_skin_light(const float amb[3], const float sun[3]);
#ifdef VU1_SKIN_OLD  /* A/B build: the in-place skin program (vu1skin.vsm) */
#define VU1_SKIN_TRIS 31
#else  /* vu1skin2.vsm: input double buffer + two output slots next to a 61-bone palette */
#define VU1_SKIN_TRIS 24
#endif
/* One skinned batch (<= VU1_SKIN_TRIS triangles, unindexed): 16-byte aligned streams per vertex: pa/pb s16x4
 * (bone-local position * 4096, palette offset 4 * bone), nrm s16x4 (normal in bone A space * 4096, weight of A * 4096),
 * st f32x2 normalized (textured) or colour u8x4 (t == NULL). tint: added to the light (0x80 = 1.0 when textured). */
void vu1_skin_add(GSTEXTURE *t, int cull, int tris, const float *tint, const void *pa, const void *pb,
                  const void *nrm, const void *st_or_col, int spec_k, const void *ms);
/* Specular (textured batches; visuals-fog-light's spec, TECH_DEBT vu1-6): spec_k = Blinn exponent 2^k (1..5), ms =
 * u8x4 per vertex (0, 0, metal m, f = s m + 0.04 s^2 (1 - m)) x 255, or NULL for a batch without metal / specular
 * (it then runs the plain textured program, which costs 131 instead of ~200 VU1 cycles per triangle). Per draw, before
 * vu1_skin_begin: hemisphere env colours (EnvUp / EnvDown) and specular light colour / direction (world), 0x80 = 1.0 */
void vu1_skin_spec(const float env_up[3], const float env_down[3], const float spec_i[3], const float ls[3]);
/* Send the current chain to VIF1 (returns at once; the EE can keep working). Waits for the previous chain first.
 * Streams written by the EE this frame must be written back with SyncDCache before this. */
void vu1_kick(void);
/* Wait until the chain, VU1 and PATH1 are done */
void vu1_wait(void);
/* Force the next textured batch to reload its CLUT (TEX0 CLD 2): call after rewriting a CLUT in VRAM at an address
 * the chain may already have loaded (batches use CLD 4 = load only when CBP changes) */
void vu1_clut_reload(void);
/* Chain sizes in qwords (R1): last / peak per buffer (0 map, 1 characters) incl. segments; segments = times a full
 * buffer was sent early (each costs an EE wait) */
typedef struct {
    int last[2], peak[2], segments;
    int mpg_loads;  /* R8: program uploads put in chains so far (only when a program is not resident) */
    int clut_loads[2];  /* CLUT loads the GS does per chain with CLD 4 (V7); before: one per textured batch */
} Vu1Stats;
extern Vu1Stats vu1_stats;

#endif
