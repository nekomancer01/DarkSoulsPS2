#include <stdio.h>
#include <string.h>
#include "drawgrp.h"

/* dg.bin layout: tools/drawgroups.py */
typedef struct {
    unsigned magic, pieces, colls, chunks, tris, pad[3];
} DgHeader;

static const unsigned (*piece)[4], (*coll)[4];
static const unsigned char *chunk_dg, *tri_dg;
static unsigned n_pieces, n_chunks, n_tris;
static int cur = -1;                 /* collision mask in effect (-1: none yet, all shown) */
static unsigned char shown[256];     /* per piece mask: shares a group with coll[cur] */

int dg_init(const void *blob, unsigned chunks, unsigned col_tris)
{
    const DgHeader *h = blob;
    n_chunks = 0;
    cur = -1;
    if (!h || h->magic != 0x314D4744 || h->chunks < chunks || h->tris != col_tris || h->pieces > 256) {
        if (h)
            printf("DSRPROTO drawgrp: dg.bin does not match (chunks %u / %u, tris %u / %u): off\n",
                   h->chunks, chunks, h->tris, col_tris);
        return 0;
    }
    piece = (const void *)(h + 1);
    coll = (const void *)(piece + h->pieces);
    chunk_dg = (const unsigned char *)(coll + h->colls);
    tri_dg = chunk_dg + h->chunks;
    n_pieces = h->pieces;
    n_chunks = h->chunks;
    n_tris = h->tris;
    printf("DSRPROTO drawgrp: %u piece / %u collision group sets, %u chunks\n", h->pieces, h->colls, h->chunks);
    return 1;
}

void dg_floor(int tri)
{
    if (!n_chunks || (unsigned)tri >= n_tris || tri_dg[tri] == cur)
        return;
    cur = tri_dg[tri];
    const unsigned *c = coll[cur];
    for (unsigned i = 0; i < n_pieces; i++)
        shown[i] = (piece[i][0] & c[0]) | (piece[i][1] & c[1]) | (piece[i][2] & c[2]) | (piece[i][3] & c[3]) ? 1 : 0;
}

int dg_shown(unsigned chunk)
{
    return cur < 0 || chunk >= n_chunks || shown[chunk_dg[chunk]];
}
