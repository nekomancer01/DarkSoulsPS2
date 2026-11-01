#include <kernel.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include "map.h"
#include "vu1.h"
#include "collision.h"
#include "texcache.h"
#include "bump.h"
#include "fx.h"
#include "mapstream.h"
#include "drawgrp.h"
#include "pack.h"

/* map.bin, loaded by main.c from the area pack (pack.c): 128-byte aligned, flushed for DMA, kept while the area is loaded */
static const unsigned char *map_bin;

typedef struct {
    char magic[4];
    unsigned chunks, textures, nav_verts, nav_tris;
    float spawn[3], spawn_yaw;
    unsigned chunk_off, tex_off, navv_off, navt_off;
    float fog_begin, fog_end, fog_max;  /* FogBank row: view z metres, maximum density (may exceed 1) */
    unsigned char fog_rgb[4], sky_fog[4]; /* sky_fog: r, g, b, constant density x 255 */
    unsigned fog_off, cell_off;          /* fog section: area fog rows + row per collision triangle; DSM5: cell table */
} Header;
/* fog section: u32 rows, collision tris, 0, 0; FogRow rows[]; u8 row per collision.bin triangle. One row per draw-param
 * set of the collision parts (export_map.fog_section): FogBank row (colour = DSR's constant), the character light of
 * that area (colour = amb + sun x max(n . L, 0), 0x80 = 1.0; dir = L x 127) and the player-attached point light
 * (PointLightBank[MSB lantern_id]: rgb added to a character's ambient x saturate((end - d) / (end - begin)), d = its
 * distance to the player; begin / end in 0.1 m) */
typedef struct {
    float begin, end, max;
    unsigned char rgb[4], amb[4], sun[4];
    signed char dir[3];
    unsigned char pl_begin, pl_rgb[3], pl_end;
    unsigned char env_up[3], env_down[3], spec_i[3];  /* specular (vu1skin): env reflection, specular light, 0x80 = 1.0 */
    signed char spec_dir[3];                          /* specular light direction x 127 (world) */
    unsigned pad;
} FogRow;
_Static_assert(sizeof(FogRow) == 48, "FogRow");
/* VU1 batch: unindexed quantised streams pos s16x3 (world = org + n x step), st u16x2 (normalized x 4096, or x 1024
 * with CHUNK_ST1K), colour u8x4 (light, 0x80 = 1.0) */
typedef struct { int tex; unsigned flags, tris, pos, st, col; float mn[3], mx[3], org[3], step; } Chunk;

#define MAX_TEX 128
#ifndef TEX_POOL_KB
#define TEX_POOL_KB 896  /* VRAM texture pool (texcache.c); CLUTs come on top. Burg's map working set reaches ~950 KB
                          * with relief: at 640 KB the bonfire-north spot skipped 84 chunks a frame and p00_082 165 (15 fps);
                          * 896 KB: 0 at every burg_cov spot, p00_082 20 fps; 1024 KB adds nothing (TECH_DEBT area-9) */
#endif
#define CHUNK_CULL 1
#define CHUNK_BLEND 2  /* alpha blended, no Z write */
#define CHUNK_ADD 4    /* with CHUNK_BLEND: additive */
#define CHUNK_SKY 8    /* sky / background: own fog, never distance-culled, drawn first */
#define CHUNK_ST1K 32  /* texture coordinates in 1/1024 units, else 1/4096 */
/* DSM5 (two-level map, tools/export_map.py STREAM): CHUNK_COARSE = the coarse level, drawn only while the cell's near
 * blob (mapstream.c) is not loaded; near chunks (in the blobs) carry their bump.bin number in flags bits 8-31 */
#define CHUNK_COARSE 64
#define CHUNK_BACKDROP 128  /* far background (exporter: far-only cells beyond STREAM_FAR of the navmesh): one resident
                             * level, never distance / subpixel culled, so its silhouette has no holes and never pops */
#define NEAR_HYST 2.0f  /* m: a cell switches to its near level inside NEAR_DRAW_R - NEAR_HYST, back beyond + NEAR_HYST
                         * (no flipping near <-> coarse while the camera hovers at the radius) */
