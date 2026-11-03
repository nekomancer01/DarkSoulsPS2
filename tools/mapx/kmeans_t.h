/* kmeans.c's body for one data type T (float / double), names suffixed S */
#define CAT2(a, b) a##_##b
#define CAT(a, b) CAT2(a, b)
#ifndef KMEANS_PW_DEFINED
#define KMEANS_PW_DEFINED
static float pw_f(const float *a, int n)  /* numpy pairwise sum of a short float32 row (d < 8) */
{
    float r = -0.0f;
    for (int i = 0; i < n; i++)
        r += a[i];
    return r;
}
static double pw_d(const double *a, int n)
{
    double r = -0.0;
    for (int i = 0; i < n; i++)
        r += a[i];
    return r;
}
/* x float32 against float64 centres: 2 x (float32, exact) promoted, products and sums in float64 */
typedef struct {
    const float *x;
    const double *c, *cc;
    long long n;
    int d, k;
    long long *out;
} fdctx;

#define FD_BLOCK 4096
static void fd_block(void *vc, long long b)
{
    fdctx *t = vc;
    const double *c = t->c, *cc = t->cc;
    int d = t->d, k = t->k;
    long long e = (b + 1) * FD_BLOCK < t->n ? (b + 1) * FD_BLOCK : t->n;
    for (long long i = b * FD_BLOCK; i < e; i++) {
        const float *x = t->x + i * d;
        long long best = 0;
        double bv = 0;
        for (int j = 0; j < k; j++) {
            double dot = (double)(2 * x[0]) * c[(long long)j * d];
            for (int a = 1; a < d; a++)
                dot = dot + (double)(2 * x[a]) * c[(long long)j * d + a];
            double v = cc[j] - dot;
            if (!j || v < bv)
                bv = v, best = j;
        }
        t->out[i] = best;
    }
}

static void nearest_fd(const float *x, long long n, int d, const double *c, int k, long long *out)
{
    double *cc = MX_NEW(double, k), *t = MX_NEW(double, d);
    for (int j = 0; j < k; j++) {
        for (int a = 0; a < d; a++)
            t[a] = c[(long long)j * d + a] * c[(long long)j * d + a];
        cc[j] = pw_d(t, d);
    }
    fdctx ctx = {x, c, cc, n, d, k, out};
    mx_parallel_for((n + FD_BLOCK - 1) / FD_BLOCK, fd_block, &ctx);
    free(cc), free(t);
}
#endif

typedef struct {
    const T *x, *ct, *cc;  /* ct: centres transposed (d x k) */
    long long n;
    int d, k;
    long long *out;
} CAT(nctx, S);

#define NEAR_BLOCK 1024
static void CAT(nblock, S)(void *vc, long long b)
{
    CAT(nctx, S) *c = vc;
    int d = c->d, k = c->k;
    T *v = MX_NEW(T, k);
    long long e = (b + 1) * NEAR_BLOCK < c->n ? (b + 1) * NEAR_BLOCK : c->n;
    for (long long i = b * NEAR_BLOCK; i < e; i++) {
        const T *xi = c->x + i * d;
        T x0 = (T)(2 * xi[0]);
        for (int j = 0; j < k; j++)
            v[j] = x0 * c->ct[j];
        for (int a = 1; a < d; a++) {
            T xa = (T)(2 * xi[a]);
            const T *row = c->ct + (long long)a * k;
            for (int j = 0; j < k; j++)
                v[j] = v[j] + xa * row[j];
        }
        long long best = 0;
        T bv = c->cc[0] - v[0];
        for (int j = 1; j < k; j++) {
            T t = c->cc[j] - v[j];
            if (t < bv)
                bv = t, best = j;
        }
        c->out[i] = best;
    }
    free(v);
}

static void CAT(nearest, CAT(S, S))(const T *x, long long n, int d, const T *c, int k, long long *out)
{
    T *cc = MX_NEW(T, k), *t = MX_NEW(T, d), *ct = MX_NEW(T, (long long)k * d);
    for (int j = 0; j < k; j++) {
        for (int a = 0; a < d; a++) {
            t[a] = c[(long long)j * d + a] * c[(long long)j * d + a];
            ct[(long long)a * k + j] = c[(long long)j * d + a];
        }
        cc[j] = CAT(pw, S)(t, d);
    }
    CAT(nctx, S) ctx = {x, ct, cc, n, d, k, out};
    mx_parallel_for((n + NEAR_BLOCK - 1) / NEAR_BLOCK, CAT(nblock, S), &ctx);
    free(cc), free(t), free(ct);
}

