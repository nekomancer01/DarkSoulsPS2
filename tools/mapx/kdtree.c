/* Nearest sample point (squared distance), as geom.c nearest_d2 for export_map.view_distance: on the lattices
   view_distance uses (2 m cells, 0.5 m samples) every squared distance is exact, so any exact search gives the same
   minimum. Thread-safe (no file statics), queries in parallel. */
#include "mapx.h"

#define KD_LEAF 8

static void swap3(double *S, long long i, long long j)
{
    for (int k = 0; k < 3; k++) {
        double t = S[3 * i + k];
        S[3 * i + k] = S[3 * j + k];
        S[3 * j + k] = t;
    }
}

static void select_nth(double *S, long long lo, long long hi, long long nth, int a)  /* quickselect on axis a */
{
    while (hi - lo > 1) {
        long long mid = lo + (hi - lo) / 2;
        double x = S[3 * lo + a], y = S[3 * mid + a], z = S[3 * (hi - 1) + a];
        double piv = x < y ? (y < z ? y : (x < z ? z : x)) : (x < z ? x : (y < z ? z : y));
        long long i = lo, j = hi - 1;
        while (i <= j) {
            while (S[3 * i + a] < piv)
                i++;
            while (S[3 * j + a] > piv)
                j--;
            if (i <= j)
                swap3(S, i++, j--);
        }
        if (nth <= j)
            hi = j + 1;
        else if (nth >= i)
            lo = i;
        else
            return;
    }
}

static void kd_build(double *S, unsigned char *ax, long long lo, long long hi)
{
    if (hi - lo <= KD_LEAF)
        return;
    double mn[3], mx[3];
    for (int k = 0; k < 3; k++)
        mn[k] = mx[k] = S[3 * lo + k];
    for (long long i = lo + 1; i < hi; i++)
        for (int k = 0; k < 3; k++) {
            double v = S[3 * i + k];
            if (v < mn[k])
                mn[k] = v;
            if (v > mx[k])
                mx[k] = v;
        }
    int a = 0;
    for (int k = 1; k < 3; k++)
        if (mx[k] - mn[k] > mx[a] - mn[a])
            a = k;
    long long m = (lo + hi) / 2;
    select_nth(S, lo, hi, m, a);
    ax[m] = (unsigned char)a;
    kd_build(S, ax, lo, m);
    kd_build(S, ax, m + 1, hi);
}

static double d2(const double *s, const double *p)
{
    double dx = p[0] - s[0], dy = p[1] - s[1], dz = p[2] - s[2];
    return dx * dx + dy * dy + dz * dz;
}

static void query(const double *S, const unsigned char *ax, long long lo, long long hi, const double *p, double *best,
                  long long *bi, double rd, double *off)
{
    if (hi - lo <= KD_LEAF) {
        for (long long i = lo; i < hi; i++) {
            double d = d2(S + 3 * i, p);
            if (d < *best)
                *best = d, *bi = i;
        }
        return;
    }
    long long m = (lo + hi) / 2;
    int a = ax[m];
    double d = p[a] - S[3 * m + a], dm = d2(S + 3 * m, p);
    if (dm < *best)
        *best = dm, *bi = m;
    long long nlo = d < 0 ? lo : m + 1, nhi = d < 0 ? m : hi, flo = d < 0 ? m + 1 : lo, fhi = d < 0 ? hi : m;
    query(S, ax, nlo, nhi, p, best, bi, rd, off);
    double old = off[a], rd2 = rd - old * old + d * d;
    if (rd2 < *best) {
        off[a] = d;
        query(S, ax, flo, fhi, p, best, bi, rd2, off);
        off[a] = old;
    }
}

typedef struct {
    const double *S, *Q;
    const unsigned char *ax;
    long long ns, nq;
    double *out;
} kd_ctx;

#define KD_BLOCK 512
static void kd_block(void *vc, long long b)
{
    kd_ctx *c = vc;
    long long e = (b + 1) * KD_BLOCK < c->nq ? (b + 1) * KD_BLOCK : c->nq, bi = -1;
    for (long long i = b * KD_BLOCK; i < e; i++) {
        double best = bi >= 0 ? d2(c->S + 3 * bi, c->Q + 3 * i) : 1e300, off[3] = {0.0, 0.0, 0.0};
        query(c->S, c->ax, 0, c->ns, c->Q + 3 * i, &best, &bi, 0.0, off);
        c->out[i] = best;
    }
}

/* out[i] = min squared distance from Q[i] to the samples S (reordered in place) */
void mx_nearest_d2(long long nq, const double *Q, long long ns, double *S, double *out)
{
    if (!ns) {
        for (long long i = 0; i < nq; i++)
            out[i] = 1e300;
        return;
    }
    unsigned char *ax = MX_ZNEW(unsigned char, ns);
    kd_build(S, ax, 0, ns);
    kd_ctx c = {S, Q, ax, ns, nq, out};
    mx_parallel_for((nq + KD_BLOCK - 1) / KD_BLOCK, kd_block, &c);
    free(ax);
}
