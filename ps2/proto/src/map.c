#include "map_i.h"
int map_vu1_batches, map_ee_tris;
static unsigned short pop_fr;
static unsigned pop_total;
MapInst mp_inst[MAP_INST] = {{.on = 0}};
static V3 player_feet;
float mp_fog_now[FOG_NOW] = {0, 0, 0, 0, 0, 0, 64, 64, 64, 96, 96, 96, 0.40f * 127, 0.80f * 127, -0.45f * 127};
int mp_fog_area;
static GSGLOBAL *gsg;
const FogRow *mp_pfog_row;
unsigned mp_pfog_rows;
static int pinst;  /* the instance the player stands in (sky, fog rows) */
const unsigned char *mp_res_base(const MapInst *m, unsigned j)
{
    return m->res_cells ? mapstream_res(m->citem[j]) : m->streams;
}


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

/* the horizon section the DSM5 cell table header points at (words 2 / 3: offset, bytes; tools/mapsplit.py), if any */
void mp_hz_parse(MapInst *m)
{
    m->hz_cell = NULL, m->hz_chunk = NULL, m->hz_base = NULL;
#ifndef HZN_OFF
    if (!m->ctab)
        return;
    const unsigned *ch = (const unsigned *)(m->cel_bin + m->hdr->cell_off);
    if (!ch[2] || ch[3] < 16)
        return;
    const unsigned char *b = m->cel_bin + ch[2];
    const unsigned *h = (const unsigned *)b;
    if (memcmp(b, "HZN1", 4) || h[2] != m->nctab || h[3] != ch[3] || ((unsigned)b & 15)) {
        printf("map: horizon section does not match the cell table (ignored)\n");
        return;
    }
    m->hz_cell = (const HzCell *)(b + 16);
    m->hz_chunk = (const Chunk *)(b + 16 + 32 * m->nctab);
    m->hz_base = b;
    printf("map: horizon %u chunks, %u KB\n", h[1], ch[3] >> 10);
#endif
}

int map_init(const void *blob)
{
    if (!blob)
        return 0;
    MapInst *m = &mp_inst[0];
    const unsigned char *map_bin = m->map_bin = m->tex_bin = m->cel_bin = blob;
    const Header *hdr = m->hdr = (const Header *)map_bin;
    if (memcmp(hdr->magic, "DSM4", 4) && memcmp(hdr->magic, "DSM5", 4))
        return 0;
    m->ctab = hdr->magic[3] == '5' && hdr->cell_off ? (const MapCell *)(map_bin + hdr->cell_off + 16) : NULL;
    m->nctab = m->ctab ? *(const unsigned *)(map_bin + hdr->cell_off) : 0;
    m->ckeys = cell_keys(m->ctab, m->nctab);
    m->pkt = m->ctab && ((const unsigned *)(map_bin + hdr->cell_off))[1] & 1 ? VU1_PKT : 0;
    m->chunks = (const Chunk *)(map_bin + hdr->chunk_off);
    m->fog_rows = *(const unsigned *)(map_bin + hdr->fog_off);
    m->fog_ntris = ((const unsigned *)(map_bin + hdr->fog_off))[1];
    m->fog_row = (const FogRow *)(map_bin + hdr->fog_off + 16);
    m->fog_tri = (const unsigned char *)(m->fog_row + m->fog_rows);
    m->sky_rows = SKY_ROWS ? ((const unsigned *)(map_bin + hdr->fog_off))[2] : 0;
    m->sky_row = (const SkyRow *)(m->fog_tri + ((m->fog_ntris + 3) & ~3u));
    /* no rows: the header's fog and the old fixed character light (0.5 + 0.75 n . L) */
    float f0[FOG_NOW] = {hdr->fog_begin, hdr->fog_end, hdr->fog_max, hdr->fog_rgb[0], hdr->fog_rgb[1], hdr->fog_rgb[2],
                          64, 64, 64, 96, 96, 96, 0.40f * 127, 0.80f * 127, -0.45f * 127};
    memcpy(mp_fog_now, f0, sizeof mp_fog_now);
    mp_fog_area = 0;
    mp_pfog_row = m->fog_row, mp_pfog_rows = m->fog_rows;
    if (m->fog_rows)
        mp_fog_ease(1);
    m->navv = (const float *)(map_bin + hdr->navv_off);
    m->navt = (const unsigned short *)(map_bin + hdr->navt_off);
    m->streams = map_bin, m->tex_base = 0;
    mp_hz_parse(m);
    return 1;
}

/* world blocks: the selected instance's split textures have no mip 0 until mapstream.c reads it (tex_hook_set) */
void mp_tex_res(int k)
{
    tc_res(k ? map_tex_slot(k) : -1, mp_inst[k].streams);
}

/* mapstream.c's mip 0 / 1 hooks (mapstream_tex_hooks): tag = the texture's index in instance k's table, | MS_RES_MIP1
 * for its mip 1 (tools/mapsplit.py MIP1_OUT) */
unsigned mp_tex_hook_age(int k, unsigned tag)
{
    int slot = k ? map_tex_slot(k) : -1;
    return k && slot < 0 ? ~0u : tc_res_age(slot, (int)(tag & ~MS_RES_MIP1), !!(tag & MS_RES_MIP1));
}

void mp_tex_hook_set(int k, unsigned tag, const void *p)
{
    int slot = k ? map_tex_slot(k) : -1;
    if (!k || slot >= 0)
        tc_res_one(slot, (int)(tag & ~MS_RES_MIP1), !!(tag & MS_RES_MIP1), p);
}

int map_upload(GSGLOBAL *gs)
{
    gsg = gs;
    MapInst *m = &mp_inst[0];
    m->ntex = m->hdr->textures < MAX_TEX ? m->hdr->textures : MAX_TEX;
    /* the textures stream through a VRAM pool (texcache.c: uploads in the VIF1 chain, LRU); the CLUTs stay resident */
    /* (tex_bin: the tables' texture piece when the boot block came in pieces, world.c WORLD_BOOT_LEAN; else map_bin) */
    int r = tc_init(gs, m->tex_bin, m->tex_bin + m->hdr->tex_off, m->ntex, TEX_POOL_KB);
    if (!r)
        mp_tex_res(0);
    return r;
}

unsigned map_chunk_count(void)
{
    return mp_inst[0].hdr ? mp_inst[0].hdr->chunks : 0;
}

void map_spawn(V3 *pos, float *yaw)
{
    const Header *hdr = mp_inst[0].hdr;
    *pos = v3(hdr->spawn[0], hdr->spawn[1], hdr->spawn[2]);
    *yaw = hdr->spawn_yaw;
}

/* Chunks that touch the near plane or guard band also go to VU1, which skips the triangles that need clipping;
 * those (and only those) are clipped on the EE and drawn through gsKit */
#ifdef MAP_PROF  /* profiling build: map_vu1_batches / map_ee_tris show visibility / EE-clip time in 0.01 ms */
static unsigned prof_vis, prof_clip;
#define PROF_BEGIN unsigned _t0 = ticks();
#define PROF_END(acc) acc += ticks() - _t0;
#else
#define PROF_BEGIN
#define PROF_END(acc)
#endif

/* map_ms breakdown (budget doc proposals 1 and 6, default on; make DEFS=-DMAP_PASS_MS=0 = off): COP0 Count ticks
 * per part of map_draw this frame. cull = frustum / distance tests, pvs_eye, the index; tex = texcache (tc_get,
 * tc_mip, tc_begin / tc_end: uploads go into the chain there); chain = VIF / GIF packets (vu1_add_*, registers);
 * bump = relief; plan = the rest (per-cell level of detail, near parts, draw groups, visibility rows, effects sort).
 * Timed in one frame of MAP_PASS_EVERY only: each reading of the Count register costs ~0.15 us in PCSX2 and the
 * ~5000 a timed frame takes add ~0.8 ms to it. Logged every MAP_PASS_LOG frames as "map: pass ms" (average of the
 * timed frames, and the timed frame with the largest total). */
#ifndef MAP_PASS_MS
#define MAP_PASS_MS 1
#endif
#define MAP_PASS_EVERY 8
#define MAP_PASS_LOG 120
static int mp_on;  /* this frame is timed */
enum { MP_CULL, MP_TEX, MP_CHAIN, MP_BUMP, MP_N };
/* plan split (the same log, a "map: plan ms" line): per visible cell the resident-miss scan, the near level, the
 * chunk loop's own work (draw groups, visibility rows, streams), the rest of the cell loop, the effects; exclusive of
 * the parts above */
enum { MF_MISS, MF_NEAR, MF_CHUNKS, MF_FX, MF_N };
static unsigned mf_t[MF_N];
static unsigned mc_cells, mc_vis, mc_tiles, mc_tvis, mc_chunks;  /* per frame: cells tested / visible, tiles tested / visible, chunks reached */


static unsigned mp_t[MP_N];
static inline unsigned mp_sum(void) { return mp_t[0] + mp_t[1] + mp_t[2] + mp_t[3]; }
#if MAP_PASS_MS
#define MP_BEGIN unsigned _mp0 = mp_on ? ticks() : 0;
#define MP_END(p) if (mp_on) mp_t[p] += ticks() - _mp0;
#define MP_SPLIT(p) do { if (mp_on) { unsigned _mp1 = ticks(); mp_t[p] += _mp1 - _mp0; _mp0 = _mp1; } } while (0)
#define MF_BEGIN unsigned _mf0 = mp_on ? ticks() : 0, _mfn = mp_on ? mp_sum() : 0;
#define MF_END(p) if (mp_on) mf_t[p] += ticks() - _mf0 - (mp_sum() - _mfn);
#define MF_AGAIN if (mp_on) _mf0 = ticks(), _mfn = mp_sum();
#else
#define MF_BEGIN
#define MF_END(p)
#define MF_AGAIN
#define MP_BEGIN
#define MP_END(p)
#define MP_SPLIT(p) ((void)0)
#endif

/* camera (set per frame in map_draw): eye, right, up, forward; focal length in pixels */
static float cam[12], cam_f;

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

static int zs_room(unsigned w)
{
    if (zs_n + w <= zs_cap)
        return 1;
    zs_lost++;
    return 0;
}

static void zs_gs_regs(int n, const u64 *regs)
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

static void zs_map_fog(float a, float b, float fmin, float fmax)
{
    vu1_map_fog(a, b, fmin, fmax);
    const float f[4] = {a, b, fmin, fmax};
    zs_rec_f(ZS_FOG, 4, f);
}

static void zs_map_zbias(float k)
{
    vu1_map_zbias(k);
    zs_rec_f(ZS_ZBIAS, 1, &k);
}

