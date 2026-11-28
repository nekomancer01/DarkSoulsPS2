/* Map chunk drawing (mapchunk.h): one chunk or horizon chunk into the chain (texture, mip chain, edge fog, water
 * colours, the relief / player light / specular collection), the ZSPLIT far / near passes and the GS state calls
 * that are recorded for the near pass */
#include "mapchunk.h"
#include "mapvis.h"
#include "maplight.h"
#include "maplog.h"

int mp_emit_close;  /* MAP_TWO_PASS emit: the cell's chunks need no distance / edge fog test */
/* ZSPLIT (2026-11-11): two depth ranges for the 16-bit Z buffer. Z = ZMAX near / z leaves ~200 codes for everything
 * beyond 80 m with the 0.25 m near plane (40 cm a step at 80 m, 1.4 m at 150 m: far walls and roofs z-fought). The map
 * is drawn in two passes inside its chain: the far pass first, everything whose box reaches past ZSPLIT, with the
 * near plane at ZSPLIT (vu1_map_near: Z = ZMAX ZSPLIT / z, ~1.5 mm a step at 80 m; vu1clip cuts at ZSPLIT); then Z
 * is set to the near mapping's value at ZSPLIT (zs_clear) and the near pass draws everything whose box starts nearer
 * than ZSPLIT with the normal near plane. Its pixels beyond ZSPLIT fail the GEQUAL test against that value, so a
 * box crossing ZSPLIT draws its far part in the far pass and its near part in the near pass, each pixel once.
 * Characters, breakables, NPCs and effects draw after the map with the normal mapping: they all end before ZSPLIT
 * (enemies / breakables 40 m, NPCs 30 m, FX_RANGE 40 m from the eye; past ZSPLIT they would not show).
 * The near pass is recorded while the far pass is drawn (zs_rec: the chunks, and every chain state call in order:
 * GS registers, fog rows, Z bias, edge fog, the relief pass) and replayed after zs_clear, into the idle character
 * chain buffer (vu1_scratch). ZSPLIT=0 builds: one range, as before. */
#ifndef ZSPLIT
#define ZSPLIT 50.0f
#endif
enum { ZS_CHUNK = 1, ZS_HZ, ZS_REGS, ZS_FOG, ZS_ZBIAS, ZS_EDGE, ZS_BUMP };
static int zs_pass;  /* 1: the far pass (recording the near one), 2: replaying the near pass, 0: one range */
static u32 *zs_buf;
static unsigned zs_n, zs_cap, zs_lost;  /* words used / available; near records that did not fit (this frame) */
static unsigned zs_st[4];  /* frames: chunks far only, near only, both; peak words */
static int zs_nobump;  /* draw_chunk_ for the far pass: the relief pass is the near pass's (BUMP_FAR < ZSPLIT) */

#ifdef MAP_REUSE_CHECK
/* debug build: every reused depth / distance / level is compared bit for bit with a fresh computation in the same frame */
static unsigned rc_hit, rc_hz, rc_miss, rc_bad, rc_fx, rc_zs, rc_lvl, rc_dd;
#endif
static float zs_route_zmax;  /* mp_zs_route's box depth of the chunk it routed last (farthest point) */

static int zs_room(unsigned w)
{
    if (zs_n + w <= zs_cap)
        return 1;
    zs_lost++;
    return 0;
}

void mp_zs_regs(int n, const u64 *regs)
{
    vu1_gs_regs(n, regs);
    if (zs_pass == 1 && (n <= 8 || (zs_lost++, 0)) && zs_room(2 + 4 * (unsigned)n)) {
        zs_buf[zs_n++] = ZS_REGS, zs_buf[zs_n++] = (u32)n;
        memcpy(zs_buf + zs_n, regs, 16 * (unsigned)n);
        zs_n += 4 * (unsigned)n;
    }
}

