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
} ColBlock;
static ColBlock blk[COL_BLOCKS];
static int ray1(ColBlock *c, const float from[3], const float to[3], unsigned mask, ColHit *hit);

#define TRI_OFF(c, ti) ((c)->off && (c)->off[(ti) >> 3] >> ((ti) & 7) & 1)
int col_contacts;

/* 1 if the box [lo, hi] overlaps block c's bounds */
static inline int box_hits(const ColBlock *c, const float lo[3], const float hi[3])
{
    return c->hdr && lo[0] <= c->hi[0] && hi[0] >= c->lo[0] && lo[1] <= c->hi[1] && hi[1] >= c->lo[1] &&
           lo[2] <= c->hi[2] && hi[2] >= c->lo[2];
}

int col_load(int b, const void *blob)
{
    if (b < 0 || b >= COL_BLOCKS)
        return 0;
    col_unload(b);
    const Header *h = blob;
    if (!h || memcmp(h->magic, "DCOL", 4) || h->version != 1)
        return 0;
    const unsigned char *p = blob;
    ColBlock *c = &blk[b];
    c->stamp = calloc(h->ntris, sizeof *c->stamp);
    if (!c->stamp)
        return 0;
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
    free(blk[b].off);
    memset(&blk[b], 0, sizeof blk[b]);
}

int col_init(const void *blob)
{
    return col_load(0, blob);  /* collision.bin from the area pack (main.c): block 0 */
}

int col_tris_enable(int b, unsigned first, unsigned count, int on)
{
    if (b < 0 || b >= COL_BLOCKS || !blk[b].hdr || first + count > blk[b].hdr->ntris)
        return 0;
    ColBlock *c = &blk[b];
    if (!c->off && !on) {
        c->off = calloc((c->hdr->ntris + 7) >> 3, 1);
        if (!c->off)
            return 0;
    }
    if (c->off)
        for (unsigned t = first; t < first + count; t++)
            c->off[t >> 3] = on ? c->off[t >> 3] & ~(1 << (t & 7)) : c->off[t >> 3] | 1 << (t & 7);
    return 1;
}

int col_tris(void) { return blk[0].hdr ? (int)blk[0].hdr->ntris : 0; }
int col_block_tris(int b) { return b >= 0 && b < COL_BLOCKS && blk[b].hdr ? (int)blk[b].hdr->ntris : 0; }

/* New query id for the per-triangle "already tested" stamps (triangles span several cells) */
static void next_stamp(ColBlock *c)
{
    if (++c->cur_stamp == 0) {
        memset(c->stamp, 0, c->hdr->ntris * sizeof *c->stamp);
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

static int push1(ColBlock *cb, float pos[3], float r, float height, float step, unsigned mask)
{
    const Header *hdr = cb->hdr;
    if (!hdr)
        return 0;
    const unsigned *cells = cb->cells;
    const unsigned short *list = cb->list;
    const Tri *tris = cb->tris;
    unsigned short *stamp = cb->stamp;
    int contacts = 0;
    for (int iter = 0; iter < MAX_PUSH_ITERS; iter++) {
        float p0[3] = {pos[0], 0, pos[2]}, p1[3] = {pos[0], 0, pos[2]};
        capsule_axis(pos, r, height, step, &p0[1], &p1[1]);
        int ix0, iz0, ix1, iz1;
        cell_index(cb, pos[0] - r, pos[2] - r, &ix0, &iz0);
        cell_index(cb, pos[0] + r, pos[2] + r, &ix1, &iz1);
        if (ix0 < 0) ix0 = 0;
        if (iz0 < 0) iz0 = 0;
        if (ix1 >= (int)hdr->gw) ix1 = hdr->gw - 1;
        if (iz1 >= (int)hdr->gd) iz1 = hdr->gd - 1;
        float lo[3] = {pos[0] - r, p0[1] - r, pos[2] - r}, hi[3] = {pos[0] + r, p1[1] + r, pos[2] + r};
        int moved = 0;
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
                    int out = 0;
                    for (int i = 0; i < 3 && !out; i++)
                        out = (a[i] < lo[i] && b[i] < lo[i] && c[i] < lo[i]) ||
                              (a[i] > hi[i] && b[i] > hi[i] && c[i] > hi[i]);
                    if (out)
                        continue;
                    float n[3];
                    tri_normal(a, b, c, n);
                    if (n[1] >= COL_WALKABLE_NY || -n[1] >= COL_WALKABLE_NY)
                        continue; /* floors (col_floor) and flat ceilings do not push sideways */
                    float cs[3], ct[3];
                    float d2 = closest_seg_tri(p0, p1, a, b, c, cs, ct);
                    if (d2 >= r * r)
                        continue;
                    float dx = cs[0] - ct[0], dz = cs[2] - ct[2], dist = sqrtf(d2), hl = sqrtf(dx * dx + dz * dz);
                    if (hl < 1e-4f) {
                        /* segment touches/crosses the triangle: push along the horizontal normal, towards the side
                         * the capsule axis is on */
                        float side[3];
                        sub3(p0, a, side);
                        float sgn = dot3(side, n) >= 0 ? 1 : -1;
                        dx = n[0] * sgn, dz = n[2] * sgn;
                        hl = sqrtf(dx * dx + dz * dz);
                        if (hl < 1e-4f)
                            continue;
                        dist = 0;
                    }
                    /* horizontal move that brings the separation back to r (the vertical part stays) */
                    float dy = dist > 0 ? (cs[1] - ct[1]) : 0, need = sqrtf(r * r - dy * dy) - hl * (dist > 0);
                    if (need <= 0)
                        continue;
                    pos[0] += dx / hl * (need + 1e-3f);
                    pos[2] += dz / hl * (need + 1e-3f);
                    contacts++;
                    col_contacts++;
                    moved = 1;
                    p0[0] = p1[0] = pos[0];
                    p0[2] = p1[2] = pos[2];
                    lo[0] = pos[0] - r, hi[0] = pos[0] + r, lo[2] = pos[2] - r, hi[2] = pos[2] + r;
                }
            }
        if (!moved)
            break;
    }
    return contacts;
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