#ifndef NEAR_DRAW_R
#define NEAR_DRAW_R 24.0f  /* m: a loaded near blob is drawn only for cells whose box is this close to the camera (the
                            * blobs load out to 40 m ahead of time); farther cells keep the coarse level (ASSUMED, map-36:
                            * Burg p00_082: 85k tris / map 15.3 ms at 24 m, 67k / 13.8 ms at 16 m; 16 m made the near <->
                            * coarse swap visible: Firelink's coarse level is judged from STREAM_D 24 m) */
#endif
/* Near / coarse cross-fade (map-37): over the band map_near_r +- map_near_fade (distance to the cell box) a cell draws
 * both levels, one opaque and the other blended over it afterwards with a FIX alpha and no Z write: inner half near
 * opaque + coarse fading in, outer half coarse opaque + near fading out, so each level's silhouette (the part in front
 * of the other) fades instead of swapping in one frame. Both levels are resident while the near blob is loaded (blobs
 * load out to MS_LOAD_R 40 m). map_near_fade 0: the old hard swap with NEAR_HYST. */
#ifndef NEAR_FADE
#define NEAR_FADE 4.0f
#endif
#define R_FADE 8       /* draw_chunk: fade pass (VU1_FADE, no relief) */
#define MAX_BAND 64    /* cells in the fade band per frame */
#define CHUNK_GRASS 16 /* foliage: bits 8-15 triangles per tuft, 16-23 / 24-31 lowest / highest tuft rank x 255 */
#define GRASS_D0 10.0f  /* metres: foliage at its exported density up to here, then (GRASS_D0 / d)^2 (export_map.py) */
#define FOG_RATE 0.06f  /* per frame: fog parameters move this part of the way to the area's row (~0.3 s at 60 fps) */
#define MAX_FX 256     /* blended chunks sorted per frame */
#define STEP_UP 0.6f    /* highest step the player walks up in one move */
#define STEP_DOWN 1.2f  /* lowest drop that still counts as the same floor */
/* Draw distance (render-distance): the edge fog (fog_terms) reaches the fog colour at FAR metres of view depth; opaque
 * boxes are drawn out to CULL_FAR (the far background stays a silhouette), blended ones to FAR. ASSUMED: DSR draws
 * the whole loaded map (plus neighbouring maps and far _L pieces) (TECH_DEBT map-7). Boxes beyond SMALL_Z whose radius
 * is under SMALL_PX pixels are skipped too. */
#ifndef FAR
#define FAR 250.0f      /* view depth where the edge fog reaches the fog colour */
#endif
#ifndef CULL_FAR
#define CULL_FAR FAR  /* view depth beyond which opaque boxes are not drawn. A/B (far-background variants): 1000 m keeps
                       * the background (Firelink's m8000 backdrop at 250-450 m) as a fog-coloured silhouette; the cut at
                       * FAR leaves sky-coloured holes and steps in it (user report 2026-10-31) */
#endif
#define EDGE0 0.6f      /* rows without DSR fog: the edge fog starts at EDGE0 x FAR */
#ifndef SMALL_Z
#define SMALL_Z 40.0f
#endif
#ifndef SMALL_PX
#define SMALL_PX 3.0f   /* box radius in px (A/B 1: 3 px drops whole far slabs, holes in the background silhouette) */
#endif

static const Header *hdr;
static const MapCell *ctab;  /* DSM5 cell table (resident range + streamed near blob per cell), else NULL */
/* a near chunk's number in bump.bin / dg.bin: flags bits 8-31; foliage keeps its tuft bits there, so its number is
 * the cell's first near number (MapCell.pad, set per cell in map_draw) + its index in the blob (export_map.py) */
static unsigned near_gid;
#define NEAR_NO(c, blob) ((c)->flags & CHUNK_GRASS ? near_gid + (unsigned)((c) - (const Chunk *)(blob)) : (c)->flags >> 8)
static unsigned nctab;
static const Chunk *chunks;
static const FogRow *fog_row;
static const unsigned char *fog_tri;
static unsigned fog_rows, fog_ntris;
static V3 player_feet;
/* the fog drawn now (begin, end, max, r, g, b) and the character light (ambient rgb, sun rgb, sun direction xyz),
 * moving towards fog_row[fog_area] */
#define FOG_NOW 27
static float fog_now[FOG_NOW] = {0, 0, 0, 0, 0, 0, 64, 64, 64, 96, 96, 96, 0.40f * 127, 0.80f * 127, -0.45f * 127};
/* + specular: env up rgb [15], env down rgb [18], specular light rgb [21], its direction x 127 [24] (0: none) */
static int fog_area;
static const float *navv;
static const unsigned short *navt;
static GSGLOBAL *gsg;
static int ntex;
int map_vu1_batches, map_ee_tris;
/* World blocks (src/world.c): the globals above describe the selected instance (map_select). streams = the base of
 * the resident chunks' vertex streams (the whole map.bin, or its stream part when loaded split); NULL = a "lite"
 * block: only its streamed near cells draw. tex_base = its first texcache index. The fog / light rows follow the
 * instance the player stands in (pfog_*). */
static const unsigned char *streams;
static int tex_base, cur_inst;
static const FogRow *pfog_row;
static unsigned pfog_rows;
static void map_select(int k);
static int map_inst_of(int col_block);
static int inst_col_block(int k);
static int pinst;  /* the instance the player stands in (sky, fog rows) */

static void fog_ease(float rate)
{
    const FogRow *r = &pfog_row[fog_area];
    const float t[FOG_NOW] = {r->begin, r->end, r->max, r->rgb[0], r->rgb[1], r->rgb[2], r->amb[0], r->amb[1],
                              r->amb[2], r->sun[0], r->sun[1], r->sun[2], r->dir[0], r->dir[1], r->dir[2],
                              r->env_up[0], r->env_up[1], r->env_up[2], r->env_down[0], r->env_down[1],
                              r->env_down[2], r->spec_i[0], r->spec_i[1], r->spec_i[2], r->spec_dir[0],
                              r->spec_dir[1], r->spec_dir[2]};
    for (int i = 0; i < FOG_NOW; i++)
        fog_now[i] += (t[i] - fog_now[i]) * rate;
}

int map_init(const void *blob)
{
    if (!blob)
        return 0;
    map_bin = blob;
    hdr = (const Header *)map_bin;
    if (memcmp(hdr->magic, "DSM4", 4) && memcmp(hdr->magic, "DSM5", 4))
        return 0;
    ctab = hdr->magic[3] == '5' && hdr->cell_off ? (const MapCell *)(map_bin + hdr->cell_off + 16) : NULL;
    nctab = ctab ? *(const unsigned *)(map_bin + hdr->cell_off) : 0;
    chunks = (const Chunk *)(map_bin + hdr->chunk_off);
    fog_rows = *(const unsigned *)(map_bin + hdr->fog_off);
    fog_ntris = ((const unsigned *)(map_bin + hdr->fog_off))[1];
    fog_row = (const FogRow *)(map_bin + hdr->fog_off + 16);
    fog_tri = (const unsigned char *)(fog_row + fog_rows);
    /* no rows: the header's fog and the old fixed character light (0.5 + 0.75 n . L) */
    float f0[FOG_NOW] = {hdr->fog_begin, hdr->fog_end, hdr->fog_max, hdr->fog_rgb[0], hdr->fog_rgb[1], hdr->fog_rgb[2],
                          64, 64, 64, 96, 96, 96, 0.40f * 127, 0.80f * 127, -0.45f * 127};
    memcpy(fog_now, f0, sizeof fog_now);
    fog_area = 0;
    pfog_row = fog_row, pfog_rows = fog_rows;
    if (fog_rows)
        fog_ease(1);
    navv = (const float *)(map_bin + hdr->navv_off);
    navt = (const unsigned short *)(map_bin + hdr->navt_off);
    streams = map_bin, tex_base = 0;
    return 1;
}

/* world blocks: the selected instance's split textures read mip 0 from its resident streams (NULL while lite) */
static void tex_res(int k)
{
    tc_res(k ? k - 1 : -1, streams);
}

int map_upload(GSGLOBAL *gs)
{
    gsg = gs;
    ntex = hdr->textures < MAX_TEX ? hdr->textures : MAX_TEX;
    /* the textures stream through a VRAM pool (texcache.c: uploads in the VIF1 chain, LRU); the CLUTs stay resident */
    int r = tc_init(gs, map_bin, map_bin + hdr->tex_off, ntex, TEX_POOL_KB);
    if (!r)
        tex_res(cur_inst);
    return r;
}

unsigned map_chunk_count(void)
{
    return hdr ? hdr->chunks : 0;
}

void map_spawn(V3 *pos, float *yaw)
{
    *pos = v3(hdr->spawn[0], hdr->spawn[1], hdr->spawn[2]);
    *yaw = hdr->spawn_yaw;
}

/* Chunks that touch the near plane or guard band also go to VU1, which skips the triangles that need clipping;
 * those (and only those) are clipped on the EE and drawn through gsKit */
#ifdef MAP_PROF  /* profiling build: map_vu1_batches / map_ee_tris show visibility / EE-clip time in 0.01 ms */
static unsigned prof_vis, prof_clip;
static inline unsigned ticks(void) { unsigned c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }
#define PROF_BEGIN unsigned _t0 = ticks();
#define PROF_END(acc) acc += ticks() - _t0;
#else
#define PROF_BEGIN
#define PROF_END(acc)
#endif

/* camera (set per frame in map_draw): eye, right, up, forward; focal length in pixels */
static float cam[12], cam_f;
/* edge fog this frame (fog_terms): slope a2 of b + a2 z and the view depth beyond which it is below the DSR fog */
static float edge_a2, edge_z;

/* GS registers for the blend passes: ALPHA (Cs - Cd) * As + Cd or Cs * As + Cd, ZBUF with or without ZMSK */
static void blend_regs(GSGLOBAL *gs, int mode)
{
    u64 zbuf = GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0);
    u64 r[4] = {mode & CHUNK_ADD ? GS_SETREG_ALPHA(0, 2, 0, 1, 0) : GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0x42,
                mode ? zbuf | (1ull << 32) : zbuf, 0x4E};
    if (!mode)
        r[0] = gs->PrimAlpha;
    vu1_gs_regs(2, r);
}

/* Far panorama (render-distance variant A, tools/pano.py): pano.bin in the area pack = u32 'PANO', chunks, chunk
 * offset, textures, texture offset; Chunk records as map.bin's (offsets from pano.bin, tex = its own texture index);
 * a world-fixed cylinder textured with the far map rendered offline, drawn after the sky, unfogged */
static unsigned char *pano;
static const Chunk *pano_chunk;
static int pano_n, pano_tex = -1;

static void pano_open(const char *pak_path)
{
    Pack pk;
    unsigned size;
    if (pano || !pack_open(&pk, pak_path))
        return;
    pano = pack_load(&pk, "pano.bin", &size);
    pack_close(&pk);
    if (!pano)
        return;
    const unsigned *h = (const unsigned *)pano;
    if (memcmp(pano, "PANO", 4) || (pano_tex = tc_add(pano, pano + h[4], h[3], 0)) < 0) {
        free(pano), pano = NULL;
        return;
    }
    pano_chunk = (const Chunk *)(pano + h[2]), pano_n = h[1];
    FlushCache(0);  /* texcache DMAs the texels and VU1 the streams straight from this buffer */
    printf("map: panorama %d chunks, %u textures, %u KB\n", pano_n, h[3], size >> 10);
}

int map_stream_open(const char *pak_path)
{
    pano_open(pak_path);
    return ctab ? mapstream_init(pak_path, ctab, nctab) : 0;
}

void map_set_player(V3 feet)
{
    const float p[3] = {feet.x, feet.y, feet.z};
    mapstream_update(p);  /* every instance's near cells (src/mapstream.c) */
    /* DSR's fog follows the collision part under the player (its MSB FogID); in the air the last one stays */
    ColHit h;
    player_feet = feet;
    int k = -1;
    if (col_floor(feet.x, feet.z, feet.y + 0.5f, feet.y - 3, COL_SOLID, &h) && (k = map_inst_of(h.block)) >= 0)
        dg_floor(k, h.tri);  /* the shown draw groups follow the same collision part (drawgrp.c, TECH_DEBT area-5) */
    /* every other loaded block takes its draw groups from its own collision under the player: at a seam both blocks'
     * collisions overlap (DSR's ConnectCollision pairs, e.g. Firelink h0029B2 / Burg h1127B1), each map shows what its
     * own part shows. Elsewhere a block keeps the set of the last of its parts the player stood on (ASSUMED,
     * TECH_DEBT world-4) */
    for (int j = 0; j < MAP_INST; j++) {
        ColHit hj;
        if (j != k && inst_col_block(j) >= 0 &&
            col_floor_block(inst_col_block(j), feet.x, feet.z, feet.y + 0.5f, feet.y - 3, COL_SOLID, &hj))
            dg_floor(j, hj.tri);
    }
    if (k >= 0) {
        int was = cur_inst;
        map_select(k);
        pinst = k;
        if (fog_rows && fog_ntris == (unsigned)col_block_tris(h.block) && (unsigned)h.tri < fog_ntris &&
            fog_tri[h.tri] < fog_rows) {
            pfog_row = fog_row, pfog_rows = fog_rows;
            fog_area = fog_tri[h.tri];
        }
        map_select(was);
    }
}

void map_char_light(V3 pos, float amb[3], float sun[3], float dir[3])
{
    float l = sqrtf(fog_now[12] * fog_now[12] + fog_now[13] * fog_now[13] + fog_now[14] * fog_now[14]);
    for (int c = 0; c < 3; c++)
        amb[c] = fog_now[6 + c], sun[c] = fog_now[9 + c], dir[c] = l > 0 ? fog_now[12 + c] / l : c == 1;
    /* the player's own light (DSR attaches the area's PointLightBank row to the player) */
    if (pfog_rows) {
        const FogRow *r = &pfog_row[fog_area];
        float dx = player_feet.x - pos.x, dy = player_feet.y - pos.y, dz = player_feet.z - pos.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz), b = r->pl_begin * 0.1f, e = r->pl_end * 0.1f;
        float att = e > b ? (e - d) / (e - b) : 0;
        if (att > 0)
            for (int c = 0; c < 3; c++)
                amb[c] += r->pl_rgb[c] * (att < 1 ? att : 1);
    }
    fx_char_light(pos, amb);  /* effect lights: the bonfire's flickering point light (src/fx.c) */
}

