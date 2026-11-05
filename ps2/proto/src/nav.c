/* Enemy pathfinding (nav.h). Data: tools/export_enemies.py "NAV1" section after the enemies.bin placements. */
#include <malloc.h>
#include <math.h>
#include <string.h>
#include "nav.h"
#include "enemy.h"  /* ENEMY_REC */

typedef struct {
    char magic[4];
    unsigned verts, tris, gw, gh;
    float org[3], x0, z0, cell;
    unsigned list;
} NavHdr;
_Static_assert(sizeof(NavHdr) == 48, "export_enemies.py NAV1 header");
typedef struct { unsigned short v[3], nbr[3]; } NavTri;
#define NONE 0xFFFF
#define NAV_Q 16.0f  /* export_enemies.py NAV_Q: 1/16 m vertex grid */

typedef struct {
    void *mem;
    unsigned bytes;
    NavHdr h;
    const short (*v)[3];
    const NavTri *t;
    const unsigned short *start, *list;
} Nav;
static Nav navs[NAV_BLOCKS];
int nav_searches, nav_searches_total;

int nav_load(int block, const void *blob, unsigned size)
{
    if (!size)
        size = ~0u;  /* unknown: the section is trusted (enemies.bin from the same build) */
    if (block < 0 || block >= NAV_BLOCKS || !blob || size < 8)
        return 0;
    nav_free(block);
    const unsigned char *b = blob;
    unsigned count = *(const unsigned *)(b + 4), at = 8 + ENEMY_REC * count;
    if (at + sizeof(NavHdr) > size || memcmp(b + at, "NAV1", 4))
        return 0;
    NavHdr h;
    memcpy(&h, b + at, sizeof h);
    unsigned vbytes = (h.verts * 6 + 3) & ~3u, tbytes = h.tris * sizeof(NavTri), gbytes = (h.gw * h.gh + 1) * 2,
             n = vbytes + tbytes + gbytes + h.list * 2;
    if (!h.tris || at + sizeof h + n > size)
        return 0;
    Nav *nv = &navs[block];
    nv->mem = malloc(n);
    if (!nv->mem)
        return 0;
    memcpy(nv->mem, b + at + sizeof h, n);
    nv->bytes = n;
    nv->h = h;
    const unsigned char *m = nv->mem;
    nv->v = (const short (*)[3])m;
    nv->t = (const NavTri *)(m + vbytes);
    nv->start = (const unsigned short *)(m + vbytes + tbytes);
    nv->list = nv->start + h.gw * h.gh + 1;
    return h.tris;
}

void nav_free(int block)
{
    if (block < 0 || block >= NAV_BLOCKS)
        return;
    free(navs[block].mem);
    memset(&navs[block], 0, sizeof navs[block]);
}

unsigned nav_bytes(void)
{
    unsigned n = 0;
    for (int i = 0; i < NAV_BLOCKS; i++)
        n += navs[i].bytes;
    return n;
}

static const Nav *nav_of(int block)
{
    return block >= 0 && block < NAV_BLOCKS && navs[block].mem ? &navs[block] : NULL;
}

static void vert(const Nav *nv, int i, float o[3])
{
    for (int c = 0; c < 3; c++)
        o[c] = nv->h.org[c] + nv->v[i][c] * (1.0f / NAV_Q);
}

static void centroid(const Nav *nv, int t, float o[3])
{
    float a[3], b[3], c[3];
    vert(nv, nv->t[t].v[0], a), vert(nv, nv->t[t].v[1], b), vert(nv, nv->t[t].v[2], c);
    for (int k = 0; k < 3; k++)
        o[k] = (a[k] + b[k] + c[k]) * (1.0f / 3);
}

/* Height of triangle t at (x, z) when (x, z) is inside it (1 mm margin) */
static int tri_height(const Nav *nv, int t, float x, float z, float *y)
{
    float a[3], b[3], c[3];
    vert(nv, nv->t[t].v[0], a), vert(nv, nv->t[t].v[1], b), vert(nv, nv->t[t].v[2], c);
    float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
    if (d > -1e-6f && d < 1e-6f)
        return 0;
    float l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
    float l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
    float l3 = 1 - l1 - l2;
    if (l1 < -1e-3f || l2 < -1e-3f || l3 < -1e-3f)
        return 0;
    *y = l1 * a[1] + l2 * b[1] + l3 * c[1];
    return 1;
}

/* Grid cell index of (x, z), -1 outside */
static int cell_of(const Nav *nv, float x, float z)
{
    int gx = (int)floorf((x - nv->h.x0) / nv->h.cell), gz = (int)floorf((z - nv->h.z0) / nv->h.cell);
    if (gx < 0 || gz < 0 || gx >= (int)nv->h.gw || gz >= (int)nv->h.gh)
        return -1;
    return gz * nv->h.gw + gx;
}

