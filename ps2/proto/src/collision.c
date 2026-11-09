/* Map collision queries over collision.bin (layout: tools/export_collision.py): quantized shared vertices, triangles
 * with HitFilterID + material, uniform x/z grid of triangle lists. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "collision.h"
#include "vmath.h"


typedef struct {
    char magic[4];
    unsigned version, nverts, ntris;
    float origin[3], scale;
    float x0, z0, cell;
    unsigned gw, gd;
    unsigned verts_off, tris_off, cells_off, list_off, nlist;
} Header;
typedef struct { unsigned short v[3]; unsigned char hit_filter, material; } Tri;
/* DCOL v2 (tools/export_collision.py write_v2, COL_V1=1 exports v1): fragments of at most 255 vertices / triangles
 * with u16 vertices from the fragment's corner, 4-byte triangles (u8 a, b, c, pattern), a grid per fragment with u8
 * lists, a top grid of fragments, and per triangle slot its v1 number (gid): what ColHit.tri, events, draw groups,
 * fog rows and block links use. ~17 % smaller (Burg west 835 -> 693 KB), the same quantized vertices. */
typedef struct {
    char magic[4];
    unsigned version, nverts, ntris;
    float origin[3], scale;
    float x0, z0, cell;
    unsigned gw, gd;
    unsigned frag_off, nfrag, top_off, toplist_off, ntoplist, pat_off, npat, gid_off;
    float ymin, ymax, fcell;
} Header2;
typedef struct {
    short lo[3], hi[3];
    unsigned short ntri, nvert;
    unsigned tri0, data;
    unsigned char gw, gd;
    unsigned short pad, pad2[2];
} Frag;
_Static_assert(sizeof(Header2) == 96 && sizeof(Frag) == 32, "DCOL v2");
/* Collision tiles (export_collision.py COL_TILE, the cube-grid plan's step 1): a v2 file whose fragments are grouped
 * per tile has a page table after gid (16-aligned): 'CTP1', pages, data_lo (the index's size: everything before the
 * first page), f32 tile; then ColPage records. A page is one tile's fragment data, a byte range from data_lo on; a
 * fragment's data offset stays a file offset (in a page buffer at data - page off). col_load takes a whole file (every
 * page in it); col_load_index the index alone, and src/coltile.c hands pages in and out with col_page_set: queries
 * skip the fragments of absent pages, as they skip a block that is not loaded. */
typedef struct {
    short key[3];
    unsigned short nfrag;
    unsigned frag0, off, bytes;
    short lo[3], hi[3];
} ColPage;
_Static_assert(sizeof(ColPage) == 32, "ColPage");

#define MAX_PUSH_ITERS 4
#define COL_MOVE_MAX 20.0f  /* metres per call; larger moves are refused */

/* One collision.bin per world block (src/world.c), all in one world frame. The queries below take the block they run
 * on; the public functions loop over the loaded blocks and skip those whose bounds the query cannot touch. */
typedef struct {
    const Header *hdr;
    const short *verts;
    const Tri *tris;
    const unsigned *cells;
    const unsigned short *list;
    unsigned short *stamp, cur_stamp;
    float inv_cell;
    unsigned char *off;  /* EMEVD "Activate Hit" (src/event.c): bit per triangle, 1 = switched off; NULL = all on */
    float lo[3], hi[3];  /* bounds of the block's vertices (map space), for the per-block early-out */
    /* v2: h2 is the same blob as hdr (the fields up to gd are shared; verts .. list are v1 only) */
    const Header2 *h2;
    const Frag *frag;
    const unsigned *top;
    const unsigned short *toplist, *pat, *gid;
    unsigned short *fstamp, fcur;
    float inv_top, inv_fcell;
    unsigned ntris;
    /* collision tiles: page table (NULL: an unpaged file), per page its data (NULL: absent), per fragment its page */
    const ColPage *pg;
    unsigned npg, data_lo;
    float tile;
    const unsigned char **pbuf;
    unsigned short *fpg;
    unsigned *pgid;  /* CTP2: per page its first triangle slot and its v1 numbers' offset in the page (NULL: gid) */
} ColBlock;
static ColBlock blk[COL_BLOCKS];
static int ray1(ColBlock *c, const float from[3], const float to[3], unsigned mask, ColHit *hit);
static int floor2(const ColBlock *cb, float x, float z, float y_top, float y_bottom, unsigned mask, ColHit *hit);
static int ray2(ColBlock *cb, const float from[3], const float to[3], unsigned mask, ColHit *hit);

#define TRI_OFF(c, ti) ((c)->off && (c)->off[(ti) >> 3] >> ((ti) & 7) & 1)
/* fragment fi's data is in memory (always, in an unpaged block) */
#define FRAG_IN(c, fi) (!(c)->pbuf || (c)->pbuf[(c)->fpg[fi]])
int col_contacts;

/* 1 if the box [lo, hi] overlaps block c's bounds */
static inline int box_hits(const ColBlock *c, const float lo[3], const float hi[3])
{
    return c->hdr && lo[0] <= c->hi[0] && hi[0] >= c->lo[0] && lo[1] <= c->hi[1] && hi[1] >= c->lo[1] &&
           lo[2] <= c->hi[2] && hi[2] >= c->lo[2];
}

/* where a v2 file's page table may be: CTP2 after the patterns (v1 numbers in the pages), CTP1 after gid */
unsigned col_ptab_at(const void *head, int v)
{
    const Header2 *h2 = head;
    return v == 2 ? (h2->pat_off + 2 * h2->npat + 15) & ~15u : (h2->gid_off + 2 * h2->ntris + 15) & ~15u;
}

/* the page table of a v2 blob (NULL: none); *v = its version */
static const unsigned *ptab_of(const Header2 *h2, int *v)
{
    for (*v = 2; *v >= 1; --*v) {
        const unsigned *t = (const unsigned *)((const unsigned char *)h2 + col_ptab_at(h2, *v));
        if (!memcmp(t, *v == 2 ? "CTP2" : "CTP1", 4))
            return t;
    }
    return NULL;
}

unsigned col_index_bytes(const void *head, unsigned n)
{
    const Header2 *h2 = head;
    if (n < sizeof *h2 || memcmp(h2->magic, "DCOL", 4) || h2->version != 2)
        return 0;
    for (int v = 2; v >= 1; v--) {
        unsigned at = col_ptab_at(head, v);
        if (at + 16 > n)
            return at + 16;  /* read this much to see */
        const unsigned *t = (const unsigned *)((const unsigned char *)head + at);
        if (!memcmp(t, v == 2 ? "CTP2" : "CTP1", 4))
            return t[2];
    }
    return 0;
}

static int load_blob(int b, const void *blob, int whole);
int col_load(int b, const void *blob) { return load_blob(b, blob, 1); }
int col_load_index(int b, const void *blob) { return load_blob(b, blob, 0); }

