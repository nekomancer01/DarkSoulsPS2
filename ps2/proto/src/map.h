/* A DS1 map (map pieces, textures, navmesh) exported by tools/export_map.py into map.bin */
#ifndef MAP_H
#define MAP_H

#include "render.h"

/* blob: map.bin (128-byte aligned, DMA-ready, kept while the area is drawn: streams and textures are used in place) */
int map_init(const void *blob);
/* Upload map textures; returns 0 on success, -1 if VRAM ran out */
int map_upload(GSGLOBAL *gs);
void map_spawn(V3 *pos, float *yaw);
/* Chunks in the loaded map.bin (bump.bin's chunk table follows them) */
unsigned map_chunk_count(void);
/* Queue the chunks visible from eye: inside ones as VU1 batches (between vu1_begin and vu1_end), ones crossing the
 * near plane or guard band clipped on the EE through gsKit. Returns triangles submitted. */
/* Passes: sky (its own constant fog, no distance cull), opaque map (map fog), then blended effects sorted far to near
 * (alpha blend, then additive; Z tested, not written). GS ALPHA / ZBUF / FOGCOL are changed between batches and
 * restored to gsKit's values at the end. */
int map_draw(V3 eye);
/* Player feet, once a frame before map_draw: the fog follows the FogID of the collision part under them (DSR's area
 * draw params), easing over ~0.3 s; needs collision.bin loaded (col_init) */
/* DSM5 maps: open the area pack's cells.bin for streaming the near level (after map_init); 0 if nothing streams.
 * map_set_player then drives the streamer every frame (src/mapstream.c) */
int map_stream_open(const char *pak_path);
void map_set_player(V3 feet);
/* Light for a skinned character at pos (feet), in 0x80 units for texture modulation: colour = amb + sun x
 * max(n . dir, 0) with dir the world direction towards the sun. From the player's area row (DSR lights characters
 * with the LightID of the collision part, export_map.char_light), plus the player-attached point light by pos's
 * distance to the player (map_set_player). */
void map_char_light(V3 pos, float amb[3], float sun[3], float dir[3]);
/* Specular terms of the player's area for vu1_skin_spec (0x80 = 1.0 for texture modulation): env reflection up /
 * down (GI_EnvSpc probe x LightBank envSpc), the LightBank specular light and its world direction; all 0 without a
 * map (no specular). */
void map_char_spec(float env_up[3], float env_down[3], float spec_i[3], float spec_dir[3]);
/* Far depth of field of the player's area (DofBank row of its collision part: farDofBegin / farDofEnd in metres,
 * farDofMul as 0..1); 0 if the area has none. main.c's flip blurs by it. */
int map_far_dof(float *begin, float *end, float *mul);
extern int map_vu1_batches, map_ee_tris;
/* World blocks (src/world.c): instance k (0 = the boot block; MAP_INST at most) from a split map.bin: tab = the 80-byte
 * header + the map.bin bytes from the chunk table on, res = the bytes before it (the resident chunks' streams), or
 * NULL: map_block_stream_open then streams them per cell from the block's mapres.bin, with each texture's mip 0
 * (mapstream_res_open; no full swap, TECH_DEBT world-2 / world-5). col_block: its collision block (ColHit.block), for
 * the fog / light rows and draw groups under the player. k = 0 then needs map_upload; k > 0 takes a free texture
 * slot. Buffers stay owned by the caller and must outlive the instance plus two frames (VIF chains). */
#define MAP_INST 5
/* the next map_block_init(k, ...) takes maptab.bin in three pieces: tab = bytes [0, b_off + ...), b = the piece
 * from byte b_off (textures, navmesh, fog rows), c = the piece from byte c_off (cell table, horizon) (world.c
 * WORLD_TAB_SPLIT) */
void map_block_split(int k, const void *b, unsigned b_off, const void *c, unsigned c_off);
int map_block_init(int k, const void *tab, const void *res, int col_block);
int map_tex_slot(int k);  /* texcache slot of instance k > 0 while it is on (texcache.h TC_BLOCKS), else -1 */
int map_block_stream_open(int k, const void *pack);  /* an open Pack (pack.h) of the block, kept open */
void map_block_drop(int k);
/* map_block_drop, but its draw groups stay (world.c WORLD_LITE_DIET: a block only kept keeps its collision, events and
 * characters, which dg_mask_shown still answers for; map_block_init brings the drawing back) */
void map_block_lean(int k);
/* world.c world_systems, at boot: block k's dg.bin from its open Pack (per-block draw groups, src/drawgrp.c) */
void map_block_dg(int k, void *pack);
/* the boot instance's pvs.bin (main.c, from the area pack; kept loaded) */
void map_pvs_init(const void *blob);
/* world.c: block k's bump.bin while the player stands in it (normal-map relief, src/bump.c); NULL drops it */
void map_block_bump(int k, const void *blob);
/* Height of the navmesh floor under (x, z) closest to y (within the step limits), or 0 if there is none */
int map_floor(float x, float z, float y, float *out_y);

#endif