void map_char_spec(float env_up[3], float env_down[3], float spec_i[3], float spec_dir[3])
{
    float l = sqrtf(fog_now[24] * fog_now[24] + fog_now[25] * fog_now[25] + fog_now[26] * fog_now[26]);
    for (int c = 0; c < 3; c++) {
        env_up[c] = fog_now[15 + c], env_down[c] = fog_now[18 + c], spec_i[c] = fog_now[21 + c];
        spec_dir[c] = l > 0 ? fog_now[24 + c] / l : c == 1;
    }
}

/* DSR's fog (FRPG shaders, CalcGetFogCoef / CalcGetFogCol): density = saturate(max * saturate((z - begin) /
 * (end - begin))) on view z, mixed towards the fog colour; GS: F = 255 * (1 - density), clamped at 0 when max > 1 */
static void fog_terms(float *a, float *b, float *fmin)
{
    if (pfog_rows)  /* ease into the area's row */
        fog_ease(FOG_RATE);
    float span = fog_now[1] - fog_now[0], mx = fog_now[2];
    if (mx <= 0 || span <= 0) {  /* no DSR fog: fmin = 255 clamps it away, b only sets the edge fog's start */
        *a = 0, *b = 255 / (1 - EDGE0), *fmin = 255;
    } else {
        *a = -255 * mx / span;
        *b = 255 + 255 * mx * fog_now[0] / span;
        *fmin = mx < 1 ? 255 * (1 - mx) : 0;
    }
    /* Edge fog (vu1map2 edge loop): F = min(F, b + a2 z), 0 at FAR. It starts where it drops below the DSR curve:
     * at once if it falls faster than DSR's ramp, else where it meets the curve's floor fmin. ASSUMED shape (linear
     * from the DSR ramp's own intercept b; TECH_DEBT map-15). */
    edge_a2 = -*b / FAR;
    edge_z = edge_a2 <= *a ? 0 : (*fmin - *b) / edge_a2;
}

/* Foliage chunks hold tufts sorted by rank: draw those whose rank is below the density for the chunk's distance
 * (whole tufts; the ranks are spread evenly between the chunk's lowest and highest). Returns triangles to draw. */
