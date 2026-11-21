/* The map's passes around the cells (mappass.h): the sky with the void ring and the boot map's backdrop pictures, the
 * horizon / coarse fades and the blended effects (the share map_draw_blend draws after the characters) */
#include "mappass.h"
#include "mapvis.h"
#include "maplight.h"
#include "mapchunk.h"
#include "maplog.h"
#include "cvar.h"

/* The boot map's far backdrop as picture layers (tools/pano.py bake): pano.bin in the area pack = u32 'PANO', chunks,
 * chunk offset, textures, texture offset; Chunk records as map.bin's (offsets from pano.bin, tex = its own texture
 * index). DSR draws its distant pieces (CHUNK_BACKDROP: Firelink's ring walls and valley, 48k triangles, 1.6 MB)
 * textured at full detail; the port never loaded them and drew the horizon's flat stand-in. The pieces beyond
 * horizon.py's HZN_PANO_D of the walkable ground are instead rendered offline with their textures and baked light
 * into 128 px tiles, drawn on world-fixed sheets at the depth they were seen at, under the map's fog (the horizon
 * leaves those triangles out). Drawn after the sky from any block of the boot map's draw-group area (the sheets are
 * fixed in the world; another area, the Undead Asylum, shows none). PANO=0 at build time: an empty file, the horizon
 * keeps the backdrop. */
static unsigned char *pano;
static const Chunk *pano_chunk;
static int pano_n, pano_tex = -1;
static unsigned pano_drawn;  /* triangles this frame (the far panorama log line) */

void map_pano_open(const char *pak_path)
{
    Pack pk;
    unsigned size;
    if (pano || !pack_open(&pk, pak_path))
        return;
    pano = pack_load(&pk, "pano.bin", &size);
    pack_close(&pk);
    if (!pano)
        return;
    const unsigned *h = (const unsigned *)pano;
    if (memcmp(pano, "PANO", 4) || !h[1] || (pano_tex = tc_add(pano, pano + h[4], h[3], 0)) < 0) {
        if (!memcmp(pano, "PANO", 4) && h[1])
            printf("map: panorama NOT loaded (%u textures: no texture slots or CLUT VRAM)\n", h[3]);
        free(pano), pano = NULL;
        return;
    }
    pano_chunk = (const Chunk *)(pano + h[2]), pano_n = h[1];
    FlushCache(0);  /* texcache DMAs the texels and VU1 the streams straight from this buffer */
    printf("map: panorama %d chunks, %u textures, %u KB\n", pano_n, h[3], size >> 10);
}

/* The picture layers are drawn while the player stands in the boot block (MAP_PANO_NEAR, default on; 0 = from any
 * block of its draw-group area, the old path): they are flat sheets baked from one eye in the middle of its walkable
 * ground, and from the Undead Burg (~150 m off that eye) they smeared and repeated their texture edges (Ame's report
 * 2026-11-17 t 245-253). Elsewhere the far panorama's FAR_PANO set (tools/farpano.py) shows that backdrop untextured,
 * as it shows the rest of the far view. */
#ifndef MAP_PANO_NEAR
#define MAP_PANO_NEAR 1
#endif
int map_pano_shown(void)
{
    return pano_n && (!MAP_PANO_NEAR || mp_pinst == 0);
}

/* map_draw keeps what its later passes draw in these lists while it walks the cells: effect chunks (sorted far to
 * near, drawn after the opaque map), horizons fading in over their cells' coarse level (HZN_FADE) and coarse levels
 * fading out over their near level (NEAR_FADE) */
typedef struct { float d; const Chunk *c; const unsigned char *base; int cls, k; unsigned no; GSTEXTURE *late; } FxItem;
typedef struct { unsigned char k, a; unsigned short ci; } FadeItem;
static FxItem fxl[MAX_FX];
static FadeItem hzfade[MAX_HZ_FADE], nrfade[MAX_NEAR_FADE];
static int nfx, nhzfade, nnrfade;  /* entries of fxl, hzfade, nrfade this frame */

