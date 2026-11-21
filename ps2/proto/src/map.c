/* Level drawing: map.bin load and textures, each block's cells at the level that is in (near, coarse, horizon) and
 * the order of passes (map_draw_sky / _pano / _world). Visibility is mapvis.c, fog and light maplight.c, chunk
 * drawing and ZSPLIT mapchunk.c, the sky / fades / effects mappass.c, timers and logs maplog.c. */
#include "map_i.h"
#include "mapvis.h"
#include "maplight.h"
#include "mapchunk.h"
#include "mappass.h"
#include "maplog.h"
#include "world.h"
#include "material.h"
int map_vu1_batches, map_ee_tris;
MapInst mp_inst[MAP_INST] = {{.on = 0}};
GSGLOBAL *mp_gs;
const unsigned char *mp_res_base(const MapInst *m, unsigned j)
{
    return m->res_cells ? mapstream_res(m->citem[j]) : m->streams;
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
    mp_gs = gs;
    MapInst *m = &mp_inst[0];
    m->ntex = m->hdr->textures < MAX_TEX ? m->hdr->textures : MAX_TEX;
    m->matcls = m->ctab && ((const unsigned *)(m->cel_bin + m->hdr->cell_off))[1] & MAT_CELLS_FLAG;
    mat_census(0, m->tex_bin + m->hdr->tex_off, m->ntex, m->chunks, m->hdr->chunks, sizeof(Chunk), m->matcls);
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

/* The boot map's spawn point, kept: the world build lets the boot block's tables go (world.c WORLD_BOOT_LEAN), and a
 * fall onto a kill plane respawns here after that (player.c respawn read a NULL header: TLB misses at 0x14..0x20 and
 * the player at 0 0 0, 2026-11-16) */
void map_spawn(V3 *pos, float *yaw)
{
    static V3 kept_pos;
    static float kept_yaw;
    static int kept;
    const Header *hdr = mp_inst[0].hdr;
    if (hdr) {
        kept_pos = v3(hdr->spawn[0], hdr->spawn[1], hdr->spawn[2]);
        kept_yaw = hdr->spawn_yaw;
        kept = 1;
    }
    if (kept)
        *pos = kept_pos, *yaw = kept_yaw;
}

int map_stream_open(const char *pak_path)
{
    map_pano_open(pak_path);
    mapstream_vis_hooks(mp_vis, mp_vis_gen);
    return mp_inst[0].ctab ? mapstream_init(pak_path, mp_inst[0].ctab, mp_inst[0].nctab) : 0;
}

#ifndef MAP_PEER_ZBIAS
#define MAP_PEER_ZBIAS 0.02f  /* depth push of other maps' blocks (map_draw; ASSUMED: about the coarse level's error, 12 px) */
#endif

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
/* The level chain (Polaris 4.2): backdrop (the void ring, always), far panorama, horizon, coarse, near. A block is
 * drawn at the finest of its levels that is in memory (mp_block_level: its cells, its horizon alone, or the far
 * panorama of the block underfoot stands in for it, mapblock.c mp_draw_far); within a block whose cells are in, each
 * cell at the finest level that is wanted at its distance and complete in memory (cell_level). A finer level that is
 * complete stands in for a coarser one that is not (the near level for a coarse level still reading: the same pieces
 * at full detail); a level whose parent is gone is never left as the only cover, because a coarser level that is in
 * takes its place. Nothing complete: the coarse level as far as it is in, a hole (counted, "map: cells missing"). */
enum { LV_HOLE = 1, LV_HZN = 2, LV_COARSE = 3, LV_NEAR = 4 };  /* (hitch.c's level codes) */
/* near_in: the near level is wanted (or stands in) and every part the camera sees is in; has_hz: the cell has a
 * horizon (in with the block's lite set); hzf: beyond HZN_R; miss / mcov: mp_miss_scan's resident chunks not in
 * (any / ones the horizon stands for) */
static inline int cell_level(int near_in, int has_hz, int hzf, int miss, int mcov)
{
    if (near_in)
        return LV_NEAR;
    if (has_hz && (hzf || mcov))
        return LV_HZN;
    return miss && !has_hz ? LV_HOLE : LV_COARSE;  /* (with a horizon only blended effects can be late: no hole) */
}

/* Block k in map_draw: its cells in view, each at its level (cell_level), with what the later passes draw collected in
 * the effect / fade lists and the frame's counters in lg; at the horizon level, its horizon alone. Returns the
 * triangles drawn. */
static int draw_block(int k, const float e[3], int fog, FrameLog *lg)
{
    MapInst *m;
    int drawn = 0;
    int bl = mp_block_level(k);
    if (bl == MB_HZN) {
        unsigned dn = mp_draw_hz_only(k, e);
        drawn += dn, lg->k_drawn[k] += dn, lg->hz_tris += dn;
    }
    if (bl != MB_CELLS)
        return drawn;
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
        mp_mc_tiles++;
        if (!mp_visible(&tb, e, 0))
            continue;
        mp_mc_tvis++;
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
        mp_mc_cells++;
        if (!mp_visible(&cb, e, 0)) {
#ifdef MAP_DBG
            for (unsigned j = ce->first; j < ce->first + ce->count; j++)
                DBGW(mp_vis_why - D_FAR + D_CELL_FAR, chunks[order[j]].tris, order[j]);
#endif
            continue;
        }
        mp_mc_vis++;

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
                int was = (unsigned short)(mp_pop_fr - ce->seen) == 1;
                if (!nd) {
                    lg->pend++;
                    if (!ce->waiting)
                        ce->wait0 = mp_pop_fr, ce->waiting = 1, ce->win = 0, memset(ce->why, 0, sizeof ce->why);
                    ce->win += ce->win < 0xFFFF;  /* (in view this draw) */
                    int why = mapstream_why((unsigned)ci);
                    if (why > 0 && why <= 5 && ce->why[why - 1] < 255)
                        ce->why[why - 1]++;
                } else if (ce->waiting) {
                    unsigned w = (unsigned short)(mp_pop_fr - ce->wait0);
                    if (was && w >= 2) {
                        lg->pops++;
                        /* the wait in view (it used to count the time out of view too: a cell that waited a frame,
                         * left the view and came back 30 s later read as a 900-frame wait) */
                        lg->pop_wait = ce->win > lg->pop_wait ? ce->win : lg->pop_wait;
                        lg->pop_d = d2 < lg->pop_d * lg->pop_d ? sqrtf(d2) : lg->pop_d;
                        mp_pop_note(k, ci, ce, sqrtf(d2), w);
                    }
                    ce->waiting = 0;
                }
            } else {
                ce->waiting = 0;
            }
            ce->seen = mp_pop_fr;
            for (unsigned p = 0; nd && p < np; p++)
                nnear += ctab[ci + p].near & ~CELL_PART;
            MF_END(MF_NEAR)
        }
        /* the cell's level (the level chain above); the horizon is never mixed with the coarse level */
        const int lv = cell_level(nd, hc != NULL, hzf, miss, mcov);
        int use_hz = lv == LV_HZN;
        {
            unsigned h = lv == LV_HOLE;
            lg->holes += h;
            lg->hole_nohz += h && !hc, lg->hole_unc += hc && miss == 2 && !nd;  /* no horizon for the cell; effects late */
            if (h && lg->hole_ex[0] == ~0u)
                lg->hole_ex[0] = (unsigned)k, lg->hole_ex[1] = (unsigned)ci;
        }
        if (use_hz && !hzf && lg->hz_fill < 3)  /* (the log names up to three: instance.cell at distance) */
            mp_hzx[lg->hz_fill][0] = (unsigned)k, mp_hzx[lg->hz_fill][1] = (unsigned)ci, mp_hzx[lg->hz_fill][2] = (unsigned)hz_d;
        lg->hz_fill += use_hz && !hzf;
        if (HZN_FADE > 0 && hc && hc->count && !use_hz && !nd && hz_d > HZN_R - HZN_FADE) {
            /* inside HZN_R, on the coarse level: its horizon over it at alpha (d - (HZN_R - HZN_FADE)) / HZN_FADE,
             * reaching 1 where the hysteresis band starts (HZN_R - HZN_HYST: the swap then shows no change) */
            float t = (hz_d - (HZN_R - HZN_FADE)) / (HZN_FADE - HZN_HYST);
            int al = t >= 1 ? 128 : (int)(t * 128);
            if (al > 0)
                mp_hzfade_add(k, ci, al);
        }
        {  /* level-of-detail flips of a cell in view (hitch.c): near / coarse / horizon / hole */
            if (NEAR_FADE > 0) {  /* coarse -> near while in view: the coarse level fades out over it */
                if (lv != LV_NEAR)
                    ce->nfade = 0;
                else if (ce->lv == LV_COARSE && (unsigned short)(mp_pop_fr - ce->lv_seen) == 1)
                    ce->nfade = NEAR_FADE;
                if (ce->nfade)
                    mp_nrfade_add(k, ci, ce->nfade * 128 / (NEAR_FADE + 1));
                ce->nfade -= ce->nfade != 0;
            }
            if (ce->lv && lv != ce->lv && (unsigned short)(mp_pop_fr - ce->lv_seen) == 1) {
                hitch_flip(k, (unsigned)ci, lv == ce->lv_prev && (unsigned short)(mp_pop_fr - ce->lv_fr) <= HITCH_FLOP_FRAMES);
                ce->lv_prev = ce->lv, ce->lv_fr = mp_pop_fr;
            }
            ce->lv = lv, ce->lv_seen = mp_pop_fr;
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
             * distance tests could cut or the edge fog reach: the frustum test alone (mp_emit_close) */
            float df2 = 0;
            for (int a = 0; a < 3; a++) {
                float u = e[a] - ce->mn[a], v = ce->mx[a] - e[a], w = u > v ? u : v;
                df2 += w * w;
            }
            mp_emit_close = SMALL_Z <= FAR && SMALL_Z <= CULL_FAR && df2 < SMALL_Z * SMALL_Z && mp_edge_z > 0 &&
                         df2 < mp_edge_z * mp_edge_z;
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
                unsigned n = c->flags & CHUNK_GRASS ? mp_grass_tris(c, e) : c->tris;
                if (!n)
                    continue;
                mp_mc_chunks++;
                int cls;
                if (mp_emit_close) {
                    MP_BEGIN
                    cls = mp_vis_test(c, e, 1);  /* (as sky: no distance tests) */
                    MP_END(MP_CULL)
                } else
                    cls = mp_visible(c, e, 0);
                if (!cls)
                    continue;
                if (c->flags & CHUNK_BLEND) {
                    mp_fx_add(c, e, k, cls, base, no);
                    continue;
                }
                unsigned dn = mp_draw_chunk(m, base, c, n, cls, fog, no);
                drawn += dn, lg->k_drawn[k] += dn;
            }
            mp_emit_close = 0;
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
                unsigned n = c->flags & CHUNK_GRASS ? mp_grass_tris(c, e) : c->tris;
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
                (void)mp_grass_tris;  /* (the per-chunk loop is its only user in MAP_DBG / MAP_PANO_R builds) */
