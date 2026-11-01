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
extern int map_vu1_batches, map_ee_tris;
/* World blocks (src/world.c): instance k (0 = the boot block; MAP_INST at most) from a split map.bin: tab = the 80-byte
 * header + the map.bin bytes from the chunk table on, res = the bytes before it (the resident chunks' streams) or NULL
 * for a "lite" block that draws only its streamed near cells. col_block: its collision block (ColHit.block), for the
 * fog / light rows and draw groups under the player. k = 0 then needs map_upload; k > 0 takes texture slot k - 1.
 * Buffers stay owned by the caller and must outlive the instance plus two frames (VIF chains). */
#define MAP_INST 3
int map_block_init(int k, const void *tab, const void *res, int col_block);
void map_block_streams(int k, const void *res);  /* lite <-> full */
int map_block_stream_open(int k, const void *pack);  /* an open Pack (pack.h) of the block, kept open */
void map_block_drop(int k);
/* world.c world_systems, at boot: block k's dg.bin from its open Pack (per-block draw groups, src/drawgrp.c) */
void map_block_dg(int k, void *pack);
/* world.c, a full swap: block k's bump.bin read behind its mapres.bin in the full pool (normal-map relief, src/bump.c);
 * map_block_streams(k, NULL) drops it with the streams */
void map_block_bump(int k, const void *blob);
/* Height of the navmesh floor under (x, z) closest to y (within the step limits), or 0 if there is none */
int map_floor(float x, float z, float y, float *out_y);

#endif
