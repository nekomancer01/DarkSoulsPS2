/* export_normal.py (normal-map relief) in C: the per-triangle frames and areas, the lighting response at
   tangent-space normals (_Response: DSR's light x shadow through the tone map, as a ratio to the flat normal) and
   the BC5 normal-map decode + power-of-two box average (xy_pow2). Same values as the numpy code: float32 where numpy
   keeps float32 (the BC5 path), row sums as numpy reduces them, vector norms as np.linalg.norm computes them
   (dot_fma: x.dot(x) is a BLAS ddot, fused multiply-adds natively; a plain loop without BLAS, in the browser). */
#include "mapx.h"

double log(double);

static double dot3(const double *v, int fma_)
{
#ifndef __wasm__  /* Pyodide's numpy has no BLAS: always the plain loop there */
    if (fma_)
        return __builtin_fma(v[2], v[2], __builtin_fma(v[1], v[1], v[0] * v[0]));
#endif
    return ((0.0 + v[0] * v[0]) + v[1] * v[1]) + v[2] * v[2];
}

static double np_maximum(double a, double b) { return a >= b || a != a ? a : b; }  /* np.maximum(a, b) */
static double red3(double a, double b, double c) { return ((-0.0 + a) + b) + c; }  /* add.reduce over a row of 3 */

