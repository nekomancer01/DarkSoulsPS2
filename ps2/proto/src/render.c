#include <math.h>
#include <stdio.h>
#include <string.h>
#include "render.h"
#include <gsInline.h>
#include <gif_tags.h>

#define SCREEN_W ((float)R_SCREEN_W)
#define SCREEN_H ((float)R_SCREEN_H)
#define NEAR R_NEAR
/* LockCamParam row 0: camFovY 43 degrees. W x 448 is shown 4:3, so a pixel is (W / 448) / (4 / 3) times wider than
 * tall: FOCAL_Y = 224 / tan(21.5 deg), FOCAL_X = FOCAL_Y * (W / 448) / (4 / 3) (609 at 640, 487.2 at 512). */
#ifndef PANO_ZOOM
#define PANO_ZOOM 1.0f  /* tools/pano.py capture builds only (-DPANO_ZOOM=2.0f): narrower view, more texels per degree */
#endif
#define FOCAL_Y (568.4f * PANO_ZOOM)
#define FOCAL_X (609.0f * R_SCREEN_W / 640.0f * PANO_ZOOM)
#define FOCAL FOCAL_X  /* guard-band tests use the larger one, which is the stricter limit on both axes */
#define ZMAX 65535.0f  /* 16-bit Z (PSMZ16S, matching the CT16 colour buffer); larger = nearer, tested GEQUAL */

static GSGLOBAL *gs;
/* The floor is drawn first at the farthest depth, so nothing on it can lose the depth test against it: with real
 * depths, large near-clipped floor tiles intermittently hid the character's lower body. */
static int flat_z;
static V3 cam_eye, cam_right, cam_up, cam_fwd;
static const V3 LIGHT = {0.4f, 0.8f, -0.45f};

M3 m3_rot_y(float a) { float c = cosf(a), s = sinf(a); M3 r = {{c, 0, s, 0, 1, 0, -s, 0, c}}; return r; }

V3 m3_apply(M3 m, V3 v)
{
    return v3(m.m[0] * v.x + m.m[1] * v.y + m.m[2] * v.z, m.m[3] * v.x + m.m[4] * v.y + m.m[5] * v.z,
              m.m[6] * v.x + m.m[7] * v.y + m.m[8] * v.z);
}

void r_init(GSGLOBAL *g) { gs = g; }

/* --- V6 VRAM allocator: one bit per 256-byte block that this allocator owns and has not handed out --- */
static u32 vfree[4 * 1024 * 1024 / 256 / 32];
int r_vram_used, r_vram_pages, r_vram_gskit, r_vram_holes;