static void zs_rec_f(int op, int nf, const float *f)
{
    if (zs_pass == 1 && zs_room(1 + (unsigned)nf)) {
        zs_buf[zs_n++] = (u32)op;
        memcpy(zs_buf + zs_n, f, 4 * (unsigned)nf);
        zs_n += (unsigned)nf;
    }
}

void mp_zs_fog(float a, float b, float fmin, float fmax)
{
    vu1_map_fog(a, b, fmin, fmax);
    const float f[4] = {a, b, fmin, fmax};
    zs_rec_f(ZS_FOG, 4, f);
}

void mp_zs_zbias(float k)
{
    vu1_map_zbias(k);
    zs_rec_f(ZS_ZBIAS, 1, &k);
}

void mp_zs_edge(float a2)
{
    vu1_map_edge(a2);
    zs_rec_f(ZS_EDGE, 1, &a2);
}

/* GS registers of the opaque passes: gsKit's ALPHA, ZBUF writing Z */
void mp_opaque_regs(GSGLOBAL *gs)
{
    u64 r[4] = {gs->PrimAlpha, GSR_ALPHA, gs_zbuf(gs, 0), GSR_ZBUF};
    mp_zs_regs(2, r);
}

/* GS registers of a material class row (material.h): its ALPHA, ZBUF with ZMSK when it writes no Z. TEST comes per
 * batch from the blend bit (vu1.c: the rows' TEST words are what it writes, material.c asserts it) */
void mp_row_regs(GSGLOBAL *gs, const MatRow *row)
{
    u64 r[4] = {row->alpha, GSR_ALPHA, gs_zbuf(gs, row->zmsk), GSR_ZBUF};
    mp_zs_regs(2, r);
}

/* the first n triangles of chunk c (n < c->tris: foliage thinned by distance) */
/* no: the chunk's number in bump.bin / dg.bin (resident: its index in the chunk table; near: NEAR_NO) */
/* Water (class WATER, Polaris 2.6, TECH_DEBT map-19): DSR's [We] shader mixes the refraction tinted by g_WaterColor
 * (alpha = its opacity) with a Fresnel reflection of g_FresnelColor, F = saturate(bias + scale (1 - cos)^pow), cos =
 * the view direction's angle to the surface (water is level: the normal is up). Per vertex here, each frame, from
 * the eye's position (the exporter's water_rgba took cos 0.5 for all); colour = WaterColor (1 - F) + FresnelColor F,
 * opacity = a + (1 - a) F, linear -> sRGB, over the white texture whose CLUT entries 1-3 carry the terms
 * (mapgather.water_texture, marker 0x57). MAP_WATER=0: the fixed angle (cos 0.5), the old look. ASSUMED: no waves,
 * no reflection image (g_FresnelColor stands for it), no refraction. */
#ifndef MAP_WATER
#define MAP_WATER 1
#endif
static u32 water_col[256] __attribute__((aligned(16)));
static unsigned char water_srgb[256];

/* counters of water_colours (logged by mp_zs_begin every MAP_PASS_LOG frames, "map: water vertices"): vertices,
 * powf calls, table lookups, fallback powf (a table was wanted, none free); wl_t: ticks inside the per-vertex loop on
 * timed frames (mp_on) */
static unsigned wl_v, wl_pf, wl_tab, wl_fb, wl_t;

/* x^pw for a non-integer pw (the CLUT pow byte k[13], 1/32 steps; the Darkroot lake has 0xFF = 7.97): a table per power
 * instead of a powf per vertex (~2 us each on the EE). Entry i = powf(i / 1024, pw), x in [0, 1], linear between. At most
 * two powers a block; a third falls back to powf (counted) */
#define WATER_TAB_N 256
static struct { unsigned char key; float t[WATER_TAB_N + 1]; } water_tab[2];

