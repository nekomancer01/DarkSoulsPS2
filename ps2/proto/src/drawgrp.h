/* DSR draw groups (TECH_DEBT area-5): a map chunk is drawn only while the collision part under the player shares a
 * draw group with the chunk's MSB piece, so low-detail stand-ins never draw on top of the detailed pieces
 * (data: dg.bin from tools/drawgroups.py, written by the map.bin export) */
#ifndef DRAWGRP_H
#define DRAWGRP_H

/* blob: dg.bin (kept loaded); chunks: bump.bin's global chunk count it must cover, col_tris: collision.bin's
 * triangles. Returns 1 when the filter is on; without a matching blob every chunk stays shown. */
int dg_init(const void *blob, unsigned chunks, unsigned col_tris);
/* Collision triangle under the player (col_floor hit): its part's draw groups become the shown set. */
void dg_floor(int tri);
/* Chunk number in bump.bin's order (resident index, near chunks: flags >> 8): 1 = draw */
int dg_shown(unsigned chunk);

#endif
