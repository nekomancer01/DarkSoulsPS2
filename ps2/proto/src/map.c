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
#include "pvs.h"
#include "pack.h"

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
    unsigned char dof_begin, dof_end, dof_mul, pad;   /* far depth of field: DofBank row, metres / % (0: none) */
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

/* a near chunk's number in bump.bin / dg.bin: flags bits 8-31; foliage keeps its tuft bits there, so its number is
 * the cell's first near number (MapCell.pad, near_gid, set per cell in map_draw) + its index in the blob
 * (export_map.py) */
#define NEAR_NO(c, blob) ((c)->flags & CHUNK_GRASS ? near_gid + (unsigned)((c) - (const Chunk *)(blob)) : (c)->flags >> 8)

/* Culling index, built on the first draw of a map (build_index): non-sky chunks grouped by CELL-metre cells of their
 * centres (a cell is tested once, its chunks only if it is visible: ~5700 sphere tests a frame cost ~4 ms), the sky
 * chunks in a list of their own */
#define CELL 32.0f
typedef struct {
    float mn[3], mx[3];
    unsigned short first, count, flags;  /* flags: CHUNK_BLEND / CHUNK_BACKDROP if any */
    unsigned char near_on;               /* DSM5: drawing the near level (NEAR_DRAW_R with NEAR_HYST hysteresis) */
} Cell;

/* One map instance: instance 0 is the single-area map (map_init) or the boot world block, 1.. the other loaded world
 * blocks (src/world.c, map_block_init). Every function that works on one takes it explicitly. streams = the base of
 * the resident chunks' vertex streams (the whole map.bin of a single-area build), NULL for a world block: res_cells,
 * its resident level streams per cell (mapstream_res: cell i's streams, the sky's at nctab) and its textures' mip 0
 * per texture (TECH_DEBT world-2 / world-5: no full swap). tex_base = its first texcache index. mapstream.c still
 * answers mapstream_get / mapstream_res for its selected instance, so the code that asks selects it first. */
typedef struct {
    const unsigned char *map_bin, *streams;  /* map.bin (main.c, from the area pack: 128-byte aligned, kept loaded) */
    const Header *hdr;
    const MapCell *ctab;  /* DSM5 cell table (resident range + streamed near blob per cell), else NULL */
    unsigned nctab;
    const Chunk *chunks;
    const FogRow *fog_row;
    const unsigned char *fog_tri;
    unsigned fog_rows, fog_ntris;
    const float *navv;
    const unsigned short *navt;
    int ntex, tex_base;
    const unsigned char *indexed;  /* the map.bin the index below was built for */
    unsigned short *order, *sky;
    Cell *cells;
    int ncells, nsky;
    int res_cells;
    unsigned short *citem;  /* res_cells: per resident chunk its mapstream item (res_open) */
    unsigned (*rrange)[3];  /* res_cells: per mapstream item its chunks [first, first + n) and cell (n 0: texture / sky) */
    unsigned nritem;
    int on, col_block;
    int pkt;  /* VU1_PKT: the chunk streams are VIF packets (DSM5 cell table flags bit 0, export_map.py CELLS_VIFPKT) */
} MapInst;
static MapInst inst[MAP_INST] = {{.on = 0}};

static V3 player_feet;
/* the fog drawn now (begin, end, max, r, g, b) and the character light (ambient rgb, sun rgb, sun direction xyz),
 * moving towards fog_row[fog_area] */
#define FOG_NOW 27
static float fog_now[FOG_NOW] = {0, 0, 0, 0, 0, 0, 64, 64, 64, 96, 96, 96, 0.40f * 127, 0.80f * 127, -0.45f * 127};
/* + specular: env up rgb [15], env down rgb [18], specular light rgb [21], its direction x 127 [24] (0: none) */
static int fog_area;
static GSGLOBAL *gsg;
int map_vu1_batches, map_ee_tris;
/* The fog / light rows follow the instance the player stands in (pfog_*) */
static const FogRow *pfog_row;
static unsigned pfog_rows;
static int map_inst_of(int col_block);
static int inst_col_block(int k);
static int pinst;  /* the instance the player stands in (sky, fog rows) */

