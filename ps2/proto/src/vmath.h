/* Shared float vector / matrix helpers: the small ones inline, the rest in vmath.c. Two forms of a 3-vector:
 * V3 {x, y, z} (render, AI, the player) and float[3] (collision, physics, ragdoll, combat). M34 is a rigid 3x4
 * transform: rotation rows m[0..8], translation m[9..11]; the bone chain (model.c), the ragdoll and the hit capsules
 * all use it. Quaternions are (x y z w). The EE FPU is single precision only: keep every literal float
 * (tools/ci/compile.sh fails on double math). sqrtf compiles to an inline sqrt.s (no errno call). */
#ifndef VMATH_H
#define VMATH_H

#include <math.h>

#define PI 3.14159265f

/* ---- scalars ---- */

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

/* angle into [-pi, pi] (the inputs are at most a few turns off) */
static inline float wrap(float a)
{
    while (a > PI)
        a -= 2 * PI;
    while (a < -PI)
        a += 2 * PI;
    return a;
}

/* ---- V3 ---- */

typedef struct { float x, y, z; } V3;

static inline V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
static inline V3 v3add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 v3sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 v3scale(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 v3cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float v3len(V3 a) { return sqrtf(v3dot(a, a)); }
static inline float v3dist(V3 a, V3 b) { return v3len(v3sub(a, b)); }
static inline V3 v3norm(V3 a) { float l = v3len(a); return l > 0 ? v3scale(a, 1.0f / l) : a; }
/* the V3 as float[3] (V3 is three packed floats) */
static inline float *v3f(V3 *a) { return &a->x; }

/* ---- float[3] ---- */

static inline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void sub3(const float *a, const float *b, float *o)
{
    o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2];
}
/* o may alias a or b */
static inline void cross3(const float *a, const float *b, float *o)
{
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
static inline void lerp3(const float *a, const float *b, float t, float *o)
{
    o[0] = a[0] + (b[0] - a[0]) * t, o[1] = a[1] + (b[1] - a[1]) * t, o[2] = a[2] + (b[2] - a[2]) * t;
}
static inline float dist3(const float *a, const float *b)
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

/* ---- out of line (vmath.c): bigger bodies, several callers per file ---- */

/* normalise in place (left alone when shorter than 1e-9); returns the old length */
float norm3(float *a);
/* squared distance between segments p1-q1 and p2-q2 (Ericson 5.1.9) */
float seg_seg_d2(const float *p1, const float *q1, const float *p2, const float *q2);
/* quaternion product a * b; o may alias a or b */
void q_mul(const float *a, const float *b, float *o);
/* o = q v q^-1 for a unit q; o may alias v */
void q_rot(const float *q, const float *v, float *o);

/* ---- M34: rotation rows m[0..8], translation m[9..11] ---- */

typedef struct { float m[12]; } M34;

/* rotation rows of a unit quaternion */
static inline void m3_from_q(const float *q, float *m)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y - z * w), m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w), m[4] = 1 - 2 * (x * x + z * z), m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w), m[7] = 2 * (y * z + x * w), m[8] = 1 - 2 * (x * x + y * y);
}

/* rotation q, then translation t */
static inline M34 m34_from_qt(const float *q, const float *t)
{
    M34 r;
    m3_from_q(q, r.m);
    r.m[9] = t[0], r.m[10] = t[1], r.m[11] = t[2];
    return r;
}

/* a * b (b first) */
M34 m34_mul(const M34 *a, const M34 *b);
/* inverse of a rigid transform */
M34 m34_inv(const M34 *a);

/* o = m p (a point: rotation and translation) */
static inline void m34_apply(const M34 *m, const float *p, float *o)
{
    float x = m->m[0] * p[0] + m->m[1] * p[1] + m->m[2] * p[2] + m->m[9];
    float y = m->m[3] * p[0] + m->m[4] * p[1] + m->m[5] * p[2] + m->m[10];
    float z = m->m[6] * p[0] + m->m[7] * p[1] + m->m[8] * p[2] + m->m[11];
    o[0] = x, o[1] = y, o[2] = z;
}

#endif