/* block number inside a page: PSMCT32 / PSMT8 (8 x 4 blocks), PSMCT16 (4 x 8 blocks) */
static const unsigned char BLK32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
                                          {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const unsigned char BLK16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
                                          {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};

static int vbit(int b) { return vfree[b >> 5] >> (b & 31) & 1; }

/* footprint of a w x h image in buffer width tbw (64-pixel units): block offsets from its base pointer */
static int footprint(int w, int h, int psm, int tbw, unsigned short *out)
{
    int bw, bh, pw, ph, ct16 = psm == GS_PSM_CT16;
    if (psm == GS_PSM_T8)
        bw = 16, bh = 16, pw = 128, ph = 64;
    else if (ct16)
        bw = 16, bh = 8, pw = 64, ph = 64;
    else
        bw = 8, bh = 8, pw = 64, ph = 32;
    int ppr = tbw * 64 / pw, n = 0;
    if (ppr < 1)
        ppr = 1;
    for (int y = 0; y < h; y += bh)
        for (int x = 0; x < w; x += bw) {
            int page = (y / ph) * ppr + x / pw, bx = (x % pw) / bw, by = (y % ph) / bh;
            out[n++] = page * 32 + (ct16 ? BLK16[by][bx] : BLK32[by][bx]);
        }
    return n;
}

static u32 vram_place(GSGLOBAL *g, const unsigned short *fp, int n)
{
    int span = 0;
    for (int i = 0; i < n; i++)
        span = fp[i] + 1 > span ? fp[i] + 1 : span;
    int top = g->CurrentPointer / 256;
    /* small allocations (CLUTs) first go into pages already in use, so freed textures leave whole pages for the
     * next texture (enemy / block textures come and go: TECH_DEBT world-4) */
    for (int pass = span <= 4 ? 0 : 1; pass < 2; pass++)
    for (int base = 0; base + span <= top; base++) {
        if (!vfree[base >> 5] || (!pass && vfree[base >> 5] == ~0u)) {  /* footprints hold block 0: skip the page */
            base |= 31;
            continue;
        }
        if (!pass && (base & 31) + span > 32)
            continue;
        int i = 0;
        while (i < n && vbit(base + fp[i]))
            i++;
        if (i == n) {
            for (i = 0; i < n; i++)
                vfree[(base + fp[i]) >> 5] &= ~(1u << ((base + fp[i]) & 31));
            r_vram_used += n;
            r_vram_holes -= n;
            return base * 256;
        }
    }
    /* new pages at the end of gsKit's VRAM, plus the blocks between its pointer and the page boundary (gsKit's
     * texture sizes already cover their footprints, so those are unused) */
    int base = (g->CurrentPointer + 8191) / 8192 * 32, pages = (span + 31) / 32;
    if ((base + pages * 32) * 256 > 4 * 1024 * 1024) {
        printf("ERROR: Not enough VRAM for %d blocks (r_vram)\n", n);
        return GSKIT_ALLOC_ERROR;
    }
    for (int b = (g->CurrentPointer + 255) / 256; b < base + pages * 32; b++)
        vfree[b >> 5] |= 1u << (b & 31), r_vram_holes++;
    g->CurrentPointer = (base + pages * 32) * 256;
    r_vram_pages += pages;
    return vram_place(g, fp, n);
}

static int tex_tbw(int w, int psm) { return psm == GS_PSM_T8 ? (w + 127) / 128 * 2 : (w + 63) / 64; }  /* vu1.c / gsKit_setup_tbw */

/* the blocks of an allocation go back to the free set (first fit reuses them) */
static void vram_release(u32 addr, const unsigned short *fp, int n)
{
    int base = addr / 256;
    for (int i = 0; i < n; i++)
        if (!vbit(base + fp[i]))
            vfree[(base + fp[i]) >> 5] |= 1u << ((base + fp[i]) & 31), r_vram_used--, r_vram_holes++;
}

u32 r_vram_tex(GSGLOBAL *g, int w, int h, int psm)
{
#ifdef V6_OFF  /* A/B measurement builds: gsKit's allocator as before */
    return gsKit_vram_alloc(g, gsKit_texture_size(w, h, psm), GSKIT_ALLOC_USERBUFFER);
#endif
    static unsigned short fp[4096];
    int tbw = tex_tbw(w, psm);
    r_vram_gskit += gsKit_texture_size(w, h, psm) / 256;
    if ((w + 15) / 16 * ((h + 15) / 16) > (int)(sizeof fp / sizeof *fp))
        return GSKIT_ALLOC_ERROR;
    return vram_place(g, fp, footprint(w, h, psm, tbw, fp));
}

u32 r_vram_clut(GSGLOBAL *g, int clut_psm)
{
#ifdef V6_OFF
    return gsKit_vram_alloc(g, gsKit_texture_size(16, 16, clut_psm), GSKIT_ALLOC_USERBUFFER);
#endif
    unsigned short fp[4];
    r_vram_gskit += gsKit_texture_size(16, 16, clut_psm) / 256;
    return vram_place(g, fp, footprint(16, 16, clut_psm, 1, fp));  /* 256 entries, CSM1: 16 x 16 */
}

u32 r_vram_lin(GSGLOBAL *g, int n)
{
    static unsigned short fp[1024];
    if (n <= 0 || n > (int)(sizeof fp / sizeof *fp))
        return GSKIT_ALLOC_ERROR;
    for (int i = 0; i < n; i++)
        fp[i] = i;
    return vram_place(g, fp, n);
}

void r_vram_free_tex(u32 addr, int w, int h, int psm)
{
#ifdef V6_OFF
    return;
#endif
    static unsigned short fp[4096];
    if (addr == GSKIT_ALLOC_ERROR || (w + 15) / 16 * ((h + 15) / 16) > (int)(sizeof fp / sizeof *fp))
        return;
    vram_release(addr, fp, footprint(w, h, psm, tex_tbw(w, psm), fp));
}

void r_vram_free_clut(u32 addr, int clut_psm)
{
#ifdef V6_OFF
    return;
#endif
    unsigned short fp[4];
    if (addr != GSKIT_ALLOC_ERROR)
        vram_release(addr, fp, footprint(16, 16, clut_psm, 1, fp));
}

void r_vram_free_lin(u32 addr, int n)
{
    for (int i = 0; addr != GSKIT_ALLOC_ERROR && i < n; i++) {
        unsigned short b = i;
        vram_release(addr + i * 256, &b, 1);
    }
}

int r_vram_avail(const GSGLOBAL *g) { return 4 * 1024 * 1024 - (int)g->CurrentPointer + r_vram_holes * 256; }

void r_camera(V3 eye, V3 target)
{
    cam_eye = eye;
    cam_fwd = v3norm(v3sub(target, eye));
    cam_right = v3norm(v3cross(v3(0, 1, 0), cam_fwd));
    cam_up = v3cross(cam_fwd, cam_right);
}

void r_camera_basis(V3 *eye, V3 *right, V3 *up, V3 *fwd, float *focal, float *focal_y, float *zscale)
{
    *eye = cam_eye, *right = cam_right, *up = cam_up, *fwd = cam_fwd;
    *focal = FOCAL_X;
    *focal_y = FOCAL_Y;
    *zscale = ZMAX * NEAR;
}

static V3 to_view(V3 p)
{
    V3 d = v3sub(p, cam_eye);
    return v3(v3dot(d, cam_right), v3dot(d, cam_up), v3dot(d, cam_fwd));
}

static void project(V3 v, float *sx, float *sy, int *sz)
{
    *sx = SCREEN_W / 2 + FOCAL_X * v.x / v.z;
    *sy = SCREEN_H / 2 - FOCAL_Y * v.y / v.z;
    *sz = flat_z ? 1 : (int)(ZMAX * NEAR / v.z);
}

static V3 clip_edge(V3 a, V3 b)
{
    float t = (NEAR - a.z) / (b.z - a.z);
    return v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, NEAR);
}

