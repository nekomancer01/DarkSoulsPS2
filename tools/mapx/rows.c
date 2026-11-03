/* small gather helpers: first-appearance ids of int rows (export_map._first_rows, dedup_tris) and is_sky's
   "any point within r of the walkable samples" */
#include "mapx.h"

/* rows K (n x k int64) -> id per row numbered by first appearance, and each id's first row; returns the id count */
MX_API long long mx_first_rows(long long n, int k, const long long *K, long long *id, long long *first)
{
    mx_map m;
    mx_map_init(&m, k, n);
    for (long long i = 0; i < n; i++) {
        int added;
        long long d = mx_map_put(&m, K + (size_t)k * i, &added);
        if (added)
            first[d] = i;
        id[i] = d;
    }
    long long u = m.n;
    mx_map_free(&m);
    return u;
}

/* 1 if some point P[i] (n x 3) has a squared distance <= r2 to some sample S[j] (m x 3), summed as numpy's
   ((P - S) ** 2).sum(-1) does ((dx^2 + dy^2) + dz^2); 0 otherwise */
MX_API int mx_any_within(long long n, const double *P, long long m, const double *S, double r2)
{
    for (long long i = 0; i < n; i++) {
        const double *p = P + 3 * i;
        for (long long j = 0; j < m; j++) {
            const double *s = S + 3 * j;
            double dx = p[0] - s[0], dy = p[1] - s[1], dz = p[2] - s[2];
            if ((dx * dx + dy * dy) + dz * dz <= r2)
                return 1;
        }
    }
    return 0;
}

double pow(double, double);

/* Python's x ** e per element (float.__pow__ calls libm pow); pos: x ** e if x > 0.0 else 0.0 (srgb_lin) */
MX_API void mx_pow_many(long long n, const double *X, double e, int pos, double *out)
{
    for (long long i = 0; i < n; i++)
        out[i] = pos && !(X[i] > 0.0) ? 0.0 : pow(X[i], e);
}

/* a FLVER face set's triangles as souls.triangles gives them: a list (strip 0) in consecutive triples; a strip
   unrolled, every other triangle flipped (a, c, b), 0xFFFF restarting the strip (the flip too), triangles with a
   repeated index dropped (they still flip the next). out: room for n - 2 triples. Returns the triangle count. */
MX_API long long mx_faceset_tris(long long n, const long long *idx, int strip, long long *out)
{
    long long k = 0;
    if (!strip) {
        for (long long i = 0; i + 2 < n; i += 3, k++)
            out[3 * k] = idx[i], out[3 * k + 1] = idx[i + 1], out[3 * k + 2] = idx[i + 2];
        return k;
    }
    int flip = 0;
    for (long long i = 0; i + 2 < n; i++) {
        long long a = idx[i], b = idx[i + 1], c = idx[i + 2];
        if (a == 0xFFFF || b == 0xFFFF || c == 0xFFFF) {
            flip = 0;
            continue;
        }
        if (a != b && b != c && a != c) {
            out[3 * k] = a;
            out[3 * k + 1] = flip ? c : b;
            out[3 * k + 2] = flip ? b : c;
            k++;
        }
        flip = !flip;
    }
    return k;
}
