/* Map collision queries over collision.bin (layout: tools/export_collision.py): quantized shared vertices, triangles
 * with HitFilterID + material, uniform x/z grid of triangle lists. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "collision.h"


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

/* One collision.bin per world block (src/world.c): the queries below run on the bound block's arrays; the public
 * functions loop over the loaded blocks (all in one world frame) */
typedef struct {
    const Header *hdr;
    const short *verts;
    const Tri *tris;
    const unsigned *cells;
    const unsigned short *list;
    unsigned short *stamp, cur_stamp;
    float inv_cell;
    unsigned char *off;  /* EMEVD "Activate Hit" (src/event.c): bit per triangle, 1 = switched off; NULL = all on */
} ColBlock;
static ColBlock blk[COL_BLOCKS];
static int bound;
static int ray1(const float from[3], const float to[3], unsigned mask, ColHit *hit);

static const Header *hdr;
static const short *verts;
static const Tri *tris;
static const unsigned *cells;
static const unsigned short *list;
static unsigned short *stamp, cur_stamp;
static float inv_cell;
static const unsigned char *off;
#define TRI_OFF(ti) (off && off[(ti) >> 3] >> ((ti) & 7) & 1)
int col_contacts;

static int bind(int b)
{
    blk[bound].cur_stamp = cur_stamp;
    const ColBlock *c = &blk[bound = b];
    hdr = c->hdr, verts = c->verts, tris = c->tris, cells = c->cells, list = c->list;
    stamp = c->stamp, cur_stamp = c->cur_stamp, inv_cell = c->inv_cell, off = c->off;
    return hdr != 0;
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
    c->hdr = h;
    bind(b);
    return 1;
}

void col_unload(int b)
{
    if (b < 0 || b >= COL_BLOCKS)
        return;
    free(blk[b].stamp);
    free(blk[b].off);
    memset(&blk[b], 0, sizeof blk[b]);
    if (bound == b)
        bind(b);
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
        if (bound == b)
            off = c->off;
    }
    if (c->off)
        for (unsigned t = first; t < first + count; t++)
            c->off[t >> 3] = on ? c->off[t >> 3] & ~(1 << (t & 7)) : c->off[t >> 3] | 1 << (t & 7);
    return 1;
}

int col_tris(void) { return blk[0].hdr ? (int)blk[0].hdr->ntris : 0; }
int col_block_tris(int b) { return b >= 0 && b < COL_BLOCKS && blk[b].hdr ? (int)blk[b].hdr->ntris : 0; }

/* New query id for the per-triangle "already tested" stamps (triangles span several cells) */
static void next_stamp(void)
{
    if (++cur_stamp == 0) {
        memset(stamp, 0, hdr->ntris * sizeof *stamp);
        cur_stamp = 1;
    }
}

static inline void vert(unsigned i, float *o)
{
    const short *v = &verts[i * 3];
    o[0] = hdr->origin[0] + v[0] * hdr->scale;
    o[1] = hdr->origin[1] + v[1] * hdr->scale;
    o[2] = hdr->origin[2] + v[2] * hdr->scale;
}

static inline void tri_verts(const Tri *t, float *a, float *b, float *c)
{
    vert(t->v[0], a);
    vert(t->v[1], b);
    vert(t->v[2], c);
}

