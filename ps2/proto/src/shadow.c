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
 * SHADOW_BLOB 0 (make DEFS=-DSHADOW_BLOB=0) = no shadows (the old picture).
 *
 * The player's silhouette (PR 6, SHADOW_SIL; 0 = the player gets a blob too). Before the player's own draw in the
 * character chain the GS is pointed at a 128 x 64 CT16 target (FRAME, its own Z, SCISSOR, XYOFFSET so the skin
 * programs' screen centre lands at its centre), cleared to alpha 0, and the player is drawn once more by the same skin
 * programs from a camera SIL_DIST m up the sun direction (r_camera_override; height along the target's x). Only the
 * coverage is used: textured texels with alpha >= 0x80 set the target's alpha bit (cut-out texels are alpha tested
 * away as in the normal draw). shadow_draw then lays the target on the floor plane under the player (col_floor) as a
 * textured quad whose corners are the target's corners cast along the sun onto that plane: black, MODULATE with the
 * alpha bit (TEXA 0 / 0x80), alpha MIX, so it darkens the floor where the player blocks the sun. Below SIL_MIN_SUN
 * (sun near the horizon: the quad would stretch for metres) the player keeps the blob. DSR draws every character
 * into its cascaded shadow maps; this is the player only (TECH_DEBT char-18). */
#include <math.h>
#include <stddef.h>
#include "shadow.h"
#include "collision.h"
#include "map.h"
#include "gs.h"
#include "vu1.h"
#include "g2d.h"
#include <gif_tags.h>

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
#ifndef SHADOW_SIL
#define SHADOW_SIL 1
#endif
/* SCISSOR_1 / XYOFFSET_1 words (GS manual: SCAX0 0-10, SCAX1 16-26, SCAY0 32-42, SCAY1 48-58; OFX 0-15, OFY 32-47) */
#define SIL_SCISSOR(x0, x1, y0, y1) ((u64)(x0) | (u64)(x1) << 16 | (u64)(y0) << 32 | (u64)(y1) << 48)
#define SIL_XYOFFSET(x, y) ((u64)(x) | (u64)(y) << 32)
#define SIL_W 128              /* target (CT16, TBW 2 as render.c's TEX0 sets it) */
#define SIL_H 64
#define SIL_DIST 40.0f         /* sun camera distance from the player's centre (near-orthographic) */
#define SIL_HALF_X 1.4f        /* metres across the target's half width (along the player's height) */
#define SIL_HALF_Y 1.1f        /* metres across its half height */
#define SIL_CENTRE 0.9f        /* the camera aims this far above the feet */
#define SIL_MIN_SUN 0.35f      /* sun direction's y below this: blob instead */
#define SIL_A 80.0f            /* alpha (0x80 = 1.0) of the silhouette's darkening */

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

static GSGLOBAL *sgs;
static u32 sil_vram, sil_z;
static int sil_on;                /* the target holds this frame's silhouette */
static V3 sil_cam[4];             /* eye, right, up, fwd of the sun camera */
static float sil_floor[4], sil_a; /* floor plane (n, n . point), alpha */

void shadow_init(GSGLOBAL *gs)
{
    sgs = gs;
    if (!SHADOW_SIL || !SHADOW_BLOB)
        return;
    u32 bytes = gsKit_texture_size(SIL_W, SIL_H, GS_PSM_CT16);  /* (8 KB pages: FRAME / ZBUF bases) */
    sil_vram = gs_vram_fixed(gs, bytes, GSKIT_ALLOC_SYSBUFFER, GSO_CHR_PLAYER, "player silhouette");
    sil_z = gs_vram_fixed(gs, bytes, GSKIT_ALLOC_SYSBUFFER, GSO_CHR_PLAYER, "player silhouette Z");
    if (sil_vram == GSKIT_ALLOC_ERROR || sil_z == GSKIT_ALLOC_ERROR)
        sil_vram = sil_z = 0;
}

int shadow_sil_begin(V3 feet)
{
    sil_on = 0;
    if (!SHADOW_SIL || !SHADOW_BLOB || !sil_vram)
        return 0;
    float amb[3], sun[3], L[3];  /* L: world, towards the sun */
    map_char_base(amb, sun, L);
    ColHit h;
    if (L[1] < SIL_MIN_SUN || !col_floor(feet.x, feet.z, feet.y + 0.5f, feet.y - SHADOW_DROP, COL_SOLID, &h) ||
        h.n[1] < 0.3f)
        return 0;
    float hgt = feet.y - h.y;
    sil_a = SIL_A * (hgt > 0 ? 1 - hgt / SHADOW_DROP : 1);
    if (sil_a < 2)
        return 0;
    for (int c = 0; c < 3; c++)
        sil_floor[c] = h.n[c];
    sil_floor[3] = h.n[0] * feet.x + h.n[1] * h.y + h.n[2] * feet.z + SHADOW_LIFT;
    /* the sun camera: fwd = -L, right = world up across the view (the player's height along the target's x) */
    V3 f = v3(-L[0], -L[1], -L[2]), c = v3(feet.x, feet.y + SIL_CENTRE, feet.z);
    float k = f.y;
    V3 r = v3norm(v3(-k * f.x, 1 - k * f.y, -k * f.z));
    V3 u = v3cross(f, r);
    sil_cam[0] = v3(c.x - f.x * SIL_DIST, c.y - f.y * SIL_DIST, c.z - f.z * SIL_DIST);
    sil_cam[1] = r, sil_cam[2] = u, sil_cam[3] = f;
    /* GS: the target (FRAME, ZBUF, SCISSOR, XYOFFSET), cleared to 0 with Z 0 (the skin batches test Z GEQUAL) */
    GSGLOBAL *gs = sgs;
    int ox = (R_SCREEN_W / 2 - SIL_W / 2) * 16 + gs->OffsetX, oy = (224 - SIL_H / 2) * 16 + gs->OffsetY;
    u64 rg[2 * 9];
    int n = 0;
#define REG(v, a) (rg[2 * n] = (v), rg[2 * n + 1] = (a), n++)
    REG(gs_frame(sil_vram, SIL_W, GS_PSM_CT16, 0), GSR_FRAME);
    REG(GS_SETREG_ZBUF(sil_z / 8192, GS_PSMZ_16S, 0), GSR_ZBUF);
    REG(SIL_SCISSOR(0, SIL_W - 1, 0, SIL_H - 1), GSR_SCISSOR);
    REG(SIL_XYOFFSET(ox, oy), GSR_XYOFFSET);
    REG(GS_TEST_ZALWAYS, GSR_TEST);
    REG(GS_PRIM_SPRITE_FLAT, GSR_PRIM);
    REG(GS_SETREG_RGBAQ(0, 0, 0, 0, 0), GSR_RGBAQ);
    REG(GS_SETREG_XYZ2(ox, oy, 0), GSR_XYZ2);
    REG(GS_SETREG_XYZ2(ox + (SIL_W << 4), oy + (SIL_H << 4), 0), GSR_XYZ2);
#undef REG
    vu1_gs_regs(n, rg);
    r_camera_override(sil_cam, (SIL_W / 2) * SIL_DIST / SIL_HALF_X, (SIL_H / 2) * SIL_DIST / SIL_HALF_Y);
    return 1;
}

void shadow_sil_end(void)
{
    GSGLOBAL *gs = sgs;
    r_camera_override(NULL, 0, 0);
    u64 rg[2 * 4];
    int n = 0;
#define REG(v, a) (rg[2 * n] = (v), rg[2 * n + 1] = (a), n++)
    REG(gs_frame(gs->ScreenBuffer[gs->ActiveBuffer & 1], gs->Width, gs->PSM, 0), GSR_FRAME);
    REG(gs_zbuf(gs, 0), GSR_ZBUF);
    REG(SIL_SCISSOR(0, gs->Width - 1, 0, gs->Height - 1), GSR_SCISSOR);
    REG(SIL_XYOFFSET(gs->OffsetX, gs->OffsetY), GSR_XYOFFSET);
#undef REG
    vu1_gs_regs(n, rg);
    sil_on = 1;
}

/* the silhouette on the floor: the target's corners cast along the sun onto the floor plane */
static void sil_draw(void)
{
    const V3 *c = sil_cam;
    const float *pl = sil_floor;
    float fx = (SIL_W / 2) * SIL_DIST / SIL_HALF_X, fy = (SIL_H / 2) * SIL_DIST / SIL_HALF_Y;
    static const float px[4][2] = {{0, 0}, {SIL_W, 0}, {SIL_W, SIL_H}, {0, SIL_H}};
    ClipVert q[4];
    for (int k = 0; k < 4; k++) {
        float xv = (px[k][0] - SIL_W / 2) * SIL_DIST / fx, yv = -(px[k][1] - SIL_H / 2) * SIL_DIST / fy;
        V3 d = v3(c[1].x * xv + c[2].x * yv + c[3].x * SIL_DIST, c[1].y * xv + c[2].y * yv + c[3].y * SIL_DIST,
                  c[1].z * xv + c[2].z * yv + c[3].z * SIL_DIST);
        float dn = pl[0] * d.x + pl[1] * d.y + pl[2] * d.z;
        if (dn > -1e-3f)
            return;
        float t = (pl[3] - (pl[0] * c[0].x + pl[1] * c[0].y + pl[2] * c[0].z)) / dn;
        V3 v = r_view(v3(c[0].x + d.x * t, c[0].y + d.y * t, c[0].z + d.z * t));
        ClipVert cv0 = {v.x, v.y, v.z, px[k][0], px[k][1], 0, 0, 0, sil_a};
        q[k] = cv0;
    }
    static GSTEXTURE tex;
    tex.Width = SIL_W, tex.Height = SIL_H, tex.PSM = GS_PSM_CT16, tex.Vram = sil_vram, tex.VramClut = 0;
    tex.ClutPSM = 0;
    u64 *p = g2d_raw(3);  /* the target was just drawn: flush the texture cache; its alpha bit -> 0 / 0x80 */
    if (!p)
        return;
    p[0] = GIF_SET_TAG(2, 1, 0, 0, 0, 1), p[1] = 0xE;
    p[2] = 0, p[3] = GSR_TEXFLUSH;
    p[4] = GS_TEXA_BIT, p[5] = GSR_TEXA;
    r_tri_clip(&tex, &q[0], &q[1], &q[2], 0, R_BLEND);
    r_tri_clip(&tex, &q[0], &q[2], &q[3], 0, R_BLEND);
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
    if (sil_on)
        sil_draw(), shadow_drawn++, sil_on = 0;
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
