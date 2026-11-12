/* Loose pieces of a map (tools/mapsimp.py WS_DROP): the connected parts of the simplifier's input (triangles joined
   through shared 2 mm position keys, any mesh), and for each small one how far it stands off the large ones.

   An edge collapse can not remove a closed shell: a barrel, an ivy leaf or a plank ends as a lump of a few triangles
   however far its cell's error threshold is raised, so the budget solve bent the walls around them instead (Undead
   Burg 2026-11-13: at the 12 px cap 35 % of the near level's triangles sat in pieces under 2 m, and raising the cap
   to 68 px took 6 % off the large structure and its outline with it). A piece's stand-off is the error of leaving
   it out whole, on the same scale as a collapse's error: the largest distance from its corners to the structure (the
   pieces over max_diag). Ivy on a wall or a plank on the floor go at a few pixels; a barrel or a merlon stands out by
   its own size and stays until that is small on screen. */
#include "mapx.h"

typedef struct { unsigned long long key; int t; } pg_ent;
static int pg_cmp(const void *a, const void *b)
{
    const pg_ent *x = a, *y = b;
    return x->key != y->key ? (x->key < y->key ? -1 : 1) : (x->t > y->t) - (x->t < y->t);
}
static unsigned long long pg_key(long long x, long long y, long long z)
{
    return ((unsigned long long)(x & 0x1fffff) << 42) | ((unsigned long long)(y & 0x1fffff) << 21) | (unsigned long long)(z & 0x1fffff);
}

static double pt_tri_d2(const double *p, const double *a, const double *b, const double *c)
{  /* squared distance from p to triangle abc (Ericson, Real-Time Collision Detection 5.1.5) */
    double ab[3], ac[3], ap[3], bp[3], cp[3], o[3];
    for (int k = 0; k < 3; k++)
        ab[k] = b[k] - a[k], ac[k] = c[k] - a[k], ap[k] = p[k] - a[k], bp[k] = p[k] - b[k], cp[k] = p[k] - c[k];
#define D3(x, y) (x[0] * y[0] + x[1] * y[1] + x[2] * y[2])
    double d1 = D3(ab, ap), d2 = D3(ac, ap), d3 = D3(ab, bp), d4 = D3(ac, bp), d5 = D3(ab, cp), d6 = D3(ac, cp);
    double vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4, v, w;
    if (d1 <= 0 && d2 <= 0)
        return D3(ap, ap);
    if (d3 >= 0 && d4 <= d3)
        return D3(bp, bp);
    if (d6 >= 0 && d5 <= d6)
        return D3(cp, cp);
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        v = d1 / (d1 - d3);
        for (int k = 0; k < 3; k++) o[k] = ap[k] - v * ab[k];
    } else if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        w = d2 / (d2 - d6);
        for (int k = 0; k < 3; k++) o[k] = ap[k] - w * ac[k];
    } else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; k++) o[k] = bp[k] - w * (c[k] - b[k]);
    } else {
        double den = va + vb + vc;
        if (!(den > 0))
            return D3(ap, ap);  /* zero area: a corner will do */
        v = vb / den, w = vc / den;
        for (int k = 0; k < 3; k++) o[k] = ap[k] - ab[k] * v - ac[k] * w;
    }
    return D3(o, o);
#undef D3
}

static int pc_root(int *par, int x)
{
    while (par[x] != x) {
        par[x] = par[par[x]];
        x = par[x];
    }
    return x;
}

typedef struct {
    const double *P;
    const long long *tg;   /* per structure triangle: 3 global positions */
    const pg_ent *G;
    long long ng;
    const int *big;
    long long nbig;
    double cs, cap2;
    const long long *lt;   /* per live triangle: 3 global positions */
    const int *ppiece;
    const unsigned char *small;
    double *td2;           /* per live triangle of a small piece: squared distance to the structure (capped), its
                              farthest sample */
} prot_ctx;