static int load_blob(int b, const void *blob, int whole)
{
    if (b < 0 || b >= COL_BLOCKS)
        return 0;
    col_unload(b);
    const Header *h = blob;
    if (!h || memcmp(h->magic, "DCOL", 4) || (h->version != 1 && h->version != 2))
        return 0;
    const unsigned char *p = blob;
    ColBlock *c = &blk[b];
    c->stamp = calloc(h->ntris, sizeof *c->stamp);
    if (!c->stamp)
        return 0;
    c->ntris = h->ntris;
    if (h->version == 2) {
        const Header2 *h2 = blob;
        c->fstamp = calloc(h2->nfrag ? h2->nfrag : 1, sizeof *c->fstamp);
        if (!c->fstamp) {
            free(c->stamp);
            c->stamp = NULL;
            return 0;
        }
        c->h2 = h2;
        c->frag = (const Frag *)(p + h2->frag_off);
        c->top = (const unsigned *)(p + h2->top_off);
        c->toplist = (const unsigned short *)(p + h2->toplist_off);
        c->pat = (const unsigned short *)(p + h2->pat_off);
        c->gid = (const unsigned short *)(p + h2->gid_off);
        c->inv_top = 1.0f / h2->cell, c->inv_fcell = 1.0f / h2->fcell;
        c->cur_stamp = c->fcur = 0;
        int pv;
        const unsigned *pt = ptab_of(h2, &pv);
        if (pt) {
            c->npg = pt[1], c->data_lo = pt[2];
            memcpy(&c->tile, pt + 3, 4);
            c->pg = (const ColPage *)(pt + 4);
            c->pbuf = calloc(c->npg ? c->npg : 1, sizeof *c->pbuf);
            c->fpg = malloc((h2->nfrag ? h2->nfrag : 1) * sizeof *c->fpg);
            if (!c->pbuf || !c->fpg) {
                free(c->pbuf), free(c->fpg), free(c->stamp), free(c->fstamp);
                memset(c, 0, sizeof *c);
                return 0;
            }
            if (pv == 2) {  /* the v1 numbers are at each page's end: its slots' first number, its offset in the page */
                c->gid = NULL;
                c->pgid = malloc(2 * (c->npg ? c->npg : 1) * sizeof *c->pgid);
                if (!c->pgid) {
                    free(c->pbuf), free(c->fpg), free(c->stamp), free(c->fstamp);
                    memset(c, 0, sizeof *c);
                    return 0;
                }
            }
            for (unsigned i = 0; i < c->npg; i++) {
                for (unsigned f = c->pg[i].frag0; f < c->pg[i].frag0 + c->pg[i].nfrag && f < h2->nfrag; f++)
                    c->fpg[f] = i;
                if (c->pgid && c->pg[i].nfrag) {
                    const Frag *f0 = &c->frag[c->pg[i].frag0], *f1 = f0 + c->pg[i].nfrag - 1;
                    c->pgid[2 * i] = f0->tri0;
                    c->pgid[2 * i + 1] = c->pg[i].bytes - 2 * (f1->tri0 + f1->ntri - f0->tri0);
                }
                if (whole)
                    c->pbuf[i] = p + c->pg[i].off;
            }
        } else if (!whole) {  /* an unpaged file has no index of its own */
            free(c->stamp), free(c->fstamp);
            memset(c, 0, sizeof *c);
            return 0;
        }
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (unsigned i = 0; i < h2->nfrag; i++)
            for (int k = 0; k < 3; k++) {
                float a = h->origin[k] + c->frag[i].lo[k] * h->scale, z = h->origin[k] + c->frag[i].hi[k] * h->scale;
                lo[k] = a < lo[k] ? a : lo[k];
                hi[k] = z > hi[k] ? z : hi[k];
            }
        for (int k = 0; k < 3; k++)
            c->lo[k] = lo[k] - 0.01f, c->hi[k] = hi[k] + 0.01f;
        c->hdr = h;
        return 1;
    }
    c->verts = (const short *)(p + h->verts_off);
    c->tris = (const Tri *)(p + h->tris_off);
    c->cells = (const unsigned *)(p + h->cells_off);
    c->list = (const unsigned short *)(p + h->list_off);
    c->inv_cell = 1.0f / h->cell;
    c->cur_stamp = 0;
    /* vertex bounds: every triangle lies inside them, so a query box outside them cannot hit the block. An empty block
     * (no vertices) gets an inverted box that nothing overlaps */
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (unsigned i = 0; i < h->nverts; i++)
        for (int k = 0; k < 3; k++) {
            float v = h->origin[k] + c->verts[i * 3 + k] * h->scale;
            lo[k] = v < lo[k] ? v : lo[k];
            hi[k] = v > hi[k] ? v : hi[k];
        }
    for (int k = 0; k < 3; k++)  /* 1 cm margin: covers float rounding in the hit tests */
        c->lo[k] = lo[k] - 0.01f, c->hi[k] = hi[k] + 0.01f;
    c->hdr = h;
    return 1;
}

void col_unload(int b)
{
    if (b < 0 || b >= COL_BLOCKS)
        return;
    free(blk[b].stamp);
    free(blk[b].fstamp);
    free(blk[b].off);
    free(blk[b].pbuf);
    free(blk[b].fpg);
    free(blk[b].pgid);
    memset(&blk[b], 0, sizeof blk[b]);
}

int col_init(const void *blob)
{
    return col_load(0, blob);  /* collision.bin from the area pack (main.c): block 0 */
}

int col_tris_enable(int b, unsigned first, unsigned count, int on)
{
    if (b < 0 || b >= COL_BLOCKS || !blk[b].hdr || first + count > blk[b].ntris)
        return 0;
    ColBlock *c = &blk[b];
    if (!c->off && !on) {
        c->off = calloc((c->ntris + 7) >> 3, 1);
        if (!c->off)
            return 0;
    }
    if (c->off)
        for (unsigned t = first; t < first + count; t++)
            c->off[t >> 3] = on ? c->off[t >> 3] & ~(1 << (t & 7)) : c->off[t >> 3] | 1 << (t & 7);
    return 1;
}

int col_pages(int b, float *tile)
{
    if (b < 0 || b >= COL_BLOCKS || !blk[b].hdr || !blk[b].pg)
        return 0;
    if (tile)
        *tile = blk[b].tile;
    return (int)blk[b].npg;
}

int col_page(int b, int i, float lo[3], float hi[3], unsigned *off, unsigned *bytes)
{
    const ColBlock *c = &blk[b];
    if (b < 0 || b >= COL_BLOCKS || !c->pg || i < 0 || (unsigned)i >= c->npg)
        return 0;
    const ColPage *g = &c->pg[i];
    for (int k = 0; lo && k < 3; k++)
        lo[k] = c->hdr->origin[k] + g->lo[k] * c->hdr->scale, hi[k] = c->hdr->origin[k] + g->hi[k] * c->hdr->scale;
    if (off)
        *off = g->off;
    if (bytes)
        *bytes = g->bytes;
    return 1;
}