static const float *water_tab_get(unsigned char key, float pw)
{
    for (int i = 0; i < 2; i++) {
        if (water_tab[i].key == key)
            return water_tab[i].t;
        if (!water_tab[i].key) {
            for (int j = 0; j <= WATER_TAB_N; j++)
                water_tab[i].t[j] = powf(j / (float)WATER_TAB_N, pw);
            water_tab[i].key = key;
            return water_tab[i].t;
        }
    }
    return NULL;
}

static inline float water_pow(const float *t, float x)
{
    x = x < 0 ? 0 : x > 1 ? 1 : x;
    float fx = x * WATER_TAB_N;
    int i = (int)fx;
    if (i >= WATER_TAB_N)
        return t[WATER_TAB_N];
    return t[i] + (t[i + 1] - t[i]) * (fx - (float)i);
}

/* the 4 output bytes of a vertex from F = bias + scale x^pw (clamped) */
static inline void water_bytes(unsigned char *o, const float *wc, const float *fc, float f)
{
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    for (int i = 0; i < 3; i++)
        o[i] = (unsigned char)((water_srgb[(int)((wc[i] * (1 - f) + fc[i] * f) * 255 + 0.5f)] * 128 + 127) / 255);
    o[3] = (unsigned char)((wc[3] + (1 - wc[3]) * f) * 128 + 0.5f);
}
#ifdef MAP_WATER_CHECK
static unsigned wk_v, wk_c, wk_d, wk_s;  /* table vertices, channels (4 each), channels that differ, largest byte step */
#endif

static const void *water_colours(const GSTEXTURE *t, const unsigned char *base, const Chunk *c, unsigned n)
{
    const unsigned char *k = t ? (const unsigned char *)t->Clut : NULL;
    unsigned nv = c->nv ? c->nv : 3 * n;
    if (!k || k[15] != 0x57 || nv > 256)
        return NULL;  /* an export from before the water terms: its colour is in the texture */
    if (!water_srgb[255])
        for (int i = 0; i < 256; i++)
            water_srgb[i] = (unsigned char)(255.0f * powf(i / 255.0f, 1 / 2.2f) + 0.5f);
    float wc[4], fc[3], bias = k[11] / 255.0f, scale = k[12] / 255.0f, pw = k[13] / 32.0f;
    for (int i = 0; i < 4; i++)
        wc[i] = k[4 + i] / 255.0f;
    for (int i = 0; i < 3; i++)
        fc[i] = k[8 + i] / 255.0f;
    int ip = (int)pw;
    const short *p = (const short *)(base + c->pos);
    float s = c->step;
    const float *tab = pw == (float)ip ? NULL : water_tab_get(k[13], pw);  /* x^pw table of this power (NULL: no slot) */
    unsigned wl0 = mp_on ? ticks() : 0;
    wl_v += nv;
    for (unsigned v = 0; v < nv; v++, p += 3) {
        float dx = mp_cam[0] - (c->org[0] + s * p[0]), dy = mp_cam[1] - (c->org[1] + s * p[1]),
              dz = mp_cam[2] - (c->org[2] + s * p[2]), l2 = dx * dx + dy * dy + dz * dz;
        float cs = MAP_WATER && l2 > 1e-6f ? fabsf(dy) / sqrtf(l2) : 0.5f, x = 1 - cs, xp = 1;
        if (pw == (float)ip)
            for (int i = 0; i < ip; i++)
                xp *= x;
        else if (tab)
            xp = water_pow(tab, x), wl_tab++;
        else
            xp = powf(x, pw), wl_pf++, wl_fb++;
        unsigned char *o = (unsigned char *)&water_col[v];
        water_bytes(o, wc, fc, bias + scale * xp);
#ifdef MAP_WATER_CHECK
        if (tab) {  /* the same vertex with powf (x clamped to [0, 1] like the table) */
            unsigned char r[4];
            float xc = x < 0 ? 0 : x > 1 ? 1 : x;
            water_bytes(r, wc, fc, bias + scale * powf(xc, pw));
            wk_v++, wk_c += 4;
            for (int i = 0; i < 4; i++) {
                unsigned d = o[i] > r[i] ? o[i] - r[i] : r[i] - o[i];
                wk_d += d != 0;
                wk_s = d > wk_s ? d : wk_s;
            }
        }
#endif
    }
    if (mp_on)
        wl_t += ticks() - wl0;
    return water_col;
}

