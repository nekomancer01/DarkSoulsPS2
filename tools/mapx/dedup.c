/* gather's per-mesh helpers: dedup_tris (repeated triangles of a face set) and _weld's keys, ids and normal sums */
#include "mapx.h"

/* dedup_tris: per vertex (rint(pos x 1e4), rint(uv x 4096) or 0) as int64, per triangle its 3 corners' keys rotated to
   start at the lexicographically smallest corner (the first of equal ones), the first triangle of each rotated key
   kept. pos: nv x 3, uv: nv x 2 or NULL; T: n x 3 vertex indices; keep: out, room for n. Returns the kept count. */
MX_API long long mx_dedup_tris(long long nv, const double *pos, const double *uv, long long n, const long long *T,
                               long long *keep)
{
    long long *K = MX_NEW(long long, 5 * (nv ? nv : 1));
    for (long long v = 0; v < nv; v++) {
        for (int a = 0; a < 3; a++)
            K[5 * v + a] = (long long)mx_rint(pos[3 * v + a] * 1e4);
        for (int a = 0; a < 2; a++)
            K[5 * v + 3 + a] = uv ? (long long)mx_rint(uv[2 * v + a] * 4096) : 0;
    }
    mx_map m;
    mx_map_init(&m, 15, n);
    long long nk = 0, key[15];
    for (long long i = 0; i < n; i++) {
        const long long *c[3] = {K + 5 * T[3 * i], K + 5 * T[3 * i + 1], K + 5 * T[3 * i + 2]};
        int r = 0;
        for (int j = 1; j < 3; j++) {
            int lt = 0;  /* c[j] < c[r] lexicographically */
            for (int a = 0; a < 5; a++)
                if (c[j][a] != c[r][a]) {
                    lt = c[j][a] < c[r][a];
                    break;
                }
            if (lt)
                r = j;
        }
        for (int j = 0; j < 3; j++)
            memcpy(key + 5 * j, c[(r + j) % 3], 5 * sizeof(long long));
        int added;
        mx_map_put(&m, key, &added);
        if (added)
            keep[nk++] = i;
    }
    mx_map_free(&m);
    free(K);
    return nk;
}

/* _weld's ids: positions by rint(Pw x 1e4), wedges by (position id, rint(uv x 4096), rint(colour x 255)) (uv NULL: 0,
   colour NULL: 255), each numbered by first appearance; with normals N, WN[w] = the normals of wedge w's vertices
   summed in vertex order (the first one as is). pid / wid: n; pfirst / wfirst: room for n; cnt: out (positions,
   wedges). */
MX_API void mx_weld(long long n, const double *Pw, const double *uv, const double *col, const double *N, long long *pid,
                    long long *pfirst, long long *wid, long long *wfirst, double *WN, long long *cnt)
{
    mx_map m;
    mx_map_init(&m, 3, n);
    long long key[7];
    for (long long i = 0; i < n; i++) {
        for (int a = 0; a < 3; a++)
            key[a] = (long long)mx_rint(Pw[3 * i + a] * 1e4);
        int added;
        long long d = mx_map_put(&m, key, &added);
        if (added)
            pfirst[d] = i;
        pid[i] = d;
    }
    cnt[0] = m.n;
    mx_map_free(&m);
    mx_map_init(&m, 7, n);
    for (long long i = 0; i < n; i++) {
        key[0] = pid[i];
        for (int a = 0; a < 2; a++)
            key[1 + a] = uv ? (long long)mx_rint(uv[2 * i + a] * 4096) : 0;
        for (int a = 0; a < 4; a++)
            key[3 + a] = col ? (long long)mx_rint(col[4 * i + a] * 255) : 255;
        int added;
        long long d = mx_map_put(&m, key, &added);
        if (added)
            wfirst[d] = i;
        wid[i] = d;
        if (N) {
            double *w = WN + 3 * d;
            const double *v = N + 3 * i;
            if (added)
                w[0] = v[0], w[1] = v[1], w[2] = v[2];
            else
                w[0] = w[0] + v[0], w[1] = w[1] + v[1], w[2] = w[2] + v[2];
        }
    }
    cnt[1] = m.n;
    mx_map_free(&m);
}
