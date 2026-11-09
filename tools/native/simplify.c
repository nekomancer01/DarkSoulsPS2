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
    char *lock, *border, *dead;
    double (*Q)[11];
    double *errm, *alost, *scale;
    const double *weight;  /* NULL: all 1 */
    double sat;
    int *ver;
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
    s->lost = fabs(da);
    *torn_out = torn;
    *dloc_out = dloc;
    return 1;
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
result *simplify_run(int nv, const double *posd, int nw, const int *wpos, int ntri, const int *tris,
                     const unsigned char *locked, const double *scale, const double *weight, double sat,
                     double border_w, double seam_w, double seam_k2, double ak2, double min_cos) {
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
    s.vt = calloc(nv ? nv : 1, sizeof(ivec));
    s.bnb = calloc(nv ? nv : 1, sizeof(ivec));
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
    for (int p = 0; p < nv; p++) { free(s.vt[p].a); free(s.bnb[p].a); }
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
