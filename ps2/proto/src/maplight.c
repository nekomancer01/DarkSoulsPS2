/* Map fog and light state (maplight.h): the fog row of the floor underfoot as it eases in, the edge fog, the sky's
 * fog rows, the character light terms, the player's light and the specular pass over the map */
#include "maplight.h"
#include "mapvis.h"
#include "mapchunk.h"
#include "frame.h"
#include "cvar.h"

float mp_fog_now[FOG_NOW] = {0, 0, 0, 0, 0, 0, 64, 64, 64, 96, 96, 96, 0.40f * 127, 0.80f * 127, -0.45f * 127};
int mp_fog_area;
const FogRow *mp_pfog_row;
unsigned mp_pfog_rows;
int mp_pinst;  /* the instance the player stands in (sky, fog rows) */
static V3 player_feet;
/* edge fog this frame (mp_fog_terms): slope a2 of b + a2 z and the view depth beyond which it is below the DSR fog */
float mp_edge_a2, mp_edge_z;

void mp_fog_ease(float rate)
{
    const FogRow *r = &mp_pfog_row[mp_fog_area];
    const float t[FOG_NOW] = {r->begin, r->end, r->max, r->rgb[0], r->rgb[1], r->rgb[2], r->amb[0], r->amb[1],
                              r->amb[2], r->sun[0], r->sun[1], r->sun[2], r->dir[0], r->dir[1], r->dir[2],
                              r->env_up[0], r->env_up[1], r->env_up[2], r->env_down[0], r->env_down[1],
                              r->env_down[2], r->spec_i[0], r->spec_i[1], r->spec_i[2], r->spec_dir[0],
                              r->spec_dir[1], r->spec_dir[2]};
    for (int i = 0; i < FOG_NOW; i++)
        mp_fog_now[i] += (t[i] - mp_fog_now[i]) * rate;
}

/* DSR's fog follows the collision part under the player (its MSB FogID), the part drawgrp.c dg_player found for the
 * draw groups; in the air the last one stays */
void map_set_floor(V3 feet, int k, int block, int tri)
{
    player_feet = feet;
    if (k >= 0) {
        const MapInst *m = &mp_inst[k];
        mp_pinst = k;
        if (m->fog_rows && m->fog_ntris == (unsigned)col_block_tris(block) && (unsigned)tri < m->fog_ntris &&
            m->fog_tri[tri] < m->fog_rows) {
            mp_pfog_row = m->fog_row, mp_pfog_rows = m->fog_rows;
            mp_fog_area = m->fog_tri[tri];
        }
    }
}

/* The player's light (PointLightBank row of the collision part underfoot) as DSR changes it: a new row blends in
 * linearly over PLIGHT_BLEND seconds (exe FUN_1402d3970 sets the draw-param banks' transitions to 0.5 s when the area
 * underfoot changes, .rdata 0x141615c68; FUN_1401d77e0 advances them by the frame time; FUN_1401d7820: a transition
 * back to the row it came from runs the elapsed part backwards). It switched at once before, for the characters and
 * the map pass alike (Ame's report 2026-11-17 t 72.8: "the light turns on too jarringly"). pl_cur: map rgb (0 where
 * MAP_PLIGHT's ratio gate leaves the pass off, so it fades out instead of cutting), character rgb, begin, end (m). */
#ifndef PLIGHT_BLEND
#define PLIGHT_BLEND 0.5f
#endif
static float pl_from[8], pl_to[8], pl_cur[8], pl_el, pl_dur;
static int pl_init;
static unsigned pl_t0;
#ifndef PLIGHT_RATIO
#define PLIGHT_RATIO 60  /* % of the area's character ambient the light must reach for the map pass (ASSUMED) */
#endif

