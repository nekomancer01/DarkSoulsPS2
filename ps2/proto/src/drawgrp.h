/* DSR draw groups (TECH_DEBT area-5): a map chunk is drawn only while the collision part under the player shares a
 * draw group with the chunk's MSB piece, so low-detail stand-ins never draw on top of the detailed pieces
 * (data: dg.bin from tools/drawgroups.py, written by the map.bin export). World blocks (src/world.c): one dg.bin per
 * map instance k (= block k), each following its own collision part under the player. */
#ifndef DRAWGRP_H
#define DRAWGRP_H

#define DG_INST 5  /* >= MAP_INST */

/* blob: dg.bin of instance k (kept loaded); chunks: bump.bin's global chunk count it must cover, col_tris: that block's
 * collision.bin triangles. Returns 1 when the filter is on; without a matching blob every chunk stays shown.
 * dg_init(...) = dg_block_init(0, ...): the boot area (main.c). */
int dg_init(const void *blob, unsigned chunks, unsigned col_tris);
int dg_block_init(int k, const void *blob, unsigned chunks, unsigned col_tris);
/* Block k turned off: its set is forgotten (all shown when it comes back, until the player's floor in it is known) */
void dg_block_drop(int k);
/* Triangle of instance k's collision under the player (col_floor hit): its part's draw groups become k's shown set,
 * and with an area group (DG_SHARED) the shown set of every block of that group */
void dg_floor(int k, int tri);
/* the area group of instance k (dg.bin; 0 = none: k follows only its own floor) */
int dg_area(int k);
/* Streaming: dg_tick(frame) every frame; dg_stream_shown 1 when a floor of the last ~20 s (the union of their shared
 * masks) shows instance k's chunk (always 1 outside the shared area group); dg_stream_gen changes with that union */
void dg_tick(unsigned now);
int dg_stream_shown(int k, unsigned chunk);
unsigned dg_stream_gen(void);
/* Chunk number in bump.bin's order (resident index, near chunks: flags >> 8): 1 = draw */
int dg_shown(int k, unsigned chunk);
/* changes whenever a dg_shown answer of any instance may change (map.c MAP_TWO_PASS: cached per-cell chunk lists) */
unsigned dg_gen(void);
/* Event-switched pieces / objects (TECH_DEBT event-5; dg.bin DGM3 keys their chunks by MSB entity id): (De)activate
 * Object and Activate Map Part (src/event.c) hide / show every chunk of entity id in every instance, also in blocks
 * that load later. Returns 1 if a loaded instance has chunks of that id. dg_entity_drawn: 1 drawn, 0 hidden, -1 no
 * chunk of that id loaded; dg_entity_hidden: id is on the hidden list (map SFX use the same list, src/fx.c). */
int dg_entity(int id, int on);
int dg_entity_drawn(int id);
int dg_entity_hidden(int id);
/* Characters (src/enemy.c): the piece-mask index of instance k's enemy part at MSB position pos (enemies.bin's
 * floats), -1 = not found (always shown); dg_mask_shown: 1 while that set shares a group with k's current part */
int dg_char(int k, const float pos[3]);
int dg_mask_shown(int k, int mask);
/* DSR's rule for a piece of any map of the shared area group (DG_SHARED) by its draw groups themselves: 1 while they
 * share a group with the collision part under the player, or no part's groups are known yet (the far panorama,
 * mapblock.c mp_draw_far: pieces of blocks that are not loaded) */
int dg_groups_shown(const unsigned m[4]);

#endif
