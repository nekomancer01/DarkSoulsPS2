/* export_map.simplify_meshes + view_distance in C: duplicate meshes dropped, meshes welded into groups by shared
   2 mm position keys, groups cut into CHUNK_GRID cell jobs (positions on cuts locked), view distances from the
   navmesh samples (k-d tree), every job through simplify.c on threads, the global error threshold (fixed or searched
   to fit BUDGET), and each job's log replayed. Python's dict orders (first appearance) are kept throughout. */
#include "mapx.h"
#include <math.h>

typedef struct {
    int nt;
    int *t0;
    int nsteps;
    double *cost;
    int *nrem, *ncha;
    int *rem;
    int *cha;
    size_t nrem_all, ncha_all;
} sresult;
sresult *simplify_run(int nv, const double *posd, int nw, const int *wpos, int ntri, const int *tris,
                      const unsigned char *locked, const double *scale, const double *weight, double sat,
                      double border_w, double seam_w, double seam_k2,
                      double ak2, double min_cos);
sresult *simplify_run_grp(int nv, const double *posd, int nw, const int *wpos, int ntri, const int *tris,
                          const unsigned char *locked, const double *scale, const double *weight, double sat,
                          double border_w, double seam_w, double seam_k2, double ak2, double min_cos, const int *agrp);
void simplify_free(sresult *R);
void simplify_set_envelope(double pad);
void simplify_set_guard(double qmin, double flip_cos);
void simplify_set_shape(double tol);

static double np_maximum(double a, double b) { return a >= b || a != a ? a : b; }
static double np_minimum(double a, double b) { return a <= b || a != a ? a : b; }

/* ---- view_distance ---- */
static int cmp_row3(const void *a, const void *b)
{
    const double *x = a, *y = b;
    for (int k = 0; k < 3; k++)
        if (x[k] != y[k])
            return x[k] < y[k] ? -1 : 1;
    return 0;
}

static long long uniq_rows(double *R, long long n)  /* np.unique(axis=0): sorted distinct rows */
{
    qsort(R, (size_t)n, 3 * sizeof(double), cmp_row3);
    long long m = 0;
    for (long long i = 0; i < n; i++)
        if (!m || cmp_row3(R + 3 * i, R + 3 * (m - 1)))
            memcpy(R + 3 * m++, R + 3 * i, 3 * sizeof(double));
    return m;
}

typedef struct {
    double floor_, spawn_k;
    const double *spawn;  /* NULL: raw (navmesh distance only) */
    int nkeep;
    const double *keep_pos;
    double keep_r2, keep_d;  /* KEEP_R ** 2, KEEP_D */
} vd_opt;

/* distances of the n points P (n x 3) -> out */
static void view_distance(long long n, const double *P, long long nnv, const double *nv, long long nnt, const int *nt,
                          const vd_opt *o, double *out)
{
    long long ns = nnv + nnt * 13;
    double *S = MX_NEW(double, 3 * (ns > 0 ? ns : 1));
    long long s = 0;
    for (long long i = 0; i < nnv; i++, s++)
        memcpy(S + 3 * s, nv + 3 * i, 3 * sizeof(double));
#define V(k) (nv + 3 * (long long)nt[3 * t + (k)])
    for (long long t = 0; t < nnt; t++, s++)
        for (int a = 0; a < 3; a++)
            S[3 * s + a] = ((V(0)[a] + V(1)[a]) + V(2)[a]) / 3;
    for (int si = 1; si <= 3; si++) {
        double uv[3][2] = {{si / 4.0, 0}, {0, si / 4.0}, {si / 4.0, (4 - si) / 4.0 * 0.5}};
        for (int q = 0; q < 3; q++)
            for (long long t = 0; t < nnt; t++, s++)
                for (int a = 0; a < 3; a++) {
                    double A = V(0)[a], B = V(1)[a], C = V(2)[a];
                    S[3 * s + a] = (A + uv[q][0] * (B - A)) + uv[q][1] * (C - A);
                }
    }
    for (int q = 0; q < 3; q++)
        for (long long t = 0; t < nnt; t++, s++)
            for (int a = 0; a < 3; a++) {
                const double *x = q == 0 ? V(0) : q == 1 ? V(1) : V(0), *y = q == 0 ? V(1) : q == 1 ? V(2) : V(2);
                S[3 * s + a] = (x[a] + y[a]) / 2;
            }
#undef V
    for (long long i = 0; i < 3 * ns; i++)
        S[i] = mx_rint(S[i] * 2) / 2;
    ns = uniq_rows(S, ns);
    double *cells = MX_NEW(double, 3 * (n > 0 ? n : 1));
    for (long long i = 0; i < 3 * n; i++)
        cells[i] = mx_rint(P[i] / 2) * 2;
    long long nc = uniq_rows(cells, n);
    double *dc = MX_NEW(double, nc > 0 ? nc : 1);
    mx_nearest_d2(nc, cells, ns, S, dc);
    for (long long i = 0; i < n; i++) {
        double c[3];
        for (int a = 0; a < 3; a++)
            c[a] = mx_rint(P[3 * i + a] / 2) * 2;
        long long lo = 0, hi = nc;
        while (lo < hi) {
            long long m = (lo + hi) / 2;
            if (cmp_row3(cells + 3 * m, c) < 0)
                lo = m + 1;
            else
                hi = m;
        }
        double dn = mx_sqrt(np_maximum(dc[lo], 0));
        if (!o->spawn) {
            out[i] = dn;
            continue;
        }
        double dx = P[3 * i] - o->spawn[0], dy = P[3 * i + 1] - o->spawn[1], dz = P[3 * i + 2] - o->spawn[2];
        double ds = mx_sqrt(((-0.0 + dx * dx) + dy * dy) + dz * dz);
        double d = np_maximum(o->floor_, np_maximum(dn, o->spawn_k * ds));
        for (int k = 0; k < o->nkeep; k++) {
            const double *kp = o->keep_pos + 3 * k;
            double ex = P[3 * i] - kp[0], ez = P[3 * i + 2] - kp[2];
            if (ex * ex + ez * ez < o->keep_r2)
                d = np_minimum(d, o->keep_d);
        }
        out[i] = d;
    }
    free(S), free(cells), free(dc);
}