static unsigned grass_tris(const Chunk *c, const float *e)
{
    float d2 = 0;
    for (int k = 0; k < 3; k++) {
        float o = e[k] < c->mn[k] ? c->mn[k] - e[k] : e[k] > c->mx[k] ? e[k] - c->mx[k] : 0;
        d2 += o * o;
    }
    float rho = d2 > GRASS_D0 * GRASS_D0 ? GRASS_D0 * GRASS_D0 / d2 : 1;
    float lo = (c->flags >> 16 & 255) * (1 / 255.0f), hi = (c->flags >> 24) * (1 / 255.0f);
    if (lo >= rho)
        return 0;
    if (hi <= rho)
        return c->tris;
    /* a prefix must end on a qword in all three streams (18 / 12 / 12 bytes per triangle): VIF only skips to the
     * next word after an UNPACK, so leftover vertex data in the last qword would run as VIF codes (the full chunk
     * is zero-padded, i.e. VIF NOPs). So: whole tufts, in steps of a multiple of 8 triangles. */
    unsigned per = c->flags >> 8 & 255, step = per;
    while (step & 7)
        step += per;
    unsigned n = (unsigned)(c->tris * (rho - lo) / (hi - lo) + 0.5f * step) / step * step;
    return n < c->tris ? n : c->tris;
}

static int visible_(const Chunk *c, const float *e, int sky);
#ifdef MAP_DBG  /* debug builds: chunks / tris dropped per step, printed every MAP_DBG frames (MAP_DBG_ALL: no
                 * draw-group, distance, subpixel or foliage culling, the reference render to compare with) */
enum { D_DRAWN, D_DG, D_CELL_FAR, D_CELL_SMALL, D_CELL_VIEW, D_FAR, D_SMALL, D_VIEW, D_GRASS, D_TEX, D_SWAP, D_NR };
static const char *const dbg_name[D_NR] = {"drawn", "dg", "cfar", "csmall", "cview", "far", "small", "view", "grass",
                                           "tex", "lod"};
static unsigned dbg_c[D_NR], dbg_t[D_NR];
static int vis_why;  /* visible_'s reason for a 0: D_FAR / D_SMALL / D_VIEW */
/* per chunk (map.bin index, near chunks by their bump.bin number = same numbering): this / last frame's step,
 * 255 = not reached (a near chunk of a cell showing its coarse level); MAP_DBG_FLIP logs the chunks drawn last frame
 * and not now, by step */
#define DBG_IDS 32768
static unsigned char dbg_why[2][DBG_IDS];
static int dbg_cur;
#define DBG(r, n) (dbg_c[r]++, dbg_t[r] += (n))
#define DBGW(r, n, id) (DBG(r, n), (id) < DBG_IDS ? dbg_why[dbg_cur][id] = (r) : 0)
#else
#define DBG(r, n) ((void)0)
#define DBGW(r, n, id) ((void)0)
#endif
static int visible(const Chunk *c, const float *e, int sky)
{
    PROF_BEGIN
    int r = visible_(c, e, sky);
    PROF_END(prof_vis)
    return r;
}

/* view depth of box c's nearest (*zmax: farthest) point and its radius squared */
static float box_depth(const Chunk *c, const float *e, float *zmax, float *r2)
{
    float zc = 0, ext = 0, rr = 0;
    for (int k = 0; k < 3; k++) {
        float h = (c->mx[k] - c->mn[k]) * 0.5f;
        zc += cam[9 + k] * (c->mn[k] + h - e[k]);
        ext += fabsf(cam[9 + k]) * h;
        rr += h * h;
    }
    if (zmax)
        *zmax = zc + ext;
    if (r2)
        *r2 = rr;
    return zc - ext;
}

static int visible_(const Chunk *c, const float *e, int sky)
{
    if (!sky && !(c->flags & CHUNK_BACKDROP)) {  /* beyond FAR the edge fog is complete: blended mist / shafts end there (a fog-coloured card would show
                  * on the sky), opaque pieces at CULL_FAR (a fog-coloured silhouette, like DSR's distant background) */
        float r2, z = box_depth(c, e, NULL, &r2);
#ifndef MAP_DBG_ALL
        float zcut = c->flags & CHUNK_BLEND ? FAR : CULL_FAR;
        if (z > zcut || (z > SMALL_Z && r2 * cam_f * cam_f < SMALL_PX * SMALL_PX * z * z)) {
#ifdef MAP_DBG
            vis_why = z > zcut ? D_FAR : D_SMALL;
#endif
            return 0;
        }
#else
        (void)z;
#endif
    }
#ifdef MAP_DBG
    vis_why = D_VIEW;
#endif
    return r_box_classify(c->mn, c->mx);
}

/* the first n triangles of chunk c (n < c->tris: foliage thinned by distance) */
static int draw_chunk(const unsigned char *base, const Chunk *c, unsigned n, int cls, int mode)
{
    GSTEXTURE *t = NULL;
    float d2 = 0;
    if (c->tex >= 0 && c->tex < ntex) {  /* mip level from the distance to the chunk's box */
        int tx = tex_base + c->tex;
        for (int k = 0; k < 3; k++) {
            float o = cam[k] < c->mn[k] ? c->mn[k] - cam[k] : cam[k] > c->mx[k] ? cam[k] - c->mx[k] : 0;
            d2 += o * o;
        }
        t = tc_get(tx, tc_level(tx, sqrtf(d2)));
        if (!t) {
            DBGW(D_TEX, n, base == streams ? (unsigned)(c - chunks) : NEAR_NO(c, base));
            return 0;  /* texture pool full this frame */
        }
    }
    /* chunks crossing the near plane / guard band (cls 2) go to VU1's clipping program (vu1clip.vsm, F4); sky
     * triangles (1-3 km) never cross the near plane and whatever VU1 rejects of them lies outside the view. map.bin
     * has no untextured chunks (they would be skipped). */
    if (t) {
        int vclip = cls == 2 && !(c->flags & CHUNK_SKY) ? VU1_CLIP : 0, edge = 0;
        if (!(c->flags & CHUNK_SKY)) {  /* reaches into the edge fog: the edge loop of vu1map2 */
            float zmax;
            box_depth(c, cam, &zmax, NULL);
            edge = zmax > edge_z && !(c->flags & CHUNK_BACKDROP) ? VU1_EDGE : 0;  /* the backdrop: DSR's fog only */
        }
        vu1_add_ex(t, c->flags & CHUNK_CULL,
                   (mode & R_FOG ? VU1_FOG : 0) | (mode & R_BLEND ? VU1_BLEND : 0) | (mode & R_FADE ? VU1_FADE : 0) |
                       vclip | edge,
                   n,
                   base + c->pos, base + c->st, base + c->col, c->org,
                   c->flags & CHUNK_ST1K ? 1024.0f : 4096.0f);
        map_vu1_batches++;
        DBGW(D_DRAWN, 0, base == streams ? (unsigned)(c - chunks) : NEAR_NO(c, base));
        if (!(mode & (R_BLEND | R_FADE)) &&
            !(c->flags & (CHUNK_SKY | CHUNK_BLEND)))  /* normal-map relief pass (bump.c: the boot
                                                                            * block, and a world block while it is full) */
            bump_add(cur_inst, base == streams ? (unsigned)(c - chunks) : NEAR_NO(c, base), sqrtf(d2),
                     c->flags & CHUNK_CULL, vclip, n, base + c->pos, base + c->st, base + c->col, c->org,
                     c->flags & CHUNK_ST1K ? 1024.0f : 4096.0f);
    }
    return n;
}

/* Culling index, built on the first draw of a map: non-sky chunks grouped by CELL-metre cells of their centres (a
 * cell is tested once, its chunks only if it is visible: ~5700 sphere tests a frame cost ~4 ms), the sky chunks in a
 * list of their own */