static GSTEXTURE *late_tex;  /* map_draw_blend: the texture the map pass resolved for the chunk being drawn */

/* zm: the box's farthest view depth when the caller has it (mp_zs_route's, stored in the near record), else NULL */
static int draw_chunk_(const MapInst *m, const unsigned char *base, const Chunk *c, unsigned n, int cls, int mode,
                       unsigned no, const float *zm)
{
    GSTEXTURE *t = late_tex;
    float d2 = 0, dd = 0;  /* d2: squared distance to the box, dd its root (0 for a late texture) */
    int lvl = -1;  /* tc_level of the chunk's texture at dd (when tested at the top) */
    if (!t && c->tex >= 0 && c->tex < m->ntex) {  /* mip level from the distance to the chunk's box */
        int tx = m->tex_base + c->tex;
        for (int k = 0; k < 3; k++) {
            float o = mp_cam[k] < c->mn[k] ? c->mn[k] - mp_cam[k] : mp_cam[k] > c->mx[k] ? mp_cam[k] - c->mx[k] : 0;
            d2 += o * o;
        }
        MP_BEGIN
        dd = sqrtf(d2);
        lvl = tc_level(tx, dd);
        t = tc_get(tx, lvl);
        MP_END(MP_TEX)
        if (!t) {
            DBGW(D_TEX, n, no);
            return 0;  /* texture pool full this frame */
        }
    }
    /* chunks crossing the near plane / guard band (cls 2) go to VU1's clipping program (vu1clip.vsm, F4); sky
     * triangles (1-3 km) never cross the near plane and whatever VU1 rejects of them lies outside the view. map.bin
     * has no untextured chunks (they would be skipped). */
    if (t) {
        MP_BEGIN
        const unsigned passes = mode & R_BLEND ? 0 : mp_row(m, c)->passes;  /* the class's passes (material.h) */
        int vclip = cls == 2 && !(c->flags & CHUNK_SKY) ? VU1_CLIP : 0, edge = 0;
        if (!(c->flags & CHUNK_SKY)) {  /* reaches into the edge fog: the edge loop of vu1map2 */
            float zmax;
            if (mp_emit_close)  /* (MAP_TWO_PASS: the cell lies nearer than the edge fog's start) */
                zmax = 0;
            else if (zm) {
                zmax = *zm;
#ifdef MAP_REUSE_CHECK
                float fz;
                mp_box_depth(c, mp_cam, &fz, NULL);
                rc_zs++, rc_bad += memcmp(&fz, &zmax, sizeof fz) != 0;
#endif
            } else if (c == mp_vis_c)  /* map_draw's eye is the camera's */
                zmax = mp_vis_zmax;
            else
                mp_box_depth(c, mp_cam, &zmax, NULL);
            edge = zmax > mp_edge_z && !(c->flags & CHUNK_BACKDROP) ? VU1_EDGE : 0;  /* the backdrop: DSR's fog only */
        }
        float st_k = c->flags & CHUNK_ST1K ? 1024.0f : 4096.0f;
        if ((passes & MP_MIPS) && c->tex >= 0) {  /* GS mip chain (render-12) */
            u64 mt;
            MP_SPLIT(MP_CHAIN);
            int k, mxl = tc_mip(m->tex_base + c->tex, t, st_k, &mt, &k);
            MP_SPLIT(MP_TEX);
            if (mxl)
                vu1_mip(mxl, k, mt);
        }
        int pkt = n == c->tris ? m->pkt : 0;  /* a foliage prefix sends its streams one by one */
        int vm = (mode & R_FOG ? VU1_FOG : 0) | (mode & R_BLEND ? VU1_BLEND : 0) | vclip | edge | pkt;
        const void *colp = base + c->col;
        if (mp_row(m, c)->passes & MP_FRESNEL) {  /* water: colour and opacity per vertex (water_colours) */
            const void *wc = water_colours(t, base, c, n);
            if (wc)
                colp = wc, vm |= VU1_COLCP;
        }
        if (c->nv)  /* indexed (whole chunks only: foliage, the only prefix drawer, stays in lists) */
            vu1_add_idx(t, c->flags & CHUNK_CULL, vm, n, c->nv, base + c->pos, base + c->st, colp, base + CHUNK_IDX(c),
                        c->org, st_k);
        else
            vu1_add_ex(t, c->flags & CHUNK_CULL, vm, n, base + c->pos, base + c->st, colp, c->org, st_k);
        map_vu1_batches++;
        DBGW(D_DRAWN, 0, no);
        MP_SPLIT(MP_CHAIN);
#ifdef MAP_REUSE_CHECK
        {
            float fd = sqrtf(d2);
            rc_dd++, rc_bad += memcmp(&fd, &dd, sizeof fd) != 0;
            if (lvl >= 0 && c->tex >= 0)
                rc_lvl++, rc_bad += tc_level(m->tex_base + c->tex, fd) != lvl;
        }
#endif
        if ((passes & MP_PLAYER_LIGHT) && !zs_nobump && c->tex >= 0)  /* the player's light */
            mp_pl_add(base, c, n, vclip, m->tex_base + c->tex, lvl >= 0 ? lvl : tc_level(m->tex_base + c->tex, dd));
        if ((passes & MP_RELIEF) && !zs_nobump)  /* normal-map relief pass (bump.c: the boot
                                                                            * block, and a world block while it is full) */
            bump_add((int)(m - mp_inst), no, dd, c->flags & CHUNK_CULL, vclip | pkt, n, c->nv, base + c->pos,
                     base + c->st, base + c->col, c->nv ? base + CHUNK_IDX(c) : NULL, c->org, st_k);
        if ((passes & MP_ENV_SPEC) && !zs_nobump && !vclip && n == c->tris)  /* the specular pass (spec_draw) */
            mp_spec_add(m, base, c, dd);
        MP_END(MP_BUMP)
    }
    return n;
}