static void cross3(const double *a, const double *b, double *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

/* x.dot(x) of a 3-vector both ways, for the caller to find which one this numpy does */
MX_API double mx_dot3(const double *v, int dot_fma) { return dot3(v, dot_fma); }

/* assign's per-triangle values: area = 0.5 |(p1 - p0) x (p2 - p0)| and Nn = N / (|N| + 1e-12) (np.linalg.norm of
   one row: dot), then _frames: N, T unit by the row norm, B = w (N x T) with w = sign of W (negative: -1) */
MX_API void mx_bump_frames(long long nt, const double *PS, double *N, double *T, const double *W, int dot_fma,
                           double *area, double *Nn, double *B)
{
    for (long long i = 0; i < nt; i++) {
        const double *p = PS + 9 * i;
        double a[3] = {p[3] - p[0], p[4] - p[1], p[5] - p[2]}, b[3] = {p[6] - p[0], p[7] - p[1], p[8] - p[2]}, c[3];
        cross3(a, b, c);
        area[i] = 0.5 * __builtin_sqrt(dot3(c, dot_fma));
        double *n = N + 3 * i, *t = T + 3 * i;
        double ln = __builtin_sqrt(dot3(n, dot_fma)) + 1e-12;
        for (int k = 0; k < 3; k++)
            Nn[3 * i + k] = n[k] / ln;
        double rn = __builtin_sqrt(red3(n[0] * n[0], n[1] * n[1], n[2] * n[2])) + 1e-12;
        double rt = __builtin_sqrt(red3(t[0] * t[0], t[1] * t[1], t[2] * t[2])) + 1e-12;
        for (int k = 0; k < 3; k++)
            n[k] = n[k] / rn, t[k] = t[k] / rt;
        double w = W[i] < 0 ? -1.0 : 1.0, x[3];
        cross3(n, t, x);
        for (int k = 0; k < 3; k++)
            B[3 * i + k] = w * x[k];
    }
}

/* ---- _Response ---- */
typedef struct {
    int ndir;
    double dir[3][3], col[3][3], up[3], down[3], env[3];
} mx_light;

static const int FACE[6][4] = {{2, -1, 1, -1}, {2, 1, 1, -1}, {0, 1, 2, 1}, {0, 1, 2, -1}, {0, 1, 1, -1}, {0, -1, 1, -1}};

static void probe(const double *faces, const double *n, double *e)  /* export_normal._probe, PROBE_RES 8 */
{
    const int R = 8;
    double an[3] = {mx_fabs(n[0]), mx_fabs(n[1]), mx_fabs(n[2])};
    int ax = 0;
    for (int k = 1; k < 3; k++)
        if (an[k] > an[ax])
            ax = k;
    double m = an[ax] > 0 ? an[ax] : 1.0;
    int f = (n[ax] > 0 ? 0 : 1) + 2 * ax;
    int sa = FACE[f][0], ss = FACE[f][1], ta = FACE[f][2], ts = FACE[f][3];
    long long u = (long long)((ss * n[sa] / m + 1) / 2 * R), v = (long long)((ts * n[ta] / m + 1) / 2 * R);
    if (u > R - 1)
        u = R - 1;
    if (v > R - 1)
        v = R - 1;
    const double *px = faces + (((long long)f * R + v) * R + u) * 3;
    e[0] = px[0], e[1] = px[1], e[2] = px[2];
}

static double interp(double x, const double *xp, const double *fp, int n)  /* np.interp */
{
    if (x != x)
        return x;
    if (x > xp[n - 1])
        return fp[n - 1];
    if (x < xp[0])
        return fp[0];
    int lo = 0, hi = n - 1;  /* largest j with xp[j] <= x */
    while (lo < hi) {
        int m = (lo + hi + 1) / 2;
        if (xp[m] <= x)
            lo = m;
        else
            hi = m - 1;
    }
    int j = lo;
    if (j == n - 1 || xp[j] == x)
        return fp[j];
    double slope = (fp[j + 1] - fp[j]) / (xp[j + 1] - xp[j]);
    double r = slope * (x - xp[j]) + fp[j];
    if (r != r) {
        r = slope * (x - xp[j + 1]) + fp[j + 1];
        if (r != r && fp[j] == fp[j + 1])
            r = fp[j];
    }
    return r;
}

typedef struct {
    const long long *rows;
    long long n;
    const double *N, *T, *B, *k, *alb;
    const int *lid, *pslot;
    const mx_light *lights;
    const double *faces;
    int J;  /* tangent normals (not counting the flat one) */
    const double *xy;
    double near[3], sa;
    int nly;
    const double *ly, *lout;
    double eps;
    double *out;
} resp_ctx;

static double resp_one(const resp_ctx *c, long long i, double x, double y)
{
    double z = (__builtin_sqrt(np_maximum(0.0, (1 - x * x) - y * y)) - 1) + 1;
    const double *N = c->N + 3 * i, *T = c->T + 3 * i, *B = c->B + 3 * i;
    double n[3];
    for (int a = 0; a < 3; a++)
        n[a] = (N[a] * z + B[a] * x) + T[a] * y;
    double ln = __builtin_sqrt(red3(n[0] * n[0], n[1] * n[1], n[2] * n[2])) + 1e-12;
    for (int a = 0; a < 3; a++)
        n[a] = n[a] / ln;
    const mx_light *L = c->lights + c->lid[i];
    double h = n[1] * 0.5 + 0.5, e[3] = {0.0, 0.0, 0.0};
    if (c->pslot[i] >= 0)
        probe(c->faces + (long long)c->pslot[i] * 6 * 8 * 8 * 3, n, e);
    double amb[3], sun[3] = {0.0, 0.0, 0.0};
    for (int a = 0; a < 3; a++)
        amb[a] = (L->down[a] + (L->up[a] - L->down[a]) * h) + L->env[a] * e[a];
    for (int d = 0; d < L->ndir; d++) {
        double m = (n[0] * L->dir[d][0] + n[1] * L->dir[d][1]) + n[2] * L->dir[d][2];
        m = np_maximum(0.0, m);
        for (int a = 0; a < 3; a++)
            sun[a] = sun[a] + m * L->col[d][a];
    }
    double k = c->k[i], t[3];
    for (int a = 0; a < 3; a++) {
        double ea = amb[a] * (1 - c->sa * (1 - k)) + sun[a] * k;
        t[a] = (ea * c->alb[3 * i + a]) * c->near[a];
    }
    double Y = (t[0] * 0.299 + t[1] * 0.587) + t[2] * 0.114;
    return interp(log(np_maximum(Y, 1e-6)), c->ly, c->lout, c->nly) + c->eps;
}

#define RESP_BLOCK 256
static void resp_block(void *vc, long long b)
{
    const resp_ctx *c = vc;
    long long e = (b + 1) * RESP_BLOCK < c->n ? (b + 1) * RESP_BLOCK : c->n;
    for (long long r = b * RESP_BLOCK; r < e; r++) {
        long long i = c->rows ? c->rows[r] : r;
        double flat = resp_one(c, i, 0.0, 0.0);
        for (int j = 0; j < c->J; j++) {
            double v = resp_one(c, i, c->xy[2 * j], c->xy[2 * j + 1]) / flat, hi = 255.0 / 128;
            v = v < 0 ? 0 : v;  /* np.clip: nan stays */
            v = v > hi ? hi : v;
            c->out[r * c->J + j] = v;
        }
    }
}

/* _Response.__call__ for the triangles rows[0..n) (NULL: 0..n) at the J tangent-space normals xy -> out (n x J).
   lights: per light id {ndir, dir, col, up, down, env} as 3 + 9 + 9 + 9 doubles (ndir as a double); faces: probe
   cubes (6 x 8 x 8 x 3) by slot (pslot -1: none); ly / lout: the tone table (_Response.ly / out). */
MX_API void mx_bump_response(long long n, const long long *rows, const double *N, const double *T, const double *B,
                             const double *k, const double *alb, const int *lid, const int *pslot, int nlight,
                             const double *light_tab, const double *faces, int J, const double *xy, const double *near,
                             double sa, int nly, const double *ly, const double *lout, double eps, double *out)
{
    mx_light *L = MX_NEW(mx_light, nlight > 0 ? nlight : 1);
    for (int l = 0; l < nlight; l++) {
        const double *s = light_tab + 31 * l;
        L[l].ndir = (int)s[0];
        for (int d = 0; d < 3; d++)
            for (int a = 0; a < 3; a++)
                L[l].dir[d][a] = s[1 + 3 * d + a], L[l].col[d][a] = s[10 + 3 * d + a];
        for (int a = 0; a < 3; a++)
            L[l].up[a] = s[19 + a], L[l].down[a] = s[22 + a], L[l].env[a] = s[25 + a];
    }
    resp_ctx c = {rows, n, N, T, B, k, alb, lid, pslot, L, faces, J, xy, {near[0], near[1], near[2]}, sa, nly, ly,
                  lout, eps, out};
    mx_parallel_for((n + RESP_BLOCK - 1) / RESP_BLOCK, resp_block, &c);
    free(L);
}

/* ---- BC5 normal maps ---- */
static void bc4_block(const uint8_t *b, float *out16)
{
    float a0 = b[0], a1 = b[1], pal[8];
    pal[0] = a0, pal[1] = a1;
    if (a0 > a1)
        for (int i = 1; i < 7; i++)
            pal[1 + i] = ((float)(7 - i) * a0 + (float)i * a1) / 7.0f;
    else {
        for (int i = 1; i < 5; i++)
            pal[1 + i] = ((float)(5 - i) * a0 + (float)i * a1) / 5.0f;
        pal[6] = 0, pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; k++)
        bits |= (uint64_t)b[2 + k] << (8 * k);
    for (int t = 0; t < 16; t++)
        out16[t] = pal[(bits >> (3 * t)) & 7] / 255.0f;
}

/* xy_pow2 for a BC5 level (data at off, w x h texels) -> out (th x tw x 2 float32, th / tw the powers of two <= cap
   that _pow2_index picks; out sized by the caller for cap x cap x 2) */
MX_API int mx_bc5_pow2(const uint8_t *d, long long off, int w, int h, int cap, float *out, int *th_out, int *tw_out)
{
    int tw = 1, th = 1;
    while (tw * 2 <= (w < cap ? w : cap))
        tw *= 2;
    while (th * 2 <= (h < cap ? h : cap))
        th *= 2;
    int bw = (w + 3) / 4 > 1 ? (w + 3) / 4 : 1, bh = (h + 3) / 4 > 1 ? (h + 3) / 4 : 1;
    float *img = MX_NEW(float, (size_t)bh * 4 * bw * 4 * 2), blk[16];
    long long W4 = (long long)bw * 4;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const uint8_t *b = d + off + ((long long)by * bw + bx) * 16;
            for (int ch = 0; ch < 2; ch++) {
                bc4_block(b + 8 * ch, blk);
                for (int t = 0; t < 16; t++)
                    img[((by * 4 + t / 4) * W4 + bx * 4 + t % 4) * 2 + ch] = blk[t] * 2 - 1;
            }
        }
    int ny = h / th > 1 ? h / th : 1, nx = w / tw > 1 ? w / tw : 1;
    float cnt = (float)(ny * nx);
    for (int ty = 0; ty < th; ty++)
        for (int tx = 0; tx < tw; tx++)
            for (int ch = 0; ch < 2; ch++) {
                float s = 0;
                for (int i = 0; i < ny; i++) {
                    long long y = (long long)ty * h / th + i;
                    y = y < h - 1 ? y : h - 1;
                    for (int j = 0; j < nx; j++) {
                        long long x = (long long)tx * w / tw + j;
                        x = x < w - 1 ? x : w - 1;
                        float v = img[(y * W4 + x) * 2 + ch];
                        s = (i || j) ? s + v : v;
                    }
                }
                out[((long long)ty * tw + tx) * 2 + ch] = s / cnt;
            }
    free(img);
    *th_out = th, *tw_out = tw;
    return 0;
}