/* ---- struct_weight: simplify.structure_weight (same operations in the same order; the reasoning lives there) ---- */
static void sw_sub(const double *a, const double *b, double *r) { r[0] = a[0] - b[0], r[1] = a[1] - b[1], r[2] = a[2] - b[2]; }
static double sw_dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void struct_weight(int npos, const double *pos, const int *wpos, int ntri, const int *tris, double w,
                          double flat_w, double crease_cos, double corner_cos, double *out)
{
    for (int v = 0; v < npos; v++)
        out[v] = 1.0;
    if ((w <= 1.0 && flat_w == 1.0) || !npos)
        return;
    int *ft = MX_NEW(int, 3 * (size_t)(ntri ? ntri : 1)), nf = 0;
    double *fu = MX_NEW(double, 3 * (size_t)(ntri ? ntri : 1));
    double *acc = MX_ZNEW(double, 3 * (size_t)npos);
    int *ninc = MX_ZNEW(int, (size_t)npos + 1);
    for (int t = 0; t < ntri; t++) {
        int p0 = wpos[tris[3 * t]], p1 = wpos[tris[3 * t + 1]], p2 = wpos[tris[3 * t + 2]];
        if (p0 == p1 || p1 == p2 || p0 == p2)
            continue;
        double e1[3], e2[3], n[3];
        sw_sub(pos + 3 * p1, pos + 3 * p0, e1), sw_sub(pos + 3 * p2, pos + 3 * p0, e2);
        n[0] = e1[1] * e2[2] - e1[2] * e2[1], n[1] = e1[2] * e2[0] - e1[0] * e2[2], n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        double ln = sqrt(sw_dot(n, n));
        if (ln == 0.0)
            continue;
        ft[3 * nf] = p0, ft[3 * nf + 1] = p1, ft[3 * nf + 2] = p2;
        fu[3 * nf] = n[0] / ln, fu[3 * nf + 1] = n[1] / ln, fu[3 * nf + 2] = n[2] / ln;
        for (int k = 0; k < 3; k++) {
            double *a = acc + 3 * ft[3 * nf + k];
            a[0] += n[0], a[1] += n[1], a[2] += n[2];
            ninc[ft[3 * nf + k] + 1]++;
        }
        nf++;
    }
    for (int v = 0; v < npos; v++)  /* incident triangles per vertex, ascending (CSR) */
        ninc[v + 1] += ninc[v];
    int *inc = MX_NEW(int, 3 * (size_t)(nf ? nf : 1)), *fill = MX_NEW(int, (size_t)npos);
    memcpy(fill, ninc, sizeof(int) * (size_t)npos);
    for (int f = 0; f < nf; f++)
        for (int k = 0; k < 3; k++)
            inc[fill[ft[3 * f + k]]++] = f;
    for (int v = 0; v < npos; v++) {
        const int *fs = inc + ninc[v];
        int n = ninc[v + 1] - ninc[v], crease = 0, corner = 0;
        if (!n)
            continue;
        for (int i = 0; i < n && !crease; i++)
            for (int j = 0; j < i; j++) {
                const int *a = ft + 3 * fs[i], *b = ft + 3 * fs[j];
                int share = 0;
                for (int x = 0; x < 3 && !share; x++)
                    share = a[x] != v && (a[x] == b[0] || a[x] == b[1] || a[x] == b[2]);
                if (share && sw_dot(fu + 3 * fs[i], fu + 3 * fs[j]) <= crease_cos) {
                    crease = 1;
                    break;
                }
            }
        const double *a = acc + 3 * v;
        double ln = sqrt(sw_dot(a, a));
        if (ln > 0.0) {
            double nv[3] = {a[0] / ln, a[1] / ln, a[2] / ln};
            for (int i = 0; i < n; i++)
                if (sw_dot(nv, fu + 3 * fs[i]) <= corner_cos) {
                    corner = 1;
                    break;
                }
        }
        int k = crease + corner;
        out[v] = k == 2 ? w * w : (k == 1 ? w : flat_w);
    }
    free(ft), free(fu), free(acc), free(ninc), free(inc), free(fill);
}

/* ---- simplify_meshes ---- */
typedef struct {
    int npos, nattr, ntri;
    double *pos;          /* npos x 3 */
    int *pmesh, *ppi;     /* per position: source mesh and its position index (the first corner with that key) */
    long long *pkid;      /* per position: global key id */
    int *wpos;            /* per attribute: job position */
    int *amesh, *aw;      /* per attribute: (mesh, wedge) */
    int *tris;            /* ntri x 3 attributes */
    unsigned char *locked;
    double *scale;
    double *weight;       /* structure weight per position (struct_weight) or NULL */
    int kind;             /* 0 no cells, 1 "far", 2 cell key */
    long long ckey[3];
    sresult *log;
    /* replay */
    int nout;
    int *out;             /* nout x 6 (p, a) */
    int *oid;             /* per output triangle: its source triangle (log T0 index) */
    int ksteps;           /* log steps the last replay applied */
    int kcap;             /* mx_simp_back_off: at most this many steps (-1: no cap) */
    int *fail, nfail;     /* mx_simp_check: source ids (oid) of the failing triangles of the last check */
} sjob;

typedef struct mx_simp {
    int nm, njobs, ngroups;
    long long full;
    unsigned char *dup;
    sjob *jobs;
    double border_w, seam_w, seam_k2, ak2, min_cos;
    double struct_w, flat_w, sat, crease_cos, corner_cos;  /* simplify.STRUCT_W, STRUCT_FLAT_W, STRUCT_CREASE_COS, ..._CORNER_COS */
    double eps, eps_far;
    int use_far;
    const double *job_eps;  /* mx_simp_replay_jobs: per job threshold, else NULL */
    long long nout;
} mx_simp;

typedef struct {
    int n;
    int c[3];
} ctri;  /* a triangle as the set of its corner key ids: sorted, distinct, padded with -1 */

static int cmp_ctri(const void *a, const void *b)
{
    const ctri *x = a, *y = b;
    for (int k = 0; k < 3; k++)
        if (x->c[k] != y->c[k])
            return x->c[k] < y->c[k] ? -1 : 1;
    return 0;
}

static int uf_root(int *par, int x)
{
    while (par[x] != x) {
        par[x] = par[par[x]];
        x = par[x];
    }
    return x;
}

static void run_job(void *vc, long long i)
{
    mx_simp *h = vc;
    sjob *j = h->jobs + i;
    j->log = simplify_run_grp(j->npos, j->pos, j->nattr, j->wpos, j->ntri, j->tris, j->locked, j->scale, j->weight, h->sat,
                              h->border_w, h->seam_w, h->seam_k2, h->ak2, h->min_cos, j->amesh);
    mx_ok(j->log, 1);
}

typedef struct {
    mx_simp *h;
    const int *order;
} run_ord;

static void run_job_ord(void *vc, long long i)
{
    run_ord *o = vc;
    run_job(o->h, o->order[i]);
}

static const int *g_sz;  /* only read by the sort below, on the calling thread */
static int cmp_size_desc(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return g_sz[x] != g_sz[y] ? (g_sz[x] > g_sz[y] ? -1 : 1) : (x < y ? -1 : x > y);
}