void col_page_set(int b, int i, const void *buf)
{
    if (b >= 0 && b < COL_BLOCKS && blk[b].pbuf && i >= 0 && (unsigned)i < blk[b].npg)
        blk[b].pbuf[i] = buf;
}

int col_page_in(int b, int i)
{
    return b >= 0 && b < COL_BLOCKS && blk[b].pbuf && i >= 0 && (unsigned)i < blk[b].npg && blk[b].pbuf[i];
}

int col_tris(void) { return blk[0].hdr ? (int)blk[0].hdr->ntris : 0; }
int col_block_tris(int b) { return b >= 0 && b < COL_BLOCKS && blk[b].hdr ? (int)blk[b].hdr->ntris : 0; }

/* New query id for the per-triangle "already tested" stamps (triangles span several cells) */
static void next_stamp(ColBlock *c)
{
    if (++c->cur_stamp == 0) {
        memset(c->stamp, 0, c->ntris * sizeof *c->stamp);
        c->cur_stamp = 1;
    }
}

static inline void vert(const ColBlock *c, unsigned i, float *o)
{
    const Header *h = c->hdr;
    const short *v = &c->verts[i * 3];
    o[0] = h->origin[0] + v[0] * h->scale;
    o[1] = h->origin[1] + v[1] * h->scale;
    o[2] = h->origin[2] + v[2] * h->scale;
}

static inline void tri_verts(const ColBlock *cb, const Tri *t, float *a, float *b, float *c)
{
    vert(cb, t->v[0], a);
    vert(cb, t->v[1], b);
    vert(cb, t->v[2], c);
}

static void tri_normal(const float *a, const float *b, const float *c, float *n)
{
    float e1[3], e2[3];
    sub3(b, a, e1);
    sub3(c, a, e2);
    cross3(e1, e2, n);
    float l = sqrtf(dot3(n, n));
    if (l > 0)
        n[0] /= l, n[1] /= l, n[2] /= l;
}

static inline int cell_index(const ColBlock *c, float x, float z, int *ix, int *iz)
{
    const Header *h = c->hdr;
    *ix = (int)floorf((x - h->x0) * c->inv_cell);
    *iz = (int)floorf((z - h->z0) * c->inv_cell);
    return *ix >= 0 && *iz >= 0 && *ix < (int)h->gw && *iz < (int)h->gd;
}

static int floor1(const ColBlock *cb, float x, float z, float y_top, float y_bottom, unsigned mask, ColHit *hit)
{
    if (cb->h2)
        return floor2(cb, x, z, y_top, y_bottom, mask, hit);
    int ix, iz;
    if (!cb->hdr || !cell_index(cb, x, z, &ix, &iz))
        return 0;
    const unsigned *cells = cb->cells;
    const unsigned short *list = cb->list;
    const Tri *tris = cb->tris;
    unsigned k = iz * cb->hdr->gw + ix;
    int found = -1;
    float best = 0;
    for (unsigned j = cells[k]; j < cells[k + 1]; j++) {
        const Tri *t = &tris[list[j]];
        if (!(mask >> t->hit_filter & 1) || TRI_OFF(cb, list[j]))
            continue;
        float a[3], b[3], c[3];
        tri_verts(cb, t, a, b, c);
        /* barycentric test in the x/z plane */
        float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
        if (d > -1e-6f && d < 1e-6f)
            continue;
        float l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
        float l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
        float l3 = 1 - l1 - l2;
        if (l1 < 0 || l2 < 0 || l3 < 0)
            continue;
        float h = l1 * a[1] + l2 * b[1] + l3 * c[1];
        if (h > y_top || h < y_bottom || (found >= 0 && h <= best))
            continue;
        best = h, found = list[j];
    }
    if (found < 0)
        return 0;
    if (hit) {
        const Tri *t = &tris[found];
        float a[3], b[3], c[3];
        tri_verts(cb, t, a, b, c);
        tri_normal(a, b, c, hit->n);
        if (hit->n[1] < 0)
            hit->n[0] = -hit->n[0], hit->n[1] = -hit->n[1], hit->n[2] = -hit->n[2];
        hit->y = best, hit->t = 0, hit->tri = found, hit->block = (int)(cb - blk);
        hit->hit_filter = t->hit_filter, hit->material = t->material;
    }
    return 1;
}

/* ---- closest points (Ericson, Real-Time Collision Detection 5.1.5 / 5.1.9) */

static void closest_pt_tri(const float *p, const float *a, const float *b, const float *c, float *o)
{
    float ab[3], ac[3], ap[3], bp[3], cp[3];
    sub3(b, a, ab);
    sub3(c, a, ac);
    sub3(p, a, ap);
    float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
    if (d1 <= 0 && d2 <= 0) {
        memcpy(o, a, 12);
        return;
    }
    sub3(p, b, bp);
    float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
    if (d3 >= 0 && d4 <= d3) {
        memcpy(o, b, 12);
        return;
    }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        lerp3(a, b, d1 / (d1 - d3), o);
        return;
    }
    sub3(p, c, cp);
    float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
    if (d6 >= 0 && d5 <= d6) {
        memcpy(o, c, 12);
        return;
    }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        lerp3(a, c, d2 / (d2 - d6), o);
        return;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
        lerp3(b, c, (d4 - d3) / ((d4 - d3) + (d5 - d6)), o);
        return;
    }
    float den = 1 / (va + vb + vc), v = vb * den, w = vc * den;
    for (int i = 0; i < 3; i++)
        o[i] = a[i] + ab[i] * v + ac[i] * w;
}

/* Closest points between segments p1-q1 and p2-q2 -> c1, c2; returns squared distance */
static float closest_seg_seg(const float *p1, const float *q1, const float *p2, const float *q2, float *c1, float *c2)
{
    float d1[3], d2[3], r[3], s, t;
    sub3(q1, p1, d1);
    sub3(q2, p2, d2);
    sub3(p1, p2, r);
    float a = dot3(d1, d1), e = dot3(d2, d2), f = dot3(d2, r);
    if (a <= 1e-9f && e <= 1e-9f) {
        s = t = 0;
    } else if (a <= 1e-9f) {
        s = 0, t = clamp01(f / e);
    } else {
        float c = dot3(d1, r);
        if (e <= 1e-9f) {
            t = 0, s = clamp01(-c / a);
        } else {
            float b = dot3(d1, d2), den = a * e - b * b;
            s = den != 0 ? clamp01((b * f - c * e) / den) : 0;
            t = (b * s + f) / e;
            if (t < 0)
                t = 0, s = clamp01(-c / a);
            else if (t > 1)
                t = 1, s = clamp01((b - c) / a);
        }
    }
    lerp3(p1, q1, s, c1);
    lerp3(p2, q2, t, c2);
    float d[3];
    sub3(c1, c2, d);
    return dot3(d, d);
}

