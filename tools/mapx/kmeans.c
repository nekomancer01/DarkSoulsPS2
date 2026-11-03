/* export_normal._kmeans / _nearest in C, for float32 or float64 data as numpy computes them: weighted Lloyd k-means
   with a k-means++ start drawn from default_rng(seed) (rng.c), squared distances as ((x - c)^2).sum(1) (pairwise
   over the d columns), labels by argmin of |c|^2 - 2 x.c (dsr_light.matmul: products summed left to right; the first
   minimum wins), centres = column sums of x * w in float64 in row order / the pairwise sum of the weights, stored
   in the data's type. */
#include "mapx.h"

#define T float
#define S f
#include "kmeans_t.h"
#undef T
#undef S
#define T double
#define S d
#include "kmeans_t.h"
#undef T
#undef S

/* xtype / ctype: 4 = float32, 8 = float64 */
MX_API void mx_nearest(int xtype, const void *x, long long n, int d, int ctype, const void *c, int k, long long *out)
{
    if (xtype == 4 && ctype == 4)
        nearest_f_f(x, n, d, c, k, out);
    else if (xtype == 8 && ctype == 8)
        nearest_d_d(x, n, d, c, k, out);
    else if (xtype == 4 && ctype == 8)
        nearest_fd(x, n, d, c, k, out);
}

/* -> centres (k' x d, the data's type; k' = min(k, n) is returned) and labels; wts NULL = ones */
MX_API int mx_kmeans(int xtype, const void *x, long long n, int d, const double *wts, int k, int iters, mx_rng *r,
                     void *centres, long long *labels)
{
    return xtype == 4 ? kmeans_f(x, n, d, wts, k, iters, r, centres, labels)
                      : kmeans_d(x, n, d, wts, k, iters, r, centres, labels);
}