/* a horizon chunk (untextured, double-sided, its packet without st: vu1.c VU1_FLAT). The clipping program (VU1_CLIP)
 * gets a zero st stream sent on its own: with the packet's two streams alone it hung PCSX2 2 s into the Burg on the
 * first ISO run (2026-11-06; a full st stream on disc, +10 bytes a triangle, ran the route) */
static int draw_hz_(const MapInst *m, const Chunk *c, int cls, int mode, int cull, const float *zm)
{
    static unsigned char zero_st[CHUNK_HZN_TRIS * 12] __attribute__((aligned(16)));
    float zmax;
    if (zm) {  /* mp_zs_route's depth of this box */
        zmax = *zm;
#ifdef MAP_REUSE_CHECK
        float fz;
        mp_box_depth(c, mp_cam, &fz, NULL);
        rc_zs++, rc_bad += memcmp(&fz, &zmax, sizeof fz) != 0;
#endif
    } else
        mp_box_depth(c, mp_cam, &zmax, NULL);
    int edge = zmax > mp_edge_z && !(c->flags & CHUNK_BACKDROP) ? VU1_EDGE : 0;
    if (c->nv) {  /* indexed (horizon.py HZN_IDX, the default): vu1mapi, or vu1clip's indexed entry at the near plane;
                   * the st stream is the colours (read, unused with TME off) */
        if (c->tris > VU1_IDX_TRIS)
            return 0;
        vu1_add_idx(NULL, cull, VU1_FOG | VU1_FLAT | mode | (cls == 2 ? VU1_CLIP : edge), c->tris, c->nv, m->hz_base + c->pos,
                    m->hz_base + c->st, m->hz_base + c->col, m->hz_base + CHUNK_IDX(c), c->org, 4096.0f);
        map_vu1_batches++;
        return c->tris;
    }
    if (c->tris > CHUNK_HZN_TRIS)
        return 0;
    if (cls == 2)
        vu1_add_ex(NULL, cull, VU1_FOG | VU1_FLAT | mode | VU1_CLIP, c->tris, m->hz_base + c->pos, zero_st, m->hz_base + c->col,
                   c->org, 4096.0f);
    else
        vu1_add_ex(NULL, cull, VU1_FOG | VU1_FLAT | mode | edge | VU1_PKT, c->tris, m->hz_base + c->pos, m->hz_base + c->st,
                   m->hz_base + c->col, c->org, 4096.0f);
    map_vu1_batches++;
    return c->tris;
}

