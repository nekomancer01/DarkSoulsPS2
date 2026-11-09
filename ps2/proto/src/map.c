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
#include "hitch.h"
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
/* tris | nv << 16 on disc: nv = 0: unindexed lists (pos / st / col: one entry per corner); nv > 0: an indexed chunk
 * (tools/export_map.py index_chunk): nv shared vertices, then a triangle stream after the colours (chunk_idx) */
typedef struct { int tex; unsigned flags; unsigned short tris, nv; unsigned pos, st, col; float mn[3], mx[3], org[3], step; } Chunk;
/* offset of an indexed chunk's triangle stream (u8 x4 per triangle, after its own VIF code qword) */
#define CHUNK_IDX(c) ((c)->col + (((c)->nv * 4u + 15) & ~15u) + 16)

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
/* Horizon (resident far backdrop, tools/horizon.py appended to a world block's maptab.bin): per cell a very coarse
 * untextured level, loaded with the block. A cell beyond HZN_R (+- HZN_HYST) of the camera draws it in place of its
 * coarse level and kept opaque meshes (hz_covers), and so does a nearer cell whose coarse level is not in yet: a late
 * read shows a blurrier cell, not a hole. mapstream.c reads those chunks only within MS_R_HZN (64 m) of the player's
 * feet: HZN_R + HZN_HYST + the camera's ~5 m stay inside it. ASSUMED 40 m (horizon.py judges its error from 38 m; 56 / 54 m before the Burg plan's B3).
 * HZN=0 at build time: no horizon section, the old drawing; make DEFS=-DHZN_OFF=1 ignores one at runtime. */
#ifndef HZN_R
#define HZN_R 40.0f  /* (Burg plan B3, 2026-11-08: 56 m before; the export's hard tier holds the coarse level to 40 m) */
#endif
#define HZN_HYST 2.0f
/* the horizon fades in over the coarse level through the HZN_FADE metres inside HZN_R (GS alpha FIX, after the opaque
 * map), so the switch at HZN_R swaps two pictures that already look alike instead of popping (ASSUMED 8 m: about a
 * second at walking speed). make DEFS=-DHZN_FADE=0: the hard switch */
#ifndef HZN_FADE
#define HZN_FADE 8.0f
#endif
#define MAX_HZ_FADE 64  /* cells fading in per frame */
/* the fading horizon is depth tested as if HZN_ZBIAS of its distance nearer: it is the coarse surface simplified
 * (horizon.py measures it within HZN_PX 8 px of 568: 1.4 % of the distance), so where it lies just behind the coarse
 * level it still blends instead of leaving the coarse level unfaded. ASSUMED 3 % (about twice that error, deviations
 * seen up to ~60 degrees off the view ray). 0: the plain depth test */
#ifndef HZN_ZBIAS
#define HZN_ZBIAS 0.03f
#endif
/* make DEFS=-DHZN_FADE_DEBUG=1: the fade pass draws the horizon solid magenta (full fog in that colour, alpha 1), so a
 * shot shows which pixels of the fading cells it reaches (coarse level left showing = unfaded) */
#ifndef HZN_FADE_DEBUG
#define HZN_FADE_DEBUG 0
#endif
#define CHUNK_HZN 256
#define CHUNK_HZN_TRIS 54  /* triangles per horizon chunk at most (horizon.py CHUNK_TRIS) */
#define hz_covers(fl) (!((fl) & (CHUNK_BLEND | CHUNK_SKY | CHUNK_GRASS)))
/* horizon.py, 32 B; flags bit 0: the cell has chunks it stands for (its horizon may have simplified to nothing),
 * bit 1: some of its chunks come from the backdrop (CHUNK_BACKDROP: never distance culled) */
typedef struct { float mn[3], mx[3]; unsigned short first, count; unsigned flags; } HzCell;
#define NEAR_HYST 2.0f  /* m: a cell switches to its near level inside NEAR_DRAW_R - NEAR_HYST, back beyond + NEAR_HYST
                         * (no flipping near <-> coarse while the camera hovers at the radius) */
#ifndef NEAR_DRAW_R
#define NEAR_DRAW_R 18.0f  /* (Burg plan B3: + NEAR_HYST = mapstream.c MS_R0_DRAW 20 m; 24 m before) m: a loaded near blob is drawn only for cells whose box is this close to the camera (the
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
    unsigned char waiting;               /* pop-in metric: in view inside the near radius, drawn coarse (not in yet) */
    unsigned char hz_on;                 /* beyond HZN_R (with HZN_HYST hysteresis): its horizon stands for it */
    unsigned short seen, wait0;          /* the map draw (pop_fr) it was last in view, the one its wait began */
    unsigned char lv, lv_prev;           /* hitch.c: level drawn (1 hole, 2 horizon, 3 coarse, 4 near), the one before */
    unsigned short lv_seen, lv_fr;       /* the draw it was last in view, the draw its level last changed */
    /* MAP_TWO_PASS: the cell's draw list (chunks that pass the level / stream / draw-group / visibility-row filters)
     * kept from the draw that built it: valid while lgen is the list pool's generation and lkey the cell's key */
    unsigned lkey[2], lgen, lcut;        /* key, pool generation, triangles the visibility rows left out */
    unsigned short loff, ln;             /* its entries in the pool */
} Cell;
/* MAP_TILES (budget doc proposal 6, default on; make DEFS=-DMAP_TILES=0 = every cell tested): the cells grouped by
 * MAP_TILE_M x MAP_TILE_M metre squares of their box centres (x, z), each group's box (cells and horizons) tested
 * first. The Burg's 5 blocks have ~1230 cells and ~200-550 of them in view: the cells of a tile out of view are no
 * longer tested one by one (~1 us each, with the box set-up) */
#ifndef MAP_TILES
#define MAP_TILES 1
#endif
#define MAP_TILE_M 128.0f
typedef struct {
    float mn[3], mx[3];
    unsigned short first, count;  /* into MapInst.tcell */
    unsigned flags;               /* CHUNK_BACKDROP if a cell's test has it (no distance cut) */
} MapTile;
/* Pop-in (streaming A/B, 2026-11-06): a cell in view whose near level is due (near_on) but not in draws its coarse
 * level; once the near level arrives while the cell is still in view, the swap is seen: a pop. map_draw counts them
 * (waits of 2 draws or more) and prints "map: near pop-in" lines (tools/test-run.py --popin-max fails a run on them). */
static unsigned short pop_fr;
static unsigned pop_total;

/* One map instance: instance 0 is the single-area map (map_init) or the boot world block, 1.. the other loaded world
 * blocks (src/world.c, map_block_init). Every function that works on one takes it explicitly. streams = the base of
 * the resident chunks' vertex streams (the whole map.bin of a single-area build), NULL for a world block: res_cells,
 * its resident level streams per cell (mapstream_res: cell i's streams, the sky's at nctab) and its textures' mip 0
 * per texture (TECH_DEBT world-2 / world-5: no full swap). tex_base = its first texcache index. mapstream.c still
 * answers mapstream_get / mapstream_res for its selected instance, so the code that asks selects it first. */