static void pl_step(void)
{
    if (!mp_pfog_rows)
        return;
    const FogRow *r = &mp_pfog_row[mp_fog_area];
    int pl_max = r->pl_rgb[0] > r->pl_rgb[1] ? r->pl_rgb[0] : r->pl_rgb[1], amb_max = r->amb[0] > r->amb[1] ? r->amb[0] : r->amb[1];
    pl_max = pl_max > r->pl_rgb[2] ? pl_max : r->pl_rgb[2], amb_max = amb_max > r->amb[2] ? amb_max : r->amb[2];
    /* the map pass only where the light is a real share of the area's light (PLIGHT_RATIO): ~2-6 ms of EE a frame at
     * Firelink for a faint glow (light 49 on ambient 120), the Depths' sewers 100 on 8 */
    int gate = r->pl_end > r->pl_begin && pl_max * 100 >= amb_max * PLIGHT_RATIO;
    float t[8];
    for (int c = 0; c < 3; c++)
        t[c] = gate ? r->pl_rgb[c] : 0, t[3 + c] = r->pl_rgb[c];
    t[6] = r->pl_begin * 0.1f, t[7] = r->pl_end * 0.1f;
    unsigned now = frame_clock();  /* (by frames with option fixeddt: src/frame.h) */
    float dt = pl_init ? (now - pl_t0) / 294912000.0f : 0;
    pl_t0 = now;
    if (dt > 0.1f)
        dt = 0.1f;
    if (!pl_init) {  /* (DSR: duration -1, at once) */
        memcpy(pl_from, t, sizeof t), memcpy(pl_to, t, sizeof t), memcpy(pl_cur, t, sizeof t);
        pl_el = pl_dur = 0, pl_init = 1;
        return;
    }
    if (memcmp(t, pl_to, sizeof t)) {
        if (pl_dur > 0 && !memcmp(t, pl_from, sizeof t))  /* back to where it came from: the elapsed part backwards */
            memcpy(pl_from, pl_to, sizeof t), pl_el = pl_dur - pl_el;
        else
            memcpy(pl_from, pl_cur, sizeof t), pl_el = 0;
        memcpy(pl_to, t, sizeof t);
        pl_dur = PLIGHT_BLEND;
    }
    if (pl_dur > 0) {
        pl_el += dt;
        if (pl_el >= pl_dur)
            pl_dur = 0;
    }
    float k = pl_dur > 0 ? pl_el / pl_dur : 1;
    for (int i = 0; i < 8; i++)
        pl_cur[i] = pl_from[i] + (pl_to[i] - pl_from[i]) * k;
}

void map_char_base(float amb[3], float sun[3], float dir[3])
{
    float l = sqrtf(mp_fog_now[12] * mp_fog_now[12] + mp_fog_now[13] * mp_fog_now[13] + mp_fog_now[14] * mp_fog_now[14]);
    for (int c = 0; c < 3; c++)
        amb[c] = mp_fog_now[6 + c], sun[c] = mp_fog_now[9 + c], dir[c] = l > 0 ? mp_fog_now[12 + c] / l : c == 1;
}

int map_player_light(float feet[3], float rgb[3], float *begin, float *end)
{
    if (!mp_pfog_rows || !pl_init || !(pl_cur[7] > pl_cur[6]))  /* (pl_step: the row blended in over PLIGHT_BLEND) */
        return 0;
    feet[0] = player_feet.x, feet[1] = player_feet.y, feet[2] = player_feet.z;
    for (int c = 0; c < 3; c++)
        rgb[c] = pl_cur[3 + c];
    *begin = pl_cur[6], *end = pl_cur[7];
    return 1;
}

void map_char_light(V3 pos, float amb[3], float sun[3], float dir[3])
{
    map_char_base(amb, sun, dir);
    /* the player's own light (DSR attaches the area's PointLightBank row to the player) */
    float feet[3], rgb[3], b, e;
    if (map_player_light(feet, rgb, &b, &e)) {
        float dx = feet[0] - pos.x, dy = feet[1] - pos.y, dz = feet[2] - pos.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        float att = (e - d) / (e - b);
        if (att > 0)
            for (int c = 0; c < 3; c++)
                amb[c] += rgb[c] * (att < 1 ? att : 1);
    }
    fx_char_light(pos, amb);  /* effect lights: the bonfire's flickering point light (src/fx.c) */
}

