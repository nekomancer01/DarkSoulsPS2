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
