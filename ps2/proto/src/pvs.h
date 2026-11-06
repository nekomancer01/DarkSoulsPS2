/* Precomputed visibility (2026-11-06): pvs.bin per map instance (tools/pvs_bake.c), one row of visible PVS units per
 * 8 m camera cell. A unit is one MSB part within one 32 m map cell; a chunk's unit is its cell's first unit plus a byte.
 * map.c draws a chunk only while it is in the row of the camera's cell AND its draw groups are on (drawgrp.c) AND it
 * is in the frustum; mapstream.c reads and keeps only items that some cell within PVS_STREAM_R of the camera sees.
 * A camera in a cell without a row (beyond every sampled cell: cutscenes, debug cameras) draws everything, as
 * before. `make PVS=0` (-DNO_PVS): no instance takes its pvs.bin, everything is drawn. */
#ifndef PVS_H
#define PVS_H

#define PVS_INST 5          /* >= MAP_INST */
#define PVS_STREAM_R 16.0f  /* the streamer's union: rows of the cells within this of the camera (~2-3 s of running) */

/* instance k's pvs.bin (kept loaded by the caller): chunks = the map's resident chunk count (the file covers at least
 * these), entries = its cell table entries. Returns 1 when on. */
int pvs_block_init(int k, const void *blob, unsigned chunks, unsigned entries);
void pvs_block_drop(int k);
/* once per frame before the map draw: every instance's row for camera e, the streaming union */
void pvs_eye(const float e[3]);
/* chunk number `chunk` (dg.bin / bump.bin order) of cell table entry `entry` of instance k: 1 = in the camera's row */
int pvs_chunk(int k, unsigned entry, unsigned chunk);
/* any of chunks [first, first + n) of entry is in the row now (now = 1) or in the streaming union (now = 0) */
int pvs_range(int k, unsigned entry, unsigned first, unsigned n, int now);
/* changes whenever instance k's row or union may answer differently */
unsigned pvs_gen(int k);
/* stats: units of instance k in the current row / all (0 / 0: off or no row) */
void pvs_stats(int k, unsigned *in_row, unsigned *units);

#endif