static void zs_map_edge(float a2)
{
    vu1_map_edge(a2);
    zs_rec_f(ZS_EDGE, 1, &a2);
}
/* edge fog this frame (fog_terms): slope a2 of b + a2 z and the view depth beyond which it is below the DSR fog */
static float edge_a2, edge_z;

/* GS registers for the blend passes: ALPHA (Cs - Cd) * As + Cd or Cs * As + Cd, ZBUF with or without ZMSK */
static void blend_regs(GSGLOBAL *gs, int mode)
{
    u64 zbuf = GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0);
    u64 r[4] = {mode & CHUNK_ADD ? GS_SETREG_ALPHA(0, 2, 0, 1, 0) : GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0x42,
                mode ? zbuf | (1ull << 32) : zbuf, 0x4E};
    if (!mode)
        r[0] = gs->PrimAlpha;
    zs_gs_regs(2, r);
}

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

/* mapstream.c's item visibility (pvs.c): item id of instance k (a near cell part, then a resident item) has a chunk
 * some camera cell within PVS_STREAM_R sees; textures, the sky and maps without pvs.bin count as seen */
static int ms_pvs_vis(int k, unsigned id)
{
    if (k < 0 || k >= MAP_INST || !mp_inst[k].ctab)
        return 1;
    const MapInst *m = &mp_inst[k];
    if (id < m->nctab) {
        unsigned n = m->ctab[id].near & ~CELL_PART;
        return !n || pvs_range(k, id, m->ctab[id].pad, n, 0);
    }
    id -= m->nctab;
    if (!m->rrange || id >= m->nritem || !m->rrange[id][1])
        return 1;
    return pvs_range(k, m->rrange[id][2], m->rrange[id][0], m->rrange[id][1], 0);
}

/* MS_DG (default on; make DEFS=-DMS_DG=0 = pvs only): an item none of whose chunks the draw groups of the recent floors
 * show (drawgrp.c dg_stream_shown) counts as unseen too: Firelink's cells from inside the Burg. A near part goes by
 * its cell's coarse chunks (the same pieces; its own chunks are known only once it is in). */
#ifndef MS_DG
#define MS_DG 1
#endif
static int ms_dg_vis(int k, unsigned id)
{
    const MapInst *m = &mp_inst[k];
    if (!dg_area(k) || !m->order)
        return 1;
    unsigned a, n;
    if (id < m->nctab) {
        unsigned c = id;
        while (c > 0 && (m->ctab[c].near & CELL_PART))  /* a later part of a cell: the cell's own entry */
            c--;
        a = m->ctab[c].first, n = m->ctab[c].count;
        if (!n)
            return 1;
        for (unsigned j = a; j < a + n; j++)
            if (dg_stream_shown(k, m->order[j]))
                return 1;
        return 0;
    }
    id -= m->nctab;
    if (!m->rrange || id >= m->nritem || !(n = m->rrange[id][1]))
        return 1;
    a = m->rrange[id][0];
    for (unsigned c = a; c < a + n; c++)
        if (dg_stream_shown(k, c))
            return 1;
    return 0;
}
int mp_vis(int k, unsigned id)
{
    if (!ms_pvs_vis(k, id))
        return 0;
    return !MS_DG || k < 0 || k >= MAP_INST || !mp_inst[k].ctab || ms_dg_vis(k, id);
}
unsigned mp_vis_gen(int k) { return pvs_gen(k) + (MS_DG ? dg_stream_gen() << 16 : 0); }

int map_stream_open(const char *pak_path)
{
    map_pano_open(pak_path);
    mapstream_vis_hooks(mp_vis, mp_vis_gen);
    return mp_inst[0].ctab ? mapstream_init(pak_path, mp_inst[0].ctab, mp_inst[0].nctab) : 0;
}

void map_set_player(V3 feet)
{
    const float p[3] = {feet.x, feet.y, feet.z};
    static unsigned dg_frames;
    dg_tick(++dg_frames);  /* the recent floors' draw groups for streaming (drawgrp.c) */
    mapstream_update(p);  /* every instance's near cells (src/mapstream.c) */
    /* DSR's fog follows the collision part under the player (its MSB FogID); in the air the last one stays */
    ColHit h;
    player_feet = feet;
    int k = -1;
    if (col_floor(feet.x, feet.z, feet.y + 0.5f, feet.y - 3, COL_SOLID, &h) && (k = map_inst_of(h.block)) >= 0)
        dg_floor(k, h.tri);  /* the shown draw groups follow the same collision part (drawgrp.c, TECH_DEBT area-5) */
    /* a block outside the area group of the floor's block (drawgrp.c DG_SHARED; every block without one) takes its
     * draw groups from its own collision under the player: at a seam both blocks' collisions overlap (DSR's
     * ConnectCollision pairs, e.g. the Asylum's crow ledge h0054B1 and Firelink), each map shows what its own part
     * shows. Elsewhere such a block keeps the set of the last of its parts the player stood on (ASSUMED, TECH_DEBT
     * world-4). The blocks of the floor's group all follow the floor's part (one mask, as DSR) */
    for (int j = 0; j < MAP_INST; j++) {
        ColHit hj;
        if (j != k && mp_inst_col_block(j) >= 0 && (k < 0 || !dg_area(k) || dg_area(j) != dg_area(k)) &&
            col_floor_block(mp_inst_col_block(j), feet.x, feet.z, feet.y + 0.5f, feet.y - 3, COL_SOLID, &hj))
            dg_floor(j, hj.tri);
    }
    if (k >= 0) {
        const MapInst *m = &mp_inst[k];
        pinst = k;
        if (m->fog_rows && m->fog_ntris == (unsigned)col_block_tris(h.block) && (unsigned)h.tri < m->fog_ntris &&
            m->fog_tri[h.tri] < m->fog_rows) {
            mp_pfog_row = m->fog_row, mp_pfog_rows = m->fog_rows;
            mp_fog_area = m->fog_tri[h.tri];
        }
    }
}

void map_char_light(V3 pos, float amb[3], float sun[3], float dir[3])
{
    float l = sqrtf(mp_fog_now[12] * mp_fog_now[12] + mp_fog_now[13] * mp_fog_now[13] + mp_fog_now[14] * mp_fog_now[14]);
    for (int c = 0; c < 3; c++)
        amb[c] = mp_fog_now[6 + c], sun[c] = mp_fog_now[9 + c], dir[c] = l > 0 ? mp_fog_now[12 + c] / l : c == 1;
    /* the player's own light (DSR attaches the area's PointLightBank row to the player) */
    if (mp_pfog_rows) {
        const FogRow *r = &mp_pfog_row[mp_fog_area];
        float dx = player_feet.x - pos.x, dy = player_feet.y - pos.y, dz = player_feet.z - pos.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz), b = r->pl_begin * 0.1f, e = r->pl_end * 0.1f;
        float att = e > b ? (e - d) / (e - b) : 0;
        if (att > 0)
            for (int c = 0; c < 3; c++)
                amb[c] += r->pl_rgb[c] * (att < 1 ? att : 1);
    }
    fx_char_light(pos, amb);  /* effect lights: the bonfire's flickering point light (src/fx.c) */
}

void map_char_spec(float env_up[3], float env_down[3], float spec_i[3], float spec_dir[3])
{
    float l = sqrtf(mp_fog_now[24] * mp_fog_now[24] + mp_fog_now[25] * mp_fog_now[25] + mp_fog_now[26] * mp_fog_now[26]);
    for (int c = 0; c < 3; c++) {
        env_up[c] = mp_fog_now[15 + c], env_down[c] = mp_fog_now[18 + c], spec_i[c] = mp_fog_now[21 + c];
        spec_dir[c] = l > 0 ? mp_fog_now[24 + c] / l : c == 1;
    }
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
static void fog_terms(float *a, float *b, float *fmin)
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
    edge_a2 = -*b / FAR;
    edge_z = edge_a2 <= *a ? 0 : (*fmin - *b) / edge_a2;
}

/* Foliage chunks hold tufts sorted by rank: draw those whose rank is below the density for the chunk's distance
 * (whole tufts; the ranks are spread evenly between the chunk's lowest and highest). Returns triangles to draw. */
static unsigned grass_tris(const Chunk *c, const float *e)
{
    float d2 = 0;
    for (int k = 0; k < 3; k++) {
        float o = e[k] < c->mn[k] ? c->mn[k] - e[k] : e[k] > c->mx[k] ? e[k] - c->mx[k] : 0;
        d2 += o * o;
    }
    float rho = d2 > GRASS_D0 * GRASS_D0 ? GRASS_D0 * GRASS_D0 / d2 : 1;
    float lo = (c->flags >> 16 & 255) * (1 / 255.0f), hi = (c->flags >> 24) * (1 / 255.0f);
    if (lo >= rho)
        return 0;
    if (hi <= rho)
        return c->tris;
    /* a prefix must end on a qword in all three streams (18 / 12 / 12 bytes per triangle): VIF only skips to the
     * next word after an UNPACK, so leftover vertex data in the last qword would run as VIF codes (the full chunk
     * is zero-padded, i.e. VIF NOPs). So: whole tufts, in steps of a multiple of 8 triangles. */
    unsigned per = c->flags >> 8 & 255, step = per;
    while (step & 7)
        step += per;
    unsigned n = (unsigned)(c->tris * (rho - lo) / (hi - lo) + 0.5f * step) / step * step;
    return n < c->tris ? n : c->tris;
}

static int visible_(const Chunk *c, const float *e, int sky);
#ifdef MAP_DBG  /* debug builds: chunks / tris dropped per step, printed every MAP_DBG frames (MAP_DBG_ALL: no
                 * draw-group, distance, subpixel or foliage culling, the reference render to compare with) */
enum { D_DRAWN, D_DG, D_PVS, D_CELL_FAR, D_CELL_SMALL, D_CELL_VIEW, D_FAR, D_SMALL, D_VIEW, D_GRASS, D_TEX, D_SWAP, D_NR };
static const char *const dbg_name[D_NR] = {"drawn", "dg", "pvs", "cfar", "csmall", "cview", "far", "small", "view", "grass",
                                           "tex", "lod"};
static unsigned dbg_c[D_NR], dbg_t[D_NR];
static int vis_why;  /* visible_'s reason for a 0: D_FAR / D_SMALL / D_VIEW */
/* per chunk (map.bin index, near chunks by their bump.bin number = same numbering): this / last frame's step,
 * 255 = not reached (a near chunk of a cell showing its coarse level); MAP_DBG_FLIP logs the chunks drawn last frame
 * and not now, by step */
