/* material.h: material classes (Polaris 2.1). DSR's MTD files map to a small set of classes (material_table.h,
 * generated from tools/material.py); each names its VU1 program, GS state and passes. The exporter assigns the class
 * (tools/material.py classify) and stores it per map.bin texture entry (flags bits 4-7); a chunk's class is its
 * texture's class with the chunk flags applied (MAT_CHUNK_CLASS: additive chunks, the sky role).
 * The map passes take a chunk's ALPHA, Z mask, blend bit (the batch TEST) and passes from its class row (map_i.h mp_row,
 * Polaris 2.1 PR 2); mat_census checks at load that the chunk flags the streaming code still reads agree. */
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

#ifndef MAT_GS_TABLE
#define MAT_GS_TABLE 1  /* make DEFS=-DMAT_GS_TABLE=0: blend state from the chunk flags as before the table (map_i.h mp_row) */
#endif

/* a map.bin texture entry's class (tex_tab = the texture table, 32-byte entries) */
static inline int mat_tex_class(const unsigned char *tex_tab, int tex)
{
    int c = tex_tab[32 * tex + 7] >> MAT_TEX_SHIFT;
    return c < MAT_COUNT ? c : MAT_OPAQUE;
}

/* the class the chunk flags alone give (MAT_GS_TABLE=0, maps exported before the classes, untextured chunks): what the
 * passes drew before the table: blended (additive outside the sky pass), sky, else opaque (material.flag_class) */
static inline int mat_flag_class(unsigned flags)
{
    return flags & 2 ? (flags & 4 && !(flags & 8) ? MAT_ADDITIVE : MAT_BLEND) : flags & 8 ? MAT_SKY : MAT_OPAQUE;
}

/* a chunk's class (material.chunk_class): its texture's class, SKY for an unblended sky-role chunk; tex < 0: from its
 * flags */
static inline int mat_chunk_class(const unsigned char *tex_tab, int tex, unsigned flags)
{
    return tex < 0 ? mat_flag_class(flags) : MAT_CHUNK_CLASS(mat_tex_class(tex_tab, tex), flags);
}

/* At map / block load: chunks per class to the EE log, and each chunk's class checked against its flags (blended and
 * additive bits = the class row's Z mask / ALPHA word): "TEST error: material ..." on a mismatch. chunks: the map.bin
 * chunk table (int tex, u32 flags, ... in rows of stride bytes); has_cls: the cell table says the export wrote classes
 * (tools/mapcells.py CELLS_MATCLS), else one note and no check. Returns the mismatches. */
int mat_census(int block, const unsigned char *tex_tab, int ntex, const void *chunks, int n, int stride, int has_cls);

#endif