/* Everything of export_map.simplify_meshes up to the threshold search, for nm meshes given flat:
   positions P (3 per position, mesh mi's at poff[mi]..poff[mi+1]), wedge -> position W (woff), source triangles T
   (3 wedges each, toff in triangles), keep flags, texture ids ti. cells: cut by CHUNK_GRID cells (grid), the nfar
   far cell keys (3 each) make one uncut "far" job per group. View distance: navmesh nv / nt, spawn, floor, SPAWN_K,
   KEEP_POS (nkeep x 3, KEEP_R^2, KEEP_D). Simplifier constants as simplify_native passes them. NULL on bad input. */
MX_API mx_simp *mx_simp_new(int nm, const double *P, const long long *poff, const int *W, const long long *woff,
                            const int *T, const long long *toff, const unsigned char *keep, const int *ti, int cells,
                            double grid, long long nfar, const long long *far, long long nnv, const double *nv,
                            long long nnt, const int *nt, const double *spawn, double floor_, double spawn_k, int nkeep,
                            const double *keep_pos, double keep_r2, double keep_d, double border_w, double seam_w,
                            double seam_k2, double ak2, double min_cos, double struct_w, double flat_w, double sat, double crease_cos,
                            double corner_cos)
{
    if (nm < 0)
        return NULL;
    mx_simp *h = MX_ZNEW(mx_simp, 1);
    h->nm = nm;
    h->border_w = border_w, h->seam_w = seam_w, h->seam_k2 = seam_k2, h->ak2 = ak2, h->min_cos = min_cos;
    h->struct_w = struct_w, h->flat_w = flat_w, h->sat = sat, h->crease_cos = crease_cos, h->corner_cos = corner_cos;
    h->dup = MX_ZNEW(unsigned char, nm);
    long long npt = poff[nm], nwt = woff[nm];
    /* key() of every non-kept position -> dense key id */
    long long *kid = MX_NEW(long long, npt ? npt : 1);
    mx_map km;
    mx_map_init(&km, 3, npt);
    for (int mi = 0; mi < nm; mi++) {
        if (keep[mi])
            continue;
        for (long long p = poff[mi]; p < poff[mi + 1]; p++) {
            long long k[3];
            for (int a = 0; a < 3; a++)
                k[a] = (long long)mx_rint(P[3 * p + a] * 500);
            int added;
            kid[p] = mx_map_put(&km, k, &added);
        }
    }
    long long nk = km.n;
    mx_map_free(&km);
    /* identical copies: (ti, set of triangles as sets of keys) seen before -> emptied */
    {
        ctri **cs = MX_ZNEW(ctri *, nm);
        long long *cn = MX_ZNEW(long long, nm);
        mx_map seen;  /* (ti, count, hash) -> first mesh */
        mx_map_init(&seen, 3, 64);
        long long *first = MX_NEW(long long, nm ? 2 * nm : 1);  /* per seen id: head mesh; per mesh: next */
        long long *head = first, *next = first + nm;
        for (int mi = 0; mi < nm; mi++) {
            if (keep[mi])
                continue;
            long long n = toff[mi + 1] - toff[mi];
            ctri *c = MX_NEW(ctri, n ? n : 1);
            for (long long t = 0; t < n; t++) {
                int v[3];
                for (int q = 0; q < 3; q++)
                    v[q] = (int)kid[poff[mi] + W[woff[mi] + T[3 * (toff[mi] + t) + q]]];
                for (int x = 1; x < 3; x++)
                    for (int y = x; y > 0 && v[y - 1] > v[y]; y--) {
                        int s = v[y];
                        v[y] = v[y - 1], v[y - 1] = s;
                    }
                c[t].n = 0;
                c[t].c[0] = c[t].c[1] = c[t].c[2] = -1;
                for (int q = 0; q < 3; q++)
                    if (!c[t].n || c[t].c[c[t].n - 1] != v[q])
                        c[t].c[c[t].n++] = v[q];
                c[t].n = 0;
            }
            if (n > 1)
                qsort(c, (size_t)n, sizeof(ctri), cmp_ctri);
            long long m = 0;
            for (long long t = 0; t < n; t++)
                if (!m || cmp_ctri(c + t, c + m - 1))
                    c[m++] = c[t];
            uint64_t hs = 1469598103934665603ull;
            for (long long t = 0; t < m; t++)
                for (int q = 0; q < 3; q++)
                    hs = (hs ^ (uint64_t)(uint32_t)c[t].c[q]) * 1099511628211ull;
            cs[mi] = c, cn[mi] = m;
            long long key[3] = {ti[mi], m, (long long)(hs >> 1)};
            int added;
            long long id = mx_map_put(&seen, key, &added);
            if (added) {
                if (id >= nm)
                    __builtin_trap();
                head[id] = mi;
                next[mi] = -1;
                continue;
            }
            int found = 0;
            long long last = -1;
            for (long long o = head[id]; o >= 0; last = o, o = next[o])
                if (!memcmp(cs[o], c, sizeof(ctri) * (size_t)m)) {
                    found = 1;
                    break;
                }
            if (found)
                h->dup[mi] = 1;  /* the earlier copy is already in the chain: same key */
            else
                next[last] = mi, next[mi] = -1;
        }
        for (int mi = 0; mi < nm; mi++)
            free(cs[mi]);
        free(cs), free(cn), free(first);
        mx_map_free(&seen);
    }
    /* live meshes welded by shared keys; groups in order of their first member */
    int *live = MX_NEW(int, nm ? nm : 1), nlive = 0;
    for (int mi = 0; mi < nm; mi++)
        if (!keep[mi] && !h->dup[mi] && toff[mi + 1] > toff[mi])
            live[nlive++] = mi;
    int *par = MX_NEW(int, nm ? nm : 1);
    for (int mi = 0; mi < nm; mi++)
        par[mi] = mi;
    {
        int *firstm = MX_NEW(int, nk ? nk : 1);
        memset(firstm, 0xff, sizeof(int) * (size_t)(nk ? nk : 1));
        for (int l = 0; l < nlive; l++) {
            int mi = live[l];
            for (long long p = poff[mi]; p < poff[mi + 1]; p++) {
                long long k = kid[p];
                if (firstm[k] >= 0)
                    par[uf_root(par, mi)] = uf_root(par, firstm[k]);
                else
                    firstm[k] = mi;
            }
        }
        free(firstm);
    }
    int *gid = MX_NEW(int, nm ? nm : 1), *gstart = MX_NEW(int, nlive + 1), *gmem = MX_NEW(int, nlive ? nlive : 1);
    int ng = 0;
    {
        int *rg = MX_NEW(int, nm ? nm : 1), *cnt = MX_ZNEW(int, nlive + 1);
        memset(rg, 0xff, sizeof(int) * (size_t)(nm ? nm : 1));
        for (int l = 0; l < nlive; l++) {
            int r = uf_root(par, live[l]);
            if (rg[r] < 0)
                rg[r] = ng++;
            gid[live[l]] = rg[r];
            cnt[rg[r]]++;
        }
        gstart[0] = 0;
        for (int g = 0; g < ng; g++)
            gstart[g + 1] = gstart[g] + cnt[g], cnt[g] = gstart[g];
        for (int l = 0; l < nlive; l++)
            gmem[cnt[gid[live[l]]]++] = live[l];
        free(rg), free(cnt);
    }
    h->ngroups = ng;
    /* far cell keys */
    mx_map farm;
    mx_map_init(&farm, 3, nfar);
    for (long long i = 0; i < nfar; i++) {
        int added;
        mx_map_put(&farm, far + 3 * i, &added);
    }
    /* per group: cells in order of first appearance, one job each */
    int capj = 16;
    h->jobs = MX_ZNEW(sjob, capj);
    int *pmap = MX_NEW(int, nk ? nk : 1), *amap = MX_NEW(int, nwt ? nwt : 1);
    memset(pmap, 0xff, sizeof(int) * (size_t)(nk ? nk : 1));
    memset(amap, 0xff, sizeof(int) * (size_t)(nwt ? nwt : 1));
    int *njk = MX_ZNEW(int, nk ? nk : 1);
    for (int g = 0; g < ng; g++) {
        long long nit = 0;
        for (int q = gstart[g]; q < gstart[g + 1]; q++)
            nit += toff[gmem[q] + 1] - toff[gmem[q]];
        int *im = MX_NEW(int, nit), *icell = MX_NEW(int, nit);
        long long *it = MX_NEW(long long, nit);
        mx_map cm;
        mx_map_init(&cm, 3, 16);
        int ncell = 0, *ckind = MX_NEW(int, nit ? nit : 1);
        long long s = 0;
        for (int q = gstart[g]; q < gstart[g + 1]; q++) {
            int mi = gmem[q];
            for (long long t = toff[mi]; t < toff[mi + 1]; t++, s++) {
                im[s] = mi, it[s] = t;
                if (!cells) {
                    icell[s] = 0;
                    if (!ncell)
                        ckind[ncell++] = 0;
                    continue;
                }
                long long c[3];
                for (int a = 0; a < 3; a++) {
                    double x[3];
                    for (int k = 0; k < 3; k++)
                        x[k] = P[3 * (poff[mi] + W[woff[mi] + T[3 * t + k]]) + a];
                    c[a] = (long long)mx_floor(mx_py_sum3(x[0], x[1], x[2]) / 3 / grid);
                }
                int isfar = mx_map_get(&farm, c) >= 0, added;
                long long fk[3] = {INT64_MIN, INT64_MIN, INT64_MIN};
                long long id = mx_map_put(&cm, isfar ? fk : c, &added);
                if (added)
                    ckind[ncell++] = isfar ? 1 : 2;
                icell[s] = (int)id;
            }
        }
        /* items by cell, stable */
        int *cs = MX_ZNEW(int, ncell + 1), *ord = MX_NEW(int, nit ? nit : 1);
        for (long long i = 0; i < nit; i++)
            cs[icell[i] + 1]++;
        for (int c = 0; c < ncell; c++)
            cs[c + 1] += cs[c];
        {
            int *fill = MX_NEW(int, ncell ? ncell : 1);
            memcpy(fill, cs, sizeof(int) * (size_t)(ncell ? ncell : 1));
            for (long long i = 0; i < nit; i++)
                ord[fill[icell[i]]++] = (int)i;
            free(fill);
        }
        for (int c = 0; c < ncell; c++) {
            if (h->njobs == capj)
                capj *= 2, h->jobs = mx_realloc(h->jobs, sizeof(sjob) * (size_t)capj);
            sjob *j = h->jobs + h->njobs++;
            memset(j, 0, sizeof *j);
            int n = cs[c + 1] - cs[c];
            j->kind = ckind[c];
            if (j->kind == 2)
                memcpy(j->ckey, cm.keys + 3 * c, sizeof j->ckey);
            j->ntri = n;
            j->tris = MX_NEW(int, 3 * (size_t)(n ? n : 1));
            int cap = 3 * n;
            j->pos = MX_NEW(double, 3 * (size_t)(cap ? cap : 1));
            j->pmesh = MX_NEW(int, cap ? cap : 1), j->ppi = MX_NEW(int, cap ? cap : 1);
            j->pkid = MX_NEW(long long, cap ? cap : 1);
            j->wpos = MX_NEW(int, cap ? cap : 1), j->amesh = MX_NEW(int, cap ? cap : 1), j->aw = MX_NEW(int, cap ? cap : 1);
            for (int r = 0; r < n; r++) {
                int i = ord[cs[c] + r], mi = im[i];
                long long t = it[i];
                for (int q = 0; q < 3; q++) {
                    int w = T[3 * t + q];
                    long long wa = woff[mi] + w;
                    if (amap[wa] < 0) {
                        int pi = W[wa];
                        long long k = kid[poff[mi] + pi];
                        if (pmap[k] < 0) {
                            pmap[k] = j->npos;
                            memcpy(j->pos + 3 * j->npos, P + 3 * (poff[mi] + pi), 3 * sizeof(double));
                            j->pmesh[j->npos] = mi, j->ppi[j->npos] = pi, j->pkid[j->npos] = k;
                            j->npos++;
                        }
                        amap[wa] = j->nattr;
                        j->wpos[j->nattr] = pmap[k];
                        j->amesh[j->nattr] = mi, j->aw[j->nattr] = w;
                        j->nattr++;
                    }
                    j->tris[3 * r + q] = amap[wa];
                }
            }
            for (int p = 0; p < j->npos; p++)
                pmap[j->pkid[p]] = -1, njk[j->pkid[p]]++;
            for (int a = 0; a < j->nattr; a++)
                amap[woff[j->amesh[a]] + j->aw[a]] = -1;
        }
        free(im), free(icell), free(it), free(ckind), free(cs), free(ord);
        mx_map_free(&cm);
    }
    free(pmap), free(amap);
    mx_map_free(&farm);
    /* locks, view distances, scales */
    long long ntp = 0;
    for (int i = 0; i < h->njobs; i++)
        ntp += h->jobs[i].npos;
    double *AP = MX_NEW(double, 3 * (ntp ? ntp : 1)), *D = MX_NEW(double, ntp ? ntp : 1);
    long long o = 0;
    for (int i = 0; i < h->njobs; i++) {
        sjob *j = h->jobs + i;
        memcpy(AP + 3 * o, j->pos, sizeof(double) * 3 * (size_t)j->npos);
        o += j->npos;
    }
    vd_opt vo = {floor_, spawn_k, spawn, nkeep, keep_pos, keep_r2, keep_d};
    view_distance(ntp, AP, nnv, nv, nnt, nt, &vo, D);
    o = 0;
    for (int i = 0; i < h->njobs; i++) {
        sjob *j = h->jobs + i;
        j->locked = MX_NEW(unsigned char, j->npos ? j->npos : 1);
        j->scale = MX_NEW(double, j->npos ? j->npos : 1);
        if (h->struct_w > 1.0 || h->flat_w != 1.0) {  /* export_map: crease / corner vertices first (struct_sat) */
            j->weight = MX_NEW(double, j->npos ? j->npos : 1);
            struct_weight(j->npos, j->pos, j->wpos, j->ntri, j->tris, h->struct_w, h->flat_w, h->crease_cos,
                          h->corner_cos, j->weight);
        }
        for (int p = 0; p < j->npos; p++, o++) {
            j->locked[p] = njk[j->pkid[p]] > 1;
            j->scale[p] = 1.0 / (D[o] * D[o]);
        }
    }
    free(AP), free(D), free(njk);
    /* simplify every job, largest first */
    int *sz = MX_NEW(int, h->njobs ? h->njobs : 1), *ordj = MX_NEW(int, h->njobs ? h->njobs : 1);
    for (int i = 0; i < h->njobs; i++)
        sz[i] = h->jobs[i].ntri, ordj[i] = i;
    g_sz = sz;
    qsort(ordj, (size_t)h->njobs, sizeof(int), cmp_size_desc);
    run_ord ro = {h, ordj};
    mx_parallel_for(h->njobs, run_job_ord, &ro);
    free(sz), free(ordj);
    for (int i = 0; i < h->njobs; i++)
        h->full += h->jobs[i].log->nt, h->jobs[i].kcap = -1;
    free(kid), free(live), free(par), free(gid), free(gstart), free(gmem);
    (void)uniq_rows;
    return h;
}

