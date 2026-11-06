/* Ragdolls and hit capsules from DSR's character hkx bodies (ragdoll.h).
 *
 * Simulation: position-based rigid bodies (after Mueller et al. 2020, "Detailed Rigid Body Simulation with Extended
 * Position Based Dynamics", without compliance): each substep integrates gravity and velocities, then moves bodies
 * directly to satisfy the joints (pivot coincidence), the joint cone and twist limits from the hkx
 * (hkpRagdollConstraintData) and the floor, and derives the velocities from what moved. Approximations (TECH_DEBT
 * enemy-17): isotropic inertia from the capsule shape, no body-body or wall contacts, the floor as one plane per body
 * per frame, damping / friction values ASSUMED (the hkx motion damping is not exported). */
#include <math.h>
#include <string.h>
#include "ragdoll.h"
#include "collision.h"
#include "vmath.h"

#define SUBSTEPS 4
#define GRAVITY 9.8f
#define LIN_DAMP 0.0f      /* 1/s: hkMotionState linearDamping of the c2510 bodies (all 0) */
#define ANG_DAMP 0.05f     /* 1/s: hkMotionState angularDamping (all 0.05); not read per type, TECH_DEBT enemy-17 */
#define FRICTION 0.8f      /* floor, ASSUMED */
#define ROLL_DAMP 10.0f    /* 1/s: angular damping of a body touching the floor (rolling friction, ASSUMED: without it
                            * feet and the neck kept twitching between the joint limits and the floor) */
#define MAX_SPEED 20.0f
#define SLEEP_V 0.06f      /* m/s: every body centre slower than this for SLEEP_TIME = at rest (centres, not
                            * capsule ends: small bodies between two joint limits, the clavicles, keep a slight
                            * rotational jitter in this solver) */
#define SLEEP_TIME 0.7f
#define MAX_AGE 10.0f      /* s: simulated at most this long after the last wake (EE budget) */

static void quat_of(const M34 *a, float *q)
{
    const float *m = a->m;
    float tr = m[0] + m[4] + m[8], s;
    if (tr > 0) {
        s = sqrtf(tr + 1) * 2;
        q[0] = (m[7] - m[5]) / s, q[1] = (m[2] - m[6]) / s, q[2] = (m[3] - m[1]) / s, q[3] = s / 4;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        s = sqrtf(1 + m[0] - m[4] - m[8]) * 2;
        q[0] = s / 4, q[1] = (m[1] + m[3]) / s, q[2] = (m[2] + m[6]) / s, q[3] = (m[7] - m[5]) / s;
    } else if (m[4] > m[8]) {
        s = sqrtf(1 + m[4] - m[0] - m[8]) * 2;
        q[0] = (m[1] + m[3]) / s, q[1] = s / 4, q[2] = (m[5] + m[7]) / s, q[3] = (m[2] - m[6]) / s;
    } else {
        s = sqrtf(1 + m[8] - m[0] - m[4]) * 2;
        q[0] = (m[2] + m[6]) / s, q[1] = (m[5] + m[7]) / s, q[2] = s / 4, q[3] = (m[3] - m[1]) / s;
    }
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++)
        q[i] /= n;
}

static void qrot_inv(const float *q, const float *v, float *o)
{
    float c[4] = {-q[0], -q[1], -q[2], q[3]};
    q_rot(c, v, o);
}

/* q turned by the rotation vector w (world axis * angle). Exact: the linearised XPBD update (q += 0.5 [w, 0] q)
 * kept the c2510 test ragdoll jittering past MAX_AGE, this one rests after ~3.5 s (host test, 2026-11-03) */
static void qturn(float *q, const float *w)
{
    float a[3] = {w[0], w[1], w[2]}, ang = norm3(a);
    if (ang < 1e-7f)
        return;
    float s = sinf(ang * 0.5f), d[4] = {a[0] * s, a[1] * s, a[2] * s, cosf(ang * 0.5f)};
    q_mul(d, q, q);
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++)
        q[i] /= n;
}

/* ---- hit capsules ---- */

int rag_hit(const RagBody *body, int n, const float (*world)[12], const float a[3], const float b[3], float r)
{
    int best = -1;
    float bd = 1e9f;
    for (int k = 0; k < n; k++) {
        const M34 *w = (const M34 *)world[body[k].bone];
        float pa[3], pb[3];
        m34_apply(w, body[k].a, pa), m34_apply(w, body[k].b, pb);
        float reach = r + body[k].radius, d = seg_seg_d2(a, b, pa, pb);
        if (d <= reach * reach && d < bd)
            bd = d, best = k;
    }
    return best;
}

/* ---- simulation ---- */

static M34 body_frame(const RagBody *b) { return m34_from_qt(b->q, b->t); }