void mp_pass_reset(void)
{
    nfx = nhzfade = nnrfade = 0;
    pano_drawn = 0;
}

unsigned mp_pano_drawn(void) { return pano_drawn; }

void mp_fx_add(const Chunk *c, const float e[3], int k, int cls, const unsigned char *base, unsigned no)
{
    if (nfx < MAX_FX) {
        float d = 0;
        for (int a = 0; a < 3; a++) {
            float mm = (c->mn[a] + c->mx[a]) * 0.5f - e[a];
            d += mm * mm;
        }
        fxl[nfx].d = d, fxl[nfx].c = c, fxl[nfx].k = k, fxl[nfx].cls = cls;
        fxl[nfx].base = base, fxl[nfx++].no = no;
    }
}

void mp_hzfade_add(int k, int ci, int al)
{
    if (nhzfade < MAX_HZ_FADE)
        hzfade[nhzfade].k = (unsigned char)k, hzfade[nhzfade].ci = (unsigned short)ci,
        hzfade[nhzfade++].a = (unsigned char)al;
}

void mp_nrfade_add(int k, int ci, int al)
{
    if (nnrfade < MAX_NEAR_FADE)
        nrfade[nnrfade].k = (unsigned char)k, nrfade[nnrfade].ci = (unsigned short)ci,
        nrfade[nnrfade++].a = (unsigned char)al;
}

/* VOID_RING (default 1, runtime: option set VOID_RING 0 = the old path): the last-resort backdrop (map streaming
 * research idea 2). main.c clears depth only, because the sky dome covers every pixel, so wherever no level covers
 * the screen (a block still loading, a far panorama refused or not baked from there, a draw-group mask hiding the far
 * side of a seam) the bright sky showed through: the "white voids" of Ame's play reports (2026-11-16 / 17). In DSR
 * every map of the area is resident and those pixels show distant geometry, which the fog has taken to the fog
 * colour at the port's edge fog distance (FAR). The ring is that stand-in: a band in the map's current fog colour
 * from the horizon down, fading out VOID_RING_FADE degrees above it, drawn in screen space over the sky dome and
 * under everything else (no Z written: every map piece, horizon and panorama draws over it). DSR's own distant
 * ground (sky chunks low on the horizon, sky_low) still draws over it. Always resident: no memory, 2 strips, about
 * 0.1 ms of GS fill (half the screen, opaque). ASSUMED: the camera has no roll (main.c's orbit camera), so the
 * horizon is a screen row. Not drawn in hole builds (F9_HOLES / option holes: the magenta clear must show) */
#ifndef VOID_RING
#define VOID_RING cv_VOID_RING  /* runtime switch, default 1 (src/cvar.h) */
#endif
#ifndef VOID_RING_SKY
#define VOID_RING_SKY 1  /* the ring in the sky's fog colour; 0 = the map's fog colour */
#endif
#ifndef VOID_RING_TOP
#define VOID_RING_TOP 0.0f   /* degrees above the horizon where the ring is still opaque (ASSUMED) */
#endif
#ifndef VOID_RING_FADE
#define VOID_RING_FADE 6.0f  /* degrees over which it fades out above that (ASSUMED: a soft edge, not a line) */
#endif
int map_holes;

/* a sky chunk whose box centre lies within 5 degrees above the eye or lower (1): distant ground, drawn over the ring;
 * 0: the dome (ASSUMED: DSR's dome pieces are cut by sky sector and reach far above the horizon, its ground rings
 * not) */
