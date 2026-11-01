/* Rigid-body pieces (physics.h). Per frame and body: one col_floor under the box centre gives the floor plane, one
 * col_ray from the last centre to the new one catches walls; then PHYS_SUBSTEPS of gravity, integration, and
 * sequential impulses at the box corners below the plane (restitution only above 1 m/s, Coulomb friction), plus a
 * push-out by the deepest corner. Bodies collide with each other as spheres (rs). Values without a DSR source are
 * marked ASSUMED (TECH_DEBT physics-1). */
#include <math.h>
#include <string.h>
#include "physics.h"
#include "collision.h"

int phys_queries, phys_contacts;

#define SLEEP_V2 0.09f         /* ASSUMED: (0.3 m/s)^2, a touching piece rocking slower than this settles */
#define SLEEP_W2 1.0f          /* ASSUMED: (1 rad/s)^2 */
#define SLEEP_TIME 0.4f
#define SETTLE_DAMP 3.0f       /* ASSUMED: extra damping per second for touching pieces under the sleep speeds */
#define DAMP_LIN 0.02f         /* ASSUMED: per second (hkMotionState linear / angular damping are half floats, unread) */
#define DAMP_ANG 0.3f
#define REST_MIN_V 1.0f        /* below this approach speed contacts do not bounce (no jitter on the floor) */
#define MAX_V 20.0f
#define SLOP 0.01f
#define PAIR_V 1.0f            /* ASSUMED */
#define FLOOR_MOVE 0.3f        /* re-query the floor after this much sideways motion */
#define QUERY_BUDGET 8         /* map queries per frame (~180 us each on the EE); the rest wait a frame */
#define FLOOR_RETRY 8
#define WALL_EVERY 3           /* frames between a body's wall rays */
#define WALL_MOVE 0.015f       /* no ray under 0.3 m/s sideways (over WALL_EVERY frames at 60 fps) */
#define ROLL_DAMP 4.0f         /* ASSUMED: per second while touching the floor */

/* sqrt.s on the EE (libm's sqrtf goes through errno handling) */
static inline float psqrt(float x)
{
#ifdef _EE
    float r;
    __asm__("sqrt.s %0, %1" : "=f"(r) : "f"(x));
    return r;
#else
    return sqrtf(x);
#endif
}

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float *a, const float *b, float *o)
{
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}

/* Rotation rows from a unit quaternion (x y z w) */
static void q_mat(const float *q, float m[9])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y - z * w), m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w), m[4] = 1 - 2 * (x * x + z * z), m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w), m[7] = 2 * (y * z + x * w), m[8] = 1 - 2 * (x * x + y * y);
}

static void m_mul(const float m[9], const float *v, float *o)
{
    float x = m[0] * v[0] + m[1] * v[1] + m[2] * v[2], y = m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
          z = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
    o[0] = x, o[1] = y, o[2] = z;
}

static void m_tmul(const float m[9], const float *v, float *o)  /* transpose(m) * v */
{
    float x = m[0] * v[0] + m[3] * v[1] + m[6] * v[2], y = m[1] * v[0] + m[4] * v[1] + m[7] * v[2],
          z = m[2] * v[0] + m[5] * v[1] + m[8] * v[2];
    o[0] = x, o[1] = y, o[2] = z;
}

void phys_point(const Body *b, const float l[3], float out[3])
{
    float m[9];
    q_mat(b->q, m);
    m_mul(m, l, out);
    out[0] += b->p[0], out[1] += b->p[1], out[2] += b->p[2];
}

/* World inverse inertia times x: R diag(inv_i) R^T x */
static void inv_i_world(const Body *b, const float m[9], const float *x, float *o)
{
    float l[3];
    m_tmul(m, x, l);
    l[0] *= b->inv_i[0], l[1] *= b->inv_i[1], l[2] *= b->inv_i[2];
    m_mul(m, l, o);
}

/* Impulse j along n at offset r from the body origin */
static void apply(Body *b, const float m[9], const float *r, const float *imp)
{
    float t[3], dw[3];
    for (int k = 0; k < 3; k++)
        b->v[k] += imp[k] * b->inv_mass;
    cross3(r, imp, t);
    inv_i_world(b, m, t, dw);
    for (int k = 0; k < 3; k++)
        b->w[k] += dw[k];
}

/* Effective inverse mass along unit d at r */
static float k_along(const Body *b, const float m[9], const float *r, const float *d)
{
    float t[3], u[3], v[3];
    cross3(r, d, t);
    inv_i_world(b, m, t, u);
    cross3(u, r, v);
    return b->inv_mass + dot3(d, v);
}

