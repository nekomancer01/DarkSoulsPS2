/* Geometry helpers of the map export, in C (ctypes; numpy fallbacks give the same results).

   Depth rasteriser for tools/dsr_light.py ShadowMap (the static sun shadow bake) and its 3x3 PCF lookup:

   shadow_raster: the numpy path of ShadowMap.__init__ in C, operation for operation in float32 (built with
   -ffp-contract=off, no fast math), so the depth map is the same to the bit: every triangle tests the pixel centres of
   its s x s box (s = the power of two its bounding box size falls under, as the numpy batches do), barycentric
   weights with the same -1e-4 tolerance, depth = w0 z0 + w1 z1 + w2 z2, max into D.
   shadow_lit: ShadowMap.lit for many points, given their light-space coordinates (computed in numpy).

   nearest_rgb: nearest palette entry for tools/texmap.py.
   nearest_d2: squared distance from each query point to the nearest sample point (k-d tree), for
   tools/export_map.py view_distance.
   soft_targets: tools/export_map.py soften_thin's footprint colours (numpy's summation order kept).

   Only malloc / free / qsort from libc: builds unchanged as a Pyodide side module (tools/native/build-wasm.sh). */
#include <stdlib.h>
/* out of memory (the in-browser converter's WebAssembly heap has a cap): trap ("RuntimeError: unreachable" in
 * Pyodide, SIGILL natively) instead of writing through NULL, which in WebAssembly is address 0 and corrupts the
 * interpreter's memory (Pyodide then fails later with "Unexpected rtype undefined") */
static void *alloc_ok(void *p, size_t n) {
    if (!p && n)
        __builtin_trap();
    return p;
}
#define malloc(n) alloc_ok(malloc(n), (n))
#define calloc(a, b) alloc_ok(calloc(a, b), (a) * (b))
#define realloc(p, n) alloc_ok(realloc(p, n), (n))
/* P: n x 3 x 2 texel coordinates, Z: n x 3 depths, den: n, x0 / y0: box corner, s: box side (power of two, 0 = skip).
   D: H x W, filled with -1e9 by the caller. */
void shadow_raster(int n, const float *P, const float *Z, const float *den, const int *x0, const int *y0,
                   const int *s, int W, int H, float *D)
{
    for (int i = 0; i < n; i++) {
        int side = s[i];
        if (side <= 0)
            continue;
        const float *a = P + 6 * i, *b = a + 2, *c = a + 4;
        float e0 = b[1] - c[1], e1 = c[0] - b[0], f0 = c[1] - a[1], f1 = a[0] - c[0], dn = den[i];
        float za = Z[3 * i], zb = Z[3 * i + 1], zc = Z[3 * i + 2];
        for (int oy = 0; oy < side; oy++) {
            int Y = y0[i] + oy;
            if (Y >= H)
                break;
            float py = (float)Y + 0.5f, dy = py - c[1];
            float *row = D + (long long)Y * W;
            for (int ox = 0; ox < side; ox++) {
                int X = x0[i] + ox;
                if (X >= W)
                    break;
                float px = (float)X + 0.5f, dx = px - c[0];
                float w0 = (e0 * dx + e1 * dy) / dn;
                float w1 = (f0 * dx + f1 * dy) / dn;
                float w2 = 1.0f - w0 - w1;
                if (w0 >= -1e-4f && w1 >= -1e-4f && w2 >= -1e-4f) {
                    float z = w0 * za + w1 * zb + w2 * zc;
                    if (z > row[X])
                        row[X] = z;
                }
            }
        }
    }
}

/* Q: n x 3 light-space points (texel x, texel y, depth); out[i] = 1 - (texels of the 3x3 around (int x, int y) whose
   depth > depth + bias) / 9 */
void shadow_lit(int n, const double *Q, const float *D, int W, int H, double bias, double *out)
{
    for (int i = 0; i < n; i++) {
        double x = Q[3 * i], y = Q[3 * i + 1], d = Q[3 * i + 2] + bias;
        int xi = (int)x, yi = (int)y, tot = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int X = xi + dx, Y = yi + dy;
                if (X >= 0 && X < W && Y >= 0 && Y < H && (double)D[(long long)Y * W + X] > d)
                    tot++;
            }
        out[i] = 1.0 - tot / 9.0;
    }
}

/* ---- nearest sample point: k-d tree over S (n x 3), implicit in the reordered array ---- */
static int kd_axis;