static int sky_low(const Chunk *c, const float e[3])
{
    float dx = (c->mn[0] + c->mx[0]) * 0.5f - e[0], dz = (c->mn[2] + c->mx[2]) * 0.5f - e[2];
    float dy = (c->mn[1] + c->mx[1]) * 0.5f - e[1], h = sqrtf(dx * dx + dz * dz);
    /* and not a bowl reaching far below: Firelink's black underside (vertex colour 0, -4 to -82 degrees) and the
     * Burg's dome bottom (-2 to -86) drew over the ring in another fog and left a hard edge across the cloud sea a few
     * degrees down (graveyard, aqueduct exit, 2026-11-18); the ground rings span -6 to 0 (ASSUMED: 15 degrees, tan 0.27) */
    return dy < 0.0875f * h && c->mn[1] - e[1] > -0.27f * h;  /* tan 5 deg */
}

/* screen row (draw buffer, 448 lines) of the directions el degrees above the horizon, camera pitch p (radians) */
static float ring_row(float el, float p)
{
    float a = el * (3.14159265f / 180.0f) - p;
    a = a > 1.4f ? 1.4f : a < -1.4f ? -1.4f : a;  /* (beyond ~80 degrees off the view axis: off screen anyway) */
    float y = 224.0f - mp_cam_fy * tanf(a);
    return y < -64.0f ? -64.0f : y > 512.0f ? 512.0f : y;
}

static void draw_void_ring(u32 col)
{
    GSGLOBAL *gs = mp_gs;
    float p = asinf(mp_cam[10] > 1 ? 1 : mp_cam[10] < -1 ? -1 : mp_cam[10]);  /* forward's y: the camera's pitch */
    float y0 = ring_row(VOID_RING_TOP, p), y1 = ring_row(VOID_RING_TOP + VOID_RING_FADE, p);
    if (y1 >= (float)gs->Height)
        return;  /* the horizon is below the screen */
    /* registers: 3 state + 9 fade band + 2 + 2 per 64-pixel strip + 2 restore = 36 at 640 pixels (24 overran into
     * draw_sky's frame: TLB miss in sky_low on its eye pointer, ISO 2026-11-17); room for 1024 */
    u64 r[2 * (16 + 2 * 16)];
    int n = 0;
    u64 zbuf = GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0);
    int ox = gs->OffsetX, oy = gs->OffsetY, w = gs->Width << 4, h = gs->Height << 4;
    int iy0 = y0 > 0 ? (int)(y0 * 16) : 0, iy1 = y1 > 0 ? (int)(y1 * 16) : 0;
    iy0 = iy0 > h ? h : iy0, iy1 = iy1 > h ? h : iy1;
    u64 c0 = GS_SETREG_RGBAQ(col & 0xFF, col >> 8 & 0xFF, col >> 16 & 0xFF, 0x80, 0);
    u64 c1 = GS_SETREG_RGBAQ(col & 0xFF, col >> 8 & 0xFF, col >> 16 & 0xFF, 0x00, 0);
#define REG(v, a) (r[2 * n] = (v), r[2 * n + 1] = (a), n++)
    REG(GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 1), 0x47);  /* ZTE, ZTST ALWAYS (the sky's own state) */
    REG(zbuf | 1ull << 32, 0x4E);                        /* Z not written */
    REG(GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0x42);           /* (Cs - Cd) As + Cd */
    if (iy1 < iy0) {  /* the fade band: alpha 0 at its top, opaque at its foot (gouraud) */
        REG(GS_SETREG_PRIM(GS_PRIM_PRIM_TRISTRIP, 1, 0, 0, 1, 0, 0, 0, 0), 0x00);
        REG(c1, 0x01), REG(GS_SETREG_XYZ2(ox, oy + iy1, 0), 0x05);
        REG(c1, 0x01), REG(GS_SETREG_XYZ2(ox + w, oy + iy1, 0), 0x05);
        REG(c0, 0x01), REG(GS_SETREG_XYZ2(ox, oy + iy0, 0), 0x05);
        REG(c0, 0x01), REG(GS_SETREG_XYZ2(ox + w, oy + iy0, 0), 0x05);
    }
    if (iy0 < h) {  /* opaque from there to the bottom */
        REG(GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0), 0x00);
        REG(c0, 0x01);
        for (int x = 0; x < w && n + 4 <= (int)(sizeof r / sizeof r[0] / 2); x += 64 << 4) {  /* 64-pixel strips
                                                                                     (z_clear's page-friendly pattern) */
            int x1 = x + (64 << 4) < w ? x + (64 << 4) : w;
            REG(GS_SETREG_XYZ2(ox + x, oy + iy0, 0), 0x05);
            REG(GS_SETREG_XYZ2(ox + x1, oy + h, 0), 0x05);
        }
    }
    REG(gs->PrimAlpha, 0x42);
    REG(zbuf, 0x4E);