/* Corner contacts against the plane (point o, normal n): ITERS passes of sequential impulses with accumulated, clamped
 * impulses per corner (a plate resting on an edge gets both corners solved together instead of being kicked by one,
 * then the other): normal, and friction along two fixed tangents (box-clamped to fr * (normal impulse + the corner's
 * weight share this step)); effective masses are computed once per corner. Then a push-out by the deepest corner.
 * Returns contacts. */
#define ITERS 4
static int plane_contacts(Body *b, const float o[3], const float n[3], float dt)
{
    float m[9], cw[3], t1[3], t2[3];
    struct { float r[3], target, jn, j1, j2, kn, k1, k2; } ct[8];
    q_mat(b->q, m);
    m_mul(m, b->c, cw);
    /* tangents: any unit vector perpendicular to n, and n x t1 */
    if (n[1] > 0.9f || n[1] < -0.9f)
        t1[0] = 1, t1[1] = 0, t1[2] = 0;
    else
        t1[0] = 0, t1[1] = 1, t1[2] = 0;
    float k = dot3(t1, n);
    for (int c = 0; c < 3; c++)
        t1[c] -= n[c] * k;
    k = 1 / psqrt(dot3(t1, t1));
    t1[0] *= k, t1[1] *= k, t1[2] *= k;
    cross3(n, t1, t2);
    int nc = 0;
    float deepest = 0;
    for (int i = 0; i < 8; i++) {
        float l[3] = {(i & 1 ? 1 : -1) * b->half[0], (i & 2 ? 1 : -1) * b->half[1], (i & 4 ? 1 : -1) * b->half[2]};
        float *ri = ct[nc].r;
        m_mul(m, l, ri);
        ri[0] += cw[0], ri[1] += cw[1], ri[2] += cw[2];
        float d = (b->p[0] + ri[0] - o[0]) * n[0] + (b->p[1] + ri[1] - o[1]) * n[1] + (b->p[2] + ri[2] - o[2]) * n[2];
        if (d >= SLOP)  /* corners within SLOP count as touching: a resting box keeps its contacts every step */
            continue;
        if (d < deepest)
            deepest = d;
        float wr[3], vel[3];
        cross3(b->w, ri, wr);
        for (int c = 0; c < 3; c++)
            vel[c] = b->v[c] + wr[c];
        float vn = dot3(vel, n);
        ct[nc].target = vn < -REST_MIN_V ? -b->rest * vn : 0;  /* bounce only off fast hits */
        ct[nc].kn = 1 / k_along(b, m, ri, n);
        ct[nc].k1 = 1 / k_along(b, m, ri, t1);
        ct[nc].k2 = 1 / k_along(b, m, ri, t2);
        ct[nc].jn = ct[nc].j1 = ct[nc].j2 = 0;
        nc++;
    }
    for (int it = 0; it < ITERS; it++)
        for (int c = 0; c < nc; c++) {
            float wr[3], vel[3], *r = ct[c].r;
            cross3(b->w, r, wr);
            for (int q = 0; q < 3; q++)
                vel[q] = b->v[q] + wr[q];
            float old = ct[c].jn, jn = old + (ct[c].target - dot3(vel, n)) * ct[c].kn;
            jn = jn < 0 ? 0 : jn;
            ct[c].jn = jn;
            float lim = b->fr * (jn + PHYS_GRAVITY * dt * ct[c].kn / nc);
            float o1 = ct[c].j1, j1 = o1 - dot3(vel, t1) * ct[c].k1;
            float o2 = ct[c].j2, j2 = o2 - dot3(vel, t2) * ct[c].k2;
            j1 = j1 > lim ? lim : j1 < -lim ? -lim : j1;
            j2 = j2 > lim ? lim : j2 < -lim ? -lim : j2;
            ct[c].j1 = j1, ct[c].j2 = j2;
            float dn = jn - old, d1 = j1 - o1, d2 = j2 - o2;
            float imp[3] = {n[0] * dn + t1[0] * d1 + t2[0] * d2, n[1] * dn + t1[1] * d1 + t2[1] * d2,
                            n[2] * dn + t1[2] * d1 + t2[2] * d2};
            apply(b, m, r, imp);
        }
    if (deepest < 0)
        for (int c = 0; c < 3; c++)
            b->p[c] -= n[c] * deepest * 0.8f;
    return nc;
}

