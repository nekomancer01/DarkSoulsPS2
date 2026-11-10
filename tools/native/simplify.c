/* C version of tools/simplify.py's simplify() (quadric half-edge collapse), for tools/simplify_native.py.
 *
 * Same algorithm, same floating-point operations in the same order (build with -O2 -ffp-contract=off, no -ffast-math),
 * same tie-breaking (heap order by (cost, u, v, ver u, ver v), a vertex's triangles visited in ascending id order), so
 * it returns the same collapse log as simplify.py, step for step; tools/simplify_native.py --test compares the two.
 * Comments name the simplify.py function / line each part mirrors; the reasoning lives there.
 *
 * Build: cc -O2 -ffp-contract=off -shared -fPIC -o libsimplify.so simplify.c -lm  (simplify_native.py does this)
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
/* out of memory (the in-browser converter's WebAssembly heap has a cap): trap ("RuntimeError: unreachable" in
 * Pyodide, SIGILL natively) instead of writing through NULL, which in WebAssembly is address 0 and corrupts the
 * interpreter's memory (Pyodide then fails later with "Unexpected rtype undefined") */
static void *alloc_ok(void *p, size_t n) {
    if (!p && n)
        __builtin_trap();
    return p;
}
#define malloc(n) alloc_ok(malloc(n), (n))
#define calloc(a, b) alloc_ok(calloc(a, b), (a) * (b))
#define realloc(p, n) alloc_ok(realloc(p, n), (n))

typedef struct { int *a; int n, cap; } ivec;

static void iv_push(ivec *v, int x) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 4;
        v->a = realloc(v->a, sizeof(int) * v->cap);
    }
    v->a[v->n++] = x;
}
static int iv_has(const ivec *v, int x) {
    for (int i = 0; i < v->n; i++)
        if (v->a[i] == x) return 1;
    return 0;
}
static void iv_add(ivec *v, int x) { if (!iv_has(v, x)) iv_push(v, x); }  /* set.add */
static void iv_discard(ivec *v, int x) {                                   /* set.discard */
    for (int i = 0; i < v->n; i++)
        if (v->a[i] == x) { v->a[i] = v->a[--v->n]; return; }
}
static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static void iv_sort(ivec *v) { if (v->n > 1) qsort(v->a, v->n, sizeof(int), cmp_int); }
/* sorted unique copy of v into out */
static void iv_uniq(ivec *v) {
    iv_sort(v);
    int k = 0;
    for (int i = 0; i < v->n; i++)
        if (!k || v->a[k - 1] != v->a[i]) v->a[k++] = v->a[i];
    v->n = k;
}

typedef struct { double x, y, z; } v3;
static v3 sub(v3 a, v3 b) { v3 r = {a.x - b.x, a.y - b.y, a.z - b.z}; return r; }
static v3 cross(v3 a, v3 b) {
    v3 r = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    return r;
}
static double dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double area3(v3 a, v3 b, v3 c) {
    v3 n = cross(sub(b, a), sub(c, a));
    return 0.5 * sqrt(dot(n, n));
}
static double pmax(double a, double b) { return b > a ? b : a; } /* Python max(a, b) */
/* source triangles by grid cell (the triangle guard's shape check, sg_build) */
typedef struct { unsigned long long key; int t; } sgent;
typedef struct { double cs; sgent *g; long long ng; int *big, nbig; } sgrid;

static void plane(double *q, v3 n, v3 p, double w) { /* _plane */
    double a = n.x, b = n.y, c = n.z;
    double d = -(a * p.x + b * p.y + c * p.z);
    q[0] = w * a * a; q[1] = w * a * b; q[2] = w * a * c; q[3] = w * a * d; q[4] = w * b * b; q[5] = w * b * c;
    q[6] = w * b * d; q[7] = w * c * c; q[8] = w * c * d; q[9] = w * d * d; q[10] = w;
}
static void qadd(double *q, const double *r) { for (int i = 0; i < 11; i++) q[i] += r[i]; }
static double qeval(const double *q, v3 p) { /* _qeval */
    double x = p.x, y = p.y, z = p.z;
    return (q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z
            + 2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9]);
}

typedef struct { double c; int u, v, vu, vv; } hent;
static int hless(const hent *a, const hent *b) {
    if (a->c != b->c) return a->c < b->c;
    if (a->u != b->u) return a->u < b->u;
    if (a->v != b->v) return a->v < b->v;
    if (a->vu != b->vu) return a->vu < b->vu;
    return a->vv < b->vv;
}
typedef struct { hent *a; size_t n, cap; } heap;
static void hpush(heap *h, hent e) {
    if (h->n == h->cap) { h->cap = h->cap ? h->cap * 2 : 1024; h->a = realloc(h->a, sizeof(hent) * h->cap); }
    size_t i = h->n++;
    while (i) {
        size_t p = (i - 1) / 2;
        if (!hless(&e, &h->a[p])) break;
        h->a[i] = h->a[p]; i = p;
    }
    h->a[i] = e;
}
static hent hpop(heap *h) {
    hent top = h->a[0], e = h->a[--h->n];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        const hent *best = &e;
        if (l < h->n && hless(&h->a[l], best)) { m = l; best = &h->a[l]; }
        if (r < h->n && hless(&h->a[r], best)) { m = r; }
        if (m == i) break;
        h->a[i] = h->a[m]; i = m;
    }
    if (h->n) h->a[i] = e;
    return top;
}