/* instance m's streams for resident chunk j, or NULL while its item is not in (mapstream.c: m selected) */
static const unsigned char *res_base(const MapInst *m, unsigned j)
{
    return m->res_cells ? mapstream_res(m->citem[j]) : m->streams;
}


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
    MapInst *m = &inst[0];
    const unsigned char *map_bin = m->map_bin = blob;
    const Header *hdr = m->hdr = (const Header *)map_bin;
    if (memcmp(hdr->magic, "DSM4", 4) && memcmp(hdr->magic, "DSM5", 4))
        return 0;
    m->ctab = hdr->magic[3] == '5' && hdr->cell_off ? (const MapCell *)(map_bin + hdr->cell_off + 16) : NULL;
    m->nctab = m->ctab ? *(const unsigned *)(map_bin + hdr->cell_off) : 0;
    m->pkt = m->ctab && ((const unsigned *)(map_bin + hdr->cell_off))[1] & 1 ? VU1_PKT : 0;
    m->chunks = (const Chunk *)(map_bin + hdr->chunk_off);
    m->fog_rows = *(const unsigned *)(map_bin + hdr->fog_off);
    m->fog_ntris = ((const unsigned *)(map_bin + hdr->fog_off))[1];
    m->fog_row = (const FogRow *)(map_bin + hdr->fog_off + 16);
    m->fog_tri = (const unsigned char *)(m->fog_row + m->fog_rows);
    /* no rows: the header's fog and the old fixed character light (0.5 + 0.75 n . L) */
    float f0[FOG_NOW] = {hdr->fog_begin, hdr->fog_end, hdr->fog_max, hdr->fog_rgb[0], hdr->fog_rgb[1], hdr->fog_rgb[2],
                          64, 64, 64, 96, 96, 96, 0.40f * 127, 0.80f * 127, -0.45f * 127};
    memcpy(fog_now, f0, sizeof fog_now);
    fog_area = 0;
    pfog_row = m->fog_row, pfog_rows = m->fog_rows;
    if (m->fog_rows)
        fog_ease(1);
    m->navv = (const float *)(map_bin + hdr->navv_off);
    m->navt = (const unsigned short *)(map_bin + hdr->navt_off);
    m->streams = map_bin, m->tex_base = 0;
    return 1;
}

/* world blocks: the selected instance's split textures have no mip 0 until mapstream.c reads it (tex_hook_set) */
static void tex_res(int k)
{
    tc_res(k ? map_tex_slot(k) : -1, inst[k].streams);
}

/* mapstream.c's mip 0 hooks (mapstream_tex_hooks): tag = the texture's index in instance k's table */
static unsigned tex_hook_age(int k, unsigned tag)
{
    int slot = k ? map_tex_slot(k) : -1;
    return k && slot < 0 ? ~0u : tc_res_age(slot, (int)tag);
}

static void tex_hook_set(int k, unsigned tag, const void *p)
{
    int slot = k ? map_tex_slot(k) : -1;
    if (!k || slot >= 0)
        tc_res_one(slot, (int)tag, p);
}

int map_upload(GSGLOBAL *gs)
{
    gsg = gs;
    MapInst *m = &inst[0];
    m->ntex = m->hdr->textures < MAX_TEX ? m->hdr->textures : MAX_TEX;
    /* the textures stream through a VRAM pool (texcache.c: uploads in the VIF1 chain, LRU); the CLUTs stay resident */
    int r = tc_init(gs, m->map_bin, m->map_bin + m->hdr->tex_off, m->ntex, TEX_POOL_KB);
    if (!r)
        tex_res(0);
    return r;
}

unsigned map_chunk_count(void)
{
    return inst[0].hdr ? inst[0].hdr->chunks : 0;
}