static int kd_cmp(const void *a, const void *b)
{
    double x = ((const double *)a)[kd_axis], y = ((const double *)b)[kd_axis];
    return x < y ? -1 : x > y;
}

#define KD_LEAF 8

static void kd_build(double *S, unsigned char *ax, int lo, int hi)
{
    if (hi - lo <= KD_LEAF)
        return;
    double mn[3], mx[3];
    for (int k = 0; k < 3; k++)
        mn[k] = mx[k] = S[3 * lo + k];
    for (int i = lo + 1; i < hi; i++)
        for (int k = 0; k < 3; k++) {
            double v = S[3 * i + k];
            if (v < mn[k]) mn[k] = v;
            if (v > mx[k]) mx[k] = v;
        }
    int a = 0;
    for (int k = 1; k < 3; k++)
        if (mx[k] - mn[k] > mx[a] - mn[a])
            a = k;
    kd_axis = a;
    qsort(S + 3 * lo, hi - lo, 3 * sizeof(double), kd_cmp);
    int m = (lo + hi) / 2;
    ax[m] = (unsigned char)a;
    kd_build(S, ax, lo, m);
    kd_build(S, ax, m + 1, hi);
}

static double kd_d2(const double *s, const double *p)
{
    double dx = p[0] - s[0], dy = p[1] - s[1], dz = p[2] - s[2];
    return dx * dx + dy * dy + dz * dz;
}

/* rd: squared distance from p to the node's cell (incremental, Arya & Mount), off: p's offset from the cell per axis.
   On the lattices view_distance uses every term is exact, so pruning (rd >= best) never drops a nearer point. */
static void kd_query(const double *S, const unsigned char *ax, int lo, int hi, const double *p, double *best, int *bi,
                     double rd, double *off)
{
    if (hi - lo <= KD_LEAF) {
        for (int i = lo; i < hi; i++) {
            double d = kd_d2(S + 3 * i, p);
            if (d < *best) *best = d, *bi = i;
        }
        return;
    }
    int m = (lo + hi) / 2, a = ax[m];
    double d = p[a] - S[3 * m + a], dm = kd_d2(S + 3 * m, p);
    if (dm < *best) *best = dm, *bi = m;
    int nlo = d < 0 ? lo : m + 1, nhi = d < 0 ? m : hi, flo = d < 0 ? m + 1 : lo, fhi = d < 0 ? hi : m;
    kd_query(S, ax, nlo, nhi, p, best, bi, rd, off);
    double old = off[a], rd2 = rd - old * old + d * d;
    if (rd2 < *best) {
        off[a] = d;
        kd_query(S, ax, flo, fhi, p, best, bi, rd2, off);
        off[a] = old;
    }
}

/* out[i] = min over samples of (dx^2 + dy^2 + dz^2) for query Q[i]; S (ns x 3) is reordered in place. Returns -1 when
   out of memory, else 0. Each search starts from the previous query's nearest sample (a real distance, so the minimum is
   unchanged): queries in spatial order (np.unique's sorted cells) prune most of the tree at once. */
int nearest_d2(int nq, const double *Q, int ns, double *S, double *out)
{
    unsigned char *ax = calloc(ns > 0 ? ns : 1, 1);
    if (!ax)
        return -1;
    kd_build(S, ax, 0, ns);
    int bi = -1;
    for (int i = 0; i < nq; i++) {
        double best = bi >= 0 ? kd_d2(S + 3 * bi, Q + 3 * i) : 1e300, off[3] = {0.0, 0.0, 0.0};
        kd_query(S, ax, 0, ns, Q + 3 * i, &best, &bi, 0.0, off);
        out[i] = best;
    }
    free(ax);
    return 0;
}

/* ---- nearest palette entry (tools/texmap.py _nearest): X (n x c), P (k x c) integer colours, c channels (3 or 4);
   out[i] = the first entry with the smallest squared distance, as numpy's argmin ---- */
void nearest_rgb(int n, const int *X, int k, const int *P, int c, long long *out)
{
    for (int i = 0; i < n; i++) {
        const int *x = X + (long long)c * i;
        long long bi = 0, best = -1;
        for (int j = 0; j < k; j++) {
            const int *p = P + (long long)c * j;
            long long d = 0;
            for (int q = 0; q < c; q++)
                d += (long long)(x[q] - p[q]) * (x[q] - p[q]);
            if (best < 0 || d < best)
                best = d, bi = j;
        }
        out[i] = bi;
    }
}