/* output buffers, owned by the library until simplify_free */
typedef struct {
    int nt;          /* T0 triangles */
    int *t0;         /* nt x 6: p0 a0 p1 a1 p2 a2 */
    int nsteps;
    double *cost;    /* per step: running maximum */
    int *nrem, *ncha;
    int *rem;        /* removed triangle ids, concatenated */
    int *cha;        /* changes (t, k, p, a), concatenated */
    size_t nrem_all, ncha_all;
} result;

typedef struct {
    int nv, nt;
    const v3 *pos;
    int (*TP)[3], (*TA)[3];
    ivec *vt, *bnb;
    ivec *snb;  /* seam neighbours (attribute seams), only while the shape envelope is on; NULL: off */
    char *lock, *border, *dead;
    double (*Q)[11];
    double *errm, *alost, *scale;
    const double *weight;  /* NULL: all 1 */
    double sat;
    int *ver;
    v3 *n0;  /* source triangle unit normals, only while the guard is on; NULL: off */
    double *l0;  /* source triangles' cross product lengths (2 x area), with n0 */
    sgrid sg;            /* source triangles by cell, only while g_shape_tol is on (P0, tmesh set) */
    int (*P0)[3], *tmesh; /* source triangles' positions and mesh (agrp of corner 0) */
    const int *agrp;
    double lost, seam_k2, ak2;
    /* attr_map scratch */
    ivec removed, keep, amk, amv, nu, nv_, opp;
} S;

static int idx3(const int *t, int x) { return t[0] == x ? 0 : t[1] == x ? 1 : 2; }

static int amap_get(S *s, int key, int *val) {
    for (int i = 0; i < s->amk.n; i++)
        if (s->amk.a[i] == key) { *val = s->amv.a[i]; return 1; }
    return 0;
}

/* attr_map(u, v): returns 0 when the attribute map is inconsistent (Python: amap None) */
static int attr_map(S *s, int u, int v, double *torn_out, double *dloc_out) {
    s->removed.n = s->keep.n = s->amk.n = s->amv.n = 0;
    iv_sort(&s->vt[u]);
    ivec *vu = &s->vt[u];
    for (int i = 0; i < vu->n; i++) {
        int t = vu->a[i];
        int *tp = s->TP[t];
        if (tp[0] == v || tp[1] == v || tp[2] == v) {
            iv_push(&s->removed, t);
            int au = s->TA[t][idx3(tp, u)], av = s->TA[t][idx3(tp, v)], cur;
            if (amap_get(s, au, &cur)) {
                if (cur != av) return 0;
            } else {
                iv_push(&s->amk, au); iv_push(&s->amv, av);
            }
        } else
            iv_push(&s->keep, t);
    }
    double torn = 0.0, dloc = 0.0;
    v3 pu = s->pos[u], pv = s->pos[v];
    double da = 0.0;
    for (int i = 0; i < s->removed.n; i++) {
        int *tp = s->TP[s->removed.a[i]];
        da += area3(s->pos[tp[0]], s->pos[tp[1]], s->pos[tp[2]]);
    }
    v3 du = sub(pu, pv);
    for (int i = 0; i < s->keep.n; i++) {
        int t = s->keep.a[i];
        int *tp = s->TP[t];
        v3 p[3] = {s->pos[tp[0]], s->pos[tp[1]], s->pos[tp[2]]};
        double a0 = area3(p[0], p[1], p[2]);
        int k = idx3(tp, u), dummy;
        if (!amap_get(s, s->TA[t][k], &dummy)) torn += a0;
        p[k] = pv;
        da += a0 - area3(p[0], p[1], p[2]);
        v3 n = cross(sub(p[1], p[0]), sub(p[2], p[0]));
        double ln = dot(n, n);
        if (ln > 0) dloc = pmax(dloc, fabs(dot(n, du)) / sqrt(ln));
    }
    if (s->border[u]) {
        ivec *b = &s->bnb[u];
        for (int i = 0; i < b->n; i++) {
            int x = b->a[i];
            if (x != v) {
                v3 e = sub(s->pos[x], pv);
                double le = dot(e, e);
                if (le > 0) {
                    v3 c = cross(du, e);
                    dloc = pmax(dloc, sqrt(dot(c, c) / le));
                }
            }
        }
    }
    if (s->snb && s->snb[u].n) {
        /* the envelope along attribute seams (a texture's outline on one surface): sliding u along a flat surface
         * moves no plane, so the plane term above reads 0 while the outline of what u's texture covers moves (a board
         * lying on the floor stretched into a sliver across the courtyard, play report 2026-11-10); bound the move
         * across each seam line as the border term does across the mesh's open edges */
        ivec *b = &s->snb[u];
        for (int i = 0; i < b->n; i++) {
            int x = b->a[i];
            if (x != v) {
                v3 e = sub(s->pos[x], pv);
                double le = dot(e, e);
                if (le > 0) {
                    v3 c = cross(du, e);
                    dloc = pmax(dloc, sqrt(dot(c, c) / le));
                }
            }
        }
    }
    s->lost = fabs(da);
    *torn_out = torn;
    *dloc_out = dloc;
    return 1;
}

