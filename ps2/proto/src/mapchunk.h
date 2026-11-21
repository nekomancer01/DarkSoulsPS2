/* Map chunk drawing and the ZSPLIT passes (src/mapchunk.c). Map files only. */
#ifndef MAPCHUNK_H
#define MAPCHUNK_H
#include "map_i.h"

/* chunk c (its first n triangles) of instance m with streams at base: cls = mp_visible's class, mode R_FOG / R_BLEND
 * / R_ADD, no = its number in bump.bin / dg.bin. In the far pass the part nearer than ZSPLIT is recorded for the near
 * pass. Returns triangles. */
int mp_draw_chunk(const MapInst *m, const unsigned char *base, const Chunk *c, unsigned n, int cls, int mode,
                  unsigned no);
int mp_draw_chunk_far(const MapInst *m, const unsigned char *base, const Chunk *c, unsigned n, int cls, int mode,
                      unsigned no);
int mp_draw_chunk_tex(const MapInst *m, const unsigned char *base, const Chunk *c, unsigned n, int cls, int mode,
                      unsigned no, GSTEXTURE *late);
/* (mp_draw_hz: map_i.h) */
extern int mp_emit_close;  /* MAP_TWO_PASS emit: the cell lies nearer than the edge fog's start */
/* GS state for the map passes, recorded for the near pass while in the far pass */
void mp_zs_regs(int n, const u64 *regs);
void mp_zs_fog(float a, float b, float fmin, float fmax);
void mp_zs_zbias(float k);
void mp_zs_edge(float a2);
void mp_opaque_regs(GSGLOBAL *gs);            /* gsKit's ALPHA, ZBUF writing Z */
void mp_row_regs(GSGLOBAL *gs, const MatRow *row);  /* a material class row's ALPHA / ZBUF */
/* ZSPLIT: map_draw's far pass from mp_zs_begin (recording the near one) to mp_zs_end (Z reset, near pass replayed) */
void mp_zs_begin(void);
void mp_zs_end(void);
int mp_zs_far(void);  /* in the far pass */
/* 1: the box reaches beyond ZSPLIT (drawn now), 2: nearer than it (near pass), 3 both; *far_cls its class for the
 * far pass */
int mp_zs_route(const Chunk *c, int *far_cls, int cls);
void mp_zs_relief(void);  /* the relief / player light / specular passes, or their near-pass record */

#endif
