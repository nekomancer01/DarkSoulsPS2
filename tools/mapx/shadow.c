/* the sun shadow map (dsr_light.ShadowMap): light-space transform, texel boxes and depth raster of the occluders in
   one call, and lit_many's light-space transform + 3x3 PCF. Every value as the numpy code (float32 for the raster:
   tris as float32, matmul with B.T as float32 summed left to right, texel coordinates, boxes, den) and
   tools/native/geom.c's shadow_raster for the pixels. The raster runs in row bands on threads: a depth texel is the
   max of the triangles covering it, the same in any order. */
#include "mapx.h"

typedef struct {
    long long n;
    const float *P, *Z, *den;
    const int *x0, *y0, *s;
    int W, H, band;
    float *D;
} rctx;

static void raster_band(void *vc, long long b)
{
    rctx *c = vc;
    int ylo = (int)b * c->band, yhi = ylo + c->band < c->H ? ylo + c->band : c->H;
    for (long long i = 0; i < c->n; i++) {
        int side = c->s[i];
        if (side <= 0)
            continue;
        int Y0 = c->y0[i], Y1 = Y0 + side;  /* rows Y0 .. min(Y1, H) - 1, as shadow_raster */
        if (Y1 > yhi)
            Y1 = yhi;
        if (Y0 < ylo)
            Y0 = ylo;
        if (Y0 >= Y1)
            continue;
        const float *a = c->P + 6 * i, *bb = a + 2, *cc = a + 4;
        float e0 = bb[1] - cc[1], e1 = cc[0] - bb[0], f0 = cc[1] - a[1], f1 = a[0] - cc[0], dn = c->den[i];
        float za = c->Z[3 * i], zb = c->Z[3 * i + 1], zc = c->Z[3 * i + 2];
        for (int Y = Y0; Y < Y1; Y++) {
            float py = (float)Y + 0.5f, dy = py - cc[1];
            float *row = c->D + (long long)Y * c->W;
            for (int ox = 0; ox < side; ox++) {
                int X = c->x0[i] + ox;
                if (X >= c->W)
                    break;
                float px = (float)X + 0.5f, dx = px - cc[0];
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

static float min3f(float a, float b, float c)  /* np.min over 3 float32 (finite) */
{
    float m = a < b ? a : b;
    return m < c ? m : c;
}
static float max3f(float a, float b, float c)
{
    float m = a > b ? a : b;
    return m > c ? m : c;
}
static long long clipi(double v, long long hi)  /* np.clip(v, 0, hi).astype(int64) */
{
    return v < 0 ? 0 : v > (double)hi ? hi : (long long)v;
}

/* tris: n x 3 x 3 world corners (float64); B: 3 x 3 rows u, v, L; lo: 2 (float64); D: H x W, filled here */
MX_API void mx_shadow_map(long long n, const double *tris, const double *B, const double *lo, double texel, int W, int H,
                          float *D)
{
    float M[3][3], lof[2] = {(float)lo[0], (float)lo[1]}, tx = (float)texel;
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < 3; j++)
            M[k][j] = (float)B[3 * j + k];  /* B.T as float32 */
    float *P = MX_NEW(float, 6 * n), *Z = MX_NEW(float, 3 * n), *den = MX_NEW(float, n);
    int *x0 = MX_NEW(int, n), *y0 = MX_NEW(int, n), *s = MX_NEW(int, n);
    for (long long i = 0; i < n; i++) {
        float *p = P + 6 * i;
        for (int v = 0; v < 3; v++) {
            const double *t = tris + 9 * i + 3 * v;
            float X0 = (float)t[0], X1 = (float)t[1], X2 = (float)t[2], T[3];
            for (int j = 0; j < 3; j++)
                T[j] = (X0 * M[0][j] + X1 * M[1][j]) + X2 * M[2][j];
            p[2 * v] = (T[0] - lof[0]) / tx;
            p[2 * v + 1] = (T[1] - lof[1]) / tx;
            Z[3 * i + v] = T[2];
        }
        float mnx = min3f(p[0], p[2], p[4]), mny = min3f(p[1], p[3], p[5]);
        float mxx = max3f(p[0], p[2], p[4]), mxy = max3f(p[1], p[3], p[5]);
        long long a0 = clipi(__builtin_floorf(mnx), W - 1), b0 = clipi(__builtin_floorf(mny), H - 1);
        long long a1 = clipi(__builtin_ceilf(mxx), W - 1), b1 = clipi(__builtin_ceilf(mxy), H - 1);
        int inside = (double)mxx >= 0 && (double)mxy >= 0 && (double)mnx < (double)W && (double)mny < (double)H;
        const float *a = p, *b = p + 2, *c = p + 4;
        float dn = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
        den[i] = dn;
        long long sw = a1 - a0 + 1, sh = b1 - b0 + 1, size = sw > sh ? sw : sh, side = 1;
        while (side < size)
            side *= 2;
        s[i] = inside && __builtin_fabsf(dn) > 1e-9f ? (int)side : 0;
        x0[i] = (int)a0, y0[i] = (int)b0;
    }
    for (long long i = 0; i < (long long)W * H; i++)
        D[i] = -1e9f;
    int th = mx_threads(), band = th > 1 ? (H + 4 * th - 1) / (4 * th) : H;  /* one band without threads */
    if (band < 8)
        band = 8;
    rctx c = {n, P, Z, den, x0, y0, s, W, H, band, D};
    mx_parallel_for((H + band - 1) / band, raster_band, &c);
    free(P), free(Z), free(den), free(x0), free(y0), free(s);
}

/* lit_many: q = (P + 0.1 N) @ B.T (summed left to right), texel coordinates (q - lo) / texel, then geom.c's
   shadow_lit (3x3 texels deeper than depth + bias) */
MX_API void mx_shadow_lit(long long n, const double *Pt, const double *N, const double *B, const double *lo, double texel,
                          const float *D, int W, int H, double bias, double *out)
{
    for (long long i = 0; i < n; i++) {
        double x[3], q[3];
        for (int a = 0; a < 3; a++)
            x[a] = Pt[3 * i + a] + 0.1 * N[3 * i + a];
        for (int j = 0; j < 3; j++)
            q[j] = (x[0] * B[3 * j] + x[1] * B[3 * j + 1]) + x[2] * B[3 * j + 2];
        double qx = (q[0] - lo[0]) / texel, qy = (q[1] - lo[1]) / texel, d = q[2] + bias;
        int xi = (int)qx, yi = (int)qy, tot = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int X = xi + dx, Y = yi + dy;
                if (X >= 0 && X < W && Y >= 0 && Y < H && (double)D[(long long)Y * W + X] > d)
                    tot++;
            }
        out[i] = 1.0 - tot / 9.0;
    }
}