/* The player's light on the map (MAP_PLIGHT). DSR attaches the area's
 * PointLightBank row (the collision part's MSB lantern id) to the player and its deferred point light shader
 * (FRPG_Deferred_PointLight, dsr_light.py: saturate((end - d) / (end - begin)) x colour x N.L) lights every surface
 * around them, the map included: in the Depths' sewers (ambient ~(3, 8, 6), light 1.5-5.5 m (100, 77, 33)) it is the
 * only light there is. The port baked the map without it and gave it to the characters only (map_char_light), so the
 * walls stayed black around a lit player (Ame's playtest 2026-11-16). Here the opaque map chunks within its reach are
 * drawn a second time after the relief pass: colour = the light's falloff at each vertex, texture x colour added to
 * the frame (Cs x As + Cd: cut-out texels add nothing), Z tested,
 * not written, no fog (the light ends within 8 m). The light sits PLIGHT_H above the feet (ASSUMED: DSR hangs it on
 * the character model; the characters' own lighting measures from the feet). No N.L, as map_char_light adds it to
 * the characters' ambient: per-vertex normals cost ~4 ms of EE a frame at the Firelink graveyard (the map streams have
 * no normals; corner order is no facing either, about half of the Depths' floor triangles wind downwards). */
#define PLIGHT_H 1.0f
#define PLIGHT_MAX 96
static struct {
    const unsigned char *base;
    const Chunk *c;
    unsigned short n, clip;
    short tex, level;
} pl_item[PLIGHT_MAX];
static int pl_n, pl_on;
static float pl_pos[3], pl_rgb[3], pl_b, pl_e, pl_e2, pl_inv;  /* reach, its square, 1 / (end - begin) */
static unsigned map_plight_batches;  /* batches of the last pass (debugger) */

void mp_pl_begin(void)
{
    pl_n = pl_on = 0;
    pl_step();
    if (!mp_pfog_rows || !pl_init)
        return;
    pl_b = pl_cur[6], pl_e = pl_cur[7];
    for (int c = 0; c < 3; c++)
        pl_rgb[c] = pl_cur[c];
    pl_on = pl_e > pl_b && (pl_rgb[0] >= 1 || pl_rgb[1] >= 1 || pl_rgb[2] >= 1);
    pl_e2 = pl_e * pl_e, pl_inv = pl_on ? 1.0f / (pl_e - pl_b) : 0;
    pl_pos[0] = player_feet.x, pl_pos[1] = player_feet.y + PLIGHT_H, pl_pos[2] = player_feet.z;
}

void mp_pl_add(const unsigned char *base, const Chunk *c, unsigned n, int clip, int tex, int level)
{
    if (!pl_on || pl_n >= PLIGHT_MAX || (c->flags & CHUNK_GRASS) || n != c->tris)
        return;
    float d2 = 0;
    for (int k = 0; k < 3; k++) {
        float o = pl_pos[k] < c->mn[k] ? c->mn[k] - pl_pos[k] : pl_pos[k] > c->mx[k] ? pl_pos[k] - c->mx[k] : 0;
        d2 += o * o;
    }
    if (d2 >= pl_e * pl_e)
        return;
    pl_item[pl_n].base = base, pl_item[pl_n].c = c, pl_item[pl_n].n = (unsigned short)n;
    pl_item[pl_n].clip = (unsigned short)clip, pl_item[pl_n].tex = (short)tex, pl_item[pl_n].level = (short)level;
    pl_n++;
}

/* 1 / sqrt(x) without a divide or sqrt.s (one Newton step: < 0.2 % error): the pass runs it per vertex */
static inline float pl_rsqrt(float x)
{
    union { float f; u32 u; } v = {x};
    v.u = 0x5F3759DFu - (v.u >> 1);
    return v.f * (1.5f - 0.5f * x * v.f * v.f);
}

/* the light at squared distance d2 (< pl_e2) -> u8 x4 colour, alpha 0x80 */
static inline u32 pl_col(float d2)
{
    float att = d2 > 1e-6f ? (pl_e - d2 * pl_rsqrt(d2)) * pl_inv : 1;
    if (att > 1)
        att = 1;
    u32 out = 0x80u << 24;
    for (int c = 0; c < 3; c++) {
        int v = (int)(pl_rgb[c] * att + 0.5f);
        out |= (u32)(v > 255 ? 255 : v) << (8 * c);
    }
    return out;
}

