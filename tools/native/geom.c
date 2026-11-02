/* Geometry helpers of the map export, in C (ctypes; numpy fallbacks give the same results).

   Depth rasteriser for tools/dsr_light.py ShadowMap (the static sun shadow bake) and its 3x3 PCF lookup:

   shadow_raster: the numpy path of ShadowMap.__init__ in C, operation for operation in float32 (built with
   -ffp-contract=off, no fast math), so the depth map is the same to the bit: every triangle tests the pixel centres of
   its s x s box (s = the power of two its bounding box size falls under, as the numpy batches do), barycentric
   weights with the same -1e-4 tolerance, depth = w0 z0 + w1 z1 + w2 z2, max into D.
   shadow_lit: ShadowMap.lit for many points, given their light-space coordinates (computed in numpy).

   nearest_d2: squared distance from each query point to the nearest sample point (k-d tree), for
   tools/export_map.py view_distance.

   Only malloc / free / qsort from libc: builds unchanged as a Pyodide side module (tools/native/build-wasm.sh). */
#include <stdlib.h>
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
