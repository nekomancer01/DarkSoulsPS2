#include <stdio.h>
#include <string.h>
#include "drawgrp.h"

/* dg.bin layout: tools/drawgroups.py ('DGM1': no character section, 'DGM2': no switched entities) */
#define DGM1 0x314D4744
#define DGM2 0x324D4744
#define DGM3 0x334D4744
typedef struct {
    unsigned magic, pieces, colls, chunks, tris, chars, ents, area;  /* area: 0 in files older than 2026-11-07 */
} DgHeader;
typedef struct { float pos[3]; unsigned mask; } DgChar;

typedef struct {
    const unsigned (*piece)[4], (*coll)[4];
    const unsigned char *chunk_dg, *tri_dg;
    const DgChar *chars;
    unsigned n_pieces, n_chunks, n_tris, n_chars;
    int cur;                   /* collision mask in effect (-1: none yet, all shown; DG_SHARED: -2 = the shared mask) */
    unsigned area;             /* area group (dg.bin header): blocks of one group share the mask (DG_SHARED) */
    unsigned char shown[256];  /* per piece mask: shares a group with coll[cur] */
    const int *ent;            /* switched entity ids (DGM3) */
    const unsigned char *chunk_ent;  /* per chunk: 1 + index into ent, 0 none */
    unsigned n_ents, gen;
    unsigned char ent_off[256];  /* per switched entity: hidden (from the list below at generation gen) */
} Dg;
static Dg dg[DG_INST];

/* DG_SHARED (default on; make DEFS=-DDG_SHARED=0 = each block follows its own floor, the old rule): DSR compares every
 * loaded map's pieces with the DrawGroups of the one collision part under the player, whichever map it is in. The maps
 * of an area load list (m10_00 / _01 / _02) share one bit space for that: Burg parts near the aqueduct show most of
 * Firelink's pieces, those deeper in none. With a set per block, Firelink kept the set of its last floor while the
 * player walked the Burg (its stand-ins of the aqueduct doorway stayed drawn there), drew every piece after a spawn
 * in the Burg (never stood on: all shown), and a block that turned on drew all its pieces, stand-ins over detail,
 * until the player stood on it (Ame's ISO report, 2026-11-07). */
#ifndef DG_SHARED
#define DG_SHARED 1
#endif
static unsigned sh_mask[4];
static unsigned sh_area;  /* 0: no shared mask yet */
static unsigned dg_now;   /* frames (dg_tick) */

static void apply_to(const Dg *d, const unsigned *c, unsigned char *out)
{
    for (unsigned i = 0; i < d->n_pieces; i++)
        out[i] = (d->piece[i][0] & c[0]) | (d->piece[i][1] & c[1]) | (d->piece[i][2] & c[2]) | (d->piece[i][3] & c[3]) ? 1 : 0;
}
static void dg_apply(Dg *d, const unsigned *c) { apply_to(d, c, d->shown); }

/* Streaming (mapstream.c via map.c ms_vis): the union of the shared masks of the floors stood on in the last
 * DG_UNION_FRAMES. A block's pieces outside it are not read or pinned (Firelink's coarse cells from deep in the Burg:
 * they starved the Burg's near level of arena room, the LOD part of Ame's report). The union, not the current part,
 * so a floor that flips every few seconds (Burg stairs) does not drop and read the same cells again. */
#define DG_UNION_FRAMES 600u  /* 20 s at 30 fps, ASSUMED */
#define DG_HIST 16
static struct { unsigned m[4], t; } hist[DG_HIST];
static int nhist;
static unsigned uni[4], ugen = 1, uarea;
static unsigned char ushown[DG_INST][256];

static void uni_push(const unsigned *c, unsigned now)
{
    if (nhist == DG_HIST)
        memmove(hist, hist + 1, --nhist * sizeof *hist);
    memcpy(hist[nhist].m, c, sizeof hist[nhist].m);
    hist[nhist++].t = now;
}

/* Entities events switched off (TECH_DEBT event-5), one list for every instance, so a block that loads later or a
 * second block of the same map hides them too; event.c's constructors set them again when a map's events start */
#define DG_HIDDEN 128
static int hidden[DG_HIDDEN], n_hidden;
static unsigned hidden_gen = 1;