static int tri_at(const Nav *nv, const float p[3], float below, float above)
{
    int c = cell_of(nv, p[0], p[2]), best = -1;
    if (c < 0)
        return -1;
    float bd = 1e9f;
    for (unsigned k = nv->start[c]; k < nv->start[c + 1]; k++) {
        int t = nv->list[k];
        float y;
        if (!tri_height(nv, t, p[0], p[2], &y) || y < p[1] - below || y > p[1] + above)
            continue;
        float d = fabsf(y - p[1]);
        if (d < bd)
            bd = d, best = t;
    }
    return best;
}

int nav_tri_at(int block, const float p[3])
{
    const Nav *nv = nav_of(block);
    return nv ? tri_at(nv, p, 1.0f, 2.0f) : -1;
}

/* A goal off the mesh (the player on a ledge, mid-jump, on an object): the triangle whose centroid is nearest in its
 * grid cell and the 8 around it, within 3 m of height */
static int tri_near(const Nav *nv, const float p[3])
{
    int t = tri_at(nv, p, 3.0f, 3.0f);
    if (t >= 0)
        return t;
    float bd = 1e9f;
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            int c = cell_of(nv, p[0] + dx * nv->h.cell, p[2] + dz * nv->h.cell);
            if (c < 0)
                continue;
            for (unsigned k = nv->start[c]; k < nv->start[c + 1]; k++) {
                float m[3];
                centroid(nv, nv->list[k], m);
                if (fabsf(m[1] - p[1]) > 3.0f)
                    continue;
                float d = (m[0] - p[0]) * (m[0] - p[0]) + (m[2] - p[2]) * (m[2] - p[2]);
                if (d < bd)
                    bd = d, t = nv->list[k];
            }
        }
    return t;
}

/* ---- A* over triangles (node position: the centroid), bounded ---- */

#define SEARCH_MAX 384   /* nodes per search (~20 % of a block's triangles); beyond, the closest one found is used */
#define HASH 2048        /* tri -> node slot, open addressing (power of two, > 2 * SEARCH_MAX) */
typedef struct { unsigned short tri; short parent; float g, f; unsigned char closed; } Node;
static Node node[SEARCH_MAX];
static short heap[SEARCH_MAX];
static unsigned short hkey[HASH];
static short hval[HASH];
static int nnodes, nheap;

static int slot_of(int tri, int add)
{
    unsigned h = (tri * 2654435761u) >> 21 & (HASH - 1);
    while (hkey[h] != NONE) {
        if (hkey[h] == tri)
            return hval[h];
        h = (h + 1) & (HASH - 1);
    }
    if (!add || nnodes >= SEARCH_MAX)
        return -1;
    hkey[h] = tri, hval[h] = nnodes;
    node[nnodes].tri = tri, node[nnodes].closed = 0;
    return nnodes++;
}

static void heap_up(int i)
{
    while (i > 0) {
        int p = (i - 1) >> 1;
        if (node[heap[p]].f <= node[heap[i]].f)
            break;
        short s = heap[p];
        heap[p] = heap[i], heap[i] = s, i = p;
    }
}

static int heap_pop(void)
{
    int top = heap[0];
    heap[0] = heap[--nheap];
    for (int i = 0;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < nheap && node[heap[l]].f < node[heap[m]].f)
            m = l;
        if (r < nheap && node[heap[r]].f < node[heap[m]].f)
            m = r;
        if (m == i)
            break;
        short s = heap[m];
        heap[m] = heap[i], heap[i] = s, i = m;
    }
    return top;
}

/* Node slot of the goal (or of the closest node to it when the search ran out / the goal is not connected) */
static int astar(const Nav *nv, int s, int g, const float to[3])
{
    memset(hkey, 0xFF, sizeof hkey);
    nnodes = nheap = 0;
    int si = slot_of(s, 1), best = si;
    float c[3], bh;
    centroid(nv, s, c);
    node[si].parent = -1, node[si].g = 0, node[si].f = bh = dist3(c, to);
    heap[nheap++] = si;
    while (nheap) {
        int i = heap_pop();
        if (node[i].closed)
            continue;
        node[i].closed = 1;
        int t = node[i].tri;
        if (t == g)
            return i;
        float ct[3];
        centroid(nv, t, ct);
        float h = node[i].f - node[i].g;
        if (h < bh)
            bh = h, best = i;
        for (int e = 0; e < 3; e++) {
            int n = nv->t[t].nbr[e];
            if (n == NONE)
                continue;
            int j = slot_of(n, 0);
            float cn[3];
            centroid(nv, n, cn);
            float gn = node[i].g + dist3(ct, cn);
            if (j >= 0 && (node[j].closed || node[j].g <= gn))
                continue;
            if (j < 0 && (j = slot_of(n, 1)) < 0)
                return best;  /* out of nodes */
            node[j].parent = i, node[j].g = gn, node[j].f = gn + dist3(cn, to);
            if (nheap < SEARCH_MAX)
                heap[nheap++] = j, heap_up(nheap - 1);
        }
    }
    return best;
}

/* ---- funnel (string pulling) through the portals of the triangle path: the first corner ---- */

/* twice the signed area of (a, b, c) in x/z, the funnel's orientation test */
static float area2(const float *a, const float *b, const float *c)
{
    float ax = b[0] - a[0], az = b[2] - a[2], bx = c[0] - a[0], bz = c[2] - a[2];
    return bx * az - ax * bz;
}

