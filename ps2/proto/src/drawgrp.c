#include <stdio.h>
#include <string.h>
#include "drawgrp.h"

/* dg.bin layout: tools/drawgroups.py ('DGM1': no character section) */
typedef struct {
    unsigned magic, pieces, colls, chunks, tris, chars, pad[2];
} DgHeader;
typedef struct { float pos[3]; unsigned mask; } DgChar;

typedef struct {
    const unsigned (*piece)[4], (*coll)[4];
    const unsigned char *chunk_dg, *tri_dg;
    const DgChar *chars;
    unsigned n_pieces, n_chunks, n_tris, n_chars;
    int cur;                   /* collision mask in effect (-1: none yet, all shown) */
    unsigned char shown[256];  /* per piece mask: shares a group with coll[cur] */
} Dg;
static Dg dg[DG_INST];

int dg_block_init(int k, const void *blob, unsigned chunks, unsigned col_tris)
{
    const DgHeader *h = blob;
    if (k < 0 || k >= DG_INST)
        return 0;
    Dg *d = &dg[k];
    memset(d, 0, sizeof *d);
    d->cur = -1;
    if (!h || (h->magic != 0x314D4744 && h->magic != 0x324D4744) || h->chunks < chunks || h->tris != col_tris ||
        h->pieces > 256) {
        if (h)
            printf("DSRPROTO drawgrp: %d: dg.bin does not match (chunks %u / %u, tris %u / %u): off\n", k, h->chunks,
                   chunks, h->tris, col_tris);
        return 0;
    }
    d->piece = (const void *)(h + 1);
    d->coll = (const void *)(d->piece + h->pieces);
    d->chunk_dg = (const unsigned char *)(d->coll + h->colls);
    d->tri_dg = d->chunk_dg + h->chunks;
    d->chars = (const DgChar *)(d->chunk_dg + ((h->chunks + h->tris + 3) & ~3u));
    d->n_chars = h->magic == 0x324D4744 ? h->chars : 0;
    d->n_pieces = h->pieces;
    d->n_chunks = h->chunks;
    d->n_tris = h->tris;
    printf("DSRPROTO drawgrp: %d: %u piece / %u collision group sets, %u chunks, %u characters\n", k, h->pieces,
           h->colls, h->chunks, d->n_chars);
    return 1;
}

int dg_init(const void *blob, unsigned chunks, unsigned col_tris)
{
    return dg_block_init(0, blob, chunks, col_tris);
}

void dg_block_drop(int k)
{
    if (k > 0 && k < DG_INST)
        dg[k].cur = -1;
}

void dg_floor(int k, int tri)
{
    if (k < 0 || k >= DG_INST)
        return;
    Dg *d = &dg[k];
    if (!d->n_chunks || (unsigned)tri >= d->n_tris || d->tri_dg[tri] == d->cur)
        return;
    d->cur = d->tri_dg[tri];
    const unsigned *c = d->coll[d->cur];
    printf("drawgrp: %d: groups %08x %08x %08x %08x\n", k, c[0], c[1], c[2], c[3]);  /* tests compare builds by it */
    for (unsigned i = 0; i < d->n_pieces; i++)
        d->shown[i] =
            (d->piece[i][0] & c[0]) | (d->piece[i][1] & c[1]) | (d->piece[i][2] & c[2]) | (d->piece[i][3] & c[3]) ? 1 : 0;
}

int dg_shown(int k, unsigned chunk)
{
    const Dg *d = &dg[k];
    return d->cur < 0 || chunk >= d->n_chunks || d->shown[d->chunk_dg[chunk]];
}

int dg_char(int k, const float pos[3])
{
    if (k < 0 || k >= DG_INST)
        return -1;
    const Dg *d = &dg[k];
    for (unsigned i = 0; i < d->n_chars; i++)
        if (!memcmp(d->chars[i].pos, pos, sizeof d->chars[i].pos))
            return d->chars[i].mask < d->n_pieces ? (int)d->chars[i].mask : -1;
    return -1;
}

int dg_mask_shown(int k, int mask)
{
    if (k < 0 || k >= DG_INST || mask < 0)
        return 1;
    const Dg *d = &dg[k];
    return d->cur < 0 || (unsigned)mask >= d->n_pieces || d->shown[mask];
}