#define CELL 32.0f
typedef struct {
    float mn[3], mx[3];
    unsigned short first, count, flags;  /* flags: CHUNK_BLEND / CHUNK_BACKDROP if any */
    unsigned char near_on;               /* DSM5: drawing the near level (NEAR_DRAW_R with NEAR_HYST hysteresis) */
} Cell;
static const unsigned char *indexed;
static unsigned short *order, *sky;
static Cell *cells;
static int ncells, nsky;

static int cell_key_cmp(const void *a, const void *b)
{
    const unsigned *x = a, *y = b;
    if (x[0] != y[0])
        return x[0] < y[0] ? -1 : 1;
    int tx = chunks[x[1]].tex, ty = chunks[y[1]].tex;  /* inside a cell by texture, so neighbours share TEX0 / CLUT */
    return tx != ty ? (tx < ty ? -1 : 1) : (x[1] < y[1] ? -1 : x[1] > y[1]);
}

static void build_index(void)
{
    if (indexed == map_bin)
        return;
    free(order), free(sky), free(cells);
    unsigned n = hdr->chunks;
    unsigned (*key)[2] = malloc(n * sizeof *key);
    order = malloc(n * sizeof *order);
    sky = malloc(n * sizeof *sky);
    int m = 0;
    nsky = 0;
    for (unsigned i = 0; i < n; i++) {
        const Chunk *c = &chunks[i];
        if (c->flags & CHUNK_SKY) {
            sky[nsky++] = i;
            continue;
        }
        unsigned k = 0;
        for (int a = 0; a < 3; a++)
            k = k << 10 | (((int)((c->mn[a] + c->mx[a]) * 0.5f / CELL + 512)) & 1023);
        key[m][0] = k, key[m++][1] = i;
    }
    cells = malloc(((nctab > (unsigned)m ? nctab : m) + 1) * sizeof *cells);
    ncells = 0;
    if (ctab) {  /* DSM5: the exporter's cells, resident chunks already grouped per cell (sky chunks after them) */
        for (unsigned i = 0; i < nctab; i++) {
            Cell *ce = &cells[ncells++];
            memcpy(ce->mn, ctab[i].mn, sizeof ce->mn), memcpy(ce->mx, ctab[i].mx, sizeof ce->mx);
            ce->first = ctab[i].first, ce->count = ctab[i].count, ce->flags = 0, ce->near_on = 0;
            for (unsigned j = ce->first; j < ce->first + ce->count; j++)
                order[j] = j, ce->flags |= chunks[j].flags & (CHUNK_BLEND | CHUNK_BACKDROP);
        }
        m = 0;
    }
    qsort(key, m, sizeof *key, cell_key_cmp);
    for (int j = 0; j < m; j++) {
        const Chunk *c = &chunks[key[j][1]];
        order[j] = key[j][1];
        if (!j || key[j][0] != key[j - 1][0]) {
            Cell *ce = &cells[ncells++];
            memcpy(ce->mn, c->mn, sizeof ce->mn), memcpy(ce->mx, c->mx, sizeof ce->mx);
            ce->first = j, ce->count = 0, ce->flags = 0, ce->near_on = 0;
        }
        Cell *ce = &cells[ncells - 1];
        ce->count++;
        ce->flags |= c->flags & (CHUNK_BLEND | CHUNK_BACKDROP);
        for (int a = 0; a < 3; a++) {
            ce->mn[a] = c->mn[a] < ce->mn[a] ? c->mn[a] : ce->mn[a];
            ce->mx[a] = c->mx[a] > ce->mx[a] ? c->mx[a] : ce->mx[a];
        }
    }
    free(key);
    indexed = map_bin;
}

/* ---- world blocks (src/world.c): one instance per loaded map block, swapped into the globals by map_select ---- */
typedef struct {
    const unsigned char *map_bin, *streams;
    const Header *hdr;
    const MapCell *ctab;
    unsigned nctab;
    const Chunk *chunks;
    const FogRow *fog_row;
    const unsigned char *fog_tri;
    unsigned fog_rows, fog_ntris;
    const float *navv;
    const unsigned short *navt;
    int ntex, tex_base;
    const unsigned char *indexed;
    unsigned short *order, *sky;
    Cell *cells;
    int ncells, nsky;
    int on, col_block;
} MapInst;
static MapInst inst[MAP_INST] = {{.on = 0}};
/* a world block's dg.bin, at boot (src/world.c world_systems; the pack read waits on the IOP): kept for the session
 * (Burg west 44 KB), handed to drawgrp.c each time the block turns on */
static void *blk_dg[MAP_INST];

static void save_inst(MapInst *m)
{
    m->map_bin = map_bin, m->streams = streams, m->hdr = hdr, m->ctab = ctab, m->nctab = nctab, m->chunks = chunks;
    m->fog_row = fog_row, m->fog_tri = fog_tri, m->fog_rows = fog_rows, m->fog_ntris = fog_ntris;
    m->navv = navv, m->navt = navt, m->ntex = ntex, m->tex_base = tex_base;
    m->indexed = indexed, m->order = order, m->sky = sky, m->cells = cells, m->ncells = ncells, m->nsky = nsky;
}

static void load_inst(const MapInst *m)
{
    map_bin = m->map_bin, streams = m->streams, hdr = m->hdr, ctab = m->ctab, nctab = m->nctab, chunks = m->chunks;
    fog_row = m->fog_row, fog_tri = m->fog_tri, fog_rows = m->fog_rows, fog_ntris = m->fog_ntris;
    navv = m->navv, navt = m->navt, ntex = m->ntex, tex_base = m->tex_base;
    indexed = m->indexed, order = m->order, sky = m->sky, cells = m->cells, ncells = m->ncells, nsky = m->nsky;
}

static void map_select(int k)
{
    if (k == cur_inst)
        return;
    save_inst(&inst[cur_inst]);
    load_inst(&inst[cur_inst = k]);
    mapstream_select(k);
}

/* the collision block of instance k (0 for the boot area), -1 while it is off */
static int inst_col_block(int k)
{
    return k == 0 ? inst[0].col_block : inst[k].on ? inst[k].col_block : -1;
}

static int map_inst_of(int col_block)
{
    if (!inst[0].on && !cur_inst)  /* single area: everything is instance 0 */
        return col_block == 0 ? 0 : -1;
    for (int k = 0; k < MAP_INST; k++)
        if ((k == 0 || inst[k].on) && inst[k].col_block == col_block)
            return k;
    return -1;
}