/* Segment p-q against triangle: 1 if they intersect (Moller-Trumbore, two-sided), *t = fraction along p-q */
static int seg_tri(const float *p, const float *q, const float *a, const float *b, const float *c, float *t)
{
    float e1[3], e2[3], d[3], h[3], s[3], qv[3];
    sub3(b, a, e1);
    sub3(c, a, e2);
    sub3(q, p, d);
    cross3(d, e2, h);
    float det = dot3(e1, h);
    if (det > -1e-9f && det < 1e-9f)
        return 0;
    float inv = 1 / det;
    sub3(p, a, s);
    float u = dot3(s, h) * inv;
    if (u < 0 || u > 1)
        return 0;
    cross3(s, e1, qv);
    float v = dot3(d, qv) * inv;
    if (v < 0 || u + v > 1)
        return 0;
    float tt = dot3(e2, qv) * inv;
    if (tt < 0 || tt > 1)
        return 0;
    *t = tt;
    return 1;
}

/* Closest points between segment p-q and triangle abc: cs on the segment, ct on the triangle; squared distance */
static float closest_seg_tri(const float *p, const float *q, const float *a, const float *b, const float *c, float *cs,
                             float *ct)
{
    float t, best, tmp_s[3], tmp_t[3], d[3];
    if (seg_tri(p, q, a, b, c, &t)) {
        lerp3(p, q, t, cs);
        memcpy(ct, cs, 12);
        return 0;
    }
    closest_pt_tri(p, a, b, c, ct);
    memcpy(cs, p, 12);
    sub3(cs, ct, d);
    best = dot3(d, d);
    closest_pt_tri(q, a, b, c, tmp_t);
    sub3(q, tmp_t, d);
    if (dot3(d, d) < best) {
        best = dot3(d, d);
        memcpy(cs, q, 12);
        memcpy(ct, tmp_t, 12);
    }
    const float *e[4] = {a, b, c, a};
    for (int i = 0; i < 3; i++) {
        float d2 = closest_seg_seg(p, q, e[i], e[i + 1], tmp_s, tmp_t);
        if (d2 < best) {
            best = d2;
            memcpy(cs, tmp_s, 12);
            memcpy(ct, tmp_t, 12);
        }
    }
    return best;
}

/* Capsule axis heights (push1): p0y .. p1y, radius r around it */
static inline void capsule_axis(const float pos[3], float r, float height, float step, float *p0y, float *p1y)
{
    *p0y = pos[1] + step + r, *p1y = pos[1] + height - r;
    if (*p1y < *p0y)
        *p0y = *p1y = (*p0y + *p1y) * 0.5f;
}

/* One triangle against the capsule axis p0-p1 (radius r): walls only (floors / flat ceilings do not push sideways);
 * moves pos (and p0, p1, the box lo / hi in x / z) out horizontally. 1 when it moved */
static int push_tri(float pos[3], float *p0, float *p1, float *lo, float *hi, float r, const float *a, const float *b,
                    const float *c)
{
    int out = 0;
    for (int i = 0; i < 3 && !out; i++)
        out = (a[i] < lo[i] && b[i] < lo[i] && c[i] < lo[i]) || (a[i] > hi[i] && b[i] > hi[i] && c[i] > hi[i]);
    if (out)
        return 0;
    float n[3];
    tri_normal(a, b, c, n);
    if (n[1] >= COL_WALKABLE_NY || -n[1] >= COL_WALKABLE_NY)
        return 0; /* floors (col_floor) and flat ceilings do not push sideways */
    float cs[3], ct[3];
    float d2 = closest_seg_tri(p0, p1, a, b, c, cs, ct);
    if (d2 >= r * r)
        return 0;
    float dx = cs[0] - ct[0], dz = cs[2] - ct[2], dist = sqrtf(d2), hl = sqrtf(dx * dx + dz * dz);
    if (hl < 1e-4f) {
        /* segment touches/crosses the triangle: push along the horizontal normal, towards the side the capsule axis
         * is on */
        float side[3];
        sub3(p0, a, side);
        float sgn = dot3(side, n) >= 0 ? 1 : -1;
        dx = n[0] * sgn, dz = n[2] * sgn;
        hl = sqrtf(dx * dx + dz * dz);
        if (hl < 1e-4f)
            return 0;
        dist = 0;
    }
    /* horizontal move that brings the separation back to r (the vertical part stays) */
    float dy = dist > 0 ? (cs[1] - ct[1]) : 0, need = sqrtf(r * r - dy * dy) - hl * (dist > 0);
    if (need <= 0)
        return 0;
    pos[0] += dx / hl * (need + 1e-3f);
    pos[2] += dz / hl * (need + 1e-3f);
    col_contacts++;
    p0[0] = p1[0] = pos[0];
    p0[2] = p1[2] = pos[2];
    lo[0] = pos[0] - r, hi[0] = pos[0] + r, lo[2] = pos[2] - r, hi[2] = pos[2] + r;
    return 1;
}

static int push2(ColBlock *cb, float pos[3], float *p0, float *p1, float *lo, float *hi, float r, unsigned mask,
                 int *contacts);
static int touch2(ColBlock *cb, const float pos[3], const float *p0, const float *p1, const float *lo, const float *hi,
                  float r, unsigned mask);

static int push1(ColBlock *cb, float pos[3], float r, float height, float step, unsigned mask)
{
    const Header *hdr = cb->hdr;
    if (!hdr)
        return 0;
    int contacts = 0;
    for (int iter = 0; iter < MAX_PUSH_ITERS; iter++) {
        float p0[3] = {pos[0], 0, pos[2]}, p1[3] = {pos[0], 0, pos[2]};
        capsule_axis(pos, r, height, step, &p0[1], &p1[1]);
        float lo[3] = {pos[0] - r, p0[1] - r, pos[2] - r}, hi[3] = {pos[0] + r, p1[1] + r, pos[2] + r};
        int moved = 0;
        if (cb->h2) {
            /* free standing (no triangle touches the capsule where it is: the order cannot matter) is the common
             * case: one cheap pass in fragment order finds it; only a contact pays for v1's order (push2) */
            if (touch2(cb, pos, p0, p1, lo, hi, r, mask))
                moved = push2(cb, pos, p0, p1, lo, hi, r, mask, &contacts);
        } else {
            const unsigned *cells = cb->cells;
            const unsigned short *list = cb->list;
            const Tri *tris = cb->tris;
            unsigned short *stamp = cb->stamp;
            int ix0, iz0, ix1, iz1;
            cell_index(cb, pos[0] - r, pos[2] - r, &ix0, &iz0);
            cell_index(cb, pos[0] + r, pos[2] + r, &ix1, &iz1);
            if (ix0 < 0) ix0 = 0;
            if (iz0 < 0) iz0 = 0;
            if (ix1 >= (int)hdr->gw) ix1 = hdr->gw - 1;
            if (iz1 >= (int)hdr->gd) iz1 = hdr->gd - 1;
            next_stamp(cb);
            unsigned short cur_stamp = cb->cur_stamp;
            for (int iz = iz0; iz <= iz1; iz++)
                for (int ix = ix0; ix <= ix1; ix++) {
                    unsigned k = iz * hdr->gw + ix;
                    for (unsigned j = cells[k]; j < cells[k + 1]; j++) {
                        unsigned ti = list[j];
                        if (stamp[ti] == cur_stamp)
                            continue;
                        stamp[ti] = cur_stamp;
                        const Tri *t = &tris[ti];
                        if (!(mask >> t->hit_filter & 1) || TRI_OFF(cb, ti))
                            continue;
                        float a[3], b[3], c[3];
                        tri_verts(cb, t, a, b, c);
                        if (push_tri(pos, p0, p1, lo, hi, r, a, b, c))
                            contacts++, moved = 1;
                    }
                }
        }
        if (!moved)
            break;
    }
    return contacts;
}