/* world frame of body k from bone world matrices */
static M34 body_world(const RagBody *b, const float (*world)[12])
{
    M34 B = body_frame(b);
    return m34_mul((const M34 *)world[b->bone], &B);
}

void rag_start(Ragdoll *r, const RagBody *body, int n, const float (*bind)[12], const float (*world)[12],
               const float (*prev)[12], float dt)
{
    memset(r, 0, sizeof *r);
    r->def = body, r->n = n > RAG_MAX ? RAG_MAX : n, r->on = 1;
    float bq[RAG_MAX][4];
    for (int k = 0; k < r->n; k++) {
        const RagBody *b = &body[k];
        for (int i = 0; i < 3; i++)
            r->com[k][i] = (b->ba[i] + b->bb[i]) * 0.5f;
        M34 w = body_world(b, world), p = body_world(b, prev), bw = body_world(b, bind);
        float xp[3], qp[4], dq[4], c[4];
        m34_apply(&w, r->com[k], r->x[k]), quat_of(&w, r->q[k]);
        m34_apply(&p, r->com[k], xp), quat_of(&p, qp);
        quat_of(&bw, bq[k]);
        float inv = dt > 1e-4f ? 1 / dt : 0;
        for (int i = 0; i < 3; i++)
            r->v[k][i] = (r->x[k][i] - xp[i]) * inv;
        c[0] = -qp[0], c[1] = -qp[1], c[2] = -qp[2], c[3] = qp[3];
        q_mul(r->q[k], c, dq);
        float sg = dq[3] < 0 ? -2 : 2;
        for (int i = 0; i < 3; i++)
            r->w[k][i] = dq[i] * sg * inv;
        float m = b->mass > 0 ? b->mass : 10, len[3] = {b->bb[0] - b->ba[0], b->bb[1] - b->ba[1], b->bb[2] - b->ba[2]};
        float l2 = dot3(len, len);
        r->im[k] = 1 / m;
        r->ii[k] = 1 / (m * (l2 / 12 + 0.4f * b->radius * b->radius) + 1e-4f);  /* ASSUMED isotropic capsule */
    }
    /* twist references: a perpendicular of the child's twist axis, and where it lies in the parent at bind */
    for (int k = 0; k < r->n; k++) {
        const RagBody *b = &body[k];
        if (b->parent < 0 || b->parent >= r->n)
            continue;
        float up[3] = {0, 1, 0}, *pc = r->perp_c[k], w[3];
        cross3(b->axis_c, up, pc);
        if (norm3(pc) < 1e-3f) {
            float z[3] = {0, 0, 1};
            cross3(b->axis_c, z, pc), norm3(pc);
        }
        q_rot(bq[k], pc, w);
        qrot_inv(bq[b->parent], w, r->perp_p[k]);
    }
}

/* positional correction p (world) at offset rel from body k's centre: sign +1 / -1 */
static void apply_corr(Ragdoll *r, int k, const float *rel, const float *p, float sign)
{
    float t[3];
    for (int i = 0; i < 3; i++)
        r->x[k][i] += sign * p[i] * r->im[k];
    cross3(rel, p, t);
    for (int i = 0; i < 3; i++)
        t[i] *= sign * r->ii[k];
    qturn(r->q[k], t);
}

static float gen_inv_mass(const Ragdoll *r, int k, const float *rel, const float *n)
{
    float c[3];
    cross3(rel, n, c);
    return r->im[k] + r->ii[k] * dot3(c, c);
}

static void solve_joint(Ragdoll *r, int c)
{
    const RagBody *b = &r->def[c];
    int p = b->parent;
    float lc[3], lp[3], rc[3], rp[3], d[3];
    for (int i = 0; i < 3; i++)
        lc[i] = b->piv_c[i] - r->com[c][i], lp[i] = b->piv_p[i] - r->com[p][i];
    q_rot(r->q[c], lc, rc), q_rot(r->q[p], lp, rp);
    for (int i = 0; i < 3; i++)
        d[i] = r->x[p][i] + rp[i] - r->x[c][i] - rc[i];
    float len = norm3(d);
    if (len < 1e-5f)
        return;
    float w = gen_inv_mass(r, c, rc, d) + gen_inv_mass(r, p, rp, d), lam = len / w, P[3] = {d[0] * lam, d[1] * lam,
                                                                                               d[2] * lam};
    apply_corr(r, c, rc, P, 1), apply_corr(r, p, rp, P, -1);
}

/* rotate c by angle * share about u, p by the rest the other way (isotropic inertia: shares by inverse inertia) */
static void turn_pair(Ragdoll *r, int c, int p, const float *u, float angle)
{
    float wsum = r->ii[c] + r->ii[p];
    if (wsum <= 0)
        return;
    float tc[3], tp[3];
    for (int i = 0; i < 3; i++)
        tc[i] = u[i] * angle * r->ii[c] / wsum, tp[i] = -u[i] * angle * r->ii[p] / wsum;
    qturn(r->q[c], tc), qturn(r->q[p], tp);
}