/* ZSPLIT routing: the far pass draws the part of a box beyond ZSPLIT now (clipped at ZSPLIT where the box starts
 * nearer), the part nearer than ZSPLIT is recorded for the near pass. cls is visible_'s, for the normal near plane. */
static int zs_route(const Chunk *c, int *far_cls, int cls, int kind)  /* kind: 0 chunk, 1 horizon chunk, 2 effect */
{
    float zmax, zmin;
    if (c == mp_vis_c) {  /* the view test measured this box this frame, same eye */
        zmin = mp_vis_zmin, zmax = mp_vis_zmax;
#ifdef MAP_REUSE_CHECK
        float fz, fn = mp_box_depth(c, mp_cam, &fz, NULL);
        rc_hit++, rc_hz += kind == 1, rc_fx += kind == 2;
        rc_bad += memcmp(&fz, &zmax, sizeof fz) != 0 || memcmp(&fn, &zmin, sizeof fn) != 0;
#endif
    } else {
        zmin = mp_box_depth(c, mp_cam, &zmax, NULL);
#ifdef MAP_REUSE_CHECK
        rc_miss++;
#endif
    }
    (void)kind;
    zs_route_zmax = zmax;
    int r = (zmax > ZSPLIT) | (zmin < ZSPLIT) << 1;
    *far_cls = cls == 1 && zmin >= ZSPLIT * 1.001f ? 1 : 2;
    zs_st[r - 1]++;
    return r;
}

int mp_zs_route(const Chunk *c, int *far_cls, int cls) { return zs_route(c, far_cls, cls, 2); }

int mp_draw_chunk(const MapInst *m, const unsigned char *base, const Chunk *c, unsigned n, int cls, int mode,
                      unsigned no)
{
    if (zs_pass != 1 || (c->flags & CHUNK_SKY))  /* the sky (1-3 km): the far pass */
        return draw_chunk_(m, base, c, n, cls, mode, no, NULL);
    int fc, r = zs_route(c, &fc, cls, 0), dn = 0;
    const float zmax = zs_route_zmax;
    if ((r & 2) && zs_room(9)) {
        u32 *w = zs_buf + zs_n;
        w[0] = ZS_CHUNK, w[1] = (u32)(m - mp_inst), w[2] = (u32)c, w[3] = (u32)base, w[4] = n, w[5] = (u32)cls;
        w[6] = (u32)mode, w[7] = no;
        memcpy(w + 8, &zmax, 4);  /* the box's farthest depth: the replay's edge test */
        zs_n += 9, dn = (int)n;
    }
    if (r & 1) {
        zs_nobump = 1;
        dn = draw_chunk_(m, base, c, n, fc, mode, no, &zmax);
        zs_nobump = 0;
    }
    return dn;
}