/* Shape envelope (export_map.py SIMPLIFY_ENV_*): a collapse whose bound on how far the surface around the vertex has
 * moved from the source mesh (errm, metres) passes this padding is not taken at any threshold, so however far a memory
 * budget pushes a cell's error, its shapes stay within the padding of the original. Set by the caller before a run
 * (one value for all its jobs); infinity: off, the log simplify.py gives. */
static double g_env_pad = 1e300;
void simplify_set_envelope(double pad) { g_env_pad = pad > 0 ? pad : 1e300; }

/* Triangle guard (export_map.py SIMPLIFY_GUARD, on by default there): hard limits on what any collapse may leave, which
 * no cost or budget overrides, so a cell pushed to the caps keeps fewer triangles instead of bad ones.
 * g_qmin: a triangle's shape quality q = 4 sqrt(3) area / (sum of squared edges) (1 equilateral, about sqrt(3) h / L for
 * a thin one) may not fall below g_qmin through a collapse unless it got no worse (source slivers stay as they are, no
 * triangle is made thinner than g_qmin: no stretched slivers). g_flip_cos: a triangle's normal may not turn further
 * than acos(g_flip_cos) from its source triangle's over all collapses together (MIN_COS bounds one step only; this
 * bounds the sum: nothing faces away from the surface it came from). Off (the log simplify.py gives): g_qmin 0,
 * g_flip_cos -2. */
static double g_qmin = 0.0, g_flip_cos = -2.0, g_shape_tol = 0.0;
void simplify_set_guard(double qmin, double flip_cos) { g_qmin = qmin > 0 ? qmin : 0.0; g_flip_cos = flip_cos; }
/* g_shape_tol (metres, 0: off; needs the per-attribute source mesh agrp): a triangle may not reach further than this
 * from every source triangle of its own mesh (its corners, edge middles and centre are measured): a material may not
 * be stretched over ground another one covers, or across a gap of its own shape (play report 2026-11-10: a board on
 * the cobbles). The envelope above bounds moves off planes and across outlines; this measures the result itself. */
void simplify_set_shape(double tol) { g_shape_tol = tol > 0 ? tol : 0.0; }

/* the source triangles in a grid of cs cells by bounding box (cs >= the tolerance, so the 27 cells around a point hold
 * every triangle within it; >= 1/128 of the extent, for uncut far jobs); triangles over more than 64 cells on a list */