/* ---- soften_thin's footprint colours (tools/export_map.py): for each query q (centre Cq[q], radius Rq[q]) the
   wt-weighted mean of col over the triangles whose centroid cen lies within the radius, searched in the 2 m grid
   cells (side G) the radius touches, x-major as the Python loops; cells given sorted (ckey, ncell x 3) with their
   members (cidx[cstart[k] .. cstart[k + 1]], ascending). The sums repeat numpy's: the squared distance
   (dx^2 + dy^2) + dz^2, wt[idx].sum() as numpy's pairwise sum, (col[idx] * wt[idx]).sum(0) as a running sum per
   channel. out[q] = the mean and ok[q] = 1 when there are members with a positive weight sum, else ok[q] = 0. Returns
   -1 when out of memory. ---- */
static double fl(double x)  /* floor() without libm (|x| < 2^62) */
{
    double f = (double)(long long)x;
    return f > x ? f - 1.0 : f;
}

static double np_pairwise(const double *a, long long n)  /* numpy's pairwise_sum (umath loops) */
{
    if (n < 8) {
        double res = -0.0;
        for (long long i = 0; i < n; i++)
            res += a[i];
        return res;
    }
    if (n <= 128) {
        double r[8], res;
        long long i;
        for (int j = 0; j < 8; j++)
            r[j] = a[j];
        for (i = 8; i < n - (n % 8); i += 8)
            for (int j = 0; j < 8; j++)
                r[j] += a[i + j];
        res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; i++)
            res += a[i];
        return res;
    }
    long long n2 = n / 2;
    n2 -= n2 % 8;
    return np_pairwise(a, n2) + np_pairwise(a + n2, n - n2);
}

static long long cell_find(const long long *ckey, int ncell, long long x, long long y, long long z)
{
    int lo = 0, hi = ncell;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        const long long *k = ckey + 3 * (long long)m;
        int lt = k[0] != x ? k[0] < x : k[1] != y ? k[1] < y : k[2] < z;
        if (lt)
            lo = m + 1;
        else
            hi = m;
    }
    if (lo < ncell && ckey[3 * (long long)lo] == x && ckey[3 * (long long)lo + 1] == y && ckey[3 * (long long)lo + 2] == z)
        return lo;
    return -1;
}

int soft_targets(int nq, const double *Cq, const double *Rq, int n, const double *cen, const double *col,
                 const double *wt, int ncell, const long long *ckey, const long long *cstart, const long long *cidx,
                 double G, double *out, unsigned char *ok)
{
    long long *idx = malloc(sizeof(long long) * (n > 0 ? n : 1));
    double *w = malloc(sizeof(double) * (n > 0 ? n : 1));
    if (!idx || !w) {
        free(idx);
        free(w);
        return -1;
    }
    for (int q = 0; q < nq; q++) {
        const double *c = Cq + 3 * (long long)q;
        double r = Rq[q], rr = r * r;
        long long lo[3], hi[3], m = 0;
        for (int a = 0; a < 3; a++) {
            lo[a] = (long long)fl((c[a] - r) / G);
            hi[a] = (long long)fl((c[a] + r) / G);
        }
        for (long long x = lo[0]; x <= hi[0]; x++)
            for (long long y = lo[1]; y <= hi[1]; y++)
                for (long long z = lo[2]; z <= hi[2]; z++) {
                    long long k = cell_find(ckey, ncell, x, y, z);
                    if (k < 0)
                        continue;
                    for (long long s = cstart[k]; s < cstart[k + 1]; s++) {
                        long long i = cidx[s];
                        double dx = cen[3 * i] - c[0], dy = cen[3 * i + 1] - c[1], dz = cen[3 * i + 2] - c[2];
                        if ((dx * dx + dy * dy) + dz * dz <= rr)
                            idx[m++] = i;
                    }
                }
        ok[q] = 0;
        if (!m)
            continue;
        for (long long s = 0; s < m; s++)
            w[s] = wt[idx[s]];
        double ws = np_pairwise(w, m);
        if (!(ws > 0))
            continue;
        for (int j = 0; j < 3; j++) {
            double acc = col[3 * idx[0] + j] * w[0];
            for (long long s = 1; s < m; s++)
                acc += col[3 * idx[s] + j] * w[s];
            out[3 * (long long)q + j] = acc / ws;
        }
        ok[q] = 1;
    }
    free(idx);
    free(w);
    return 0;
}
