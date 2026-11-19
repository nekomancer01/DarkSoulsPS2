/* Blob shadows (Polaris 2.2 PR 5, shadow.h).
 *
 * DSR casts character shadows into its cascaded sun shadow map (dynamic shadows, TECH_DEBT map-27); the port has no
 * shadow map, so each character gets the classic PS2 stand-in: a disc on the floor under its feet, black, alpha
 * SHADOW_A at the centre falling to 0 at the rim (gouraud, no texture), drawn with the decal material class's GS
 * state (tools/material.py DECAL: alpha MIX, Z test GEQUAL, no Z write; render.c R_BLEND) after the characters.
 *
 * The floor is col_floor straight down from the feet (COL_SOLID, 0.5 m above to SHADOW_DROP below), the disc laid in
 * the floor triangle's plane, lifted SHADOW_LIFT off it. It fades as the character rises above the floor (jumps,
 * falls, ledges) and over SHADOW_FAR_FADE before SHADOW_FAR. Radius = capsule radius x SHADOW_SCALE. All ASSUMED
 * (TECH_DEBT char-18): tuned by eye, not measured against DSR.
 *
 * SHADOW_BLOB 0 (make DEFS=-DSHADOW_BLOB=0) = no shadows (the old picture). */
#include <math.h>
#include <stddef.h>
#include "shadow.h"
#include "collision.h"

#ifndef SHADOW_BLOB
#define SHADOW_BLOB 1
#endif
#define SHADOW_MAX 32
#define SHADOW_SEG 12          /* rim segments */
#define SHADOW_A 72.0f         /* centre alpha (0x80 = 1.0): ~56 % darker */
#define SHADOW_SCALE 1.6f      /* radius / capsule radius */
#define SHADOW_LIFT 0.03f      /* metres off the floor */
#define SHADOW_DROP 3.0f       /* floor search below the feet; fades out over it */
#define SHADOW_FAR 30.0f       /* metres from the eye */
#define SHADOW_FAR_FADE 8.0f
#define SHADOW_BATCH 96        /* vertices per triangle list */

static struct {
    V3 feet;
    float r, fade;
} q[SHADOW_MAX];
static int nq;
int shadow_drawn;

void shadow_add(V3 feet, float r, float fade)
{
    if (!SHADOW_BLOB || nq >= SHADOW_MAX || fade <= 0)
        return;
    q[nq].feet = feet, q[nq].r = r * SHADOW_SCALE, q[nq].fade = fade > 1 ? 1 : fade;
    nq++;
}

static ClipVert lv[SHADOW_BATCH];
static float ls[SHADOW_BATCH][3];
static int nl;

static void flush(void)
{
    if (nl)
        r_tri_list(NULL, R_BLEND, nl, lv, (const float (*)[3])ls);
    nl = 0;
}

static ClipVert cv(V3 w, float a)
{
    V3 v = r_view(w);
    ClipVert c = {v.x, v.y, v.z, 0, 0, 0, 0, 0, a};
    return c;
}

void shadow_draw(void)
{
    shadow_drawn = 0;
    if (!nq)
        return;
    V3 eye, right, up, fwd;
    float f, fy, zs;
    r_camera_basis(&eye, &right, &up, &fwd, &f, &fy, &zs);
    static float rim[SHADOW_SEG][2];
    if (!rim[0][0])
        for (int k = 0; k < SHADOW_SEG; k++)
            rim[k][0] = cosf(k * 6.2831853f / SHADOW_SEG), rim[k][1] = sinf(k * 6.2831853f / SHADOW_SEG);
    for (int i = 0; i < nq; i++) {
        V3 p = q[i].feet;
        float dx = p.x - eye.x, dz = p.z - eye.z, d = sqrtf(dx * dx + dz * dz), r = q[i].r;
        if (d > SHADOW_FAR)
            continue;
        const float mn[3] = {p.x - r, p.y - SHADOW_DROP, p.z - r}, mx[3] = {p.x + r, p.y + 0.5f, p.z + r};
        if (!r_box_classify(mn, mx))
            continue;
        ColHit h;
        if (!col_floor(p.x, p.z, p.y + 0.5f, p.y - SHADOW_DROP, COL_SOLID, &h))
            continue;
        float hgt = p.y - h.y, a = SHADOW_A * q[i].fade;
        a *= hgt > 0 ? 1 - hgt / SHADOW_DROP : 1;
        a *= d > SHADOW_FAR - SHADOW_FAR_FADE ? (SHADOW_FAR - d) / SHADOW_FAR_FADE : 1;
        if (a < 2)
            continue;
        /* the floor plane's basis: n up, t1 / t2 along it */
        float n[3] = {h.n[0], h.n[1], h.n[2]};
        if (n[1] < 0.3f)  /* (a steep wall face: flat instead) */
            n[0] = n[2] = 0, n[1] = 1;
        float t1[3] = {n[1], -n[0], 0}, l = sqrtf(t1[0] * t1[0] + t1[1] * t1[1]);  /* n x z */
        t1[0] /= l, t1[1] /= l;
        float t2[3] = {n[1] * t1[2] - n[2] * t1[1], n[2] * t1[0] - n[0] * t1[2], n[0] * t1[1] - n[1] * t1[0]};
        V3 c = v3(p.x + n[0] * SHADOW_LIFT, h.y + n[1] * SHADOW_LIFT, p.z + n[2] * SHADOW_LIFT);
        ClipVert cc = cv(c, a), ring[SHADOW_SEG];
        for (int k = 0; k < SHADOW_SEG; k++) {
            float u = rim[k][0] * r, w = rim[k][1] * r;
            /* (the rim follows the floor's plane; on the floor's slope the y term keeps it on the plane) */
            ring[k] = cv(v3(c.x + t1[0] * u + t2[0] * w, c.y + t1[1] * u + t2[1] * w, c.z + t1[2] * u + t2[2] * w), 0);
        }
        int inside = r_point_inside((const V3 *)&cc.x);
        for (int k = 0; k < SHADOW_SEG && inside; k++)
            inside = r_point_inside((const V3 *)&ring[k].x);
        for (int k = 0; k < SHADOW_SEG; k++) {
            const ClipVert *b = &ring[k], *e = &ring[(k + 1) % SHADOW_SEG];
            if (!inside) {
                r_tri_clip(NULL, &cc, b, e, 0, R_BLEND);
                continue;
            }
            if (nl + 3 > SHADOW_BATCH)
                flush();
            const ClipVert *t[3] = {&cc, b, e};
            for (int j = 0; j < 3; j++, nl++) {
                lv[nl] = *t[j];
                r_view_to_screen((const V3 *)&lv[nl].x, ls[nl]);
            }
        }
        shadow_drawn++;
    }
    flush();
    nq = 0;
}
