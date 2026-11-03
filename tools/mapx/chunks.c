/* export_map.main's flatten loop + build_chunks in C: every drawn entry's corners welded into vertices, triangles
   grouped by (texture, flags, cell, draw group, entity, relief key), the relief groups under BUMP_MIN_GROUP merged
   (export_normal.merge_small), split_batch's median cuts, foliage tufts by density layer, and the quantised s16 / u16 /
   u8 streams. Python's dict orders (first appearance), min() / max() (first of equal values) and sum() are kept. */
#include "mapx.h"

double atan2(double, double);
double log2(double);

static double np_maximum(double a, double b) { return a >= b || a != a ? a : b; }

/* frexp's exponent of a finite r > 0 and its mantissa in [0.5, 1) */
static int fr_exp(double r, double *mant)
{
    union { double d; uint64_t u; } v = {r};
    int e = (int)((v.u >> 52) & 0x7ff);
    if (!e) {  /* subnormal */
        v.d = r * 18014398509481984.0;  /* 2^54 */
        e = (int)((v.u >> 52) & 0x7ff) - 54;
    }
    union { double d; uint64_t u; } m = {.u = (v.u & ~(0x7ffull << 52)) | (1022ull << 52)};
    *mant = m.d;
    return e - 1022;
}

static double ldexp1(int e)  /* 2^e, -1022 <= e <= 1023 */
{
    union { uint64_t u; double d; } v = {.u = (uint64_t)(e + 1023) << 52};
    return v.d;
}

typedef struct {
    int ti;
    long long fl;
    int ntri;
    int kind;            /* 0 plain (CHUNK_TAG None), 1 stream (level, cell), 2 foliage layer (1, sc), 3 foliage, no sc */
    long long tag[4];    /* level, cell x, y, z */
    int dg, ent, bk;     /* ids (bk -1: none) */
    long long t0;        /* first triangle in the batch order (bt) */
    double mn[3], mx[3], org[3];
    int e, sts;
} batch;

typedef struct mx_chunks {
    int nb;
    batch *b;
    long long ntri, nvert;
    long long *bt;       /* triangles of every batch, in batch order */
    unsigned char *pos, *st, *col;  /* padded streams, batch after batch */
    long long *pos_off, *st_off, *col_off;  /* nb + 1 */
    double err;
} mx_chunks;

/* ---- split_batch ---- */
typedef struct {
    const double *C;
    int ax;
} sb_ctx;

static void merge_sort_by(long long *a, long long *tmp, long long n, const double *C, int ax)
{
    /* stable sort of triangle numbers by C[k][ax] (np.argsort kind="stable") */
    if (n < 2)
        return;
    long long h = n / 2;
    merge_sort_by(a, tmp, h, C, ax);
    merge_sort_by(a + h, tmp, n - h, C, ax);
    long long i = 0, j = h, o = 0;
    while (i < h && j < n)
        tmp[o++] = C[3 * a[j] + ax] < C[3 * a[i] + ax] ? a[j++] : a[i++];
    while (i < h)
        tmp[o++] = a[i++];
    while (j < n)
        tmp[o++] = a[j++];
    memcpy(a, tmp, sizeof(long long) * (size_t)n);
}

typedef struct {
    long long *ofs;  /* part starts */
    long long n, cap;
} parts;

static void parts_add(parts *p, long long start)
{
    if (p->n == p->cap)
        p->cap = p->cap ? 2 * p->cap : 64, p->ofs = mx_realloc(p->ofs, sizeof(long long) * (size_t)p->cap);
    p->ofs[p->n++] = start;
}

/* ks[0..n) (at position base of the whole list) -> parts appended to p (start positions); ks reordered in place */
static void split_batch(long long *ks, long long *tmp, long long n, long long base, const double *C, int ct, parts *p)
{
    long long k = (n + ct - 1) / ct;
    if (k <= 1) {
        parts_add(p, base);
        return;
    }
    double lo[3], hi[3];
    for (int a = 0; a < 3; a++)
        lo[a] = hi[a] = C[3 * ks[0] + a];
    for (long long i = 1; i < n; i++)
        for (int a = 0; a < 3; a++) {
            double v = C[3 * ks[i] + a];
            hi[a] = np_maximum(hi[a], v);
            lo[a] = (lo[a] <= v || lo[a] != lo[a]) ? lo[a] : v;
        }
    int ax = 0;
    double best = hi[0] - lo[0];
    for (int a = 1; a < 3; a++)
        if (hi[a] - lo[a] > best)
            best = hi[a] - lo[a], ax = a;
    merge_sort_by(ks, tmp, n, C, ax);
    long long k1 = k / 2;
    long long r = (long long)mx_rint((double)(n * k1) / (double)k), a1 = k1 * ct, cut = n - (k - k1) * ct;
    if (a1 < r)
        r = a1;
    if (r > cut)
        cut = r;
    split_batch(ks, tmp, cut, base, C, ct, p);
    split_batch(ks + cut, tmp, n - cut, base + cut, C, ct, p);
}