/* View-space triangle clipped against the near plane: 0, 1 or 2 triangles. Drawn with the gouraud call: this gsKit
 * build's flat gsKit_prim_triangle_3d left only vertex dots after the first triangle. */
static void tri_view(V3 a, V3 b, V3 c, u64 col)
{
    V3 in[3] = {a, b, c}, out[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        V3 p = in[i], q = in[(i + 1) % 3];
        int pin = p.z >= NEAR, qin = q.z >= NEAR;
        if (pin)
            out[n++] = p;
        if (pin != qin)
            out[n++] = clip_edge(p, q);
    }
    float x[4], y[4];
    int z[4];
    for (int i = 0; i < n; i++)
        project(out[i], &x[i], &y[i], &z[i]);
    if (n >= 3)
        gsKit_prim_triangle_gouraud_3d(gs, x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2], col, col, col);
    if (n == 4)
        gsKit_prim_triangle_gouraud_3d(gs, x[0], y[0], z[0], x[2], y[2], z[2], x[3], y[3], z[3], col, col, col);
}

static u64 shade(V3 n, int r, int g, int b)
{
    float l = v3dot(n, v3norm(LIGHT));
    float k = 0.45f + 0.55f * (l > 0 ? l : 0);
    return GS_SETREG_RGBAQ((int)(r * k), (int)(g * k), (int)(b * k), 0x80, 0);
}