static double prot_point(const prot_ctx *c, const double *q)
{
    long long g[3];
    for (int k = 0; k < 3; k++)
        g[k] = (long long)mx_floor(q[k] / c->cs);
    double best = c->cap2;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
                unsigned long long key = pg_key(g[0] + dx, g[1] + dy, g[2] + dz);
                long long lo = 0, hi = c->ng;
                while (lo < hi) {
                    long long m = (lo + hi) / 2;
                    if (c->G[m].key < key) lo = m + 1;
                    else hi = m;
                }
                for (long long e = lo; e < c->ng && c->G[e].key == key; e++) {
                    const long long *t = c->tg + 3 * c->G[e].t;
                    double d = pt_tri_d2(q, c->P + 3 * t[0], c->P + 3 * t[1], c->P + 3 * t[2]);
                    if (d < best) best = d;
                }
            }
    for (long long e = 0; e < c->nbig; e++) {
        const long long *t = c->tg + 3 * c->big[e];
        double d = pt_tri_d2(q, c->P + 3 * t[0], c->P + 3 * t[1], c->P + 3 * t[2]);
        if (d < best) best = d;
    }
    return best;
}

static void prot_one(void *vc, long long t)
{
    prot_ctx *c = vc;
    const long long *r = c->lt + 3 * t;
    if (!c->small[c->ppiece[r[0]]])
        return;
    /* corners, edge middles and the centre: a panel that closes a hole in the structure touches it with every corner */
    static const double Wt[7][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {.5, .5, 0}, {0, .5, .5}, {.5, 0, .5},
                                    {1 / 3., 1 / 3., 1 / 3.}};
    const double *a = c->P + 3 * r[0], *b = c->P + 3 * r[1], *d = c->P + 3 * r[2];
    double worst = 0;
    for (int s = 0; s < 7 && worst < c->cap2; s++) {
        double q[3];
        for (int k = 0; k < 3; k++)
            q[k] = Wt[s][0] * a[k] + Wt[s][1] * b[k] + Wt[s][2] * d[k];
        double x = prot_point(c, q);
        if (x > worst) worst = x;
    }
    c->td2[t] = worst;
}

/* The meshes as mx_simp_new takes them (positions P, mesh mi's at poff[mi]..poff[mi+1]; wedge -> position W, woff;
   triangles T of 3 wedges, toff in triangles; keep flags: kept meshes are no part of this).
   solo (per mesh): its pieces stand alone (cut-out cards: ivy leaves), see pclus.
   ppiece (per position): its piece, -1 when no triangle of a simplified mesh uses it. Per piece (the caller sizes
   these for one piece per triangle): pdiag its box diagonal, pbox its box (min xyz, max xyz), pclus its cluster (a
   piece id): loose pieces whose boxes, grown by gap metres, overlap are one thing (the stones of an arch, the planks
   of a floor, a pile of barrels) and stay or go together; pprot its stand-off in metres:
   the largest distance from its triangles' corners, edge middles and centres to a triangle of a piece over max_diag, at most cap; pieces over
   max_diag (the structure) get -1. Returns the number of pieces. */