void map_spawn(V3 *pos, float *yaw)
{
    const Header *hdr = inst[0].hdr;
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

/* mapstream.c's item visibility (pvs.c): item id of instance k (a near cell part, then a resident item) has a chunk
 * some camera cell within PVS_STREAM_R sees; textures, the sky and maps without pvs.bin count as seen */
static int ms_pvs_vis(int k, unsigned id)
{
    if (k < 0 || k >= MAP_INST || !inst[k].ctab)
        return 1;
    const MapInst *m = &inst[k];
    if (id < m->nctab) {
        unsigned n = m->ctab[id].near & ~CELL_PART;
        return !n || pvs_range(k, id, m->ctab[id].pad, n, 0);
    }
    id -= m->nctab;
    if (!m->rrange || id >= m->nritem || !m->rrange[id][1])
        return 1;
    return pvs_range(k, m->rrange[id][2], m->rrange[id][0], m->rrange[id][1], 0);
}

int map_stream_open(const char *pak_path)
{
    pano_open(pak_path);
    mapstream_vis_hooks(ms_pvs_vis, pvs_gen);
    return inst[0].ctab ? mapstream_init(pak_path, inst[0].ctab, inst[0].nctab) : 0;
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
        const MapInst *m = &inst[k];
        pinst = k;
        if (m->fog_rows && m->fog_ntris == (unsigned)col_block_tris(h.block) && (unsigned)h.tri < m->fog_ntris &&
            m->fog_tri[h.tri] < m->fog_rows) {
            pfog_row = m->fog_row, pfog_rows = m->fog_rows;
            fog_area = m->fog_tri[h.tri];
        }
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

int map_far_dof(float *begin, float *end, float *mul)
{
    if (!pfog_rows)
        return 0;
    const FogRow *r = &pfog_row[fog_area];
    *begin = r->dof_begin, *end = r->dof_end, *mul = r->dof_mul * 0.01f;
    return r->dof_mul > 0 && r->dof_end > r->dof_begin;
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
enum { D_DRAWN, D_DG, D_PVS, D_CELL_FAR, D_CELL_SMALL, D_CELL_VIEW, D_FAR, D_SMALL, D_VIEW, D_GRASS, D_TEX, D_SWAP, D_NR };
static const char *const dbg_name[D_NR] = {"drawn", "dg", "pvs", "cfar", "csmall", "cview", "far", "small", "view", "grass",
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
/* no: the chunk's number in bump.bin / dg.bin (resident: its index in the chunk table; near: NEAR_NO) */
static int draw_chunk(const MapInst *m, const unsigned char *base, const Chunk *c, unsigned n, int cls, int mode,
                      unsigned no)
{
    GSTEXTURE *t = NULL;
    float d2 = 0;
    if (c->tex >= 0 && c->tex < m->ntex) {  /* mip level from the distance to the chunk's box */
        int tx = m->tex_base + c->tex;
        for (int k = 0; k < 3; k++) {
            float o = cam[k] < c->mn[k] ? c->mn[k] - cam[k] : cam[k] > c->mx[k] ? cam[k] - c->mx[k] : 0;
            d2 += o * o;
        }
        t = tc_get(tx, tc_level(tx, sqrtf(d2)));
        if (!t) {
            DBGW(D_TEX, n, no);
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
        float st_k = c->flags & CHUNK_ST1K ? 1024.0f : 4096.0f;
        if (!(mode & R_BLEND) && !(c->flags & (CHUNK_SKY | CHUNK_BLEND)) && c->tex >= 0) {  /* GS mip chain (render-12) */
            u64 mt;
            int k, mxl = tc_mip(m->tex_base + c->tex, t, st_k, &mt, &k);
            if (mxl)
                vu1_mip(mxl, k, mt);
        }
        int pkt = n == c->tris ? m->pkt : 0;  /* a foliage prefix sends its streams one by one */
        vu1_add_ex(t, c->flags & CHUNK_CULL,
                   (mode & R_FOG ? VU1_FOG : 0) | (mode & R_BLEND ? VU1_BLEND : 0) | vclip | edge | pkt, n,
                   base + c->pos, base + c->st, base + c->col, c->org,
                   c->flags & CHUNK_ST1K ? 1024.0f : 4096.0f);
        map_vu1_batches++;
        DBGW(D_DRAWN, 0, no);
        if (!(mode & R_BLEND) && !(c->flags & (CHUNK_SKY | CHUNK_BLEND)))  /* normal-map relief pass (bump.c: the boot
                                                                            * block, and a world block while it is full) */
            bump_add((int)(m - inst), no, sqrtf(d2),
                     c->flags & CHUNK_CULL, vclip | pkt, n, base + c->pos, base + c->st, base + c->col, c->org,
                     c->flags & CHUNK_ST1K ? 1024.0f : 4096.0f);
    }
    return n;
}

/* index sort key: cell, then texture inside a cell (so neighbours share TEX0 / CLUT), then chunk number */
static int cell_key_cmp(const void *a, const void *b)
{
    const unsigned *x = a, *y = b;
    if (x[0] != y[0])
        return x[0] < y[0] ? -1 : 1;
    int tx = (int)x[1], ty = (int)y[1];
    return tx != ty ? (tx < ty ? -1 : 1) : (x[2] < y[2] ? -1 : x[2] > y[2]);
}

static void build_index(MapInst *mi)
{
    if (mi->indexed == mi->map_bin)
        return;
    const Chunk *chunks = mi->chunks;
    free(mi->order), free(mi->sky), free(mi->cells);
    unsigned n = mi->hdr->chunks;
    unsigned (*key)[3] = malloc(n * sizeof *key);
    unsigned short *order = mi->order = malloc(n * sizeof *order);
    unsigned short *sky = mi->sky = malloc(n * sizeof *sky);
    int m = 0, nsky = 0;
    for (unsigned i = 0; i < n; i++) {
        const Chunk *c = &chunks[i];
        if (c->flags & CHUNK_SKY) {
            sky[nsky++] = i;
            continue;
        }
        unsigned k = 0;
        for (int a = 0; a < 3; a++)
            k = k << 10 | (((int)((c->mn[a] + c->mx[a]) * 0.5f / CELL + 512)) & 1023);
        key[m][0] = k, key[m][1] = (unsigned)c->tex, key[m++][2] = i;
    }
    mi->nsky = nsky;
    Cell *cells = mi->cells = malloc(((mi->nctab > (unsigned)m ? mi->nctab : m) + 1) * sizeof *cells);
    int ncells = 0;
    if (mi->ctab) {  /* DSM5: the exporter's cells, resident chunks already grouped per cell (sky chunks after them) */
        const MapCell *ctab = mi->ctab;
        for (unsigned i = 0; i < mi->nctab; i++) {
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
        const Chunk *c = &chunks[key[j][2]];
        order[j] = key[j][2];
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
    mi->ncells = ncells;
    mi->indexed = mi->map_bin;
}

/* ---- world blocks (src/world.c): one MapInst per loaded map block ---- */
/* a world block's dg.bin, at boot (src/world.c world_systems; the pack read waits on the IOP): kept for the session
 * (Burg west 44 KB), handed to drawgrp.c each time the block turns on */
static void *blk_dg[MAP_INST], *blk_pvs[MAP_INST];
/* texcache slot of instance k > 0: the first one free when it turns on. More instances than TC_BLOCKS (the Asylum is
 * a fifth block) are fine: world.c has at most WORLD_SLOTS lite sets loaded, and only loaded blocks are instances */
static signed char tslot[MAP_INST];

int map_tex_slot(int k)
{
    return k > 0 && k < MAP_INST && inst[k].on ? tslot[k] : -1;
}

/* the collision block of instance k (0 for the boot area), -1 while it is off */
static int inst_col_block(int k)
{
    return k == 0 ? inst[0].col_block : inst[k].on ? inst[k].col_block : -1;
}

static int map_inst_of(int col_block)
{
    if (!inst[0].on)  /* single area: everything is instance 0 */
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
    MapInst *m = &inst[k];
    if (k) {  /* a fresh instance: no index yet */
        m->indexed = NULL, m->order = m->sky = NULL, m->cells = NULL, m->ncells = m->nsky = 0;
    }
    /* tab = the 80-byte header + map.bin from the chunk table on (tools/mapsplit.py): offsets >= chunk_off resolve
     * against a base chunk_off - 80 bytes before it */
    const unsigned char *map_bin = m->map_bin = (const unsigned char *)tab + sizeof(Header) - h->chunk_off;
    const Header *hdr = m->hdr = h;
    m->ctab = (const MapCell *)(map_bin + hdr->cell_off + 16);
    m->nctab = *(const unsigned *)(map_bin + hdr->cell_off);
    m->pkt = ((const unsigned *)(map_bin + hdr->cell_off))[1] & 1 ? VU1_PKT : 0;
    m->chunks = (const Chunk *)(map_bin + hdr->chunk_off);
    m->fog_rows = *(const unsigned *)(map_bin + hdr->fog_off);
    m->fog_ntris = ((const unsigned *)(map_bin + hdr->fog_off))[1];
    m->fog_row = (const FogRow *)(map_bin + hdr->fog_off + 16);
    m->fog_tri = (const unsigned char *)(m->fog_row + m->fog_rows);
    m->navv = (const float *)(map_bin + hdr->navv_off);
    m->navt = (const unsigned short *)(map_bin + hdr->navt_off);
    m->streams = res, m->res_cells = 0, m->citem = NULL;
    m->ntex = hdr->textures < MAX_TEX ? hdr->textures : MAX_TEX;
    m->on = 1, m->col_block = col_block;
    if (!k) {  /* the boot block: fog / light rows and the texture pool as map_init / map_upload */
        float f0[FOG_NOW] = {hdr->fog_begin, hdr->fog_end, hdr->fog_max, hdr->fog_rgb[0], hdr->fog_rgb[1],
                             hdr->fog_rgb[2], 64, 64, 64, 96, 96, 96, 0.40f * 127, 0.80f * 127, -0.45f * 127};
        memcpy(fog_now, f0, sizeof fog_now);
        fog_area = 0;
        pfog_row = m->fog_row, pfog_rows = m->fog_rows;
        if (m->fog_rows)
            fog_ease(1);
        m->tex_base = 0;
    } else {
        int used = 0;
        for (int j = 1; j < MAP_INST; j++)
            if (j != k && inst[j].on)
                used |= 1 << tslot[j];
        tslot[k] = 0;
        while (tslot[k] < TC_BLOCKS - 1 && used >> tslot[k] & 1)
            tslot[k]++;
        m->tex_base = used >> tslot[k] & 1 ? -1 : tc_block_add(tslot[k], map_bin, map_bin + hdr->tex_off, m->ntex);
        if (m->tex_base < 0) {
            printf("map: block %d: no texture slot / CLUT VRAM\n", k);
            m->on = 0;
            return 0;
        }
        tex_res(k);
        dg_block_init(k, blk_dg[k], hdr->chunks, (unsigned)col_block_tris(col_block));  /* per-block draw groups */
        pvs_block_init(k, blk_pvs[k], hdr->chunks, m->nctab);  /* and visibility (src/pvs.c) */
    }
    return 1;
}

void map_block_bump(int k, const void *blob)
{
    if (k <= 0 || k >= MAP_INST || !inst[k].on)
        return;
    int on = bump_block(k, blob, inst[k].hdr->chunks);
    if (blob)
        printf("map: block %d relief %s\n", k, on ? "on" : "off");
}

void map_block_drop(int k)
{
    if (k <= 0 || k >= MAP_INST || !inst[k].on)
        return;
    MapInst *m = &inst[k];
    bump_block(k, NULL, 0);
    mapstream_close(k);  /* before the slot's textures go: it hands their mip 0 back (tex_hook_set) */
    tc_block_drop(tslot[k]);
    dg_block_drop(k);
    pvs_block_drop(k);
    free(m->order), free(m->sky), free(m->cells);
    m->indexed = NULL, m->order = m->sky = NULL, m->cells = NULL, m->ncells = m->nsky = 0;
    if (pfog_row == m->fog_row)  /* the player's rows were this block's: back to the boot block's */
        pfog_row = inst[0].fog_row ? inst[0].fog_row : NULL, pfog_rows = inst[0].fog_row ? inst[0].fog_rows : 0,
        fog_area = 0;
    free(m->citem), free(m->rrange);
    m->rrange = NULL, m->nritem = 0;
    m->hdr = NULL, m->ctab = NULL, m->nctab = 0, m->streams = NULL, m->ntex = 0, m->res_cells = 0, m->citem = NULL;
    m->on = 0;
    mapstream_select(0);
}

void map_pvs_init(const void *blob)
{
    if (inst[0].hdr)
        pvs_block_init(0, blob, inst[0].hdr->chunks, inst[0].nctab);
}

void map_block_dg(int k, void *pack)
{
    if (k > 0 && k < MAP_INST && pack && !blk_dg[k])
        blk_dg[k] = pack_load(pack, "dg.bin", NULL);
#ifndef NO_PVS
    if (k > 0 && k < MAP_INST && pack && !blk_pvs[k])
        blk_pvs[k] = pack_load(pack, "pvs.bin", NULL);
#endif
}

/* bytes of the streams of resident chunks [a, b) (export_map.py writes pos, st, colour per chunk, each 16-byte
 * aligned, chunk after chunk in table order, each after a VIF code qword: VU1_PKT): [*lo, return) in mapres.bin */
static unsigned chunk_range(const Chunk *chunks, unsigned a, unsigned b, unsigned *lo)
{
    unsigned l = ~0u, h = 0;
    for (unsigned j = a; j < b; j++) {
        const Chunk *c = &chunks[j];
        unsigned e = (c->col + c->tris * 12 + 15) & ~15u;  /* colour u8x4 per corner, the last stream */
        l = c->pos - 16 < l ? c->pos - 16 : l;  /* from pos's code qword (VU1_PKT sends from there) */
        h = e > h ? e : h;
    }
    *lo = l == ~0u ? 0 : l;
    return h;
}

/* World blocks: the selected instance k's resident level as mapstream items (mapstream_res_open) instead of one
 * mapres.bin in a full pool: per cell the streams of its resident chunks (grouped by cell in the chunk table) in
 * items of whole chunks up to RES_ITEM bytes, the sky chunks' the same way, and each texture's mip 0 that
 * tools/mapsplit.py moved into mapres.bin. A block without it draws its near cells only. RES_ITEM: one cell up to
 * 2.2 MB as one item found no hole with 4 MB of the arena free (largest hole 0.9-1 MB; ISO runs 2026-11-05); ASSUMED
 * size, small enough to fit the holes near cells leave. */
#define RES_ITEM (64u << 10)  /* one page of the arena (mapstream.c MS_PAGE, plan step 6; 128 KB before) */
static void res_open(int k, const void *pack)
{
    MapInst *mi = &inst[k];
    const Header *hdr = mi->hdr;
    const Chunk *chunks = mi->chunks;
    const MapCell *ctab = mi->ctab;
    unsigned nctab = mi->nctab;
    unsigned roff, rsize;
    if (!pack_entry_raw((Pack *)pack, "mapres.bin", &roff, &rsize) || !rsize)
        return;
    unsigned n = hdr->chunks + 1 + (unsigned)mi->ntex;
    MapResItem *it = malloc(n * sizeof *it);
    unsigned short *citem = mi->citem = malloc((hdr->chunks + 1) * sizeof *citem);
    unsigned (*rr)[3] = mi->rrange = malloc(n * sizeof *rr);
    if (!it || !citem || !rr || n > 65535) {
        free(it), free(citem), free(rr);
        mi->citem = NULL, mi->rrange = NULL;
        return;
    }
    unsigned m = 0, bad = 0, last = 0;
    for (unsigned i = 0; i <= nctab; i++) {  /* cells, then (i = nctab) the sky chunks after them (res_order) */
        unsigned a = i < nctab ? ctab[i].first : last, e = i < nctab ? a + ctab[i].count : hdr->chunks;
        if (i < nctab)
            last = e > last ? e : last;
        for (unsigned j = a; j < e;) {  /* whole chunks [j, g) up to RES_ITEM bytes (at least one) */
            unsigned lo, hi = chunk_range(chunks, j, j + 1, &lo), g = j + 1, l2, h2;
            /* coarse chunks in items of their own (MS_RES_COARSE: they go while the cell's near level is in), and
             * foliage too (MS_RES_GRASS: only near the player) */
            unsigned coarse = chunks[j].flags & (CHUNK_COARSE | CHUNK_GRASS);
            while (g < e && (chunks[g].flags & (CHUNK_COARSE | CHUNK_GRASS)) == coarse &&
                   (h2 = chunk_range(chunks, g, g + 1, &l2)) - lo <= RES_ITEM && l2 >= lo)
                hi = h2 > hi ? h2 : hi, g++;
            unsigned far = 0;
            for (unsigned c = j; c < g; c++)
                citem[c] = (unsigned short)m, far |= chunks[c].flags & CHUNK_BACKDROP;
            rr[m][0] = j, rr[m][1] = i < nctab ? g - j : 0, rr[m][2] = i;
            it[m++] = (MapResItem){roff + lo, lo, hi - lo,
                                   i < nctab ? i | (far ? MS_RES_FAR : 0) |
                                                   ((coarse & CHUNK_COARSE) && ctab[i].near ? MS_RES_COARSE : 0) |
                                                   (coarse & CHUNK_GRASS ? MS_RES_GRASS : 0)
                                             : MS_RES_SKY};
            bad |= hi > rsize;
            j = g;
        }
    }
    const unsigned char *te = mi->map_bin + hdr->tex_off;
    for (int t = 0; t < mi->ntex; t++) {  /* texmap.py entry: u16 w, h; u8 psm; ...; u32 pix[0] at +16 */
        const unsigned char *e = te + 32 * t;
        unsigned w = *(const unsigned short *)e, h = *(const unsigned short *)(e + 2), p0 = *(const unsigned *)(e + 16);
        if (!(p0 & TC_RES))
            continue;
        unsigned b = e[4] == GS_PSM_T4 ? w * h / 2 : w * h;
        rr[m][0] = rr[m][1] = rr[m][2] = 0;
        it[m++] = (MapResItem){roff + (p0 & ~TC_RES), p0 & ~TC_RES, b, MS_RES_TEX | (unsigned)t};
        bad |= (p0 & ~TC_RES) + b > rsize;
    }
    if (bad)
        printf("map: block %d: mapres.bin (%u KB) shorter than its chunk / texture records\n", k, rsize >> 10);
    else if (mi->nritem = m, mapstream_res_open(k, it, nctab, m)) {
        mi->res_cells = 1;
        mapstream_tex_hooks(tex_hook_age, tex_hook_set);
    }
    if (!mi->res_cells)
        free(citem), free(rr), mi->citem = NULL, mi->rrange = NULL, mi->nritem = 0;
    free(it);
}

int map_block_stream_open(int k, const void *pack)
{
    if (k < 0 || k >= MAP_INST || !inst[k].on)
        return 0;
    const MapInst *m = &inst[k];
    mapstream_select(k);
    int r = m->ctab ? mapstream_open_pack(k, pack, m->ctab, m->nctab) : 0;
    mapstream_vis_hooks(ms_pvs_vis, pvs_gen);
    if (r && !m->streams)
        res_open(k, pack);
    mapstream_select(0);  /* mapstream_open_pack leaves instance k selected */
    return r;
}

/* instance k has a map and its sky's streams are in (the first sky chunk's item); selects k in mapstream.c */
static int sky_in(int k)
{
    MapInst *m = &inst[k];
    mapstream_select(k);
    if (!m->hdr)
        return 0;
    build_index(m);
    return m->nsky && res_base(m, m->sky[0]);
}

int map_draw(V3 eye)
{
    GSGLOBAL *gs = gsg;
    int drawn = 0;
    if (!inst[pinst].on && pinst)
        pinst = 0;
    /* sky and fog colours from the player's block; while its sky streams are not in yet the sky of one that has them */
    int sk = pinst;
    if (!sky_in(pinst))
        for (int k = 0; k < MAP_INST; k++)
            if ((k == 0 || inst[k].on) && sky_in(k)) {
                sk = k;
                break;
            }
    MapInst *m = &inst[sk];
    mapstream_select(sk);
    build_index(m);
    bump_owner(pinst);  /* relief for the player's block (bump.c: one CLUT area) */
    map_vu1_batches = map_ee_tris = 0;
#ifdef MAP_DBG
    static int dbg_init;
    if (!dbg_init++)
        memset(dbg_why, 255, sizeof dbg_why);
#endif
    const float e[3] = {eye.x, eye.y, eye.z};
    pvs_eye(e);  /* every instance's visibility row for this camera cell (src/pvs.c) */
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
    u32 sky_col = m->hdr->sky_fog[0] | m->hdr->sky_fog[1] << 8 | m->hdr->sky_fog[2] << 16;
    float sky_f = 255 - m->hdr->sky_fog[3];

    /* sky first: the FogBank row of the sky part at its maximum density, constant over the dome */
    vu1_map_fog(0, sky_f, sky_f, sky_f);
    u64 r[2] = {sky_col, 0x3D};
    vu1_gs_regs(1, r);
    r_set_fog(0, sky_f, sky_f, sky_f, sky_col, col);
    r_set_fog_edge(0);
    for (int pass = 0; pass < 2; pass++) {  /* the player's block's sky (its streams once they are in) */
        if (pass)
            blend_regs(gs, CHUNK_BLEND);
        for (int i = 0; i < m->nsky; i++) {
            const unsigned no = m->sky[i];
            const Chunk *c = &m->chunks[no];
            if (!(c->flags & CHUNK_BLEND) != !pass || (c->flags & CHUNK_BACKDROP) || !dg_shown(sk, no))
                continue;
#ifdef MAP_PANO_R
            continue;  /* panorama capture: no sky (the magenta clear marks the panorama's transparent texels) */
#endif
            const unsigned char *sb = res_base(m, no);
            int cls = sb ? visible(c, e, 1) : 0;
            if (cls)
                drawn += draw_chunk(m, sb, c, c->tris, cls, R_FOG | (pass ? R_BLEND : 0), no);
        }
    }
    blend_regs(gs, 0);
    /* the far panorama (export_map.py PANO: sky chunks with CHUNK_BACKDROP, a cylinder textured with the far map
     * rendered offline, DSR's fog baked in): over the sky, unfogged, alpha tested */
    for (int i = 0, any = 0; i < pano_n && !sk; i++) {
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

    /* opaque map with the map's main fog row, and the edge fog; TEST for the batches with a mip chain (vu1_mip: they
     * send MIPTBP1 in its place) */
    vu1_map_fog(a, b, fmin, 255);
    vu1_map_edge(edge_a2);
    r[0] = col;
    vu1_gs_regs(1, r);
    {
        u64 rt[2] = {GS_SETREG_TEST(1, 5, 0x40, 0, 0, 0, 1, 2), 0x47};
        vu1_gs_regs(1, rt);
    }
    r_set_fog(a, b, fmin, 255, col, col);
    r_set_fog_edge(edge_a2);
    static struct { float d; const Chunk *c; const unsigned char *base; int cls, k; unsigned no; } fx[MAX_FX];
    int nfx = 0;
    unsigned holes = 0;  /* visible cells with resident chunks not in and no near level drawn for them */
    unsigned pvs_cut = 0;  /* triangles the visibility rows left out (src/pvs.c) */
    unsigned k_drawn[MAP_INST] = {0}, k_cut[MAP_INST] = {0};  /* the same and the drawn ones per instance (A/B log) */
    for (int k = 0; k < MAP_INST; k++) {  /* every loaded block (src/world.c); one instance in a single-area build */
        if (k && !inst[k].on)
            continue;
        m = &inst[k];
        mapstream_select(k);  /* mapstream_get / mapstream_res below answer for k */
        if (!m->hdr)
            continue;
        build_index(m);
        const Chunk *chunks = m->chunks;
        const MapCell *ctab = m->ctab;
        const unsigned short *order = m->order;
        for (int ci = 0; ci < m->ncells; ci++) {
            Cell *ce = &m->cells[ci];
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
            /* DSM5: the near level when its blob is loaded (then the coarse chunks are skipped), else the coarse one;
             * a world block's resident chunks once their item's streams are in (mapstream_res) */
            if (ctab && (ctab[ci].near & CELL_PART))
                continue;  /* a later part of the cell before it: drawn with it */
            const unsigned char *nb = NULL, *nbp[NEAR_PARTS] = {0};
            unsigned np = 1, nnear = 0;  /* the cell's near parts (export_map.py NEAR_PART), their chunks */
            int miss = 0;  /* a resident chunk of the cell is not in yet: its near cell draws as far as it is loaded */
            int nd = 0;    /* the near level is drawn (every part in, or unseen: pvs.c) */
            for (unsigned j = ce->first; m->res_cells && j < (unsigned)ce->first + ce->count; j++)
                /* (mapstream_res also ranks it up: the cell is in view); foliage is resident only near the player
                 * (mapstream.c MS_GRASS_WANT) and thinned to nothing farther: its absence is no hole */
                if (!res_base(m, order[j]) && !(chunks[order[j]].flags & CHUNK_GRASS) &&
                    pvs_chunk(k, (unsigned)ci, order[j]))  /* a chunk the camera cannot see is not read (pvs.c) */
                    miss = 1;
            if (ctab) {
                float d2 = 0;
                for (int k = 0; k < 3; k++) {
                    float o = e[k] < ce->mn[k] ? ce->mn[k] - e[k] : e[k] > ce->mx[k] ? e[k] - ce->mx[k] : 0;
                    d2 += o * o;
                }
                /* without the cell's resident streams its near cells draw as far as they are loaded */
                float r = ce->near_on ? NEAR_DRAW_R + NEAR_HYST : NEAR_DRAW_R - NEAR_HYST;
                ce->near_on = r > 0 && d2 < r * r;
                while (np < NEAR_PARTS && ci + np < (unsigned)m->ncells && (ctab[ci + np].near & CELL_PART))
                    np++;
                /* every part asked for (mapstream ranks them up), drawn once all are in: the coarse chunks stand
                 * for the whole cell until then */
                int all = ce->near_on || miss;
                for (unsigned p = 0; p < np; p++)
                    all &= (nbp[p] = ce->near_on || miss ? mapstream_get(ci + p) : NULL) != NULL ||
                           (all && !pvs_range(k, (unsigned)ci, ctab[ci + p].pad, ctab[ci + p].near & ~CELL_PART, 1));
                /* (a part no chunk of which the camera sees is not read: the near level draws without it) */
                nd = all;
                nb = nd ? nbp[0] : NULL;
                for (unsigned p = 0; nd && p < np; p++)
                    nnear += ctab[ci + p].near & ~CELL_PART;
            }
            holes += miss && !nd;
            unsigned near_gid = nd ? ctab[ci].pad : 0;
            unsigned part = 0, part_end = nd ? ctab[ci].near : 0;  /* the near part of chunk j, its last index + 1 */
            for (unsigned j = m->streams || m->res_cells ? ce->first : ce->first + ce->count;
                 j < ce->first + ce->count + nnear; j++) {
                int is_near = j >= (unsigned)ce->first + ce->count;
                const unsigned char *rb = is_near ? NULL : res_base(m, order[j]);
                if (!is_near && !rb)
                    continue;  /* its streams are not in */
                unsigned nj = j - ce->first - ce->count;
                while (is_near && nj >= part_end && part + 1 < np) {  /* into the next part's blob */
                    part++;
                    nb = nbp[part], near_gid = ctab[ci + part].pad;
                    part_end += ctab[ci + part].near & ~CELL_PART;
                }
                if (is_near && !nb)
                    continue;  /* an unseen part that is not in (pvs.c) */
                const Chunk *c = is_near ? (const Chunk *)nb + (nj - (part_end - (ctab[ci + part].near & ~CELL_PART)))
                                         : &chunks[order[j]];
                if (nd && !is_near && (c->flags & CHUNK_COARSE)) {
                    DBGW(D_SWAP, 0, order[j]);
                    continue;
                }
#ifndef MAP_DBG_ALL
                if (!dg_shown(k, is_near ? NEAR_NO(c, nb) : order[j])) {  /* MSB draw groups (drawgrp.c) */
                    DBGW(D_DG, c->tris, is_near ? NEAR_NO(c, nb) : order[j]);
                    continue;
                }
                if (!pvs_chunk(k, (unsigned)ci, is_near ? NEAR_NO(c, nb) : order[j])) {  /* behind something (pvs.c) */
                    DBGW(D_PVS, c->tris, is_near ? NEAR_NO(c, nb) : order[j]);
                    pvs_cut += c->tris, k_cut[k] += c->tris;
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
                        fx[nfx].d = d, fx[nfx].c = c, fx[nfx].k = k, fx[nfx].cls = cls;
                        fx[nfx].base = is_near ? nb : rb, fx[nfx++].no = is_near ? NEAR_NO(c, nb) : order[j];
                    }
                    continue;
                }
                {
                    unsigned dn = draw_chunk(m, is_near ? nb : rb, c, n, cls, fog, is_near ? NEAR_NO(c, nb) : order[j]);
                    drawn += dn, k_drawn[k] += dn;
                }
            }
        }
    }
    mapstream_select(0);
    bump_draw();  /* relief over the opaque map (bump.c), before the blended effects */

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
            drawn += draw_chunk(&inst[fx[i].k], fx[i].base, c, c->tris, fx[i].cls, fog | R_BLEND | (add ? R_ADD : 0),
                                fx[i].no);
        }
    }
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
                    if (i < inst[0].hdr->chunks)
                        tris[r] += inst[0].chunks[i].tris;
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
    {  /* streaming A/B (world streaming v2): the most cells missing in one frame over each second, when any were */
        static unsigned fr, worst, cut_max;
        worst = holes > worst ? holes : worst;
        cut_max = pvs_cut > cut_max ? pvs_cut : cut_max;
        if (!(++fr % 30)) {
            if (worst)
                printf("map: cells missing %u (most in a frame over 1 s)\n", worst);
            if (cut_max)  /* occlusion A/B (src/pvs.c): this frame's per-instance drawn / culled counts */
                printf("map: pvs culled %u tris (most in a frame over 1 s), eye %.1f %.1f %.1f; now drawn / culled %u/%u "
                       "%u/%u %u/%u\n", cut_max, (double)e[0], (double)e[1], (double)e[2], k_drawn[0], k_cut[0],
                       MAP_INST > 1 ? k_drawn[1] : 0, MAP_INST > 1 ? k_cut[1] : 0, MAP_INST > 2 ? k_drawn[2] : 0,
                       MAP_INST > 2 ? k_cut[2] : 0);
            worst = cut_max = 0;
        }
    }
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
    const Header *hdr = inst[0].hdr;
    const float *navv = inst[0].navv;
    const unsigned short *navt = inst[0].navt;
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