#undef REG
    vu1_gs_regs(n, r);
}

/* The sky of block sk and the far panorama, then the fog and test registers of the opaque map (fog terms a, b, fmin,
 * colour col). Returns the triangles drawn. */
int mp_draw_sky(int sk, const float e[3], u32 col, float a, float b, float fmin)
{
    GSGLOBAL *gs = mp_gs;
    MapInst *m = &mp_inst[sk];
    int drawn = 0;
    u64 r[2] = {0, 0x3D};

    /* sky first, each piece under its own FogBank row as in DSR (SkyRow): opaque, then the alpha clouds. The rows in
     * turn, so the fog constants change once per row and pass at most. The edge fog is the map's, not the sky's. */
    r_set_fog_edge(0);
    /* passes: 0 the opaque sky (with VOID_RING: the pieces above the horizon), 1 (VOID_RING) the void ring, then the
     * opaque pieces low on the horizon over it (DSR's distant ground rings, Firelink's m9000), 2 the alpha clouds */
    int ring = VOID_RING && !map_holes;
#ifdef MAP_PANO_R
    ring = 0;  /* panorama capture: no sky (the magenta clear marks the panorama's transparent texels) */
#endif
    for (int pass = 0; pass < 3; pass++) {  /* the player's block's sky (its streams once they are in) */
        int blend = pass == 2;
        if (pass == 1) {
            if (!ring)
                continue;
            /* VOID_RING_SKY (default 1; 0 = the map's fog colour): the sky's own fog colour (the header's sky row,
             * what DSR's sky fades to at the horizon). In the map's fog colour the ring was a dark band with a hard
             * edge over Firelink's cloud sea (graveyard, aqueduct exit; ISO 2026-11-17) */
            draw_void_ring(VOID_RING_SKY && m->hdr ? (u32)(m->hdr->sky_fog[0] | m->hdr->sky_fog[1] << 8 |
                                                           m->hdr->sky_fog[2] << 16) : col);
        }
        const MatRow *last = NULL;  /* the class row whose state is set (blended pass) */
        for (unsigned row = 0; row <= m->sky_rows; row++) {
            int set = 0;
            for (int i = 0; i < m->nsky; i++) {
                const unsigned no = m->sky[i];
                const Chunk *c = &m->chunks[no];
                unsigned cr = CHUNK_SKY_ROW(c->flags);
                if ((cr <= m->sky_rows ? cr : 0) != row)
                    continue;
                const MatRow *mr = mp_row(m, c);
                if (!mr->zmsk != !blend || (c->flags & CHUNK_BACKDROP) || !dg_shown(sk, no))
                    continue;
                if (ring && !blend && sky_low(c, e) != pass)
                    continue;
#ifdef MAP_PANO_R
                continue;  /* panorama capture: no sky (the magenta clear marks the panorama's transparent texels) */
#endif
                const unsigned char *sb = mp_res_base(m, no);
                int cls = sb ? mp_visible(c, e, 1) : 0;
                if (!cls)
                    continue;
                if (!set++) {
                    float sa, sb2, smin;
                    u32 sky_col = mp_sky_fog(m, row, &sa, &sb2, &smin);
                    mp_zs_fog(sa, sb2, smin, 255);
                    r[0] = sky_col;
                    mp_zs_regs(1, r);
                    r_set_fog(sa, sb2, smin, 255, sky_col, col);
                }
                if (blend && mr != last)  /* the class's ALPHA / ZBUF (material.h) */
                    mp_row_regs(gs, mr), last = mr;
                drawn += mp_draw_chunk(m, sb, c, c->tris, cls, R_FOG | (blend ? R_BLEND : 0), no);
            }
        }
    }
    mp_opaque_regs(gs);
    /* opaque map with the map's main fog row, and the edge fog; TEST for the batches with a mip chain (vu1_mip: they
     * send MIPTBP1 in its place) */
    mp_zs_fog(a, b, fmin, 255);
    mp_zs_edge(mp_edge_a2);
    r[0] = col;
    mp_zs_regs(1, r);
    {
        u64 rt[2] = {GS_SETREG_TEST(1, 5, 0x40, 0, 0, 0, 1, 2), 0x47};
        mp_zs_regs(1, rt);
    }
    r_set_fog(a, b, fmin, 255, col, col);
    r_set_fog_edge(mp_edge_a2);
    /* the far backdrop's picture layers (map_pano_open): over the sky, under the map's fog as the pieces they show
     * (the backdrop: DSR's fog only, no edge fog), alpha tested, far layer first */
    if (map_pano_shown() && (mp_inst[0].on ? dg_area(mp_pinst) == dg_area(0) : !sk)) {
        mp_zs_edge(0);
        for (int i = 0; i < pano_n; i++) {
            const Chunk *c = &pano_chunk[i];
            int cls = mp_visible(c, e, 1);
            GSTEXTURE *t = cls ? tc_get(pano_tex + c->tex, 0) : NULL;
            if (!t)
                continue;
            vu1_add_ex(t, 0, VU1_FOG | (cls == 2 ? VU1_CLIP : 0), c->tris, pano + c->pos, pano + c->st, pano + c->col,
                       c->org, c->flags & CHUNK_ST1K ? 1024.0f : 4096.0f);
            drawn += c->tris, pano_drawn += c->tris;
        }
        mp_zs_edge(mp_edge_a2);
    }
    return drawn;
}