void r_box(V3 pos, M3 rot, V3 c, V3 h, int r, int g, int b)
{
    static const signed char corner[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                              {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    static const unsigned char face[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 4, 7, 3},
                                             {1, 2, 6, 5}, {3, 7, 6, 2}, {0, 1, 5, 4}};
    static const signed char fn[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    V3 w[8], v[8];
    for (int i = 0; i < 8; i++) {
        V3 l = v3(c.x + h.x * corner[i][0], c.y + h.y * corner[i][1], c.z + h.z * corner[i][2]);
        w[i] = v3add(pos, m3_apply(rot, l));
        v[i] = to_view(w[i]);
    }
    for (int f = 0; f < 6; f++) {
        V3 n = m3_apply(rot, v3(fn[f][0], fn[f][1], fn[f][2]));
        if (v3dot(n, v3sub(w[face[f][0]], cam_eye)) >= 0)
            continue;  /* back face */
        u64 col = shade(n, r, g, b);
        tri_view(v[face[f][0]], v[face[f][1]], v[face[f][2]], col);
        tri_view(v[face[f][0]], v[face[f][2]], v[face[f][3]], col);
    }
}

void r_line(V3 a, V3 b, int r, int g, int bl)
{
    V3 va = to_view(a), vb = to_view(b);
    if (va.z < NEAR && vb.z < NEAR)
        return;
    if (va.z < NEAR)
        va = clip_edge(va, vb);
    else if (vb.z < NEAR)
        vb = clip_edge(vb, va);
    float x1, y1, x2, y2;
    int z1, z2;
    project(va, &x1, &y1, &z1);
    project(vb, &x2, &y2, &z2);
    gsKit_prim_line_3d(gs, x1, y1, z1, x2, y2, z2, GS_SETREG_RGBAQ(r, g, bl, 0x80, 0));
}

/* White plane with a light grid every `cell` metres, centred on the origin */
#define GUARD R_GUARD

V3 r_view(V3 p) { return to_view(p); }

int r_point_inside(const V3 *v)
{
    float lim = GUARD / FOCAL * v->z;
    return v->z >= NEAR && fabsf(v->x) <= lim && fabsf(v->y) <= lim;
}

void r_view_to_screen(const V3 *v, float out[3])
{
    int z;
    project(*v, &out[0], &out[1], &z);
    out[2] = (float)z;
}

static ClipVert lerp_cv(const ClipVert *a, const ClipVert *b, float t)
{
    ClipVert r;
    const float *pa = &a->x, *pb = &b->x;
    float *pr = &r.x;
    for (int i = 0; i < 9; i++)
        pr[i] = pa[i] + (pb[i] - pa[i]) * t;
    return r;
}

/* plane value >= 0 means inside: 0 near, 1-4 guard band sides */
static float plane(const ClipVert *v, int pl)
{
    float lim = GUARD / FOCAL * v->z;
    switch (pl) {
    case 0: return v->z - NEAR;
    case 1: return lim - v->x;
    case 2: return lim + v->x;
    case 3: return lim - v->y;
    default: return lim + v->y;
    }
}

/* fog for the EE path, same terms as the VU1 map program; FOGCOL is restored to fog_base after each packet */
static float fog_a, fog_b = 255, fog_min = 255, fog_max = 255, fog_edge;
static u64 fog_col, fog_base;

void r_set_fog_edge(float a2)
{
    fog_edge = a2;
}

void r_get_fog(float f[5], u64 *rgb, u64 *base_rgb)
{
    f[0] = fog_a, f[1] = fog_b, f[2] = fog_min, f[3] = fog_max, f[4] = fog_edge;
    *rgb = fog_col, *base_rgb = fog_base;
}

void r_set_fog(float a, float b, float fmin, float fmax, u32 rgb, u32 base_rgb)
{
    fog_a = a, fog_b = b, fog_min = fmin, fog_max = fmax;
    fog_col = rgb, fog_base = base_rgb;
}

/* Triangle list as one GS packet: A+D (TEX0, TEX1, TEST, ALPHA, ZBUF, FOGCOL), the triangles in PACKED
 * ST / RGBAQ / XYZF2 (perspective-correct STQ, fog factor per vertex), A+D restoring ALPHA, ZBUF and FOGCOL for gsKit.
 * gsKit's UV primitives are affine and their 14.4 fixed-point UV register wraps above 1024 texels.
 * t == NULL: untextured gouraud (TEX0 not sent, TEX1 twice keeps the register count), fogged the same way. */
static void tri_list(GSTEXTURE *t, int mode, int nv, const ClipVert *v, const float (*s)[3])
{
    int nq = 1 + 6 + 1 + 3 * nv + 1 + 3;
    u64 *p = gsKit_heap_alloc(gs, nq - 1, (nq - 1) * 16, GIF_AD);
    u32 tw = 0, th = 0;
    while (t && (1u << tw) < t->Width)
        tw++;
    while (t && (1u << th) < t->Height)
        th++;
    u64 zbuf = GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0);
    *p++ = GIF_SET_TAG(6, 0, 0, 0, 0, 1), *p++ = 0xE;
    if (t)
        *p++ = GS_SETREG_TEX0(t->Vram / 256, (t->Width + 127) / 128 * 2, t->PSM, tw, th, 1, 0, t->VramClut / 256,
                              t->ClutPSM, 0, 0, 1), *p++ = 0x06;
    else
        *p++ = GS_SETREG_TEX1(0, 0, 1, 1, 0, 0, 0), *p++ = 0x14;
    *p++ = GS_SETREG_TEX1(0, 0, 1, 1, 0, 0, 0), *p++ = 0x14;
    *p++ = mode & R_BLEND ? GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 2) : GS_SETREG_TEST(1, 5, 0x40, 0, 0, 0, 1, 2), *p++ = 0x47;
    *p++ = mode & R_ADD ? GS_SETREG_ALPHA(0, 2, 0, 1, 0) : GS_SETREG_ALPHA(0, 1, 0, 1, 0), *p++ = 0x42;
    *p++ = mode & R_BLEND ? zbuf | (1ull << 32) : zbuf, *p++ = 0x4E;  /* ZMSK: blended triangles do not write Z */
    *p++ = fog_col, *p++ = 0x3D;
    int prim = (t ? 0x1B : 0x0B) | (mode & R_FOG ? 0x20 : 0) | (mode & R_BLEND ? 0x40 : 0);
    *p++ = GIF_SET_TAG(nv, 0, 1, prim, 0, 3), *p++ = 0x412;
    for (int i = 0; i < nv; i++) {
        float q = 1.0f / v[i].z;
        float f = fog_a * v[i].z + fog_b;
        f = f < fog_min ? fog_min : f > fog_max ? fog_max : f;
        float fe = fog_b + fog_edge * v[i].z;  /* edge fog, as vu1map2's edge loop */
        f = f < fe ? f : fe > 0 ? fe : 0;
        float st[2] = {t ? v[i].u / t->Width * q : 0, t ? v[i].v / t->Height * q : 0};
        u32 w[2];
        memcpy(w, st, 8);
        *p++ = (u64)w[0] | ((u64)w[1] << 32);
        memcpy(w, &q, 4);
        *p++ = w[0];
        *p++ = (u64)(int)v[i].r | ((u64)(int)v[i].g << 32);
        *p++ = (u64)(int)v[i].b | ((u64)(int)v[i].a << 32);
        *p++ = (u64)gsKit_float_to_int_x(gs, s[i][0]) | ((u64)gsKit_float_to_int_y(gs, s[i][1]) << 32);
        *p++ = ((u64)(u32)s[i][2] << 4) | ((u64)((int)f << 4) << 32);
    }
    *p++ = GIF_SET_TAG(3, 1, 0, 0, 0, 1), *p++ = 0xE;
    *p++ = gs->PrimAlpha, *p++ = 0x42;
    *p++ = zbuf, *p++ = 0x4E;
    *p++ = fog_base, *p++ = 0x3D;
}

