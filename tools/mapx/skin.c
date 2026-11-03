/* pose_obj.skin_mesh in C: a dynamic mesh's vertices posed by their bone influences, every value as the Python loop
   computes it (havok_anim.qrot's operation order, Python's sum() of the weights, the per-influence accumulation). */
#include "mapx.h"

/* havok_anim.qrot: v rotated by q (x, y, z, w) */
static void qrot(const double *q, const double *v, double *o)
{
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + y * tz - z * ty;
    o[1] = v[1] + w * ty + z * tx - x * tz;
    o[2] = v[2] + w * tz + x * ty - y * tx;
}

/* n vertices: P (n x 3), N (n x 3, NULL: (0, 1, 0) each), BI (n x 4 mesh bone indices, NULL: no influences: the bind
   position), BW (n x 4, NULL: weights (1, 0, 0, 0)); bone table of nb mesh bones, 15 doubles each: present (1 / 0),
   bind t[3], bind q[4], pose t[3], pose q[4]. -> OP, ON (n x 3). */
MX_API void mx_skin(long long n, const double *P, const double *N, const long long *BI, const double *BW, long long nb,
                    const double *tab, double *OP, double *ON)
{
    static const double up[3] = {0.0, 1.0, 0.0}, w1[4] = {1.0, 0.0, 0.0, 0.0};
    for (long long vi = 0; vi < n; vi++) {
        const double *p = P + 3 * vi, *nn = N ? N + 3 * vi : up;
        const double *in[4], *wt = BW ? BW + 4 * vi : w1;
        double ws[4];
        int k = 0;
        if (BI)
            for (int j = 0; j < 4; j++) {
                long long b = BI[4 * vi + j];
                if (wt[j] > 0 && b >= 0 && b < nb && tab[15 * b] != 0.0) {
                    in[k] = tab + 15 * b + 1;
                    ws[k++] = wt[j];
                }
            }
        double *op = OP + 3 * vi, *on = ON + 3 * vi;
        if (!k) {
            for (int a = 0; a < 3; a++)
                op[a] = p[a], on[a] = nn[a];
            continue;
        }
        mx_psum s;
        mx_psum_init(&s, 0.0);
        for (int j = 0; j < k; j++)
            mx_psum_add(&s, ws[j]);
        double tw = mx_psum_get(&s), acc[3] = {0.0, 0.0, 0.0}, nacc[3] = {0.0, 0.0, 0.0};
        for (int j = 0; j < k; j++) {
            const double *bt = in[j], *bq = in[j] + 3, *t = in[j] + 7, *q = in[j] + 10;
            double iq[4] = {-bq[0], -bq[1], -bq[2], bq[3]}, d[3] = {p[0] - bt[0], p[1] - bt[1], p[2] - bt[2]};
            double r0[3], r[3], rn0[3], rn[3];
            qrot(iq, d, r0);
            qrot(q, r0, r);
            qrot(iq, nn, rn0);
            qrot(q, rn0, rn);
            for (int a = 0; a < 3; a++) {
                acc[a] += ws[j] / tw * (t[a] + r[a]);
                nacc[a] += ws[j] / tw * rn[a];
            }
        }
        for (int a = 0; a < 3; a++)
            op[a] = acc[a], on[a] = nacc[a];
    }
}