static unsigned long long sg_key(long long x, long long y, long long z) {
    return ((unsigned long long)(x & 0x1fffff) << 42) | ((unsigned long long)(y & 0x1fffff) << 21) | (unsigned long long)(z & 0x1fffff);
}
static int sg_cmp(const void *a, const void *b) {
    const sgent *x = a, *y = b;
    return x->key != y->key ? (x->key < y->key ? -1 : 1) : (x->t > y->t) - (x->t < y->t);
}
static void cp_tri3(v3 p, v3 a, v3 b, v3 c, v3 *o) {  /* closest point on abc to p (Ericson 5.1.5) */
    v3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a), bp = sub(p, b), cp = sub(p, c);
    double d1 = dot(ab, ap), d2 = dot(ac, ap), d3 = dot(ab, bp), d4 = dot(ac, bp), d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d1 <= 0 && d2 <= 0) { *o = a; return; }
    if (d3 >= 0 && d4 <= d3) { *o = b; return; }
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { double v = d1 / (d1 - d3); v3 r = {a.x + v * ab.x, a.y + v * ab.y, a.z + v * ab.z}; *o = r; return; }
    if (d6 >= 0 && d5 <= d6) { *o = c; return; }
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { double w = d2 / (d2 - d6); v3 r = {a.x + w * ac.x, a.y + w * ac.y, a.z + w * ac.z}; *o = r; return; }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        v3 r = {b.x + w * (c.x - b.x), b.y + w * (c.y - b.y), b.z + w * (c.z - b.z)}; *o = r; return;
    }
    double den = va + vb + vc;
    if (!(den > 0)) { *o = a; return; }
    double v = vb / den, w = vc / den;
    v3 r = {a.x + ab.x * v + ac.x * w, a.y + ab.y * v + ac.y * w, a.z + ab.z * v + ac.z * w};
    *o = r;
}
static void sg_build(sgrid *g, int nv, const v3 *pos, int nt, int (*TP)[3], double tol) {
    double cs = tol > 0.5 ? tol : 0.5;
    v3 lo = {1e300, 1e300, 1e300}, hi = {-1e300, -1e300, -1e300};
    for (int p = 0; p < nv; p++) {
        lo.x = pos[p].x < lo.x ? pos[p].x : lo.x, hi.x = pos[p].x > hi.x ? pos[p].x : hi.x;
        lo.y = pos[p].y < lo.y ? pos[p].y : lo.y, hi.y = pos[p].y > hi.y ? pos[p].y : hi.y;
        lo.z = pos[p].z < lo.z ? pos[p].z : lo.z, hi.z = pos[p].z > hi.z ? pos[p].z : hi.z;
    }
    if (nv) {
        double e = pmax(pmax(hi.x - lo.x, hi.y - lo.y), hi.z - lo.z) / 128;
        cs = pmax(cs, e);
    }
    g->cs = cs, g->ng = 0, g->nbig = 0;
    long long cap = 0;
    g->g = NULL;
    g->big = malloc(sizeof(int) * (nt ? nt : 1));
    for (int t = 0; t < nt; t++) {
        v3 a = pos[TP[t][0]], b = pos[TP[t][1]], c = pos[TP[t][2]];
        long long l[3], h[3];
        double mn[3] = {fmin(a.x, fmin(b.x, c.x)), fmin(a.y, fmin(b.y, c.y)), fmin(a.z, fmin(b.z, c.z))};
        double mx[3] = {fmax(a.x, fmax(b.x, c.x)), fmax(a.y, fmax(b.y, c.y)), fmax(a.z, fmax(b.z, c.z))};
        for (int k = 0; k < 3; k++) l[k] = (long long)floor(mn[k] / cs), h[k] = (long long)floor(mx[k] / cs);
        if ((h[0] - l[0] + 1) * (h[1] - l[1] + 1) * (h[2] - l[2] + 1) > 64) { g->big[g->nbig++] = t; continue; }
        for (long long x = l[0]; x <= h[0]; x++)
            for (long long y = l[1]; y <= h[1]; y++)
                for (long long z = l[2]; z <= h[2]; z++) {
                    if (g->ng == cap) { cap = cap ? cap * 2 : 4096; g->g = realloc(g->g, sizeof(sgent) * (size_t)cap); }
                    g->g[g->ng].key = sg_key(x, y, z), g->g[g->ng++].t = t;
                }
    }
    if (g->ng > 1) qsort(g->g, (size_t)g->ng, sizeof(sgent), sg_cmp);
}
/* squared distance from p to the nearest source triangle t (T0 positions P0) with tmesh[t] == mesh, capped at lim2 */
static double sg_d2(const sgrid *g, v3 p, const v3 *pos, int (*P0)[3], const int *tmesh, int mesh, double lim2) {
    double best = lim2;
    long long c[3] = {(long long)floor(p.x / g->cs), (long long)floor(p.y / g->cs), (long long)floor(p.z / g->cs)};
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
                unsigned long long key = sg_key(c[0] + dx, c[1] + dy, c[2] + dz);
                long long lo = 0, hi = g->ng;
                while (lo < hi) { long long m = (lo + hi) / 2; if (g->g[m].key < key) lo = m + 1; else hi = m; }
                for (long long e = lo; e < g->ng && g->g[e].key == key; e++) {
                    int t = g->g[e].t;
                    if (tmesh[t] != mesh) continue;
                    v3 o; cp_tri3(p, pos[P0[t][0]], pos[P0[t][1]], pos[P0[t][2]], &o);
                    v3 d = sub(o, p);
                    double dd = dot(d, d);
                    if (dd < best) { best = dd; if (best == 0) return 0; }
                }
            }
    for (int e = 0; e < g->nbig; e++) {
        int t = g->big[e];
        if (tmesh[t] != mesh) continue;
        v3 o; cp_tri3(p, pos[P0[t][0]], pos[P0[t][1]], pos[P0[t][2]], &o);
        v3 d = sub(o, p);
        double dd = dot(d, d);
        if (dd < best) best = dd;
    }
    return best;
}
/* 1 when one of the triangle's corners, edge middles or centre is further than tol from its mesh's source surface */
static int sg_off(const sgrid *g, v3 a, v3 b, v3 c, const v3 *pos, int (*P0)[3], const int *tmesh, int mesh, double tol) {
    static const double W[7][3] = {{1 / 3., 1 / 3., 1 / 3.}, {.5, .5, 0}, {0, .5, .5}, {.5, 0, .5}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double t2 = tol * tol;
    for (int s = 0; s < 7; s++) {
        v3 p = {W[s][0] * a.x + W[s][1] * b.x + W[s][2] * c.x, W[s][0] * a.y + W[s][1] * b.y + W[s][2] * c.y,
                W[s][0] * a.z + W[s][1] * b.z + W[s][2] * c.z};
        if (sg_d2(g, p, pos, P0, tmesh, mesh, t2 * 1.0001 + 1e-12) > t2) return 1;
    }
    return 0;
}
static double tri_q(v3 a, v3 b, v3 c, double cross_len) {  /* 4 sqrt(3) area / sum of squared edges, cross_len = |n| */
    v3 e0 = sub(b, a), e1 = sub(c, b), e2 = sub(a, c);
    double l = dot(e0, e0) + dot(e1, e1) + dot(e2, e2);
    return l > 0 ? 3.4641016151377544 * cross_len / l : 0.0;  /* 4 sqrt(3) * (cross_len / 2) / l */
}

static void push(S *s, heap *h, int u, int v) {
    if (s->lock[u]) return;
    if (s->border[u] && !iv_has(&s->bnb[u], v)) return;
    double torn, dloc;
    if (!attr_map(s, u, v, &torn, &dloc)) return;
    double w = s->Q[u][10] + s->Q[v][10];
    v3 pv = s->pos[v];
    double e = w > 0 ? (qeval(s->Q[u], pv) + qeval(s->Q[v], pv) + s->seam_k2 * torn) / w : 0.0;
    double b = pmax(s->errm[v], s->errm[u] + dloc);
    if (b > g_env_pad) return;  /* the surface would leave the envelope around the source shape: never collapsed */
    double m = e, bb = b * b, ar = s->ak2 * (s->alost[u] + s->alost[v] + s->lost);
    if (bb > m) m = bb;
    if (ar > m) m = ar;
    double c = m * pmax(s->scale[u], s->scale[v]);
    if (s->weight && c < s->sat) {  /* simplify.py: structure weight, saturating at sat */
        double wc = c * pmax(s->weight[u], s->weight[v]);
        c = wc < s->sat ? wc : s->sat;
    }
    hent he = {c, u, v, s->ver[u], s->ver[v]};
    hpush(h, he);
}

static void neighbours(S *s, int u, ivec *out) {
    out->n = 0;
    ivec *vu = &s->vt[u];
    for (int i = 0; i < vu->n; i++) {
        int *tp = s->TP[vu->a[i]];
        for (int k = 0; k < 3; k++)
            if (tp[k] != u) iv_push(out, tp[k]);
    }
    iv_uniq(out);
}

/* edge table in insertion order (Python dict et) */
typedef struct { int a, b, n, t0, t1; } edge;

static uint64_t ekey(int a, int b) { return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; }
static uint64_t hmix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x;
}