static T CAT(d2row, S)(const T *x, const T *c, int d, T *tmp)
{
    for (int a = 0; a < d; a++) {
        T v = x[a] - c[a];
        tmp[a] = v * v;
    }
    return CAT(pw, S)(tmp, d);
}

static int CAT(kmeans, S)(const T *x, long long n, int d, const double *wts, int k, int iters, mx_rng *r, T *c,
                          long long *lab)
{
    double *w = MX_NEW(double, n), *p = MX_NEW(double, n);
    for (long long i = 0; i < n; i++)
        w[i] = wts ? wts[i] : 1.0;
    if (k > n)
        k = (int)n;
    T *d2 = MX_NEW(T, n), *tmp = MX_NEW(T, d);
    double ws = mx_np_pairwise(w, n, 1);
    for (long long i = 0; i < n; i++)
        p[i] = w[i] / ws;
    long long first = mx_rng_choice_p(r, n, p);
    memcpy(c, x + first * d, sizeof(T) * (size_t)d);
    for (long long i = 0; i < n; i++)
        d2[i] = CAT(d2row, S)(x + i * d, c, d, tmp);
    double *cdf = MX_NEW(double, n);
    for (int j = 1; j < k; j++) {
        /* p = d2 * w (d2: the previous centres' minimum, updated in the same pass for the next round below) */
        if (j > 1) {
            const T *cj = c + (long long)(j - 1) * d;
            for (long long i = 0; i < n; i++) {
                T v = CAT(d2row, S)(x + i * d, cj, d, tmp);
                if (v < d2[i] || v != v)  /* np.minimum: nan wins */
                    d2[i] = v;
                p[i] = (double)d2[i] * w[i];
            }
        } else
            for (long long i = 0; i < n; i++)
                p[i] = (double)d2[i] * w[i];
        double ps = mx_np_pairwise(p, n, 1);
        long long pick;
        if (ps > 0) {  /* rng.choice(n, p=p / ps): cdf = cumsum, searchsorted(cdf / cdf[-1], u, "right") */
            double s = 0;
            for (long long i = 0; i < n; i++)
                cdf[i] = s = (i ? s + p[i] / ps : p[i] / ps);
            double last = cdf[n - 1], u = mx_rng_random(r);
            long long lo = 0, hi = n;  /* cdf / last is monotone: divided only where the search looks */
            while (lo < hi) {
                long long m = (lo + hi) / 2;
                if (cdf[m] / last <= u)
                    lo = m + 1;
                else
                    hi = m;
            }
            pick = lo;
        } else
            pick = mx_rng_integers(r, n);
        memcpy(c + (long long)j * d, x + pick * d, sizeof(T) * (size_t)d);
    }
    free(cdf);
    double *acc = MX_NEW(double, d), *wj = MX_NEW(double, n);
    long long *cnt = MX_NEW(long long, k + 1), *start = MX_NEW(long long, k + 1), *order = MX_NEW(long long, n);
    for (int it = 0; it < iters; it++) {
        CAT(nearest, CAT(S, S))(x, n, d, c, k, lab);
        memset(cnt, 0, sizeof(long long) * (size_t)(k + 1));
        for (long long i = 0; i < n; i++)
            cnt[lab[i]]++;
        start[0] = 0;
        for (int j = 0; j < k; j++)
            start[j + 1] = start[j] + cnt[j];
        memset(cnt, 0, sizeof(long long) * (size_t)(k + 1));
        for (long long i = 0; i < n; i++)  /* members in index order (the stable argsort by label) */
            order[start[lab[i]] + cnt[lab[i]]++] = i;
        for (int j = 0; j < k; j++) {
            long long s0 = start[j], s1 = start[j + 1];
            if (s0 >= s1)
                continue;
            for (int a = 0; a < d; a++) {
                long long i = order[s0];
                acc[a] = (double)x[i * d + a] * w[i];
            }
            for (long long s = s0 + 1; s < s1; s++)
                for (int a = 0; a < d; a++) {
                    long long i = order[s];
                    acc[a] = acc[a] + (double)x[i * d + a] * w[i];
                }
            for (long long s = s0; s < s1; s++)
                wj[s - s0] = w[order[s]];
            double wsum = mx_np_pairwise(wj, s1 - s0, 1);
            for (int a = 0; a < d; a++)
                c[(long long)j * d + a] = (T)(acc[a] / wsum);
        }
    }
    CAT(nearest, CAT(S, S))(x, n, d, c, k, lab);
    free(w), free(p), free(d2), free(tmp), free(acc), free(wj), free(cnt), free(start), free(order);
    return k;
}
#undef CAT
#undef CAT2