typedef struct {
    long long *bt, *tmp;
    const long long *ocnt;
    const double *C;
    int ct;
    parts *gp;
} spctx;

static void sp_group(void *vc, long long o)
{
    spctx *c = vc;
    long long s = c->ocnt[o], n = c->ocnt[o + 1] - s;
    split_batch(c->bt + s, c->tmp + s, n, s, c->C, c->ct, c->gp + o);
}

/* ---- flatten, an entry at a time (threads): sort its corners, number the unique ones, then (fl_fill, after the
   entries' vertex counts are summed into offsets) write the vertices and corner -> vertex ---- */
typedef struct {
    const int *e_ti;
    const long long *e_toff, *e_woff;
    const double *cpos;
    const int *cw, *wrgb, *wa;
    const double *wuv;
    const int *texwh;
    long long *ord, *loc, *ecnt;  /* ord: per corner its order, then the run's first corner per local vertex */
    int *ebad;
    long long *G;
    double *P, *UV;
    unsigned char *VC;
} flctx;

static void fl_sort(void *vc, long long e)
{
    flctx *f = vc;
    long long t0 = f->e_toff[e], n = f->e_toff[e + 1] - t0, nc = 3 * n;
    const double *cp = f->cpos + 9 * t0;
    const int *cwe = f->cw + 3 * t0;
    long long *ord = f->ord + 3 * t0, *loc = f->loc + 3 * t0;
    for (long long i = 0; i < nc; i++)
        ord[i] = i;
    /* sort corners by (x, y, z, wedge), ties (equal values, -0 == 0) by corner order: the run's first corner is
       the one a Python set keeps */
    long long *tmp = MX_NEW(long long, nc ? nc : 1);
    for (long long w = 1; w < nc; w *= 2) {  /* bottom-up merge sort */
        for (long long s = 0; s < nc; s += 2 * w) {
            long long m = s + w < nc ? s + w : nc, en = s + 2 * w < nc ? s + 2 * w : nc, i = s, j = m, o = s;
            while (i < m && j < en) {
                long long x = ord[i], y = ord[j];
                int lt = 0;  /* corner y before corner x */
                for (int a = 0; a < 3; a++)
                    if (cp[3 * y + a] != cp[3 * x + a]) {
                        lt = cp[3 * y + a] < cp[3 * x + a] ? 1 : -1;
                        break;
                    }
                if (!lt && cwe[y] != cwe[x])
                    lt = cwe[y] < cwe[x] ? 1 : -1;
                tmp[o++] = lt > 0 ? ord[j++] : ord[i++];
            }
            while (i < m)
                tmp[o++] = ord[i++];
            while (j < en)
                tmp[o++] = ord[j++];
        }
        memcpy(ord, tmp, sizeof(long long) * (size_t)nc);
    }
    long long prev = -1, nv = 0;
    for (long long r = 0; r < nc; r++) {
        long long c = ord[r];
        int same = prev >= 0 && cwe[c] == cwe[prev];
        for (int a = 0; same && a < 3; a++)
            same = cp[3 * c + a] == cp[3 * prev + a];
        if (!same) {
            tmp[nv++] = c;
            prev = c;
        }
        loc[c] = nv - 1;
    }
    memcpy(ord, tmp, sizeof(long long) * (size_t)nv);
    free(tmp);
    f->ecnt[e] = nv;
    /* texture coordinates: ((u - floor(min u)) * w, (v - floor(min v)) * h), then / (w, h) in build_chunks */
    int ti = f->e_ti[e];
    for (long long t = 0; t < n; t++) {
        double *o = f->UV + 6 * (t0 + t);
        if (ti < 0) {
            memset(o, 0, 6 * sizeof(double));  /* (0, 0) / (1, 1) */
            continue;
        }
        double tw = f->texwh[2 * ti], th = f->texwh[2 * ti + 1];
        const double *uv[3];
        for (int q = 0; q < 3; q++)
            uv[q] = f->wuv + 2 * (f->e_woff[e] + cwe[3 * t + q]);
        double mu = uv[0][0], mv = uv[0][1];
        for (int q = 1; q < 3; q++) {
            mu = uv[q][0] < mu ? uv[q][0] : mu;
            mv = uv[q][1] < mv ? uv[q][1] : mv;
        }
        double du = mx_floor(mu) + 0.0, dv = mx_floor(mv) + 0.0;  /* math.floor: an int */
        for (int q = 0; q < 3; q++)
            o[2 * q] = (uv[q][0] - du) * tw / tw, o[2 * q + 1] = (uv[q][1] - dv) * th / th;
    }
}