static void solve_limits(Ragdoll *r, int c)
{
    const RagBody *b = &r->def[c];
    int p = b->parent;
    float ac[3], ap[3], u[3];
    q_rot(r->q[c], b->axis_c, ac), q_rot(r->q[p], b->axis_p, ap);
    float cs = dot3(ac, ap);
    cs = cs > 1 ? 1 : cs < -1 ? -1 : cs;
    float ang = acosf(cs);
    if (b->cone > 0 && ang > b->cone) {  /* cone: bring the child's axis back inside */
        cross3(ac, ap, u);
        if (norm3(u) > 1e-6f)
            turn_pair(r, c, p, u, ang - b->cone);
        q_rot(r->q[c], b->axis_c, ac);
    }
    /* twist about the child's axis, from the parent's bind reference to the child's */
    float pc[3], pp[3], x[3];
    q_rot(r->q[c], r->perp_c[c], pc), q_rot(r->q[p], r->perp_p[c], pp);
    float k = dot3(pp, ac);
    for (int i = 0; i < 3; i++)
        pp[i] -= ac[i] * k;
    if (norm3(pp) < 1e-4f)
        return;
    cross3(pp, pc, x);
    float tw = atan2f(dot3(x, ac), dot3(pp, pc)), over = 0;
    if (tw > b->twist_max)
        over = tw - b->twist_max;
    else if (tw < b->twist_min)
        over = tw - b->twist_min;
    if (over != 0)
        turn_pair(r, c, p, ac, -over);
}

static int solve_floor(Ragdoll *r, int k, const float *xp, const float *qp)
{
    int touched = 0;
    if (!r->fok[k])
        return 0;
    const RagBody *b = &r->def[k];
    const float *n = r->fn[k];
    const float *ends[2] = {b->ba, b->bb};
    for (int e = 0; e < 2; e++) {
        float l[3], rel[3], p[3];
        for (int i = 0; i < 3; i++)
            l[i] = ends[e][i] - r->com[k][i];
        q_rot(r->q[k], l, rel);
        for (int i = 0; i < 3; i++)
            p[i] = r->x[k][i] + rel[i];
        float o[3] = {p[0] - r->x[k][0], p[1] - r->fy[k], p[2] - r->x[k][2]};
        float dist = dot3(o, n) - b->radius;
        if (dist >= 0)
            continue;
        touched = 1;
        for (int i = 0; i < 3; i++)
            rel[i] -= n[i] * b->radius;  /* the contact point on the capsule's surface */
        float pen = -dist, w = gen_inv_mass(r, k, rel, n), P[3];
        for (int i = 0; i < 3; i++)
            P[i] = n[i] * pen / w;
        apply_corr(r, k, rel, P, 1);
        /* friction: undo the contact point's sliding this substep, up to FRICTION x the push out */
        float lp[3], was[3], now[3], slide[3];
        qrot_inv(r->q[k], rel, lp);
        q_rot(qp, lp, was), q_rot(r->q[k], lp, now);
        for (int i = 0; i < 3; i++)
            slide[i] = (r->x[k][i] + now[i]) - (xp[i] + was[i]);
        float sn = dot3(slide, n);
        for (int i = 0; i < 3; i++)
            slide[i] -= n[i] * sn;
        float sl = sqrtf(dot3(slide, slide));
        if (sl < 1e-6f)
            continue;
        float keep = sl > FRICTION * pen ? FRICTION * pen / sl : 1, dir[3] = {-slide[0] / sl, -slide[1] / sl,
                                                                              -slide[2] / sl};
        float wt = gen_inv_mass(r, k, rel, dir);
        for (int i = 0; i < 3; i++)
            P[i] = -slide[i] * keep / wt;
        apply_corr(r, k, rel, P, 1);
    }
    return touched;
}