/* The first n entries of hzfade. Returns the triangles drawn (all horizon triangles). */
int mp_draw_hz_fades(const float e[3], float a, float b, float fmin)
{
    int n = nhzfade;
    if (!n)
        return 0;
    GSGLOBAL *gs = mp_gs;
    int drawn = 0;
    /* horizons fading in (HZN_FADE) over their cells' coarse level, Cs a + Cd (1 - a), Z tested HZN_ZBIAS nearer
     * and not written, front faces only: the horizon is drawn double-sided, so a wall's back face (or a layer just
     * behind the coarse surface, now inside the bias) would blend a second time. A depth-only pre-pass did the same
     * but left the horizon's biased depth for the characters and effects drawn after (feet in the ground at
     * ~50 m) */
    mp_zs_zbias(1.0f + HZN_ZBIAS);
    mp_zs_fog(a, b, fmin, 255);
    mp_zs_edge(mp_edge_a2);
    u64 zb = GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0) | (1ull << 32);
    for (int i = 0; i < n; i++) {
        const MapInst *mk = &mp_inst[hzfade[i].k];
        const HzCell *hc = &mk->hz_cell[hzfade[i].ci];
        if (HZN_FADE_DEBUG && !i) {
            u64 fc[2] = {0xFF00FFull, 0x3D};  /* FOGCOL r, g, b */
            mp_zs_regs(1, fc);
            mp_zs_fog(a, b, 0, 0);
        }
        u64 r[4] = {GS_SETREG_ALPHA(0, 1, 2, 1, HZN_FADE_DEBUG ? 0x80 : hzfade[i].a), 0x42, zb, 0x4E};
        mp_zs_regs(2, r);
        for (unsigned j = 0; j < hc->count; j++) {
            const Chunk *c = &mk->hz_chunk[hc->first + j];
            if (!dg_mask_shown(hzfade[i].k, (int)(c->flags >> 16 & 0xFF)))
                continue;
            int cls = mp_visible(c, e, 0);
            if (cls) {
                unsigned dn = mp_draw_hz(mk, c, cls, VU1_BLEND, 1);
                drawn += dn;
            }
        }
    }
    mp_zs_zbias(1.0f);
    mp_zs_fog(a, b, fmin, 255);
    if (HZN_FADE_DEBUG) {  /* the frame's fog colour back for what is drawn after */
        float f[5];
        u64 fc[2] = {0, 0x3D}, fb;
        r_get_fog(f, &fc[0], &fb);
        mp_zs_regs(1, fc);
    }
    mp_opaque_regs(gs);
    return drawn;
}