int mp_draw_hz(const MapInst *m, const Chunk *c, int cls, int mode, int cull)
{
    if (zs_pass != 1)
        return draw_hz_(m, c, cls, mode, cull, NULL);
    int fc, r = zs_route(c, &fc, cls, 1), dn = 0;
    const float zmax = zs_route_zmax;
    if ((r & 2) && zs_room(7)) {
        u32 *w = zs_buf + zs_n;
        w[0] = ZS_HZ, w[1] = (u32)m, w[2] = (u32)c,  /* (m: a map instance, or the far panorama's base, mapblock.c) */ w[3] = (u32)cls, w[4] = (u32)mode, w[5] = (u32)cull;
        memcpy(w + 6, &zmax, 4);
        zs_n += 7, dn = (int)c->tris;
    }
    if (r & 1)
        dn = draw_hz_(m, c, fc, mode, cull, &zmax);
    return dn;
}

/* the near pass's Z: everything set to the near mapping's Z at ZSPLIT (colour masked), the far pass's depth is
 * done with; the near pass's pixels beyond ZSPLIT then fail GEQUAL */
static void zs_clear(void)
{
    GSGLOBAL *gs = mp_gs;
    u64 r[2 * 32];
    int n = 0;
    u64 frame = gs_frame(gs->ScreenBuffer[gs->ActiveBuffer & 1], gs->Width, gs->PSM, 0);
    u32 z = (u32)(65535.0f * R_NEAR / (ZSPLIT > 0 ? ZSPLIT : 1));  /* (ZSPLIT=0 builds never call it) */
#define REG(v, a) (r[2 * n] = (v), r[2 * n + 1] = (a), n++)
    REG(frame | 0xFFFFFFFFull << 32, GSR_FRAME);
    REG(GS_TEST_ZALWAYS, GSR_TEST);  /* ZTE, ZTST ALWAYS */
    REG(gs_zbuf(gs, 0), GSR_ZBUF);
    REG(GS_PRIM_SPRITE_FLAT, GSR_PRIM);
    n = gs_fill_strips(r, n, 30, gs->OffsetX, gs->OffsetY, gs->Width << 4, 0, gs->Height << 4, z);
    REG(frame, GSR_FRAME);
#undef REG
    vu1_gs_regs(n, r);
}

/* map_draw: the far pass from here on (recording the near pass), when there is room to record it */
void mp_zs_begin(void)
{
    int bytes = 0;
    static unsigned wfr, wnt;  /* frames, timed frames since the last "map: water vertices" line */
    if (wfr == MAP_PASS_LOG) {
        unsigned nt = wnt ? wnt : 1;
        printf("map: water vertices %u powf %u table %u fallback %u loop ms %.2f (per frame, %u timed frames)\n",
               wl_v / wfr, wl_pf / wfr, wl_tab / wfr, wl_fb / wfr, (double)((float)wl_t / 294912.0f / (float)nt), wnt);
        wfr = wnt = wl_v = wl_pf = wl_tab = wl_fb = wl_t = 0;
    }
#ifdef MAP_WATER_CHECK
    static unsigned wkf;
    if (++wkf % 600 == 0)
        printf("map: water check: vertices %u channels %u diff %u maxstep %u\n", wk_v, wk_c, wk_d, wk_s);
#endif
    wfr++, wnt += mp_on;
    zs_pass = 0;
    zs_n = zs_lost = 0;
    if (ZSPLIT <= 0 || !(zs_buf = vu1_scratch(&bytes)) || bytes < 4096)
        return;
    zs_cap = (unsigned)bytes / 4;
    zs_pass = 1;
    vu1_map_near(ZSPLIT);
}