#define DBG_IDS 32768
static unsigned char dbg_why[2][DBG_IDS];
static int dbg_cur;
#define DBG(r, n) (dbg_c[r]++, dbg_t[r] += (n))
#define DBGW(r, n, id) (DBG(r, n), (id) < DBG_IDS ? dbg_why[dbg_cur][id] = (r) : 0)
#else
#define DBG(r, n) ((void)0)
#define DBGW(r, n, id) ((void)0)
#endif
int mp_visible(const Chunk *c, const float *e, int sky)
{
    PROF_BEGIN
    MP_BEGIN
    int r = visible_(c, e, sky);
    MP_END(MP_CULL)
    PROF_END(prof_vis)
    return r;
}

/* view depth of box c's nearest (*zmax: farthest) point and its radius squared */
static float box_depth(const Chunk *c, const float *e, float *zmax, float *r2)
{
    float zc = 0, ext = 0, rr = 0;
    for (int k = 0; k < 3; k++) {
        float h = (c->mx[k] - c->mn[k]) * 0.5f;
        zc += cam[9 + k] * (c->mn[k] + h - e[k]);
        ext += fabsf(cam[9 + k]) * h;
        rr += h * h;
    }
    if (zmax)
        *zmax = zc + ext;
    if (r2)
        *r2 = rr;
    return zc - ext;
}

/* the last chunk visible_ measured and its box's farthest view depth (draw_chunk's edge fog test, same eye) */
static const Chunk *vis_c;
static float vis_zmax;
static int emit_close;  /* MAP_TWO_PASS emit: the cell's chunks need no distance / edge fog test */

/* the frustum test of render.c r_box_classify (the box's bounding sphere against the view planes with a 40 px margin,
 * then the near plane and guard band) fused with the distance tests: one view transform of the box centre. Its plane
 * constants for this frame's camera (map_draw: vis_frame) */
static float vc_tx, vc_ty, vc_kx, vc_ky, vc_tg, vc_kg;

static void vis_frame(float fx, float fy)
{
    vc_tx = (R_SCREEN_W / 2 + 40) / fx, vc_ty = (R_SCREEN_H / 2 + 40) / fy;
    vc_kx = 1.0f / sqrtf(1 + vc_tx * vc_tx), vc_ky = 1.0f / sqrtf(1 + vc_ty * vc_ty);
    vc_tg = R_GUARD / fx, vc_kg = 1.0f / sqrtf(1 + vc_tg * vc_tg);
}

static int visible_(const Chunk *c, const float *e, int sky)
{
    float h0 = (c->mx[0] - c->mn[0]) * 0.5f, h1 = (c->mx[1] - c->mn[1]) * 0.5f, h2 = (c->mx[2] - c->mn[2]) * 0.5f;
    float d0 = c->mn[0] + h0 - e[0], d1 = c->mn[1] + h1 - e[1], d2 = c->mn[2] + h2 - e[2];
    float vz = cam[9] * d0 + cam[10] * d1 + cam[11] * d2, r2 = h0 * h0 + h1 * h1 + h2 * h2;
    if (!sky && !(c->flags & CHUNK_BACKDROP)) {  /* beyond FAR the edge fog is complete: blended mist / shafts end there (a fog-coloured card would show
                  * on the sky), opaque pieces at CULL_FAR (a fog-coloured silhouette, like DSR's distant background) */
        float ext = fabsf(cam[9]) * h0 + fabsf(cam[10]) * h1 + fabsf(cam[11]) * h2, z = vz - ext;
        vis_zmax = vz + ext, vis_c = c;
#ifndef MAP_DBG_ALL
        float zcut = c->flags & CHUNK_BLEND ? FAR : CULL_FAR;
        if (z > zcut || (z > SMALL_Z && r2 * cam_f * cam_f < SMALL_PX * SMALL_PX * z * z)) {
#ifdef MAP_DBG
            vis_why = z > zcut ? D_FAR : D_SMALL;
#endif
            return 0;
        }
#else
        (void)z;
#endif
    }
#ifdef MAP_DBG
    vis_why = D_VIEW;
#endif
    float r = sqrtf(r2);
    if (vz < R_NEAR - r)
        return 0;
    float ax = fabsf(cam[3] * d0 + cam[4] * d1 + cam[5] * d2), ay = fabsf(cam[6] * d0 + cam[7] * d1 + cam[8] * d2);
    if ((ax - vc_tx * vz) * vc_kx > r || (ay - vc_ty * vz) * vc_ky > r)
        return 0;
    /* VU1-safe: the whole sphere in front of the near plane and inside the guard band */
    if (vz - r >= R_NEAR && (ax - vc_tg * vz) * vc_kg < -r && (ay - vc_tg * vz) * vc_kg < -r)
        return 1;
    return 2;
}

/* the first n triangles of chunk c (n < c->tris: foliage thinned by distance) */
/* no: the chunk's number in bump.bin / dg.bin (resident: its index in the chunk table; near: NEAR_NO) */
static int draw_chunk_(const MapInst *m, const unsigned char *base, const Chunk *c, unsigned n, int cls, int mode,
                       unsigned no)
{
    GSTEXTURE *t = NULL;
    float d2 = 0;
    if (c->tex >= 0 && c->tex < m->ntex) {  /* mip level from the distance to the chunk's box */
        int tx = m->tex_base + c->tex;
        for (int k = 0; k < 3; k++) {
            float o = cam[k] < c->mn[k] ? c->mn[k] - cam[k] : cam[k] > c->mx[k] ? cam[k] - c->mx[k] : 0;
            d2 += o * o;
        }
        MP_BEGIN
        t = tc_get(tx, tc_level(tx, sqrtf(d2)));
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
        int vclip = cls == 2 && !(c->flags & CHUNK_SKY) ? VU1_CLIP : 0, edge = 0;
        if (!(c->flags & CHUNK_SKY)) {  /* reaches into the edge fog: the edge loop of vu1map2 */
            float zmax;
            if (emit_close)  /* (MAP_TWO_PASS: the cell lies nearer than the edge fog's start) */
                zmax = 0;
            else if (c == vis_c)  /* map_draw's eye is the camera's */
                zmax = vis_zmax;
            else
                box_depth(c, cam, &zmax, NULL);
            edge = zmax > edge_z && !(c->flags & CHUNK_BACKDROP) ? VU1_EDGE : 0;  /* the backdrop: DSR's fog only */
        }
        float st_k = c->flags & CHUNK_ST1K ? 1024.0f : 4096.0f;
        if (!(mode & R_BLEND) && !(c->flags & (CHUNK_SKY | CHUNK_BLEND)) && c->tex >= 0) {  /* GS mip chain (render-12) */
            u64 mt;
            MP_SPLIT(MP_CHAIN);
            int k, mxl = tc_mip(m->tex_base + c->tex, t, st_k, &mt, &k);
            MP_SPLIT(MP_TEX);
            if (mxl)
                vu1_mip(mxl, k, mt);
        }
        int pkt = n == c->tris ? m->pkt : 0;  /* a foliage prefix sends its streams one by one */
        int vm = (mode & R_FOG ? VU1_FOG : 0) | (mode & R_BLEND ? VU1_BLEND : 0) | vclip | edge | pkt;
        if (c->nv)  /* indexed (whole chunks only: foliage, the only prefix drawer, stays in lists) */
            vu1_add_idx(t, c->flags & CHUNK_CULL, vm, n, c->nv, base + c->pos, base + c->st, base + c->col,
                        base + CHUNK_IDX(c), c->org, st_k);
        else
            vu1_add_ex(t, c->flags & CHUNK_CULL, vm, n, base + c->pos, base + c->st, base + c->col, c->org, st_k);
        map_vu1_batches++;
        DBGW(D_DRAWN, 0, no);
        MP_SPLIT(MP_CHAIN);
        if (!(mode & R_BLEND) && !(c->flags & (CHUNK_SKY | CHUNK_BLEND)) && !zs_nobump)  /* normal-map relief pass (bump.c: the boot
                                                                            * block, and a world block while it is full) */
            bump_add((int)(m - mp_inst), no, sqrtf(d2), c->flags & CHUNK_CULL, vclip | pkt, n, c->nv, base + c->pos,
                     base + c->st, base + c->col, c->nv ? base + CHUNK_IDX(c) : NULL, c->org, st_k);
        MP_END(MP_BUMP)
    }
    return n;
}

/* a horizon chunk (untextured, double-sided, its packet without st: vu1.c VU1_FLAT). The clipping program (VU1_CLIP)
 * gets a zero st stream sent on its own: with the packet's two streams alone it hung PCSX2 2 s into the Burg on the
 * first ISO run (2026-11-06; a full st stream on disc, +10 bytes a triangle, ran the route) */