static void fl_fill(void *vc, long long e)
{
    flctx *f = vc;
    long long t0 = f->e_toff[e], nc = 3 * (f->e_toff[e + 1] - t0), v0 = f->ecnt[e], nv = f->ecnt[e + 1] - v0;
    const double *cp = f->cpos + 9 * t0;
    const int *cwe = f->cw + 3 * t0;
    const long long *rep = f->ord + 3 * t0, *loc = f->loc + 3 * t0;
    int bad = 0;
    for (long long u = 0; u < nv; u++) {
        long long c = rep[u], v = v0 + u;
        memcpy(f->P + 3 * v, cp + 3 * c, 3 * sizeof(double));
        long long wi = f->e_woff[e] + cwe[c];
        for (int a = 0; a < 4; a++) {
            int x = a < 3 ? f->wrgb[3 * wi + a] : f->wa[wi];
            bad |= x < 0 || x > 255;
            f->VC[4 * v + a] = (unsigned char)x;
        }
    }
    for (long long c = 0; c < nc; c++)
        f->G[3 * t0 + c] = v0 + loc[c];
    f->ebad[e] = bad;
}

typedef struct {
    mx_chunks *h;
    const long long *G;
    const double *P;
    double *PC;
    long long *CG;
    int *cb;
    double pos_max;
} bxctx;