MX_API long long mx_pieces(int nm, const double *P, const long long *poff, const int *W, const long long *woff,
                           const int *T, const long long *toff, const unsigned char *keep, const unsigned char *solo,
                           double max_diag, double cap, double gap, int *ppiece, double *pdiag, double *pbox, int *pclus,
                           double *pprot)
{
    long long npt = poff[nm], ntt = toff[nm];
    long long *kid = MX_NEW(long long, npt ? npt : 1);
    mx_map km;
    mx_map_init(&km, 3, npt);
    for (long long p = 0; p < npt; p++) {
        long long k[3];
        for (int a = 0; a < 3; a++)
            k[a] = (long long)mx_rint(P[3 * p + a] * 500);
        int added;
        kid[p] = mx_map_put(&km, k, &added);
    }
    long long nk = km.n;
    mx_map_free(&km);
    int *par = MX_NEW(int, nk ? nk : 1);
    unsigned char *used = MX_ZNEW(unsigned char, nk ? nk : 1);
    for (long long k = 0; k < nk; k++)
        par[k] = (int)k;
    long long *tg = MX_NEW(long long, 3 * (ntt ? ntt : 1)), nlive = 0;  /* live triangles' global positions */
    unsigned char *tsolo = MX_ZNEW(unsigned char, ntt ? ntt : 1);
    for (int mi = 0; mi < nm; mi++) {
        if (keep[mi])
            continue;
        for (long long t = toff[mi]; t < toff[mi + 1]; t++, nlive++) {
            int r[3];
            tsolo[nlive] = solo ? solo[mi] : 0;
            for (int q = 0; q < 3; q++) {
                long long p = poff[mi] + W[woff[mi] + T[3 * t + q]];
                tg[3 * nlive + q] = p;
                r[q] = pc_root(par, (int)kid[p]);
                used[kid[p]] = 1;
            }
            if (r[1] != r[0])
                par[r[1]] = r[0];
            r[2] = pc_root(par, r[2]);
            if (r[2] != r[0])
                par[r[2]] = r[0];
        }
    }
    int *pid = MX_NEW(int, nk ? nk : 1);  /* root key -> piece, in order of first position */
    memset(pid, 0xff, sizeof(int) * (size_t)(nk ? nk : 1));
    long long np = 0;
    for (long long p = 0; p < npt; p++) {
        if (!used[kid[p]]) {
            ppiece[p] = -1;
            continue;
        }
        int r = pc_root(par, (int)kid[p]);
        if (pid[r] < 0)
            pid[r] = (int)np++;
        ppiece[p] = pid[r];
    }
    double *lo = MX_NEW(double, 3 * (np ? np : 1)), *hi = MX_NEW(double, 3 * (np ? np : 1));
    for (long long i = 0; i < 3 * np; i++)
        lo[i] = 1e300, hi[i] = -1e300;
    for (long long t = 0; t < 3 * nlive; t++) {
        long long p = tg[t];
        int pc = ppiece[p];
        for (int a = 0; a < 3; a++) {
            double x = P[3 * p + a];
            if (x < lo[3 * pc + a]) lo[3 * pc + a] = x;
            if (x > hi[3 * pc + a]) hi[3 * pc + a] = x;
        }
    }
    unsigned char *small = MX_ZNEW(unsigned char, np ? np : 1), *psolo = MX_ZNEW(unsigned char, np ? np : 1);
    for (long long t = 0; t < nlive; t++)
        if (tsolo[t])
            psolo[ppiece[tg[3 * t]]] = 1;
    for (long long i = 0; i < np; i++) {
        double d[3] = {hi[3 * i] - lo[3 * i], hi[3 * i + 1] - lo[3 * i + 1], hi[3 * i + 2] - lo[3 * i + 2]};
        pdiag[i] = mx_sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int a = 0; a < 3; a++)
            pbox[6 * i + a] = lo[3 * i + a], pbox[6 * i + 3 + a] = hi[3 * i + a];
        pclus[i] = (int)i;
        small[i] = pdiag[i] <= max_diag;
        pprot[i] = small[i] ? 0.0 : -1.0;
    }
    /* the structure's triangles in a grid of cap-sized cells (by box; those over 512 cells on a list of their own) */
    long long ns = 0, *lt = tg;
    tg = MX_NEW(long long, 3 * (nlive ? nlive : 1));
    for (long long t = 0; t < nlive; t++)
        if (!small[ppiece[lt[3 * t]]]) {
            memcpy(tg + 3 * ns, lt + 3 * t, 3 * sizeof(long long));
            ns++;
        }
    double cs = cap > 0.5 ? cap : 0.5;
    long long ng = 0, gcap = 0, nbig = 0;
    pg_ent *G = NULL;
    int *big = MX_NEW(int, ns ? ns : 1);
    for (long long t = 0; t < ns; t++) {
        long long l[3], h[3];
        for (int a = 0; a < 3; a++) {
            double x = P[3 * tg[3 * t] + a], y = P[3 * tg[3 * t + 1] + a], z = P[3 * tg[3 * t + 2] + a];
            double mn = x < y ? (x < z ? x : z) : (y < z ? y : z), mxv = x > y ? (x > z ? x : z) : (y > z ? y : z);
            l[a] = (long long)mx_floor(mn / cs), h[a] = (long long)mx_floor(mxv / cs);
        }
        if ((h[0] - l[0] + 1) * (h[1] - l[1] + 1) * (h[2] - l[2] + 1) > 512) {
            big[nbig++] = (int)t;
            continue;
        }
        for (long long x = l[0]; x <= h[0]; x++)
            for (long long y = l[1]; y <= h[1]; y++)
                for (long long z = l[2]; z <= h[2]; z++) {
                    if (ng == gcap) {
                        gcap = gcap ? gcap * 2 : 4096;
                        G = mx_realloc(G, sizeof(pg_ent) * (size_t)gcap);
                    }
                    G[ng].key = pg_key(x, y, z), G[ng++].t = (int)t;
                }
    }
    if (ng > 1)
        qsort(G, (size_t)ng, sizeof(pg_ent), pg_cmp);
    double *td2 = MX_ZNEW(double, nlive ? nlive : 1);
    prot_ctx c = {P, tg, G, ng, big, nbig, cs, cap * cap, lt, ppiece, small, td2};
    mx_parallel_for(nlive, prot_one, &c);
    for (long long t = 0; t < nlive; t++) {
        int pc = ppiece[lt[3 * t]];
        if (small[pc] && td2[t] > pprot[pc])
            pprot[pc] = td2[t];
    }
    for (long long i = 0; i < np; i++)
        if (small[i])
            pprot[i] = mx_sqrt(pprot[i]);
    /* clusters: the loose, not solo pieces in a grid of 2 m cells by their grown boxes; pieces sharing a cell whose
       grown boxes overlap are joined */
    {
        const double gs = 2.0;
        long long nc = 0, ccap = 0;
        pg_ent *C = NULL;
        for (long long i = 0; i < np; i++) {
            if (!small[i] || psolo[i])
                continue;
            long long l[3], h[3];
            for (int a = 0; a < 3; a++)
                l[a] = (long long)mx_floor((lo[3 * i + a] - gap) / gs), h[a] = (long long)mx_floor((hi[3 * i + a] + gap) / gs);
            for (long long x = l[0]; x <= h[0]; x++)
                for (long long y = l[1]; y <= h[1]; y++)
                    for (long long z = l[2]; z <= h[2]; z++) {
                        if (nc == ccap) {
                            ccap = ccap ? ccap * 2 : 4096;
                            C = mx_realloc(C, sizeof(pg_ent) * (size_t)ccap);
                        }
                        C[nc].key = pg_key(x, y, z), C[nc++].t = (int)i;
                    }
        }
        if (nc > 1)
            qsort(C, (size_t)nc, sizeof(pg_ent), pg_cmp);
        for (long long a = 0; a < nc;) {
            long long b = a;
            while (b < nc && C[b].key == C[a].key)
                b++;
            for (long long i = a; i < b; i++)
                for (long long j = i + 1; j < b; j++) {
                    int x = C[i].t, y = C[j].t, ov = 1;
                    for (int k = 0; k < 3 && ov; k++)
                        ov = lo[3 * x + k] - gap <= hi[3 * y + k] + gap && lo[3 * y + k] - gap <= hi[3 * x + k] + gap;
                    if (ov) {
                        int rx = pc_root(pclus, x), ry = pc_root(pclus, y);
                        if (rx != ry)
                            pclus[rx > ry ? rx : ry] = rx > ry ? ry : rx;
                    }
                }
            a = b;
        }
        for (long long i = 0; i < np; i++)
            pclus[i] = pc_root(pclus, (int)i);
        free(C);
    }
    free(tsolo), free(psolo);
    free(kid), free(par), free(used), free(tg), free(pid), free(lo), free(hi), free(small), free(G), free(big), free(td2), free(lt);
    return np;
}