static int draw_hz_(const MapInst *m, const Chunk *c, int cls, int mode, int cull)
{
    static unsigned char zero_st[CHUNK_HZN_TRIS * 12] __attribute__((aligned(16)));
    float zmax;
    box_depth(c, cam, &zmax, NULL);
    int edge = zmax > edge_z && !(c->flags & CHUNK_BACKDROP) ? VU1_EDGE : 0;
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
static int zs_route(const Chunk *c, int *far_cls, int cls)
{
    float zmax, zmin = box_depth(c, cam, &zmax, NULL);
    int r = (zmax > ZSPLIT) | (zmin < ZSPLIT) << 1;
    *far_cls = cls == 1 && zmin >= ZSPLIT * 1.001f ? 1 : 2;
    zs_st[r - 1]++;
    return r;
}

static int draw_chunk(const MapInst *m, const unsigned char *base, const Chunk *c, unsigned n, int cls, int mode,
                      unsigned no)
{
    if (zs_pass != 1 || (c->flags & CHUNK_SKY))  /* the sky (1-3 km): the far pass */
        return draw_chunk_(m, base, c, n, cls, mode, no);
    int fc, r = zs_route(c, &fc, cls), dn = 0;
    if ((r & 2) && zs_room(8)) {
        u32 *w = zs_buf + zs_n;
        w[0] = ZS_CHUNK, w[1] = (u32)(m - mp_inst), w[2] = (u32)c, w[3] = (u32)base, w[4] = n, w[5] = (u32)cls;
        w[6] = (u32)mode, w[7] = no;
        zs_n += 8, dn = (int)n;
    }
    if (r & 1) {
        zs_nobump = 1;
        dn = draw_chunk_(m, base, c, n, fc, mode, no);
        zs_nobump = 0;
    }
    return dn;
}

int mp_draw_hz(const MapInst *m, const Chunk *c, int cls, int mode, int cull)
{
    if (zs_pass != 1)
        return draw_hz_(m, c, cls, mode, cull);
    int fc, r = zs_route(c, &fc, cls), dn = 0;
    if ((r & 2) && zs_room(6)) {
        u32 *w = zs_buf + zs_n;
        w[0] = ZS_HZ, w[1] = (u32)m, w[2] = (u32)c,  /* (m: a map instance, or the far panorama's base, mapblock.c) */ w[3] = (u32)cls, w[4] = (u32)mode, w[5] = (u32)cull;
        zs_n += 6, dn = (int)c->tris;
    }
    if (r & 1)
        dn = draw_hz_(m, c, fc, mode, cull);
    return dn;
}

/* the near pass's Z: everything set to the near mapping's Z at ZSPLIT (colour masked), the far pass's depth is
 * done with; the near pass's pixels beyond ZSPLIT then fail GEQUAL */
static void zs_clear(void)
{
    GSGLOBAL *gs = gsg;
    u64 r[2 * 32];
    int n = 0;
    u64 frame = GS_SETREG_FRAME_1(gs->ScreenBuffer[gs->ActiveBuffer & 1] / 8192, gs->Width / 64, gs->PSM, 0);
    u32 z = (u32)(65535.0f * R_NEAR / (ZSPLIT > 0 ? ZSPLIT : 1));  /* (ZSPLIT=0 builds never call it) */
#define REG(v, a) (r[2 * n] = (v), r[2 * n + 1] = (a), n++)
    REG(frame | 0xFFFFFFFFull << 32, 0x4C);
    REG(GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 1), 0x47);  /* ZTE, ZTST ALWAYS */
    REG(GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0), 0x4E);
    REG(GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0), 0x00);
    for (int x = 0; x < gs->Width && n < 30; x += 64) {
        int x1 = x + 64 < gs->Width ? x + 64 : gs->Width;
        REG(GS_SETREG_XYZ2((x << 4) + gs->OffsetX, gs->OffsetY, z), 0x05);
        REG(GS_SETREG_XYZ2((x1 << 4) + gs->OffsetX, (gs->Height << 4) + gs->OffsetY, z), 0x05);
    }
    REG(frame, 0x4C);
#undef REG
    vu1_gs_regs(n, r);
}

/* map_draw: the far pass from here on (recording the near pass), when there is room to record it */
static void zs_begin(void)
{
    int bytes = 0;
    zs_pass = 0;
    zs_n = zs_lost = 0;
    if (ZSPLIT <= 0 || !(zs_buf = vu1_scratch(&bytes)) || bytes < 4096)
        return;
    zs_cap = (unsigned)bytes / 4;
    zs_pass = 1;
    vu1_map_near(ZSPLIT);
}

/* map_draw: Z reset, the normal near plane, the recorded near pass */
static void zs_end(void)
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
            draw_chunk_(&mp_inst[w[1]], (const unsigned char *)w[3], (const Chunk *)w[2], w[4], (int)w[5], (int)w[6], w[7]);
            i += 8;
            break;
        case ZS_HZ:
            draw_hz_((const MapInst *)w[1], (const Chunk *)w[2], (int)w[3], (int)w[4], (int)w[5]);
            i += 6;
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
    }
    zs_pass = 0;
}

/* instance k has a map and its sky's streams are in (the first sky chunk's item); selects k in mapstream.c */
static int sky_in(int k)
{
    MapInst *m = &mp_inst[k];
    mapstream_select(k);
    if (!m->hdr)
        return 0;
    mp_build_index(m);
    return m->nsky && mp_res_base(m, m->sky[0]);
}

/* map_draw keeps what its later passes draw in these lists while it walks the cells: effect chunks (sorted far to
 * near, drawn after the opaque map), horizons fading in over their cells' coarse level (HZN_FADE) and coarse levels
 * fading out over their near level (NEAR_FADE) */
typedef struct { float d; const Chunk *c; const unsigned char *base; int cls, k; unsigned no; } FxItem;
typedef struct { unsigned char k, a; unsigned short ci; } FadeItem;
static FxItem fxl[MAX_FX];
static FadeItem hzfade[MAX_HZ_FADE], nrfade[MAX_NEAR_FADE];
static unsigned hzx[3][3];  /* cells the horizon stood in for inside HZN_R this frame (log) */
static int nfx, nhzfade, nnrfade;  /* entries of fxl, hzfade, nrfade this frame */

/* One frame's counters for the log lines (log_frame) and the hitch monitor */
typedef struct {
    unsigned holes;  /* visible cells with resident chunks not in and no near level drawn for them */
    unsigned hole_nohz, hole_unc, hole_ex[2];
    unsigned hz_fill, hz_tris;  /* cells inside HZN_R drawn by their horizon (coarse not in); horizon tris */
    unsigned far_tris;          /* of them the far panorama's (blocks that are not loaded) */
    unsigned pend, pops, pop_wait;  /* pop-in: cells drawn coarse while due near; swaps seen; longest wait */
    float pop_d;
    unsigned pvs_cut;  /* triangles the visibility rows left out (src/pvs.c) */
    unsigned k_drawn[MAP_INST], k_cut[MAP_INST];  /* the same and the drawn ones per instance (A/B log) */
} FrameLog;

/* Fog of sky row `row` of m (counted from 1) as vu1_map_fog / r_set_fog terms, F = clamp(a z + b, fmin, 255), and its
 * FOGCOL. DSR's curve as fog_terms: density = saturate(max x saturate((z - begin) / (end - begin))). Row 0 (chunks
 * without a row, and maps exported before the rows): the header's sky fog at its maximum density at every distance. */
static u32 sky_fog(const MapInst *m, unsigned row, float *a, float *b, float *fmin)
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

/* The sky of block sk and the far panorama, then the fog and test registers of the opaque map (fog terms a, b, fmin,
 * colour col). Returns the triangles drawn. */
static int draw_sky(int sk, const float e[3], u32 col, float a, float b, float fmin)
{
    GSGLOBAL *gs = gsg;
    MapInst *m = &mp_inst[sk];
    int drawn = 0;
    u64 r[2] = {0, 0x3D};

    /* sky first, each piece under its own FogBank row as in DSR (SkyRow): opaque, then the alpha clouds. The rows in
     * turn, so the fog constants change once per row and pass at most. The edge fog is the map's, not the sky's. */
    r_set_fog_edge(0);
    for (int pass = 0; pass < 2; pass++) {  /* the player's block's sky (its streams once they are in) */
        if (pass)
            blend_regs(gs, CHUNK_BLEND);
        for (unsigned row = 0; row <= m->sky_rows; row++) {
            int set = 0;
            for (int i = 0; i < m->nsky; i++) {
                const unsigned no = m->sky[i];
                const Chunk *c = &m->chunks[no];
                unsigned cr = CHUNK_SKY_ROW(c->flags);
                if ((cr <= m->sky_rows ? cr : 0) != row)
                    continue;
                if (!(c->flags & CHUNK_BLEND) != !pass || (c->flags & CHUNK_BACKDROP) || !dg_shown(sk, no))
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
                    u32 sky_col = sky_fog(m, row, &sa, &sb2, &smin);
                    zs_map_fog(sa, sb2, smin, 255);
                    r[0] = sky_col;
                    zs_gs_regs(1, r);
                    r_set_fog(sa, sb2, smin, 255, sky_col, col);
                }
                drawn += draw_chunk(m, sb, c, c->tris, cls, R_FOG | (pass ? R_BLEND : 0), no);
            }
        }
    }
    blend_regs(gs, 0);
    /* opaque map with the map's main fog row, and the edge fog; TEST for the batches with a mip chain (vu1_mip: they
     * send MIPTBP1 in its place) */
    zs_map_fog(a, b, fmin, 255);
    zs_map_edge(edge_a2);
    r[0] = col;
    zs_gs_regs(1, r);
    {
        u64 rt[2] = {GS_SETREG_TEST(1, 5, 0x40, 0, 0, 0, 1, 2), 0x47};
        zs_gs_regs(1, rt);
    }
    r_set_fog(a, b, fmin, 255, col, col);
    r_set_fog_edge(edge_a2);
    /* the far backdrop's picture layers (map_pano_open): over the sky, under the map's fog as the pieces they show
     * (the backdrop: DSR's fog only, no edge fog), alpha tested, far layer first */
    if (pano_n && (mp_inst[0].on ? dg_area(pinst) == dg_area(0) : !sk)) {
        zs_map_edge(0);
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
        zs_map_edge(edge_a2);
    }
    return drawn;
}

/* The first n entries of hzfade. Returns the triangles drawn (all horizon triangles). */
static int draw_hz_fades(int n, const float e[3], float a, float b, float fmin)
{
    GSGLOBAL *gs = gsg;
    int drawn = 0;
    /* horizons fading in (HZN_FADE) over their cells' coarse level, Cs a + Cd (1 - a), Z tested HZN_ZBIAS nearer
     * and not written, front faces only: the horizon is drawn double-sided, so a wall's back face (or a layer just
     * behind the coarse surface, now inside the bias) would blend a second time. A depth-only pre-pass did the same
     * but left the horizon's biased depth for the characters and effects drawn after (feet in the ground at
     * ~50 m) */
    zs_map_zbias(1.0f + HZN_ZBIAS);
    zs_map_fog(a, b, fmin, 255);
    zs_map_edge(edge_a2);
    u64 zb = GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0) | (1ull << 32);
    for (int i = 0; i < n; i++) {
        const MapInst *mk = &mp_inst[hzfade[i].k];
        const HzCell *hc = &mk->hz_cell[hzfade[i].ci];
        if (HZN_FADE_DEBUG && !i) {
            u64 fc[2] = {0xFF00FFull, 0x3D};  /* FOGCOL r, g, b */
            zs_gs_regs(1, fc);
            zs_map_fog(a, b, 0, 0);
        }
        u64 r[4] = {GS_SETREG_ALPHA(0, 1, 2, 1, HZN_FADE_DEBUG ? 0x80 : hzfade[i].a), 0x42, zb, 0x4E};
        zs_gs_regs(2, r);
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
    zs_map_zbias(1.0f);
    zs_map_fog(a, b, fmin, 255);
    if (HZN_FADE_DEBUG) {  /* the frame's fog colour back for what is drawn after */
        float f[5];
        u64 fc[2] = {0, 0x3D}, fb;
        r_get_fog(f, &fc[0], &fb);
        zs_gs_regs(1, fc);
    }
    blend_regs(gs, 0);
    return drawn;
}