/* The first n entries of nrfade. Returns the triangles drawn. */
int mp_draw_near_fades(const float e[3])
{
    int n = nnrfade;
    if (!n)
        return 0;
    GSGLOBAL *gs = mp_gs;
    int drawn = 0;
    /* coarse levels fading out (NEAR_FADE) over their cells' near level, as the horizon fade above: Cs a + Cd
     * (1 - a), Z tested HZN_ZBIAS nearer and not written (the coarse surface is the near one simplified) */
    mp_zs_zbias(1.0f + HZN_ZBIAS);
    u64 zb = GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0) | (1ull << 32);
    for (int i = 0; i < n; i++) {
        const MapInst *mk = &mp_inst[nrfade[i].k];
        const Cell *ce = &mk->cells[nrfade[i].ci];
        u64 r[4] = {GS_SETREG_ALPHA(0, 1, 2, 1, nrfade[i].a), 0x42, zb, 0x4E};
        mp_zs_regs(2, r);
        mapstream_select(nrfade[i].k);  /* mapstream_res answers for that instance */
        for (unsigned j = ce->first; j < (unsigned)ce->first + ce->count; j++) {
            unsigned no = mk->order[j];
            const Chunk *c = &mk->chunks[no];
            if (!(c->flags & CHUNK_COARSE) || !hz_covers(c->flags) || (c->flags & CHUNK_BACKDROP))
                continue;
            const unsigned char *base = mp_res_base(mk, no);
            if (!base || !dg_shown(nrfade[i].k, no))
                continue;
            int cls = mp_visible(c, e, 0);
            if (cls)
                drawn += mp_draw_chunk(mk, base, c, c->tris, cls, R_FOG | R_BLEND, no);
        }
    }
    mapstream_select(0);
    mp_zs_zbias(1.0f);
    mp_opaque_regs(gs);
    return drawn;
}

static int nfx_late;  /* fxl entries map_draw_blend draws (late != NULL) */

/* The first n entries of fxl, after the opaque map: alpha-blended (mist, decals, water) far to near, then additive
 * (light shafts), then subtractive (both order-free); Z tested, not written; each chunk's GS state from its class row
 * (material.h). which: 0 all of them here (MAP_BLEND_LATE=0); 1 the map segment's share: with ZSPLIT the part beyond
 * it now, in the far pass (its Z range is gone once the near pass clears Z), the rest kept for map_draw_blend with its
 * texture resolved now (its upload goes in the map chain, tc_begin..tc_end); 2 map_draw_blend: the kept ones, after
 * the characters and objects (render-19: a character behind a blended surface is drawn under it, as in DSR).
 * The GS fogs a chunk's colour before blending, so additive and subtractive chunks fog towards black (fading out, as
 * fx.c's additive particles); fogged towards the map's fog colour col they would add / subtract it with distance.
 * MAP_FX_FOG0=0: all three towards col, as before. Returns the triangles drawn. */