/* stats: full (triangles before), welded groups, jobs */
MX_API void mx_simp_stats(const mx_simp *h, long long *out)
{
    out[0] = h->full, out[1] = h->ngroups, out[2] = h->njobs;
}

MX_API void mx_simp_dup(const mx_simp *h, unsigned char *out) { memcpy(out, h->dup, (size_t)h->nm); }

/* per job: cell kind (0 none, 1 far, 2 key) and key */
MX_API void mx_simp_jobs(const mx_simp *h, int *kind, long long *key)
{
    for (int i = 0; i < h->njobs; i++) {
        kind[i] = h->jobs[i].kind;
        memcpy(key + 3 * i, h->jobs[i].ckey, sizeof h->jobs[i].ckey);
    }
}

/* triangles left at threshold eps (every step with cost <= eps, as the masked sum; without the kept meshes) */
MX_API long long mx_simp_count(const mx_simp *h, double eps)
{
    long long n = h->full;
    for (int i = 0; i < h->njobs; i++) {
        const sresult *R = h->jobs[i].log;
        for (int s = 0; s < R->nsteps; s++)
            if (R->cost[s] <= eps)
                n -= R->nrem[s];
    }
    return n;
}

/* per job: triangles left at threshold eps (the budget solve, export_map.py solve_budget: one job is one welded group's
   part of one cell, so a cell's count at a threshold is the sum over its jobs) */