static inline float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void sub(const float *a, const float *b, float *o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
static inline void cross(const float *a, const float *b, float *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
static inline void lerp(const float *a, const float *b, float t, float *o)
{
    o[0] = a[0] + (b[0] - a[0]) * t, o[1] = a[1] + (b[1] - a[1]) * t, o[2] = a[2] + (b[2] - a[2]) * t;
}

static void tri_normal(const float *a, const float *b, const float *c, float *n)
{
    float e1[3], e2[3];
    sub(b, a, e1);
    sub(c, a, e2);
    cross(e1, e2, n);
    float l = sqrtf(dot(n, n));
    if (l > 0)
        n[0] /= l, n[1] /= l, n[2] /= l;
}

static inline int cell_index(float x, float z, int *ix, int *iz)
{
    *ix = (int)floorf((x - hdr->x0) * inv_cell);
    *iz = (int)floorf((z - hdr->z0) * inv_cell);
    return *ix >= 0 && *iz >= 0 && *ix < (int)hdr->gw && *iz < (int)hdr->gd;
}

static int floor1(float x, float z, float y_top, float y_bottom, unsigned mask, ColHit *hit)
{
    int ix, iz;
    if (!hdr || !cell_index(x, z, &ix, &iz))
        return 0;
    unsigned k = iz * hdr->gw + ix;
    int found = -1;
    float best = 0;
    for (unsigned j = cells[k]; j < cells[k + 1]; j++) {
        const Tri *t = &tris[list[j]];
        if (!(mask >> t->hit_filter & 1) || TRI_OFF(list[j]))
            continue;
        float a[3], b[3], c[3];
        tri_verts(t, a, b, c);
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
        tri_verts(t, a, b, c);
        tri_normal(a, b, c, hit->n);
        if (hit->n[1] < 0)
            hit->n[0] = -hit->n[0], hit->n[1] = -hit->n[1], hit->n[2] = -hit->n[2];
        hit->y = best, hit->t = 0, hit->tri = found, hit->block = bound;
        hit->hit_filter = t->hit_filter, hit->material = t->material;
    }
    return 1;
}

/* ---- closest points (Ericson, Real-Time Collision Detection 5.1.5 / 5.1.9) */

static void closest_pt_tri(const float *p, const float *a, const float *b, const float *c, float *o)
{
    float ab[3], ac[3], ap[3], bp[3], cp[3];
    sub(b, a, ab);
    sub(c, a, ac);
    sub(p, a, ap);
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) {
        memcpy(o, a, 12);
        return;
    }
    sub(p, b, bp);
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) {
        memcpy(o, b, 12);
        return;
    }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        lerp(a, b, d1 / (d1 - d3), o);
        return;
    }
    sub(p, c, cp);
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) {
        memcpy(o, c, 12);
        return;
    }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        lerp(a, c, d2 / (d2 - d6), o);
        return;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
        lerp(b, c, (d4 - d3) / ((d4 - d3) + (d5 - d6)), o);
        return;
    }
    float den = 1 / (va + vb + vc), v = vb * den, w = vc * den;
    for (int i = 0; i < 3; i++)
        o[i] = a[i] + ab[i] * v + ac[i] * w;
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

/* Closest points between segments p1-q1 and p2-q2 -> c1, c2; returns squared distance */
static float closest_seg_seg(const float *p1, const float *q1, const float *p2, const float *q2, float *c1, float *c2)
{
    float d1[3], d2[3], r[3], s, t;
    sub(q1, p1, d1);
    sub(q2, p2, d2);
    sub(p1, p2, r);
    float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    if (a <= 1e-9f && e <= 1e-9f) {
        s = t = 0;
    } else if (a <= 1e-9f) {
        s = 0, t = clamp01(f / e);
    } else {
        float c = dot(d1, r);
        if (e <= 1e-9f) {
            t = 0, s = clamp01(-c / a);
        } else {
            float b = dot(d1, d2), den = a * e - b * b;
            s = den != 0 ? clamp01((b * f - c * e) / den) : 0;
            t = (b * s + f) / e;
            if (t < 0)
                t = 0, s = clamp01(-c / a);
            else if (t > 1)
                t = 1, s = clamp01((b - c) / a);
        }
    }
    lerp(p1, q1, s, c1);
    lerp(p2, q2, t, c2);
    float d[3];
    sub(c1, c2, d);
    return dot(d, d);
}

