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
#define VU1_IDX_TRIS 64  /* most triangles in one vu1_add_idx batch (map chunks: <= 54) */
/* VU1_EDGE: fog = max(min(fog, b + a2 z), 0) on top of the map fog (b = vu1_map_fog's b, a2 = vu1_map_edge): far
 * batches fade into the fog colour by map.c's cull distance (vu1map2 edge loop, 91 instead of 79 VU1 cycles per
 * triangle; ignored with VU1_CLIP) */
#define VU1_EDGE 8
/* VU1_PKT: pos / st / col are the streams of one prebuilt VIF packet (tools/export_map.py chunk_streams: a code qword
 * in front of each stream) holding exactly tris triangles: one DMA REF sends all three (render rebuild PR 1). Ignored
 * when the batch is split (VU1_CLIP over VU1_CLIP_TRIS) and in VU1_NO_PKT builds. */
#define VU1_PKT 16
void vu1_add_ex(GSTEXTURE *tex, int cull, int mode, int tris, const void *pos, const void *st, const void *col,
                const float *org, float st_k);
/* Indexed map batch (vu1mapi.vsm; tools/export_map.py index_chunk): nv shared vertices (a multiple of 3; pos / st / col
 * streams as vu1_add_ex, one entry per vertex) and tris triangles in idx, u8 x4 each: the vertex numbers of its
 * corners, 0. 8 + 3 nv + tris <= 493 (the VU1 input buffer). VU1_PKT: the four streams of one prebuilt VIF packet (a
 * code qword before each, idx last). VU1_CLIP batches go to vu1clip.vsm as lists the EE writes into the chain. */
void vu1_add_idx(GSTEXTURE *tex, int cull, int mode, int tris, int nv, const void *pos, const void *st, const void *col,
                 const void *idx, const float *org, float st_k);
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
/* Skinned character (vu1skin2.vsm, or vu1skinclip.vsm near the camera), after vu1_begin in a chain of its own (call once per character, any number per
 * chain): switch to the skin memory layout and upload
 * the bone palette (4 qwords per bone: columns of view * world, col0-2 .w = bone-space light vector). win = NULL:
 * all bones (<= 61) go to palette slots 0..bones-1; else nwin (slot, bone) byte pairs say which bone goes to which slot
 * (the first bone window of a model over 61 bones, char-7). The near-plane test and the specular view direction use
 * every bone either way. */
void vu1_skin_begin(const void *palette, int bones, const unsigned char *win, int nwin);
/* The next bone window of the same character: waits for the batches before it (they read the slots it replaces), then
 * uploads its n (slot, bone) pairs; the other slots keep their bones */
void vu1_skin_window(const void *palette, const unsigned char *win, int n);
/* Fog cap (255 = none, 0 = all fog colour) for the next vu1_skin_begin only: a character fading out at its draw
 * distance (enemy.c); the map's edge fog at the character's depth is applied on top */
void vu1_skin_fade(float fmax);
/* Light for the following textured skin batches: colour = amb + sun x max(n . L, 0) per channel, 0x80 = 1.0 */
void vu1_skin_light(const float amb[3], const float sun[3]);
#define VU1_SKIN_TRIS 24  /* vu1skin2.vsm: input double buffer + two output slots next to a 61-bone palette */
/* One skinned batch (<= VU1_SKIN_TRIS triangles, unindexed): 16-byte aligned streams per vertex: pa/pb s16x4
 * (bone-local position * 4096, palette offset 4 * bone), nrm s16x4 (normal in bone A space * 4096, weight of A * 4096),
 * st f32x2 normalized (textured) or colour u8x4 (t == NULL). tint: added to the light (0x80 = 1.0 when textured). */
void vu1_skin_add(GSTEXTURE *t, int cull, int tris, const float *tint, const void *pa, const void *pb,
                  const void *nrm, const void *st_or_col, int spec_k, const void *ms);
/* Prebuilt skin batches (renderer plan PR 8): vu1_skin_packet writes the VIF packet of one batch into buf (16-byte
 * aligned, zeroed, NULL = size only): the stream unpacks of vu1_skin_add, each stream behind its own code qword. It
 * returns the size in bytes and the streams' places (pa, pb, nrm, ms or NULL, st_or_col) for the caller to fill.
 * vu1_skin_add_pkt then sends the per-frame header and the packet as one DMA reference instead of 4-6; batches the
 * clip program splits (> 12 triangles near the camera) still go stream by stream. */