MX_API void mx_simp_job_counts(const mx_simp *h, double eps, long long *out)
{
    for (int i = 0; i < h->njobs; i++) {
        const sresult *R = h->jobs[i].log;
        long long n = R->nt;
        for (int s = 0; s < R->nsteps; s++)
            if (R->cost[s] <= eps)
                n -= R->nrem[s];
        out[i] = n;
    }
}

static void replay_job(void *vc, long long i)
{
    mx_simp *h = vc;
    sjob *j = h->jobs + i;
    const sresult *R = j->log;
    double eps = h->job_eps ? h->job_eps[i] : h->use_far && j->kind == 1 ? h->eps_far : h->eps;
    int k = 0;
    while (k < R->nsteps && !(R->cost[k] > eps))
        k++;
    if (j->kcap >= 0)  /* mx_simp_back_off: the steps it chose (fewer than the last replay's), whatever the threshold */
        k = j->kcap < R->nsteps ? j->kcap : R->nsteps;
    long long nr = 0, nc = 0;
    for (int s = 0; s < k; s++)
        nr += R->nrem[s], nc += R->ncha[s];
    int *Tt = MX_NEW(int, 6 * (size_t)(R->nt ? R->nt : 1));
    memcpy(Tt, R->t0, sizeof(int) * 6 * (size_t)R->nt);
    unsigned char *alive = MX_NEW(unsigned char, R->nt ? R->nt : 1);
    memset(alive, 1, (size_t)(R->nt ? R->nt : 1));
    for (long long r = 0; r < nr; r++)
        alive[R->rem[r]] = 0;
    for (long long c = 0; c < nc; c++) {
        const int *C = R->cha + 4 * c;
        Tt[6 * C[0] + 2 * C[1]] = C[2];
        Tt[6 * C[0] + 2 * C[1] + 1] = C[3];
    }
    int m = 0, *oid = MX_NEW(int, R->nt ? R->nt : 1);
    for (int t = 0; t < R->nt; t++)
        if (alive[t]) {
            oid[m] = t;
            memmove(Tt + 6 * m++, Tt + 6 * t, 6 * sizeof(int));
        }
    free(alive);
    free(j->out), free(j->oid);
    j->out = Tt, j->oid = oid, j->nout = m, j->ksteps = k;
}

/* replay every job's log at eps (far jobs at eps_far when use_far) -> surviving triangle count */
MX_API long long mx_simp_replay(mx_simp *h, double eps, double eps_far, int use_far)
{
    for (int i = 0; i < h->njobs; i++)
        h->jobs[i].kcap = -1;
    h->eps = eps, h->eps_far = eps_far, h->use_far = use_far, h->job_eps = NULL;
    mx_parallel_for(h->njobs, replay_job, h);
    h->nout = 0;
    for (int i = 0; i < h->njobs; i++)
        h->nout += h->jobs[i].nout;
    return h->nout;
}

/* replay with one threshold per job (job_eps[njobs]: the budget solve's per-cell thresholds) -> surviving triangles */
MX_API long long mx_simp_replay_jobs(mx_simp *h, const double *job_eps)
{
    for (int i = 0; i < h->njobs; i++)
        h->jobs[i].kcap = -1;
    h->job_eps = job_eps;
    mx_parallel_for(h->njobs, replay_job, h);
    h->job_eps = NULL;
    h->nout = 0;
    for (int i = 0; i < h->njobs; i++)
        h->nout += h->jobs[i].nout;
    return h->nout;
}

/* the replayed triangles, job by job: job, mesh (of the first corner's attribute), per corner the source position
   (mesh, index) and wedge */