static void integrate(Body *b, float dt)
{
    b->v[1] -= PHYS_GRAVITY * dt;
    float dl = 1 - DAMP_LIN * dt, da = 1 - DAMP_ANG * dt;
    for (int k = 0; k < 3; k++) {
        b->v[k] *= dl, b->w[k] *= da;
        if (b->v[k] > MAX_V) b->v[k] = MAX_V;
        if (b->v[k] < -MAX_V) b->v[k] = -MAX_V;
        b->p[k] += b->v[k] * dt;
    }
    /* q += 0.5 * (w, 0) * q * dt */
    float *q = b->q, w0 = b->w[0], w1 = b->w[1], w2 = b->w[2], h = 0.5f * dt;
    float x = q[0] + h * (w0 * q[3] + w1 * q[2] - w2 * q[1]);
    float y = q[1] + h * (w1 * q[3] + w2 * q[0] - w0 * q[2]);
    float z = q[2] + h * (w2 * q[3] + w0 * q[1] - w1 * q[0]);
    float w = q[3] - h * (w0 * q[0] + w1 * q[1] + w2 * q[2]);
    float l = 1 / psqrt(x * x + y * y + z * z + w * w);
    q[0] = x * l, q[1] = y * l, q[2] = z * l, q[3] = w * l;
}

/* Two bodies as spheres around their box centres: separate along the centre line, no bounce */
static void pair(Body *a, Body *b)
{
    float ca[3], cb[3];
    phys_point(a, a->c, ca);
    phys_point(b, b->c, cb);
    float d[3] = {cb[0] - ca[0], cb[1] - ca[1], cb[2] - ca[2]}, rr = a->rs + b->rs, d2 = dot3(d, d);
    if (d2 >= rr * rr || d2 < 1e-8f)
        return;
    float l = psqrt(d2), n[3] = {d[0] / l, d[1] / l, d[2] / l}, pen = rr - l;
    float wa = a->asleep ? 0 : a->inv_mass, wb = b->asleep ? 0 : b->inv_mass, ws = wa + wb;
    if (ws <= 0)
        return;
    for (int k = 0; k < 3; k++) {
        a->p[k] -= n[k] * pen * 0.5f * wa / ws;
        b->p[k] += n[k] * pen * 0.5f * wb / ws;
    }
    float vn = (b->v[0] - a->v[0]) * n[0] + (b->v[1] - a->v[1]) * n[1] + (b->v[2] - a->v[2]) * n[2];
    if (vn < 0) {
        float j = -vn / ws;
        for (int k = 0; k < 3; k++)
            a->v[k] -= n[k] * j * wa, b->v[k] += n[k] * j * wb;
    }
    phys_contacts++;
}

