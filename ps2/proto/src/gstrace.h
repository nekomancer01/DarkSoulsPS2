/* gstrace.h: the effective-draw decoder for the Polaris 1.6 gate (OT_HASH builds; gstrace.c) */
#ifndef GSTRACE_H
#define GSTRACE_H
#include <tamtypes.h>
void gstrace_begin(int dump);  /* a new frame; dump: print that many decoded primitives */
void gstrace_chain(const void *start, const void *end);    /* a DMA chain of CNT / END tags with GIF data */
void gstrace_packets(const void *start, const void *end);  /* bare GIF packets */
/* the frame's hash of effective draws of the EE-path 3D (and effects) and their count; the 2D layer's (every vertex at
 * Z 0xFFFF: HUD, menus, text) in ui_hash / ui_n */
unsigned gstrace_end(unsigned *prims, unsigned *ui_hash, unsigned *ui_n);
#endif