MX_API void mx_simp_fetch(const mx_simp *h, int *tjob, int *tmesh, int *cmesh, int *cpos, int *cw)
{
    long long o = 0;
    for (int i = 0; i < h->njobs; i++) {
        const sjob *j = h->jobs + i;
        for (int t = 0; t < j->nout; t++, o++) {
            const int *r = j->out + 6 * t;
            tjob[o] = i;
            tmesh[o] = j->amesh[r[1]];
            for (int q = 0; q < 3; q++) {
                int p = r[2 * q], a = r[2 * q + 1];
                cmesh[3 * o + q] = j->pmesh[p], cpos[3 * o + q] = j->ppi[p], cw[3 * o + q] = j->aw[a];
            }
        }
    }
}

/* Jobs mask[i] of h take o's replayed result (log, triangles) and o takes h's: both handles were made from the same
   input, so their jobs match one to one (export_map.py: the cells whose spots stay over budget with the shape envelope
   take the run without it). Returns h's surviving triangle count. */
MX_API long long mx_simp_swap_jobs(mx_simp *h, mx_simp *o, const unsigned char *mask)
{
    if (h->njobs != o->njobs)
        return -1;
    for (int i = 0; i < h->njobs; i++) {
        sjob *a = h->jobs + i, *b = o->jobs + i;
        if (!mask[i] || a->nattr != b->nattr || a->npos != b->npos)
            continue;
        sresult *l = a->log;
        int *out = a->out, *oid = a->oid, n = a->nout, k = a->ksteps, kc = a->kcap;
        a->log = b->log, a->out = b->out, a->oid = b->oid, a->nout = b->nout, a->ksteps = b->ksteps, a->kcap = b->kcap;
        b->log = l, b->out = out, b->oid = oid, b->nout = n, b->ksteps = k, b->kcap = kc;
    }
    h->nout = o->nout = 0;
    for (int i = 0; i < h->njobs; i++)
        h->nout += h->jobs[i].nout, o->nout += o->jobs[i].nout;
    return h->nout;
}

/* per job: triangles of the last replay */
MX_API void mx_simp_job_nout(const mx_simp *h, long long *out)
{
    for (int i = 0; i < h->njobs; i++)
        out[i] = h->jobs[i].nout;
}

/* the shape envelope's padding in metres for the next mx_simp_new (tools/native/simplify.c g_env_pad); <= 0: off */
MX_API void mx_simp_envelope(double pad) { simplify_set_envelope(pad); }

/* the triangle guard for the next mx_simp_new (tools/native/simplify.c g_qmin / g_flip_cos); qmin 0, flip_cos -2: off */
MX_API void mx_simp_guard(double qmin, double flip_cos) { simplify_set_guard(qmin, flip_cos); }

/* the guard's shape tolerance in metres for the next mx_simp_new (tools/native/simplify.c g_shape_tol); 0: off */
MX_API void mx_simp_shape(double tol) { simplify_set_shape(tol); }

/* ---- validation (export_map.py SIMPLIFY_VALIDATE): every replayed triangle checked against its source, independent
   of the simplifier's own bookkeeping (its bounds are conservative estimates; this measures the result) ---- */
typedef struct {
    double tol, qmin, flip_cos;
    const unsigned char *only;  /* NULL: every job, else only these (the others keep bad 0 and their counts) */
    unsigned char *bad;
    long long (*cnt)[5];  /* per job: degenerate, thin, flipped, off shape, worst distance (mm) */
} vchk;

static void cp_tri(const double *p, const double *a, const double *b, const double *c, double *o)
{  /* closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5) */
    double ab[3], ac[3], ap[3], bp[3], cp[3];
    for (int k = 0; k < 3; k++)
        ab[k] = b[k] - a[k], ac[k] = c[k] - a[k], ap[k] = p[k] - a[k], bp[k] = p[k] - b[k], cp[k] = p[k] - c[k];
#define D3(x, y) (x[0] * y[0] + x[1] * y[1] + x[2] * y[2])
    double d1 = D3(ab, ap), d2 = D3(ac, ap), d3 = D3(ab, bp), d4 = D3(ac, bp), d5 = D3(ab, cp), d6 = D3(ac, cp);
#undef D3
    double v, w;
    if (d1 <= 0 && d2 <= 0) { memcpy(o, a, 24); return; }
    if (d3 >= 0 && d4 <= d3) { memcpy(o, b, 24); return; }
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        v = d1 / (d1 - d3);
        for (int k = 0; k < 3; k++) o[k] = a[k] + v * ab[k];
        return;
    }
    if (d6 >= 0 && d5 <= d6) { memcpy(o, c, 24); return; }
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        w = d2 / (d2 - d6);
        for (int k = 0; k < 3; k++) o[k] = a[k] + w * ac[k];
        return;
    }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; k++) o[k] = b[k] + w * (c[k] - b[k]);
        return;
    }
    double den = va + vb + vc;
    if (!(den > 0)) { memcpy(o, a, 24); return; }  /* zero area: a corner will do */
    v = vb / den, w = vc / den;
    for (int k = 0; k < 3; k++) o[k] = a[k] + ab[k] * v + ac[k] * w;
}

typedef struct { unsigned long long key; int t; } gent;
static int cmp_gent(const void *a, const void *b)
{
    const gent *x = a, *y = b;
    return x->key != y->key ? (x->key < y->key ? -1 : 1) : (x->t > y->t) - (x->t < y->t);
}
static unsigned long long gkey(long long x, long long y, long long z)
{
    return ((unsigned long long)(x & 0x1fffff) << 42) | ((unsigned long long)(y & 0x1fffff) << 21) | (unsigned long long)(z & 0x1fffff);
}

static void tri_nq(const double *a, const double *b, const double *c, double *n, double *q)
{  /* cross product n and shape quality q (tools/native/simplify.c tri_q) */
    double e0[3], e1[3], e2[3];
    for (int k = 0; k < 3; k++) e0[k] = b[k] - a[k], e1[k] = c[k] - b[k], e2[k] = a[k] - c[k];
    double f[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    n[0] = e0[1] * f[2] - e0[2] * f[1], n[1] = e0[2] * f[0] - e0[0] * f[2], n[2] = e0[0] * f[1] - e0[1] * f[0];
    double l = e0[0] * e0[0] + e0[1] * e0[1] + e0[2] * e0[2] + e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]
               + e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
    double ln = mx_sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    *q = l > 0 ? 3.4641016151377544 * ln / l : 0.0;
}

static void check_job(void *vc, long long i);
static void back_off_job(void *vc, long long i);
static void back_off_ord(void *vc, long long i)
{
    run_ord *o = vc;
    back_off_job(o->h, o->order[i]);
}
static mx_simp *g_vh;  /* the handle check_job reads (mx_simp_check runs one at a time) */

