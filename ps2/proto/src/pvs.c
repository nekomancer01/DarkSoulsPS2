#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pvs.h"

/* pvs.bin 'PVS3' (tools/pvs_bake.c): header u32 magic, row bits, chunks, entries, cells, rows, row bytes, f32 cell
 * size; u16 x (entries + 1) list starts; u16 lists (a unit's row bit, PVS_ALWAYS, PVS_NEVER); u8 per chunk (index in
 * its entry's list, 0xFF always drawn); u32 key + u16 row per sampled camera cell; rows */
#define PVS3 0x33535650u
#define PVS_ALWAYS 0xFFFFu
#define PVS_NEVER 0xFFFEu
typedef struct {
    const unsigned short *eoff, *ulist;
    const unsigned char *loc;
    const unsigned *keys;
    const unsigned short *row_of;
    const unsigned char *rows;
    unsigned units, chunks, entries, cells, nrows, rb;
    float cell;
    unsigned key;               /* camera cell of the current row (~0u: none yet) */
    const unsigned char *row;   /* NULL: no row (draw everything) */
    unsigned char *uni, *tmp;   /* streaming union, a missing cell's neighbour union: rb bytes each */
    int uni_all;                /* the union is "everything" */
} Pvs;
static Pvs pv[PVS_INST];
static unsigned gen[PVS_INST];  /* per instance: a crossing in one block's camera grid re-checks only its items */

int pvs_block_init(int k, const void *blob, unsigned chunks, unsigned entries)
{
    if (k < 0 || k >= PVS_INST)
        return 0;
    Pvs *p = &pv[k];
    free(p->uni), free(p->tmp);
    memset(p, 0, sizeof *p);
    p->key = ~0u;
    gen[k]++;
#ifdef NO_PVS
    (void)blob, (void)chunks, (void)entries;
    return 0;
#else
    const unsigned *h = blob;
    if (!h || h[0] != PVS3 || !h[4] || h[2] < chunks || h[3] != entries) {
        if (h && h[0] == PVS3 && h[4])
            printf("DSRPROTO pvs: %d: pvs.bin does not match (chunks %u / %u, entries %u / %u): off\n", k, h[2], chunks,
                   h[3], entries);
        return 0;
    }
    const unsigned char *b = blob;
    unsigned o = 32;
    p->units = h[1], p->chunks = h[2], p->entries = h[3], p->cells = h[4], p->nrows = h[5], p->rb = h[6];
    memcpy(&p->cell, &h[7], 4);
    p->eoff = (const unsigned short *)(b + o), o += (2 * (p->entries + 1) + 3) & ~3u;
    p->ulist = (const unsigned short *)(b + o), o += (2 * p->eoff[p->entries] + 3) & ~3u;
    p->loc = b + o, o += (p->chunks + 3) & ~3u;
    p->keys = (const unsigned *)(b + o), o += 4 * p->cells;
    p->row_of = (const unsigned short *)(b + o), o += (2 * p->cells + 3) & ~3u;
    p->rows = b + o;
    p->uni = malloc(p->rb + 1), p->tmp = malloc(p->rb + 1);
    if (!p->uni || !p->tmp || !(p->cell > 0)) {
        free(p->uni), free(p->tmp);
        memset(p, 0, sizeof *p);
        return 0;
    }
    p->uni_all = 1;
    printf("DSRPROTO pvs: %d: %u unit lists, %u camera cells of %.0f m, %u rows of %u bits\n", k, p->eoff[p->entries],
           p->cells, (double)p->cell, p->nrows, p->units);
    return 1;
#endif
}

void pvs_block_drop(int k)
{
    if (k >= 0 && k < PVS_INST) {
        free(pv[k].uni), free(pv[k].tmp);
        memset(&pv[k], 0, sizeof pv[k]);
        gen[k]++;
    }
}

unsigned pvs_gen(int k) { return k >= 0 && k < PVS_INST ? gen[k] : 0; }

static unsigned key_of(int x, int y, int z)
{
    x += 512, y += 512, z += 512;
    if (x < 0 || y < 0 || z < 0 || x > 1023 || y > 1023 || z > 1023)
        return ~0u;
    return (unsigned)x | (unsigned)y << 10 | (unsigned)z << 20;
}

