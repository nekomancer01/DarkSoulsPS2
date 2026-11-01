/* Unsigned distance from points to a triangle mesh (for simplification error measurements in tools/meshopt).
 * Triangles are binned into a uniform grid over the mesh bounds (about 4 triangles per cell); a query searches cubic
 * shells of cells around its cell until the best distance is below the inner radius of the next shell.
 * Build: gcc -O2 -shared -fPIC -o libmeshdist.so meshdist.c -lm */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, z; } V;
static V sub(V a, V b) { return (V){a.x - b.x, a.y - b.y, a.z - b.z}; }
static float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

/* squared distance point-triangle (Ericson, Real-Time Collision Detection 5.1.5) */
static float pt_tri(V p, V a, V b, V c)
{
    V ab = sub(b, a), ac = sub(c, a), ap = sub(p, a), q;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) { q = a; goto done; }
    V bp = sub(p, b);
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) { q = b; goto done; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { float v = d1 / (d1 - d3); q = (V){a.x + v * ab.x, a.y + v * ab.y, a.z + v * ab.z}; goto done; }
    V cp = sub(p, c);
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) { q = c; goto done; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { float w = d2 / (d2 - d6); q = (V){a.x + w * ac.x, a.y + w * ac.y, a.z + w * ac.z}; goto done; }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        q = (V){b.x + w * (c.x - b.x), b.y + w * (c.y - b.y), b.z + w * (c.z - b.z)};
        goto done;
    }
    {
        float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den;
        q = (V){a.x + ab.x * v + ac.x * w, a.y + ab.y * v + ac.y * w, a.z + ab.z * v + ac.z * w};
    }
done:;
    V d = sub(p, q);
    return dot(d, d);
}

void point_mesh_distance(const float *pts, size_t np, const float *pos, size_t nv, const unsigned *idx, size_t nt,
                         float *out)
{
    (void)nv;
    if (!nt) {
        for (size_t i = 0; i < np; i++)
            out[i] = FLT_MAX;
        return;
    }
    const V *P = (const V *)pos;
    V lo = {FLT_MAX, FLT_MAX, FLT_MAX}, hi = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t t = 0; t < 3 * nt; t++) {
        V v = P[idx[t]];
        lo.x = fminf(lo.x, v.x), lo.y = fminf(lo.y, v.y), lo.z = fminf(lo.z, v.z);
        hi.x = fmaxf(hi.x, v.x), hi.y = fmaxf(hi.y, v.y), hi.z = fmaxf(hi.z, v.z);
    }
    float ex = hi.x - lo.x + 1e-3f, ey = hi.y - lo.y + 1e-3f, ez = hi.z - lo.z + 1e-3f;
    float cell = cbrtf(ex * ey * ez / (nt / 4.0f + 1));
    float m = fmaxf(fmaxf(ex, ey), ez) / 256;  /* at most 256 cells per axis */
    if (cell < m) cell = m;
    int gx = (int)(ex / cell) + 1, gy = (int)(ey / cell) + 1, gz = (int)(ez / cell) + 1;
    size_t nc = (size_t)gx * gy * gz;
    unsigned *cnt = calloc(nc + 1, sizeof *cnt);
    /* a triangle goes into every cell its bounding box touches */
    for (int pass = 0; pass < 2; pass++) {
        static unsigned *list;
        if (pass == 1) {
            for (size_t c = 1; c <= nc; c++) cnt[c] += cnt[c - 1];
            list = malloc((cnt[nc] + 1) * sizeof *list);
            memmove(cnt + 1, cnt, nc * sizeof *cnt);
            cnt[0] = 0;
        }
        for (size_t t = 0; t < nt; t++) {
            V a = P[idx[3 * t]], b = P[idx[3 * t + 1]], c = P[idx[3 * t + 2]];
            int x0 = (int)((fminf(fminf(a.x, b.x), c.x) - lo.x) / cell), x1 = (int)((fmaxf(fmaxf(a.x, b.x), c.x) - lo.x) / cell);
            int y0 = (int)((fminf(fminf(a.y, b.y), c.y) - lo.y) / cell), y1 = (int)((fmaxf(fmaxf(a.y, b.y), c.y) - lo.y) / cell);
            int z0 = (int)((fminf(fminf(a.z, b.z), c.z) - lo.z) / cell), z1 = (int)((fmaxf(fmaxf(a.z, b.z), c.z) - lo.z) / cell);
            for (int x = x0; x <= x1; x++)
                for (int y = y0; y <= y1; y++)
                    for (int z = z0; z <= z1; z++) {
                        size_t ci = ((size_t)x * gy + y) * gz + z;
                        if (pass == 0) cnt[ci + 1]++;
                        else list[cnt[ci + 1]++] = (unsigned)t;
                    }
        }
        if (pass == 1) {
            /* cnt[c] .. cnt[c+1] now spans cell c */
            for (size_t i = 0; i < np; i++) {
                V p = {pts[3 * i], pts[3 * i + 1], pts[3 * i + 2]};
                int cx = (int)floorf((p.x - lo.x) / cell), cy = (int)floorf((p.y - lo.y) / cell), cz = (int)floorf((p.z - lo.z) / cell);
                float best = FLT_MAX;
                int maxr = gx + gy + gz;
                for (int r = 0; r <= maxr; r++) {
                    for (int x = cx - r; x <= cx + r; x++)
                        for (int y = cy - r; y <= cy + r; y++)
                            for (int z = cz - r; z <= cz + r; z++) {
                                if (abs(x - cx) != r && abs(y - cy) != r && abs(z - cz) != r) continue;  /* shell only */
                                if (x < 0 || y < 0 || z < 0 || x >= gx || y >= gy || z >= gz) continue;
                                size_t ci = ((size_t)x * gy + y) * gz + z;
                                for (unsigned k = cnt[ci]; k < cnt[ci + 1]; k++) {
                                    unsigned t = list[k];
                                    float d = pt_tri(p, P[idx[3 * t]], P[idx[3 * t + 1]], P[idx[3 * t + 2]]);
                                    if (d < best) best = d;
                                }
                            }
                    /* everything outside shell r is at least r * cell away (point inside its cell) */
                    float inner = r * cell;
                    if (best <= inner * inner) break;
                }
                out[i] = sqrtf(best);
            }
            free(list);
        }
    }
    free(cnt);
}