#ifndef MAP_FX_FOG0
#define MAP_FX_FOG0 cv_MAP_FX_FOG0  /* runtime switch, default 1 (src/cvar.h) */
#endif
int mp_draw_effects(int fog, int which, u32 col)
{
    int n = nfx;
    GSGLOBAL *gs = mp_gs;
    int drawn = 0;
    if (which != 2) {
        for (int i = 1; i < n; i++)  /* insertion sort by distance, far first; a few dozen per frame */
            for (int j = i; j > 0 && fxl[j].d > fxl[j - 1].d; j--) {
                FxItem tmp = fxl[j];
                fxl[j] = fxl[j - 1], fxl[j - 1] = tmp;
            }
        nfx_late = 0;
        for (int i = 0; i < n; i++)
            fxl[i].late = NULL;
    }
    for (int pass = 0; pass < 3; pass++) {  /* 0 blended, 1 additive, 2 subtractive */
        const MatRow *last = NULL;
        if (MAP_FX_FOG0 && pass == 1) {
            u64 fc[2] = {0, 0x3D};  /* FOGCOL black for the order-free passes */
            mp_zs_regs(1, fc);
        }
        for (int i = 0; i < n; i++) {
            const Chunk *c = fxl[i].c;
            const MapInst *mi = &mp_inst[fxl[i].k];
            const MatRow *mr = mp_row(mi, c);
            if ((mr->alpha == GS_ALPHA_ADD ? 1 : mr->alpha == GS_ALPHA_SUB ? 2 : 0) != pass)
                continue;
            int cls = fxl[i].cls, mode = fog | R_BLEND | (pass == 1 ? R_ADD : 0), far = 0;
            if (which == 1) {
                int r = 2;
                if (mp_zs_far())
                    r = mp_zs_route(c, &cls, fxl[i].cls);
                if ((r & 2) && c->tex >= 0 && c->tex < mi->ntex) {  /* nearer than ZSPLIT: drawn after the characters */
                    int tx = mi->tex_base + c->tex;
                    float d2 = 0;
                    for (int a = 0; a < 3; a++) {
                        float o = mp_cam[a] < c->mn[a] ? c->mn[a] - mp_cam[a] : mp_cam[a] > c->mx[a] ? mp_cam[a] - c->mx[a] : 0;
                        d2 += o * o;
                    }
                    MP_BEGIN
                    fxl[i].late = tc_get(tx, tc_level(tx, sqrtf(d2)));
                    MP_END(MP_TEX)
                    nfx_late += fxl[i].late != NULL;
                }
                if (!(r & 1))
                    continue;
                far = 1;
            } else if (which == 2 && !fxl[i].late)
                continue;
            if (mr != last)
                mp_row_regs(gs, mr), last = mr;
            if (far) {  /* (draw_chunk would record the near part again: the far part alone, as it draws it) */
                drawn += mp_draw_chunk_far(mi, fxl[i].base, c, c->tris, cls, mode, fxl[i].no);
            } else {
                drawn += mp_draw_chunk_tex(mi, fxl[i].base, c, c->tris, cls, mode, fxl[i].no, which == 2 ? fxl[i].late : NULL);
            }
        }
    }
    if (MAP_FX_FOG0) {
        u64 fc[2] = {col, 0x3D};
        mp_zs_regs(1, fc);
    }
    mp_opaque_regs(gs);
    return drawn;
}

int map_draw_blend(void)
{
    if (!MAP_BLEND_LATE || !nfx_late || !mp_gs)
        return 0;
    float a, b, fmin;
    mp_fog_terms(&a, &b, &fmin);
    u32 col = (u32)mp_fog_now[3] | (u32)mp_fog_now[4] << 8 | (u32)mp_fog_now[5] << 16;
    /* a segment in the characters' buffer (ot.h OT_BLEND): the map programs' layout, constants and fog again */
    vu1_map_resume();
    vu1_map_fog(a, b, fmin, 255);
    vu1_map_edge(mp_edge_a2);
    u64 r[2] = {col, GSR_FOGCOL};  /* the characters set their own fog colour */
    vu1_gs_regs(1, r);
    int n = mp_draw_effects(R_FOG, 2, col);
    nfx_late = 0;
    return n;
}