int phys_step(Body *bodies, int n, float dt)
{
    phys_queries = phys_contacts = 0;
    if (dt <= 0)
        return 0;
    if (dt > 1 / 20.0f)
        dt = 1 / 20.0f;
    int awake = 0;
    static int start;  /* round-robin: the bodies first in line for the query budget change every frame */
    if (n > PHYS_MAX)
        n = PHYS_MAX;
    start = n ? (start + 1) % n : 0;
    for (int k = 0; k < n; k++) {
        int i = (start + k) % n;
        Body *b = &bodies[i];
        if (!b->active || b->asleep)
            continue;
        awake++;
        if (b->nocol > 0)
            b->nocol -= dt;
        float c0[3];
        phys_point(b, b->c, c0);
        /* floor plane under the box centre, from a little above it: cached until the body moved FLOOR_MOVE sideways,
         * dropped below it, or (no floor found) FLOOR_RETRY frames passed; col_floor is the EE's main cost here */
        float fx = c0[0] - b->fl_x, fz = c0[2] - b->fl_z;
        if ((!b->fl_age || fx * fx + fz * fz > FLOOR_MOVE * FLOOR_MOVE || (b->fl_ok && c0[1] < b->fl_o[1] - 0.3f) ||
            (!b->fl_ok && b->fl_age > FLOOR_RETRY)) && phys_queries < QUERY_BUDGET) {
            ColHit h;
            b->fl_ok = col_floor(c0[0], c0[2], c0[1] + b->rs + 0.3f, c0[1] - 4.0f, COL_SOLID, &h);
            b->fl_x = c0[0], b->fl_z = c0[2], b->fl_age = 0;
            phys_queries++;
            if (b->fl_ok) {
                b->fl_o[0] = c0[0], b->fl_o[1] = h.y, b->fl_o[2] = c0[2];
                memcpy(b->fl_n, h.n, sizeof h.n);
            }
        }
        b->fl_age++;
        float sdt = dt / PHYS_SUBSTEPS;
        int touching = 0;
        for (int s = 0; s < PHYS_SUBSTEPS; s++) {
            integrate(b, sdt);
            if (b->fl_ok)
                touching += plane_contacts(b, b->fl_o, b->fl_n, sdt);
        }
        phys_contacts += touching;
        if (touching) {  /* rolling resistance on the ground (ASSUMED; hkpMaterial rollingFrictionMultiplier unread) */
            float k = 1 - ROLL_DAMP * dt;
            b->w[0] *= k, b->w[1] *= k, b->w[2] *= k;
            if (dot3(b->v, b->v) < SLEEP_V2 && dot3(b->w, b->w) < SLEEP_W2) {
                k = 1 - SETTLE_DAMP * dt;
                for (int c = 0; c < 3; c++)
                    b->v[c] *= k, b->w[c] *= k;
            }
        }
        /* walls (and anything else the centre crossed) */
        float c1[3];
        phys_point(b, b->c, c1);
        /* every WALL_EVERY frames (staggered by body), from where the last ray ended: slow pieces skip it */
        if (!b->ray_age)
            memcpy(b->ray_o, c0, sizeof c0);
        if (++b->ray_age < WALL_EVERY - (i % WALL_EVERY))
            goto settle;
        b->ray_age = 0;
        memcpy(c0, b->ray_o, sizeof c0);
        float d[3] = {c1[0] - c0[0], c1[1] - c0[1], c1[2] - c0[2]}, l2 = dot3(d, d);
        ColHit h;
        if (d[0] * d[0] + d[2] * d[2] > WALL_MOVE * WALL_MOVE && phys_queries < QUERY_BUDGET) {
            float l = psqrt(l2), e = b->rs / l, to[3] = {c1[0] + d[0] * e, c1[1] + d[1] * e, c1[2] + d[2] * e};
            phys_queries++;
            /* floors are the plane contacts' (a ray hit on one would park the piece rs above it) */
            if (col_ray(c0, to, COL_SOLID, &h) && dot3(d, h.n) < 0 && h.n[1] < COL_WALKABLE_NY) {
                /* centre back to rs in front of the hit point (the normal faces the ray origin) */
                for (int k = 0; k < 3; k++)
                    b->p[k] += c0[k] + (to[k] - c0[k]) * h.t + h.n[k] * b->rs - c1[k];
                float vn = dot3(b->v, h.n);
                if (vn < 0)
                    for (int k = 0; k < 3; k++)
                        b->v[k] -= (1 + b->rest) * vn * h.n[k];
                touching++;
            }
        }
    settle:
        /* sleep once settled on something */
        if (touching && dot3(b->v, b->v) < SLEEP_V2 && dot3(b->w, b->w) < SLEEP_W2) {
            if ((b->still += dt) > SLEEP_TIME) {
                /* last check before sleeping: the floor right under the body, not the cached plane from up to
                 * FLOOR_MOVE away (a step there left pieces resting inside the floor) */
                if (phys_queries >= QUERY_BUDGET)
                    continue;
                ColHit h;
                float c[3];
                phys_point(b, b->c, c);
                phys_queries++;
                if (col_floor(c[0], c[2], c[1] + b->rs + 0.3f, c[1] - 4.0f, COL_SOLID, &h)) {
                    float dy = h.y - (b->fl_o[1] + ((b->fl_o[0] - c[0]) * b->fl_n[0] + (b->fl_o[2] - c[2]) * b->fl_n[2]) / b->fl_n[1]);
                    if (!b->fl_ok || dy > 0.02f || dy < -0.02f) {
                        b->fl_ok = 1, b->fl_age = 1, b->fl_x = c[0], b->fl_z = c[2], b->still = 0;
                        b->fl_o[0] = c[0], b->fl_o[1] = h.y, b->fl_o[2] = c[2];
                        memcpy(b->fl_n, h.n, sizeof h.n);
                        continue;
                    }
                }
                b->asleep = 1;
                memset(b->v, 0, sizeof b->v);
                memset(b->w, 0, sizeof b->w);
            }
        } else {
            b->still = 0;
        }
    }
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            Body *a = &bodies[i], *b = &bodies[j];
            if (!a->active || !b->active || (a->asleep && b->asleep))
                continue;
            if (a->group == b->group && (a->nocol > 0 || b->nocol > 0))
                continue;
            /* spheres are a poor fit for plates at rest (a 4 cm shard would hover rs above another): pieces only
             * collide while one of them flies (> PAIR_V), and may overlap once settled (TECH_DEBT physics-1) */
            if (dot3(a->v, a->v) < PAIR_V * PAIR_V && dot3(b->v, b->v) < PAIR_V * PAIR_V)
                continue;
            pair(a, b);
        }
    return awake;
}

void phys_wake(Body *b, int n, const float c[3], float r)
{
    for (int i = 0; i < n; i++) {
        if (!b[i].active || !b[i].asleep)
            continue;
        float d[3] = {b[i].p[0] - c[0], b[i].p[1] - c[1], b[i].p[2] - c[2]};
        if (dot3(d, d) < r * r)
            b[i].asleep = 0, b[i].still = 0;
    }
}