#endif
                DBG(D_GRASS, c->tris - n);
                if (!n) {
                    DBGW(D_GRASS, 0, is_near ? NEAR_NO(c, nb) : order[j]);
                    continue;
                }
                mp_mc_chunks++;
                int cls = mp_visible(c, e, 0);
                if (!cls) {
#ifdef MAP_DBG
                    DBGW(mp_vis_why, n, is_near ? NEAR_NO(c, nb) : order[j]);
#endif
                    continue;
                }
                if (c->flags & CHUNK_BLEND) {
                    mp_fx_add(c, e, k, cls, is_near ? nb : rb, is_near ? NEAR_NO(c, nb) : order[j]);
                    continue;
                }
                {
                    unsigned dn = mp_draw_chunk(m, is_near ? nb : rb, c, n, cls, fog, is_near ? NEAR_NO(c, nb) : order[j]);
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

/* One map draw, in the order table's buckets (frame.c: a segment each, back to back in the map chain buffer, so they
 * go out in one kick as before): map_draw_sky (OT_SKY: the frame's setup, the sky, the void ring), map_draw_pano
 * (OT_PANO: the boot map's backdrop pictures, the far panorama of the blocks that are not loaded), map_draw_world
 * (OT_HORIZON..OT_WORLD: each block's cells at the level that is in, the fades, relief and the blended chunks beyond
 * ZSPLIT, then the ZSPLIT near pass). The blended chunks nearer than ZSPLIT come after the characters (map_draw_blend,
 * OT_BLEND). Within a cell the horizon, coarse and near levels interleave, so HORIZON and WORLD are one segment; the
 * map's alpha-tested pieces draw with the opaque ones (no blending: order-free). */
static struct {
    float e[3], a, b, fmin;
    u32 col;
    int sk, drawn;
    unsigned mp_t0;
    FrameLog lg;
} fr;

int map_draw_sky(V3 eye)
{
    fr.drawn = 0;
    static unsigned mp_fr;
    mp_on = MAP_PASS_MS && !(mp_fr++ % MAP_PASS_EVERY);
    fr.mp_t0 = mp_on ? ticks() : 0;
    memset(mp_t, 0, sizeof mp_t), memset(mf_t, 0, sizeof mf_t);
    if (!mp_inst[mp_pinst].on && mp_pinst)
        mp_pinst = 0;
    /* sky and fog colours from the player's block; while its sky streams are not in yet the sky of one that has them */
    int sk = mp_pinst;
    if (!sky_in(mp_pinst))
        for (int k = 0; k < MAP_INST; k++)
            if ((k == 0 || mp_inst[k].on) && sky_in(k)) {
                sk = k;
                break;
            }
    fr.sk = sk;
    MapInst *m = &mp_inst[sk];
    mapstream_select(sk);
    if (m->hdr) {  /* (none: the boot block let go and no other block's map on yet, world.c WORLD_BOOT_LEAN) */
        MP_BEGIN
        mp_build_index(m);
        MP_END(MP_CULL)
    }
    bump_owner(mp_pinst);  /* relief for the player's block (bump.c: one CLUT area) */
    mp_pl_begin();
    map_vu1_batches = map_ee_tris = 0;
#ifdef MAP_DBG
    mp_dbg_begin();
#endif
    fr.e[0] = eye.x, fr.e[1] = eye.y, fr.e[2] = eye.z;
    const float *e = fr.e;
    {
        MP_BEGIN
        pvs_eye(e);  /* every instance's visibility row for this camera cell (src/pvs.c) */
        MP_END(MP_CULL)
    }
    mp_cam_set();
    {
        MP_BEGIN
        tc_begin(mp_cam_f);  /* this frame's texture uploads go into the chain from here on */
        MP_END(MP_TEX)
    }
    mp_zs_begin();
    mp_fog_terms(&fr.a, &fr.b, &fr.fmin);
    fr.col = (u32)mp_fog_now[3] | (u32)mp_fog_now[4] << 8 | (u32)mp_fog_now[5] << 16;
    mp_pass_reset();  /* before the sky pass: the pictures count pano_drawn (the far panorama log line) */
    int dn = mp_draw_sky(sk, e, fr.col, fr.a, fr.b, fr.fmin);
    fr.drawn += dn;
    return dn;
}

int map_draw_pano(void)
{
    int dn = mp_draw_pictures(fr.sk, fr.e);
    fr.lg = (FrameLog){.hole_ex = {~0u, 0}, .pop_d = 1e9f};
    /* the far panorama: the blocks that are not loaded, as seen from the player's block (mapblock.c) */
    int fd = (int)mp_draw_far(fr.e);
    fr.lg.hz_tris += fd, fr.lg.far_tris += fd;
    fr.drawn += dn + fd;
    return dn + fd;
}

int map_draw_world(void)
{
    const float *e = fr.e;
    int drawn = 0;
    int fog = R_FOG;  /* the edge fog needs FGE even where the row has no fog */
    FrameLog *lg = &fr.lg;
    mp_pop_fr++;
    if (mapstream_settling())
        mp_pop_settle = mp_pop_fr;
    for (int k = 0; k < MAP_INST; k++) {  /* every loaded block (src/world.c); one instance in a single-area build */
        /* MAP_PEER_ZBIAS: another map's block is tested PEER_ZBIAS further away. Neighbouring maps carry the same
         * pieces where they meet (Firelink's aqueduct and cliff walls are in m10_01 too: ~14k m2 of coplanar
         * surface within 70 m of the Firelink stairs that DSR's draw groups show in both), and each block is
         * simplified on its own, so the two copies fought (Ame's report 2026-11-17 21:31, t 61.1 / 80.1:
         * "z-fighting with undead burg"). The map the player stands in wins; the other's copy shows where the
         * player's has nothing. 0 = no bias */
        int peer = MAP_PEER_ZBIAS > 0 && k != mp_pinst && (k == 0 || mp_inst[k].on) && !dg_same_map(k, mp_pinst);
        if (peer)
            mp_zs_zbias(1.0f - MAP_PEER_ZBIAS);
        drawn += draw_block(k, e, fog, lg);
        if (peer)
            mp_zs_zbias(1.0f);
    }
    mapstream_select(0);
    {
        int dn = mp_draw_hz_fades(e, fr.a, fr.b, fr.fmin);
        drawn += dn, lg->hz_tris += dn;
    }
    drawn += mp_draw_near_fades(e);
    {
        MP_BEGIN
        mp_zs_relief();  /* relief, the player's light and the specular pass over the opaque map */
        MP_END(MP_BUMP)
    }

    MF_BEGIN
    drawn += mp_draw_effects(fog, MAP_BLEND_LATE ? 1 : 0, fr.col);  /* the rest after the characters: map_draw_blend */
    MF_END(MF_FX)
    mp_zs_end();
    {
        MP_BEGIN
        tc_end();
        MP_END(MP_TEX)
    }
    fr.drawn += drawn;
#if MAP_PASS_MS
    mp_log_pass_ms(fr.mp_t0);
#endif
    mp_log_frame(lg, e, fr.drawn);
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