void r_tri_list(GSTEXTURE *t, int mode, int nv, const ClipVert *v, const float (*s)[3])
{
    tri_list(t, mode, nv, v, s);
}

void r_tri_clip(GSTEXTURE *t, const ClipVert *a, const ClipVert *b, const ClipVert *c, int cull, int mode)
{
    ClipVert buf[2][12];
    int n = 3, cur = 0;
    buf[0][0] = *a, buf[0][1] = *b, buf[0][2] = *c;
    for (int pl = 0; pl < 5 && n >= 3; pl++) {
        ClipVert *in = buf[cur], *out = buf[cur ^ 1];
        int m = 0;
        for (int i = 0; i < n; i++) {
            ClipVert *p = &in[i], *q = &in[(i + 1) % n];
            float dp = plane(p, pl), dq = plane(q, pl);
            if (dp >= 0)
                out[m++] = *p;
            if ((dp >= 0) != (dq >= 0) && m < 12)
                out[m++] = lerp_cv(p, q, dp / (dp - dq));
        }
        n = m;
        cur ^= 1;
    }
    if (n < 3)
        return;
    ClipVert *v = buf[cur];
    float s[12][3];
    for (int i = 0; i < n; i++) {
        int z;
        project(v3(v[i].x, v[i].y, v[i].z), &s[i][0], &s[i][1], &z);
        s[i][2] = (float)z;
    }
    ClipVert lv[30];
    float ls[30][3];
    int nl = 0;
    for (int i = 1; i + 1 < n; i++) {
        if (cull && r_backface(s[0], s[i], s[i + 1]))
            continue;
        const int k[3] = {0, i, i + 1};
        for (int j = 0; j < 3; j++, nl++) {
            lv[nl] = v[k[j]];
            memcpy(ls[nl], s[k[j]], sizeof ls[nl]);
        }
    }
    if (nl)
        tri_list(t, mode, nl, lv, (const float (*)[3])ls);
}