/* The first n entries of nrfade. Returns the triangles drawn. */
static int draw_near_fades(int n, const float e[3])
{
    GSGLOBAL *gs = gsg;
    int drawn = 0;
    /* coarse levels fading out (NEAR_FADE) over their cells' near level, as the horizon fade above: Cs a + Cd
     * (1 - a), Z tested HZN_ZBIAS nearer and not written (the coarse surface is the near one simplified) */
    zs_map_zbias(1.0f + HZN_ZBIAS);
    u64 zb = GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0) | (1ull << 32);
    for (int i = 0; i < n; i++) {
        const MapInst *mk = &mp_inst[nrfade[i].k];
        const Cell *ce = &mk->cells[nrfade[i].ci];
        u64 r[4] = {GS_SETREG_ALPHA(0, 1, 2, 1, nrfade[i].a), 0x42, zb, 0x4E};
        zs_gs_regs(2, r);
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
                drawn += draw_chunk(mk, base, c, c->tris, cls, R_FOG | R_BLEND, no);
        }
    }
    mapstream_select(0);
    zs_map_zbias(1.0f);
    blend_regs(gs, 0);
    return drawn;
}

/* The first n entries of fxl, after the opaque map: alpha-blended (mist) far to near, then additive (light shafts,
 * order-free); Z tested, not written. Returns the triangles drawn. */
static int draw_effects(int n, int fog)
{
    GSGLOBAL *gs = gsg;
    int drawn = 0;
    for (int i = 1; i < n; i++)  /* insertion sort by distance, far first; a few dozen per frame */
        for (int j = i; j > 0 && fxl[j].d > fxl[j - 1].d; j--) {
            FxItem tmp = fxl[j];
            fxl[j] = fxl[j - 1], fxl[j - 1] = tmp;
        }
    for (int add = 0; add < 2; add++) {
        int any = 0;
        for (int i = 0; i < n; i++) {
            const Chunk *c = fxl[i].c;
            if (!(c->flags & CHUNK_ADD) != !add)
                continue;
            if (!any++)
                blend_regs(gs, CHUNK_BLEND | (add ? CHUNK_ADD : 0));
            drawn += draw_chunk(&mp_inst[fxl[i].k], fxl[i].base, c, c->tris, fxl[i].cls, fog | R_BLEND | (add ? R_ADD : 0),
                                fxl[i].no);
        }
    }
    blend_regs(gs, 0);
    return drawn;
}

#if MAP_PASS_MS
/* "map: pass ms": the breakdown over 30 frames (average, and the frame with the largest map_draw) */
static void log_pass_ms(unsigned mp_t0)
{
    static unsigned fr, nt, acc[MP_N + 1], wst[MP_N + 1], wtot, facc[MF_N];
    unsigned tot = mp_on ? ticks() - mp_t0 : 0, v[MP_N + 1], sub = 0;
    for (int i = 0; i < MF_N; i++)
        facc[i] += mf_t[i];
    nt += mp_on;
    for (int i = 0; i < MP_N; i++)
        v[i] = mp_t[i], sub += mp_t[i];
    v[MP_N] = tot > sub ? tot - sub : 0;  /* plan: the rest */
    for (int i = 0; i <= MP_N; i++)
        acc[i] += v[i];
    if (tot > wtot)
        memcpy(wst, v, sizeof wst), wtot = tot;
    if (!(++fr % MAP_PASS_LOG) && nt) {
#define MPMS(x) ((double)((float)(x) / 294912.0f / (float)nt))
#define MPMS1(x) ((double)((float)(x) / 294912.0f))
        unsigned at = acc[0] + acc[1] + acc[2] + acc[3] + acc[4];
        printf("map: pass ms cull %.2f plan %.2f chain %.2f tex %.2f bump %.2f total %.2f | worst cull %.2f plan "
               "%.2f chain %.2f tex %.2f bump %.2f total %.2f (%u timed frames)\n", MPMS(acc[MP_CULL]),
               MPMS(acc[MP_N]), MPMS(acc[MP_CHAIN]), MPMS(acc[MP_TEX]), MPMS(acc[MP_BUMP]), MPMS(at),
               MPMS1(wst[MP_CULL]), MPMS1(wst[MP_N]), MPMS1(wst[MP_CHAIN]), MPMS1(wst[MP_TEX]), MPMS1(wst[MP_BUMP]),
               MPMS1(wtot), nt);
        printf("map: plan ms miss %.2f near %.2f chunks %.2f fx %.2f other %.2f; per frame tiles %u visible %u, cells %u "
               "visible %u, chunks %u\n", MPMS(facc[MF_MISS]), MPMS(facc[MF_NEAR]),
               MPMS(facc[MF_CHUNKS]), MPMS(facc[MF_FX]), MPMS(acc[MP_N] - facc[0] - facc[1] - facc[2] - facc[3]),
               mc_tiles / MAP_PASS_LOG, mc_tvis / MAP_PASS_LOG, mc_cells / MAP_PASS_LOG, mc_vis / MAP_PASS_LOG,
               mc_chunks / MAP_PASS_LOG);
        mc_cells = mc_vis = mc_tiles = mc_tvis = mc_chunks = 0, nt = 0;
#undef MPMS
#undef MPMS1
        memset(acc, 0, sizeof acc), memset(wst, 0, sizeof wst), wtot = 0, memset(facc, 0, sizeof facc);
    }
}
#endif

/* map_draw's log lines and debug-build reports for one frame */
static void log_frame(const FrameLog *lg, const float e[3], int drawn)
{
#ifdef MAP_PANO_R  /* panorama capture: the camera basis for tools/pano.py, every 8 frames */
    {
        static unsigned pf;
        if (!(++pf & 7))
            printf("PANOCAM %.3f %.3f %.3f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.2f\n", cam[0], cam[1],
                   cam[2], cam[3], cam[4], cam[5], cam[6], cam[7], cam[8], cam[9], cam[10], cam[11], cam_f, 0.0f, 0.0f, 0.0f);
    }
#endif
#ifdef MAP_DBG
    {
        static unsigned fr;
        dbg_t[D_DRAWN] = drawn;
        if (!(++fr % MAP_DBG)) {
            char line[320];
            int k = snprintf(line, sizeof line, "MAPDBG %u", fr);
            for (int r = 0; r < D_NR; r++)
                k += snprintf(line + k, sizeof line - k, " %s %u/%u", dbg_name[r], dbg_c[r], dbg_t[r]);
            puts(line);
        }
        memset(dbg_c, 0, sizeof dbg_c), memset(dbg_t, 0, sizeof dbg_t);
#ifdef MAP_DBG_FLIP
        {  /* chunks drawn last frame, not now: by this frame's step (view = left the frustum: expected when turning) */
            unsigned cnt[D_NR + 1] = {0}, tris[D_NR + 1] = {0}, on = 0;
            const unsigned char *now = dbg_why[dbg_cur], *was = dbg_why[!dbg_cur];
            char line[400];
            int k = snprintf(line, sizeof line, "FLIP %u", fr);
            for (unsigned i = 0; i < DBG_IDS; i++) {
                if (was[i] == D_DRAWN && now[i] != D_DRAWN) {
                    int r = now[i] < D_NR ? now[i] : D_NR;
                    cnt[r]++;
                    if (mp_inst[0].hdr && i < mp_inst[0].hdr->chunks)
                        tris[r] += mp_inst[0].chunks[i].tris;
                    if (r != D_VIEW && r != D_CELL_VIEW && r != D_SWAP && r != D_NR && k < 300)
                        k += snprintf(line + k, sizeof line - k, " [%u %s]", i, dbg_name[r]);
                } else if (was[i] != D_DRAWN && now[i] == D_DRAWN)
                    on++;
            }
            k += snprintf(line + k, sizeof line - k, " | on %u off", on);
            for (int r = 1; r <= D_NR; r++)
                if (cnt[r])
                    k += snprintf(line + k, sizeof line - k, " %s %u/%u", r < D_NR ? dbg_name[r] : "unseen", cnt[r],
                                  tris[r]);
            puts(line);
        }
#endif
        dbg_cur = !dbg_cur;
        memset(dbg_why[dbg_cur], 255, DBG_IDS);
    }
#endif
    {  /* streaming A/B (world streaming v2): the most cells missing in one frame over each second, when any were */
        static unsigned fr, worst, cut_max, pend_max, pops_s, wait_max, hzf_max, hzt_max, w_nohz, unc_max, w_k, w_ci;
        static float near_min = 1e9f;
        if (lg->holes > worst)
            worst = lg->holes, w_nohz = lg->hole_nohz, w_k = lg->hole_ex[0], w_ci = lg->hole_ex[1];
        unc_max = lg->hole_unc > unc_max ? lg->hole_unc : unc_max;
        hitch_map(lg->pend, lg->hz_fill, lg->pops);  /* the hitch monitor's "stream:" line (src/hitch.c) */
        hzf_max = lg->hz_fill > hzf_max ? lg->hz_fill : hzf_max;
        hzt_max = lg->hz_tris > hzt_max ? lg->hz_tris : hzt_max;
        pend_max = lg->pend > pend_max ? lg->pend : pend_max;
        pops_s += lg->pops, pop_total += lg->pops;
        wait_max = lg->pop_wait > wait_max ? lg->pop_wait : wait_max;
        near_min = lg->pop_d < near_min ? lg->pop_d : near_min;
        cut_max = lg->pvs_cut > cut_max ? lg->pvs_cut : cut_max;
        if (!(++fr % 30)) {
#if MAP_TWO_PASS
            if (!(fr % 300))
                printf("map: two-pass lists built %u, pool resets %u, pool %u / %u entries\n", mp_lpool_builds,
                       mp_lpool_resets, mp_lpool_top, LPOOL_N);
#ifdef MAP_TWO_PASS_CHECK
            printf("map: two-pass check %u lists, %u differ\n", mp_chk_lists, mp_chk_bad);
#endif
#endif
            if (worst)
                printf("map: cells missing %u (most in a frame over 1 s; %u without a horizon; e.g. cell %u.%u)\n", worst,
                       w_nohz, w_k, w_ci);
            if (unc_max)
                printf("map: effects not in yet in %u cells (most in a frame over 1 s)\n", unc_max);
            unc_max = 0;
            if (hzf_max)  /* not holes: the horizon stood in for a coarse level not in yet, inside HZN_R */
                printf("map: horizon filled %u cells (most in a frame over 1 s), %u horizon tris; e.g. %u.%u at %u m, %u.%u at %u m\n",
                       hzf_max, hzt_max, hzx[0][0], hzx[0][1], hzx[0][2], hzx[1][0], hzx[1][1], hzx[1][2]);
            hzf_max = hzt_max = 0;
            if (pops_s)
                printf("map: near pop-in %u cells over 1 s (wait max %u frames, nearest %.0f m; %u in the run), %u due "
                       "near drawn coarse (most in a frame)\n", pops_s, wait_max, (double)near_min, pop_total, pend_max);
            pend_max = pops_s = wait_max = 0, near_min = 1e9f;
            if (cut_max)  /* occlusion A/B (src/pvs.c): this frame's per-instance drawn / culled counts */
                printf("map: pvs culled %u tris (most in a frame over 1 s), eye %.1f %.1f %.1f; now drawn / culled %u/%u "
                       "%u/%u %u/%u\n", cut_max, (double)e[0], (double)e[1], (double)e[2], lg->k_drawn[0], lg->k_cut[0],
                       MAP_INST > 1 ? lg->k_drawn[1] : 0, MAP_INST > 1 ? lg->k_cut[1] : 0, MAP_INST > 2 ? lg->k_drawn[2] : 0,
                       MAP_INST > 2 ? lg->k_cut[2] : 0);
            worst = cut_max = 0;
            {  /* the far panorama, when what it draws changes by a tenth (tests read the line) */
                static unsigned far_was;
                unsigned d = lg->far_tris > far_was ? lg->far_tris - far_was : far_was - lg->far_tris;
                if (d && d * 10 >= (far_was > 100 ? far_was : 100)) {
                    printf("map: far panorama %u tris (boot block %s), eye %.1f %.1f %.1f, instance 0 drew %u, backdrop "
                           "pictures %u tris\n", lg->far_tris, mp_inst[0].hdr ? "on" : "let go", (double)e[0],
                           (double)e[1], (double)e[2], lg->k_drawn[0], pano_drawn);
                    far_was = lg->far_tris;
                }
            }
        }
    }
#ifdef MAP_STATS  /* A/B builds: batches, texture pool traffic and streamed cells, every 60 frames */
    {
        static unsigned fr;
        if (!(++fr % 60))
            printf("MAPSTATS batches %d tris %d tc uploads %d bytes %d fails %d stream cells %u KB %u\n",
                   map_vu1_batches, drawn, tc_stats.uploads, tc_stats.bytes, tc_stats.fails, mapstream_stats.cells,
                   mapstream_stats.bytes >> 10);
    }
#endif
#ifdef MAP_PROF
    {
        static unsigned frames, v, c;
        v += prof_vis, c += prof_clip;
        prof_vis = prof_clip = 0;
        if (++frames == 30) {  /* average over 30 frames, in 0.01 ms */
            map_vu1_batches = v / 30 / 2949, map_ee_tris = c / 30 / 2949;
            frames = v = c = 0;
        }
        static int sv, sc;
        if (frames)
            map_vu1_batches = sv, map_ee_tris = sc;
        else
            sv = map_vu1_batches, sc = map_ee_tris;
    }
#endif
}