void mp_pl_draw(void)
{
    map_plight_batches = 0;
    if (!pl_n)
        return;
    static u32 col[256] __attribute__((aligned(16)));
    GSGLOBAL *gs = mp_gs;
    /* Cs x As + Cd (As = texel alpha: cut-out texels add nothing), Z tested, not written */
    u64 r[4] = {GS_ALPHA_ADD, GSR_ALPHA, gs_zbuf(gs, 1), GSR_ZBUF};
    vu1_gs_regs(2, r);
    for (int i = 0; i < pl_n; i++) {
        const Chunk *c = pl_item[i].c;
        const unsigned char *base = pl_item[i].base;
        unsigned n = pl_item[i].n, nv = c->nv;
        if (nv ? nv > 256 : n * 3 > 256)
            continue;
        GSTEXTURE *t = tc_get(pl_item[i].tex, pl_item[i].level);
        if (!t)
            continue;
        const short *ps = (const short *)(base + c->pos);
        const float *o = c->org, s = c->step;
        int lit = 0;
        unsigned nc = nv ? nv : 3 * n;
        for (unsigned j = 0; j < nc; j++) {
            const short *q = ps + 3 * j;
            float d0 = pl_pos[0] - (o[0] + s * q[0]), d1 = pl_pos[1] - (o[1] + s * q[1]), d2 = pl_pos[2] - (o[2] + s * q[2]);
            float dd = d0 * d0 + d1 * d1 + d2 * d2;
            col[j] = dd < pl_e2 ? pl_col(dd) : 0x80u << 24;
            lit |= col[j] & 0xF8F8F8;
        }
        if (!lit)
            continue;
        float st_k = c->flags & CHUNK_ST1K ? 1024.0f : 4096.0f;
        int mode = VU1_BLEND | VU1_COLCP | pl_item[i].clip;
        if (nv)
            vu1_add_idx(t, c->flags & CHUNK_CULL, mode, n, nv, ps, base + c->st, col, base + CHUNK_IDX(c), c->org, st_k);
        else
            vu1_add_ex(t, c->flags & CHUNK_CULL, mode, n, ps, base + c->st, col, c->org, st_k);
        map_plight_batches++;
    }
    u64 rr[4] = {gs->PrimAlpha, GSR_ALPHA, gs_zbuf(gs, 0), GSR_ZBUF};
    vu1_gs_regs(2, rr);
    pl_n = 0;
}

void map_char_spec(float env_up[3], float env_down[3], float spec_i[3], float spec_dir[3])
{
    float l = sqrtf(mp_fog_now[24] * mp_fog_now[24] + mp_fog_now[25] * mp_fog_now[25] + mp_fog_now[26] * mp_fog_now[26]);
    for (int c = 0; c < 3; c++) {
        env_up[c] = mp_fog_now[15 + c], env_down[c] = mp_fog_now[18 + c], spec_i[c] = mp_fog_now[21 + c];
        spec_dir[c] = l > 0 ? mp_fog_now[24 + c] / l : c == 1;
    }
}

/* The specular / environment pass (Polaris 2.3, TECH_DEBT map-23 / render-21): the opaque chunks whose texture has a
 * specular code (tools/mapspec.py: texture entry flags bits 2-3, from its _s map) drawn again by vu1mapspec.vsm within
 * SPEC_DIST, untextured, added (GS ALPHA Cs x As + Cd) over the opaque map with depth GEQUAL and no Z write: DSR's
 * probe reflection x envSpc and the LightBank specular light, by Fresnel (the program's doc). Collected while the
 * opaque map is drawn (draw_chunk_; the near pass with ZSPLIT), drawn with the relief pass. The normals are computed
 * here per chunk from its own triangles when it first comes within reach and kept in spec_nrm (no normals in map.bin:
 * a stream of them was over the world budget). MAP_SPEC=0: no pass. */