static const unsigned char *find1(const Pvs *p, unsigned key)
{
    unsigned lo = 0, hi = p->cells;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (p->keys[m] < key)
            lo = m + 1;
        else
            hi = m;
    }
    if (lo < p->cells && p->keys[lo] == key && p->row_of[lo] < p->nrows)
        return p->rows + (unsigned)p->row_of[lo] * p->rb;
    return NULL;
}

/* the row of camera cell (x, y, z): its own, else the union of its stored neighbours (into tmp; the bake's
 * validation does the same), else NULL */
static const unsigned char *find(const Pvs *p, int x, int y, int z, unsigned char *tmp)
{
    static const signed char nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    unsigned key = key_of(x, y, z);
    const unsigned char *r = key == ~0u ? NULL : find1(p, key);
    if (r || !tmp)
        return r;
    int any = 0;
    memset(tmp, 0, p->rb);
    for (int d = 0; d < 6; d++) {
        unsigned nk = key_of(x + nb[d][0], y + nb[d][1], z + nb[d][2]);
        const unsigned char *n = nk == ~0u ? NULL : find1(p, nk);
        if (n) {
            any = 1;
            for (unsigned i = 0; i < p->rb; i++)
                tmp[i] |= n[i];
        }
    }
    return any ? tmp : NULL;
}

void pvs_eye(const float e[3])
{
    for (int k = 0; k < PVS_INST; k++) {
        Pvs *p = &pv[k];
        if (!p->uni)
            continue;
        int x = (int)floorf(e[0] / p->cell), y = (int)floorf(e[1] / p->cell), z = (int)floorf(e[2] / p->cell);
        unsigned key = key_of(x, y, z);
        if (key == p->key)
            continue;
        p->key = key;
        p->row = find(p, x, y, z, p->tmp);
        /* union for the streamer: every row within PVS_STREAM_R; everything when the camera's own cell has none */
        p->uni_all = !p->row;
        if (p->row) {
            memset(p->uni, 0, p->rb);
            int r = (int)ceilf(PVS_STREAM_R / p->cell);
            for (int dz = -r; dz <= r; dz++)
                for (int dy = -r; dy <= r; dy++)
                    for (int dx = -r; dx <= r; dx++) {
                        if ((float)(dx * dx + dy * dy + dz * dz) * p->cell * p->cell >
                            (PVS_STREAM_R + p->cell) * (PVS_STREAM_R + p->cell))
                            continue;
                        const unsigned char *rw = find(p, x + dx, y + dy, z + dz, NULL);
                        if (rw)
                            for (unsigned i = 0; i < p->rb; i++)
                                p->uni[i] |= rw[i];
                    }
        }
        gen[k]++;
    }
}

/* 1 when the chunk's unit is in row r (NULL: no row, every chunk is) */
static inline int seen(const Pvs *p, const unsigned char *r, unsigned entry, unsigned chunk)
{
    if (!r || chunk >= p->chunks || entry >= p->entries)
        return 1;
    unsigned l = p->loc[chunk], o = p->eoff[entry] + l;
    if (l == 0xFF || o >= p->eoff[entry + 1])
        return 1;
    unsigned b = p->ulist[o];
    if (b == PVS_ALWAYS || b >= p->units)
        return b != PVS_NEVER;
    return r[b >> 3] >> (b & 7) & 1;
}

int pvs_chunk(int k, unsigned entry, unsigned chunk)
{
    if (k < 0 || k >= PVS_INST)
        return 1;
    return seen(&pv[k], pv[k].row, entry, chunk);
}

int pvs_range(int k, unsigned entry, unsigned first, unsigned n, int now)
{
    if (k < 0 || k >= PVS_INST)
        return 1;
    const Pvs *p = &pv[k];
    const unsigned char *r = now ? p->row : p->uni_all ? NULL : p->uni;
    if (!p->uni || !r)
        return 1;
    for (unsigned c = first; c < first + n; c++)
        if (seen(p, r, entry, c))
            return 1;
    return 0;
}

void pvs_stats(int k, unsigned *in_row, unsigned *units)
{
    *in_row = *units = 0;
    if (k < 0 || k >= PVS_INST || !pv[k].row)
        return;
    const Pvs *p = &pv[k];
    *units = p->units;  /* row bits */
    for (unsigned u = 0; u < p->units; u++)
        *in_row += p->row[u >> 3] >> (u & 7) & 1;
}
