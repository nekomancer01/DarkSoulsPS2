/* export_map.soften_thin in C (render-12): the projected width of every coarse / backdrop triangle seen from the
   navmesh samples, the footprint colour average around the thin ones, and the per-wedge pull toward it. Same values
   as the numpy code: numpy's row sums (pairwise), np.cross, the masked median (mean of the two middle values) and
   nanmean, np.add.at in triangle order per corner. Triangles are independent until the per-wedge sums, which run per
   entry in triangle order, so the threads change nothing. */
#include "mapx.h"

typedef struct {
    const double *P;  /* ntri x 3 corners x 3 */
    const double *S;  /* ns x 3 */
    int ns;
    double floor_, soft_focal, soft_reach, cam_reach;
    double *area2, *px, *mpp, *seen, *cen;
    long long ntri;
} soft_ctx;

static double nrm3(double x, double y, double z) { return mx_sqrt(((-0.0 + x * x) + y * y) + z * z); }

static void cross3(const double *a, const double *b, double *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median_sorted(double *v, int n)  /* numpy.ma.median of n unmasked values: (low + high) / 2 */
{
    qsort(v, (size_t)n, sizeof(double), cmp_d);
    int h = n / 2, l = n % 2 ? h : h - 1;
    return (v[l] + v[h]) / 2.;
}

#define SOFT_BLOCK 256
static void soft_rows(void *vc, long long blk)
{
    soft_ctx *c = vc;
    int ns = c->ns;
    double *d = MX_NEW(double, 4 * (size_t)ns), *cs = d + ns, *wv = cs + ns, *dv = wv + ns;
    long long i1 = (blk + 1) * SOFT_BLOCK < c->ntri ? (blk + 1) * SOFT_BLOCK : c->ntri;
    for (long long i = blk * SOFT_BLOCK; i < i1; i++) {
        const double *p = c->P + 9 * i;
        double E[3][3], N[3], m[3];
        for (int a = 0; a < 3; a++) {
            E[0][a] = p[3 + a] - p[a];
            E[1][a] = p[6 + a] - p[3 + a];
            E[2][a] = p[a] - p[6 + a];
        }
        double ne2[3] = {-E[2][0], -E[2][1], -E[2][2]};
        cross3(E[0], ne2, N);
        double a2 = nrm3(N[0], N[1], N[2]);
        double dn = a2 > 1e-12 ? a2 : 1e-12;  /* np.maximum(area2, 1e-12) */
        for (int a = 0; a < 3; a++)
            N[a] /= dn;
        double el = nrm3(E[0][0], E[0][1], E[0][2]), e1 = nrm3(E[1][0], E[1][1], E[1][2]), e2 = nrm3(E[2][0], E[2][1], E[2][2]);
        el = e1 > el ? e1 : el;
        el = e2 > el ? e2 : el;
        double height = a2 / (el > 1e-6 ? el : 1e-6);
        for (int a = 0; a < 3; a++)
            m[a] = ((p[a] + p[3 + a]) + p[6 + a]) / 3;
        double dmin = 0;
        for (int s = 0; s < ns; s++) {
            double V[3] = {c->S[3 * s] - m[0], c->S[3 * s + 1] - m[1], c->S[3 * s + 2] - m[2]};
            d[s] = nrm3(V[0], V[1], V[2]);
            double dot = ((-0.0 + V[0] * N[0]) + V[1] * N[1]) + V[2] * N[2];
            cs[s] = mx_fabs(dot) / (d[s] > 1e-6 ? d[s] : 1e-6);
            if (!s || d[s] < dmin || d[s] != d[s])
                dmin = d[s];
        }
        int nk = 0;
        for (int s = 0; s < ns; s++) {
            if (d[s] > dmin + c->soft_reach) {
                cs[s] = 0.0;  /* nan, then 0 in nanmean's sum */
                continue;
            }
            double x = d[s] - c->cam_reach, dd = x > c->floor_ || x != x ? x : c->floor_;
            wv[nk] = height * cs[s] * c->soft_focal / dd;
            dv[nk++] = dd;
        }
        c->px[i] = median_sorted(wv, nk);
        c->mpp[i] = median_sorted(dv, nk) / c->soft_focal;
        c->seen[i] = mx_np_pairwise(cs, ns, 1) / nk;
        c->area2[i] = a2;
        for (int a = 0; a < 3; a++)
            c->cen[3 * i + a] = m[a];
    }
    free(d);
}

typedef struct {
    const double *cen, *col, *wt;
    const long long *ckey, *cstart, *cidx;
    long long ncell;
    double G, r_px;
    const double *mpp;
    const long long *q;
    long long nq;
    double *tgt;  /* ntri x 3, preset to the triangle colours */
} tgt_ctx;

static long long cell_find(const long long *ckey, long long ncell, long long x, long long y, long long z)
{
    long long lo = 0, hi = ncell;
    while (lo < hi) {
        long long m = (lo + hi) / 2;
        const long long *k = ckey + 3 * m;
        int lt = k[0] != x ? k[0] < x : k[1] != y ? k[1] < y : k[2] < z;
        if (lt)
            lo = m + 1;
        else
            hi = m;
    }
    if (lo < ncell && ckey[3 * lo] == x && ckey[3 * lo + 1] == y && ckey[3 * lo + 2] == z)
        return lo;
    return -1;
}

#define TGT_BLOCK 128
static void soft_tgt(void *vc, long long blk)  /* geom.c soft_targets, as soften_thin's C path ran it */
{
    tgt_ctx *t = vc;
    long long cap = 1024, *idx = MX_NEW(long long, cap);
    double *w = MX_NEW(double, cap);
    long long e = (blk + 1) * TGT_BLOCK < t->nq ? (blk + 1) * TGT_BLOCK : t->nq;
    for (long long qq = blk * TGT_BLOCK; qq < e; qq++) {
        long long i = t->q[qq];
        const double *c = t->cen + 3 * i;
        double r = t->r_px * t->mpp[i], rr = r * r;
        long long lo[3], hi[3], m = 0;
        for (int a = 0; a < 3; a++) {
            lo[a] = (long long)mx_floor((c[a] - r) / t->G);
            hi[a] = (long long)mx_floor((c[a] + r) / t->G);
        }
        for (long long x = lo[0]; x <= hi[0]; x++)
            for (long long y = lo[1]; y <= hi[1]; y++)
                for (long long z = lo[2]; z <= hi[2]; z++) {
                    long long k = cell_find(t->ckey, t->ncell, x, y, z);
                    if (k < 0)
                        continue;
                    for (long long s = t->cstart[k]; s < t->cstart[k + 1]; s++) {
                        long long j = t->cidx[s];
                        double dx = t->cen[3 * j] - c[0], dy = t->cen[3 * j + 1] - c[1], dz = t->cen[3 * j + 2] - c[2];
                        if ((dx * dx + dy * dy) + dz * dz <= rr) {
                            if (m == cap) {
                                cap *= 2;
                                idx = mx_realloc(idx, sizeof(long long) * (size_t)cap);
                                w = mx_realloc(w, sizeof(double) * (size_t)cap);
                            }
                            idx[m++] = j;
                        }
                    }
                }
        if (!m)
            continue;
        for (long long s = 0; s < m; s++)
            w[s] = t->wt[idx[s]];
        double ws = mx_np_pairwise(w, m, 1);
        if (!(ws > 0))
            continue;
        for (int j = 0; j < 3; j++) {
            double acc = t->col[3 * idx[0] + j] * w[0];
            for (long long s = 1; s < m; s++)
                acc += t->col[3 * idx[s] + j] * w[s];
            t->tgt[3 * i + j] = acc / ws;
        }
    }
    free(idx);
    free(w);
}

static const long long *sort_key;
static int cmp_cell(const void *a, const void *b)  /* np.lexsort by (x, y, z): stable, so ties by index */
{
    long long i = *(const long long *)a, j = *(const long long *)b;
    for (int k = 0; k < 3; k++)
        if (sort_key[3 * i + k] != sort_key[3 * j + k])
            return sort_key[3 * i + k] < sort_key[3 * j + k] ? -1 : 1;
    return (i > j) - (i < j);
}

typedef struct {
    const long long *toff, *woff;
    const double *P, *tgt;
    const int *W;
    const double *k;
    const int *wrgb;
    int *out;
    unsigned char *changed;
} wedge_ctx;

static void soft_wedges(void *vc, long long e)
{
    wedge_ctx *c = vc;
    long long t0 = c->toff[e], t1 = c->toff[e + 1], w0 = c->woff[e], nw = c->woff[e + 1] - w0;
    memcpy(c->out + 3 * w0, c->wrgb + 3 * w0, sizeof(int) * 3 * (size_t)nw);
    int any = 0;
    for (long long i = t0; i < t1; i++)
        if (c->k[i] != 0)  /* k.any(): NaN counts */
            any = 1;
    if (!any)
        return;
    double *kw = MX_ZNEW(double, 5 * (size_t)nw), *ww = kw + nw, *tw = ww + nw;
    for (int j = 0; j < 3; j++)
        for (long long i = t0; i < t1; i++) {
            const double *p = c->P + 9 * i;
            double a[3] = {p[3] - p[0], p[4] - p[1], p[5] - p[2]}, b[3] = {p[6] - p[0], p[7] - p[1], p[8] - p[2]}, n[3];
            cross3(a, b, n);
            double ta = nrm3(n[0], n[1], n[2]);
            long long w = c->W[3 * i + j];
            kw[w] += c->k[i] * ta;
            ww[w] += ta;
            for (int q = 0; q < 3; q++)
                tw[3 * w + q] += c->tgt[3 * i + q] * ta;
        }
    for (long long w = 0; w < nw; w++)
        if (ww[w] > 0 && kw[w] > 0) {
            double f = kw[w] / ww[w];
            int *o = c->out + 3 * (w0 + w);
            for (int j = 0; j < 3; j++) {
                double t = tw[3 * w + j] / ww[w], v = (double)o[j];
                long long r = (long long)(v + f * (t - v) + 0.5);
                o[j] = r < 255 ? (int)r : 255;
            }
        }
    c->changed[e] = 1;
    free(kw);
}

/* ne entries: triangles toff[e]..toff[e + 1] (corners P, wedges W local to the entry), wedge colours wrgb (rows
   woff[e]..woff[e + 1], 3 ints each). out: the new wedge colours (entries with changed[e] = 0 are copies). Returns the
   number of softened triangles (k > 0). */
MX_API long long mx_soften_thin(int ne, const long long *toff, const double *P, const int *W, const long long *woff,
                                const int *wrgb, int ns, const double *S, double floor_, double soft_px, double soft_focal,
                                double soft_reach, double soft_r_px, double cam_reach, int *out, unsigned char *changed)
{
    long long nt = toff[ne];
    soft_ctx c = {P, S, ns, floor_, soft_focal, soft_reach, cam_reach, MX_NEW(double, nt), MX_NEW(double, nt),
                  MX_NEW(double, nt), MX_NEW(double, nt), MX_NEW(double, 3 * nt), nt};
    mx_parallel_for((nt + SOFT_BLOCK - 1) / SOFT_BLOCK, soft_rows, &c);
    double *col = MX_NEW(double, 3 * nt), *wt = MX_NEW(double, nt), *k = MX_NEW(double, nt);
    for (int e = 0; e < ne; e++)
        for (long long i = toff[e]; i < toff[e + 1]; i++) {
            for (int j = 0; j < 3; j++) {
                const int *w = W + 3 * i;
                const int *r0 = wrgb + 3 * (woff[e] + w[0]), *r1 = wrgb + 3 * (woff[e] + w[1]), *r2 = wrgb + 3 * (woff[e] + w[2]);
                col[3 * i + j] = (((double)r0[j] + (double)r1[j]) + (double)r2[j]) / 3;
            }
            wt[i] = c.area2[i] * 0.5 * c.seen[i];
            double x = 1 - c.px[i] / soft_px;
            k[i] = x < 0 ? 0 : x > 1 ? 1 : x;
        }
    /* centroid cells (2 m), sorted by key, members ascending */
    const double G = 2.0;
    long long *gk = MX_NEW(long long, 3 * nt), *ord = MX_NEW(long long, nt);
    for (long long i = 0; i < nt; i++) {
        ord[i] = i;
        for (int a = 0; a < 3; a++)
            gk[3 * i + a] = (long long)mx_floor(c.cen[3 * i + a] / G);
    }
    sort_key = gk;
    qsort(ord, (size_t)nt, sizeof(long long), cmp_cell);
    long long *ckey = MX_NEW(long long, 3 * nt), *cstart = MX_NEW(long long, nt + 1), ncell = 0;
    for (long long s = 0; s < nt; s++) {
        const long long *g = gk + 3 * ord[s];
        if (!ncell || memcmp(g, ckey + 3 * (ncell - 1), 3 * sizeof(long long))) {
            memcpy(ckey + 3 * ncell, g, 3 * sizeof(long long));
            cstart[ncell++] = s;
        }
    }
    cstart[ncell] = nt;
    long long *q = MX_NEW(long long, nt), nq = 0, soft = 0;
    for (long long i = 0; i < nt; i++)
        if (k[i] > 0)
            q[nq++] = i;
    soft = nq;
    double *tgt = MX_NEW(double, 3 * nt);
    memcpy(tgt, col, sizeof(double) * 3 * (size_t)nt);
    tgt_ctx tc = {c.cen, col, wt, ckey, cstart, ord, ncell, G, soft_r_px, c.mpp, q, nq, tgt};
    mx_parallel_for((nq + TGT_BLOCK - 1) / TGT_BLOCK, soft_tgt, &tc);
    memset(changed, 0, (size_t)ne);
    wedge_ctx wc = {toff, woff, P, tgt, W, k, wrgb, out, changed};
    mx_parallel_for(ne, soft_wedges, &wc);
    free(c.area2), free(c.px), free(c.mpp), free(c.seen), free(c.cen);
    free(col), free(wt), free(k), free(gk), free(ord), free(ckey), free(cstart), free(q), free(tgt);
    return soft;
}