#ifndef MAP_SPEC
#define MAP_SPEC cv_MAP_SPEC  /* runtime switch, default 1 (src/cvar.h) */
#endif
#ifndef SPEC_DIST
#define SPEC_DIST 24.0f  /* m: the pass's reach (ASSUMED: DSR has none; VU1 cost ~2.3x the map program per triangle) */
#endif
#define SPEC_MAX 64      /* chunks a frame */
#define SPEC_CACHE 48    /* chunks whose normals are kept (40 KB) */
#define SPEC_NV 208      /* vertices of a chunk at most (indexed chunks up to 255: the rest go without) */
#define SPEC_NEW_MAX 6   /* chunks whose normals are computed in one frame (~10-25 us each) */
typedef struct { const MapInst *m; const unsigned char *base; const Chunk *c; } SpecItem;
static SpecItem spec_l[SPEC_MAX];
static int nspec, spec_new;
static unsigned spec_frame;
static struct { const void *key; unsigned check, last; } spec_slot[SPEC_CACHE];
static unsigned char spec_nrm[SPEC_CACHE][SPEC_NV * 4] __attribute__((aligned(16)));
unsigned map_spec_batches, map_spec_tris, map_spec_new;  /* this frame (logs) */
/* per code (mapspec.py): Blinn fit (a, b) of c^(2^k) (vu1.c's skin table), Schlick's grazing weight G, F0. ASSUMED:
 * G = 1 - the code's roughness, metal F0 0.6 (DSR tints it by the albedo, the pass is untextured) */
static const float spec_tab[3][4] = {{1, 0, 0.15f, 0.04f}, {1.695f, -0.695f, 0.25f, 0.04f}, {1.695f, -0.695f, 0.25f, 0.6f}};

static int spec_code(const MapInst *m, const Chunk *c)
{
    return c->tex >= 0 && c->tex < m->ntex ? (m->tex_bin + m->hdr->tex_off)[32 * c->tex + 7] >> 2 & 3 : 0;
}

void mp_spec_add(const MapInst *m, const unsigned char *base, const Chunk *c, float d)
{
    if (MAP_SPEC && nspec < SPEC_MAX && d < SPEC_DIST && !(c->flags & CHUNK_COARSE) && spec_code(m, c))
        spec_l[nspec].m = m, spec_l[nspec].base = base, spec_l[nspec++].c = c;
}

/* the chunk's normal / F0 stream (s8 normal x 127, u8 F0 x 127 per vertex: vu1mapspec's input), or NULL this frame.
 * Normals point out of DSR's triangles: -(b - a) x (c - a) (FLVER winding, measured on m10_02's map pieces:
 * 198575 of 198806 faces). Indexed chunks: area-weighted over the triangles sharing a vertex; lists: per face. */
