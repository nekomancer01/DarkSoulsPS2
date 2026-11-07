/* Point-to-mesh distances for tools/horizon.py's error check (mesh_err): for each point, the distance to the nearest
 * triangle of a mesh, searched only within that point's own radius (the answer is capped at it). Triangles go into a
 * sparse grid (cell h metres, their bounding boxes: (cell key, triangle) pairs sorted by key); a query walks the cells
 * its radius box overlaps. Plain C99, no dependencies. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct { int64_t key; int tri; } Pair;

static int cmp_pair(const void *a, const void *b)
{
    int64_t x = ((const Pair *)a)->key, y = ((const Pair *)b)->key;
    return x < y ? -1 : x > y;
}

static int64_t cell_key(int64_t x, int64_t y, int64_t z)
{
    return ((x + (1 << 20)) << 42) | ((y + (1 << 20)) << 21) | (z + (1 << 20));
}

static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* squared distance from p to triangle abc (Ericson, Real-Time Collision Detection 5.1.5) */
static double tri_d2(const double *p, const float *t)
{
    double a[3] = {t[0], t[1], t[2]}, b[3] = {t[3], t[4], t[5]}, c[3] = {t[6], t[7], t[8]};
    double ab[3], ac[3], ap[3], bp[3], cp[3], q[3];
    for (int k = 0; k < 3; k++)
        ab[k] = b[k] - a[k], ac[k] = c[k] - a[k], ap[k] = p[k] - a[k], bp[k] = p[k] - b[k], cp[k] = p[k] - c[k];
    double d1 = dot3(ab, ap), d2 = dot3(ac, ap), d3 = dot3(ab, bp), d4 = dot3(ac, bp), d5 = dot3(ab, cp),
           d6 = dot3(ac, cp);
    if (d1 <= 0 && d2 <= 0)
        return dot3(ap, ap);
    if (d3 >= 0 && d4 <= d3)
        return dot3(bp, bp);
    if (d6 >= 0 && d5 <= d6)
        return dot3(cp, cp);
    double vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4, v, w;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        v = d1 / (d1 - d3);
        for (int k = 0; k < 3; k++)
            q[k] = a[k] + v * ab[k];
    } else if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        w = d2 / (d2 - d6);
        for (int k = 0; k < 3; k++)
            q[k] = a[k] + w * ac[k];
    } else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; k++)
            q[k] = b[k] + w * (c[k] - b[k]);
    } else {
        double den = va + vb + vc;
        if (den <= 0)  /* degenerate: nearest of the three vertices */
        {
            double m = dot3(ap, ap);
            return fmin(m, fmin(dot3(bp, bp), dot3(cp, cp)));
        }
        v = vb / den, w = vc / den;
        for (int k = 0; k < 3; k++)
            q[k] = a[k] + ab[k] * v + ac[k] * w;
    }
    for (int k = 0; k < 3; k++)
        q[k] = p[k] - q[k];
    return dot3(q, q);
}

/* nt triangles (9 floats each), np points (3 floats), rmax per point; out = distance, at most rmax. h: cell size.
 * Returns 0, or -1 when out of memory (out untouched). */
int hz_dist(int nt, const float *tri, int np, const float *pts, const float *rmax, float h, float *out)
{
    size_t cap = (size_t)nt * 8 + 16, n = 0;
    Pair *pr = malloc(cap * sizeof *pr);
    if (!pr)
        return -1;
    for (int i = 0; i < nt; i++) {
        const float *t = tri + 9 * (size_t)i;
        int64_t lo[3], hi[3];
        for (int k = 0; k < 3; k++) {
            float mn = fminf(t[k], fminf(t[3 + k], t[6 + k])), mx = fmaxf(t[k], fmaxf(t[3 + k], t[6 + k]));
            lo[k] = (int64_t)floorf(mn / h), hi[k] = (int64_t)floorf(mx / h);
        }
        for (int64_t x = lo[0]; x <= hi[0]; x++)
            for (int64_t y = lo[1]; y <= hi[1]; y++)
                for (int64_t z = lo[2]; z <= hi[2]; z++) {
                    if (n == cap) {
                        Pair *q = realloc(pr, (cap *= 2) * sizeof *pr);
                        if (!q) {
                            free(pr);
                            return -1;
                        }
                        pr = q;
                    }
                    pr[n].key = cell_key(x, y, z), pr[n++].tri = i;
                }
    }
    qsort(pr, n, sizeof *pr, cmp_pair);
    for (int i = 0; i < np; i++) {
        double p[3] = {pts[3 * (size_t)i], pts[3 * (size_t)i + 1], pts[3 * (size_t)i + 2]};
        double r = rmax[i], best = r * r;
        int64_t lo[3], hi[3];
        for (int k = 0; k < 3; k++)
            lo[k] = (int64_t)floor((p[k] - r) / h), hi[k] = (int64_t)floor((p[k] + r) / h);
        for (int64_t x = lo[0]; x <= hi[0]; x++)
            for (int64_t y = lo[1]; y <= hi[1]; y++)
                for (int64_t z = lo[2]; z <= hi[2]; z++) {
                    int64_t key = cell_key(x, y, z);
                    size_t a = 0, b = n;  /* first pair with this key */
                    while (a < b) {
                        size_t m = (a + b) / 2;
                        if (pr[m].key < key)
                            a = m + 1;
                        else
                            b = m;
                    }
                    for (; a < n && pr[a].key == key; a++) {
                        double d2 = tri_d2(p, tri + 9 * (size_t)pr[a].tri);
                        if (d2 < best)
                            best = d2;
                    }
                }
        out[i] = (float)sqrt(best);
    }
    free(pr);
    return 0;
}