static void check_job(void *vc, long long i)
{
    vchk *c = vc;
    sjob *j = g_vh->jobs + i;
    const sresult *R = j->log;
    long long *cnt = c->cnt[i];
    if (c->only && !c->only[i])
        return;
    memset(cnt, 0, sizeof c->cnt[i]);
    j->nfail = 0;
    if (!j->ksteps)
        return;  /* nothing collapsed: the source itself */
    /* the source triangles in a grid of cs cells (by bounding box), to find those near a point: cs at least the
       tolerance (so the 27 cells around a point hold every triangle within it) and 1 / 128 of the job's extent (an
       uncut far job spans hundreds of metres); triangles over more than 64 cells go on a list checked for every point */
    double cs = c->tol > 0.5 ? c->tol : 0.5;
    {
        double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
        for (int p = 0; p < j->npos; p++)
            for (int k = 0; k < 3; k++) {
                double x = j->pos[3 * p + k];
                lo[k] = x < lo[k] ? x : lo[k], hi[k] = x > hi[k] ? x : hi[k];
            }
        for (int k = 0; k < 3; k++)
            if (j->npos && (hi[k] - lo[k]) / 128 > cs)
                cs = (hi[k] - lo[k]) / 128;
    }
    long long ng = 0, gcap = 0, nbig = 0;
    gent *G = NULL;
    int *big = MX_NEW(int, R->nt ? R->nt : 1);
    for (int t = 0; t < R->nt; t++) {
        const int *r = R->t0 + 6 * t;
        long long lo[3], hi[3];
        for (int k = 0; k < 3; k++) {
            double a = j->pos[3 * r[0] + k], b = j->pos[3 * r[2] + k], d = j->pos[3 * r[4] + k];
            double mn = a < b ? (a < d ? a : d) : (b < d ? b : d), mxv = a > b ? (a > d ? a : d) : (b > d ? b : d);
            lo[k] = (long long)floor(mn / cs), hi[k] = (long long)floor(mxv / cs);
        }
        if ((hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) > 64) {
            big[nbig++] = t;
            continue;
        }
        for (long long x = lo[0]; x <= hi[0]; x++)
            for (long long y = lo[1]; y <= hi[1]; y++)
                for (long long z = lo[2]; z <= hi[2]; z++) {
                    if (ng == gcap) {
                        gcap = gcap ? gcap * 2 : 4096;
                        G = mx_realloc(G, sizeof(gent) * (size_t)gcap);
                    }
                    G[ng].key = gkey(x, y, z), G[ng++].t = t;
                }
    }
    qsort(G, (size_t)ng, sizeof(gent), cmp_gent);
    double worst = 0;
    for (int t = 0; t < j->nout; t++) {
        const int *r = j->out + 6 * t, *s = R->t0 + 6 * j->oid[t];
        const double *a = j->pos + 3 * r[0], *b = j->pos + 3 * r[2], *d = j->pos + 3 * r[4];
        double n[3], n0[3], q, q0;
        tri_nq(a, b, d, n, &q);
        tri_nq(j->pos + 3 * s[0], j->pos + 3 * s[2], j->pos + 3 * s[4], n0, &q0);
        double ln = mx_sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), l0 = mx_sqrt(n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2]);
        int bad = 0;
        if (l0 > 0 && ln <= 1e-6 * l0) {  /* (a source triangle of no area is as it was) */
            cnt[0]++, bad = 1;
        } else {
            if (c->qmin > 0 && q < 0.999 * (q0 < c->qmin ? q0 : c->qmin))
                cnt[1]++, bad = 1;
            if (l0 > 0 && n[0] * n0[0] + n[1] * n0[1] + n[2] * n0[2] < c->flip_cos * ln * l0)
                cnt[2]++, bad = 1;
        }
        /* off shape: the triangle's corners, edge midpoints and centroid each near the source surface of its own mesh
           (a material stretched over another one's ground reads far from its own) */
        int mesh = j->amesh[r[1]];
        double far_ = 0;
        for (int sp = 0; sp < 7 && c->tol > 0; sp++) {
            static const double W[7][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {.5, .5, 0}, {0, .5, .5}, {.5, 0, .5},
                                           {1 / 3., 1 / 3., 1 / 3.}};
            double p[3];
            for (int k = 0; k < 3; k++) p[k] = W[sp][0] * a[k] + W[sp][1] * b[k] + W[sp][2] * d[k];
            long long g[3];
            for (int k = 0; k < 3; k++) g[k] = (long long)floor(p[k] / cs);
            double best = c->tol * c->tol * 4 + 1;
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dz = -1; dz <= 1; dz++) {
                        gent key = {gkey(g[0] + dx, g[1] + dy, g[2] + dz), -1};
                        long long lo = 0, hi = ng;
                        while (lo < hi) {
                            long long m = (lo + hi) / 2;
                            if (cmp_gent(G + m, &key) < 0) lo = m + 1;
                            else hi = m;
                        }
                        for (long long e = lo; e < ng && G[e].key == key.key; e++) {
                            const int *u = R->t0 + 6 * G[e].t;
                            if (j->amesh[u[1]] != mesh)
                                continue;
                            double o[3];
                            cp_tri(p, j->pos + 3 * u[0], j->pos + 3 * u[2], j->pos + 3 * u[4], o);
                            double dd = (o[0] - p[0]) * (o[0] - p[0]) + (o[1] - p[1]) * (o[1] - p[1]) + (o[2] - p[2]) * (o[2] - p[2]);
                            if (dd < best) best = dd;
                        }
                    }
            for (long long e = 0; e < nbig; e++) {
                const int *u = R->t0 + 6 * big[e];
                if (j->amesh[u[1]] != mesh)
                    continue;
                double o[3];
                cp_tri(p, j->pos + 3 * u[0], j->pos + 3 * u[2], j->pos + 3 * u[4], o);
                double dd = (o[0] - p[0]) * (o[0] - p[0]) + (o[1] - p[1]) * (o[1] - p[1]) + (o[2] - p[2]) * (o[2] - p[2]);
                if (dd < best) best = dd;
            }
            if (best > far_) far_ = best;
            if (best > c->tol * c->tol)
                break;
        }
        far_ = mx_sqrt(far_);
        if (far_ > worst) worst = far_;
        if (c->tol > 0 && far_ > c->tol)
            cnt[3]++, bad = 1;
        if (bad) {
            c->bad[i] = 1;
            j->fail = mx_realloc(j->fail, sizeof(int) * (size_t)(j->nfail + 1));
            j->fail[j->nfail++] = j->oid[t];
        }
    }
    cnt[4] = (long long)(worst * 1000);
    free(G), free(big);
}