/* Block k in map_draw: its cells in view, each at the level that is loaded (near, coarse or its horizon), with what
 * the later passes draw collected in fxl / hzfade / nrfade and the frame's counters in lg. A block that is kept only
 * for its horizon draws that alone. Returns the triangles drawn. */
static int draw_block(int k, const float e[3], int fog, FrameLog *lg)
{
    MapInst *m;
    int drawn = 0;
    if (k && !mp_inst[k].on) {
        if (mp_inst[k].hz_only) {  /* a kept-only block: its horizon alone */
            unsigned dn = mp_draw_hz_only(k, e);
            drawn += dn, lg->k_drawn[k] += dn, lg->hz_tris += dn;
        }
        return drawn;
    }
    m = &mp_inst[k];
    mapstream_select(k);  /* mapstream_get / mapstream_res below answer for k */
    if (!m->hdr)
        return drawn;
    {
        MP_BEGIN
        mp_build_index(m);
        MP_END(MP_CULL)
    }
    const Chunk *chunks = m->chunks;
    const MapCell *ctab = m->ctab;
    const unsigned short *order = m->order;
    int ntl = m->tiles ? m->ntiles : 1;
    for (int ti = 0; ti < ntl; ti++) {
    const MapTile *tl = m->tiles ? &m->tiles[ti] : NULL;
    if (tl) {
        Chunk tb = {.flags = tl->flags, .mn = {tl->mn[0], tl->mn[1], tl->mn[2]}, .mx = {tl->mx[0], tl->mx[1], tl->mx[2]}};
        mc_tiles++;
        if (!mp_visible(&tb, e, 0))
            continue;
        mc_tvis++;
    }
    for (int ii = 0, nci = tl ? tl->count : m->ncells; ii < nci; ii++) {
        const int ci = tl ? m->tcell[tl->first + ii] : ii;
        Cell *ce = &m->cells[ci];
        /* the cell's box, tested as opaque (out to CULL_FAR); its blended chunks end at FAR on their own test. A
         * cell holding backdrop chunks is only frustum tested, its other chunks get their own distance test (the
         * cell cut dropped whole far-only cells beyond 250 m: the sky holes and floating wall tops of map-39) */
        Chunk cb = {.flags = ce->flags & CHUNK_BACKDROP, .mn = {ce->mn[0], ce->mn[1], ce->mn[2]},
                    .mx = {ce->mx[0], ce->mx[1], ce->mx[2]}};  /* designated: Chunk's scalar fields change */
        /* the cell's horizon (HZN_R): its triangles went to the cell by centroid and may reach past its box */
        const HzCell *hc = m->hz_cell && ctab && !(ctab[ci].near & CELL_PART) && (m->hz_cell[ci].flags & 1)
                               ? &m->hz_cell[ci] : NULL;
        if (hc && hc->count) {
            cb.flags |= hc->flags & 2 ? CHUNK_BACKDROP : 0;
            for (int a = 0; a < 3; a++)
                cb.mn[a] = hc->mn[a] < cb.mn[a] ? hc->mn[a] : cb.mn[a],
                cb.mx[a] = hc->mx[a] > cb.mx[a] ? hc->mx[a] : cb.mx[a];
        }
        mc_cells++;
        if (!mp_visible(&cb, e, 0)) {
#ifdef MAP_DBG
            for (unsigned j = ce->first; j < ce->first + ce->count; j++)
                DBGW(vis_why - D_FAR + D_CELL_FAR, chunks[order[j]].tris, order[j]);
#endif
            continue;
        }
        mc_vis++;

        /* DSM5: the near level when its blob is loaded (then the coarse chunks are skipped), else the coarse one;
         * a world block's resident chunks once their item's streams are in (mapstream_res) */
        if (ctab && (ctab[ci].near & CELL_PART))
            continue;  /* a later part of the cell before it: drawn with it */
        const unsigned char *nb = NULL, *nbp[NEAR_PARTS] = {0};
        unsigned np = 1, nnear = 0;  /* the cell's near parts (export_map.py NEAR_PART), their chunks */
        int miss = 0;  /* a resident chunk of the cell is not in yet: its near cell draws as far as it is loaded */
        int mcov = 0;  /* ... of those the horizon stands for (hz_covers) */
        int nd = 0;    /* the near level is drawn (every part in, or unseen: pvs.c) */
        int hzf = 0;   /* beyond HZN_R: the horizon is drawn, the covered chunks are not even asked for */
        float hz_d = 0;  /* the cell box's distance (with a horizon) */
        if (hc) {
            float d2 = 0;
            for (int a = 0; a < 3; a++) {
                float o = e[a] < ce->mn[a] ? ce->mn[a] - e[a] : e[a] > ce->mx[a] ? e[a] - ce->mx[a] : 0;
                d2 += o * o;
            }
            float r = ce->hz_on ? HZN_R - HZN_HYST : HZN_R + HZN_HYST;
            hzf = ce->hz_on = d2 > r * r;
            hz_d = sqrtf(d2);
        }
#if MAP_TWO_PASS
        /* the cell's resident items, asked for once each (mapstream_res marks them used, as the chunk loop's
         * per-chunk calls did): those the horizon stands for wholly (RI_COV) not beyond HZN_R */
        const unsigned char *ib[CELL_ITEMS];
        unsigned i0 = 0, ni = 0;
        int two = !m->res_cells || !ce->count;  /* items fit ib: the cached list (else the per-chunk loop) */
        if (!two) {
            i0 = m->citem[order[ce->first]], ni = m->citem[order[ce->first + ce->count - 1]] + 1u - i0;
            two = ni <= CELL_ITEMS;
            for (unsigned t = 0; two && t < ni; t++)
                ib[t] = hzf && (m->rflag[i0 + t] & RI_COV) ? NULL : mapstream_res(i0 + t);
        }
#define MISS_SCAN() (two ? mp_miss_items(m, k, ci, hzf, hc != NULL, ib, i0, ni, &miss, &mcov) \
                     : mp_miss_scan(m, k, ci, hzf, hc != NULL, &miss, &mcov))
#else
#define MISS_SCAN() mp_miss_scan(m, k, ci, hzf, hc != NULL, &miss, &mcov)
#endif
        /* the near switch first (MAP_NEAR_SKIP): a cell inside the near radius needs the resident-miss scan only
         * when its near level is not drawn after all (the scan decides holes and the horizon stand-in, nothing
         * else once the near level draws; its mapstream_res calls are made again by the chunk loop) */
        MF_BEGIN
        int near_on = 0;
        float d2 = 0;  /* (ctab) the near level's distance */
        if (ctab) {
            for (int k = 0; k < 3; k++) {
                float o = e[k] < ce->mn[k] ? ce->mn[k] - e[k] : e[k] > ce->mx[k] ? e[k] - ce->mx[k] : 0;
                d2 += o * o;
            }
            /* without the cell's resident streams its near cells draw as far as they are loaded */
            float r = ce->near_on ? NEAR_DRAW_R + NEAR_HYST : NEAR_DRAW_R - NEAR_HYST;
            if (m->ckeys)  /* the near level's distance: the box clipped to the cell's cube (MS_NEAR_CUBE) */
                d2 = cell_near_d2(&ctab[ci], m->ckeys + 4 * ci, e);
            near_on = r > 0 && d2 < r * r;
        }
        int scanned = !(MAP_NEAR_SKIP && near_on);
        if (scanned) {
            MF_AGAIN
            MISS_SCAN();
            MF_END(MF_MISS)
        }
        if (ctab) {
            MF_BEGIN
            ce->near_on = near_on;
            while (np < NEAR_PARTS && ci + np < (unsigned)m->ncells && (ctab[ci + np].near & CELL_PART))
                np++;
            /* every part asked for (mapstream ranks them up), drawn once all are in: the coarse chunks stand
             * for the whole cell until then */
            int all = ce->near_on || miss;
            for (unsigned p = 0; p < np; p++)
                all &= (nbp[p] = ce->near_on || miss ? mapstream_get(ci + p) : NULL) != NULL ||
                       (all && !pvs_range(k, (unsigned)ci, ctab[ci + p].pad, ctab[ci + p].near & ~CELL_PART, 1));
            /* (a part no chunk of which the camera sees is not read: the near level draws without it) */
            nd = all;
            nb = nd ? nbp[0] : NULL;
            if (!scanned && !nd) {
                MF_END(MF_NEAR)
                MF_AGAIN
                MISS_SCAN();
                MF_END(MF_MISS)
                MF_AGAIN
            }
            if (ce->near_on && (ctab[ci].near & ~CELL_PART)) {
                int was = (unsigned short)(pop_fr - ce->seen) == 1;
                if (!nd) {
                    lg->pend++;
                    if (!ce->waiting)
                        ce->wait0 = pop_fr, ce->waiting = 1;
                } else if (ce->waiting) {
                    unsigned w = (unsigned short)(pop_fr - ce->wait0);
                    if (was && w >= 2) {
                        lg->pops++;
                        lg->pop_wait = w > lg->pop_wait ? w : lg->pop_wait;
                        lg->pop_d = d2 < lg->pop_d * lg->pop_d ? sqrtf(d2) : lg->pop_d;
                    }
                    ce->waiting = 0;
                }
            } else {
                ce->waiting = 0;
            }
            ce->seen = pop_fr;
            for (unsigned p = 0; nd && p < np; p++)
                nnear += ctab[ci + p].near & ~CELL_PART;
            MF_END(MF_NEAR)
        }
        /* the horizon in place of the coarse level: far, or the coarse level not complete (never mixed) */
        int use_hz = hc && !nd && (hzf || mcov);
        {
            /* with the horizon only blended effects (mist, light shafts) can be missing: late, but no hole */
            unsigned h = use_hz ? 0 : miss && !nd && (!hc || mcov);
            lg->holes += h;
            lg->hole_nohz += h && !hc, lg->hole_unc += hc && miss == 2 && !nd;  /* no horizon for the cell; effects late */
            if (h && lg->hole_ex[0] == ~0u)
                lg->hole_ex[0] = (unsigned)k, lg->hole_ex[1] = (unsigned)ci;
        }
        if (use_hz && !hzf && lg->hz_fill < 3)  /* (the log names up to three: instance.cell at distance) */
            hzx[lg->hz_fill][0] = (unsigned)k, hzx[lg->hz_fill][1] = (unsigned)ci, hzx[lg->hz_fill][2] = (unsigned)hz_d;
        lg->hz_fill += use_hz && !hzf;
        if (HZN_FADE > 0 && hc && hc->count && !use_hz && !nd && hz_d > HZN_R - HZN_FADE && nhzfade < MAX_HZ_FADE) {
            /* inside HZN_R, on the coarse level: its horizon over it at alpha (d - (HZN_R - HZN_FADE)) / HZN_FADE,
             * reaching 1 where the hysteresis band starts (HZN_R - HZN_HYST: the swap then shows no change) */
            float t = (hz_d - (HZN_R - HZN_FADE)) / (HZN_FADE - HZN_HYST);
            int al = t >= 1 ? 128 : (int)(t * 128);
            if (al > 0)
                hzfade[nhzfade].k = (unsigned char)k, hzfade[nhzfade].ci = (unsigned short)ci,
                hzfade[nhzfade++].a = (unsigned char)al;
        }
        {  /* level-of-detail flips of a cell in view (hitch.c): near / coarse / horizon / hole */
            unsigned char lv = nd ? 4 : use_hz ? 2 : miss && (!hc || mcov) ? 1 : 3;
            if (NEAR_FADE > 0) {  /* coarse -> near while in view: the coarse level fades out over it */
                if (lv != 4)
                    ce->nfade = 0;
                else if (ce->lv == 3 && (unsigned short)(pop_fr - ce->lv_seen) == 1)
                    ce->nfade = NEAR_FADE;
                if (ce->nfade && nnrfade < MAX_NEAR_FADE) {
                    nrfade[nnrfade].k = (unsigned char)k, nrfade[nnrfade].ci = (unsigned short)ci;
                    nrfade[nnrfade++].a = (unsigned char)(ce->nfade * 128 / (NEAR_FADE + 1));
                }
                ce->nfade -= ce->nfade != 0;
            }
            if (ce->lv && lv != ce->lv && (unsigned short)(pop_fr - ce->lv_seen) == 1) {
                hitch_flip(k, (unsigned)ci, lv == ce->lv_prev && (unsigned short)(pop_fr - ce->lv_fr) <= HITCH_FLOP_FRAMES);
                ce->lv_prev = ce->lv, ce->lv_fr = pop_fr;
            }
            ce->lv = lv, ce->lv_seen = pop_fr;
        }
        MF_AGAIN
#if MAP_TWO_PASS
        if (two && hzf && nd)  /* (the near level beyond HZN_R: the chunk loop asked for every item) */
            for (unsigned t = 0; t < ni; t++)
                if (m->rflag[i0 + t] & RI_COV)
                    ib[t] = mapstream_res(i0 + t);
        unsigned jb = m->streams || m->res_cells ? ce->first : ce->first + ce->count, je = ce->first + ce->count + nnear;
        two = two && je - jb <= LPOOL_N && je - ce->first <= 65535u;
        if (two) {
            /* pre-pass: the cell's list from the last draw if nothing it depends on changed, else built again */
            unsigned h1 = 2166136261u ^ (unsigned)k, h2 = 0x9E3779B9u + (unsigned)ci;
#define LMIX(w) (h1 = (h1 ^ (unsigned)(w)) * 16777619u, h2 = (h2 + (unsigned)(w)) * 0x85EBCA6Bu, h2 ^= h2 >> 13)
            LMIX((unsigned)nd | (unsigned)use_hz << 1 | np << 2);
            LMIX(dg_gen());
            LMIX(pvs_gen(k));
            LMIX((unsigned)m->chunks);
            LMIX((unsigned)m->streams);
            for (unsigned p = 0; nd && p < np; p++)
                LMIX((unsigned)nbp[p]);
            for (unsigned t = 0; t < ni; t++)
                LMIX((unsigned)ib[t]);
#undef LMIX
            if (ce->lgen != mp_lpool_gen || ce->lkey[0] != h1 || ce->lkey[1] != h2) {
                if (mp_lpool_top + (je - jb) > LPOOL_N)  /* full: every list goes, the cells build theirs again */
                    mp_lpool_gen++, mp_lpool_top = 0, mp_lpool_resets++;
                unsigned cut, ln = mp_list_build(m, k, ci, nd, use_hz, ib, i0, nbp, np, jb, je, mp_lpool + mp_lpool_top, &cut);
                ce->lkey[0] = h1, ce->lkey[1] = h2, ce->lgen = mp_lpool_gen, ce->lcut = cut;
                ce->loff = (unsigned short)mp_lpool_top, ce->ln = (unsigned short)ln;
                mp_lpool_top += ln;
                mp_lpool_builds++;
            }
#ifdef MAP_TWO_PASS_CHECK  /* debug: every kept list against a fresh build ("map: two-pass check" line) */
            else if (mp_lpool_top + (je - jb) <= LPOOL_N) {
                unsigned cut, ln = mp_list_build(m, k, ci, nd, use_hz, ib, i0, nbp, np, jb, je, mp_lpool + mp_lpool_top, &cut);
                mp_chk_lists++;
                if (ln != ce->ln || cut != ce->lcut || memcmp(mp_lpool + mp_lpool_top, mp_lpool + ce->loff, ln * 2))
                    mp_chk_bad++;
            }
#endif
            lg->pvs_cut += ce->lcut, lg->k_cut[k] += ce->lcut;
            MF_END(MF_NEAR)
            MF_AGAIN
            /* emit: the list's chunks through the frustum and texture tests into the chain. A cell whose farthest
             * corner is nearer than SMALL_Z and the edge fog's start (its chunks lie in its box) has no chunk the
             * distance tests could cut or the edge fog reach: the frustum test alone (emit_close) */
            float df2 = 0;
            for (int a = 0; a < 3; a++) {
                float u = e[a] - ce->mn[a], v = ce->mx[a] - e[a], w = u > v ? u : v;
                df2 += w * w;
            }
            emit_close = SMALL_Z <= FAR && SMALL_Z <= CULL_FAR && df2 < SMALL_Z * SMALL_Z && edge_z > 0 &&
                         df2 < edge_z * edge_z;
            const unsigned short *ls = mp_lpool + ce->loff;
            const unsigned char *nbb = nd ? nbp[0] : NULL;
            unsigned gid = nd ? ctab[ci].pad : 0, part = 0, pe = nd ? ctab[ci].near : 0;
            for (unsigned li = 0, ln = ce->ln; li < ln; li++) {
                unsigned j = ce->first + ls[li], no;
                const Chunk *c;
                const unsigned char *base;
                if (j < (unsigned)ce->first + ce->count) {
                    no = order[j], c = &chunks[no];
                    base = m->res_cells ? ib[m->citem[no] - i0] : m->streams;
                } else {
                    unsigned nj = j - ce->first - ce->count;
                    while (nj >= pe && part + 1 < np)
                        part++, nbb = nbp[part], gid = ctab[ci + part].pad, pe += ctab[ci + part].near & ~CELL_PART;
                    c = (const Chunk *)nbb + (nj - (pe - (ctab[ci + part].near & ~CELL_PART)));
                    no = c->flags & CHUNK_GRASS ? gid + (unsigned)(c - (const Chunk *)nbb) : c->flags >> 8;
                    base = nbb;
                }
                unsigned n = c->flags & CHUNK_GRASS ? grass_tris(c, e) : c->tris;
                if (!n)
                    continue;
                mc_chunks++;
                int cls;
                if (emit_close) {
                    MP_BEGIN
                    cls = visible_(c, e, 1);  /* (as sky: no distance tests) */
                    MP_END(MP_CULL)
                } else
                    cls = mp_visible(c, e, 0);
                if (!cls)
                    continue;
                if (c->flags & CHUNK_BLEND) {
                    if (nfx < MAX_FX) {
                        float d = 0;
                        for (int a = 0; a < 3; a++) {
                            float mm = (c->mn[a] + c->mx[a]) * 0.5f - e[a];
                            d += mm * mm;
                        }
                        fxl[nfx].d = d, fxl[nfx].c = c, fxl[nfx].k = k, fxl[nfx].cls = cls;
                        fxl[nfx].base = base, fxl[nfx++].no = no;
                    }
                    continue;
                }
                unsigned dn = draw_chunk(m, base, c, n, cls, fog, no);
                drawn += dn, lg->k_drawn[k] += dn;
            }
            emit_close = 0;
        } else
#endif
        {
            unsigned near_gid = nd ? ctab[ci].pad : 0;
            unsigned part = 0, part_end = nd ? ctab[ci].near : 0;  /* the near part of chunk j, its last index + 1 */
            for (unsigned j = m->streams || m->res_cells ? ce->first : ce->first + ce->count;
                 j < ce->first + ce->count + nnear; j++) {
                int is_near = j >= (unsigned)ce->first + ce->count;
                if (use_hz && !is_near && hz_covers(chunks[order[j]].flags))
                    continue;  /* the horizon stands for it */
                const unsigned char *rb = is_near ? NULL : mp_res_base(m, order[j]);
                if (!is_near && !rb)
                    continue;  /* its streams are not in */
                unsigned nj = j - ce->first - ce->count;
                while (is_near && nj >= part_end && part + 1 < np) {  /* into the next part's blob */
                    part++;
                    nb = nbp[part], near_gid = ctab[ci + part].pad;
                    part_end += ctab[ci + part].near & ~CELL_PART;
                }
                if (is_near && !nb)
                    continue;  /* an unseen part that is not in (pvs.c) */
                const Chunk *c = is_near ? (const Chunk *)nb + (nj - (part_end - (ctab[ci + part].near & ~CELL_PART)))
                                         : &chunks[order[j]];
                if (nd && !is_near && (c->flags & CHUNK_COARSE)) {
                    DBGW(D_SWAP, 0, order[j]);
                    continue;
                }
#ifndef MAP_DBG_ALL
                if (!dg_shown(k, is_near ? NEAR_NO(c, nb) : order[j])) {  /* MSB draw groups (drawgrp.c) */
                    DBGW(D_DG, c->tris, is_near ? NEAR_NO(c, nb) : order[j]);
                    continue;
                }
                if (!pvs_chunk(k, (unsigned)ci, is_near ? NEAR_NO(c, nb) : order[j])) {  /* behind something (pvs.c) */
                    DBGW(D_PVS, c->tris, is_near ? NEAR_NO(c, nb) : order[j]);
                    lg->pvs_cut += c->tris, lg->k_cut[k] += c->tris;
                    continue;
                }
                unsigned n = c->flags & CHUNK_GRASS ? grass_tris(c, e) : c->tris;
#ifdef MAP_PANO_R
            {
                float d2 = 0;
                for (int k = 0; k < 3; k++) {
                    float o = e[k] < c->mn[k] ? c->mn[k] - e[k] : e[k] > c->mx[k] ? e[k] - c->mx[k] : 0;
                    d2 += o * o;
                }
                if (d2 < MAP_PANO_R * MAP_PANO_R)
                    n = 0;
            }
#endif
#else
                unsigned n = c->tris;  /* MAP_DBG_ALL: every tuft */
                (void)grass_tris;  /* (the per-chunk loop is its only user in MAP_DBG / MAP_PANO_R builds) */
#endif
                DBG(D_GRASS, c->tris - n);
                if (!n) {
                    DBGW(D_GRASS, 0, is_near ? NEAR_NO(c, nb) : order[j]);
                    continue;
                }
                mc_chunks++;
                int cls = mp_visible(c, e, 0);
                if (!cls) {
#ifdef MAP_DBG
                    DBGW(vis_why, n, is_near ? NEAR_NO(c, nb) : order[j]);
#endif
                    continue;
                }
                if (c->flags & CHUNK_BLEND) {
                    if (nfx < MAX_FX) {
                        float d = 0;
                        for (int k = 0; k < 3; k++) {
                            float m = (c->mn[k] + c->mx[k]) * 0.5f - e[k];
                            d += m * m;
                        }
                        fxl[nfx].d = d, fxl[nfx].c = c, fxl[nfx].k = k, fxl[nfx].cls = cls;
                        fxl[nfx].base = is_near ? nb : rb, fxl[nfx++].no = is_near ? NEAR_NO(c, nb) : order[j];
                    }
                    continue;
                }
                {
                    unsigned dn = draw_chunk(m, is_near ? nb : rb, c, n, cls, fog, is_near ? NEAR_NO(c, nb) : order[j]);
                    drawn += dn, lg->k_drawn[k] += dn;
                }
            }
        }
        MF_END(MF_CHUNKS)
        for (unsigned j = 0; use_hz && j < hc->count; j++) {  /* the horizon (no pvs rows: merged) */
            const Chunk *c = &m->hz_chunk[hc->first + j];
            /* one mesh per draw-group set (horizon.py, flags >> 16 = dg.bin piece mask; 0 always drawn): only
             * the sets the floor's groups show, as the chunks it stands for */
            if (!dg_mask_shown(k, (int)(c->flags >> 16 & 0xFF)))
                continue;
            int cls = mp_visible(c, e, 0);
            if (cls) {
                unsigned dn = mp_draw_hz(m, c, cls, 0, 0);
                drawn += dn, lg->k_drawn[k] += dn, lg->hz_tris += dn;
            }
        }
    }
    }
    return drawn;
}