static int same_xz(const float *a, const float *b)
{
    return fabsf(a[0] - b[0]) < 1e-3f && fabsf(a[2] - b[2]) < 1e-3f;
}

#define PATH_MAX_TRIS 64  /* the funnel reads this far along the path; the corner is re-found as the character walks */
#define CORNER_MIN 0.3f   /* m: corners closer than this to the walker are passed (it stands on them) */
#define PORTAL_SHRINK 0.3f  /* m: portal ends moved inwards, so routes keep off walls and corners (ASSUMED) */

static void first_corner(const Nav *nv, const unsigned short *path, int n, const float from[3], const float to[3],
                         float out[3])
{
    float L[PATH_MAX_TRIS + 2][3], R[PATH_MAX_TRIS + 2][3];
    int np = 0;
    memcpy(L[np], from, 12), memcpy(R[np], from, 12), np++;
    for (int k = 0; k + 1 < n && np < PATH_MAX_TRIS; k++) {
        const NavTri *t = &nv->t[path[k]];
        int e = 0;
        while (e < 3 && t->nbr[e] != path[k + 1])
            e++;
        if (e == 3)
            break;
        float a[3], b[3], c[3];
        vert(nv, t->v[e], a), vert(nv, t->v[(e + 1) % 3], b);
        centroid(nv, path[k], c);
        float *l = L[np], *r = R[np];
        if (area2(c, a, b) > 0)  /* left / right as seen walking out of path[k] (area2(apex, left, right) > 0) */
            memcpy(l, a, 12), memcpy(r, b, 12);
        else
            memcpy(l, b, 12), memcpy(r, a, 12);
        float d[3] = {r[0] - l[0], r[1] - l[1], r[2] - l[2]}, len = sqrtf(d[0] * d[0] + d[2] * d[2]);
        float k2 = len > 2 * PORTAL_SHRINK ? PORTAL_SHRINK / len : 0.5f;
        for (int i = 0; i < 3; i++)
            l[i] += d[i] * k2, r[i] -= d[i] * k2;
        np++;
    }
    int reached = np - 1 == n - 1;
    if (reached)
        memcpy(L[np], to, 12), memcpy(R[np], to, 12), np++;
    /* simple stupid funnel (Mononen): corners in order; the first one far enough from the walker is the answer */
    float apex[3], pl[3], pr[3];
    int ai = 0, li = 0, ri = 0;
    memcpy(apex, from, 12), memcpy(pl, L[0], 12), memcpy(pr, R[0], 12);
    for (int i = 1; i < np; i++) {
        int corner = 0;
        if (area2(apex, pr, R[i]) <= 0) {  /* right side moves in */
            if (same_xz(apex, pr) || area2(apex, pl, R[i]) > 0)
                memcpy(pr, R[i], 12), ri = i;
            else  /* crosses the left side: the left point is a corner */
                memcpy(apex, pl, 12), ai = li, corner = 1;
        }
        if (!corner && area2(apex, pl, L[i]) >= 0) {  /* left side moves in */
            if (same_xz(apex, pl) || area2(apex, pr, L[i]) < 0)
                memcpy(pl, L[i], 12), li = i;
            else
                memcpy(apex, pr, 12), ai = ri, corner = 1;
        }
        if (corner) {
            float dx = apex[0] - from[0], dz = apex[2] - from[2];
            if (dx * dx + dz * dz > CORNER_MIN * CORNER_MIN) {
                memcpy(out, apex, 12);
                return;
            }
            memcpy(pl, apex, 12), memcpy(pr, apex, 12), li = ri = ai, i = ai;  /* restart from the corner */
        }
    }
    if (reached)
        memcpy(out, to, 12);
    else  /* the path goes on past what was read: head for the middle of the last portal read */
        for (int c = 0; c < 3; c++)
            out[c] = (L[np - 1][c] + R[np - 1][c]) * 0.5f;
}

int nav_next(int block, const float from[3], const float to[3], float out[3])
{
    const Nav *nv = nav_of(block);
    if (!nv)
        return 0;
    int s = tri_at(nv, from, 1.0f, 2.0f);
    if (s < 0)
        return 0;
    int g = tri_near(nv, to);
    if (g == s || g < 0) {
        memcpy(out, to, 12);
        return 2;
    }
    nav_searches++, nav_searches_total++;
    int i = astar(nv, s, g, to);
    float goal[3];
    if (node[i].tri == g)
        memcpy(goal, to, 12);
    else
        centroid(nv, node[i].tri, goal);  /* unreachable: as close as the mesh allows */
    static unsigned short path[SEARCH_MAX];
    int n = 0;
    for (int j = i; j >= 0 && n < SEARCH_MAX; j = node[j].parent)
        path[n++] = node[j].tri;
    for (int a = 0, b = n - 1; a < b; a++, b--) {  /* start first */
        unsigned short x = path[a];
        path[a] = path[b], path[b] = x;
    }
    first_corner(nv, path, n, from, goal, out);
    return node[i].tri == g ? 2 : 1;
}