/* Check the last replay of every job: a triangle fails when it has (almost) no area left where its source triangle
   had some, is thinner than qmin and than its source triangle, has turned further than acos(flip_cos) from its source
   triangle, or has a corner / edge middle / centre further than tol (metres) from every source triangle of its own
   mesh (off its shape, or stretched over another material). bad[njobs]: 1 for jobs with a failing triangle; cnt
   (5 per job): those four counts and the worst distance in mm (tol 0: not measured). Returns the failing jobs. */
MX_API long long mx_simp_check(mx_simp *h, double tol, double qmin, double flip_cos, const unsigned char *only,
                               unsigned char *bad, long long *cnt)
{
    vchk c = {tol, qmin, flip_cos, only, bad, (long long (*)[5])cnt};
    memset(bad, 0, (size_t)h->njobs);
    g_vh = h;
    mx_parallel_for(h->njobs, check_job, &c);
    long long n = 0;
    for (int i = 0; i < h->njobs; i++)
        n += bad[i];
    return n;
}

/* Jobs mask[i] go back to just before the last collapse step that changed one of their failing triangles (the
   last check's), and are replayed: fewer steps of the same log is the same mesh at a finer error, never a new one, and
   0 steps is the source, so repeating check and back-off ends. Returns h's surviving triangle count. */
static void back_off_job(void *vc, long long i)
{
    mx_simp *h = vc;
    sjob *j = h->jobs + i;
    const sresult *R = j->log;
    unsigned char *f = MX_ZNEW(unsigned char, R->nt ? R->nt : 1);
    for (int e = 0; e < j->nfail; e++)
        f[j->fail[e]] = 1;
    long long nc = 0;
    for (int s = 0; s < j->ksteps; s++)
        nc += R->ncha[s];
    int k = -1;
    for (int s = j->ksteps - 1; s >= 0 && k < 0; s--) {
        nc -= R->ncha[s];
        for (int c = 0; c < R->ncha[s]; c++)
            if (f[R->cha[4 * (nc + c)]]) {
                k = s;
                break;
            }
    }
    free(f);
    j->kcap = k >= 0 ? k : j->ksteps / 2;
    replay_job(h, i);
}

MX_API long long mx_simp_back_off(mx_simp *h, const unsigned char *mask)
{
    int *ids = MX_NEW(int, h->njobs ? h->njobs : 1), n = 0;
    for (int i = 0; i < h->njobs; i++)
        if (mask[i])
            ids[n++] = i;
    run_ord ro = {h, ids};
    mx_parallel_for(n, back_off_ord, &ro);
    free(ids);
    h->nout = 0;
    for (int i = 0; i < h->njobs; i++)
        h->nout += h->jobs[i].nout;
    return h->nout;
}

/* Colour refit (export_map.py SIMPLIFY_REFIT): a half-edge collapse leaves each surviving vertex the colour it had in
   the source mesh, a point sample of the baked light, so after many collapses one vertex that sat in a dark joint or
   on a lit edge paints a whole large triangle with it (light and dark shards on walls and floors). Here every
   surviving wedge gets the mean colour of the source wedges that collapsed into it (the log's attribute changes of
   the last replay, followed to their end), weighted by a third of the area of the source triangles at each: the
   surface it now stands for, like a texture mip. Wedges that kept their own attributes across a seam stay apart.
   wrgb: every mesh's wedge colours (mesh mi's at woff[mi]); acc (zeroed by the caller, 4 per wedge): summed r, g, b
   and weight over all jobs; weight 0: not a surviving wedge, or one at a locked position (cell cuts: both levels and
   both cells must agree there), which keeps its colour. */
MX_API void mx_simp_refit(const mx_simp *h, const long long *woff, const int *wrgb, double *acc)
{
    for (int i = 0; i < h->njobs; i++) {
        const sjob *j = h->jobs + i;
        const sresult *R = j->log;
        int na = j->nattr > 0 ? j->nattr : 1;
        int *par = MX_NEW(int, na), *Ta = MX_NEW(int, 3 * (size_t)(R->nt ? R->nt : 1));
        double *sum = MX_ZNEW(double, 4 * (size_t)na);
        unsigned char *done = MX_ZNEW(unsigned char, na);
        for (int a = 0; a < na; a++)
            par[a] = a;
        for (int t = 0; t < R->nt; t++)
            for (int q = 0; q < 3; q++)
                Ta[3 * t + q] = R->t0[6 * t + 2 * q + 1];
        long long nc = 0;
        for (int s = 0; s < j->ksteps; s++)
            nc += R->ncha[s];
        for (long long c = 0; c < nc; c++) {
            const int *C = R->cha + 4 * c;
            int old = Ta[3 * C[0] + C[1]];
            if (old != C[3]) {
                int ra = uf_root(par, old), rb = uf_root(par, C[3]);
                if (ra != rb)
                    par[ra] = rb;
                Ta[3 * C[0] + C[1]] = C[3];
            }
        }
        for (int t = 0; t < R->nt; t++) {
            const int *r = R->t0 + 6 * t;
            const double *a = j->pos + 3 * r[0], *b = j->pos + 3 * r[2], *c = j->pos + 3 * r[4];
            double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            double w = mx_sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) / 6;
            for (int q = 0; q < 3; q++) {
                int at = r[2 * q + 1];
                const int *col = wrgb + 3 * (woff[j->amesh[at]] + j->aw[at]);
                double *o = sum + 4 * (size_t)uf_root(par, at);
                o[0] += w * col[0], o[1] += w * col[1], o[2] += w * col[2], o[3] += w;
            }
        }
        for (int t = 0; t < j->nout; t++)
            for (int q = 0; q < 3; q++) {
                int at = j->out[6 * t + 2 * q + 1];
                if (done[at] || j->locked[j->wpos[at]])
                    continue;  /* locked (cell cuts): the same colour in every level, so a near cell meets a coarse one */
                done[at] = 1;
                const double *o = sum + 4 * (size_t)uf_root(par, at);
                double *d = acc + 4 * (woff[j->amesh[at]] + j->aw[at]);
                d[0] += o[0], d[1] += o[1], d[2] += o[2], d[3] += o[3];
            }
        free(par), free(Ta), free(sum), free(done);
    }
}

MX_API void mx_simp_free(mx_simp *h)
{
    if (!h)
        return;
    for (int i = 0; i < h->njobs; i++) {
        sjob *j = h->jobs + i;
        free(j->pos), free(j->pmesh), free(j->ppi), free(j->pkid), free(j->wpos), free(j->amesh), free(j->aw);
        free(j->tris), free(j->locked), free(j->scale), free(j->weight), free(j->out), free(j->oid), free(j->fail);
        if (j->log)
            simplify_free(j->log);
    }
    free(h->jobs), free(h->dup), free(h);
}