typedef struct {
    const unsigned char *map_bin, *streams;  /* map.bin (main.c, from the area pack: 128-byte aligned, kept loaded) */
    /* map.bin bases for the textures / navmesh / fog rows and the cell table / horizon: map_bin, or those of the
     * pieces a world block's maptab.bin was read in (map_block_split, world.c WORLD_TAB_SPLIT) */
    const unsigned char *tex_bin, *cel_bin;
    const Header *hdr;
    const MapCell *ctab;  /* DSM5 cell table (resident range + streamed near blob per cell), else NULL */
    const short *ckeys;   /* its cell keys (mapstream.h cell_keys: MS_NEAR_CUBE), else NULL */
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
    MapTile *tiles;          /* MAP_TILES: groups of cells, their cells (lead cells only: no CELL_PART) */
    unsigned short *tcell;
    int ntiles;
    int res_cells;
    unsigned short *citem;  /* res_cells: per resident chunk its mapstream item (res_open) */
    unsigned char *rflag;   /* res_cells: per mapstream item RI_COV (every chunk hz_covers) / RI_GRASS (foliage) */
    unsigned (*rrange)[3];  /* res_cells: per mapstream item its chunks [first, first + n) and cell (n 0: texture / sky) */
    unsigned nritem;
    int on, col_block;
    int pkt;  /* VU1_PKT: the chunk streams are VIF packets (DSM5 cell table flags bit 0, export_map.py CELLS_VIFPKT) */
    /* horizon (HZN_R): per cell table entry its box and chunks, the chunk records, the base their offsets add to */
    const HzCell *hz_cell;
    const Chunk *hz_chunk;
    const unsigned char *hz_base;
    int hz_only;  /* world.c WORLD_KEPT_HZN: a kept-only block, its horizon drawn alone (map_block_lean) */
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

/* the horizon section the DSM5 cell table header points at (words 2 / 3: offset, bytes; tools/mapsplit.py), if any */
static void hz_parse(MapInst *m)
{
    m->hz_cell = NULL, m->hz_chunk = NULL, m->hz_base = NULL;
#ifndef HZN_OFF
    if (!m->ctab)
        return;
    const unsigned *ch = (const unsigned *)(m->cel_bin + m->hdr->cell_off);
    if (!ch[2] || ch[3] < 16)
        return;
    const unsigned char *b = m->cel_bin + ch[2];
    const unsigned *h = (const unsigned *)b;
    if (memcmp(b, "HZN1", 4) || h[2] != m->nctab || h[3] != ch[3] || ((unsigned)b & 15)) {
        printf("map: horizon section does not match the cell table (ignored)\n");
        return;
    }
    m->hz_cell = (const HzCell *)(b + 16);
    m->hz_chunk = (const Chunk *)(b + 16 + 32 * m->nctab);
    m->hz_base = b;
    printf("map: horizon %u chunks, %u KB\n", h[1], ch[3] >> 10);
#endif
}

int map_init(const void *blob)
{
    if (!blob)
        return 0;
    MapInst *m = &inst[0];
    const unsigned char *map_bin = m->map_bin = m->tex_bin = m->cel_bin = blob;
    const Header *hdr = m->hdr = (const Header *)map_bin;
    if (memcmp(hdr->magic, "DSM4", 4) && memcmp(hdr->magic, "DSM5", 4))
        return 0;
    m->ctab = hdr->magic[3] == '5' && hdr->cell_off ? (const MapCell *)(map_bin + hdr->cell_off + 16) : NULL;
    m->nctab = m->ctab ? *(const unsigned *)(map_bin + hdr->cell_off) : 0;
    m->ckeys = cell_keys(m->ctab, m->nctab);
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
    hz_parse(m);
    return 1;
}

/* world blocks: the selected instance's split textures have no mip 0 until mapstream.c reads it (tex_hook_set) */
static void tex_res(int k)
{
    tc_res(k ? map_tex_slot(k) : -1, inst[k].streams);
}

/* mapstream.c's mip 0 / 1 hooks (mapstream_tex_hooks): tag = the texture's index in instance k's table, | MS_RES_MIP1
 * for its mip 1 (tools/mapsplit.py MIP1_OUT) */
static unsigned tex_hook_age(int k, unsigned tag)
{
    int slot = k ? map_tex_slot(k) : -1;
    return k && slot < 0 ? ~0u : tc_res_age(slot, (int)(tag & ~MS_RES_MIP1), !!(tag & MS_RES_MIP1));
}

static void tex_hook_set(int k, unsigned tag, const void *p)
{
    int slot = k ? map_tex_slot(k) : -1;
    if (!k || slot >= 0)
        tc_res_one(slot, (int)(tag & ~MS_RES_MIP1), !!(tag & MS_RES_MIP1), p);
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

/* map_ms breakdown (budget doc proposals 1 and 6, default on; make DEFS=-DMAP_PASS_MS=0 = off): COP0 Count ticks
 * per part of map_draw this frame. cull = frustum / distance tests, pvs_eye, the index; tex = texcache (tc_get,
 * tc_mip, tc_begin / tc_end: uploads go into the chain there); chain = VIF / GIF packets (vu1_add_*, registers);
 * bump = relief; plan = the rest (per-cell level of detail, near parts, draw groups, visibility rows, effects sort).
 * Timed in one frame of MAP_PASS_EVERY only: each reading of the Count register costs ~0.15 us in PCSX2 and the
 * ~5000 a timed frame takes add ~0.8 ms to it. Logged every MAP_PASS_LOG frames as "map: pass ms" (average of the
 * timed frames, and the timed frame with the largest total). */
#ifndef MAP_PASS_MS
#define MAP_PASS_MS 1
#endif
#define MAP_PASS_EVERY 8
#define MAP_PASS_LOG 120
static int mp_on;  /* this frame is timed */
enum { MP_CULL, MP_TEX, MP_CHAIN, MP_BUMP, MP_N };
/* plan split (the same log, a "map: plan ms" line): per visible cell the resident-miss scan, the near level, the
 * chunk loop's own work (draw groups, visibility rows, streams), the rest of the cell loop, the effects; exclusive of
 * the parts above */
enum { MF_MISS, MF_NEAR, MF_CHUNKS, MF_FX, MF_N };
static unsigned mf_t[MF_N];
static unsigned mc_cells, mc_vis, mc_tiles, mc_tvis, mc_chunks;  /* per frame: cells tested / visible, tiles tested / visible, chunks reached */


static unsigned mp_t[MP_N];
static inline unsigned mp_sum(void) { return mp_t[0] + mp_t[1] + mp_t[2] + mp_t[3]; }
static inline unsigned mp_now(void) { unsigned c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }
#if MAP_PASS_MS
#define MP_BEGIN unsigned _mp0 = mp_on ? mp_now() : 0;
#define MP_END(p) if (mp_on) mp_t[p] += mp_now() - _mp0;
#define MP_SPLIT(p) do { if (mp_on) { unsigned _mp1 = mp_now(); mp_t[p] += _mp1 - _mp0; _mp0 = _mp1; } } while (0)
#define MF_BEGIN unsigned _mf0 = mp_on ? mp_now() : 0, _mfn = mp_on ? mp_sum() : 0;
#define MF_END(p) if (mp_on) mf_t[p] += mp_now() - _mf0 - (mp_sum() - _mfn);
#define MF_AGAIN if (mp_on) _mf0 = mp_now(), _mfn = mp_sum();
#else
#define MF_BEGIN
#define MF_END(p)
#define MF_AGAIN
#define MP_BEGIN
#define MP_END(p)
#define MP_SPLIT(p) ((void)0)
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

/* MS_DG (default on; make DEFS=-DMS_DG=0 = pvs only): an item none of whose chunks the draw groups of the recent floors
 * show (drawgrp.c dg_stream_shown) counts as unseen too: Firelink's cells from inside the Burg. A near part goes by
 * its cell's coarse chunks (the same pieces; its own chunks are known only once it is in). */
#ifndef MS_DG
#define MS_DG 1
#endif
static int ms_dg_vis(int k, unsigned id)
{
    const MapInst *m = &inst[k];
    if (!dg_area(k) || !m->order)
        return 1;
    unsigned a, n;
    if (id < m->nctab) {
        unsigned c = id;
        while (c > 0 && (m->ctab[c].near & CELL_PART))  /* a later part of a cell: the cell's own entry */
            c--;
        a = m->ctab[c].first, n = m->ctab[c].count;
        if (!n)
            return 1;
        for (unsigned j = a; j < a + n; j++)
            if (dg_stream_shown(k, m->order[j]))
                return 1;
        return 0;
    }
    id -= m->nctab;
    if (!m->rrange || id >= m->nritem || !(n = m->rrange[id][1]))
        return 1;
    a = m->rrange[id][0];
    for (unsigned c = a; c < a + n; c++)
        if (dg_stream_shown(k, c))
            return 1;
    return 0;
}
static int ms_vis(int k, unsigned id)
{
    if (!ms_pvs_vis(k, id))
        return 0;
    return !MS_DG || k < 0 || k >= MAP_INST || !inst[k].ctab || ms_dg_vis(k, id);
}
static unsigned ms_vis_gen(int k) { return pvs_gen(k) + (MS_DG ? dg_stream_gen() << 16 : 0); }

int map_stream_open(const char *pak_path)
{
    pano_open(pak_path);
    mapstream_vis_hooks(ms_vis, ms_vis_gen);
    return inst[0].ctab ? mapstream_init(pak_path, inst[0].ctab, inst[0].nctab) : 0;
}

void map_set_player(V3 feet)
{
    const float p[3] = {feet.x, feet.y, feet.z};
    static unsigned dg_frames;
    dg_tick(++dg_frames);  /* the recent floors' draw groups for streaming (drawgrp.c) */
    mapstream_update(p);  /* every instance's near cells (src/mapstream.c) */
    /* DSR's fog follows the collision part under the player (its MSB FogID); in the air the last one stays */
    ColHit h;
    player_feet = feet;
    int k = -1;
    if (col_floor(feet.x, feet.z, feet.y + 0.5f, feet.y - 3, COL_SOLID, &h) && (k = map_inst_of(h.block)) >= 0)
        dg_floor(k, h.tri);  /* the shown draw groups follow the same collision part (drawgrp.c, TECH_DEBT area-5) */
    /* a block outside the area group of the floor's block (drawgrp.c DG_SHARED; every block without one) takes its
     * draw groups from its own collision under the player: at a seam both blocks' collisions overlap (DSR's
     * ConnectCollision pairs, e.g. the Asylum's crow ledge h0054B1 and Firelink), each map shows what its own part
     * shows. Elsewhere such a block keeps the set of the last of its parts the player stood on (ASSUMED, TECH_DEBT
     * world-4). The blocks of the floor's group all follow the floor's part (one mask, as DSR) */
    for (int j = 0; j < MAP_INST; j++) {
        ColHit hj;
        if (j != k && inst_col_block(j) >= 0 && (k < 0 || !dg_area(k) || dg_area(j) != dg_area(k)) &&
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
    MP_BEGIN
    int r = visible_(c, e, sky);
    MP_END(MP_CULL)
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

/* the last chunk visible_ measured and its box's farthest view depth (draw_chunk's edge fog test, same eye) */
static const Chunk *vis_c;
static float vis_zmax;
static int emit_close;  /* MAP_TWO_PASS emit: the cell's chunks need no distance / edge fog test */

/* the frustum test of render.c r_box_classify (the box's bounding sphere against the view planes with a 40 px margin,
 * then the near plane and guard band) fused with the distance tests: one view transform of the box centre. Its plane
 * constants for this frame's camera (map_draw: vis_frame) */
static float vc_tx, vc_ty, vc_kx, vc_ky, vc_tg, vc_kg;

static void vis_frame(float fx, float fy)
{
    vc_tx = (R_SCREEN_W / 2 + 40) / fx, vc_ty = (R_SCREEN_H / 2 + 40) / fy;
    vc_kx = 1.0f / sqrtf(1 + vc_tx * vc_tx), vc_ky = 1.0f / sqrtf(1 + vc_ty * vc_ty);
    vc_tg = R_GUARD / fx, vc_kg = 1.0f / sqrtf(1 + vc_tg * vc_tg);
}

static int visible_(const Chunk *c, const float *e, int sky)
{
    float h0 = (c->mx[0] - c->mn[0]) * 0.5f, h1 = (c->mx[1] - c->mn[1]) * 0.5f, h2 = (c->mx[2] - c->mn[2]) * 0.5f;
    float d0 = c->mn[0] + h0 - e[0], d1 = c->mn[1] + h1 - e[1], d2 = c->mn[2] + h2 - e[2];
    float vz = cam[9] * d0 + cam[10] * d1 + cam[11] * d2, r2 = h0 * h0 + h1 * h1 + h2 * h2;
    if (!sky && !(c->flags & CHUNK_BACKDROP)) {  /* beyond FAR the edge fog is complete: blended mist / shafts end there (a fog-coloured card would show
                  * on the sky), opaque pieces at CULL_FAR (a fog-coloured silhouette, like DSR's distant background) */
        float ext = fabsf(cam[9]) * h0 + fabsf(cam[10]) * h1 + fabsf(cam[11]) * h2, z = vz - ext;
        vis_zmax = vz + ext, vis_c = c;
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
    float r = sqrtf(r2);
    if (vz < R_NEAR - r)
        return 0;
    float ax = fabsf(cam[3] * d0 + cam[4] * d1 + cam[5] * d2), ay = fabsf(cam[6] * d0 + cam[7] * d1 + cam[8] * d2);
    if ((ax - vc_tx * vz) * vc_kx > r || (ay - vc_ty * vz) * vc_ky > r)
        return 0;
    /* VU1-safe: the whole sphere in front of the near plane and inside the guard band */
    if (vz - r >= R_NEAR && (ax - vc_tg * vz) * vc_kg < -r && (ay - vc_tg * vz) * vc_kg < -r)
        return 1;
    return 2;
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
        MP_BEGIN
        t = tc_get(tx, tc_level(tx, sqrtf(d2)));
        MP_END(MP_TEX)
        if (!t) {
            DBGW(D_TEX, n, no);
            return 0;  /* texture pool full this frame */
        }
    }
    /* chunks crossing the near plane / guard band (cls 2) go to VU1's clipping program (vu1clip.vsm, F4); sky
     * triangles (1-3 km) never cross the near plane and whatever VU1 rejects of them lies outside the view. map.bin
     * has no untextured chunks (they would be skipped). */
    if (t) {
        MP_BEGIN
        int vclip = cls == 2 && !(c->flags & CHUNK_SKY) ? VU1_CLIP : 0, edge = 0;
        if (!(c->flags & CHUNK_SKY)) {  /* reaches into the edge fog: the edge loop of vu1map2 */
            float zmax;
            if (emit_close)  /* (MAP_TWO_PASS: the cell lies nearer than the edge fog's start) */
                zmax = 0;
            else if (c == vis_c)  /* map_draw's eye is the camera's */
                zmax = vis_zmax;
            else
                box_depth(c, cam, &zmax, NULL);
            edge = zmax > edge_z && !(c->flags & CHUNK_BACKDROP) ? VU1_EDGE : 0;  /* the backdrop: DSR's fog only */
        }
        float st_k = c->flags & CHUNK_ST1K ? 1024.0f : 4096.0f;
        if (!(mode & R_BLEND) && !(c->flags & (CHUNK_SKY | CHUNK_BLEND)) && c->tex >= 0) {  /* GS mip chain (render-12) */
            u64 mt;
            MP_SPLIT(MP_CHAIN);
            int k, mxl = tc_mip(m->tex_base + c->tex, t, st_k, &mt, &k);
            MP_SPLIT(MP_TEX);
            if (mxl)
                vu1_mip(mxl, k, mt);
        }
        int pkt = n == c->tris ? m->pkt : 0;  /* a foliage prefix sends its streams one by one */
        int vm = (mode & R_FOG ? VU1_FOG : 0) | (mode & R_BLEND ? VU1_BLEND : 0) | vclip | edge | pkt;
        if (c->nv)  /* indexed (whole chunks only: foliage, the only prefix drawer, stays in lists) */
            vu1_add_idx(t, c->flags & CHUNK_CULL, vm, n, c->nv, base + c->pos, base + c->st, base + c->col,
                        base + CHUNK_IDX(c), c->org, st_k);
        else
            vu1_add_ex(t, c->flags & CHUNK_CULL, vm, n, base + c->pos, base + c->st, base + c->col, c->org, st_k);
        map_vu1_batches++;
        DBGW(D_DRAWN, 0, no);
        MP_SPLIT(MP_CHAIN);
        if (!(mode & R_BLEND) && !(c->flags & (CHUNK_SKY | CHUNK_BLEND)))  /* normal-map relief pass (bump.c: the boot
                                                                            * block, and a world block while it is full) */
            bump_add((int)(m - inst), no, sqrtf(d2), c->flags & CHUNK_CULL, vclip | pkt, n, c->nv, base + c->pos,
                     base + c->st, base + c->col, c->nv ? base + CHUNK_IDX(c) : NULL, c->org, st_k);
        MP_END(MP_BUMP)
    }
    return n;
}

/* a horizon chunk (untextured, double-sided, its packet without st: vu1.c VU1_FLAT). The clipping program (VU1_CLIP)
 * gets a zero st stream sent on its own: with the packet's two streams alone it hung PCSX2 2 s into the Burg on the
 * first ISO run (2026-11-06; a full st stream on disc, +10 bytes a triangle, ran the route) */
static int draw_hz(const MapInst *m, const Chunk *c, int cls, int mode, int cull)
{
    static unsigned char zero_st[CHUNK_HZN_TRIS * 12] __attribute__((aligned(16)));
    float zmax;
    box_depth(c, cam, &zmax, NULL);
    int edge = zmax > edge_z && !(c->flags & CHUNK_BACKDROP) ? VU1_EDGE : 0;
    if (c->nv) {  /* indexed (horizon.py HZN_IDX, the default): vu1mapi, or vu1clip's indexed entry at the near plane;
                   * the st stream is the colours (read, unused with TME off) */
        if (c->tris > VU1_IDX_TRIS)
            return 0;
        vu1_add_idx(NULL, cull, VU1_FOG | VU1_FLAT | mode | (cls == 2 ? VU1_CLIP : edge), c->tris, c->nv, m->hz_base + c->pos,
                    m->hz_base + c->st, m->hz_base + c->col, m->hz_base + CHUNK_IDX(c), c->org, 4096.0f);
        map_vu1_batches++;
        return c->tris;
    }
    if (c->tris > CHUNK_HZN_TRIS)
        return 0;
    if (cls == 2)
        vu1_add_ex(NULL, cull, VU1_FOG | VU1_FLAT | mode | VU1_CLIP, c->tris, m->hz_base + c->pos, zero_st, m->hz_base + c->col,
                   c->org, 4096.0f);
    else
        vu1_add_ex(NULL, cull, VU1_FOG | VU1_FLAT | mode | edge | VU1_PKT, c->tris, m->hz_base + c->pos, m->hz_base + c->st,
                   m->hz_base + c->col, c->org, 4096.0f);
    map_vu1_batches++;
    return c->tris;
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

/* the box map_draw tests cell ci with: the cell's, grown by its horizon's (flags CHUNK_BACKDROP when either has it) */
static void cell_cull_box(const MapInst *m, int ci, Chunk *cb)
{
    const Cell *ce = &m->cells[ci];
    const MapCell *ctab = m->ctab;
    memset(cb, 0, sizeof *cb);
    cb->flags = ce->flags & CHUNK_BACKDROP;
    memcpy(cb->mn, ce->mn, sizeof cb->mn), memcpy(cb->mx, ce->mx, sizeof cb->mx);
    const HzCell *hc = m->hz_cell && ctab && !(ctab[ci].near & CELL_PART) && (m->hz_cell[ci].flags & 1)
                           ? &m->hz_cell[ci] : NULL;
    if (hc && hc->count) {
        cb->flags |= hc->flags & 2 ? CHUNK_BACKDROP : 0;
        for (int a = 0; a < 3; a++)
            cb->mn[a] = hc->mn[a] < cb->mn[a] ? hc->mn[a] : cb->mn[a],
            cb->mx[a] = hc->mx[a] > cb->mx[a] ? hc->mx[a] : cb->mx[a];
    }
}

static int tile_key_cmp(const void *a, const void *b)
{
    const unsigned *x = a, *y = b;
    return x[0] != y[0] ? (x[0] < y[0] ? -1 : 1) : (x[1] < y[1] ? -1 : x[1] > y[1]);
}

/* MAP_TILES: the lead cells grouped by tile, in cell order inside a tile */
static void tile_index(MapInst *mi)
{
    free(mi->tiles), free(mi->tcell);
    mi->tiles = NULL, mi->tcell = NULL, mi->ntiles = 0;
    if (!MAP_TILES || mi->ncells <= 0)
        return;
    unsigned (*key)[2] = malloc(mi->ncells * sizeof *key);
    mi->tcell = malloc(mi->ncells * sizeof *mi->tcell);
    mi->tiles = malloc(mi->ncells * sizeof *mi->tiles);
    if (!key || !mi->tcell || !mi->tiles) {
        free(key), free(mi->tiles), free(mi->tcell);
        mi->tiles = NULL, mi->tcell = NULL;
        return;
    }
    int n = 0;
    for (int ci = 0; ci < mi->ncells; ci++) {
        if (mi->ctab && (mi->ctab[ci].near & CELL_PART))
            continue;  /* drawn with the cell before it */
        const Cell *ce = &mi->cells[ci];
        int tx = (int)floorf((ce->mn[0] + ce->mx[0]) * 0.5f / MAP_TILE_M) + 2048;
        int tz = (int)floorf((ce->mn[2] + ce->mx[2]) * 0.5f / MAP_TILE_M) + 2048;
        key[n][0] = (unsigned)(tx & 4095) << 12 | (unsigned)(tz & 4095), key[n++][1] = (unsigned)ci;
    }
    qsort(key, n, sizeof *key, tile_key_cmp);
    int nt = 0;
    for (int j = 0; j < n; j++) {
        Chunk cb;
        cell_cull_box(mi, (int)key[j][1], &cb);
        if (!j || key[j][0] != key[j - 1][0]) {
            MapTile *t = &mi->tiles[nt++];
            memcpy(t->mn, cb.mn, sizeof t->mn), memcpy(t->mx, cb.mx, sizeof t->mx);
            t->first = (unsigned short)j, t->count = 0, t->flags = 0;
        }
        MapTile *t = &mi->tiles[nt - 1];
        t->count++;
        t->flags |= cb.flags & CHUNK_BACKDROP;
        for (int a = 0; a < 3; a++)
            t->mn[a] = cb.mn[a] < t->mn[a] ? cb.mn[a] : t->mn[a], t->mx[a] = cb.mx[a] > t->mx[a] ? cb.mx[a] : t->mx[a];
        mi->tcell[j] = (unsigned short)key[j][1];
    }
    free(key);
    mi->ntiles = nt;
}

/* MAP_NEAR_SKIP (budget doc proposal 6, default on; make DEFS=-DMAP_NEAR_SKIP=0 = scan every visible cell) */
#ifndef MAP_NEAR_SKIP
#define MAP_NEAR_SKIP 1
#endif
/* a visible cell's resident chunks not in yet (*miss 1; 2: one the horizon does not stand for, with a horizon; *mcov:
 * one it does), skipping those the horizon stands for beyond HZN_R (hzf) */
static void miss_scan(const MapInst *m, int k, int ci, int hzf, int has_hz, int *miss, int *mcov)
{
    const Cell *ce = &m->cells[ci];
    const Chunk *chunks = m->chunks;
    const unsigned short *order = m->order;
    for (unsigned j = ce->first; m->res_cells && j < (unsigned)ce->first + ce->count; j++) {
        unsigned fl = chunks[order[j]].flags;
        if (hzf && hz_covers(fl))
            continue;
        /* (mapstream_res also ranks it up: the cell is in view); foliage is resident only near the player
         * (mapstream.c MS_GRASS_WANT) and thinned to nothing farther: its absence is no hole */
        /* a chunk the camera cannot see is not read (pvs.c), nor one the draw groups hide (drawgrp.c, mapstream.c
         * MS_DG): neither is a hole. Counted as missing, a cell whose coarse chunks are hidden stand-ins drew its
         * horizon inside HZN_R for good (Firelink's cell 47 seen from the Burg: report3's 99.5 %, 2026-11-08) */
        if (!res_base(m, order[j]) && !(fl & CHUNK_GRASS) &&
            pvs_chunk(k, (unsigned)ci, order[j]) && dg_shown(k, order[j])) {
            *miss = 1;
            if (hz_covers(fl))
                *mcov = 1;
            else if (has_hz)
                *miss = 2;  /* not covered (with a horizon only blended effects are not): drawn late */
        }
    }
}

/* MAP_TWO_PASS (Burg plan B5, default on; make DEFS=-DMAP_TWO_PASS=0 = the per-chunk loop as before): map_draw splits
 * each visible cell into a pre-pass and an emit pass. The pre-pass decides the cell's level (near / coarse / horizon),
 * asks for its resident items once each instead of once per chunk, and takes the cell's draw list (the chunks that pass
 * the level, stream, draw-group and visibility-row filters, as offsets from the cell's first chunk) from the last draw
 * that built it, while nothing it depends on changed: the level, the near parts' and items' addresses, dg_gen and
 * pvs_gen (a 2 x 32-bit key). The emit pass runs only the per-frame tests over the list: foliage thinning, frustum /
 * distance, texture, batch. Chunks keep their order, so the chain is the same as the per-chunk loop's. The lists live in
 * one pool of LPOOL_N entries (24 KB); a full pool starts over (every cell builds its list again once). */
#ifndef MAP_TWO_PASS
#define MAP_TWO_PASS 1
#endif
#if defined(MAP_DBG) || defined(MAP_PANO_R)  /* their per-chunk reasons / panorama cut need the per-chunk loop */
#undef MAP_TWO_PASS
#define MAP_TWO_PASS 0
#endif
#define RI_COV 1    /* MapInst.rflag: every chunk of the item hz_covers (its tag has MS_RES_LOD) */
#define RI_GRASS 2  /* ... the item holds foliage (MS_RES_GRASS) */
#define CELL_ITEMS 32  /* resident items of one cell the pre-pass holds (more: that cell takes the per-chunk loop) */
#ifndef LPOOL_N
#define LPOOL_N 12288
#endif
#if MAP_TWO_PASS
static unsigned short lpool[LPOOL_N];
static unsigned lpool_top, lpool_gen = 1, lpool_builds, lpool_resets;
/* miss_scan from the cell's items (ib: their bases, NULL = not in; ib[t] of item i0 + t): only the chunks of items not
 * in are looked at, in chunk order as miss_scan does */
static void miss_items(const MapInst *m, int k, int ci, int hzf, int has_hz, const unsigned char *const *ib, unsigned i0,
                       unsigned ni, int *miss, int *mcov)
{
    for (unsigned t = 0; t < ni; t++) {
        if (ib[t] || (m->rflag[i0 + t] & RI_GRASS))
            continue;
        const unsigned *rr = m->rrange[i0 + t];
        for (unsigned j = rr[0]; j < rr[0] + rr[1]; j++) {
            unsigned fl = m->chunks[j].flags;
            if (hzf && hz_covers(fl))
                continue;
            if (!(fl & CHUNK_GRASS) && pvs_chunk(k, (unsigned)ci, j) && dg_shown(k, j)) {  /* (as miss_scan) */
                *miss = 1;
                if (hz_covers(fl))
                    *mcov = 1;
                else if (has_hz)
                    *miss = 2;
            }
        }
    }
}

/* the cell's draw list (MAP_TWO_PASS pre-pass): offsets from ce->first of the chunks [jb, je) that pass the level,
 * stream, draw-group and visibility-row filters, in chunk order, as the per-chunk loop takes them; *cut = triangles
 * the visibility rows left out */
static unsigned list_build(const MapInst *m, int k, int ci, int nd, int use_hz, const unsigned char *const *ib,
                           unsigned i0, const unsigned char *const *nbp, unsigned np, unsigned jb, unsigned je,
                           unsigned short *ls, unsigned *cut)
{
    const Cell *ce = &m->cells[ci];
    const MapCell *ctab = m->ctab;
    unsigned ln = 0, ct = 0;
    const unsigned char *nbb = nd ? nbp[0] : NULL;
    unsigned gid = nd ? ctab[ci].pad : 0, part = 0, pe = nd ? ctab[ci].near : 0;
    for (unsigned j = jb; j < je; j++) {
        const Chunk *c;
        unsigned no;
        if (j < (unsigned)ce->first + ce->count) {
            no = m->order[j], c = &m->chunks[no];
            if (use_hz && hz_covers(c->flags))
                continue;  /* the horizon stands for it */
            if (!(m->res_cells ? ib[m->citem[no] - i0] : m->streams))
                continue;  /* its streams are not in */
            if (nd && (c->flags & CHUNK_COARSE))
                continue;
        } else {
            unsigned nj = j - ce->first - ce->count;
            while (nj >= pe && part + 1 < np)  /* into the next part's blob */
                part++, nbb = nbp[part], gid = ctab[ci + part].pad, pe += ctab[ci + part].near & ~CELL_PART;
            if (!nbb)
                continue;  /* an unseen part that is not in (pvs.c) */
            c = (const Chunk *)nbb + (nj - (pe - (ctab[ci + part].near & ~CELL_PART)));
            no = c->flags & CHUNK_GRASS ? gid + (unsigned)(c - (const Chunk *)nbb) : c->flags >> 8;
        }
        if (!dg_shown(k, no))  /* MSB draw groups (drawgrp.c) */
            continue;
        if (!pvs_chunk(k, (unsigned)ci, no)) {  /* behind something (pvs.c) */
            ct += c->tris;
            continue;
        }
        ls[ln++] = (unsigned short)(j - ce->first);
    }
    *cut = ct;
    return ln;
}
#ifdef MAP_TWO_PASS_CHECK
static unsigned chk_lists, chk_bad;
#endif
#endif

static void build_index(MapInst *mi)
{
    if (mi->indexed == mi->map_bin)
        return;
    const Chunk *chunks = mi->chunks;
    free(mi->order), free(mi->sky), free(mi->cells), free(mi->tiles), free(mi->tcell);
    mi->tiles = NULL, mi->tcell = NULL, mi->ntiles = 0;
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
    Cell *cells = mi->cells = calloc((mi->nctab > (unsigned)m ? mi->nctab : m) + 1, sizeof *cells);
    int ncells = 0;
    if (mi->ctab) {  /* DSM5: the exporter's cells, resident chunks already grouped per cell (sky chunks after them) */
        const MapCell *ctab = mi->ctab;
        for (unsigned i = 0; i < mi->nctab; i++) {
            Cell *ce = &cells[ncells++];
            memcpy(ce->mn, ctab[i].mn, sizeof ce->mn), memcpy(ce->mx, ctab[i].mx, sizeof ce->mx);
            ce->first = ctab[i].first, ce->count = ctab[i].count, ce->flags = 0, ce->near_on = 0, ce->hz_on = 0;
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
    tile_index(mi);
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

static int split_k = -1;
static const unsigned char *split_b, *split_c;
static unsigned split_boff, split_coff;
void map_block_split(int k, const void *b, unsigned b_off, const void *c, unsigned c_off)
{
    split_k = k, split_b = b, split_boff = b_off, split_c = c, split_coff = c_off;
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
        m->tiles = NULL, m->tcell = NULL, m->ntiles = 0;
    }
    /* tab = the 80-byte header + map.bin from the chunk table on (tools/mapsplit.py): offsets >= chunk_off resolve
     * against a base chunk_off - 80 bytes before it */
    const unsigned char *map_bin = m->map_bin = (const unsigned char *)tab + sizeof(Header) - h->chunk_off;
    /* pieces (world.c WORLD_TAB_SPLIT): piece base - its offset in maptab.bin, in map.bin coordinates as map_bin */
    const unsigned char *tex_bin = m->tex_bin =
        split_k == k && split_b ? split_b - split_boff + sizeof(Header) - h->chunk_off : map_bin;
    const unsigned char *cel_bin = m->cel_bin =
        split_k == k && split_c ? split_c - split_coff + sizeof(Header) - h->chunk_off : map_bin;
    split_k = -1, split_b = split_c = NULL;
    const Header *hdr = m->hdr = h;
    m->ctab = (const MapCell *)(cel_bin + hdr->cell_off + 16);
    m->nctab = *(const unsigned *)(cel_bin + hdr->cell_off);
    m->ckeys = cell_keys(m->ctab, m->nctab);
    m->pkt = ((const unsigned *)(cel_bin + hdr->cell_off))[1] & 1 ? VU1_PKT : 0;
    m->chunks = (const Chunk *)(map_bin + hdr->chunk_off);
    m->fog_rows = *(const unsigned *)(tex_bin + hdr->fog_off);
    m->fog_ntris = ((const unsigned *)(tex_bin + hdr->fog_off))[1];
    m->fog_row = (const FogRow *)(tex_bin + hdr->fog_off + 16);
    m->fog_tri = (const unsigned char *)(m->fog_row + m->fog_rows);
    m->navv = (const float *)(tex_bin + hdr->navv_off);
    m->navt = (const unsigned short *)(tex_bin + hdr->navt_off);
    m->streams = res, m->res_cells = 0, m->citem = NULL;
    hz_parse(m);
    m->ntex = hdr->textures < MAX_TEX ? hdr->textures : MAX_TEX;
    m->on = 1, m->col_block = col_block, m->hz_only = 0;
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
        m->tex_base = used >> tslot[k] & 1 ? -1 : tc_block_add(tslot[k], tex_bin, tex_bin + hdr->tex_off, m->ntex);
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
    if (k < 0 || k >= MAP_INST || (k && !inst[k].on) || !inst[k].hdr)  /* (0: the boot block, world.c) */
        return;
    int on = bump_block(k, blob, inst[k].hdr->chunks);
    if (blob)
        printf("map: block %d relief %s\n", k, on ? "on" : "off");
}

static void block_drop(int k, int keep_dg)
{
    if (k <= 0 || k >= MAP_INST || !inst[k].on)
        return;
    MapInst *m = &inst[k];
    bump_block(k, NULL, 0);
    mapstream_close(k);  /* before the slot's textures go: it hands their mip 0 back (tex_hook_set) */
    tc_block_drop(tslot[k]);
    if (!keep_dg)
        dg_block_drop(k);
    pvs_block_drop(k);
    free(m->order), free(m->sky), free(m->cells), free(m->tiles), free(m->tcell);
    m->indexed = NULL, m->order = m->sky = NULL, m->cells = NULL, m->ncells = m->nsky = 0;
    m->tiles = NULL, m->tcell = NULL, m->ntiles = 0;
    if (pfog_row == m->fog_row)  /* the player's rows were this block's: back to the boot block's */
        pfog_row = inst[0].fog_row ? inst[0].fog_row : NULL, pfog_rows = inst[0].fog_row ? inst[0].fog_rows : 0,
        fog_area = 0;
    free(m->citem), free(m->rrange), free(m->rflag);
    m->rrange = NULL, m->rflag = NULL, m->nritem = 0;
    m->hdr = NULL, m->ctab = NULL, m->ckeys = NULL, m->nctab = 0, m->streams = NULL, m->ntex = 0, m->res_cells = 0, m->citem = NULL;
    m->hz_cell = NULL, m->hz_chunk = NULL, m->hz_base = NULL;
    m->on = 0;
    mapstream_select(0);
}

void map_block_drop(int k)
{
    block_drop(k, 0);
    if (k > 0 && k < MAP_INST)
        inst[k].hz_only = 0, inst[k].hz_cell = NULL, inst[k].hz_chunk = NULL, inst[k].hz_base = NULL;
}
void map_block_lean(int k, int keep_hz)
{
    if (k <= 0 || k >= MAP_INST || !inst[k].on)
        return;
    MapInst *m = &inst[k];
    const HzCell *hc = m->hz_cell;
    const Chunk *hk = m->hz_chunk;
    const unsigned char *hb = m->hz_base;
    unsigned n = m->nctab;
    block_drop(k, 1);
    if (keep_hz && hc)  /* (they lie in maptab's last piece, which world.c keeps) */
        m->hz_cell = hc, m->hz_chunk = hk, m->hz_base = hb, m->nctab = n, m->hz_only = 1;
}

/* a kept-only block (hz_only): every horizon chunk in view, as the shown draw groups pick them */
static unsigned draw_hz_only(int k, const float *e)
{
    const MapInst *m = &inst[k];
    unsigned n = 0;
    for (unsigned ci = 0; ci < m->nctab; ci++) {
        const HzCell *hc = &m->hz_cell[ci];
        for (unsigned j = 0; j < hc->count; j++) {
            const Chunk *c = &m->hz_chunk[hc->first + j];
            if (!dg_mask_shown(k, (int)(c->flags >> 16 & 0xFF)))
                continue;
            int cls = visible(c, e, 0);
            if (cls)
                n += draw_hz(m, c, cls, 0, 0);
        }
    }
    return n;
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
        unsigned e = c->nv ? (CHUNK_IDX(c) + c->tris * 4u + 15) & ~15u  /* the triangle stream is the last */
                           : (c->col + c->tris * 12 + 15) & ~15u;  /* colour u8x4 per corner, the last stream */
        l = c->pos - 16 < l ? c->pos - 16 : l;  /* from pos's code qword (VU1_PKT sends from there) */
        h = e > h ? e : h;
    }
    *lo = l == ~0u ? 0 : l;
    return h;
}

/* World blocks: the selected instance k's resident level as mapstream items (mapstream_res_open) instead of one
 * mapres.bin in a full pool: per cell the streams of its resident chunks (grouped by cell in the chunk table) in
 * items of whole chunks up to RES_ITEM bytes, the sky chunks' the same way, and each texture's mip 0 (and mip 1,
 * MIP1_OUT) that tools/mapsplit.py moved into mapres.bin. A block without it draws its near cells only. RES_ITEM: one cell up to
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
    unsigned n = hdr->chunks + 1 + 2 * (unsigned)mi->ntex;  /* (mip 0 and 1 per texture) */
    MapResItem *it = malloc(n * sizeof *it);
    unsigned short *citem = mi->citem = malloc((hdr->chunks + 1) * sizeof *citem);
    unsigned (*rr)[3] = mi->rrange = malloc(n * sizeof *rr);
    unsigned char *rf = mi->rflag = calloc(n, 1);
    if (!it || !citem || !rr || !rf || n > 65535) {
        free(it), free(citem), free(rr), free(rf);
        mi->citem = NULL, mi->rrange = NULL, mi->rflag = NULL;
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
            unsigned far = 0, cov = i < nctab && mi->hz_cell && (mi->hz_cell[i].flags & 1);
            for (unsigned c = j; c < g; c++)
                citem[c] = (unsigned short)m, far |= chunks[c].flags & CHUNK_BACKDROP, cov &= hz_covers(chunks[c].flags);
            rr[m][0] = j, rr[m][1] = i < nctab ? g - j : 0, rr[m][2] = i;
            rf[m] = (cov ? RI_COV : 0) | (coarse & CHUNK_GRASS ? RI_GRASS : 0);
            it[m++] = (MapResItem){roff + lo, lo, hi - lo,
                                   i < nctab ? i | (far ? MS_RES_FAR : 0) |
                                                   ((coarse & CHUNK_COARSE) && ctab[i].near ? MS_RES_COARSE : 0) |
                                                   (coarse & CHUNK_GRASS ? MS_RES_GRASS : 0) | (cov ? MS_RES_LOD : 0)
                                             : MS_RES_SKY};
            bad |= hi > rsize;
            j = g;
        }
    }
    const unsigned char *te = mi->tex_bin + hdr->tex_off;
    for (int t = 0; t < mi->ntex; t++) {  /* texmap.py entry: u16 w, h; u8 psm; ...; u32 pix[0] at +16 */
        const unsigned char *e = te + 32 * t;
        unsigned w = *(const unsigned short *)e, h = *(const unsigned short *)(e + 2);
        for (int l = 0; l < 2; l++) {  /* mip 0, and mip 1 when it streams too (mapsplit.py MIP1_OUT) */
            unsigned p = *(const unsigned *)(e + 16 + 4 * l), wl = w >> l, hl = h >> l;
            if (!(p & TC_RES))
                continue;
            unsigned b = e[4] == GS_PSM_T4 ? wl * hl / 2 : wl * hl;
            rr[m][0] = rr[m][1] = rr[m][2] = 0;
            it[m++] = (MapResItem){roff + (p & ~TC_RES), p & ~TC_RES, b, MS_RES_TEX | (l ? MS_RES_MIP1 : 0) | (unsigned)t};
            bad |= (p & ~TC_RES) + b > rsize;
        }
    }
    if (bad)
        printf("map: block %d: mapres.bin (%u KB) shorter than its chunk / texture records\n", k, rsize >> 10);
    else if (mi->nritem = m, mapstream_res_open(k, it, nctab, m)) {
        mi->res_cells = 1;
        mapstream_tex_hooks(tex_hook_age, tex_hook_set);
    }
    if (!mi->res_cells)
        free(citem), free(rr), free(rf), mi->citem = NULL, mi->rrange = NULL, mi->rflag = NULL, mi->nritem = 0;
    free(it);
}

int map_block_stream_open(int k, const void *pack)
{
    if (k < 0 || k >= MAP_INST || !inst[k].on)
        return 0;
    const MapInst *m = &inst[k];
    mapstream_select(k);
    int r = m->ctab ? mapstream_open_pack(k, pack, m->ctab, m->nctab) : 0;
    mapstream_vis_hooks(ms_vis, ms_vis_gen);
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
    static unsigned mp_fr;
    mp_on = MAP_PASS_MS && !(mp_fr++ % MAP_PASS_EVERY);
    unsigned mp_t0 = mp_on ? mp_now() : 0;
    (void)mp_t0;
    memset(mp_t, 0, sizeof mp_t), memset(mf_t, 0, sizeof mf_t);
    if (!inst[pinst].on && pinst)
        pinst = 0;
    vis_c = NULL;
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
    {
        MP_BEGIN
        build_index(m);
        MP_END(MP_CULL)
    }
    bump_owner(pinst);  /* relief for the player's block (bump.c: one CLUT area) */
    map_vu1_batches = map_ee_tris = 0;
#ifdef MAP_DBG
    static int dbg_init;
    if (!dbg_init++)
        memset(dbg_why, 255, sizeof dbg_why);
#endif
    const float e[3] = {eye.x, eye.y, eye.z};
    {
        MP_BEGIN
        pvs_eye(e);  /* every instance's visibility row for this camera cell (src/pvs.c) */
        MP_END(MP_CULL)
    }
    {
        V3 ey, rt, up, fw;
        float fx, fy, zs;
        r_camera_basis(&ey, &rt, &up, &fw, &fx, &fy, &zs);
        const float b[12] = {ey.x, ey.y, ey.z, rt.x, rt.y, rt.z, up.x, up.y, up.z, fw.x, fw.y, fw.z};
        memcpy(cam, b, sizeof cam);
        cam_f = fx;
        vis_frame(fx, fy);
        MP_BEGIN
        tc_begin(fx);  /* this frame's texture uploads go into the chain from here on */
        MP_END(MP_TEX)
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
    static struct { unsigned char k, a; unsigned short ci; } hzfade[MAX_HZ_FADE];  /* cells fading in (HZN_FADE) */
    int nhzfade = 0;
    unsigned holes = 0;  /* visible cells with resident chunks not in and no near level drawn for them */
    static unsigned hzx[3][3];  /* cells the horizon stood in for inside HZN_R this frame (log) */
    unsigned hz_fill = 0, hz_tris = 0, hole_nohz = 0, hole_unc = 0, hole_ex[2] = {~0u, 0};  /* cells inside HZN_R drawn by their horizon (coarse not in); horizon tris */
    unsigned pend = 0, pops = 0, pop_wait = 0;  /* pop-in: cells drawn coarse while due near; swaps seen; longest wait */
    float pop_d = 1e9f;
    pop_fr++;
    unsigned pvs_cut = 0;  /* triangles the visibility rows left out (src/pvs.c) */
    unsigned k_drawn[MAP_INST] = {0}, k_cut[MAP_INST] = {0};  /* the same and the drawn ones per instance (A/B log) */
    for (int k = 0; k < MAP_INST; k++) {  /* every loaded block (src/world.c); one instance in a single-area build */
        if (k && !inst[k].on) {
            if (inst[k].hz_only) {  /* a kept-only block: its horizon alone */
                unsigned dn = draw_hz_only(k, e);
                drawn += dn, k_drawn[k] += dn, hz_tris += dn;
            }
            continue;
        }
        m = &inst[k];
        mapstream_select(k);  /* mapstream_get / mapstream_res below answer for k */
        if (!m->hdr)
            continue;
        {
            MP_BEGIN
            build_index(m);
            MP_END(MP_CULL)
        }
        const Chunk *chunks = m->chunks;
        const MapCell *ctab = m->ctab;
        const unsigned short *order = m->order;
        int ntl = m->tiles ? m->ntiles : 1;
        for (int ti = 0; ti < ntl; ti++) {
        const MapTile *tl = m->tiles ? &m->tiles[ti] : NULL;
        if (tl) {
            Chunk tb = {.flags = tl->flags, .mn = {tl->mn[0], tl->mn[1], tl->mn[2]}, .mx = {tl->mx[0], tl->mx[1], tl->mx[2]}};
            mc_tiles++;
            if (!visible(&tb, e, 0))
                continue;
            mc_tvis++;
        }
        for (int ii = 0, nci = tl ? tl->count : m->ncells; ii < nci; ii++) {
            const int ci = tl ? m->tcell[tl->first + ii] : ii;
            Cell *ce = &m->cells[ci];
            /* the cell's box, tested as opaque (out to CULL_FAR); its blended chunks end at FAR on their own test. A
             * cell holding backdrop chunks is only frustum tested, its other chunks get their own distance test (the
             * cell cut dropped whole far-only cells beyond 250 m: the sky holes and floating wall tops of map-39) */
            Chunk cb = {.flags = ce->flags & CHUNK_BACKDROP, .mn = {ce->mn[0], ce->mn[1], ce->mn[2]},
                        .mx = {ce->mx[0], ce->mx[1], ce->mx[2]}};  /* designated: Chunk's scalar fields change */
            /* the cell's horizon (HZN_R): its triangles went to the cell by centroid and may reach past its box */
            const HzCell *hc = m->hz_cell && ctab && !(ctab[ci].near & CELL_PART) && (m->hz_cell[ci].flags & 1)
                                   ? &m->hz_cell[ci] : NULL;
            if (hc && hc->count) {
                cb.flags |= hc->flags & 2 ? CHUNK_BACKDROP : 0;
                for (int a = 0; a < 3; a++)
                    cb.mn[a] = hc->mn[a] < cb.mn[a] ? hc->mn[a] : cb.mn[a],
                    cb.mx[a] = hc->mx[a] > cb.mx[a] ? hc->mx[a] : cb.mx[a];
            }
            mc_cells++;
            if (!visible(&cb, e, 0)) {
#ifdef MAP_DBG
                for (unsigned j = ce->first; j < ce->first + ce->count; j++)
                    DBGW(vis_why - D_FAR + D_CELL_FAR, chunks[order[j]].tris, order[j]);
#endif
                continue;
            }
            mc_vis++;

            /* DSM5: the near level when its blob is loaded (then the coarse chunks are skipped), else the coarse one;
             * a world block's resident chunks once their item's streams are in (mapstream_res) */
            if (ctab && (ctab[ci].near & CELL_PART))
                continue;  /* a later part of the cell before it: drawn with it */
            const unsigned char *nb = NULL, *nbp[NEAR_PARTS] = {0};
            unsigned np = 1, nnear = 0;  /* the cell's near parts (export_map.py NEAR_PART), their chunks */
            int miss = 0;  /* a resident chunk of the cell is not in yet: its near cell draws as far as it is loaded */
            int mcov = 0;  /* ... of those the horizon stands for (hz_covers) */
            int nd = 0;    /* the near level is drawn (every part in, or unseen: pvs.c) */
            int hzf = 0;   /* beyond HZN_R: the horizon is drawn, the covered chunks are not even asked for */
            float hz_d = 0;  /* the cell box's distance (with a horizon) */
            if (hc) {
                float d2 = 0;
                for (int a = 0; a < 3; a++) {
                    float o = e[a] < ce->mn[a] ? ce->mn[a] - e[a] : e[a] > ce->mx[a] ? e[a] - ce->mx[a] : 0;
                    d2 += o * o;
                }
                float r = ce->hz_on ? HZN_R - HZN_HYST : HZN_R + HZN_HYST;
                hzf = ce->hz_on = d2 > r * r;
                hz_d = sqrtf(d2);
            }
#if MAP_TWO_PASS
            /* the cell's resident items, asked for once each (mapstream_res marks them used, as the chunk loop's
             * per-chunk calls did): those the horizon stands for wholly (RI_COV) not beyond HZN_R */
            const unsigned char *ib[CELL_ITEMS];
            unsigned i0 = 0, ni = 0;
            int two = !m->res_cells || !ce->count;  /* items fit ib: the cached list (else the per-chunk loop) */
            if (!two) {
                i0 = m->citem[order[ce->first]], ni = m->citem[order[ce->first + ce->count - 1]] + 1u - i0;
                two = ni <= CELL_ITEMS;
                for (unsigned t = 0; two && t < ni; t++)
                    ib[t] = hzf && (m->rflag[i0 + t] & RI_COV) ? NULL : mapstream_res(i0 + t);
            }
#define MISS_SCAN() (two ? miss_items(m, k, ci, hzf, hc != NULL, ib, i0, ni, &miss, &mcov) \
                         : miss_scan(m, k, ci, hzf, hc != NULL, &miss, &mcov))
#else
#define MISS_SCAN() miss_scan(m, k, ci, hzf, hc != NULL, &miss, &mcov)
#endif
            /* the near switch first (MAP_NEAR_SKIP): a cell inside the near radius needs the resident-miss scan only
             * when its near level is not drawn after all (the scan decides holes and the horizon stand-in, nothing
             * else once the near level draws; its mapstream_res calls are made again by the chunk loop) */
            MF_BEGIN
            int near_on = 0;
            float d2 = 0;  /* (ctab) the near level's distance */
            if (ctab) {
                for (int k = 0; k < 3; k++) {
                    float o = e[k] < ce->mn[k] ? ce->mn[k] - e[k] : e[k] > ce->mx[k] ? e[k] - ce->mx[k] : 0;
                    d2 += o * o;
                }
                /* without the cell's resident streams its near cells draw as far as they are loaded */
                float r = ce->near_on ? NEAR_DRAW_R + NEAR_HYST : NEAR_DRAW_R - NEAR_HYST;
                if (m->ckeys)  /* the near level's distance: the box clipped to the cell's cube (MS_NEAR_CUBE) */
                    d2 = cell_near_d2(&ctab[ci], m->ckeys + 4 * ci, e);
                near_on = r > 0 && d2 < r * r;
            }
            int scanned = !(MAP_NEAR_SKIP && near_on);
            if (scanned) {
                MF_AGAIN
                MISS_SCAN();
                MF_END(MF_MISS)
            }
            if (ctab) {
                MF_BEGIN
                ce->near_on = near_on;
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
                if (!scanned && !nd) {
                    MF_END(MF_NEAR)
                    MF_AGAIN
                    MISS_SCAN();
                    MF_END(MF_MISS)
                    MF_AGAIN
                }
                if (ce->near_on && (ctab[ci].near & ~CELL_PART)) {
                    int was = (unsigned short)(pop_fr - ce->seen) == 1;
                    if (!nd) {
                        pend++;
                        if (!ce->waiting)
                            ce->wait0 = pop_fr, ce->waiting = 1;
                    } else if (ce->waiting) {
                        unsigned w = (unsigned short)(pop_fr - ce->wait0);
                        if (was && w >= 2) {
                            pops++;
                            pop_wait = w > pop_wait ? w : pop_wait;
                            pop_d = d2 < pop_d * pop_d ? sqrtf(d2) : pop_d;
                        }
                        ce->waiting = 0;
                    }
                } else {
                    ce->waiting = 0;
                }
                ce->seen = pop_fr;
                for (unsigned p = 0; nd && p < np; p++)
                    nnear += ctab[ci + p].near & ~CELL_PART;
                MF_END(MF_NEAR)
            }
            /* the horizon in place of the coarse level: far, or the coarse level not complete (never mixed) */
            int use_hz = hc && !nd && (hzf || mcov);
            {
                /* with the horizon only blended effects (mist, light shafts) can be missing: late, but no hole */
                unsigned h = use_hz ? 0 : miss && !nd && (!hc || mcov);
                holes += h;
                hole_nohz += h && !hc, hole_unc += hc && miss == 2 && !nd;  /* no horizon for the cell; effects late */
                if (h && hole_ex[0] == ~0u)
                    hole_ex[0] = (unsigned)k, hole_ex[1] = (unsigned)ci;
            }
            if (use_hz && !hzf && hz_fill < 3)  /* (the log names up to three: instance.cell at distance) */
                hzx[hz_fill][0] = (unsigned)k, hzx[hz_fill][1] = (unsigned)ci, hzx[hz_fill][2] = (unsigned)hz_d;
            hz_fill += use_hz && !hzf;
            if (HZN_FADE > 0 && hc && hc->count && !use_hz && !nd && hz_d > HZN_R - HZN_FADE && nhzfade < MAX_HZ_FADE) {
                /* inside HZN_R, on the coarse level: its horizon over it at alpha (d - (HZN_R - HZN_FADE)) / HZN_FADE,
                 * reaching 1 where the hysteresis band starts (HZN_R - HZN_HYST: the swap then shows no change) */
                float t = (hz_d - (HZN_R - HZN_FADE)) / (HZN_FADE - HZN_HYST);
                int al = t >= 1 ? 128 : (int)(t * 128);
                if (al > 0)
                    hzfade[nhzfade].k = (unsigned char)k, hzfade[nhzfade].ci = (unsigned short)ci,
                    hzfade[nhzfade++].a = (unsigned char)al;
            }
            {  /* level-of-detail flips of a cell in view (hitch.c): near / coarse / horizon / hole */
                unsigned char lv = nd ? 4 : use_hz ? 2 : miss && (!hc || mcov) ? 1 : 3;
                if (ce->lv && lv != ce->lv && (unsigned short)(pop_fr - ce->lv_seen) == 1) {
                    hitch_flip(k, (unsigned)ci, lv == ce->lv_prev && (unsigned short)(pop_fr - ce->lv_fr) <= HITCH_FLOP_FRAMES);
                    ce->lv_prev = ce->lv, ce->lv_fr = pop_fr;
                }
                ce->lv = lv, ce->lv_seen = pop_fr;
            }
            MF_AGAIN
#if MAP_TWO_PASS
            if (two && hzf && nd)  /* (the near level beyond HZN_R: the chunk loop asked for every item) */
                for (unsigned t = 0; t < ni; t++)
                    if (m->rflag[i0 + t] & RI_COV)
                        ib[t] = mapstream_res(i0 + t);
            unsigned jb = m->streams || m->res_cells ? ce->first : ce->first + ce->count, je = ce->first + ce->count + nnear;
            two = two && je - jb <= LPOOL_N && je - ce->first <= 65535u;
            if (two) {
                /* pre-pass: the cell's list from the last draw if nothing it depends on changed, else built again */
                unsigned h1 = 2166136261u ^ (unsigned)k, h2 = 0x9E3779B9u + (unsigned)ci;
#define LMIX(w) (h1 = (h1 ^ (unsigned)(w)) * 16777619u, h2 = (h2 + (unsigned)(w)) * 0x85EBCA6Bu, h2 ^= h2 >> 13)
                LMIX((unsigned)nd | (unsigned)use_hz << 1 | np << 2);
                LMIX(dg_gen());
                LMIX(pvs_gen(k));
                LMIX((unsigned)m->chunks);
                LMIX((unsigned)m->streams);
                for (unsigned p = 0; nd && p < np; p++)
                    LMIX((unsigned)nbp[p]);
                for (unsigned t = 0; t < ni; t++)
                    LMIX((unsigned)ib[t]);
#undef LMIX
                if (ce->lgen != lpool_gen || ce->lkey[0] != h1 || ce->lkey[1] != h2) {
                    if (lpool_top + (je - jb) > LPOOL_N)  /* full: every list goes, the cells build theirs again */
                        lpool_gen++, lpool_top = 0, lpool_resets++;
                    unsigned cut, ln = list_build(m, k, ci, nd, use_hz, ib, i0, nbp, np, jb, je, lpool + lpool_top, &cut);
                    ce->lkey[0] = h1, ce->lkey[1] = h2, ce->lgen = lpool_gen, ce->lcut = cut;
                    ce->loff = (unsigned short)lpool_top, ce->ln = (unsigned short)ln;
                    lpool_top += ln;
                    lpool_builds++;
                }
#ifdef MAP_TWO_PASS_CHECK  /* debug: every kept list against a fresh build ("map: two-pass check" line) */
                else if (lpool_top + (je - jb) <= LPOOL_N) {
                    unsigned cut, ln = list_build(m, k, ci, nd, use_hz, ib, i0, nbp, np, jb, je, lpool + lpool_top, &cut);
                    chk_lists++;
                    if (ln != ce->ln || cut != ce->lcut || memcmp(lpool + lpool_top, lpool + ce->loff, ln * 2))
                        chk_bad++;
                }
#endif
                pvs_cut += ce->lcut, k_cut[k] += ce->lcut;
                MF_END(MF_NEAR)
                MF_AGAIN
                /* emit: the list's chunks through the frustum and texture tests into the chain. A cell whose farthest
                 * corner is nearer than SMALL_Z and the edge fog's start (its chunks lie in its box) has no chunk the
                 * distance tests could cut or the edge fog reach: the frustum test alone (emit_close) */
                float df2 = 0;
                for (int a = 0; a < 3; a++) {
                    float u = e[a] - ce->mn[a], v = ce->mx[a] - e[a], w = u > v ? u : v;
                    df2 += w * w;
                }
                emit_close = SMALL_Z <= FAR && SMALL_Z <= CULL_FAR && df2 < SMALL_Z * SMALL_Z && edge_z > 0 &&
                             df2 < edge_z * edge_z;
                const unsigned short *ls = lpool + ce->loff;
                const unsigned char *nbb = nd ? nbp[0] : NULL;
                unsigned gid = nd ? ctab[ci].pad : 0, part = 0, pe = nd ? ctab[ci].near : 0;
                for (unsigned li = 0, ln = ce->ln; li < ln; li++) {
                    unsigned j = ce->first + ls[li], no;
                    const Chunk *c;
                    const unsigned char *base;
                    if (j < (unsigned)ce->first + ce->count) {
                        no = order[j], c = &chunks[no];
                        base = m->res_cells ? ib[m->citem[no] - i0] : m->streams;
                    } else {
                        unsigned nj = j - ce->first - ce->count;
                        while (nj >= pe && part + 1 < np)
                            part++, nbb = nbp[part], gid = ctab[ci + part].pad, pe += ctab[ci + part].near & ~CELL_PART;
                        c = (const Chunk *)nbb + (nj - (pe - (ctab[ci + part].near & ~CELL_PART)));
                        no = c->flags & CHUNK_GRASS ? gid + (unsigned)(c - (const Chunk *)nbb) : c->flags >> 8;
                        base = nbb;
                    }
                    unsigned n = c->flags & CHUNK_GRASS ? grass_tris(c, e) : c->tris;
                    if (!n)
                        continue;
                    mc_chunks++;
                    int cls;
                    if (emit_close) {
                        MP_BEGIN
                        cls = visible_(c, e, 1);  /* (as sky: no distance tests) */
                        MP_END(MP_CULL)
                    } else
                        cls = visible(c, e, 0);
                    if (!cls)
                        continue;
                    if (c->flags & CHUNK_BLEND) {
                        if (nfx < MAX_FX) {
                            float d = 0;
                            for (int a = 0; a < 3; a++) {
                                float mm = (c->mn[a] + c->mx[a]) * 0.5f - e[a];
                                d += mm * mm;
                            }
                            fx[nfx].d = d, fx[nfx].c = c, fx[nfx].k = k, fx[nfx].cls = cls;
                            fx[nfx].base = base, fx[nfx++].no = no;
                        }
                        continue;
                    }
                    unsigned dn = draw_chunk(m, base, c, n, cls, fog, no);
                    drawn += dn, k_drawn[k] += dn;
                }
                emit_close = 0;
            } else
#endif
            {
                unsigned near_gid = nd ? ctab[ci].pad : 0;
                unsigned part = 0, part_end = nd ? ctab[ci].near : 0;  /* the near part of chunk j, its last index + 1 */
                for (unsigned j = m->streams || m->res_cells ? ce->first : ce->first + ce->count;
                     j < ce->first + ce->count + nnear; j++) {
                    int is_near = j >= (unsigned)ce->first + ce->count;
                    if (use_hz && !is_near && hz_covers(chunks[order[j]].flags))
                        continue;  /* the horizon stands for it */
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
                    mc_chunks++;
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
            MF_END(MF_CHUNKS)
            for (unsigned j = 0; use_hz && j < hc->count; j++) {  /* the horizon (no pvs rows: merged) */
                const Chunk *c = &m->hz_chunk[hc->first + j];
                /* one mesh per draw-group set (horizon.py, flags >> 16 = dg.bin piece mask; 0 always drawn): only
                 * the sets the floor's groups show, as the chunks it stands for */
                if (!dg_mask_shown(k, (int)(c->flags >> 16 & 0xFF)))
                    continue;
                int cls = visible(c, e, 0);
                if (cls) {
                    unsigned dn = draw_hz(m, c, cls, 0, 0);
                    drawn += dn, k_drawn[k] += dn, hz_tris += dn;
                }
            }
        }
        }
    }
    mapstream_select(0);
    if (nhzfade) {
        /* horizons fading in (HZN_FADE) over their cells' coarse level, Cs a + Cd (1 - a), Z tested HZN_ZBIAS nearer
         * and not written, front faces only: the horizon is drawn double-sided, so a wall's back face (or a layer just
         * behind the coarse surface, now inside the bias) would blend a second time. A depth-only pre-pass did the same
         * but left the horizon's biased depth for the characters and effects drawn after (feet in the ground at
         * ~50 m) */
        vu1_map_zbias(1.0f + HZN_ZBIAS);
        vu1_map_fog(a, b, fmin, 255);
        vu1_map_edge(edge_a2);
        u64 zb = GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0) | (1ull << 32);
        for (int i = 0; i < nhzfade; i++) {
            const MapInst *mk = &inst[hzfade[i].k];
            const HzCell *hc = &mk->hz_cell[hzfade[i].ci];
            if (HZN_FADE_DEBUG && !i) {
                u64 fc[2] = {0xFF00FFull, 0x3D};  /* FOGCOL r, g, b */
                vu1_gs_regs(1, fc);
                vu1_map_fog(a, b, 0, 0);
            }
            u64 r[4] = {GS_SETREG_ALPHA(0, 1, 2, 1, HZN_FADE_DEBUG ? 0x80 : hzfade[i].a), 0x42, zb, 0x4E};
            vu1_gs_regs(2, r);
            for (unsigned j = 0; j < hc->count; j++) {
                const Chunk *c = &mk->hz_chunk[hc->first + j];
                if (!dg_mask_shown(hzfade[i].k, (int)(c->flags >> 16 & 0xFF)))
                    continue;
                int cls = visible(c, e, 0);
                if (cls) {
                    unsigned dn = draw_hz(mk, c, cls, VU1_BLEND, 1);
                    drawn += dn, hz_tris += dn;
                }
            }
        }
        vu1_map_zbias(1.0f);
        vu1_map_fog(a, b, fmin, 255);
        if (HZN_FADE_DEBUG) {  /* the frame's fog colour back for what is drawn after */
            float f[5];
            u64 fc[2] = {0, 0x3D}, fb;
            r_get_fog(f, &fc[0], &fb);
            vu1_gs_regs(1, fc);
        }
        blend_regs(gs, 0);
    }
    {
        MP_BEGIN
        bump_draw();  /* relief over the opaque map (bump.c), before the blended effects */
        MP_END(MP_BUMP)
    }

    MF_BEGIN
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
    MF_END(MF_FX)
    {
        MP_BEGIN
        tc_end();
        MP_END(MP_TEX)
    }
#if MAP_PASS_MS
    {  /* "map: pass ms": the breakdown over 30 frames (average, and the frame with the largest map_draw) */
        static unsigned fr, nt, acc[MP_N + 1], wst[MP_N + 1], wtot, facc[MF_N];
        unsigned tot = mp_on ? mp_now() - mp_t0 : 0, v[MP_N + 1], sub = 0;
        for (int i = 0; i < MF_N; i++)
            facc[i] += mf_t[i];
        nt += mp_on;
        for (int i = 0; i < MP_N; i++)
            v[i] = mp_t[i], sub += mp_t[i];
        v[MP_N] = tot > sub ? tot - sub : 0;  /* plan: the rest */
        for (int i = 0; i <= MP_N; i++)
            acc[i] += v[i];
        if (tot > wtot)
            memcpy(wst, v, sizeof wst), wtot = tot;
        if (!(++fr % MAP_PASS_LOG) && nt) {
#define MPMS(x) ((double)((float)(x) / 294912.0f / (float)nt))
#define MPMS1(x) ((double)((float)(x) / 294912.0f))
            unsigned at = acc[0] + acc[1] + acc[2] + acc[3] + acc[4];
            printf("map: pass ms cull %.2f plan %.2f chain %.2f tex %.2f bump %.2f total %.2f | worst cull %.2f plan "
                   "%.2f chain %.2f tex %.2f bump %.2f total %.2f (%u timed frames)\n", MPMS(acc[MP_CULL]),
                   MPMS(acc[MP_N]), MPMS(acc[MP_CHAIN]), MPMS(acc[MP_TEX]), MPMS(acc[MP_BUMP]), MPMS(at),
                   MPMS1(wst[MP_CULL]), MPMS1(wst[MP_N]), MPMS1(wst[MP_CHAIN]), MPMS1(wst[MP_TEX]), MPMS1(wst[MP_BUMP]),
                   MPMS1(wtot), nt);
            printf("map: plan ms miss %.2f near %.2f chunks %.2f fx %.2f other %.2f; per frame tiles %u visible %u, cells %u "
                   "visible %u, chunks %u\n", MPMS(facc[MF_MISS]), MPMS(facc[MF_NEAR]),
                   MPMS(facc[MF_CHUNKS]), MPMS(facc[MF_FX]), MPMS(acc[MP_N] - facc[0] - facc[1] - facc[2] - facc[3]),
                   mc_tiles / MAP_PASS_LOG, mc_tvis / MAP_PASS_LOG, mc_cells / MAP_PASS_LOG, mc_vis / MAP_PASS_LOG,
                   mc_chunks / MAP_PASS_LOG);
            mc_cells = mc_vis = mc_tiles = mc_tvis = mc_chunks = 0, nt = 0;
#undef MPMS
#undef MPMS1
            memset(acc, 0, sizeof acc), memset(wst, 0, sizeof wst), wtot = 0, memset(facc, 0, sizeof facc);
        }
    }
#endif
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
        static unsigned fr, worst, cut_max, pend_max, pops_s, wait_max, hzf_max, hzt_max, w_nohz, unc_max, w_k, w_ci;
        static float near_min = 1e9f;
        if (holes > worst)
            worst = holes, w_nohz = hole_nohz, w_k = hole_ex[0], w_ci = hole_ex[1];
        unc_max = hole_unc > unc_max ? hole_unc : unc_max;
        hitch_map(pend, hz_fill, pops);  /* the hitch monitor's "stream:" line (src/hitch.c) */
        hzf_max = hz_fill > hzf_max ? hz_fill : hzf_max;
        hzt_max = hz_tris > hzt_max ? hz_tris : hzt_max;
        pend_max = pend > pend_max ? pend : pend_max;
        pops_s += pops, pop_total += pops;
        wait_max = pop_wait > wait_max ? pop_wait : wait_max;
        near_min = pop_d < near_min ? pop_d : near_min;
        cut_max = pvs_cut > cut_max ? pvs_cut : cut_max;
        if (!(++fr % 30)) {
#if MAP_TWO_PASS
            if (!(fr % 300))
                printf("map: two-pass lists built %u, pool resets %u, pool %u / %u entries\n", lpool_builds,
                       lpool_resets, lpool_top, LPOOL_N);
#ifdef MAP_TWO_PASS_CHECK
            printf("map: two-pass check %u lists, %u differ\n", chk_lists, chk_bad);
#endif
#endif
            if (worst)
                printf("map: cells missing %u (most in a frame over 1 s; %u without a horizon; e.g. cell %u.%u)\n", worst,
                       w_nohz, w_k, w_ci);
            if (unc_max)
                printf("map: effects not in yet in %u cells (most in a frame over 1 s)\n", unc_max);
            unc_max = 0;
            if (hzf_max)  /* not holes: the horizon stood in for a coarse level not in yet, inside HZN_R */
                printf("map: horizon filled %u cells (most in a frame over 1 s), %u horizon tris; e.g. %u.%u at %u m, %u.%u at %u m\n",
                       hzf_max, hzt_max, hzx[0][0], hzx[0][1], hzx[0][2], hzx[1][0], hzx[1][1], hzx[1][2]);
            hzf_max = hzt_max = 0;
            if (pops_s)
                printf("map: near pop-in %u cells over 1 s (wait max %u frames, nearest %.0f m; %u in the run), %u due "
                       "near drawn coarse (most in a frame)\n", pops_s, wait_max, (double)near_min, pop_total, pend_max);
            pend_max = pops_s = wait_max = 0, near_min = 1e9f;
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
