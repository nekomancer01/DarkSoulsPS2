/* export_map.far_cells in C: the CHUNK_GRID cells of triangle centroids (Python's sum() of the corners / 3) whose
   centre lies beyond STREAM_FAR of every navmesh vertex. */
#include "mapx.h"

static const long long *ck_sort;
static int cmp_key3(const void *a, const void *b)
{
    const long long *x = ck_sort + 3 * *(const long long *)a, *y = ck_sort + 3 * *(const long long *)b;
    for (int k = 0; k < 3; k++)
        if (x[k] != y[k])
            return x[k] < y[k] ? -1 : 1;
    return 0;
}

typedef struct {
    const long long *keys;
    const double *nv;
    long long nnv;
    double grid, far2;
    unsigned char *far;
} far_ctx;

static void far_one(void *vc, long long i)
{
    far_ctx *c = vc;
    double p[3], best = 0;
    for (int a = 0; a < 3; a++)
        p[a] = ((double)c->keys[3 * i + a] + 0.5) * c->grid;
    for (long long j = 0; j < c->nnv; j++) {
        double dx = p[0] - c->nv[3 * j], dy = p[1] - c->nv[3 * j + 1], dz = p[2] - c->nv[3 * j + 2];
        double d = ((-0.0 + dx * dx) + dy * dy) + dz * dz;
        if (!j || d < best || d != d)  /* ndarray.min: nan wins */
            best = d;
    }
    c->far[i] = best > c->far2;
}

/* ntri triangles of corner positions T (ntri x 9). out: the far cells' keys (3 int64 each), sorted; returns their
   count; *ncells gets the number of distinct cells (printed by far_cells). The caller sizes out for ntri keys. */
MX_API long long mx_far_cells(long long ntri, const double *T, long long nnv, const double *nv, double grid,
                              double stream_far, long long *out, long long *ncells)
{
    mx_map m;
    mx_map_init(&m, 3, ntri / 8 + 16);
    for (long long t = 0; t < ntri; t++) {
        const double *p = T + 9 * t;
        long long k[3];
        for (int a = 0; a < 3; a++)
            k[a] = (long long)mx_floor(mx_py_sum3(p[a], p[3 + a], p[6 + a]) / 3 / grid);
        mx_map_put(&m, k, NULL);
    }
    long long n = m.n, *ord = MX_NEW(long long, n);
    for (long long i = 0; i < n; i++)
        ord[i] = i;
    ck_sort = m.keys;
    qsort(ord, (size_t)n, sizeof(long long), cmp_key3);
    long long *keys = MX_NEW(long long, 3 * n);
    for (long long i = 0; i < n; i++)
        memcpy(keys + 3 * i, m.keys + 3 * ord[i], 3 * sizeof(long long));
    *ncells = n;
    long long nf = 0;
    if (n && nnv) {
        unsigned char *far = MX_ZNEW(unsigned char, n);
        far_ctx c = {keys, nv, nnv, grid, stream_far * stream_far, far};
        mx_parallel_for(n, far_one, &c);
        for (long long i = 0; i < n; i++)
            if (far[i])
                memcpy(out + 3 * nf++, keys + 3 * i, 3 * sizeof(long long));
        free(far);
    }
    free(ord);
    free(keys);
    mx_map_free(&m);
    return nf;
}
