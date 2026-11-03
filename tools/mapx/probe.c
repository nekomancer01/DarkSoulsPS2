/* dsr_light.ProbeIndex in C: collision-triangle centroids on a cell grid, the probe of the nearest one in the 27
   cells around a query, cached per query cell (the first query's answer, as the Python cache keeps it) */
#include "mapx.h"

double pow(double, double);

typedef struct mx_probe {
    double cell;
    long long dflt;            /* -1: None */
    mx_map grid, cache;        /* cell key -> grid id; query cell -> cache id */
    long long *head, *next;    /* per grid id: first entry; per entry: next (insertion order) */
    long long *tail;
    double *C;                 /* entries: centroid */
    long long *pr;             /* entries: probe */
    long long *cval;           /* per cache id: the answer */
    long long ccap;
} mx_probe;

static void cell_key(double cell, const double *p, long long *k)
{
    for (int a = 0; a < 3; a++)
        k[a] = (long long)mx_floor(p[a] / cell);
}

/* n collision triangles T (n x 3 corners x 3), probe pr per triangle (in the order ProbeIndex adds them) */
MX_API mx_probe *mx_probe_new(long long n, const double *T, const long long *pr, double cell, long long dflt)
{
    mx_probe *h = MX_ZNEW(mx_probe, 1);
    h->cell = cell, h->dflt = dflt;
    mx_map_init(&h->grid, 3, n / 4 + 16);
    mx_map_init(&h->cache, 3, 1024);
    h->C = MX_NEW(double, 3 * (n ? n : 1));
    h->pr = MX_NEW(long long, n ? n : 1);
    h->next = MX_NEW(long long, n ? n : 1);
    h->head = MX_NEW(long long, n ? n : 1);
    h->tail = MX_NEW(long long, n ? n : 1);
    for (long long i = 0; i < n; i++) {
        const double *t = T + 9 * i;
        double *c = h->C + 3 * i;
        for (int a = 0; a < 3; a++)
            c[a] = mx_py_sum3(t[a], t[3 + a], t[6 + a]) / 3;
        h->pr[i] = pr[i];
        h->next[i] = -1;
        long long k[3];
        cell_key(cell, c, k);
        int added;
        long long g = mx_map_put(&h->grid, k, &added);
        if (added)
            h->head[g] = i;
        else
            h->next[h->tail[g]] = i;
        h->tail[g] = i;
    }
    return h;
}

/* probe per row of P (n x 3), in row order; -1 = None */
MX_API void mx_probe_query(mx_probe *h, long long n, const double *P, long long *out)
{
    for (long long i = 0; i < n; i++) {
        const double *p = P + 3 * i;
        long long k[3];
        cell_key(h->cell, p, k);
        int added;
        long long id = mx_map_put(&h->cache, k, &added);
        if (!added) {
            out[i] = h->cval[id];
            continue;
        }
        long long best = h->dflt;
        double bd = 1e18;
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dz = -1; dz <= 1; dz++) {
                    long long q[3] = {k[0] + dx, k[1] + dy, k[2] + dz};
                    long long g = mx_map_get(&h->grid, q);
                    if (g < 0)
                        continue;
                    for (long long e = h->head[g]; e >= 0; e = h->next[e]) {
                        const double *c = h->C + 3 * e;
                        /* Python's (c - p) ** 2: libm pow, as float.__pow__ calls it */
                        double d = (pow(c[0] - p[0], 2.0) + pow(c[1] - p[1], 2.0)) + pow(c[2] - p[2], 2.0);
                        if (d < bd)
                            best = h->pr[e], bd = d;
                    }
                }
        if (id >= h->ccap) {
            h->ccap = h->ccap ? 2 * h->ccap : 1024;
            h->cval = mx_realloc(h->cval, sizeof(long long) * (size_t)h->ccap);
        }
        h->cval[id] = best;
        out[i] = best;
    }
}

MX_API void mx_probe_free(mx_probe *h)
{
    if (!h)
        return;
    mx_map_free(&h->grid), mx_map_free(&h->cache);
    free(h->C), free(h->pr), free(h->next), free(h->head), free(h->tail), free(h->cval), free(h);
}
