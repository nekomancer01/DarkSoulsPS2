/* material.h: material classes (Polaris 2.1). DSR's MTD files map to a small set of classes (material_table.h,
 * generated from tools/material.py); each names its VU1 program, GS state and passes. The exporter assigns the class
 * (tools/material.py classify) and stores it per map.bin texture entry (flags bits 4-7); a chunk's class is its
 * texture's class with the chunk flags applied (MAT_CHUNK_CLASS: additive chunks, the sky role).
 * PR 1 (this file's first version) records the classes and checks them against the chunk flags at load; the map
 * passes still choose their GS state from the flags (map.c blend_regs) until Polaris 2.1 PR 2 reads the table. */
#ifndef MATERIAL_H
#define MATERIAL_H

#include <tamtypes.h>
#include "gs.h"
#include "material_table.h"

typedef struct {
    const char *name;
    int prog;               /* VU1 program (vu1_layout.h VU1_PROGRAMS row, as VU1L_CODE_<name>: its micro address) */
    u64 alpha, test;        /* GS ALPHA / TEST words (gs.h) */
    unsigned char zmsk;     /* 1: no Z write (ZBUF ZMSK) */
    unsigned char passes;   /* MP_* */
} MatRow;

extern const MatRow mat_rows[MAT_COUNT];

/* a map.bin texture entry's class (tex_tab = the texture table, 32-byte entries); tex < 0: OPAQUE */
static inline int mat_tex_class(const unsigned char *tex_tab, int tex)
{
    int c = tex < 0 ? MAT_OPAQUE : tex_tab[32 * tex + 7] >> MAT_TEX_SHIFT;
    return c < MAT_COUNT ? c : MAT_OPAQUE;
}

/* a chunk's class from its texture and flags */
static inline int mat_chunk_class(const unsigned char *tex_tab, int tex, unsigned flags)
{
    return MAT_CHUNK_CLASS(mat_tex_class(tex_tab, tex), flags);
}

/* At map / block load: chunks per class to the EE log, and each chunk's class checked against its flags (blended and
 * additive bits = the class row's Z mask / ALPHA word): "TEST error: material ..." on a mismatch. chunks: the map.bin
 * chunk table (int tex, u32 flags, ... in rows of stride bytes); has_cls: the cell table says the export wrote classes
 * (tools/mapcells.py CELLS_MATCLS), else one note and no check. Returns the mismatches. */
int mat_census(int block, const unsigned char *tex_tab, int ntex, const void *chunks, int n, int stride, int has_cls);

#endif