/* Bounding-sphere frustum test: one view transform and four plane distances per chunk */
int r_box_classify(const float *mn, const float *mx)
{
    V3 c = v3((mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f);
    float dx = mx[0] - c.x, dy = mx[1] - c.y, dz = mx[2] - c.z, r = sqrtf(dx * dx + dy * dy + dz * dz);
    V3 v = to_view(c);
    if (v.z < NEAR - r)
        return 0;
    /* side planes |x| = t z with t = tan(half fov) (+40 px margin); signed distance = (|x| - t z) / sqrt(1 + t^2) */
    const float tx = (SCREEN_W / 2 + 40) / FOCAL_X, ty = (SCREEN_H / 2 + 40) / FOCAL_Y;
    const float kx = 1.0f / sqrtf(1 + tx * tx), ky = 1.0f / sqrtf(1 + ty * ty);
    if ((fabsf(v.x) - tx * v.z) * kx > r || (fabsf(v.y) - ty * v.z) * ky > r)
        return 0;
    /* VU1-safe: the whole sphere in front of the near plane and inside the guard band */
    const float tg = GUARD / FOCAL, kg = 1.0f / sqrtf(1 + tg * tg);
    if (v.z - r >= NEAR && (fabsf(v.x) - tg * v.z) * kg < -r && (fabsf(v.y) - tg * v.z) * kg < -r)
        return 1;
    return 2;
}

void r_ground(float size, float cell)
{
    u64 white = GS_SETREG_RGBAQ(0xF0, 0xF0, 0xF0, 0x80, 0);
    flat_z = 1;
    int n = 8;  /* split into tiles so near-plane clipping stays cheap and precise */
    float t = 2 * size / n;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            float x0 = -size + i * t, z0 = -size + j * t;
            V3 a = to_view(v3(x0, 0, z0)), b = to_view(v3(x0 + t, 0, z0)), c = to_view(v3(x0 + t, 0, z0 + t)),
               d = to_view(v3(x0, 0, z0 + t));
            tri_view(a, b, c, white);
            tri_view(a, c, d, white);
        }
    for (float k = -size; k <= size + 0.01f; k += cell) {
        r_line(v3(k, 0, -size), v3(k, 0, size), 0xC8, 0xC8, 0xC8);
        r_line(v3(-size, 0, k), v3(size, 0, k), 0xC8, 0xC8, 0xC8);
    }
    flat_z = 0;
}

