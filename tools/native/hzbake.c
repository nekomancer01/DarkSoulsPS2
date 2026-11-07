/* Texture bake for tools/horizon.py (HZN_TEX): the colour of the source map at points of the simplified horizon mesh.
 *
 *   hz_bake(n, tri, uv, tid, light, ntex, pix, toff, tw, th, m, pt, nrm, maxd, out, found)
 *     tri: n x 9 floats (source corners), uv: n x 6 (texture coordinates, 1.0 = the texture's width / height, wrapped),
 *     tid: n ints (texture, -1: untextured), light: n x 9 (corner vertex colours, 128 = 1.0); textures: RGBA bytes at
 *     pix + toff[t], tw[t] x th[t]; m query points pt (m x 3) with normals nrm (m x 3); out: m x 3 floats (0..255),
 *     found: m bytes (1: a source surface within maxd)
 *
 * Each query takes the source surface hit by the line through the point along its normal (nearest |t| <= maxd), else
 * the nearest source point within maxd. Colour = texel (point sampled) x interpolated light / 128, as the GS's
 * modulate draws the source. A transparent texel counts as not found (the caller keeps the mesh colour there).
 * Candidates come from a hashed grid of CELL metres (triangles listed in every cell their box touches). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CELL 4.0f
#define HBITS 20
#define HSIZE (1u << HBITS)

static unsigned hkey(int x, int y, int z)
{
    return ((unsigned)x * 73856093u ^ (unsigned)y * 19349663u ^ (unsigned)z * 83492791u) & (HSIZE - 1);
}

static void sub(float *r, const float *a, const float *b) { r[0] = a[0] - b[0], r[1] = a[1] - b[1], r[2] = a[2] - b[2]; }
static float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross(float *r, const float *a, const float *b)
{
    r[0] = a[1] * b[2] - a[2] * b[1], r[1] = a[2] * b[0] - a[0] * b[2], r[2] = a[0] * b[1] - a[1] * b[0];
}

/* closest point of triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5) -> barycentrics w */
static void closest(const float *p, const float *a, const float *b, const float *c, float *w)
{
    float ab[3], ac[3], ap[3], bp[3], cp[3];
    sub(ab, b, a), sub(ac, c, a), sub(ap, p, a);
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) { w[0] = 1, w[1] = 0, w[2] = 0; return; }
    sub(bp, p, b);
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) { w[0] = 0, w[1] = 1, w[2] = 0; return; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { float v = d1 / (d1 - d3); w[0] = 1 - v, w[1] = v, w[2] = 0; return; }
    sub(cp, p, c);
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) { w[0] = 0, w[1] = 0, w[2] = 1; return; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { float u = d2 / (d2 - d6); w[0] = 1 - u, w[1] = 0, w[2] = u; return; }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        float u = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        w[0] = 0, w[1] = 1 - u, w[2] = u;
        return;
    }
    float den = 1.0f / (va + vb + vc), v = vb * den, u = vc * den;
    w[0] = 1 - v - u, w[1] = v, w[2] = u;
}

