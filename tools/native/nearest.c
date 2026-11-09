/* tools/horizon.py view_d in C (2026-11-07, build speed): per query point the smallest squared distance to the samples,
 * (dx * dx + dy * dy) + dz * dz in double as numpy's ((v - s) ** 2).sum(-1) adds it, by brute force (no pruning: the
 * minimum is the same double numpy finds). A query stops at the first sample nearer than sqrt(stop): view_d clamps every
 * distance up to HZN_D, so any value under stop = HZN_D^2 gives the same result. Built with -ffp-contract=off. */
void min_d2(int nq, const double *Q, int ns, const double *S, double stop, double *out)
{
    /* each query writes only out[i]: the same values with any thread count (-fopenmp, or serial) */
#pragma omp parallel for schedule(dynamic, 256)
    for (int i = 0; i < nq; i++) {
        const double qx = Q[3 * i], qy = Q[3 * i + 1], qz = Q[3 * i + 2];
        double best = 1e300;
        for (int j = 0; j < ns; j++) {
            double dx = qx - S[3 * j], dy = qy - S[3 * j + 1], dz = qz - S[3 * j + 2];
            double d = dx * dx + dy * dy + dz * dz;
            if (d < best) {
                best = d;
                if (best < stop)
                    break;
            }
        }
        out[i] = best;
    }
}