static const unsigned char *spec_normals(const unsigned char *base, const Chunk *c, int f0)
{
    int n = c->nv ? c->nv : 3 * c->tris;
    if (n > SPEC_NV || c->tris <= 0)
        return NULL;
    const short *p = (const short *)(base + c->pos);
    const void *key = p;
    unsigned check = (unsigned)c->tris | (unsigned)n << 8 | (unsigned)(unsigned short)p[0] << 16;
    int lru = 0;
    for (int i = 0; i < SPEC_CACHE; i++) {
        if (spec_slot[i].key == key && spec_slot[i].check == check) {
            spec_slot[i].last = spec_frame;
            return spec_nrm[i];
        }
        if (spec_slot[i].last < spec_slot[lru].last)
            lru = i;
    }
    if ((spec_slot[lru].key && spec_slot[lru].last == spec_frame) || spec_new >= SPEC_NEW_MAX)
        return NULL;  /* every slot drawn this frame / enough computed this frame: next frame */
    spec_new++;
    float acc[SPEC_NV][3];
    memset(acc, 0, sizeof(float) * 3 * (unsigned)n);
    const unsigned char *ix = c->nv ? base + CHUNK_IDX(c) : NULL;
    for (int t = 0; t < c->tris; t++) {
        int a = ix ? ix[4 * t] : 3 * t, b = ix ? ix[4 * t + 1] : 3 * t + 1, d = ix ? ix[4 * t + 2] : 3 * t + 2;
        if (a >= n || b >= n || d >= n)
            continue;
        float e1[3], e2[3];
        for (int k = 0; k < 3; k++)
            e1[k] = (float)(p[3 * b + k] - p[3 * a + k]), e2[k] = (float)(p[3 * d + k] - p[3 * a + k]);
        float cr[3] = {e2[1] * e1[2] - e2[2] * e1[1], e2[2] * e1[0] - e2[0] * e1[2], e2[0] * e1[1] - e2[1] * e1[0]};
        for (int k = 0; k < 3; k++)
            acc[a][k] += cr[k], acc[b][k] += cr[k], acc[d][k] += cr[k];
    }
    unsigned char *o = spec_nrm[lru];
    for (int i = 0; i < n; i++) {
        float l = acc[i][0] * acc[i][0] + acc[i][1] * acc[i][1] + acc[i][2] * acc[i][2];
        float k = l > 1e-12f ? 127.0f / sqrtf(l) : 0;
        signed char *q = (signed char *)(o + 4 * i);
        if (k > 0)
            q[0] = (signed char)(acc[i][0] * k), q[1] = (signed char)(acc[i][1] * k), q[2] = (signed char)(acc[i][2] * k);
        else
            q[0] = 0, q[1] = 127, q[2] = 0;
        o[4 * i + 3] = (unsigned char)f0;
    }
    /* zero to the qword's end: VIF reads the words after an UNPACK's data in the DMA's last qword as VIFcodes; then
     * out of the data cache, the chain REFs it */
    unsigned bytes = (4u * (unsigned)n + 15) & ~15u;
    memset(o + 4 * n, 0, bytes - 4u * (unsigned)n);
    SyncDCache(o, o + bytes);
    spec_slot[lru].key = key, spec_slot[lru].check = check, spec_slot[lru].last = spec_frame;
    map_spec_new++;
    return o;
}

void mp_spec_draw(void)
{
    spec_frame++, spec_new = 0;
    map_spec_batches = map_spec_tris = map_spec_new = 0;
    if (!nspec)
        return;
    GSGLOBAL *gs = mp_gs;
    float up[3], dn[3], si[3], ls[3];
    map_char_spec(up, dn, si, ls);
    /* added, depth GEQUAL against the opaque pass's own Z, no Z write, faded to black by the fog */
    u64 r[8] = {GS_ALPHA_ADD, GSR_ALPHA, gs_zbuf(gs, 1), GSR_ZBUF, GS_TEST_ZGE, GSR_TEST, 0, GSR_FOGCOL};
    vu1_gs_regs(4, r);
    vu1_map_spec(up, dn, si);
    for (int i = 0; i < nspec; i++) {
        const SpecItem *it = &spec_l[i];
        const Chunk *c = it->c;
        const float *tb = spec_tab[spec_code(it->m, c) - 1];
        const unsigned char *nrm = spec_normals(it->base, c, (int)(tb[3] * 127 + 0.5f));
        if (!nrm)
            continue;
        float v[3], h[3], vl = 0, hl = 0;
        for (int a = 0; a < 3; a++)
            v[a] = mp_cam[a] - (c->mn[a] + c->mx[a]) * 0.5f, vl += v[a] * v[a];
        vl = vl > 1e-12f ? 1 / sqrtf(vl) : 0;
        for (int a = 0; a < 3; a++)
            v[a] *= vl, h[a] = ls[a] + v[a], hl += h[a] * h[a];
        hl = hl > 1e-12f ? 1 / sqrtf(hl) : 0;
        float rows[16];
        for (int a = 0; a < 3; a++)  /* row a: component a of (Ls, 2 Up, V, H), / 127 for the s8 normals */
            rows[4 * a] = ls[a] / 127, rows[4 * a + 1] = (a == 1 ? 2.0f : 0) / 127, rows[4 * a + 2] = v[a] / 127,
            rows[4 * a + 3] = h[a] * hl / 127;
        rows[12] = 1e6f, rows[13] = -v[1], rows[14] = tb[0], rows[15] = tb[1];
        vu1_add_spec(c->flags & CHUNK_CULL, c->tris, c->nv, it->base + c->pos, nrm,
                     c->nv ? it->base + CHUNK_IDX(c) : NULL, c->org, rows, tb[2]);
        map_spec_batches++, map_spec_tris += c->tris;
    }
    nspec = 0;
    {   /* every 300 frames: batches / triangles a frame (VU1 cost: vu1mapspec's doc) and normals computed */
        static unsigned fr, sb, st, sn;
        sb += map_spec_batches, st += map_spec_tris, sn += map_spec_new;
        if (!(++fr % 300))
            printf("map: spec pass %u batches %u tris a frame, %u chunks' normals computed in 300 frames\n", sb / 300,
                   st / 300, sn), sb = st = sn = 0;
    }
    /* the map's state again: gsKit's ALPHA, Z written, its fog colour (TEST comes per batch) */
    u32 col = (u32)mp_fog_now[3] | (u32)mp_fog_now[4] << 8 | (u32)mp_fog_now[5] << 16;
    u64 rr[6] = {gs->PrimAlpha, GSR_ALPHA, gs_zbuf(gs, 0), GSR_ZBUF, col, GSR_FOGCOL};
    vu1_gs_regs(3, rr);
}