/* map_draw: Z reset, the normal near plane, the recorded near pass */
void mp_zs_end(void)
{
    if (zs_pass != 1)
        return;
    zs_pass = 2;
    zs_clear();
    vu1_map_near(R_NEAR);
    for (unsigned i = 0; i < zs_n;) {
        const u32 *w = zs_buf + i;
        float f[4];
        switch (w[0]) {
        case ZS_CHUNK:
            memcpy(f, w + 8, 4);
            draw_chunk_(&mp_inst[w[1]], (const unsigned char *)w[3], (const Chunk *)w[2], w[4], (int)w[5], (int)w[6], w[7], f);
            i += 9;
            break;
        case ZS_HZ:
            memcpy(f, w + 6, 4);
            draw_hz_((const MapInst *)w[1], (const Chunk *)w[2], (int)w[3], (int)w[4], (int)w[5], f);
            i += 7;
            break;
        case ZS_REGS: {
            u64 r[2 * 8];
            int n = (int)w[1];
            if (n > 8)
                n = 8;
            memcpy(r, w + 2, 16 * (unsigned)n);
            vu1_gs_regs(n, r);
            i += 2 + 4 * w[1];
            break;
        }
        case ZS_FOG:
            memcpy(f, w + 1, 16);
            vu1_map_fog(f[0], f[1], f[2], f[3]);
            i += 5;
            break;
        case ZS_ZBIAS:
        case ZS_EDGE:
            memcpy(f, w + 1, 4);
            (w[0] == ZS_ZBIAS ? vu1_map_zbias : vu1_map_edge)(f[0]);
            i += 2;
            break;
        case ZS_BUMP:
            bump_draw();
            mp_pl_draw();
            mp_spec_draw();
            i += 1;
            break;
        default:
            i = zs_n;
        }
    }
    if (zs_n > zs_st[3])
        zs_st[3] = zs_n;
    if (zs_lost)
        printf("map: zsplit: %u near records did not fit (%u words)\n", zs_lost, zs_cap);
    static unsigned zs_fr;
    if (!(++zs_fr % 600)) {  /* every 600 frames: boxes drawn far only / near only / in both passes, peak record */
        printf("map: zsplit %.0f m: far %u near %u both %u, peak %u words of %u\n", (double)ZSPLIT, zs_st[0], zs_st[1],
               zs_st[2], zs_st[3], zs_cap);
        memset(zs_st, 0, sizeof zs_st);
#ifdef MAP_REUSE_CHECK
        printf("map: reuse check: hits %u (hz %u) misses %u bad %u fx %u zs %u lvl %u dd %u\n", rc_hit, rc_hz, rc_miss, rc_bad,
               rc_fx, rc_zs, rc_lvl, rc_dd);
#endif
    }
    zs_pass = 0;
}

/* the far pass's part of chunk c alone (mp_draw_effects: its near part is kept for map_draw_blend) */
int mp_draw_chunk_far(const MapInst *m, const unsigned char *base, const Chunk *c, unsigned n, int cls, int mode,
                      unsigned no)
{
    zs_nobump = 1;
    int dn = draw_chunk_(m, base, c, n, cls, mode, no, NULL);
    zs_nobump = 0;
    return dn;
}

/* mp_draw_chunk with the texture late resolved for it (NULL: resolved here) */
int mp_draw_chunk_tex(const MapInst *m, const unsigned char *base, const Chunk *c, unsigned n, int cls, int mode,
                      unsigned no, GSTEXTURE *late)
{
    late_tex = late;
    int dn = mp_draw_chunk(m, base, c, n, cls, mode, no);
    late_tex = NULL;
    return dn;
}

int mp_zs_far(void) { return zs_pass == 1; }

/* the relief, player light and specular passes over the opaque map: recorded for the near pass while in the far
 * pass (ZSPLIT), else drawn now */
void mp_zs_relief(void)
{
    if (zs_pass == 1) {  /* (ZSPLIT: in the near pass) */
        if (zs_room(1))
            zs_buf[zs_n++] = ZS_BUMP;
    } else {
        bump_draw();  /* relief over the opaque map (bump.c), before the blended effects */
        mp_pl_draw();  /* the player's light (MAP_PLIGHT) */
        mp_spec_draw();  /* the specular pass (Polaris 2.3) */
    }
}