#define GROW(ptr, n, cap, type) do { if ((n) == (cap)) { (cap) = (cap) ? (cap) * 2 : 1024; \
    (ptr) = realloc((ptr), sizeof(type) * (cap)); } } while (0)

/* pos: nv x 3 doubles; wpos: nw; tris: ntri x 3 attribute vertices; locked: nv bytes or NULL; scale: nv;
 * weight: nv or NULL, sat (simplify.py's weight / sat).
 * Constants from simplify.py: border_w, seam_w, seam_k2 (SEAM_K^2), ak2 ((AREA_K or area_k)^2), min_cos. */
result *simplify_run_grp(int nv, const double *posd, int nw, const int *wpos, int ntri, const int *tris,
                         const unsigned char *locked, const double *scale, const double *weight, double sat,
                         double border_w, double seam_w, double seam_k2, double ak2, double min_cos, const int *agrp);
result *simplify_run(int nv, const double *posd, int nw, const int *wpos, int ntri, const int *tris,
                     const unsigned char *locked, const double *scale, const double *weight, double sat,
                     double border_w, double seam_w, double seam_k2, double ak2, double min_cos) {
    return simplify_run_grp(nv, posd, nw, wpos, ntri, tris, locked, scale, weight, sat, border_w, seam_w, seam_k2, ak2,
                            min_cos, NULL);
}
/* agrp: per attribute vertex its source mesh (one material each), or NULL. With the shape envelope on, an edge between
 * two triangles of different source meshes is a material outline the envelope holds (snb); UV seams inside one mesh
 * are not (bounding those too took the Asylum's near level to 28 % of spots over the room) */