void rag_step(Ragdoll *r, float dt)
{
    if (!r->on || r->asleep || dt <= 0)
        return;
    r->age += dt;
    /* the floor under each body, once a frame */
    for (int k = 0; k < r->n; k++) {
        ColHit hit;
        r->fok[k] = col_floor(r->x[k][0], r->x[k][2], r->x[k][1] + 1.0f, r->x[k][1] - 4.0f, COL_SOLID, &hit);
        if (r->fok[k])
            r->fy[k] = hit.y, memcpy(r->fn[k], hit.n, 12);
    }
    float h = dt / SUBSTEPS, xp[RAG_MAX][3], qp[RAG_MAX][4], fast = 0;
    for (int s = 0; s < SUBSTEPS; s++) {
        for (int k = 0; k < r->n; k++) {
            memcpy(xp[k], r->x[k], 12), memcpy(qp[k], r->q[k], 16);
            r->v[k][1] -= GRAVITY * h;
            float t[3];
            for (int i = 0; i < 3; i++)
                r->x[k][i] += r->v[k][i] * h, t[i] = r->w[k][i] * h;
            qturn(r->q[k], t);
        }
        for (int k = 0; k < r->n; k++)
            if (r->def[k].parent >= 0 && r->def[k].parent < r->n)
                solve_joint(r, k);
        for (int k = 0; k < r->n; k++)
            if (r->def[k].parent >= 0 && r->def[k].parent < r->n)
                solve_limits(r, k);
        unsigned char touch[RAG_MAX];
        for (int k = 0; k < r->n; k++)
            touch[k] = solve_floor(r, k, xp[k], qp[k]);
        float lin = 1 - LIN_DAMP * h;
        for (int k = 0; k < r->n; k++) {
            float ang = 1 - (touch[k] ? ROLL_DAMP : ANG_DAMP) * h;
            float c[4] = {-qp[k][0], -qp[k][1], -qp[k][2], qp[k][3]}, dq[4];
            q_mul(r->q[k], c, dq);
            float sg = dq[3] < 0 ? -2 / h : 2 / h, sp = 0;
            for (int i = 0; i < 3; i++) {
                r->v[k][i] = (r->x[k][i] - xp[k][i]) / h * lin;
                r->w[k][i] = dq[i] * sg * ang;
                sp += r->v[k][i] * r->v[k][i];
            }
            if (sp > MAX_SPEED * MAX_SPEED)
                for (int i = 0; i < 3; i++)
                    r->v[k][i] *= MAX_SPEED / sqrtf(sp);
            float lv = sqrtf(dot3(r->v[k], r->v[k]));
            fast = fast > lv ? fast : lv;
        }
    }
    r->still = fast < SLEEP_V ? r->still + dt : 0;
    if (r->still > SLEEP_TIME || r->age > MAX_AGE)
        r->asleep = 1;
}

void rag_push(Ragdoll *r, const float feet[3], float radius, float height, const float vel[3])
{
    if (!r->on)
        return;
    for (int k = 0; k < r->n; k++) {
        float *x = r->x[k], dx = x[0] - feet[0], dz = x[2] - feet[2], d = sqrtf(dx * dx + dz * dz),
              min = radius + r->def[k].radius;
        if (d >= min || x[1] < feet[1] - 0.2f || x[1] > feet[1] + height)
            continue;
        if (d < 1e-3f)
            dx = 1, dz = 0, d = 1;
        x[0] += dx / d * (min - d), x[2] += dz / d * (min - d);
        r->v[k][0] += vel[0] * 0.5f, r->v[k][2] += vel[2] * 0.5f;  /* ASSUMED: half the walker's speed */
        if (r->asleep)
            r->asleep = 0, r->still = 0, r->age = r->age > MAX_AGE - 3 ? MAX_AGE - 3 : r->age;
    }
}

void rag_pose(const Ragdoll *r, const Model *m, const BoneLocal *base, V3 pos, float yaw, BoneLocal *out)
{
    int nb = mdl_bones(m);
    M34 W[nb];  /* VLA: <= 12 KB of stack at MDL_MAX_BONES */
    signed char of[nb];
    const short *par = mdl_parents(m);
    memset(of, -1, sizeof of);
    for (int k = r->n - 1; k >= 0; k--)  /* the first body of a bone wins */
        if (r->def[k].bone >= 0 && r->def[k].bone < nb)
            of[r->def[k].bone] = k;
    float c = cosf(yaw + 3.14159265f), s = sinf(yaw + 3.14159265f);
    M34 place = {{c, 0, s, 0, 1, 0, -s, 0, c, pos.x, pos.y, pos.z}}, iplace = m34_inv(&place);
    for (int i = 0; i < nb; i++) {
        int k = of[i];
        const M34 *P = par[i] < 0 ? &place : &W[par[i]];
        if (k >= 0) {
            float o[3], rc[3];
            q_rot(r->q[k], r->com[k], rc);
            for (int j = 0; j < 3; j++)
                o[j] = r->x[k][j] - rc[j];
            M34 body = m34_from_qt(r->q[k], o), B = body_frame(&r->def[k]), iB = m34_inv(&B);
            W[i] = m34_mul(&body, &iB);
        } else {
            M34 L = m34_from_qt(base[i].q, base[i].t);
            W[i] = m34_mul(P, &L);
        }
        M34 iP = par[i] < 0 ? iplace : m34_inv(P), L = m34_mul(&iP, &W[i]);
        quat_of(&L, out[i].q);
        memcpy(out[i].t, &L.m[9], 12);
    }
}