int vu1_skin_packet(void *buf, int tris, int textured, int ms, void *streams[5]);
/* st: what the packet's / streams' last stream holds (1 st, 0 colour; vu1_skin_packet's `textured`), apart from t: the
 * VIF unpacks follow the data, never the texture. A texture without st draws flat (t ignored); st without a texture
 * (texture missing: VRAM full / not uploaded) draws unlit grey from the st packet. */
void vu1_skin_add_pkt(GSTEXTURE *t, int st, int cull, int tris, const float *tint, const void *pa, const void *pb,
                      const void *nrm, const void *st_or_col, int spec_k, const void *ms, const void *pkt, int pkt_bytes);
/* Specular (textured batches; visuals-fog-light's spec, TECH_DEBT vu1-6): spec_k = Blinn exponent 2^k (1..5), ms =
 * u8x4 per vertex (0, 0, metal m, f = s m + 0.04 s^2 (1 - m)) x 255, or NULL for a batch without metal / specular
 * (it then runs the plain textured program, which costs 131 instead of ~200 VU1 cycles per triangle). Per draw, before
 * vu1_skin_begin: hemisphere env colours (EnvUp / EnvDown) and specular light colour / direction (world), 0x80 = 1.0 */
void vu1_skin_spec(const float env_up[3], const float env_down[3], const float spec_i[3], const float ls[3]);
/* Particles (vu1fx.vsm, fx.c FX_VU1 builds): a third chain, vu1_begin(2) + vu1_map_fog + vu1_fx_begin, then batches of
 * n (even, <= VU1_FX_N) particles of one texture and mode. vu1_fx_add puts the batch in the chain and returns its
 * header (7 qwords) and *parts (2 n qwords) for the caller to fill: header 0 {n, 0, 0, 0}; 1 (cw, ch, 0.5, 1 / fpr);
 * 2 (0.5 / W - cw / 2, 0.5 / H, 0, fpr); 3 (du, dv, 0, edge fog a2); 4 (du, 0, 0, 0); 5 (0, dv, 0, 0); 6 (fog min,
 * fog max, 0, 2048); particle i {world x, y, z, 8 s + 7 age / life}, {half size x, y, roll (-pi..pi), frame + 0.5}.
 * tabs[s] (s < nsys <= VU1_FX_SYS): curve tables of VU1_FX_TABF floats (96 bytes): 8 keys (kx, ky) f32, then 8 keys
 * (r, g, b, a) u8, sent by reference: 16-byte aligned, written back from the data cache, unchanged until the chain is
 * done. Flipbook: cw / ch =
 * cell size, du / dv = cell - 1 texel, all normalized. NULL: no room. GS registers: as render.c's tri_list; restore
 * ALPHA / ZBUF / FOGCOL with vu1_gs_regs before gsKit draws again. */
#define VU1_FX_N 64
#define VU1_FX_SYS 8
#define VU1_FX_TABF 24
#define VU1_FX_TAB 15
#define VU1_FX_PART (VU1_FX_TAB + 16 * VU1_FX_SYS)
#define VU1_FX_ADD 1  /* additive (else alpha blended) */
#define VU1_FX_FOG 2
void vu1_fx_begin(void);
float *vu1_fx_add(GSTEXTURE *t, int mode, u64 fogcol, int n, int nsys, const float *const *tabs, float **parts);
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
/* texstream-4: map texture uploads on PATH3 behind one FLUSHA gate per segment (vu1.c); -DVU1_TEX_PATH2 builds keep
 * the old FLUSH + PATH2 uploads in the chain */
#ifndef VU1_TEX_PATH2
#define VU1_TEX_PATH3
#endif
typedef struct {
    int last[3], peak[3], segments;  /* [2]: the particle chain */
    int mpg_loads;  /* R8: program uploads put in chains so far (only when a program is not resident) */
    int skin_windows;  /* char-7: bone windows uploaded after a character's first (models over 61 bones), so far */
    int clut_loads[3];  /* CLUT loads the GS does per chain with CLD 4 (V7); before: one per textured batch */
    int tex_path3, tex_path2, tex_gates;  /* VU1_TEX_PATH3 builds: uploads queued on PATH3 / sent on PATH2 because
                                           * the queue was full, FLUSHA gates (one per segment with uploads) */
    int idx_batches, idx_clip, idx_vuclip;  /* indexed map batches so far / triangles split on the EE for vu1clip /
                                             * VU1_CLIP batches clipped by vu1clip's indexed entry */
    unsigned long long idx_clip_ticks, wait_ticks;  /* EE ticks so far: vu1_add_idx's near-plane split, room()'s mid-chain waits */
} Vu1Stats;
extern Vu1Stats vu1_stats;

#endif