static void bx_batch(void *vc, long long bi)  /* a batch's corners, box, origin and position exponent */
{
    bxctx *k = vc;
    mx_chunks *h = k->h;
    const long long *G = k->G;
    const double *P = k->P;
    double *PC = k->PC, pos_max = k->pos_max;
    long long *CG = k->CG;
    int *cb = k->cb;
    batch *b = h->b + bi;
    for (long long t = b->t0; t < b->t0 + b->ntri; t++)
        for (int q = 0; q < 3; q++) {
            long long c = 3 * t + q, v = G[3 * h->bt[t] + q];
            CG[c] = v, cb[c] = (int)bi;
            memcpy(PC + 3 * c, P + 3 * v, 3 * sizeof(double));
        }
    long long c0 = 3 * b->t0, c1 = 3 * (b->t0 + b->ntri);
    for (int a = 0; a < 3; a++) {
        double lo = PC[3 * c0 + a], hi = lo;
        for (long long c = c0 + 1; c < c1; c++) {
            double v = PC[3 * c + a];
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
        b->mn[a] = lo, b->mx[a] = hi;
        b->org[a] = mx_rint((lo + hi) / 2) + 0.0;
    }
    double m = 0;
    for (int a = 0; a < 3; a++) {
        double x = mx_fabs(b->mx[a] - b->org[a]), y = mx_fabs(b->mn[a] - b->org[a]), v = y > x ? y : x;
        if (!a || v > m)
            m = v;
    }
    int e = -16;
    while (m / ldexp1(e) > pos_max && e < 0)
        e++;
    b->e = e;
}

typedef struct {
    mx_chunks *h;
    const double *Q, *PC, *UV;
    const unsigned char *VC;
    const long long *CG;
    double *berr;
    int *bbad;
} stctx;

static void st_batch(void *vc, long long bi)  /* a batch's position, texture coordinate and colour streams */
{
    stctx *k = vc;
    mx_chunks *h = k->h;
    const double *Q = k->Q, *PC = k->PC, *UV = k->UV;
    const unsigned char *VC = k->VC;
    const long long *CG = k->CG;
    double err = 0;
    int bad = 0;
    batch *b = h->b + bi;
    double scl = ldexp1(b->e);
    long long c0 = 3 * b->t0, c1 = 3 * (b->t0 + b->ntri);
    unsigned char *po = h->pos + h->pos_off[bi], *so = h->st + h->st_off[bi], *co = h->col + h->col_off[bi];
    double smax = 0;
    for (long long c = c0; c < c1; c++) {
        for (int a = 0; a < 3; a++) {
            double nq = mx_rint((Q[3 * c + a] - b->org[a]) / scl);
            double d = mx_fabs(b->org[a] + nq * scl - PC[3 * c + a]);
            err = np_maximum(err, d);
            if (!(nq >= -32768 && nq <= 32767))
                bad = 1;
            int16_t v = (int16_t)nq;
            memcpy(po + 6 * (c - c0) + 2 * a, &v, 2);
        }
        long long t = c / 3;
        const double *uv = UV + 6 * h->bt[t] + 2 * (c % 3);
        double m = np_maximum(uv[0], uv[1]);
        smax = c == c0 ? m : np_maximum(smax, m);
        memcpy(co + 4 * (c - c0), VC + 4 * CG[c], 4);
    }
    b->sts = smax < 65535.0 / 4096 ? 4096 : 1024;
    for (long long c = c0; c < c1; c++) {
        long long t = c / 3;
        const double *uv = UV + 6 * h->bt[t] + 2 * (c % 3);
        for (int a = 0; a < 2; a++) {
            double x = mx_rint(uv[a] * b->sts);
            x = 65535 <= x ? 65535 : x;
            if (!(x >= 0))
                bad = 1;
            uint16_t v = (uint16_t)x;
            memcpy(so + 4 * (c - c0) + 2 * a, &v, 2);
        }
    }
    k->berr[bi] = err;
    k->bbad[bi] = bad;
}

/* ---- the whole stage ----
   ne entries (the drawn ones, in order), entry e's triangles at e_toff[e]..e_toff[e + 1] (global numbers) with
   corners cpos (3 doubles) and cw (entry-local wedge), its wedges at e_woff[e].. (wrgb 3 ints, wa, wuv 2 doubles).
   Per triangle: foliage ftuft (id, -1: none) / frank / fsize, stream cell tlev (-1: none) / tkey (3), draw group dg,
   entity ent, relief key bk (-1: none); has_tcell: build_chunks got tcell. texwh: (w, h) per texture. */
MX_API mx_chunks *mx_chunks_new(int ne, const int *e_ti, const long long *e_fl, const long long *e_toff,
                                const long long *e_woff, const double *cpos, const int *cw, const int *wrgb,
                                const int *wa, const double *wuv, const int *texwh, const long long *ftuft,
                                const double *frank, const int *fsize, const int *tlev, const long long *tkey,
                                const int *dg, const int *ent, const int *bk, int has_tcell, double grid,
                                const double *centre, double grass_grid, double near_r, int chunk_tris,
                                int bump_min, double pos_max, long long f_sky, long long f_grass)
{
    if (ne < 0 || chunk_tris < 1)
        return NULL;
    long long nt = e_toff[ne];
    mx_chunks *h = MX_ZNEW(mx_chunks, 1);
    h->ntri = nt;
    /* flatten: per entry the corners (position, wedge) sorted and made unique -> vertices */
    long long *G = MX_NEW(long long, 3 * (nt ? nt : 1));
    double *P = MX_NEW(double, 9 * (nt ? nt : 1)), *UV = MX_NEW(double, 6 * (nt ? nt : 1));
    unsigned char *VC = MX_NEW(unsigned char, 12 * (nt ? nt : 1));
    long long nv = 0;
    int bad = 0;
    long long *ord = MX_NEW(long long, 3 * (nt ? nt : 1)), *loc = MX_NEW(long long, 3 * (nt ? nt : 1));
    long long *ecnt = MX_NEW(long long, ne + 1);
    int *ebad = MX_ZNEW(int, ne ? ne : 1);
    flctx fc = {e_ti, e_toff, e_woff, cpos, cw, wrgb, wa, wuv, texwh, ord, loc, ecnt, ebad, G, P, UV, VC};
    mx_parallel_for(ne, fl_sort, &fc);  /* entries are independent: their corners, then their vertex numbers */
    for (int e = 0; e < ne; e++) {
        long long c = ecnt[e];
        ecnt[e] = nv;
        nv += c;
    }
    ecnt[ne] = nv;
    mx_parallel_for(ne, fl_fill, &fc);
    for (int e = 0; e < ne; e++)
        bad |= ebad[e];
    free(ord), free(loc), free(ecnt), free(ebad);
    h->nvert = nv;
    /* centroids, the distance-scaled cell */
    double *C = MX_NEW(double, 3 * (nt ? nt : 1));
    for (long long k = 0; k < nt; k++)
        for (int a = 0; a < 3; a++)
            C[3 * k + a] = mx_py_sum3(P[3 * G[3 * k] + a], P[3 * G[3 * k + 1] + a], P[3 * G[3 * k + 2] + a]) / 3;
    /* groups in first-appearance order */
    int *tgrp = MX_NEW(int, nt ? nt : 1);
    mx_map gm, tm;
    mx_map_init(&gm, 10, 64);
    mx_map_init(&tm, 1, 64);
    long long *tuft_of = MX_NEW(long long, nt ? nt : 1);
    for (long long k = 0; k < nt; k++)
        tuft_of[k] = -1;
    /* entry of each triangle */
    int *tent = MX_NEW(int, nt ? nt : 1);
    for (int e = 0; e < ne; e++)
        for (long long k = e_toff[e]; k < e_toff[e + 1]; k++)
            tent[k] = e;
    double *tsum = NULL;
    long long ntuft = 0, captuft = 0;
    long long *tfirst = NULL;
    for (long long k = 0; k < nt; k++) {
        int e = tent[k], ti = e_ti[e];
        long long fl = e_fl[e];
        const double *c = C + 3 * k;
        if (ftuft[k] >= 0) {
            int added;
            long long id = mx_map_put(&tm, ftuft + k, &added);
            if (added) {
                if (ntuft == captuft) {
                    captuft = captuft ? 2 * captuft : 64;
                    tsum = mx_realloc(tsum, sizeof(double) * 3 * (size_t)captuft);
                    tfirst = mx_realloc(tfirst, sizeof(long long) * (size_t)captuft);
                }
                tsum[3 * id] = tsum[3 * id + 1] = tsum[3 * id + 2] = 0.0;
                tfirst[id] = k;
                ntuft++;
            }
            for (int a = 0; a < 3; a++)
                tsum[3 * id + a] = tsum[3 * id + a] + c[a];
            tuft_of[k] = id;
            tgrp[k] = -1;
            continue;
        }
        long long key[10] = {ti, fl, 0, 0, 0, 0, 0, dg[k], ent[k], -1};
        int stream = has_tcell && tlev[k] >= 0;
        if (fl & f_sky) {
            const double pi = 3.141592653589793;
            key[2] = 0;
            key[3] = (long long)((atan2(c[0] - centre[0], c[2] - centre[2]) + pi) / (pi / 4)) % 8;
            key[4] = c[1] > centre[1];
        } else if (stream) {
            key[2] = 1, key[3] = tlev[k];
            memcpy(key + 4, tkey + 3 * k, 3 * sizeof(long long));
        } else {
            double D[3];
            for (int a = 0; a < 3; a++)
                D[a] = c[a] - centre[a];
            double r = mx_sqrt(mx_py_sum3(D[0] * D[0], D[1] * D[1], D[2] * D[2])) / (4 * grid), mant = 0;
            int lvl = 0;
            if (r >= 1.0) {
                lvl = fr_exp(r, &mant) - 1;
                if (mx_fabs(mant - 0.5) < 1e-9 || mx_fabs(mant - 1.0) < 1e-9) {
                    double dist = mx_sqrt(mx_py_sum3(D[0] * D[0], D[1] * D[1], D[2] * D[2])), q = dist / (4 * grid);
                    long long l2 = (long long)log2(q > 1.0 ? q : 1.0);
                    lvl = l2 > 0 ? (int)l2 : 0;
                }
            }
            double gq = grid * ldexp1(lvl);
            union { double d; long long i; } gb = {gq};
            key[2] = 2, key[3] = gb.i;
            for (int a = 0; a < 3; a++)
                key[4 + a] = (long long)mx_floor(c[a] / gq);
        }
        key[9] = bk[k] >= 0 && !(stream && tlev[k] == 0) ? bk[k] : -1;
        int added;
        tgrp[k] = (int)mx_map_put(&gm, key, &added);
    }
    free(tent);
    long long ng = gm.n;
    /* members per group (triangle order), counts */
    long long *gcnt = MX_ZNEW(long long, ng + 1);
    for (long long k = 0; k < nt; k++)
        if (tgrp[k] >= 0)
            gcnt[tgrp[k] + 1]++;
    for (long long g = 0; g < ng; g++)
        gcnt[g + 1] += gcnt[g];
    long long *gmem = MX_NEW(long long, gcnt[ng] ? gcnt[ng] : 1);
    {
        long long *fill = MX_NEW(long long, ng ? ng : 1);
        memcpy(fill, gcnt, sizeof(long long) * (size_t)(ng ? ng : 1));
        for (long long k = 0; k < nt; k++)
            if (tgrp[k] >= 0)
                gmem[fill[tgrp[k]]++] = k;
        free(fill);
    }
    free(tgrp);
    /* merge_small: per (ti, fl, cell) the largest relief group (first of equal sizes); smaller relief groups under
       bump_min go into it. out: groups in order of their destination's first appearance */
    long long *dst = MX_NEW(long long, ng ? ng : 1);
    {
        mx_map bm;
        mx_map_init(&bm, 9, 64);
        long long *best = MX_NEW(long long, ng ? ng : 1);
        for (long long g = 0; g < ng; g++) {
            const long long *K = gm.keys + 10 * g;
            if (K[9] < 0)
                continue;
            int added;
            long long id = mx_map_put(&bm, K, &added);
            if (added || gcnt[g + 1] - gcnt[g] > gcnt[best[id] + 1] - gcnt[best[id]])
                best[id] = g;
        }
        for (long long g = 0; g < ng; g++) {
            const long long *K = gm.keys + 10 * g;
            dst[g] = K[9] >= 0 && gcnt[g + 1] - gcnt[g] < bump_min ? best[mx_map_get(&bm, K)] : g;
        }
        free(best);
        mx_map_free(&bm);
    }
    /* out order: destinations by first appearance in the iteration */
    long long *oorder = MX_NEW(long long, ng ? ng : 1), no = 0;
    long long *opos = MX_NEW(long long, ng ? ng : 1);
    for (long long g = 0; g < ng; g++)
        opos[g] = -1;
    for (long long g = 0; g < ng; g++)
        if (opos[dst[g]] < 0)
            opos[dst[g]] = no, oorder[no++] = dst[g];
    /* each out group's triangle list: the sources in iteration order */
    long long *ocnt = MX_ZNEW(long long, no + 1);
    for (long long g = 0; g < ng; g++)
        ocnt[opos[dst[g]] + 1] += gcnt[g + 1] - gcnt[g];
    for (long long o = 0; o < no; o++)
        ocnt[o + 1] += ocnt[o];
    long long nplain = ocnt[no];
    h->bt = MX_NEW(long long, nt ? nt : 1);
    {
        long long *fill = MX_NEW(long long, no ? no : 1);
        memcpy(fill, ocnt, sizeof(long long) * (size_t)(no ? no : 1));
        for (long long g = 0; g < ng; g++) {
            long long o = opos[dst[g]];
            for (long long i = gcnt[g]; i < gcnt[g + 1]; i++)
                h->bt[fill[o]++] = gmem[i];
        }
        free(fill);
    }
    free(gmem), free(gcnt), free(opos);
    /* split each group */
    int capb = 64;
    h->b = MX_NEW(batch, capb);
    long long *tmp = MX_NEW(long long, nt ? nt : 1);
    parts *gp = MX_ZNEW(parts, no ? no : 1);
    spctx spc = {h->bt, tmp, ocnt, C, chunk_tris, gp};
    mx_parallel_for(no, sp_group, &spc);  /* groups split apart (their triangles, scratch and parts are their own) */
    for (long long o = 0; o < no; o++) {
        parts p = gp[o];
        long long s = ocnt[o], n = ocnt[o + 1] - s;
        const long long *K = gm.keys + 10 * oorder[o];
        for (long long i = 0; i < p.n; i++) {
            if (h->nb == capb)
                capb *= 2, h->b = mx_realloc(h->b, sizeof(batch) * (size_t)capb);
            batch *b = h->b + h->nb++;
            memset(b, 0, sizeof *b);
            b->ti = (int)K[0], b->fl = K[1];
            b->t0 = p.ofs[i];
            b->ntri = (int)((i + 1 < p.n ? p.ofs[i + 1] : s + n) - p.ofs[i]);
            b->kind = K[2] == 1 ? 1 : 0;
            if (b->kind)
                memcpy(b->tag, K + 3, 4 * sizeof(long long));
            b->dg = (int)K[7], b->ent = (int)K[8], b->bk = (int)K[9];
        }
        free(p.ofs);
    }
    free(gp), free(ocnt), free(oorder), free(dst);
    mx_map_free(&gm);
    /* foliage: tufts by (ti, fl, size, GRASS_GRID cell, dg, ent, stream cell), sorted by rank, CHUNK_TRIS / size
       tufts per batch */
    {
        long long *tcnt = MX_ZNEW(long long, ntuft + 1);
        for (long long k = 0; k < nt; k++)
            if (tuft_of[k] >= 0)
                tcnt[tuft_of[k] + 1]++;
        for (long long t = 0; t < ntuft; t++)
            tcnt[t + 1] += tcnt[t];
        long long *tmem = MX_NEW(long long, tcnt[ntuft] ? tcnt[ntuft] : 1);
        long long *fill = MX_NEW(long long, ntuft ? ntuft : 1);
        memcpy(fill, tcnt, sizeof(long long) * (size_t)(ntuft ? ntuft : 1));
        for (long long k = 0; k < nt; k++)
            if (tuft_of[k] >= 0)
                tmem[fill[tuft_of[k]]++] = k;
        free(fill);
        mx_map fm;
        mx_map_init(&fm, 12, 64);
        long long *tf = MX_NEW(long long, ntuft ? ntuft : 1);
        for (long long t = 0; t < ntuft; t++) {
            long long k = tfirst[t], len = tcnt[t + 1] - tcnt[t];
            int e = 0;
            while (!(k >= e_toff[e] && k < e_toff[e + 1]))
                e++;
            long long key[12] = {e_ti[e], e_fl[e], fsize[k], 0, 0, 0, dg[k], ent[k], 0, 0, 0, 0};
            for (int a = 0; a < 3; a++)
                key[3 + a] = (long long)mx_floor(tsum[3 * t + a] / (double)len / grass_grid);
            if (has_tcell && frank[k] >= near_r) {
                key[8] = 1;
                for (int a = 0; a < 3; a++)
                    key[9 + a] = (long long)mx_floor(tsum[3 * t + a] / (double)len / grid);
            }
            int added;
            tf[t] = mx_map_put(&fm, key, &added);
        }
        long long nf = fm.n;
        long long *fcnt = MX_ZNEW(long long, nf + 1), *fmem = MX_NEW(long long, ntuft ? ntuft : 1);
        for (long long t = 0; t < ntuft; t++)
            fcnt[tf[t] + 1]++;
        for (long long f = 0; f < nf; f++)
            fcnt[f + 1] += fcnt[f];
        fill = MX_NEW(long long, nf ? nf : 1);
        memcpy(fill, fcnt, sizeof(long long) * (size_t)(nf ? nf : 1));
        for (long long t = 0; t < ntuft; t++)
            fmem[fill[tf[t]]++] = t;
        free(fill);
        long long o = nplain;
        long long *tt = MX_NEW(long long, ntuft ? ntuft : 1);
        for (long long f = 0; f < nf; f++) {
            const long long *K = fm.keys + 12 * f;
            long long s = fcnt[f], n = fcnt[f + 1] - s;
            /* stable sort of the tufts by rank */
            long long *a = fmem + s;
            for (long long w = 1; w < n; w *= 2) {
                for (long long i0 = 0; i0 < n; i0 += 2 * w) {
                    long long m = i0 + w < n ? i0 + w : n, en = i0 + 2 * w < n ? i0 + 2 * w : n, i = i0, j = m, q = i0;
                    while (i < m && j < en)
                        tt[q++] = frank[tfirst[a[j]]] < frank[tfirst[a[i]]] ? a[j++] : a[i++];
                    while (i < m)
                        tt[q++] = a[i++];
                    while (j < en)
                        tt[q++] = a[j++];
                }
                memcpy(a, tt, sizeof(long long) * (size_t)n);
            }
            long long sz = K[2], per = chunk_tris / (sz > 0 ? sz : 1);
            per = per > 1 ? per : 1;
            for (long long i = 0; i < n; i += per) {
                long long j1 = i + per < n ? i + per : n;
                if (h->nb == capb)
                    capb *= 2, h->b = mx_realloc(h->b, sizeof(batch) * (size_t)capb);
                batch *b = h->b + h->nb++;
                memset(b, 0, sizeof *b);
                double r0 = frank[tfirst[a[i]]], r1 = frank[tfirst[a[j1 - 1]]];
                long long lo = (long long)(r0 * 255), hi = (long long)__builtin_ceil(r1 * 255);
                hi = hi < 255 ? hi : 255;
                b->ti = (int)K[0];
                b->fl = K[1] | f_grass | (sz < 255 ? sz : 255) << 8 | lo << 16 | hi << 24;
                b->t0 = o;
                for (long long u = i; u < j1; u++)
                    for (long long x = tcnt[a[u]]; x < tcnt[a[u] + 1]; x++)
                        h->bt[o++] = tmem[x];
                b->ntri = (int)(o - b->t0);
                b->kind = K[8] ? 2 : 3;
                if (K[8])
                    b->tag[0] = 1, memcpy(b->tag + 1, K + 9, 3 * sizeof(long long));
                b->dg = (int)K[6], b->ent = (int)K[7], b->bk = -1;
            }
        }
        free(tt), free(fcnt), free(fmem), free(tf), free(tcnt), free(tmem);
        mx_map_free(&fm);
    }
    free(tuft_of), free(tsum), free(tfirst), free(tmp);
    mx_map_free(&tm);
    /* streams: boxes, origins, steps */
    int nb = h->nb;
    long long ncorner = 3 * nt;
    double *PC = MX_NEW(double, 3 * (ncorner ? ncorner : 1));
    long long *CG = MX_NEW(long long, ncorner ? ncorner : 1);
    int *cb = MX_NEW(int, ncorner ? ncorner : 1);
    bxctx bx = {h, G, P, PC, CG, cb, pos_max};
    mx_parallel_for(nb, bx_batch, &bx);  /* batches own their corners */
    /* the step of each position (by value): the coarsest among the batches using it */
    long long *cp = MX_NEW(long long, ncorner ? ncorner : 1), npk;
    {
        mx_map pm;
        mx_map_init(&pm, 3, nv);
        long long *vid = MX_NEW(long long, nv ? nv : 1);
        for (long long v = 0; v < nv; v++) {
            union { double d; long long i; } u[3];
            for (int a = 0; a < 3; a++)
                u[a].d = P[3 * v + a] + 0.0;  /* -0 == 0 */
            long long key[3] = {u[0].i, u[1].i, u[2].i};
            int added;
            vid[v] = mx_map_put(&pm, key, &added);
        }
        npk = pm.n;
        mx_map_free(&pm);
        for (long long c = 0; c < ncorner; c++)
            cp[c] = vid[CG[c]];
        free(vid);
    }
    long long *S = MX_NEW(long long, npk ? npk : 1);
    double *Q = MX_NEW(double, 3 * (ncorner ? ncorner : 1));
    for (;;) {
        for (long long i = 0; i < npk; i++)
            S[i] = -99;
        for (long long c = 0; c < ncorner; c++)
            if (h->b[cb[c]].e > S[cp[c]])
                S[cp[c]] = h->b[cb[c]].e;
        int grow = 0;
        double *bm = MX_NEW(double, nb ? nb : 1);
        for (long long c = 0; c < ncorner; c++) {
            double sv = ldexp1((int)S[cp[c]]), m = 0;
            const batch *b = h->b + cb[c];
            for (int a = 0; a < 3; a++) {
                Q[3 * c + a] = mx_rint(PC[3 * c + a] / sv) * sv;
                double d = mx_fabs(Q[3 * c + a] - b->org[a]);
                m = a ? np_maximum(m, d) : d;
            }
            bm[cb[c]] = c == 3 * b->t0 ? m : np_maximum(bm[cb[c]], m);
        }
        for (int bi = 0; bi < nb; bi++)
            if (h->b[bi].ntri && bm[bi] / ldexp1(h->b[bi].e) > 32767)
                h->b[bi].e++, grow = 1;
        free(bm);
        if (!grow)
            break;
    }
    free(S), free(cp);
    /* streams */
    h->pos_off = MX_NEW(long long, nb + 1), h->st_off = MX_NEW(long long, nb + 1), h->col_off = MX_NEW(long long, nb + 1);
    long long sp = 0, ss = 0, sc_ = 0;
    for (int bi = 0; bi < nb; bi++) {
        long long n3 = 3 * (long long)h->b[bi].ntri;
        h->pos_off[bi] = sp, h->st_off[bi] = ss, h->col_off[bi] = sc_;
        sp += (6 * n3 + 15) / 16 * 16, ss += (4 * n3 + 15) / 16 * 16, sc_ += (4 * n3 + 15) / 16 * 16;
    }
    h->pos_off[nb] = sp, h->st_off[nb] = ss, h->col_off[nb] = sc_;
    h->pos = MX_ZNEW(unsigned char, sp), h->st = MX_ZNEW(unsigned char, ss), h->col = MX_ZNEW(unsigned char, sc_);
    double *berr = MX_NEW(double, nb ? nb : 1), err = 0;
    int *bbad = MX_ZNEW(int, nb ? nb : 1);
    stctx sx = {h, Q, PC, UV, VC, CG, berr, bbad};
    mx_parallel_for(nb, st_batch, &sx);
    for (int bi = 0; bi < nb; bi++) {  /* np.maximum over the batches in order (the same max either way) */
        err = np_maximum(err, berr[bi]);
        bad |= bbad[bi];
    }
    free(berr), free(bbad);
    h->err = err;
    free(PC), free(CG), free(cb), free(Q), free(G), free(P), free(UV), free(VC), free(C);
    if (bad) {  /* out of range: the Python code's asserts say which */
        h->nb = -1;
    }
    return h;
}

/* -1: a stream value out of range (run the Python code for its assert) */
MX_API int mx_chunks_count(const mx_chunks *h, long long *out)
{
    out[0] = h->nvert, out[1] = h->pos_off[h->nb > 0 ? h->nb : 0], out[2] = h->st_off[h->nb > 0 ? h->nb : 0];
    out[3] = h->col_off[h->nb > 0 ? h->nb : 0];
    return h->nb;
}

/* per batch: ints (ti, ntri, kind, dg, ent, bk, e, sts, t0), fl, tag (4), doubles (mn, mx, org), stream offsets;
   the triangle list (bt), the streams, the largest position error */
MX_API void mx_chunks_fetch(const mx_chunks *h, int *iv, long long *fl, long long *tag, double *box, long long *offs,
                            long long *bt, unsigned char *pos, unsigned char *st, unsigned char *col, double *err)
{
    for (int bi = 0; bi < h->nb; bi++) {
        const batch *b = h->b + bi;
        int v[9] = {b->ti, b->ntri, b->kind, b->dg, b->ent, b->bk, b->e, b->sts, (int)b->t0};
        memcpy(iv + 9 * bi, v, sizeof v);
        fl[bi] = b->fl;
        memcpy(tag + 4 * bi, b->tag, sizeof b->tag);
        memcpy(box + 9 * bi, b->mn, 3 * sizeof(double));
        memcpy(box + 9 * bi + 3, b->mx, 3 * sizeof(double));
        memcpy(box + 9 * bi + 6, b->org, 3 * sizeof(double));
        offs[3 * bi] = h->pos_off[bi], offs[3 * bi + 1] = h->st_off[bi], offs[3 * bi + 2] = h->col_off[bi];
    }
    if (h->nb >= 0) {
        offs[3 * h->nb] = h->pos_off[h->nb], offs[3 * h->nb + 1] = h->st_off[h->nb], offs[3 * h->nb + 2] = h->col_off[h->nb];
        memcpy(bt, h->bt, sizeof(long long) * (size_t)h->ntri);
        memcpy(pos, h->pos, (size_t)h->pos_off[h->nb]);
        memcpy(st, h->st, (size_t)h->st_off[h->nb]);
        memcpy(col, h->col, (size_t)h->col_off[h->nb]);
    }
    *err = h->err;
}

MX_API void mx_chunks_free(mx_chunks *h)
{
    if (!h)
        return;
    free(h->b), free(h->bt), free(h->pos), free(h->st), free(h->col), free(h->pos_off), free(h->st_off), free(h->col_off);
    free(h);
}
