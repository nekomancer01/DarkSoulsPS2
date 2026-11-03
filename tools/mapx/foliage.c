/* export_map.thin_foliage and its is_canopy test (gather), in C: the same components, hashes, ranks and positions.

   mx_navgrid: gather's nav_grid, navmesh heights by 2 m xz cell (Python dict lists, in insertion order).
   mx_thin_foliage: one foliage mesh's triangles -> the kept triangles in Python's order, each with its component's
   rank, root and size; positions of kept, non-canopy components moved as thin_foliage moves them. */
#include "mapx.h"

typedef struct {
    mx_map cells;           /* (i, j) -> cell id */
    long long *start, *cnt; /* per cell: run in ys */
    double *ys;
    long long n;
} mx_navgrid;

static long long py_floordiv2(double x) { return (long long)mx_floor(x / 2.0); }  /* int(x // 2): x / 2 is exact */

MX_API mx_navgrid *mx_navgrid_new(long long n, const double *pts)
{
    mx_navgrid *g = MX_ZNEW(mx_navgrid, 1);
    mx_map_init(&g->cells, 2, n / 4 + 16);
    long long *cid = MX_NEW(long long, n);
    for (long long i = 0; i < n; i++) {
        long long k[2] = {py_floordiv2(pts[3 * i]), py_floordiv2(pts[3 * i + 2])};
        cid[i] = mx_map_put(&g->cells, k, NULL);
    }
    long long nc = g->cells.n;
    g->cnt = MX_ZNEW(long long, nc);
    g->start = MX_ZNEW(long long, nc + 1);
    for (long long i = 0; i < n; i++)
        g->cnt[cid[i]]++;
    for (long long c = 0; c < nc; c++)
        g->start[c + 1] = g->start[c] + g->cnt[c];
    g->ys = MX_NEW(double, n);
    long long *fill = MX_ZNEW(long long, nc);
    for (long long i = 0; i < n; i++)  /* each cell's heights in point order, as the dict's lists */
        g->ys[g->start[cid[i]] + fill[cid[i]]++] = pts[3 * i + 1];
    free(fill);
    free(cid);
    g->n = n;
    return g;
}

MX_API void mx_navgrid_free(mx_navgrid *g)
{
    if (!g)
        return;
    mx_map_free(&g->cells);
    free(g->start);
    free(g->cnt);
    free(g->ys);
    free(g);
}