/* ---- DCOL v2 (fragments) ---- */
/* the v1 number of fragment f's triangle lt (FRAG_IN) */
static inline unsigned gid_of(const ColBlock *c, const Frag *f, unsigned lt)
{
    if (!c->pgid)
        return c->gid[f->tri0 + lt];
    unsigned i = c->fpg[f - c->frag];
    return ((const unsigned short *)(c->pbuf[i] + c->pgid[2 * i + 1]))[f->tri0 + lt - c->pgid[2 * i]];
}
/* fragment f's data (FRAG_IN: in memory) */
static inline const unsigned char *fbase(const ColBlock *c, const Frag *f)
{
    if (!c->pbuf)
        return (const unsigned char *)c->hdr + f->data;
    const ColPage *g = &c->pg[c->fpg[f - c->frag]];
    return c->pbuf[g - c->pg] + (f->data - g->off);
}
static inline void vert2(const ColBlock *c, const Frag *f, unsigned i, float *o)
{
    const Header *h = c->hdr;
    const unsigned short *v = (const unsigned short *)fbase(c, f) + i * 3;
    o[0] = h->origin[0] + (f->lo[0] + v[0]) * h->scale;
    o[1] = h->origin[1] + (f->lo[1] + v[1]) * h->scale;
    o[2] = h->origin[2] + (f->lo[2] + v[2]) * h->scale;
}
/* fragment f's triangles (4 bytes each, after its 4-aligned vertices), cell starts and lists */
static inline const unsigned char *ftris(const ColBlock *c, const Frag *f)
{
    return fbase(c, f) + ((f->nvert * 6u + 3) & ~3u);
}
static inline const unsigned short *fcells(const ColBlock *c, const Frag *f) { return (const unsigned short *)(ftris(c, f) + f->ntri * 4u); }
static inline const unsigned char *flist(const ColBlock *c, const Frag *f) { return (const unsigned char *)(fcells(c, f) + f->gw * f->gd + 1); }
static inline void tri2(const ColBlock *c, const Frag *f, const unsigned char *t, float *a, float *b, float *cc)
{
    vert2(c, f, t[0], a);
    vert2(c, f, t[1], b);
    vert2(c, f, t[2], cc);
}
/* the fragment grid's origin (lo - 0.02 m, export_collision.write_v2) */
static inline void forigin(const ColBlock *c, const Frag *f, float *x0, float *z0)
{
    const Header *h = c->hdr;
    *x0 = h->origin[0] + f->lo[0] * h->scale - 0.02f;
    *z0 = h->origin[2] + f->lo[2] * h->scale - 0.02f;
}
static inline int fbox_hits(const ColBlock *c, const Frag *f, const float lo[3], const float hi[3])
{
    const Header *h = c->hdr;
    for (int k = 0; k < 3; k++)
        if (lo[k] > h->origin[k] + f->hi[k] * h->scale + 0.02f || hi[k] < h->origin[k] + f->lo[k] * h->scale - 0.02f)
            return 0;
    return 1;
}
static void next_fstamp(ColBlock *c)
{
    if (++c->fcur == 0) {
        memset(c->fstamp, 0, c->h2->nfrag * sizeof *c->fstamp);
        c->fcur = 1;
    }
}

/* Calls fn(c, f, triangle bytes, gid, user) for every triangle of the fragments overlapping box [lo, hi] whose cells
 * overlap it, each triangle once per call (stamp by gid); fn returns nonzero to stop */
typedef int (*TriFn)(ColBlock *c, const Frag *f, const unsigned char *t, unsigned gid, void *u);
static int each_tri2(ColBlock *c, const float lo[3], const float hi[3], unsigned mask, TriFn fn, void *u)
{
    const Header2 *h2 = c->h2;
    int tx0 = (int)floorf((lo[0] - h2->x0) * c->inv_top), tz0 = (int)floorf((lo[2] - h2->z0) * c->inv_top);
    int tx1 = (int)floorf((hi[0] - h2->x0) * c->inv_top), tz1 = (int)floorf((hi[2] - h2->z0) * c->inv_top);
    tx0 = tx0 < 0 ? 0 : tx0, tz0 = tz0 < 0 ? 0 : tz0;
    tx1 = tx1 >= (int)h2->gw ? (int)h2->gw - 1 : tx1, tz1 = tz1 >= (int)h2->gd ? (int)h2->gd - 1 : tz1;
    if (tx0 > tx1 || tz0 > tz1)
        return 0;
    next_stamp(c);
    next_fstamp(c);
    const unsigned short cur = c->cur_stamp, fcur = c->fcur;
    for (int tz = tz0; tz <= tz1; tz++)
        for (int tx = tx0; tx <= tx1; tx++) {
            unsigned k = tz * h2->gw + tx;
            for (unsigned j = c->top[k]; j < c->top[k + 1]; j++) {
                unsigned fi = c->toplist[j];
                if (c->fstamp[fi] == fcur)
                    continue;
                c->fstamp[fi] = fcur;
                const Frag *f = &c->frag[fi];
                if (!FRAG_IN(c, fi) || !fbox_hits(c, f, lo, hi))
                    continue;
                float fx0, fz0;
                forigin(c, f, &fx0, &fz0);
                int ix0 = (int)floorf((lo[0] - fx0) * c->inv_fcell), iz0 = (int)floorf((lo[2] - fz0) * c->inv_fcell);
                int ix1 = (int)floorf((hi[0] - fx0) * c->inv_fcell), iz1 = (int)floorf((hi[2] - fz0) * c->inv_fcell);
                ix0 = ix0 < 0 ? 0 : ix0, iz0 = iz0 < 0 ? 0 : iz0;
                ix1 = ix1 >= f->gw ? f->gw - 1 : ix1, iz1 = iz1 >= f->gd ? f->gd - 1 : iz1;
                const unsigned char *tr = ftris(c, f), *ls = flist(c, f);
                const unsigned short *cs = fcells(c, f);
                for (int iz = iz0; iz <= iz1; iz++)
                    for (int ix = ix0; ix <= ix1; ix++) {
                        unsigned ck = iz * f->gw + ix;
                        for (unsigned q = cs[ck]; q < cs[ck + 1]; q++) {
                            unsigned lt = ls[q], g = gid_of(c, f, lt);
                            if (c->stamp[g] == cur)
                                continue;
                            c->stamp[g] = cur;
                            const unsigned char *t = tr + lt * 4;
                            if (!(mask >> (c->pat[t[3]] & 0xff) & 1) || TRI_OFF(c, g))
                                continue;
                            if (fn(c, f, t, g, u))
                                return 1;
                        }
                    }
            }
        }
    return 0;
}