int map_block_init(int k, const void *tab, const void *res, int col_block)
{
    if (k < 0 || k >= MAP_INST)
        return 0;
    const Header *h = tab;
    if (!h || memcmp(h->magic, "DSM5", 4) || !h->cell_off)
        return 0;
    int was = cur_inst;
    map_select(k);
    if (k) {  /* a fresh instance: no index yet */
        indexed = NULL, order = sky = NULL, cells = NULL, ncells = nsky = 0;
    }
    /* tab = the 80-byte header + map.bin from the chunk table on (tools/mapsplit.py): offsets >= chunk_off resolve
     * against a base chunk_off - 80 bytes before it */
    map_bin = (const unsigned char *)tab + sizeof(Header) - h->chunk_off;
    hdr = h;
    ctab = (const MapCell *)(map_bin + hdr->cell_off + 16);
    nctab = *(const unsigned *)(map_bin + hdr->cell_off);
    chunks = (const Chunk *)(map_bin + hdr->chunk_off);
    fog_rows = *(const unsigned *)(map_bin + hdr->fog_off);
    fog_ntris = ((const unsigned *)(map_bin + hdr->fog_off))[1];
    fog_row = (const FogRow *)(map_bin + hdr->fog_off + 16);
    fog_tri = (const unsigned char *)(fog_row + fog_rows);
    navv = (const float *)(map_bin + hdr->navv_off);
    navt = (const unsigned short *)(map_bin + hdr->navt_off);
    streams = res;
    ntex = hdr->textures < MAX_TEX ? hdr->textures : MAX_TEX;
    inst[k].on = 1, inst[k].col_block = col_block;
    if (!k) {  /* the boot block: fog / light rows and the texture pool as map_init / map_upload */
        float f0[FOG_NOW] = {hdr->fog_begin, hdr->fog_end, hdr->fog_max, hdr->fog_rgb[0], hdr->fog_rgb[1],
                             hdr->fog_rgb[2], 64, 64, 64, 96, 96, 96, 0.40f * 127, 0.80f * 127, -0.45f * 127};
        memcpy(fog_now, f0, sizeof fog_now);
        fog_area = 0;
        pfog_row = fog_row, pfog_rows = fog_rows;
        if (fog_rows)
            fog_ease(1);
        tex_base = 0;
    } else {
        tex_base = tc_block_add(k - 1, map_bin, map_bin + hdr->tex_off, ntex);
        if (tex_base < 0) {
            printf("map: block %d: no texture slot / CLUT VRAM\n", k);
            inst[k].on = 0;
            map_select(was);
            return 0;
        }
        tex_res(k);
        dg_block_init(k, blk_dg[k], hdr->chunks, (unsigned)col_block_tris(col_block));  /* per-block draw groups */
    }
    map_select(was);
    return 1;
}

void map_block_streams(int k, const void *res)
{
    if (k < 0 || k >= MAP_INST)
        return;
    int was = cur_inst;
    map_select(k);
    streams = res;
    if (k || inst[0].on)  /* instance 0 of a world build (single-area builds never call this) */
        tex_res(k);
    if (k && !res)
        bump_block(k, NULL, 0);  /* its bump.bin lay in the full pool behind the streams */
    map_select(was);
}

void map_block_bump(int k, const void *blob)
{
    if (k <= 0 || k >= MAP_INST || !inst[k].on)
        return;
    int was = cur_inst;
    map_select(k);
    int on = bump_block(k, blob, hdr->chunks);
    map_select(was);
    if (blob)
        printf("map: block %d relief %s\n", k, on ? "on" : "off");
}

void map_block_drop(int k)
{
    if (k <= 0 || k >= MAP_INST || !inst[k].on)
        return;
    int was = cur_inst;
    map_select(k);
    bump_block(k, NULL, 0);
    tc_block_drop(k - 1);
    dg_block_drop(k);
    mapstream_close(k);
    free(order), free(sky), free(cells);
    indexed = NULL, order = sky = NULL, cells = NULL, ncells = nsky = 0;
    if (pfog_row == fog_row)  /* the player's rows were this block's: back to the boot block's */
        pfog_row = inst[0].fog_row ? inst[0].fog_row : NULL, pfog_rows = inst[0].fog_row ? inst[0].fog_rows : 0,
        fog_area = 0;
    hdr = NULL, ctab = NULL, nctab = 0, streams = NULL, ntex = 0;
    inst[k].on = 0;
    map_select(was == k ? 0 : was);
}

void map_block_dg(int k, void *pack)
{
    if (k > 0 && k < MAP_INST && pack && !blk_dg[k])
        blk_dg[k] = pack_load(pack, "dg.bin", NULL);
}

int map_block_stream_open(int k, const void *pack)
{
    if (k < 0 || k >= MAP_INST || !inst[k].on)
        return 0;
    int was = cur_inst;
    map_select(k);
    int r = ctab ? mapstream_open_pack(k, pack, ctab, nctab) : 0;
    map_select(was);
    return r;
}

float map_near_r = NEAR_DRAW_R, map_near_fade = NEAR_FADE;
static struct { float d; const Chunk *c; int cls, k; } fx[MAX_FX];
static int nfx;
static struct { unsigned short ci; unsigned char k, lvl, a; } band[MAX_BAND];
static int nband;

/* one cell's chunks: lvl 0 its level (near when nb, else coarse), 1 only its coarse chunks, 2 only its near chunks;
 * blended chunks of lvl 0 go to fx */
static int draw_cell(int k, int ci, const Cell *ce, const unsigned char *nb, int lvl, const float *e, int mode)
{
    int drawn = 0;
    unsigned nnear = nb ? ctab[ci].near : 0, j0 = streams && lvl != 2 ? ce->first : ce->first + ce->count;
    near_gid = nb ? ctab[ci].pad : 0;
    for (unsigned j = j0; j < ce->first + ce->count + (lvl == 1 ? 0 : nnear); j++) {
        int is_near = j >= (unsigned)ce->first + ce->count;
        const Chunk *c = is_near ? (const Chunk *)nb + (j - ce->first - ce->count) : &chunks[order[j]];
        if (nb && !is_near && (c->flags & CHUNK_COARSE)) {
            DBGW(D_SWAP, 0, order[j]);
            continue;
        }
        if (lvl && (c->flags & CHUNK_BLEND || (lvl == 1 && !(c->flags & CHUNK_COARSE))))
            continue;  /* fade pass: only the other level's opaque chunks */
#ifndef MAP_DBG_ALL
        if (!dg_shown(cur_inst, is_near ? NEAR_NO(c, nb) : order[j])) {  /* MSB draw groups (drawgrp.c) */
            DBGW(D_DG, c->tris, is_near ? NEAR_NO(c, nb) : order[j]);
            continue;
        }
        unsigned n = c->flags & CHUNK_GRASS ? grass_tris(c, e) : c->tris;
#ifdef MAP_PANO_R
    {
        float d2 = 0;
        for (int k = 0; k < 3; k++) {
            float o = e[k] < c->mn[k] ? c->mn[k] - e[k] : e[k] > c->mx[k] ? e[k] - c->mx[k] : 0;
            d2 += o * o;
        }
        if (d2 < MAP_PANO_R * MAP_PANO_R)
            n = 0;
    }
#endif
#else
        unsigned n = c->flags & CHUNK_GRASS ? c->tris : c->tris;
#endif
        DBG(D_GRASS, c->tris - n);
        if (!n) {
            DBGW(D_GRASS, 0, is_near ? NEAR_NO(c, nb) : order[j]);
            continue;
        }
        int cls = visible(c, e, 0);
        if (!cls) {
#ifdef MAP_DBG
            DBGW(vis_why, n, is_near ? NEAR_NO(c, nb) : order[j]);
#endif
            continue;
        }
        if (c->flags & CHUNK_BLEND) {
            if (nfx < MAX_FX) {
                float d = 0;
                for (int k = 0; k < 3; k++) {
                    float m = (c->mn[k] + c->mx[k]) * 0.5f - e[k];
                    d += m * m;
                }
                fx[nfx].d = d, fx[nfx].c = c, fx[nfx].k = k, fx[nfx++].cls = cls;
            }
            continue;
        }
        drawn += draw_chunk(is_near ? nb : streams, c, n, cls, mode);
    }
    return drawn;
}

