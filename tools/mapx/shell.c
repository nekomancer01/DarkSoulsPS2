/* Plank shells (tools/plankshell.py, TECH_DEBT world-49): the unsigned distance from every point of a regular grid
 * (spacing v, origin o, nx x ny x nz points) within `band` of a triangle soup, and the nearest triangle's index. The
 * shell is the field's iso surface at a small offset (surface nets in plankshell.py): loose pieces that lie closer than
 * twice the offset fuse into one closed surface the simplifier can reduce, which separate boxes never are. */
#include <math.h>
#include "mapx.h"

static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* squared distance from p to triangle abc (Ericson, Real-Time Collision Detection 5.1.5) */
static double pt_tri_d2(const double *p, const double *a, const double *b, const double *c)
{
    double ab[3], ac[3], ap[3], q[3];
    for (int i = 0; i < 3; i++)
        ab[i] = b[i] - a[i], ac[i] = c[i] - a[i], ap[i] = p[i] - a[i];
    double d1 = dot3(ab, ap), d2 = dot3(ac, ap);
    if (d1 <= 0 && d2 <= 0) {
        for (int i = 0; i < 3; i++) q[i] = a[i];
        goto out;
    }
    double bp[3], cp[3];
    for (int i = 0; i < 3; i++)
        bp[i] = p[i] - b[i], cp[i] = p[i] - c[i];
    double d3 = dot3(ab, bp), d4 = dot3(ac, bp);
    if (d3 >= 0 && d4 <= d3) {
        for (int i = 0; i < 3; i++) q[i] = b[i];
        goto out;
    }
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        double t = d1 / (d1 - d3);
        for (int i = 0; i < 3; i++) q[i] = a[i] + t * ab[i];
        goto out;
    }
    double d5 = dot3(ab, cp), d6 = dot3(ac, cp);
    if (d6 >= 0 && d5 <= d6) {
        for (int i = 0; i < 3; i++) q[i] = c[i];
        goto out;
    }
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        double t = d2 / (d2 - d6);
        for (int i = 0; i < 3; i++) q[i] = a[i] + t * ac[i];
        goto out;
    }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        double t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int i = 0; i < 3; i++) q[i] = b[i] + t * (c[i] - b[i]);
        goto out;
    }
    {
        double den = 1.0 / (va + vb + vc), v = vb * den, w = vc * den;
        for (int i = 0; i < 3; i++) q[i] = a[i] + ab[i] * v + ac[i] * w;
    }
out:;
    double d = 0;
    for (int i = 0; i < 3; i++)
        d += (p[i] - q[i]) * (p[i] - q[i]);
    return d;
}

MX_API long long mx_shell_field(long long n, const double *tri, double x0, double y0, double z0, double v, int nx, int ny,
                                int nz, double band, float *dist, int *near)
{
    long long touched = 0;
    double b2 = band * band, o[3] = {x0, y0, z0};
    int dims[3] = {nx, ny, nz};
    for (long long t = 0; t < n; t++) {
        const double *a = tri + 9 * t, *b = a + 3, *c = a + 6;
        int lo[3], hi[3];
        for (int i = 0; i < 3; i++) {
            double mn = fmin(a[i], fmin(b[i], c[i])) - band, mx = fmax(a[i], fmax(b[i], c[i])) + band;
            lo[i] = (int)ceil((mn - o[i]) / v), hi[i] = (int)floor((mx - o[i]) / v);
            if (lo[i] < 0) lo[i] = 0;
            if (hi[i] > dims[i] - 1) hi[i] = dims[i] - 1;
        }
        for (int z = lo[2]; z <= hi[2]; z++)
            for (int y = lo[1]; y <= hi[1]; y++)
                for (int x = lo[0]; x <= hi[0]; x++) {
                    double p[3] = {o[0] + x * v, o[1] + y * v, o[2] + z * v};
                    double d2 = pt_tri_d2(p, a, b, c);
                    if (d2 >= b2)
                        continue;
                    long long k = ((long long)z * ny + y) * nx + x;
                    float d = (float)sqrt(d2);
                    if (d < dist[k]) {
                        dist[k] = d, near[k] = (int)t;
                        touched++;
                    }
                }
    }
    return touched;
}