/* col_floor on a v2 block: the fragments of the point's top cell, the point's cell in each */
static int floor2(const ColBlock *cb, float x, float z, float y_top, float y_bottom, unsigned mask, ColHit *hit)
{
    const Header2 *h2 = cb->h2;
    int tx = (int)floorf((x - h2->x0) * cb->inv_top), tz = (int)floorf((z - h2->z0) * cb->inv_top);
    if (tx < 0 || tz < 0 || tx >= (int)h2->gw || tz >= (int)h2->gd)
        return 0;
    unsigned k = tz * h2->gw + tx;
    int found = -1;
    float best = 0;
    const Frag *bf = NULL;
    const unsigned char *bt = NULL;
    for (unsigned j = cb->top[k]; j < cb->top[k + 1]; j++) {
        if (!FRAG_IN(cb, cb->toplist[j]))
            continue;
        const Frag *f = &cb->frag[cb->toplist[j]];
        float fx0, fz0;
        forigin(cb, f, &fx0, &fz0);
        int ix = (int)floorf((x - fx0) * cb->inv_fcell), iz = (int)floorf((z - fz0) * cb->inv_fcell);
        if (ix < 0 || iz < 0 || ix >= f->gw || iz >= f->gd)
            continue;
        const unsigned char *tr = ftris(cb, f), *ls = flist(cb, f);
        const unsigned short *cs = fcells(cb, f);
        unsigned ck = iz * f->gw + ix;
        for (unsigned q = cs[ck]; q < cs[ck + 1]; q++) {
            const unsigned char *t = tr + ls[q] * 4;
            unsigned g = gid_of(cb, f, ls[q]);
            if (!(mask >> (cb->pat[t[3]] & 0xff) & 1) || TRI_OFF(cb, g))
                continue;
            float a[3], b[3], c[3];
            tri2(cb, f, t, a, b, c);
            float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
            if (d > -1e-6f && d < 1e-6f)
                continue;
            float l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
            float l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
            float l3 = 1 - l1 - l2;
            if (l1 < 0 || l2 < 0 || l3 < 0)
                continue;
            float h = l1 * a[1] + l2 * b[1] + l3 * c[1];
            /* ties: the lower v1 number wins, as v1's cell order (ascending triangle numbers) did */
            if (h > y_top || h < y_bottom || (found >= 0 && (h < best || (h == best && (int)g > found))))
                continue;
            best = h, found = (int)g, bf = f, bt = t;
        }
    }
    if (found < 0)
        return 0;
    if (hit) {
        float a[3], b[3], c[3];
        tri2(cb, bf, bt, a, b, c);
        tri_normal(a, b, c, hit->n);
        if (hit->n[1] < 0)
            hit->n[0] = -hit->n[0], hit->n[1] = -hit->n[1], hit->n[2] = -hit->n[2];
        hit->y = best, hit->t = 0, hit->tri = found, hit->block = (int)(cb - blk);
        hit->hit_filter = cb->pat[bt[3]] & 0xff, hit->material = cb->pat[bt[3]] >> 8;
    }
    return 1;
}

/* push1 on a v2 block. The pushes move the capsule one triangle after the other, so their order decides where it ends
 * in a corner (a v2 walker wedged in the Taurus tower's ground floor doorway that v1 slid through, ISO 2026-11-08):
 * the triangles go in v1's order. v1 visits the 2 m cells the start box covers (rows of z, then x) and in each cell
 * its triangles by number, each once; v2 gathers the triangles of the fragments over those cells' rectangle and sorts
 * them by (first such cell whose rectangle they overlap, as export_collision.build tests it, then v1 number). */
#define PUSH2_MAX 512
#ifndef PUSH2_CELLS
#define PUSH2_CELLS 1
#endif
typedef struct { unsigned key; float a[3], b[3], c[3]; } PushCand;
typedef struct { PushCand *c; int n; int ix0, iz0, nx, nz; float x0, z0, cell, blo[3], bhi[3]; } PushGather;
#define PUSH2_REACH 1.2f  /* m: three contacts of the player capsule (r 0.4) */
/* export_collision._tri_overlaps_rect, in float: the EE has no double unit (doubles here took the logic of a
 * frame to 0.8-1.4 s on the ISO); a triangle exactly on a cell edge may land one cell over, a rare order swap */
static int tri_rect(const float p[3][2], float x0, float z0, float x1, float z1)
{
    float mnx = p[0][0], mxx = p[0][0], mnz = p[0][1], mxz = p[0][1];
    for (int i = 1; i < 3; i++) {
        mnx = p[i][0] < mnx ? p[i][0] : mnx, mxx = p[i][0] > mxx ? p[i][0] : mxx;
        mnz = p[i][1] < mnz ? p[i][1] : mnz, mxz = p[i][1] > mxz ? p[i][1] : mxz;
    }
    if (mxx < x0 || mnx > x1 || mxz < z0 || mnz > z1)
        return 0;
    const float rc[4][2] = {{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}};
    for (int i = 0; i < 3; i++) {
        const float *a = p[i], *b = p[(i + 1) % 3], *o = p[(i + 2) % 3];
        float nx = b[1] - a[1], nz = a[0] - b[0], side = nx * (o[0] - a[0]) + nz * (o[1] - a[1]);
        if (side == 0)
            continue;
        int all = 1;
        for (int k = 0; k < 4 && all; k++)
            all = (nx * (rc[k][0] - a[0]) + nz * (rc[k][1] - a[1])) * side < 0;
        if (all)
            return 0;
    }
    return 1;
}
static int gather_cb(ColBlock *cb, const Frag *f, const unsigned char *t, unsigned g, void *u)
{
    PushGather *q = u;
    if (q->n >= PUSH2_MAX)
        return 1;
    PushCand *c = &q->c[q->n];
    tri2(cb, f, t, c->a, c->b, c->c);
    const float *v[3] = {c->a, c->b, c->c};
    /* a triangle PUSH2_REACH beyond the start box in x / z (or outside it in y) never pushes in this pass (push_tri
     * tests the box the pushes moved: ~r per contact): dropped before the cell test, the rest keeps its order */
    for (int k = 0; k < 3; k++)
        if ((v[0][k] < q->blo[k] && v[1][k] < q->blo[k] && v[2][k] < q->blo[k]) ||
            (v[0][k] > q->bhi[k] && v[1][k] > q->bhi[k] && v[2][k] > q->bhi[k]))
            return 0;
    if (!PUSH2_CELLS || (q->nx == 1 && q->nz == 1)) {  /* by number (one cell, most pushes: v1's order exactly) */
        c->key = g, q->n++;
        return 0;
    }
    const float p[3][2] = {{c->a[0], c->a[2]}, {c->b[0], c->b[2]}, {c->c[0], c->c[2]}};
    const float eps = 0.02f;
    for (int iz = 0; iz < q->nz; iz++)
        for (int ix = 0; ix < q->nx; ix++) {
            float cx = q->x0 + (q->ix0 + ix) * q->cell, cz = q->z0 + (q->iz0 + iz) * q->cell;
            if (tri_rect(p, cx - eps, cz - eps, cx + q->cell + eps, cz + q->cell + eps)) {
                c->key = (unsigned)(iz * q->nx + ix) << 16 | g, q->n++;
                return 0;
            }
        }
    return 0;
}
/* any triangle of a v2 block pushes the capsule as it stands (push_tri on a copy; stops at the first) */
typedef struct { float pos[3], p0[3], p1[3], lo[3], hi[3], r; } TouchQ;
static int touch_cb(ColBlock *cb, const Frag *f, const unsigned char *t, unsigned g, void *u)
{
    (void)g;
    TouchQ *q = u;
    float a[3], b[3], c[3];
    tri2(cb, f, t, a, b, c);
    TouchQ k = *q;  /* push_tri moves its arguments */
    return push_tri(k.pos, k.p0, k.p1, k.lo, k.hi, k.r, a, b, c);
}
static int touch2(ColBlock *cb, const float pos[3], const float *p0, const float *p1, const float *lo, const float *hi,
                  float r, unsigned mask)
{
    TouchQ q;
    memcpy(q.pos, pos, 12), memcpy(q.p0, p0, 12), memcpy(q.p1, p1, 12), memcpy(q.lo, lo, 12), memcpy(q.hi, hi, 12);
    q.r = r;
    int c0 = col_contacts, hit = each_tri2(cb, lo, hi, mask, touch_cb, &q);
    col_contacts = c0;  /* (push_tri counts the contact it found) */
    return hit;
}