int map_draw(V3 eye)
{
    GSGLOBAL *gs = gsg;
    int drawn = 0;
    if (!inst[pinst].on && pinst)
        pinst = 0;
    /* sky and fog colours from the player's block; while it is lite (a full swap in flight) the sky of a full one */
    int sk = pinst;
    if (!(pinst == cur_inst ? streams : inst[pinst].streams))
        for (int k = 0; k < MAP_INST; k++)
            if ((k == 0 || inst[k].on) && (k == cur_inst ? streams : inst[k].streams)) {
                sk = k;
                break;
            }
    map_select(sk);
    build_index();
    bump_owner(pinst);  /* relief for the player's block (bump.c: one CLUT area) */
    map_vu1_batches = map_ee_tris = 0;
#ifdef MAP_DBG
    static int dbg_init;
    if (!dbg_init++)
        memset(dbg_why, 255, sizeof dbg_why);
#endif
    const float e[3] = {eye.x, eye.y, eye.z};
    {
        V3 ey, rt, up, fw;
        float fx, fy, zs;
        r_camera_basis(&ey, &rt, &up, &fw, &fx, &fy, &zs);
        const float b[12] = {ey.x, ey.y, ey.z, rt.x, rt.y, rt.z, up.x, up.y, up.z, fw.x, fw.y, fw.z};
        memcpy(cam, b, sizeof cam);
        cam_f = fx;
        tc_begin(fx);  /* this frame's texture uploads go into the chain from here on */
    }
    float a, b, fmin;
    fog_terms(&a, &b, &fmin);
    int fog = R_FOG;  /* the edge fog needs FGE even where the row has no fog */
    u32 col = (u32)fog_now[3] | (u32)fog_now[4] << 8 | (u32)fog_now[5] << 16;
    u32 sky_col = hdr->sky_fog[0] | hdr->sky_fog[1] << 8 | hdr->sky_fog[2] << 16;
    float sky_f = 255 - hdr->sky_fog[3];

    /* sky first: the FogBank row of the sky part at its maximum density, constant over the dome */
    vu1_map_fog(0, sky_f, sky_f, sky_f);
    u64 r[2] = {sky_col, 0x3D};
    vu1_gs_regs(1, r);
    r_set_fog(0, sky_f, sky_f, sky_f, sky_col, col);
    r_set_fog_edge(0);
    for (int pass = 0; pass < 2 && streams; pass++) {  /* the player's block's sky (a lite block has none loaded) */
        if (pass)
            blend_regs(gs, CHUNK_BLEND);
        for (int i = 0; i < nsky; i++) {
            const Chunk *c = &chunks[sky[i]];
            if (!(c->flags & CHUNK_BLEND) != !pass || (c->flags & CHUNK_BACKDROP) ||
                !dg_shown(cur_inst, sky[i]))
                continue;
#ifdef MAP_PANO_R
            continue;  /* panorama capture: no sky (the magenta clear marks the panorama's transparent texels) */
#endif
            int cls = visible(c, e, 1);
            if (cls)
                drawn += draw_chunk(streams, c, c->tris, cls, R_FOG | (pass ? R_BLEND : 0));
        }
    }
    blend_regs(gs, 0);
    /* the far panorama (export_map.py PANO: sky chunks with CHUNK_BACKDROP, a cylinder textured with the far map
     * rendered offline, DSR's fog baked in): over the sky, unfogged, alpha tested */
    for (int i = 0, any = 0; i < pano_n && !cur_inst; i++) {
        const Chunk *c = &pano_chunk[i];
        int cls = visible(c, e, 1);
        GSTEXTURE *t = cls ? tc_get(pano_tex + c->tex, 0) : NULL;
        if (!t)
            continue;
        if (!any++)
            vu1_map_fog(0, 255, 255, 255);
        vu1_add_ex(t, 0, VU1_FOG | (cls == 2 ? VU1_CLIP : 0), c->tris, pano + c->pos, pano + c->st, pano + c->col,
                   c->org, c->flags & CHUNK_ST1K ? 1024.0f : 4096.0f);
        drawn += c->tris;
    }

    /* opaque map with the map's main fog row, and the edge fog */
    vu1_map_fog(a, b, fmin, 255);
    vu1_map_edge(edge_a2);
    r[0] = col;
    vu1_gs_regs(1, r);
    r_set_fog(a, b, fmin, 255, col, col);
    r_set_fog_edge(edge_a2);
    nfx = nband = 0;
    for (int k = 0; k < MAP_INST; k++) {  /* every loaded block (src/world.c); one instance in a single-area build */
        if (k && !inst[k].on)
            continue;
        map_select(k);
        if (!hdr)
            continue;
        build_index();
        for (int ci = 0; ci < ncells; ci++) {
            Cell *ce = &cells[ci];
            /* the cell's box, tested as opaque (out to CULL_FAR); its blended chunks end at FAR on their own test. A
             * cell holding backdrop chunks is only frustum tested, its other chunks get their own distance test (the
             * cell cut dropped whole far-only cells beyond 250 m: the sky holes and floating wall tops of map-39) */
            Chunk cb = {0, ce->flags & CHUNK_BACKDROP, 0, 0, 0, 0, {ce->mn[0], ce->mn[1], ce->mn[2]},
                        {ce->mx[0], ce->mx[1], ce->mx[2]}};
            if (!visible(&cb, e, 0)) {
#ifdef MAP_DBG
                for (unsigned j = ce->first; j < ce->first + ce->count; j++)
                    DBGW(vis_why - D_FAR + D_CELL_FAR, chunks[order[j]].tris, order[j]);
#endif
                continue;
            }
            /* DSM5: the near level when its blob is loaded (then the coarse chunks are skipped), else the coarse one */
            const unsigned char *nb = NULL;
            if (ctab) {
                float d2 = 0;
                for (int k = 0; k < 3; k++) {
                    float o = e[k] < ce->mn[k] ? ce->mn[k] - e[k] : e[k] > ce->mx[k] ? e[k] - ce->mx[k] : 0;
                    d2 += o * o;
                }
                /* a lite block (no resident streams) draws its near cells as far as they are loaded */
                float f = map_near_fade, r = map_near_r + (f > 0 ? f : ce->near_on ? NEAR_HYST : -NEAR_HYST);
                ce->near_on = r > 0 && d2 < r * r;
                nb = ce->near_on || !streams ? mapstream_get(ci) : NULL;
                float d;
                if (nb && streams && f > 0 && (d = sqrtf(d2)) > map_near_r - f && nband < MAX_BAND) {
                    float s = (d - (map_near_r - f)) / (2 * f);  /* 0..1 across the band */
                    int outer = s >= 0.5f, a = (int)((outer ? 2 - 2 * s : 2 * s) * 128 + 0.5f);
                    if (a > 2)
                        band[nband].ci = ci, band[nband].k = k, band[nband].lvl = outer ? 2 : 1, band[nband++].a = a;
                    if (outer)
                        nb = NULL;  /* the coarse level is the opaque one */
                }
            }
            drawn += draw_cell(k, ci, ce, nb, 0, e, fog);
        }
    }
    map_select(0);
    bump_draw();  /* relief over the opaque map (bump.c), before the blended effects */
    /* the fading level of the band cells, over the finished opaque map and its relief
     * (Z tested, not written) */
    if (nband) {
        u64 zr[4] = {0, 0x42, GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0) | (1ull << 32), 0x4E};
        for (int i = 0; i < nband; i++) {
            map_select(band[i].k);
            const unsigned char *nb = mapstream_get(band[i].ci);
            if (!nb)
                continue;
            zr[0] = GS_SETREG_ALPHA(0, 1, 2, 1, band[i].a);  /* (Cs - Cd) x FIX + Cd */
            vu1_gs_regs(2, zr);
            drawn += draw_cell(band[i].k, band[i].ci, &cells[band[i].ci], band[i].lvl == 2 ? nb : NULL, band[i].lvl, e,
                               fog | R_FADE);
        }
        blend_regs(gs, 0);
    }
    map_select(0);

    /* effects after the opaque map: alpha-blended (mist) far to near, then additive (light shafts, order-free);
     * Z tested, not written */
    for (int i = 1; i < nfx; i++)  /* insertion sort by distance, far first; a few dozen per frame */
        for (int j = i; j > 0 && fx[j].d > fx[j - 1].d; j--) {
            __typeof__(fx[0]) tmp = fx[j];
            fx[j] = fx[j - 1], fx[j - 1] = tmp;
        }
    for (int add = 0; add < 2; add++) {
        int any = 0;
        for (int i = 0; i < nfx; i++) {
            const Chunk *c = fx[i].c;
            if (!(c->flags & CHUNK_ADD) != !add)
                continue;
            if (!any++)
                blend_regs(gs, CHUNK_BLEND | (add ? CHUNK_ADD : 0));
            map_select(fx[i].k);
            drawn += draw_chunk(streams, c, c->tris, fx[i].cls, fog | R_BLEND | (add ? R_ADD : 0));
        }
    }
    map_select(0);
    blend_regs(gs, 0);
    tc_end();