int map_draw(V3 eye)
{
    int drawn = 0;
    static unsigned mp_fr;
    mp_on = MAP_PASS_MS && !(mp_fr++ % MAP_PASS_EVERY);
    unsigned mp_t0 = mp_on ? ticks() : 0;
    (void)mp_t0;
    memset(mp_t, 0, sizeof mp_t), memset(mf_t, 0, sizeof mf_t);
    if (!mp_inst[pinst].on && pinst)
        pinst = 0;
    vis_c = NULL;
    /* sky and fog colours from the player's block; while its sky streams are not in yet the sky of one that has them */
    int sk = pinst;
    if (!sky_in(pinst))
        for (int k = 0; k < MAP_INST; k++)
            if ((k == 0 || mp_inst[k].on) && sky_in(k)) {
                sk = k;
                break;
            }
    MapInst *m = &mp_inst[sk];
    mapstream_select(sk);
    if (m->hdr) {  /* (none: the boot block let go and no other block's map on yet, world.c WORLD_BOOT_LEAN) */
        MP_BEGIN
        mp_build_index(m);
        MP_END(MP_CULL)
    }
    bump_owner(pinst);  /* relief for the player's block (bump.c: one CLUT area) */
    map_vu1_batches = map_ee_tris = 0;
#ifdef MAP_DBG
    static int dbg_init;
    if (!dbg_init++)
        memset(dbg_why, 255, sizeof dbg_why);
#endif
    const float e[3] = {eye.x, eye.y, eye.z};
    {
        MP_BEGIN
        pvs_eye(e);  /* every instance's visibility row for this camera cell (src/pvs.c) */
        MP_END(MP_CULL)
    }
    {
        V3 ey, rt, up, fw;
        float fx, fy, zs;
        r_camera_basis(&ey, &rt, &up, &fw, &fx, &fy, &zs);
        const float b[12] = {ey.x, ey.y, ey.z, rt.x, rt.y, rt.z, up.x, up.y, up.z, fw.x, fw.y, fw.z};
        memcpy(cam, b, sizeof cam);
        cam_f = fx;
        vis_frame(fx, fy);
        MP_BEGIN
        tc_begin(fx);  /* this frame's texture uploads go into the chain from here on */
        MP_END(MP_TEX)
    }
    zs_begin();
    float a, b, fmin;
    fog_terms(&a, &b, &fmin);
    int fog = R_FOG;  /* the edge fog needs FGE even where the row has no fog */
    u32 col = (u32)mp_fog_now[3] | (u32)mp_fog_now[4] << 8 | (u32)mp_fog_now[5] << 16;
    pano_drawn = 0;
    drawn += draw_sky(sk, e, col, a, b, fmin);
    FrameLog lg = {.hole_ex = {~0u, 0}, .pop_d = 1e9f};
    pop_fr++;
    nfx = nhzfade = nnrfade = 0;
    for (int k = 0; k < MAP_INST; k++)  /* every loaded block (src/world.c); one instance in a single-area build */
        drawn += draw_block(k, e, fog, &lg);
    {  /* the far panorama: the blocks that are not loaded, as seen from the player's block (mapblock.c) */
        int dn = (int)mp_draw_far(e);
        drawn += dn, lg.hz_tris += dn, lg.far_tris += dn;
    }
    mapstream_select(0);
    if (nhzfade) {
        int dn = draw_hz_fades(nhzfade, e, a, b, fmin);
        drawn += dn, lg.hz_tris += dn;
    }
    if (nnrfade)
        drawn += draw_near_fades(nnrfade, e);
    {
        MP_BEGIN
        if (zs_pass == 1) {  /* (ZSPLIT: in the near pass) */
            if (zs_room(1))
                zs_buf[zs_n++] = ZS_BUMP;
        } else
            bump_draw();  /* relief over the opaque map (bump.c), before the blended effects */
        MP_END(MP_BUMP)
    }

    MF_BEGIN
    drawn += draw_effects(nfx, fog);
    MF_END(MF_FX)
    zs_end();
    {
        MP_BEGIN
        tc_end();
        MP_END(MP_TEX)
    }
#if MAP_PASS_MS
    log_pass_ms(mp_t0);
#endif
    log_frame(&lg, e, drawn);
    return drawn;
}

int map_floor(float x, float z, float y, float *out_y)
{
    const Header *hdr = mp_inst[0].hdr ? mp_inst[0].hdr : &mp_boot_hdr;  /* (the navmesh stays with the block let go) */
    const float *navv = mp_inst[0].navv;
    const unsigned short *navt = mp_inst[0].navt;
    int found = 0;
    float best = 0;
    for (unsigned i = 0; i < hdr->nav_tris; i++) {
        const float *a = &navv[navt[i * 3] * 3], *b = &navv[navt[i * 3 + 1] * 3], *c = &navv[navt[i * 3 + 2] * 3];
        /* barycentric test in the x/z plane */
        float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
        if (d > -1e-6f && d < 1e-6f)
            continue;
        float l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
        float l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
        float l3 = 1 - l1 - l2;
        if (l1 < 0 || l2 < 0 || l3 < 0)
            continue;
        float h = l1 * a[1] + l2 * b[1] + l3 * c[1];
        if (h > y + STEP_UP || h < y - STEP_DOWN)
            continue;
        if (!found || (h - y) * (h - y) < (best - y) * (best - y))
            best = h, found = 1;
    }
    if (found)
        *out_y = best;
    return found;
}