int map_far_dof(float *begin, float *end, float *mul)
{
    if (!mp_pfog_rows)
        return 0;
    const FogRow *r = &mp_pfog_row[mp_fog_area];
    *begin = r->dof_begin, *end = r->dof_end, *mul = r->dof_mul * 0.01f;
    return r->dof_mul > 0 && r->dof_end > r->dof_begin;
}

/* DSR's fog (FRPG shaders, CalcGetFogCoef / CalcGetFogCol): density = saturate(max * saturate((z - begin) /
 * (end - begin))) on view z, mixed towards the fog colour; GS: F = 255 * (1 - density), clamped at 0 when max > 1 */
void mp_fog_terms(float *a, float *b, float *fmin)
{
    if (mp_pfog_rows)  /* ease into the area's row */
        mp_fog_ease(FOG_RATE);
    float span = mp_fog_now[1] - mp_fog_now[0], mx = mp_fog_now[2];
    if (mx <= 0 || span <= 0) {  /* no DSR fog: fmin = 255 clamps it away, b only sets the edge fog's start */
        *a = 0, *b = 255 / (1 - EDGE0), *fmin = 255;
    } else {
        *a = -255 * mx / span;
        *b = 255 + 255 * mx * mp_fog_now[0] / span;
        *fmin = mx < 1 ? 255 * (1 - mx) : 0;
    }
    /* Edge fog (vu1map2 edge loop): F = min(F, b + a2 z), 0 at FAR. It starts where it drops below the DSR curve:
     * at once if it falls faster than DSR's ramp, else where it meets the curve's floor fmin. ASSUMED shape (linear
     * from the DSR ramp's own intercept b; TECH_DEBT map-15). */
    mp_edge_a2 = -*b / FAR;
    mp_edge_z = mp_edge_a2 <= *a ? 0 : (*fmin - *b) / mp_edge_a2;
}

/* Fog of sky row `row` of m (counted from 1) as vu1_map_fog / r_set_fog terms, F = clamp(a z + b, fmin, 255), and its
 * FOGCOL. DSR's curve as fog_terms: density = saturate(max x saturate((z - begin) / (end - begin))). Row 0 (chunks
 * without a row, and maps exported before the rows): the header's sky fog at its maximum density at every distance. */
u32 mp_sky_fog(const MapInst *m, unsigned row, float *a, float *b, float *fmin)
{
    if (!row) {
        *a = 0, *b = *fmin = 255 - m->hdr->sky_fog[3];
        return m->hdr->sky_fog[0] | m->hdr->sky_fog[1] << 8 | m->hdr->sky_fog[2] << 16;
    }
    const SkyRow *s = &m->sky_row[row - 1];
    float span = s->end - s->begin, mx = s->max;
    if (mx <= 0 || span <= 0)
        *a = 0, *b = *fmin = 255;  /* a row without fog */
    else
        *a = -255 * mx / span, *b = 255 + 255 * mx * s->begin / span, *fmin = mx < 1 ? 255 * (1 - mx) : 0;
    return s->rgb[0] | s->rgb[1] << 8 | s->rgb[2] << 16;
}