#ifdef MAP_PANO_R  /* panorama capture: the camera basis for tools/pano.py, every 8 frames */
    {
        static unsigned pf;
        if (!(++pf & 7))
            printf("PANOCAM %.3f %.3f %.3f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.2f\n", cam[0], cam[1],
                   cam[2], cam[3], cam[4], cam[5], cam[6], cam[7], cam[8], cam[9], cam[10], cam[11], cam_f, 0.0f, 0.0f, 0.0f);
    }
#endif
#ifdef MAP_DBG
    {
        static unsigned fr;
        dbg_t[D_DRAWN] = drawn;
        if (!(++fr % MAP_DBG)) {
            char line[320];
            int k = snprintf(line, sizeof line, "MAPDBG %u", fr);
            for (int r = 0; r < D_NR; r++)
                k += snprintf(line + k, sizeof line - k, " %s %u/%u", dbg_name[r], dbg_c[r], dbg_t[r]);
            puts(line);
        }
        memset(dbg_c, 0, sizeof dbg_c), memset(dbg_t, 0, sizeof dbg_t);
#ifdef MAP_DBG_FLIP
        {  /* chunks drawn last frame, not now: by this frame's step (view = left the frustum: expected when turning) */
            unsigned cnt[D_NR + 1] = {0}, tris[D_NR + 1] = {0}, on = 0;
            const unsigned char *now = dbg_why[dbg_cur], *was = dbg_why[!dbg_cur];
            char line[400];
            int k = snprintf(line, sizeof line, "FLIP %u", fr);
            for (unsigned i = 0; i < DBG_IDS; i++) {
                if (was[i] == D_DRAWN && now[i] != D_DRAWN) {
                    int r = now[i] < D_NR ? now[i] : D_NR;
                    cnt[r]++;
                    if (i < hdr->chunks)
                        tris[r] += chunks[i].tris;
                    if (r != D_VIEW && r != D_CELL_VIEW && r != D_SWAP && r != D_NR && k < 300)
                        k += snprintf(line + k, sizeof line - k, " [%u %s]", i, dbg_name[r]);
                } else if (was[i] != D_DRAWN && now[i] == D_DRAWN)
                    on++;
            }
            k += snprintf(line + k, sizeof line - k, " | on %u off", on);
            for (int r = 1; r <= D_NR; r++)
                if (cnt[r])
                    k += snprintf(line + k, sizeof line - k, " %s %u/%u", r < D_NR ? dbg_name[r] : "unseen", cnt[r],
                                  tris[r]);
            puts(line);
        }
#endif
        dbg_cur = !dbg_cur;
        memset(dbg_why[dbg_cur], 255, DBG_IDS);
    }
#endif
#ifdef MAP_STATS  /* A/B builds: batches, texture pool traffic and streamed cells, every 60 frames */
    {
        static unsigned fr;
        if (!(++fr % 60))
            printf("MAPSTATS batches %d tris %d tc uploads %d bytes %d fails %d stream cells %u KB %u\n",
                   map_vu1_batches, drawn, tc_stats.uploads, tc_stats.bytes, tc_stats.fails, mapstream_stats.cells,
                   mapstream_stats.bytes >> 10);
    }
#endif
#ifdef MAP_PROF
    {
        static unsigned frames, v, c;
        v += prof_vis, c += prof_clip;
        prof_vis = prof_clip = 0;
        if (++frames == 30) {  /* average over 30 frames, in 0.01 ms */
            map_vu1_batches = v / 30 / 2949, map_ee_tris = c / 30 / 2949;
            frames = v = c = 0;
        }
        static int sv, sc;
        if (frames)
            map_vu1_batches = sv, map_ee_tris = sc;
        else
            sv = map_vu1_batches, sc = map_ee_tris;
    }
#endif
    return drawn;
}

int map_floor(float x, float z, float y, float *out_y)
{
    int found = 0;
    float best = 0;
    for (unsigned i = 0; i < hdr->nav_tris; i++) {
        const float *a = &navv[navt[i * 3] * 3], *b = &navv[navt[i * 3 + 1] * 3], *c = &navv[navt[i * 3 + 2] * 3];
        /* barycentric test in the x/z plane */
        float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
        if (d > -1e-6f && d < 1e-6f)
            continue;
        float l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
        float l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
        float l3 = 1 - l1 - l2;
        if (l1 < 0 || l2 < 0 || l3 < 0)
            continue;
        float h = l1 * a[1] + l2 * b[1] + l3 * c[1];
        if (h > y + STEP_UP || h < y - STEP_DOWN)
            continue;
        if (!found || (h - y) * (h - y) < (best - y) * (best - y))
            best = h, found = 1;
    }
    if (found)
        *out_y = best;
    return found;
}
