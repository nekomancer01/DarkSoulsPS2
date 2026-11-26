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
    /* per triangle its box (min xyz, max xyz): a triangle whose box is further from a point than the nearest found so
       far can not be nearer, and is not measured (the margin covers rounding; the same distances come out) */
    float *bb = malloc(sizeof(float) * 6 * (size_t)(nt > 0 ? nt : 1));
    if (!bb) {
        free(pr);
        return -1;
    }
    for (int i = 0; i < nt; i++) {
        const float *t = tri + 9 * (size_t)i;
        for (int k = 0; k < 3; k++)
            bb[6 * i + k] = fminf(t[k], fminf(t[3 + k], t[6 + k])), bb[6 * i + 3 + k] = fmaxf(t[k], fmaxf(t[3 + k], t[6 + k]));
    }
    /* the distinct cells (key, first pair, the union of their triangles' boxes): a cell whose union box is further than
       the nearest so far holds no triangle that passes its own box test (the union's distance is no larger, also after
       rounding), so it is skipped whole; the same triangles are measured in the same order */
    size_t nc = 0;
    for (size_t e = 0; e < n; e++)
        nc += !e || pr[e].key != pr[e - 1].key;
    int64_t *ck = malloc(sizeof(int64_t) * (nc ? nc : 1));
    size_t *cst = malloc(sizeof(size_t) * (nc + 1));
    float *cbb = malloc(sizeof(float) * 6 * (nc ? nc : 1));
    if (!ck || !cst || !cbb) {
        free(pr), free(bb), free(ck), free(cst), free(cbb);
        return -1;
    }
    for (size_t e = 0, k = (size_t)-1; e < n; e++) {
        const float *B = bb + 6 * (size_t)pr[e].tri;
        if (!e || pr[e].key != pr[e - 1].key) {
            k++;
            ck[k] = pr[e].key, cst[k] = e;
            for (int a = 0; a < 6; a++)
                cbb[6 * k + a] = B[a];
            continue;
        }
        for (int a = 0; a < 3; a++) {
            if (B[a] < cbb[6 * k + a]) cbb[6 * k + a] = B[a];
            if (B[3 + a] > cbb[6 * k + 3 + a]) cbb[6 * k + 3 + a] = B[3 + a];
        }
    }
    cst[nc] = n;
    /* points are independent (each writes only out[i]): the same values with any thread count (-fopenmp, or serial) */
#pragma omp parallel for schedule(dynamic, 512)
    for (int i = 0; i < np; i++) {
        double p[3] = {pts[3 * (size_t)i], pts[3 * (size_t)i + 1], pts[3 * (size_t)i + 2]};
        double r = rmax[i], best = r * r;
        int64_t lo[3], hi[3];
        for (int k = 0; k < 3; k++)
            lo[k] = (int64_t)floor((p[k] - r) / h), hi[k] = (int64_t)floor((p[k] + r) / h);
        /* the cells of the radius box in shells around the point's own cell (Chebyshev ring k), nearest first; a ring
           whose cells all lie further than the nearest triangle found so far can not hold a nearer one, so the search
           ends there (the minimum is the same in any order) */
        int64_t c0[3], K = 0;
        for (int k = 0; k < 3; k++) {
            c0[k] = (int64_t)floor(p[k] / h);
            if (c0[k] < lo[k]) c0[k] = lo[k];
            if (c0[k] > hi[k]) c0[k] = hi[k];
            if (c0[k] - lo[k] > K) K = c0[k] - lo[k];
            if (hi[k] - c0[k] > K) K = hi[k] - c0[k];
        }
        for (int64_t k = 0; k <= K; k++) {
            double gap = (double)(k - 1) * h;
            if (k > 1 && gap * gap > best * 1.000001 + 1e-12)
                break;
            for (int64_t x = c0[0] - k; x <= c0[0] + k; x++) {
                if (x < lo[0] || x > hi[0])
                    continue;
                for (int64_t y = c0[1] - k; y <= c0[1] + k; y++) {
                    if (y < lo[1] || y > hi[1])
                        continue;
                    int edge = x == c0[0] - k || x == c0[0] + k || y == c0[1] - k || y == c0[1] + k;
                    for (int64_t z = c0[2] - k; z <= c0[2] + k; z += edge || k == 0 ? 1 : 2 * k) {
                        if (z < lo[2] || z > hi[2])
                            continue;
                        int64_t key = cell_key(x, y, z);
                        size_t a = 0, b = nc;  /* the cell with this key */
                        while (a < b) {
                            size_t m = (a + b) / 2;
                            if (ck[m] < key)
                                a = m + 1;
                            else
                                b = m;
                        }
                        if (a == nc || ck[a] != key)
                            continue;
                        {
                            const float *B = cbb + 6 * a;
                            double e0 = p[0] < B[0] ? B[0] - p[0] : p[0] > B[3] ? p[0] - B[3] : 0,
                                   e1 = p[1] < B[1] ? B[1] - p[1] : p[1] > B[4] ? p[1] - B[4] : 0,
                                   e2 = p[2] < B[2] ? B[2] - p[2] : p[2] > B[5] ? p[2] - B[5] : 0;
                            if (e0 * e0 + e1 * e1 + e2 * e2 > best * 1.000001 + 1e-12)
                                continue;
                        }
                        size_t end = cst[a + 1];
                        for (a = cst[a]; a < end; a++) {
                            const float *B = bb + 6 * (size_t)pr[a].tri;
                            double e0 = p[0] < B[0] ? B[0] - p[0] : p[0] > B[3] ? p[0] - B[3] : 0,
                                   e1 = p[1] < B[1] ? B[1] - p[1] : p[1] > B[4] ? p[1] - B[4] : 0,
                                   e2 = p[2] < B[2] ? B[2] - p[2] : p[2] > B[5] ? p[2] - B[5] : 0;
                            if (e0 * e0 + e1 * e1 + e2 * e2 > best * 1.000001 + 1e-12)
                                continue;
                            double d2 = tri_d2(p, tri + 9 * (size_t)pr[a].tri);
                            if (d2 < best)
                                best = d2;
                        }
                    }
                }
            }
        }
        out[i] = (float)sqrt(best);
    }
    free(pr), free(bb), free(ck), free(cst), free(cbb);
    return 0;
}