/* gather's is_canopy over world positions W of vertices vs[0..nv) */
static int is_canopy(const mx_navgrid *g, const double *W, const int *vs, int nv, double canopy_h)
{
    double base = W[3 * vs[0] + 1];
    for (int k = 1; k < nv; k++)
        if (W[3 * vs[k] + 1] < base)
            base = W[3 * vs[k] + 1];
    mx_psum sx, sz;
    mx_psum_init(&sx, W[3 * vs[0]]);
    mx_psum_init(&sz, W[3 * vs[0] + 2]);
    for (int k = 1; k < nv; k++) {
        mx_psum_add(&sx, W[3 * vs[k]]);
        mx_psum_add(&sz, W[3 * vs[k] + 2]);
    }
    double cx = mx_psum_get(&sx) / nv, cz = mx_psum_get(&sz) / nv;
    long long ci = py_floordiv2(cx), cj = py_floordiv2(cz);
    if (!g)
        return 1;
    static const int rs[2] = {2, 6};
    for (int r = 0; r < 2; r++) {
        int found = 0;
        double best = 0;
        for (long long i = ci - rs[r]; i <= ci + rs[r]; i++)
            for (long long j = cj - rs[r]; j <= cj + rs[r]; j++) {
                long long key[2] = {i, j}, c = mx_map_get(&g->cells, key);
                if (c < 0)
                    continue;
                for (long long s = g->start[c]; s < g->start[c + 1]; s++) {
                    double y = g->ys[s];
                    if (y <= base + 0.5 && (!found || y > best))  /* max(): the first of equal values */
                        best = y, found = 1;
                }
            }
        if (found)
            return base - best > canopy_h;
    }
    return 1;
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* tl: nt x 3 vertex indices; P: nv x 3 model positions (in: thin_foliage's pos, out: as it returns them; moved[i]
   set for moved vertices); W: nv x 3 world positions for is_canopy, or NULL (no canopy test); grid: NULL = no
   navmesh (is_canopy is then always true). Outputs per kept triangle (at most nt): out_t (index into tl), rank,
   root (the component id in thin_foliage's tuft name "seed/root"), size. Returns the kept count; *canopy_tris gets
   the triangles kept as canopy (CANOPY_STATS). */
MX_API int mx_thin_foliage(int nt, const int *tl, int nv, double *P, unsigned char *moved, const double *W,
                           const mx_navgrid *grid, int canopy, double canopy_h, const char *seed, int ivy, int grass_keep,
                           double fol_xz, double fol_y, double fol_ivy, int *out_t, double *rank, int *root,
                           int *size, int *canopy_tris)
{
    int *par = MX_NEW(int, nt);
    for (int k = 0; k < nt; k++)
        par[k] = k;
#define ROOT(x0) ({ int x_ = (x0); while (par[x_] != x_) { par[x_] = par[par[x_]]; x_ = par[x_]; } x_; })
    mx_map first;
    mx_map_init(&first, 3, nv);
    long long *first_k = MX_NEW(long long, nv > 0 ? nv : 1);
    long long *vkey = MX_NEW(long long, 3 * (size_t)(nv > 0 ? nv : 1));
    for (int i = 0; i < nv; i++)
        for (int a = 0; a < 3; a++)
            vkey[3 * i + a] = (long long)mx_rint(P[3 * i + a] * 1e4);
    for (int k = 0; k < nt; k++)
        for (int c = 0; c < 3; c++) {
            int i = tl[3 * k + c], added;
            long long id = mx_map_put(&first, vkey + 3 * (long long)i, &added);
            if (added)
                first_k[id] = k;
            else {
                int r2 = ROOT((int)first_k[id]);
                par[ROOT(k)] = r2;
            }
        }
    /* components in order of first appearance, members ascending */
    int *rt = MX_NEW(int, nt), *cnt = MX_ZNEW(int, nt), *cstart = MX_NEW(int, nt + 1), *order = MX_NEW(int, nt);
    int *crank = MX_NEW(int, nt), nc = 0;
    for (int k = 0; k < nt; k++)
        rt[k] = ROOT(k);
    for (int k = 0; k < nt; k++)
        crank[k] = -1;
    int *comp_root = MX_NEW(int, nt);
    for (int k = 0; k < nt; k++) {
        if (crank[rt[k]] < 0)
            comp_root[nc] = rt[k], crank[rt[k]] = nc++;
        cnt[crank[rt[k]]]++;
    }
    cstart[0] = 0;
    for (int c = 0; c < nc; c++)
        cstart[c + 1] = cstart[c] + cnt[c];
    memset(cnt, 0, sizeof(int) * (size_t)nt);
    for (int k = 0; k < nt; k++) {
        int c = crank[rt[k]];
        order[cstart[c] + cnt[c]++] = k;
    }
    int keep = ivy ? ((grass_keep + 1) / 2 > 1 ? (grass_keep + 1) / 2 : 1) : grass_keep;
    double sxz = ivy ? fol_ivy : fol_xz, sy = ivy ? fol_ivy : fol_y;
    int *vs = MX_NEW(int, 3 * (size_t)(nt > 0 ? nt : 1));
    size_t slen = 0;
    while (seed[slen])
        slen++;
    char *msg = MX_NEW(char, slen + 16);
    memcpy(msg, seed, slen);
    msg[slen] = '/';
    int nout = 0;
    *canopy_tris = 0;
    for (int c = 0; c < nc; c++) {
        int r = comp_root[c], n = cstart[c + 1] - cstart[c];
        const int *ks = order + cstart[c];
        char dig[12];
        int nd = 0, mlen = (int)slen + 1;
        for (unsigned v = (unsigned)r; !nd || v; v /= 10)
            dig[nd++] = (char)('0' + v % 10);
        while (nd)
            msg[mlen++] = dig[--nd];
        uint8_t h[16];
        mx_md5(msg, (size_t)mlen, h);
        double u = (double)((uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24) / 4294967296.0;
        int m = 0;
        for (int j = 0; j < n; j++)
            for (int q = 0; q < 3; q++)
                vs[m++] = tl[3 * ks[j] + q];
        qsort(vs, (size_t)m, sizeof(int), cmp_int);
        int nu = 0;
        for (int j = 0; j < m; j++)
            if (!nu || vs[j] != vs[nu - 1])
                vs[nu++] = vs[j];
        if (canopy && is_canopy(grid, W, vs, nu, canopy_h)) {
            *canopy_tris += n;
            for (int j = 0; j < n; j++, nout++)
                out_t[nout] = ks[j], rank[nout] = 0.0, root[nout] = r, size[nout] = n;
            continue;
        }
        if (u * keep >= 1)
            continue;
        double o[3];
        for (int a = 0; a < 3; a++) {
            mx_psum s;
            mx_psum_init(&s, P[3 * vs[0] + a]);
            for (int j = 1; j < nu; j++)
                mx_psum_add(&s, P[3 * vs[j] + a]);
            o[a] = mx_psum_get(&s) / nu;
        }
        if (!ivy) {
            o[1] = P[3 * vs[0] + 1];
            for (int j = 1; j < nu; j++)
                if (P[3 * vs[j] + 1] < o[1])
                    o[1] = P[3 * vs[j] + 1];
        }
        for (int j = 0; j < nu; j++) {
            double *p = P + 3 * vs[j];
            p[0] = o[0] + (p[0] - o[0]) * sxz;
            p[1] = o[1] + (p[1] - o[1]) * sy;
            p[2] = o[2] + (p[2] - o[2]) * sxz;
            moved[vs[j]] = 1;
        }
        for (int j = 0; j < n; j++, nout++)
            out_t[nout] = ks[j], rank[nout] = u * keep, root[nout] = r, size[nout] = n;
    }
#undef ROOT
    free(par), free(first_k), free(vkey), free(rt), free(cnt), free(cstart), free(order), free(crank);
    free(comp_root), free(vs), free(msg);
    mx_map_free(&first);
    return nout;
}