result *simplify_run_grp(int nv, const double *posd, int nw, const int *wpos, int ntri, const int *tris,
                         const unsigned char *locked, const double *scale, const double *weight, double sat,
                         double border_w, double seam_w, double seam_k2, double ak2, double min_cos, const int *agrp) {
    (void)nw;
    result *R = calloc(1, sizeof(result));
    S s;
    memset(&s, 0, sizeof s);
    s.nv = nv;
    s.pos = (const v3 *)posd;
    s.seam_k2 = seam_k2;
    s.ak2 = ak2;
    /* T0: input triangles minus those with repeated positions */
    int nt = 0;
    R->t0 = malloc(sizeof(int) * 6 * (ntri ? ntri : 1));
    for (int i = 0; i < ntri; i++) {
        const int *t = tris + 3 * i;
        int p0 = wpos[t[0]], p1 = wpos[t[1]], p2 = wpos[t[2]];
        if (p0 == p1 || p1 == p2 || p0 == p2) continue;
        int *o = R->t0 + 6 * nt++;
        o[0] = p0; o[1] = t[0]; o[2] = p1; o[3] = t[1]; o[4] = p2; o[5] = t[2];
    }
    R->nt = nt;
    s.nt = nt;
    s.TP = malloc(sizeof(int[3]) * (nt ? nt : 1));
    s.TA = malloc(sizeof(int[3]) * (nt ? nt : 1));
    for (int i = 0; i < nt; i++)
        for (int k = 0; k < 3; k++) { s.TP[i][k] = R->t0[6 * i + 2 * k]; s.TA[i][k] = R->t0[6 * i + 2 * k + 1]; }
    if (g_flip_cos > -1.0 || g_qmin > 0) {
        s.n0 = malloc(sizeof(v3) * (nt ? nt : 1));
        s.l0 = malloc(sizeof(double) * (nt ? nt : 1));
        for (int i = 0; i < nt; i++) {
            v3 p0 = s.pos[s.TP[i][0]], p1 = s.pos[s.TP[i][1]], p2 = s.pos[s.TP[i][2]];
            v3 n = cross(sub(p1, p0), sub(p2, p0));
            double ln = sqrt(dot(n, n));
            v3 z = {0, 0, 0}, nn = {n.x / ln, n.y / ln, n.z / ln};
            s.n0[i] = ln > 0 ? nn : z;  /* zero area source triangle: no direction to keep */
            s.l0[i] = ln;
        }
    }
    if (g_shape_tol > 0 && agrp) {
        s.agrp = agrp;
        s.P0 = malloc(sizeof(int[3]) * (nt ? nt : 1));
        s.tmesh = malloc(sizeof(int) * (nt ? nt : 1));
        for (int i = 0; i < nt; i++) {
            memcpy(s.P0[i], s.TP[i], sizeof(int[3]));
            s.tmesh[i] = agrp[s.TA[i][0]];
        }
        sg_build(&s.sg, nv, s.pos, nt, s.P0, g_shape_tol);
    }
    s.vt = calloc(nv ? nv : 1, sizeof(ivec));
    s.bnb = calloc(nv ? nv : 1, sizeof(ivec));
    s.snb = g_env_pad < 1e299 && agrp ? calloc(nv ? nv : 1, sizeof(ivec)) : NULL;
    for (int i = 0; i < nt; i++)
        for (int k = 0; k < 3; k++) iv_add(&s.vt[s.TP[i][k]], i);
    /* et */
    size_t hcap = 16;
    while (hcap < (size_t)nt * 6 + 16) hcap <<= 1;
    int *htab = malloc(sizeof(int) * hcap);
    memset(htab, 0xff, sizeof(int) * hcap);
    edge *E = NULL;
    int ne = 0, ecap = 0;
    for (int i = 0; i < nt; i++)
        for (int k = 0; k < 3; k++) {
            int a = s.TP[i][k], b = s.TP[i][(k + 1) % 3];
            if (b < a) { int x = a; a = b; b = x; }
            uint64_t key = ekey(a, b);
            size_t j = hmix(key) & (hcap - 1);
            while (htab[j] >= 0 && ekey(E[htab[j]].a, E[htab[j]].b) != key) j = (j + 1) & (hcap - 1);
            if (htab[j] < 0) {
                GROW(E, ne, ecap, edge);
                E[ne].a = a; E[ne].b = b; E[ne].n = 0; E[ne].t0 = E[ne].t1 = -1;
                htab[j] = ne++;
            }
            edge *e = &E[htab[j]];
            if (e->n == 0) e->t0 = i;
            else if (e->n == 1) e->t1 = i;
            e->n++;
        }
    free(htab);
    s.lock = calloc(nv ? nv : 1, 1);
    s.border = calloc(nv ? nv : 1, 1);
    s.dead = calloc(nv ? nv : 1, 1);
    s.Q = calloc(nv ? nv : 1, sizeof(double[11]));
    for (int i = 0; i < nt; i++) {
        v3 p0 = s.pos[s.TP[i][0]], p1 = s.pos[s.TP[i][1]], p2 = s.pos[s.TP[i][2]];
        v3 n = cross(sub(p1, p0), sub(p2, p0));
        double ln = sqrt(dot(n, n));
        if (ln < 1e-12) continue;
        v3 nn = {n.x / ln, n.y / ln, n.z / ln};
        double q[11];
        plane(q, nn, p0, ln * 0.5);
        for (int k = 0; k < 3; k++) qadd(s.Q[s.TP[i][k]], q);
    }
    for (int ei = 0; ei < ne; ei++) {
        edge *ed = &E[ei];
        int a = ed->a, b = ed->b;
        double wgt;
        if (ed->n > 2) { s.lock[a] = s.lock[b] = 1; continue; }
        if (ed->n == 2) {
            int *tp0 = s.TP[ed->t0], *ta0 = s.TA[ed->t0], *tp1 = s.TP[ed->t1], *ta1 = s.TA[ed->t1];
            if (ta0[idx3(tp0, a)] == ta1[idx3(tp1, a)] && ta0[idx3(tp0, b)] == ta1[idx3(tp1, b)]) continue;
            if (s.snb && agrp[ta0[idx3(tp0, a)]] != agrp[ta1[idx3(tp1, a)]]) { iv_add(&s.snb[a], b); iv_add(&s.snb[b], a); }
            wgt = seam_w;
        } else {
            s.border[a] = s.border[b] = 1;
            iv_add(&s.bnb[a], b);
            iv_add(&s.bnb[b], a);
            wgt = border_w;
        }
        int *tp = s.TP[ed->t0];
        v3 p0 = s.pos[tp[0]], p1 = s.pos[tp[1]], p2 = s.pos[tp[2]];
        v3 fn = cross(sub(p1, p0), sub(p2, p0));
        v3 e = sub(s.pos[b], s.pos[a]);
        v3 en = cross(e, fn);
        double ln = sqrt(dot(en, en));
        if (ln < 1e-12) continue;
        v3 enn = {en.x / ln, en.y / ln, en.z / ln};
        double q[11];
        plane(q, enn, s.pos[a], wgt * dot(e, e));
        qadd(s.Q[a], q);
        qadd(s.Q[b], q);
    }
    for (int p = 0; p < nv; p++)
        if (s.bnb[p].n && (s.bnb[p].n > 2 || (locked && locked[p]))) s.lock[p] = 1;
    s.errm = calloc(nv ? nv : 1, sizeof(double));
    s.alost = calloc(nv ? nv : 1, sizeof(double));
    s.ver = calloc(nv ? nv : 1, sizeof(int));
    s.scale = (double *)scale;
    s.weight = weight, s.sat = sat;
    heap h = {0};
    for (int ei = 0; ei < ne; ei++) {
        push(&s, &h, E[ei].a, E[ei].b);
        push(&s, &h, E[ei].b, E[ei].a);
    }
    free(E);
    size_t scap = 0, rcap = 0, ccap = 0;
    int ns = 0;
    double runmax = 0.0;
    ivec chg = {0};
    while (h.n) {
        hent he = hpop(&h);
        int u = he.u, v = he.v;
        if (s.dead[u] || s.dead[v] || s.ver[u] != he.vu || s.ver[v] != he.vv) continue;
        double torn, dloc;
        if (!attr_map(&s, u, v, &torn, &dloc) || !s.removed.n) continue;
        /* link condition */
        s.opp.n = 0;
        for (int i = 0; i < s.removed.n; i++) {
            int *tp = s.TP[s.removed.a[i]];
            for (int k = 0; k < 3; k++)
                if (tp[k] != u && tp[k] != v) iv_push(&s.opp, tp[k]);
        }
        iv_uniq(&s.opp);
        neighbours(&s, u, &s.nu);
        neighbours(&s, v, &s.nv_);
        {
            int i = 0, j = 0, k = 0, ok = 1;
            while (i < s.nu.n && j < s.nv_.n) {  /* intersection, compared with opp as we go */
                if (s.nu.a[i] < s.nv_.a[j]) i++;
                else if (s.nu.a[i] > s.nv_.a[j]) j++;
                else {
                    if (k >= s.opp.n || s.opp.a[k] != s.nu.a[i]) { ok = 0; break; }
                    k++; i++; j++;
                }
            }
            if (!ok || k != s.opp.n) continue;
        }
        v3 pv = s.pos[v];
        chg.n = 0;
        int ok = 1;
        for (int i = 0; i < s.keep.n; i++) {
            int t = s.keep.a[i];
            int *tp = s.TP[t];
            v3 p[3] = {s.pos[tp[0]], s.pos[tp[1]], s.pos[tp[2]]};
            v3 old = cross(sub(p[1], p[0]), sub(p[2], p[0]));
            int k = idx3(tp, u);
            int a = s.TA[t][k], na;
            if (!amap_get(&s, a, &na)) na = a;
            iv_push(&chg, t); iv_push(&chg, k); iv_push(&chg, v); iv_push(&chg, na);
            p[k] = pv;
            v3 nw_ = cross(sub(p[1], p[0]), sub(p[2], p[0]));
            double lo = dot(old, old), ln = dot(nw_, nw_);
            if (ln <= 1e-14 * pmax(lo, 1e-12) || dot(old, nw_) < min_cos * sqrt(lo * ln)) { ok = 0; break; }
            if (g_qmin > 0) {  /* the guard: no thinner than g_qmin unless no thinner than before */
                double qn = tri_q(p[0], p[1], p[2], sqrt(ln));
                if (qn < g_qmin) {
                    v3 q0 = s.pos[tp[0]], q1 = s.pos[tp[1]], q2 = s.pos[tp[2]];
                    if (qn < tri_q(q0, q1, q2, sqrt(lo))) { ok = 0; break; }
                }
            }
            if (s.n0 && dot(s.n0[t], nw_) < g_flip_cos * sqrt(ln)) { ok = 0; break; }  /* the guard: turned from source */
            if (s.n0 && sqrt(ln) <= 1e-5 * s.l0[t]) { ok = 0; break; }  /* the guard: (almost) no area left of its source's */
            if (s.P0) {  /* the guard: still on its own mesh's source surface (the mesh of corner 0's new attribute) */
                int mesh = s.agrp[k == 0 ? na : s.TA[t][0]];
                if (sg_off(&s.sg, p[0], p[1], p[2], s.pos, s.P0, s.tmesh, mesh, g_shape_tol)) { ok = 0; break; }
            }
        }
        if (!ok) continue;
        /* apply */
        for (int i = 0; i < s.removed.n; i++) {
            int t = s.removed.a[i];
            for (int k = 0; k < 3; k++) iv_discard(&s.vt[s.TP[t][k]], t);
        }
        for (int i = 0; i < chg.n; i += 4) {
            int t = chg.a[i], k = chg.a[i + 1];
            s.TP[t][k] = chg.a[i + 2];
            s.TA[t][k] = chg.a[i + 3];
            iv_add(&s.vt[v], t);
        }
        s.vt[u].n = 0;
        s.dead[u] = 1;
        qadd(s.Q[v], s.Q[u]);
        s.errm[v] = pmax(s.errm[v], s.errm[u] + dloc);
        s.alost[v] += s.alost[u] + s.lost;
        s.ver[v] += 1;
        if (s.border[u]) {
            ivec *b = &s.bnb[u];
            for (int i = 0; i < b->n; i++) {
                int x = b->a[i];
                iv_discard(&s.bnb[x], u);
                if (x != v) { iv_add(&s.bnb[x], v); iv_add(&s.bnb[v], x); }
            }
        }
        if (s.snb) {  /* u's seam lines now end at v */
            ivec *b = &s.snb[u];
            for (int i = 0; i < b->n; i++) {
                int x = b->a[i];
                iv_discard(&s.snb[x], u);
                if (x != v) { iv_add(&s.snb[x], v); iv_add(&s.snb[v], x); }
            }
        }
        runmax = pmax(runmax, he.c);
        /* record the step */
        if ((size_t)ns == scap) {
            scap = scap ? scap * 2 : 1024;
            R->cost = realloc(R->cost, sizeof(double) * scap);
            R->nrem = realloc(R->nrem, sizeof(int) * scap);
            R->ncha = realloc(R->ncha, sizeof(int) * scap);
        }
        R->cost[ns] = runmax;
        R->nrem[ns] = s.removed.n;
        R->ncha[ns] = chg.n / 4;
        ns++;
        while (R->nrem_all + s.removed.n > rcap) { rcap = rcap ? rcap * 2 : 4096; R->rem = realloc(R->rem, sizeof(int) * rcap); }
        memcpy(R->rem + R->nrem_all, s.removed.a, sizeof(int) * s.removed.n);
        R->nrem_all += s.removed.n;
        while (R->ncha_all + chg.n > ccap) { ccap = ccap ? ccap * 2 : 4096; R->cha = realloc(R->cha, sizeof(int) * ccap); }
        memcpy(R->cha + R->ncha_all, chg.a, sizeof(int) * chg.n);
        R->ncha_all += chg.n;
        /* costs of the edges at v changed */
        neighbours(&s, v, &s.nu);
        ivec nb = {0};
        for (int i = 0; i < s.nu.n; i++) iv_push(&nb, s.nu.a[i]);
        for (int i = 0; i < nb.n; i++) {
            push(&s, &h, nb.a[i], v);
            push(&s, &h, v, nb.a[i]);
        }
        free(nb.a);
    }
    R->nsteps = ns;
    for (int p = 0; p < nv; p++) { free(s.vt[p].a); free(s.bnb[p].a); if (s.snb) free(s.snb[p].a); }
    free(s.snb);
    free(s.n0);
    free(s.l0);
    if (s.P0) { free(s.P0); free(s.tmesh); free(s.sg.g); free(s.sg.big); }
    free(s.vt); free(s.bnb); free(s.TP); free(s.TA); free(s.lock); free(s.border); free(s.dead); free(s.Q);
    free(s.errm); free(s.alost); free(s.ver); free(h.a); free(chg.a);
    free(s.removed.a); free(s.keep.a); free(s.amk.a); free(s.amv.a); free(s.nu.a); free(s.nv_.a); free(s.opp.a);
    return R;
}

void simplify_free(result *R) {
    if (!R) return;
    free(R->t0); free(R->cost); free(R->nrem); free(R->ncha); free(R->rem); free(R->cha);
    free(R);
}
