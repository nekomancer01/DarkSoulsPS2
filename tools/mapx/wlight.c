/* gather's per-wedge light (export_map._wedge_light): unit normals, positions, dsr_light.light_fn(...).many
   (hemisphere + GI probe, directional lights) and the static point lights (dsr_light.cubic_light(...).many), every
   element as the numpy code computes it. */
#include "mapx.h"

double pow(double, double);

/* light: [ndirs, (d xyz, col rgb) x 3, up rgb, down rgb, env rgb] (28 doubles); faces: nslot x 6 x R x R x 3;
   slot: per wedge (-1 = no probe); pl: npl x (pos xyz, k, end, rgb) (8 doubles). wn: nw x 3 summed normals;
   pos / wpos: welded positions and wedge -> position. Out: E, S, P, N (nw x 3 each). */
MX_API void mx_wedge_light(long long nw, const double *wn, const double *pos, const long long *wpos, const int *slot,
                           const double *light, const double *faces, int R, int npl, const double *pl, double *E,
                           double *S, double *P, double *N)
{
    /* dsr_light._FACE: (major axis, sign, s axis / sign, t axis / sign) of +X -X +Y -Y +Z -Z */
    static const int FA[6][4] = {{2, -1, 1, -1}, {2, 1, 1, -1}, {0, 1, 2, 1}, {0, 1, 2, -1}, {0, 1, 1, -1}, {0, -1, 1, -1}};
    int nd = (int)light[0];
    const double *up = light + 19, *down = light + 22, *env = light + 25;
    for (long long i = 0; i < nw; i++) {
        double n[3] = {wn[3 * i], wn[3 * i + 1], wn[3 * i + 2]}, *Ni = N + 3 * i, *Pi = P + 3 * i;
        double ln = mx_sqrt(mx_py_sum3(n[0] * n[0], n[1] * n[1], n[2] * n[2]));
        if (ln == 0)
            ln = 1.0;
        for (int a = 0; a < 3; a++)
            Ni[a] = n[a] / ln, Pi[a] = pos[3 * wpos[i] + a];
        double h = Ni[1] * 0.5 + 0.5, e[3] = {0.0, 0.0, 0.0};
        if (slot[i] >= 0) {  /* probe_sample_many */
            int ax = 0;
            for (int a = 1; a < 3; a++)
                if (mx_fabs(Ni[a]) > mx_fabs(Ni[ax]))
                    ax = a;
            double na = Ni[ax], m = mx_fabs(na);
            if (!(m != 0))
                m = 1.0;
            int f = (na > 0 ? 0 : 1) + 2 * ax;
            const int *fa = FA[f];
            double us = ((double)fa[1] * Ni[fa[0]] / m + 1) / 2 * R, vs = ((double)fa[3] * Ni[fa[2]] / m + 1) / 2 * R;
            long long u = (long long)__builtin_trunc(us), v = (long long)__builtin_trunc(vs);
            if (u > R - 1)
                u = R - 1;
            if (v > R - 1)
                v = R - 1;
            const double *c = faces + ((((long long)slot[i] * 6 + f) * R + v) * R + u) * 3;
            e[0] = c[0], e[1] = c[1], e[2] = c[2];
        }
        double *Ei = E + 3 * i, *Si = S + 3 * i;
        for (int c = 0; c < 3; c++) {
            Ei[c] = (down[c] + (up[c] - down[c]) * h) + env[c] * e[c];
            Si[c] = 0.0;
        }
        for (int j = 0; j < nd; j++) {
            const double *d = light + 1 + 3 * j, *col = light + 10 + 3 * j;
            double k = (Ni[0] * d[0] + Ni[1] * d[1]) + Ni[2] * d[2];
            k = k > 0.0 ? k : 0.0;
            for (int c = 0; c < 3; c++)
                Si[c] = Si[c] + col[c] * k;
        }
        for (int j = 0; j < npl; j++) {  /* cubic_light(...).many */
            const double *L = pl + 8 * j;
            double V[3] = {L[0] - Pi[0], L[1] - Pi[1], L[2] - Pi[2]}, o[3] = {0.0, 0.0, 0.0};
            double dd = mx_sqrt(mx_py_sum3(V[0] * V[0], V[1] * V[1], V[2] * V[2]));
            if (dd == 0)
                dd = 1e-6;
            if (!(dd >= L[4])) {
                double att = pow(L[3] * (L[4] - dd), 3.0);
                att = att < 1.0 ? att : 1.0;
                double nl = mx_py_sum3(Ni[0] * V[0], Ni[1] * V[1], Ni[2] * V[2]) / dd;
                nl = nl > 0.0 ? nl : 0.0;
                for (int c = 0; c < 3; c++)
                    o[c] = L[5 + c] * att * nl;
            }
            for (int c = 0; c < 3; c++)
                Ei[c] = Ei[c] + o[c];
        }
    }
}
