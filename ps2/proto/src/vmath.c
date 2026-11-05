/* Out-of-line helpers of vmath.h (the ones with several callers per file, where inlining grew ragdoll.o by 4 KB:
 * the EE has a 16 KB I-cache). */
#include "vmath.h"

float norm3(float *a)
{
    float l = sqrtf(dot3(a, a));
    if (l > 1e-9f)
        a[0] /= l, a[1] /= l, a[2] /= l;
    return l;
}

float seg_seg_d2(const float *p1, const float *q1, const float *p2, const float *q2)
{
    float d1[3], d2[3], r[3], s, t;
    sub3(q1, p1, d1), sub3(q2, p2, d2), sub3(p1, p2, r);
    float a = dot3(d1, d1), e = dot3(d2, d2), f = dot3(d2, r);
    if (a <= 1e-9f && e <= 1e-9f) {
        s = t = 0;
    } else if (a <= 1e-9f) {
        s = 0, t = f / e;
    } else {
        float c = dot3(d1, r);
        if (e <= 1e-9f) {
            t = 0, s = -c / a;
        } else {
            float b = dot3(d1, d2), den = a * e - b * b;
            s = den != 0 ? (b * f - c * e) / den : 0;
            s = clamp01(s);
            t = (b * s + f) / e;
            if (t < 0)
                t = 0, s = -c / a;
            else if (t > 1)
                t = 1, s = (b - c) / a;
        }
        s = clamp01(s);
    }
    t = clamp01(t);
    float d = 0;
    for (int i = 0; i < 3; i++) {
        float k = (p1[i] + d1[i] * s) - (p2[i] + d2[i] * t);
        d += k * k;
    }
    return d;
}

void q_mul(const float *a, const float *b, float *o)
{
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x, o[1] = y, o[2] = z, o[3] = w;
}

void q_rot(const float *q, const float *v, float *o)
{
    float t[3], u[3];
    cross3(q, v, t);
    t[0] *= 2, t[1] *= 2, t[2] *= 2;
    cross3(q, t, u);
    for (int i = 0; i < 3; i++)
        o[i] = v[i] + q[3] * t[i] + u[i];
}

M34 m34_mul(const M34 *a, const M34 *b)
{
    M34 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            r.m[i * 3 + j] = a->m[i * 3] * b->m[j] + a->m[i * 3 + 1] * b->m[3 + j] + a->m[i * 3 + 2] * b->m[6 + j];
        r.m[9 + i] = a->m[i * 3] * b->m[9] + a->m[i * 3 + 1] * b->m[10] + a->m[i * 3 + 2] * b->m[11] + a->m[9 + i];
    }
    return r;
}

M34 m34_inv(const M34 *a)
{
    M34 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r.m[i * 3 + j] = a->m[j * 3 + i];
    for (int i = 0; i < 3; i++)
        r.m[9 + i] = -(r.m[i * 3] * a->m[9] + r.m[i * 3 + 1] * a->m[10] + r.m[i * 3 + 2] * a->m[11]);
    return r;
}

void c44_mul_skin(float (*o)[4], const float (*a)[4], const float (*b)[4], const float *l)
{
#if defined(_EE) && !defined(NO_VU0)
    /* vf1-4 = a's columns, vf5-8 = b's, vf9 = l; vf10-13 = o's columns, vf14-16 = the light products. Macro-mode
     * COP2 instructions interlock, so no manual hazard handling; the three light sums are interleaved to hide the
     * 4-cycle FMAC latency. vf0 = (0, 0, 0, 1). */
    __asm__ volatile(
        "lqc2 $vf1, 0x00(%1)\n\t"
        "lqc2 $vf2, 0x10(%1)\n\t"
        "lqc2 $vf3, 0x20(%1)\n\t"
        "lqc2 $vf4, 0x30(%1)\n\t"
        "lqc2 $vf5, 0x00(%2)\n\t"
        "lqc2 $vf6, 0x10(%2)\n\t"
        "lqc2 $vf7, 0x20(%2)\n\t"
        "lqc2 $vf8, 0x30(%2)\n\t"
        "lqc2 $vf9, 0x00(%3)\n\t"
        "vmulax.xyz $ACC, $vf1, $vf5x\n\t"   /* rotation columns: a3 drops out (b's w = 0) */
        "vmadday.xyz $ACC, $vf2, $vf5y\n\t"
        "vmaddz.xyz $vf10, $vf3, $vf5z\n\t"
        "vmulax.xyz $ACC, $vf1, $vf6x\n\t"
        "vmadday.xyz $ACC, $vf2, $vf6y\n\t"
        "vmaddz.xyz $vf11, $vf3, $vf6z\n\t"
        "vmulax.xyz $ACC, $vf1, $vf7x\n\t"
        "vmadday.xyz $ACC, $vf2, $vf7y\n\t"
        "vmaddz.xyz $vf12, $vf3, $vf7z\n\t"
        "vmulax.xyz $ACC, $vf1, $vf8x\n\t"   /* translation: + a3 (b's w = 1) */
        "vmadday.xyz $ACC, $vf2, $vf8y\n\t"
        "vmaddaz.xyz $ACC, $vf3, $vf8z\n\t"
        "vmaddw.xyz $vf13, $vf4, $vf0w\n\t"
        "vmul.xyz $vf14, $vf10, $vf9\n\t"
        "vmul.xyz $vf15, $vf11, $vf9\n\t"
        "vmul.xyz $vf16, $vf12, $vf9\n\t"
        "vsub.w $vf13, $vf0, $vf0\n\t"       /* translation .w = 0 */
        "vaddy.x $vf14, $vf14, $vf14y\n\t"
        "vaddy.x $vf15, $vf15, $vf15y\n\t"
        "vaddy.x $vf16, $vf16, $vf16y\n\t"
        "sqc2 $vf13, 0x30(%0)\n\t"
        "vaddz.x $vf14, $vf14, $vf14z\n\t"
        "vaddz.x $vf15, $vf15, $vf15z\n\t"
        "vaddz.x $vf16, $vf16, $vf16z\n\t"
        "vmr32.w $vf10, $vf14\n\t"           /* .w <- .x */
        "vmr32.w $vf11, $vf15\n\t"
        "vmr32.w $vf12, $vf16\n\t"
        "sqc2 $vf10, 0x00(%0)\n\t"
        "sqc2 $vf11, 0x10(%0)\n\t"
        "sqc2 $vf12, 0x20(%0)\n\t"
        : : "r"(o), "r"(a), "r"(b), "r"(l) : "memory");
#else
    float r[4][4];
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 3; i++)
            r[j][i] = a[0][i] * b[j][0] + a[1][i] * b[j][1] + a[2][i] * b[j][2] + (j == 3 ? a[3][i] : 0);
    for (int j = 0; j < 3; j++)
        r[j][3] = r[j][0] * l[0] + r[j][1] * l[1] + r[j][2] * l[2];
    r[3][3] = 0;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            o[j][i] = r[j][i];
#endif
}
