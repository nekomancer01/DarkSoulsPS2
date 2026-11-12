/* Conservative triangle visibility for tools/horizon.py (HZN_VIS): which triangles of a block can be seen at all, from
 * the block's walkable area (cube maps at navmesh points) or from outside it (orthographic views from directions all
 * round: a far camera in a neighbouring block sees a surface only along a ray that leaves this block's geometry).
 *
 *   hz_vis(n, tri, set, nview, view, ndir, res_o, res_p, out)
 *     tri: n x 9 floats (corners), set: n ints (draw-group set; 0 = always drawn), view: nview x 3 eye points,
 *     out: n bytes, set to 1 for every triangle some view may see (the caller zeroes it)
 *
 * Each view renders a depth buffer of the occluders, then tests each triangle against it: the triangle is hidden only
 * when every pixel it covers, grown by one pixel all round, holds a surface clearly nearer than the triangle there
 * (outside it: the depth of its nearest edge point). So slivers that cover no pixel centre, and surfaces
 * just behind a crack, are kept: an error keeps a triangle, never drops one. Occluders of a set-s triangle are set 0 and set s only (map.c may hide any other
 * set: what it hides cannot be relied on to cover). Double-sided (map.c draws the horizon without culling). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, z; } P3;  /* screen x, y (pixels), depth (larger = nearer) */

static void raster(float *zb, int R, const P3 *p, int m)
{
    for (int t = 1; t + 1 < m; t++) {
        const P3 *a = &p[0], *b = &p[t], *c = &p[t + 1];
        float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
        if (fabsf(area) < 1e-12f)
            continue;
        float fx0 = fminf(a->x, fminf(b->x, c->x)), fx1 = fmaxf(a->x, fmaxf(b->x, c->x));
        float fy0 = fminf(a->y, fminf(b->y, c->y)), fy1 = fmaxf(a->y, fmaxf(b->y, c->y));
        if (fx1 < 0 || fy1 < 0 || fx0 >= R || fy0 >= R)
            continue;
        int x0 = fx0 < 0 ? 0 : (int)fx0, x1 = fx1 >= R ? R - 1 : (int)fx1;
        int y0 = fy0 < 0 ? 0 : (int)fy0, y1 = fy1 >= R ? R - 1 : (int)fy1;
        float ia = 1.0f / area;
        for (int y = y0; y <= y1; y++) {
            float py = y + 0.5f;
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f;
                float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) * ia;
                float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) * ia;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                float z = w0 * a->z + w1 * b->z + w2 * c->z;
                if (z > zb[y * R + x])
                    zb[y * R + x] = z;
            }
        }
    }
}

/* any pixel within one pixel of the polygon p[0..m) where nothing is clearly nearer than the polygon there (the depth
 * of its nearest point to the pixel centre, + margin); edge-on and sliver triangles go by their edges */
static int seen(const float *zb, int R, const P3 *p, int m, int persp, float eps)
{
    for (int t = 1; t + 1 < m; t++) {
        const P3 *v[3] = {&p[0], &p[t], &p[t + 1]};
        float area = (v[1]->x - v[0]->x) * (v[2]->y - v[0]->y) - (v[1]->y - v[0]->y) * (v[2]->x - v[0]->x);
        float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
        for (int i = 0; i < 3; i++)
            x0 = fminf(x0, v[i]->x), x1 = fmaxf(x1, v[i]->x), y0 = fminf(y0, v[i]->y), y1 = fmaxf(y1, v[i]->y);
        if (x1 < -1 || y1 < -1 || x0 > R || y0 > R)
            continue;  /* off this view */
        int ix0 = (int)floorf(x0) - 1, ix1 = (int)floorf(x1) + 1, iy0 = (int)floorf(y0) - 1, iy1 = (int)floorf(y1) + 1;
        ix0 = ix0 < 0 ? 0 : ix0, iy0 = iy0 < 0 ? 0 : iy0, ix1 = ix1 >= R ? R - 1 : ix1, iy1 = iy1 >= R ? R - 1 : iy1;
        int flat = fabsf(area) < 1e-3f;
        float ia = flat ? 0 : 1.0f / area;
        for (int y = iy0; y <= iy1; y++) {
            float py = y + 0.5f;
            for (int x = ix0; x <= ix1; x++) {
                float px = x + 0.5f, z = -1e30f;
                if (!flat) {
                    float w0 = ((v[1]->x - px) * (v[2]->y - py) - (v[1]->y - py) * (v[2]->x - px)) * ia;
                    float w1 = ((v[2]->x - px) * (v[0]->y - py) - (v[2]->y - py) * (v[0]->x - px)) * ia;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 >= 0 && w1 >= 0 && w2 >= 0)
                        z = w0 * v[0]->z + w1 * v[1]->z + w2 * v[2]->z;
                }
                if (z == -1e30f) {  /* outside: the nearest point of the edges, if within a pixel */
                    float best = 1.0f;
                    for (int i = 0; i < 3; i++) {
                        const P3 *a = v[i], *b = v[(i + 1) % 3];
                        float ex = b->x - a->x, ey = b->y - a->y, l2 = ex * ex + ey * ey;
                        float u = l2 > 0 ? ((px - a->x) * ex + (py - a->y) * ey) / l2 : 0;
                        u = u < 0 ? 0 : u > 1 ? 1 : u;
                        float dx = a->x + u * ex - px, dy = a->y + u * ey - py, d2 = dx * dx + dy * dy;
                        if (d2 <= best)
                            best = d2, z = a->z + u * (b->z - a->z);
                    }
                    if (z == -1e30f)
                        continue;
                }
                if (zb[y * R + x] <= (persp ? z * (1.0f + eps) : z + eps))
                    return 1;
            }
        }
    }
    return 0;
}