void hz_bake(int n, const float *tri, const float *uv, const int *tid, const float *light, int ntex,
             const unsigned char *pix, const long *toff, const int *tw, const int *th, int m, const float *pt,
             const float *nrm, float maxd, float *out, unsigned char *found)
{
    unsigned *cnt = calloc(HSIZE + 1, sizeof(unsigned));
    for (int pass = 0; pass < 2; pass++) {  /* count, then fill (CSR over hash buckets) */
        static unsigned *items;
        unsigned *fill = NULL;
        if (pass) {
            for (unsigned i = 0, s = 0; i <= HSIZE; i++) {
                unsigned c = cnt[i];
                cnt[i] = s, s += c;
            }
            items = malloc(sizeof(unsigned) * (cnt[HSIZE] + 1));
            fill = malloc(sizeof(unsigned) * HSIZE);
            memcpy(fill, cnt, sizeof(unsigned) * HSIZE);
        }
        for (int i = 0; i < n; i++) {
            const float *t = tri + 9 * i;
            int lo[3], hi[3];
            for (int k = 0; k < 3; k++) {
                float a = fminf(t[k], fminf(t[3 + k], t[6 + k])), b = fmaxf(t[k], fmaxf(t[3 + k], t[6 + k]));
                lo[k] = (int)floorf(a / CELL), hi[k] = (int)floorf(b / CELL);
            }
            for (int x = lo[0]; x <= hi[0]; x++)
                for (int y = lo[1]; y <= hi[1]; y++)
                    for (int z = lo[2]; z <= hi[2]; z++) {
                        unsigned h = hkey(x, y, z);
                        if (pass)
                            items[fill[h]++] = (unsigned)i;
                        else
                            cnt[h]++;
                    }
        }
        if (pass) {
            free(fill);
            /* queries */
            unsigned *seen = calloc((size_t)n, sizeof(unsigned));
            for (int q = 0; q < m; q++) {
                const float *p = pt + 3 * q, *d = nrm + 3 * q;
                float best_t = maxd + 1, best_d2 = maxd * maxd + 1, wt[3] = {0}, wd[3] = {0};
                int it = -1, id = -1;
                int lo[3], hi[3];
                for (int k = 0; k < 3; k++)
                    lo[k] = (int)floorf((p[k] - maxd) / CELL), hi[k] = (int)floorf((p[k] + maxd) / CELL);
                for (int x = lo[0]; x <= hi[0]; x++)
                    for (int y = lo[1]; y <= hi[1]; y++)
                        for (int z = lo[2]; z <= hi[2]; z++) {
                            unsigned h = hkey(x, y, z);
                            for (unsigned j = cnt[h]; j < cnt[h + 1]; j++) {
                                unsigned i = items[j];
                                if (seen[i] == (unsigned)q + 1)
                                    continue;
                                seen[i] = (unsigned)q + 1;
                                const float *a = tri + 9 * i, *b = a + 3, *c = a + 6;
                                float e1[3], e2[3], s[3], h3[3], qv[3];
                                sub(e1, b, a), sub(e2, c, a);
                                cross(h3, d, e2);
                                float det = dot(e1, h3);
                                if (fabsf(det) > 1e-9f) {  /* the line along the normal (both ways) */
                                    float f = 1.0f / det;
                                    sub(s, p, a);
                                    float u = f * dot(s, h3);
                                    if (u >= 0 && u <= 1) {
                                        cross(qv, s, e1);
                                        float v = f * dot(d, qv);
                                        if (v >= 0 && u + v <= 1) {
                                            float tt = fabsf(f * dot(e2, qv));
                                            if (tt <= maxd && tt < best_t)
                                                best_t = tt, it = (int)i, wt[0] = 1 - u - v, wt[1] = u, wt[2] = v;
                                        }
                                    }
                                }
                                if (it < 0) {  /* nearest point, while no hit along the normal */
                                    float w[3], cpt[3], dd[3];
                                    closest(p, a, b, c, w);
                                    for (int k = 0; k < 3; k++)
                                        cpt[k] = w[0] * a[k] + w[1] * b[k] + w[2] * c[k];
                                    sub(dd, cpt, p);
                                    float d2 = dot(dd, dd);
                                    if (d2 < best_d2)
                                        best_d2 = d2, id = (int)i, memcpy(wd, w, sizeof w);
                                }
                            }
                        }
                int i = it >= 0 ? it : id;
                const float *w = it >= 0 ? wt : wd;
                found[q] = 0;
                if (i < 0)
                    continue;
                float l[3];
                for (int k = 0; k < 3; k++)
                    l[k] = (w[0] * light[9 * i + k] + w[1] * light[9 * i + 3 + k] + w[2] * light[9 * i + 6 + k]) / 128.0f;
                int t = tid[i];
                float rgb[3] = {128, 128, 128};
                if (t >= 0 && t < ntex) {
                    float u = w[0] * uv[6 * i] + w[1] * uv[6 * i + 2] + w[2] * uv[6 * i + 4];
                    float v = w[0] * uv[6 * i + 1] + w[1] * uv[6 * i + 3] + w[2] * uv[6 * i + 5];
                    int x = (int)floorf(u * tw[t]) % tw[t], y = (int)floorf(v * th[t]) % th[t];
                    x += x < 0 ? tw[t] : 0, y += y < 0 ? th[t] : 0;
                    const unsigned char *px = pix + toff[t] + 4 * ((long)y * tw[t] + x);
                    if (!px[3])
                        continue;  /* transparent texel */
                    rgb[0] = px[0], rgb[1] = px[1], rgb[2] = px[2];
                }
                for (int k = 0; k < 3; k++) {
                    float c = rgb[k] * l[k];
                    out[3 * q + k] = c > 255 ? 255 : c;
                }
                found[q] = 1;
            }
            free(seen);
            free(items);
        }
    }
    free(cnt);
}