static int push2(ColBlock *cb, float pos[3], float *p0, float *p1, float *lo, float *hi, float r, unsigned mask,
                 int *contacts)
{
    static PushCand cand[PUSH2_MAX];
    const Header2 *h2 = cb->h2;
    PushGather q = {cand, 0, 0, 0, 0, 0, h2->x0, h2->z0, h2->fcell,
                    {lo[0] - PUSH2_REACH, lo[1], lo[2] - PUSH2_REACH}, {hi[0] + PUSH2_REACH, hi[1], hi[2] + PUSH2_REACH}};
    q.ix0 = (int)floorf((lo[0] - h2->x0) * cb->inv_fcell), q.iz0 = (int)floorf((lo[2] - h2->z0) * cb->inv_fcell);
    int ix1 = (int)floorf((hi[0] - h2->x0) * cb->inv_fcell), iz1 = (int)floorf((hi[2] - h2->z0) * cb->inv_fcell);
    q.ix0 = q.ix0 < 0 ? 0 : q.ix0, q.iz0 = q.iz0 < 0 ? 0 : q.iz0;
    if (ix1 < q.ix0 || iz1 < q.iz0)
        return 0;
    q.nx = ix1 - q.ix0 + 1, q.nz = iz1 - q.iz0 + 1;
    /* the cells' rectangle (+ the export's 0.02 m) as the gather box; y as push1's box (a triangle outside it in y
     * never pushes) */
    /* only triangles within PUSH2_REACH can push: the cells' rectangle cut to that box */
    float glo[3] = {h2->x0 + q.ix0 * h2->fcell - 0.03f, lo[1], h2->z0 + q.iz0 * h2->fcell - 0.03f};
    float ghi[3] = {h2->x0 + (ix1 + 1) * h2->fcell + 0.03f, hi[1], h2->z0 + (iz1 + 1) * h2->fcell + 0.03f};
    for (int k = 0; k < 3; k += 2)
        glo[k] = glo[k] > q.blo[k] ? glo[k] : q.blo[k], ghi[k] = ghi[k] < q.bhi[k] ? ghi[k] : q.bhi[k];
    each_tri2(cb, glo, ghi, mask, gather_cb, &q);
    for (int i = 1; i < q.n; i++) {  /* insertion sort: a few dozen, mostly in order already */
        PushCand x = cand[i];
        int j = i;
        while (j > 0 && cand[j - 1].key > x.key)
            cand[j] = cand[j - 1], j--;
        cand[j] = x;
    }
    int moved = 0;
    for (int i = 0; i < q.n; i++) {
        if (push_tri(pos, p0, p1, lo, hi, r, cand[i].a, cand[i].b, cand[i].c))
            moved = 1, (*contacts)++;
    }
    return moved;
}

typedef struct { const float *from, *to; float best; int tri; const Frag *f; const unsigned char *t; } RayQ;
static int ray_tri_cb(ColBlock *cb, const Frag *f, const unsigned char *t, unsigned g, void *u)
{
    RayQ *q = u;
    float a[3], b[3], c[3], tt;
    tri2(cb, f, t, a, b, c);
    if (seg_tri(q->from, q->to, a, b, c, &tt) && (tt < q->best || (tt == q->best && (int)g < q->tri)))
        q->best = tt, q->tri = (int)g, q->f = f, q->t = t;
    return 0;
}
static int ray2(ColBlock *cb, const float from[3], const float to[3], unsigned mask, ColHit *hit)
{
    float lo[3], hi[3], d[3];
    for (int k = 0; k < 3; k++)
        lo[k] = from[k] < to[k] ? from[k] : to[k], hi[k] = from[k] < to[k] ? to[k] : from[k];
    RayQ q = {from, to, 2, -1, NULL, NULL};
    each_tri2(cb, lo, hi, mask, ray_tri_cb, &q);
    if (q.tri < 0)
        return 0;
    if (hit) {
        float a[3], b[3], c[3];
        sub3(to, from, d);
        tri2(cb, q.f, q.t, a, b, c);
        tri_normal(a, b, c, hit->n);
        if (dot3(hit->n, d) > 0)
            hit->n[0] = -hit->n[0], hit->n[1] = -hit->n[1], hit->n[2] = -hit->n[2];
        hit->t = q.best, hit->y = from[1] + d[1] * q.best, hit->tri = q.tri, hit->block = (int)(cb - blk);
        hit->hit_filter = cb->pat[q.t[3]] & 0xff, hit->material = cb->pat[q.t[3]] >> 8;
    }
    return 1;
}

int col_floor(float x, float z, float y_top, float y_bottom, unsigned mask, ColHit *hit)
{
    ColHit h, best;
    int found = 0;
    const float lo[3] = {x, y_bottom, z}, hi[3] = {x, y_top, z};
    for (int b = 0; b < COL_BLOCKS; b++)
        if (box_hits(&blk[b], lo, hi) && floor1(&blk[b], x, z, y_top, y_bottom, mask, &h) && (!found || h.y > best.y))
            best = h, found = 1;
    if (found && hit)
        *hit = best;
    return found;
}