#define NEAR 0.05f

/* polygon of triangle i in a view: orthographic (dir d, basis u v, scale s, offsets) or perspective (eye e, face basis)
 * -> m corners (0: behind the eye / degenerate); *cross: the triangle crosses the near plane */
static int project(const float *t, int persp, const float *e, const float *rt, const float *up, const float *fw,
                   float s, float ou, float ov, int R, P3 *out, int *cross)
{
    float q[3][3];
    *cross = 0;
    for (int k = 0; k < 3; k++) {
        float d[3] = {t[3 * k] - e[0], t[3 * k + 1] - e[1], t[3 * k + 2] - e[2]};
        q[k][0] = d[0] * rt[0] + d[1] * rt[1] + d[2] * rt[2];
        q[k][1] = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
        q[k][2] = d[0] * fw[0] + d[1] * fw[1] + d[2] * fw[2];
    }
    if (!persp) {
        for (int k = 0; k < 3; k++)
            out[k].x = (q[k][0] - ou) * s, out[k].y = (q[k][1] - ov) * s, out[k].z = -q[k][2];
        return 3;
    }
    float c[4][3];
    int m = 0;
    for (int i = 0; i < 3; i++) {  /* clip against z > NEAR */
        const float *a = q[i], *b = q[(i + 1) % 3];
        if (a[2] >= NEAR)
            memcpy(c[m++], a, 12);
        if ((a[2] >= NEAR) != (b[2] >= NEAR)) {
            float u = (NEAR - a[2]) / (b[2] - a[2]);
            for (int k = 0; k < 3; k++)
                c[m][k] = a[k] + u * (b[k] - a[k]);
            m++;
            *cross = 1;
        }
    }
    for (int i = 0; i < m; i++)  /* 90 degree face: x / z in [-1, 1] */
        out[i].x = (c[i][0] / c[i][2] * 0.5f + 0.5f) * R, out[i].y = (c[i][1] / c[i][2] * 0.5f + 0.5f) * R,
        out[i].z = 1.0f / c[i][2];
    return m >= 3 ? m : 0;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

/* one view: occluders per set, then the test */
static void one_view(int n, const float *tri, const int *set, const int *sets, int ns, int persp, const float *e,
                     const float *rt, const float *up, const float *fw, float s, float ou, float ov, int R, float *zb0,
                     float *zb, unsigned char *out)
{
    P3 p[4];
    int cross;
    for (int i = 0; i < R * R; i++)
        zb0[i] = -1e30f;
    for (int i = 0; i < n; i++) {
        if (set[i])
            continue;
        int m = project(tri + 9 * i, persp, e, rt, up, fw, s, ou, ov, R, p, &cross);
        if (m)
            raster(zb0, R, p, m);
    }
    for (int si = 0; si < ns; si++) {
        const float *z = zb0;
        if (sets[si]) {
            memcpy(zb, zb0, sizeof(float) * R * R);
            for (int i = 0; i < n; i++) {
                if (set[i] != sets[si])
                    continue;
                int m = project(tri + 9 * i, persp, e, rt, up, fw, s, ou, ov, R, p, &cross);
                if (m)
                    raster(zb, R, p, m);
            }
            z = zb;
        }
        for (int i = 0; i < n; i++) {
            if (out[i] || set[i] != sets[si])
                continue;
            int m = project(tri + 9 * i, persp, e, rt, up, fw, s, ou, ov, R, p, &cross);
            if (!m)
                continue;
            if (cross) {  /* through the near plane: on screen at all is enough */
                float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
                for (int k = 0; k < m; k++)
                    x0 = fminf(x0, p[k].x), x1 = fmaxf(x1, p[k].x), y0 = fminf(y0, p[k].y), y1 = fmaxf(y1, p[k].y);
                if (!(x1 < 0 || y1 < 0 || x0 > R || y0 > R))
                    out[i] = 1;
                continue;
            }
            if (seen(z, R, p, m, persp, persp ? 0.002f : 0.05f))
                out[i] = 1;
        }
    }
}

static void basis(const float *fw, float *rt, float *up)
{
    float a[3] = {0, 1, 0};
    if (fabsf(fw[1]) > 0.9f)
        a[0] = 1, a[1] = 0;
    rt[0] = a[1] * fw[2] - a[2] * fw[1], rt[1] = a[2] * fw[0] - a[0] * fw[2], rt[2] = a[0] * fw[1] - a[1] * fw[0];
    float l = sqrtf(rt[0] * rt[0] + rt[1] * rt[1] + rt[2] * rt[2]);
    for (int k = 0; k < 3; k++)
        rt[k] /= l;
    up[0] = fw[1] * rt[2] - fw[2] * rt[1], up[1] = fw[2] * rt[0] - fw[0] * rt[2], up[2] = fw[0] * rt[1] - fw[1] * rt[0];
}

void hz_vis(int n, const float *tri, const int *set, int nview, const float *view, int ndir, int res_o, int res_p,
            unsigned char *out)
{
    if (n <= 0)
        return;
    int *sets = malloc(sizeof(int) * n), ns = 0;
    memcpy(sets, set, sizeof(int) * n);
    qsort(sets, n, sizeof(int), cmp_int);
    for (int i = 0; i < n; i++)
        if (!ns || sets[ns - 1] != sets[i])
            sets[ns++] = sets[i];
    int R = res_o > res_p ? res_o : res_p;
    /* The views are independent: with -fopenmp each thread renders its share of them into its own depth buffers and
     * its own copy of out, OR-ed together at the end. A triangle is marked when any view sees it, so out is the same
     * bytes with any thread count or order (the per-thread "already seen" skip only saves work). */
    int nv = ndir + 6 * nview;
#pragma omp parallel
    {
        float *zb0 = malloc(sizeof(float) * R * R), *zb = malloc(sizeof(float) * R * R);
        unsigned char *mine = calloc(n, 1);
        const float o[3] = {0, 0, 0};
#pragma omp for schedule(dynamic, 1)
        for (int k = 0; k < nv; k++) {
            if (k < ndir) {  /* orthographic, from directions on a Fibonacci sphere */
                int di = k;
                float y = 1.0f - 2.0f * (di + 0.5f) / ndir, r = sqrtf(1.0f - y * y), ph = di * 2.39996323f;
                float d[3] = {r * cosf(ph), y, r * sinf(ph)}, fw[3] = {-d[0], -d[1], -d[2]}, rt[3], up[3];
                basis(fw, rt, up);
                float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
                for (int i = 0; i < 3 * n; i++) {
                    const float *p = tri + 3 * i;
                    float u = p[0] * rt[0] + p[1] * rt[1] + p[2] * rt[2], v = p[0] * up[0] + p[1] * up[1] + p[2] * up[2];
                    u0 = fminf(u0, u), u1 = fmaxf(u1, u), v0 = fminf(v0, v), v1 = fmaxf(v1, v);
                }
                float ext = fmaxf(u1 - u0, v1 - v0) + 2.0f, s = (res_o - 2) / ext;
                one_view(n, tri, set, sets, ns, 0, o, rt, up, fw, s, u0 - 1.0f, v0 - 1.0f, res_o, zb0, zb, mine);
            } else {  /* cube maps at the eye points */
                static const float F[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                int vi = (k - ndir) / 6, f = (k - ndir) % 6;
                float rt[3], up[3];
                basis(F[f], rt, up);
                one_view(n, tri, set, sets, ns, 1, view + 3 * vi, rt, up, F[f], 0, 0, 0, res_p, zb0, zb, mine);
            }
        }
#pragma omp critical
        for (int i = 0; i < n; i++)
            out[i] |= mine[i];
        free(zb0), free(zb), free(mine);
    }
    free(sets);
}
