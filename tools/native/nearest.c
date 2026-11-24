/* tools/horizon.py view_d in C (2026-11-07, build speed): per query point the smallest squared distance to the samples,
 * (dx * dx + dy * dy) + dz * dz in double as numpy's ((v - s) ** 2).sum(-1) adds it, by brute force (no pruning: the
 * minimum is the same double numpy finds). A query stops at the first sample nearer than sqrt(stop): view_d clamps every
 * distance up to HZN_D, so any value under stop = HZN_D^2 gives the same result. Built with -ffp-contract=off. */
/* 2026-11-24 (build profile: min_d2 ~2.5 % of a cold world build's CPU, brute force over up to 4000 samples per
 * point, the far points visiting all of them): a k-d tree over the samples, nearest side first, a subtree skipped when
 * the squared gap to its splitting plane exceeds the best so far. Each distance is still the double above, and a
 * skipped sample is provably no nearer (rounded subtraction and squaring are monotonic): the same minimum. */
#include <stdlib.h>

typedef struct { double x[3]; } KP;
static int kd_axis;
static int kp_cmp(const void *a, const void *b)
{
    double u = ((const KP *)a)->x[kd_axis], v = ((const KP *)b)->x[kd_axis];
    return u < v ? -1 : u > v;
}

/* the tree in place: node = the middle of [lo, hi), its axis the box's widest */
static void kd_build(KP *p, unsigned char *ax, int lo, int hi)
{
    if (hi - lo <= 8)
        return;
    double mn[3], mx[3];
    for (int k = 0; k < 3; k++)
        mn[k] = mx[k] = p[lo].x[k];
    for (int i = lo + 1; i < hi; i++)
        for (int k = 0; k < 3; k++)
            mn[k] = p[i].x[k] < mn[k] ? p[i].x[k] : mn[k], mx[k] = p[i].x[k] > mx[k] ? p[i].x[k] : mx[k];
    int a = 0;
    for (int k = 1; k < 3; k++)
        if (mx[k] - mn[k] > mx[a] - mn[a])
            a = k;
    kd_axis = a;
    qsort(p + lo, (size_t)(hi - lo), sizeof(KP), kp_cmp);
    int m = (lo + hi) / 2;
    ax[m] = (unsigned char)a;
    kd_build(p, ax, lo, m);
    kd_build(p, ax, m + 1, hi);
}

static void kd_near(const KP *p, const unsigned char *ax, int lo, int hi, const double *q, double *best)
{
    if (hi - lo <= 8) {
        for (int i = lo; i < hi; i++) {
            double dx = q[0] - p[i].x[0], dy = q[1] - p[i].x[1], dz = q[2] - p[i].x[2];
            double d = dx * dx + dy * dy + dz * dz;
            if (d < *best)
                *best = d;
        }
        return;
    }
    int m = (lo + hi) / 2, a = ax[m];
    double dx = q[0] - p[m].x[0], dy = q[1] - p[m].x[1], dz = q[2] - p[m].x[2];
    double d = dx * dx + dy * dy + dz * dz;
    if (d < *best)
        *best = d;
    double g = q[a] - p[m].x[a];
    /* left of m: x[a] <= split, right: >= split */
    if (g < 0) {
        kd_near(p, ax, lo, m, q, best);
        if (g * g <= *best)
            kd_near(p, ax, m + 1, hi, q, best);
    } else {
        kd_near(p, ax, m + 1, hi, q, best);
        if (g * g <= *best)
            kd_near(p, ax, lo, m, q, best);
    }
}

static void brute(int nq, const double *Q, int ns, const double *S, double stop, double *out);

void min_d2(int nq, const double *Q, int ns, const double *S, double stop, double *out)
{
    KP *p = ns >= 64 ? malloc(sizeof(KP) * (size_t)ns) : NULL;
    unsigned char *ax = p ? calloc((size_t)ns, 1) : NULL;
    if (!p || !ax) {
        free(p), free(ax);
        brute(nq, Q, ns, S, stop, out);
        return;
    }
    for (int j = 0; j < ns; j++)
        p[j].x[0] = S[3 * j], p[j].x[1] = S[3 * j + 1], p[j].x[2] = S[3 * j + 2];
    kd_build(p, ax, 0, ns);
    (void)stop;  /* the exact minimum is below stop whenever the brute force's early answer is */
#pragma omp parallel for schedule(dynamic, 256)
    for (int i = 0; i < nq; i++) {
        double best = 1e300;
        kd_near(p, ax, 0, ns, Q + 3 * i, &best);
        out[i] = best;
    }
    free(p), free(ax);
}

/* the brute force (small sample sets) */
static void brute(int nq, const double *Q, int ns, const double *S, double stop, double *out)
{

    /* each query writes only out[i]: the same values with any thread count (-fopenmp, or serial) */
#pragma omp parallel for schedule(dynamic, 256)
    for (int i = 0; i < nq; i++) {
        const double qx = Q[3 * i], qy = Q[3 * i + 1], qz = Q[3 * i + 2];
        double best = 1e300;
        for (int j = 0; j < ns; j++) {
            double dx = qx - S[3 * j], dy = qy - S[3 * j + 1], dz = qz - S[3 * j + 2];
            double d = dx * dx + dy * dy + dz * dz;
            if (d < best) {
                best = d;
                if (best < stop)
                    break;
            }
        }
        out[i] = best;
    }
}