int col_floor_block(int b, float x, float z, float y_top, float y_bottom, unsigned mask, ColHit *hit)
{
    ColHit h;
    if (b < 0 || b >= COL_BLOCKS || !floor1(&blk[b], x, z, y_top, y_bottom, mask, &h))
        return 0;
    if (hit)
        *hit = h;
    return 1;
}

int col_push_capsule(float pos[3], float r, float height, float step, unsigned mask)
{
    int contacts = 0;
    for (int b = 0; b < COL_BLOCKS; b++) {
        /* the capsule's box as push1 tests it (its axis only moves in x / z, and only while it touches this block) */
        float lo[3] = {pos[0] - r, 0, pos[2] - r}, hi[3] = {pos[0] + r, 0, pos[2] + r};
        capsule_axis(pos, r, height, step, &lo[1], &hi[1]);
        lo[1] -= r, hi[1] += r;
        if (box_hits(&blk[b], lo, hi))
            contacts += push1(&blk[b], pos, r, height, step, mask);
    }
    return contacts;
}

int col_ray(const float from[3], const float to[3], unsigned mask, ColHit *hit)
{
    ColHit h, best;
    int found = 0;
    float lo[3], hi[3];  /* the segment's box */
    for (int k = 0; k < 3; k++)
        lo[k] = from[k] < to[k] ? from[k] : to[k], hi[k] = from[k] < to[k] ? to[k] : from[k];
    for (int b = 0; b < COL_BLOCKS; b++)
        if (box_hits(&blk[b], lo, hi) && ray1(&blk[b], from, to, mask, &h) && (!found || h.t < best.t))
            best = h, found = 1;
    if (found && hit)
        *hit = best;
    return found;
}

int col_move(float pos[3], float dx, float dz, float r, float height, float step, unsigned mask)
{
    float len = sqrtf(dx * dx + dz * dz);
    if (!(len < COL_MOVE_MAX))  /* also NaN: a saturated move would otherwise loop ~2^31 substeps on the EE */
        return 0;
    int n = (int)(len / (r * 0.5f)) + 1, touched = 0;
    for (int i = 0; i < n; i++) {
        pos[0] += dx / n;
        pos[2] += dz / n;
        touched |= col_push_capsule(pos, r, height, step, mask) > 0;
    }
    return touched;
}

int col_walk(float pos[3], float dx, float dz, float r, float height, float step_up, float step_down, unsigned mask,
             ColHit *hit)
{
    ColHit h;
    col_move(pos, dx, dz, r, height, step_up, mask);
    if (!col_floor(pos[0], pos[2], pos[1] + step_up, pos[1] - step_down, mask, &h))
        return 0;
    pos[1] = h.y;
    if (hit)
        *hit = h;
    return 1;
}

static int ray1(ColBlock *cb, const float from[3], const float to[3], unsigned mask, ColHit *hit)
{
    const Header *hdr = cb->hdr;
    if (!hdr)
        return 0;
    if (cb->h2)
        return ray2(cb, from, to, mask, hit);
    const unsigned *cells = cb->cells;
    const unsigned short *list = cb->list;
    const Tri *tris = cb->tris;
    unsigned short *stamp = cb->stamp;
    const float inv_cell = cb->inv_cell;
    float d[3];
    sub3(to, from, d);
    /* 2D DDA over the x/z grid (Amanatides-Woo), clipped to the grid */
    float gx = (from[0] - hdr->x0) * inv_cell, gz = (from[2] - hdr->z0) * inv_cell;
    float ddx = d[0] * inv_cell, ddz = d[2] * inv_cell;
    float t0 = 0, t1 = 1;
    /* clip t to the grid rectangle */
    const float lim[2] = {(float)hdr->gw, (float)hdr->gd}, org[2] = {gx, gz}, dir[2] = {ddx, ddz};
    for (int i = 0; i < 2; i++) {
        if (dir[i] > -1e-9f && dir[i] < 1e-9f) {
            if (org[i] < 0 || org[i] >= lim[i])
                return 0;
            continue;
        }
        float ta = (0 - org[i]) / dir[i], tb = (lim[i] - org[i]) / dir[i];
        if (ta > tb) { float s = ta; ta = tb; tb = s; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
    }
    if (t0 > t1)
        return 0;
    float sx = gx + ddx * t0, sz = gz + ddz * t0;
    int ix = (int)floorf(sx), iz = (int)floorf(sz);
    if (ix >= (int)hdr->gw) ix = hdr->gw - 1;
    if (iz >= (int)hdr->gd) iz = hdr->gd - 1;
    if (ix < 0) ix = 0;
    if (iz < 0) iz = 0;
    int stx = ddx > 0 ? 1 : -1, stz = ddz > 0 ? 1 : -1;
    float tdx = ddx != 0 ? fabsf(1 / ddx) : 1e30f, tdz = ddz != 0 ? fabsf(1 / ddz) : 1e30f;
    float tmx = ddx != 0 ? t0 + ((ddx > 0 ? ix + 1 - sx : sx - ix) * tdx) : 1e30f;
    float tmz = ddz != 0 ? t0 + ((ddz > 0 ? iz + 1 - sz : sz - iz) * tdz) : 1e30f;
    float best = 2;
    int best_tri = -1;
    next_stamp(cb);
    const unsigned short cur_stamp = cb->cur_stamp;
    for (;;) {
        unsigned k = iz * hdr->gw + ix;
        for (unsigned j = cells[k]; j < cells[k + 1]; j++) {
            unsigned ti = list[j];
            if (stamp[ti] == cur_stamp)
                continue;
            stamp[ti] = cur_stamp;
            const Tri *t = &tris[ti];
            if (!(mask >> t->hit_filter & 1) || TRI_OFF(cb, ti))
                continue;
            float a[3], b[3], c[3], tt;
            tri_verts(cb, t, a, b, c);
            if (seg_tri(from, to, a, b, c, &tt) && tt < best)
                best = tt, best_tri = ti;
        }
        float t_exit = tmx < tmz ? tmx : tmz;
        if (best <= t_exit || t_exit > t1)
            break;
        if (tmx < tmz) {
            ix += stx;
            tmx += tdx;
            if (ix < 0 || ix >= (int)hdr->gw)
                break;
        } else {
            iz += stz;
            tmz += tdz;
            if (iz < 0 || iz >= (int)hdr->gd)
                break;
        }
    }
    if (best_tri < 0)
        return 0;
    if (hit) {
        const Tri *t = &tris[best_tri];
        float a[3], b[3], c[3];
        tri_verts(cb, t, a, b, c);
        tri_normal(a, b, c, hit->n);
        if (dot3(hit->n, d) > 0)
            hit->n[0] = -hit->n[0], hit->n[1] = -hit->n[1], hit->n[2] = -hit->n[2];
        hit->t = best, hit->y = from[1] + d[1] * best, hit->tri = best_tri, hit->block = (int)(cb - blk);
        hit->hit_filter = t->hit_filter, hit->material = t->material;
    }
    return 1;
}
