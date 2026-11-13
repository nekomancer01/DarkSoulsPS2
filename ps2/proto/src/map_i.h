/* map.c and mapblock.c: shared types, state and helpers (not for other modules). */
#ifndef MAP_I_H
#define MAP_I_H
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
#include "ticks.h"

typedef struct {
    char magic[4];
    unsigned chunks, textures, nav_verts, nav_tris;
    float spawn[3], spawn_yaw;
    unsigned chunk_off, tex_off, navv_off, navt_off;
    float fog_begin, fog_end, fog_max;  /* FogBank row: view z metres, maximum density (may exceed 1) */
    unsigned char fog_rgb[4], sky_fog[4]; /* sky_fog: r, g, b, constant density x 255 */
    unsigned fog_off, cell_off;          /* fog section: area fog rows + row per collision triangle; DSM5: cell table */
} Header;
/* fog section: u32 rows, collision tris, sky rows, 0; FogRow rows[]; u8 row per collision.bin triangle; padding to 4;
 * SkyRow sky rows[]. One row per draw-param
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
/* FogBank row of a sky / far background piece (a sky chunk's flags name it, CHUNK_SKY_ROW): DSR fogs every map piece by
 * the row of its own MSB FogID through the one curve (exe FUN_140565600 reads the row by id, no sky case; a RenderDoc
 * frame of Firelink holds three rows, TECH_DEBT map-15). rgb as the header's fog colour. */
typedef struct { float begin, end, max; unsigned char rgb[4]; } SkyRow;
#ifndef SKY_ROWS
#define SKY_ROWS 1  /* make DEFS=-DSKY_ROWS=0: every sky chunk under the header's one sky row at full density, as before */
#endif
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
#define CHUNK_SKY_ROW(f) ((f) >> 12 & 15)  /* a sky chunk's SkyRow, counted from 1; 0: the header's constant sky fog */
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
#define HZN_R 48.0f  /* (residency.h RES_PIN_R: 40 until 2026-11-12, 32 for a day) */  /* (Burg plan B3, 2026-11-08: 56 m before; the export's hard tier holds the coarse level to 40 m) */
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
/* the near / coarse swap fades (Burg plan B9, TECH_DEBT world-12): for NEAR_FADE map draws after a cell in view goes
 * from its coarse level to its near level, its resident coarse chunks are drawn again over the near level at alpha
 * falling from 1 to 0 (GS alpha FIX after the opaque map, Z tested HZN_ZBIAS nearer and not written, as the horizon
 * fade), so the detail dissolves in instead of popping. ASSUMED 10 draws (~0.33 s at 30 fps). The way back (near ->
 * coarse) is at the radius, behind the hysteresis, and stays a hard switch. make DEFS=-DNEAR_FADE=0: the hard swap */
#ifndef NEAR_FADE
#define NEAR_FADE 10
#endif
#define MAX_NEAR_FADE 32  /* cells fading per frame */
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
    unsigned char nfade;                 /* NEAR_FADE: map draws left of its coarse level fading out over the near one */
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
    const SkyRow *sky_row;  /* fog rows of the sky chunks (CHUNK_SKY_ROW) */
    unsigned sky_rows;      /* 0: a map.bin from before them */
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

/* the fog drawn now (begin, end, max, r, g, b) and the character light (ambient rgb, sun rgb, sun direction xyz),
 * moving towards fog_row[fog_area] */
#define FOG_NOW 27
/* + specular: env up rgb [15], env down rgb [18], specular light rgb [21], its direction x 127 [24] (0: none) */
extern int map_vu1_batches, map_ee_tris;
/* The fog / light rows follow the instance the player stands in (pfog_*) */

/* instance m's streams for resident chunk j, or NULL while its item is not in (mapstream.c: m selected) */
/* MAP_NEAR_SKIP (budget doc proposal 6, default on; make DEFS=-DMAP_NEAR_SKIP=0 = scan every visible cell) */
#ifndef MAP_NEAR_SKIP
#define MAP_NEAR_SKIP 1
#endif
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

extern MapInst mp_inst[MAP_INST];
extern float mp_fog_now[FOG_NOW];
extern int mp_fog_area;
extern const FogRow *mp_pfog_row;
extern unsigned mp_pfog_rows;
int map_inst_of(int col_block);
int mp_inst_col_block(int k);
const unsigned char *mp_res_base(const MapInst *m, unsigned j);
void mp_fog_ease(float rate);
void mp_hz_parse(MapInst *m);
void mp_tex_res(int k);
unsigned mp_tex_hook_age(int k, unsigned tag);
void mp_tex_hook_set(int k, unsigned tag, const void *p);
int mp_vis(int k, unsigned id);
unsigned mp_vis_gen(int k);
int mp_visible(const Chunk *c, const float *e, int sky);
int mp_draw_hz(const MapInst *m, const Chunk *c, int cls, int mode, int cull);
void mp_miss_scan(const MapInst *m, int k, int ci, int hzf, int has_hz, int *miss, int *mcov);
extern unsigned short mp_lpool[LPOOL_N];
extern unsigned mp_lpool_top, mp_lpool_gen, mp_lpool_builds, mp_lpool_resets;
void mp_miss_items(const MapInst *m, int k, int ci, int hzf, int has_hz, const unsigned char *const *ib, unsigned i0,
                       unsigned ni, int *miss, int *mcov);
unsigned mp_list_build(const MapInst *m, int k, int ci, int nd, int use_hz, const unsigned char *const *ib,
                           unsigned i0, const unsigned char *const *nbp, unsigned np, unsigned jb, unsigned je,
                           unsigned short *ls, unsigned *cut);
extern unsigned mp_chk_lists, mp_chk_bad;
void mp_build_index(MapInst *mi);
unsigned mp_draw_hz_only(int k, const float *e);

#endif