int dg_block_init(int k, const void *blob, unsigned chunks, unsigned col_tris)
{
    const DgHeader *h = blob;
    if (k < 0 || k >= DG_INST)
        return 0;
    Dg *d = &dg[k];
    memset(d, 0, sizeof *d);
    d->cur = -1;
    if (!h || (h->magic != DGM1 && h->magic != DGM2 && h->magic != DGM3) || h->chunks < chunks || h->tris != col_tris ||
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
    d->n_chars = h->magic != DGM1 ? h->chars : 0;
    d->n_pieces = h->pieces;
    d->n_chunks = h->chunks;
    d->n_tris = h->tris;
    d->area = DG_SHARED ? h->area : 0;
    if (h->magic == DGM3 && h->ents && h->ents < 256) {
        d->ent = (const int *)(d->chars + d->n_chars);
        d->chunk_ent = (const unsigned char *)(d->ent + h->ents);
        d->n_ents = h->ents;
        d->gen = 0;  /* ent_off from the hidden list at the first dg_shown */
    }
    printf("DSRPROTO drawgrp: %d: %u piece / %u collision group sets, %u chunks, %u characters, %u switched parts, "
           "area %u\n", k, h->pieces, h->colls, h->chunks, d->n_chars, d->n_ents, d->area);
    if (d->area && d->area == sh_area) {  /* a block turning on shows what the part under the player shows */
        dg_apply(d, sh_mask);
        d->cur = -2;
    }
    if (d->area && d->area == uarea) {
        apply_to(d, uni, ushown[k]);
        ugen++;
    }
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
    if (!d->n_chunks || (unsigned)tri >= d->n_tris)
        return;
    const unsigned *c = d->coll[d->tri_dg[tri]];
    if (d->area) {  /* DG_SHARED: this part's groups for every block of the area group */
        if (sh_area == d->area && !memcmp(sh_mask, c, sizeof sh_mask))
            return;
        if (sh_area != d->area)
            nhist = 0;  /* another area group: its old masks mean nothing here */
        sh_area = d->area;
        memcpy(sh_mask, c, sizeof sh_mask);
        uni_push(c, dg_now);
        printf("drawgrp: %d: groups %08x %08x %08x %08x\n", k, c[0], c[1], c[2], c[3]);  /* tests compare builds by it */
        for (int j = 0; j < DG_INST; j++)
            if (dg[j].n_chunks && dg[j].area == sh_area) {
                dg_apply(&dg[j], sh_mask);
                dg[j].cur = -2;
            }
        return;
    }
    if (d->tri_dg[tri] == d->cur)
        return;
    d->cur = d->tri_dg[tri];
    printf("drawgrp: %d: groups %08x %08x %08x %08x\n", k, c[0], c[1], c[2], c[3]);  /* tests compare builds by it */
    dg_apply(d, c);
}

void dg_tick(unsigned now)
{
    dg_now = now;
    if (!sh_area)
        return;
    unsigned u[4] = {0, 0, 0, 0};
    int keep = 0;
    for (int i = 0; i < nhist; i++)  /* the current mask (the last) always counts */
        if (i == nhist - 1 || now - hist[i].t <= DG_UNION_FRAMES) {
            for (int w = 0; w < 4; w++)
                u[w] |= hist[i].m[w];
            hist[keep++] = hist[i];
        }
    nhist = keep;
    if (uarea == sh_area && !memcmp(u, uni, sizeof u))
        return;
    memcpy(uni, u, sizeof uni);
    uarea = sh_area;
    for (int j = 0; j < DG_INST; j++)
        if (dg[j].n_chunks && dg[j].area == uarea)
            apply_to(&dg[j], uni, ushown[j]);
    ugen++;
}

unsigned dg_stream_gen(void) { return ugen; }

int dg_stream_shown(int k, unsigned chunk)
{
    if (k < 0 || k >= DG_INST)
        return 1;
    const Dg *d = &dg[k];
    if (!d->area || d->area != uarea || chunk >= d->n_chunks)
        return 1;
    return ushown[k][d->chunk_dg[chunk]];
}

int dg_area(int k)
{
    return k >= 0 && k < DG_INST && dg[k].n_chunks ? (int)dg[k].area : 0;
}

int dg_entity_hidden(int id)
{
    for (int i = 0; i < n_hidden; i++)
        if (hidden[i] == id)
            return 1;
    return 0;
}

int dg_entity(int id, int on)
{
    int i;
    for (i = 0; i < n_hidden && hidden[i] != id; i++)
        ;
    if (on && i < n_hidden)
        hidden[i] = hidden[--n_hidden];
    else if (!on && i == n_hidden && n_hidden < DG_HIDDEN)
        hidden[n_hidden++] = id;
    else
        return dg_entity_drawn(id) >= 0;
    hidden_gen++;
    return dg_entity_drawn(id) >= 0;
}

int dg_entity_drawn(int id)
{
    for (int k = 0; k < DG_INST; k++)
        for (unsigned i = 0; i < dg[k].n_ents; i++)
            if (dg[k].ent[i] == id)
                return !dg_entity_hidden(id);
    return -1;
}

int dg_shown(int k, unsigned chunk)
{
    Dg *d = &dg[k];
    if (chunk >= d->n_chunks)
        return 1;
    unsigned e = d->n_ents ? d->chunk_ent[chunk] : 0;
    if (e) {
        if (d->gen != hidden_gen) {
            for (unsigned i = 0; i < d->n_ents; i++)
                d->ent_off[i] = dg_entity_hidden(d->ent[i]);
            d->gen = hidden_gen;
        }
        if (d->ent_off[e - 1])
            return 0;
    }
    return d->cur == -1 || d->shown[d->chunk_dg[chunk]];
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
    return d->cur == -1 || (unsigned)mask >= d->n_pieces || d->shown[mask];
}