/* Segment p-q against triangle: 1 if they intersect (Moller-Trumbore, two-sided), *t = fraction along p-q */
static int seg_tri(const float *p, const float *q, const float *a, const float *b, const float *c, float *t)
{
    float e1[3], e2[3], d[3], h[3], s[3], qv[3];
    sub(b, a, e1);
    sub(c, a, e2);
    sub(q, p, d);
    cross(d, e2, h);
    float det = dot(e1, h);
    if (det > -1e-9f && det < 1e-9f)
        return 0;
    float inv = 1 / det;
    sub(p, a, s);
    float u = dot(s, h) * inv;
    if (u < 0 || u > 1)
        return 0;
    cross(s, e1, qv);
    float v = dot(d, qv) * inv;
    if (v < 0 || u + v > 1)
        return 0;
    float tt = dot(e2, qv) * inv;
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
        lerp(p, q, t, cs);
        memcpy(ct, cs, 12);
        return 0;
    }
    closest_pt_tri(p, a, b, c, ct);
    memcpy(cs, p, 12);
    sub(cs, ct, d);
    best = dot(d, d);
    closest_pt_tri(q, a, b, c, tmp_t);
    sub(q, tmp_t, d);
    if (dot(d, d) < best) {
        best = dot(d, d);
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

static int push1(float pos[3], float r, float height, float step, unsigned mask)
{
    if (!hdr)
        return 0;
    int contacts = 0;
    for (int iter = 0; iter < MAX_PUSH_ITERS; iter++) {
        float p0[3] = {pos[0], pos[1] + step + r, pos[2]}, p1[3] = {pos[0], pos[1] + height - r, pos[2]};
        if (p1[1] < p0[1])
            p0[1] = p1[1] = (p0[1] + p1[1]) * 0.5f;
        int ix0, iz0, ix1, iz1;
        cell_index(pos[0] - r, pos[2] - r, &ix0, &iz0);
        cell_index(pos[0] + r, pos[2] + r, &ix1, &iz1);
        if (ix0 < 0) ix0 = 0;
        if (iz0 < 0) iz0 = 0;
        if (ix1 >= (int)hdr->gw) ix1 = hdr->gw - 1;
        if (iz1 >= (int)hdr->gd) iz1 = hdr->gd - 1;
        float lo[3] = {pos[0] - r, p0[1] - r, pos[2] - r}, hi[3] = {pos[0] + r, p1[1] + r, pos[2] + r};
        int moved = 0;
        next_stamp();
        for (int iz = iz0; iz <= iz1; iz++)
            for (int ix = ix0; ix <= ix1; ix++) {
                unsigned k = iz * hdr->gw + ix;
                for (unsigned j = cells[k]; j < cells[k + 1]; j++) {
                    unsigned ti = list[j];
                    if (stamp[ti] == cur_stamp)
                        continue;
                    stamp[ti] = cur_stamp;
                    const Tri *t = &tris[ti];
                    if (!(mask >> t->hit_filter & 1) || TRI_OFF(ti))
                        continue;
                    float a[3], b[3], c[3];
                    tri_verts(t, a, b, c);
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
                        sub(p0, a, side);
                        float sgn = dot(side, n) >= 0 ? 1 : -1;
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
    for (int b = 0; b < COL_BLOCKS; b++)
        if (blk[b].hdr && bind(b) && floor1(x, z, y_top, y_bottom, mask, &h) && (!found || h.y > best.y))
            best = h, found = 1;
    if (found && hit)
        *hit = best;
    return found;
}

int col_floor_block(int b, float x, float z, float y_top, float y_bottom, unsigned mask, ColHit *hit)
{
    ColHit h;
    if (b < 0 || b >= COL_BLOCKS || !blk[b].hdr || !bind(b) || !floor1(x, z, y_top, y_bottom, mask, &h))
        return 0;
    if (hit)
        *hit = h;
    return 1;
}

int col_push_capsule(float pos[3], float r, float height, float step, unsigned mask)
{
    int contacts = 0;
    for (int b = 0; b < COL_BLOCKS; b++)
        if (blk[b].hdr && bind(b))
            contacts += push1(pos, r, height, step, mask);
    return contacts;
}

int col_ray(const float from[3], const float to[3], unsigned mask, ColHit *hit)
{
    ColHit h, best;
    int found = 0;
    for (int b = 0; b < COL_BLOCKS; b++)
        if (blk[b].hdr && bind(b) && ray1(from, to, mask, &h) && (!found || h.t < best.t))
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

static int ray1(const float from[3], const float to[3], unsigned mask, ColHit *hit)
{
    if (!hdr)
        return 0;
    float d[3];
    sub(to, from, d);
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
    next_stamp();
    for (;;) {
        unsigned k = iz * hdr->gw + ix;
        for (unsigned j = cells[k]; j < cells[k + 1]; j++) {
            unsigned ti = list[j];
            if (stamp[ti] == cur_stamp)
                continue;
            stamp[ti] = cur_stamp;
            const Tri *t = &tris[ti];
            if (!(mask >> t->hit_filter & 1) || TRI_OFF(ti))
                continue;
            float a[3], b[3], c[3], tt;
            tri_verts(t, a, b, c);
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
        tri_verts(t, a, b, c);
        tri_normal(a, b, c, hit->n);
        if (dot(hit->n, d) > 0)
            hit->n[0] = -hit->n[0], hit->n[1] = -hit->n[1], hit->n[2] = -hit->n[2];
        hit->t = best, hit->y = from[1] + d[1] * best, hit->tri = best_tri, hit->block = bound;
        hit->hit_filter = t->hit_filter, hit->material = t->material;
    }
    return 1;
}