void r_rect(float x, float y, float w, float h, int r, int g, int b)
{
    gsKit_prim_sprite(gs, R_HX(x), y, R_HX(x + w), y + h, (int)ZMAX, GS_SETREG_RGBAQ(r, g, b, 0x80, 0));
}

/* 3x5 pixel font: each glyph is 5 rows of 3 bits */
static const unsigned short *glyph(char c)
{
    static const unsigned short font[][5] = {
        /* 0-9 */
        {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
        {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
        /* A-Z */
        {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {3, 4, 4, 4, 3}, {6, 5, 5, 5, 6}, {7, 4, 6, 4, 7},
        {7, 4, 6, 4, 4}, {3, 4, 5, 5, 3}, {5, 5, 7, 5, 5}, {7, 2, 2, 2, 7}, {1, 1, 1, 5, 2},
        {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7}, {5, 7, 7, 5, 5}, {6, 5, 5, 5, 5}, {2, 5, 5, 5, 2},
        {6, 5, 6, 4, 4}, {2, 5, 5, 6, 3}, {6, 5, 6, 5, 5}, {3, 4, 2, 1, 6}, {7, 2, 2, 2, 2},
        {5, 5, 5, 5, 7}, {5, 5, 5, 5, 2}, {5, 5, 7, 7, 5}, {5, 5, 2, 5, 5}, {5, 5, 2, 2, 2},
        {7, 1, 2, 4, 7},
        /* symbols: . : / - ( ) % _ */
        {0, 0, 0, 0, 2}, {0, 2, 0, 2, 0}, {1, 1, 2, 4, 4}, {0, 0, 7, 0, 0}, {1, 2, 2, 2, 1},
        {4, 2, 2, 2, 4}, {5, 1, 2, 4, 5}, {0, 0, 0, 0, 7}, {0, 0, 0, 0, 0}};
    static const char sym[] = ".:/-()%_";
    if (c >= '0' && c <= '9')
        return font[c - '0'];
    if (c >= 'a' && c <= 'z')
        c -= 32;
    if (c >= 'A' && c <= 'Z')
        return font[10 + c - 'A'];
    const char *p = strchr(sym, c);
    return font[p && c ? 36 + (p - sym) : 44];
}

void r_text(float x, float y, float s, const char *str, int r, int g, int b)
{
    for (; *str; str++, x += 4 * s) {
        const unsigned short *gl = glyph(*str);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (gl[row] & (4 >> col))
                    r_rect(x + col * s, y + row * s, s, s, r, g, b);
    }
}

void r_text_outlined(float x, float y, float s, const char *str)
{
    /* one dark box behind the string: a per-pixel outline cost thousands of sprites per frame */
    r_rect(x - s, y - s, (4 * strlen(str) + 1) * s, 7 * s, 0x18, 0x18, 0x18);
    r_text(x, y, s, str, 0xF0, 0xF0, 0xE0);
}

unsigned r_gsq_peak;
void r_queue_exec(GSGLOBAL *gs)
{
    GSQUEUE *q = gs->Os_Queue;
    unsigned n = (char *)q->pool_cur - (char *)q->pool[q->dbuf];
    if (n > r_gsq_peak) {
        r_gsq_peak = n;
        if (n > R_GSQ_OS * 3 / 4)
            printf("TEST error: gsKit queue %u of %u bytes (R_GSQ_OS)\n", n, (unsigned)R_GSQ_OS);
    }
    gsKit_queue_exec(gs);
}
