/* map.c's world-block side (split 2026-11-12, code moved as it was): a MapInst per loaded block (map_block_*),
 * its cell and tile indexes for culling, the miss scan, the resident items (res_open) and the block stream. map.c
 * draws them; map_i.h has the types and state both use. */
#include "map_i.h"

/* index sort key: cell, then texture inside a cell (so neighbours share TEX0 / CLUT), then chunk number */
static int cell_key_cmp(const void *a, const void *b)
{
    const unsigned *x = a, *y = b;
    if (x[0] != y[0])
        return x[0] < y[0] ? -1 : 1;
    int tx = (int)x[1], ty = (int)y[1];
    return tx != ty ? (tx < ty ? -1 : 1) : (x[2] < y[2] ? -1 : x[2] > y[2]);
}

/* the box map_draw tests cell ci with: the cell's, grown by its horizon's (flags CHUNK_BACKDROP when either has it) */
static void cell_cull_box(const MapInst *m, int ci, Chunk *cb)
{
    const Cell *ce = &m->cells[ci];
    const MapCell *ctab = m->ctab;
    memset(cb, 0, sizeof *cb);
    cb->flags = ce->flags & CHUNK_BACKDROP;
    memcpy(cb->mn, ce->mn, sizeof cb->mn), memcpy(cb->mx, ce->mx, sizeof cb->mx);
    const HzCell *hc = m->hz_cell && ctab && !(ctab[ci].near & CELL_PART) && (m->hz_cell[ci].flags & 1)
                           ? &m->hz_cell[ci] : NULL;
    if (hc && hc->count) {
        cb->flags |= hc->flags & 2 ? CHUNK_BACKDROP : 0;
        for (int a = 0; a < 3; a++)
            cb->mn[a] = hc->mn[a] < cb->mn[a] ? hc->mn[a] : cb->mn[a],
            cb->mx[a] = hc->mx[a] > cb->mx[a] ? hc->mx[a] : cb->mx[a];
    }
}

static int tile_key_cmp(const void *a, const void *b)
{
    const unsigned *x = a, *y = b;
    return x[0] != y[0] ? (x[0] < y[0] ? -1 : 1) : (x[1] < y[1] ? -1 : x[1] > y[1]);
}

/* MAP_TILES: the lead cells grouped by tile, in cell order inside a tile */
static void tile_index(MapInst *mi)
{
    free(mi->tiles), free(mi->tcell);
    mi->tiles = NULL, mi->tcell = NULL, mi->ntiles = 0;
    if (!MAP_TILES || mi->ncells <= 0)
        return;
    unsigned (*key)[2] = malloc(mi->ncells * sizeof *key);
    mi->tcell = malloc(mi->ncells * sizeof *mi->tcell);
    mi->tiles = malloc(mi->ncells * sizeof *mi->tiles);
    if (!key || !mi->tcell || !mi->tiles) {
        free(key), free(mi->tiles), free(mi->tcell);
        mi->tiles = NULL, mi->tcell = NULL;
        return;
    }
    int n = 0;
    for (int ci = 0; ci < mi->ncells; ci++) {
        if (mi->ctab && (mi->ctab[ci].near & CELL_PART))
            continue;  /* drawn with the cell before it */
        const Cell *ce = &mi->cells[ci];
        int tx = (int)floorf((ce->mn[0] + ce->mx[0]) * 0.5f / MAP_TILE_M) + 2048;
        int tz = (int)floorf((ce->mn[2] + ce->mx[2]) * 0.5f / MAP_TILE_M) + 2048;
        key[n][0] = (unsigned)(tx & 4095) << 12 | (unsigned)(tz & 4095), key[n++][1] = (unsigned)ci;
    }
    qsort(key, n, sizeof *key, tile_key_cmp);
    int nt = 0;
    for (int j = 0; j < n; j++) {
        Chunk cb;
        cell_cull_box(mi, (int)key[j][1], &cb);
        if (!j || key[j][0] != key[j - 1][0]) {
            MapTile *t = &mi->tiles[nt++];
            memcpy(t->mn, cb.mn, sizeof t->mn), memcpy(t->mx, cb.mx, sizeof t->mx);
            t->first = (unsigned short)j, t->count = 0, t->flags = 0;
        }
        MapTile *t = &mi->tiles[nt - 1];
        t->count++;
        t->flags |= cb.flags & CHUNK_BACKDROP;
        for (int a = 0; a < 3; a++)
            t->mn[a] = cb.mn[a] < t->mn[a] ? cb.mn[a] : t->mn[a], t->mx[a] = cb.mx[a] > t->mx[a] ? cb.mx[a] : t->mx[a];
        mi->tcell[j] = (unsigned short)key[j][1];
    }
    free(key);
    mi->ntiles = nt;
}

/* a visible cell's resident chunks not in yet (*miss 1; 2: one the horizon does not stand for, with a horizon; *mcov:
 * one it does), skipping those the horizon stands for beyond HZN_R (hzf) */
void mp_miss_scan(const MapInst *m, int k, int ci, int hzf, int has_hz, int *miss, int *mcov)
{
    const Cell *ce = &m->cells[ci];
    const Chunk *chunks = m->chunks;
    const unsigned short *order = m->order;
    for (unsigned j = ce->first; m->res_cells && j < (unsigned)ce->first + ce->count; j++) {
        unsigned fl = chunks[order[j]].flags;
        if (hzf && hz_covers(fl))
            continue;
        /* (mapstream_res also ranks it up: the cell is in view); foliage is resident only near the player
         * (mapstream.c MS_GRASS_WANT) and thinned to nothing farther: its absence is no hole */
        /* a chunk the camera cannot see is not read (pvs.c), nor one the draw groups hide (drawgrp.c, mapstream.c
         * MS_DG): neither is a hole. Counted as missing, a cell whose coarse chunks are hidden stand-ins drew its
         * horizon inside HZN_R for good (Firelink's cell 47 seen from the Burg: report3's 99.5 %, 2026-11-08) */
        if (!mp_res_base(m, order[j]) && !(fl & CHUNK_GRASS) &&
            pvs_chunk(k, (unsigned)ci, order[j]) && dg_shown(k, order[j])) {
            *miss = 1;
            if (hz_covers(fl))
                *mcov = 1;
            else if (has_hz)
                *miss = 2;  /* not covered (with a horizon only blended effects are not): drawn late */
        }
    }
}

#if MAP_TWO_PASS
unsigned short mp_lpool[LPOOL_N];
unsigned mp_lpool_top, mp_lpool_gen = 1, mp_lpool_builds, mp_lpool_resets;
/* miss_scan from the cell's items (ib: their bases, NULL = not in; ib[t] of item i0 + t): only the chunks of items not
 * in are looked at, in chunk order as miss_scan does */
void mp_miss_items(const MapInst *m, int k, int ci, int hzf, int has_hz, const unsigned char *const *ib, unsigned i0,
                       unsigned ni, int *miss, int *mcov)
{
    for (unsigned t = 0; t < ni; t++) {
        if (ib[t] || (m->rflag[i0 + t] & RI_GRASS))
            continue;
        const unsigned *rr = m->rrange[i0 + t];
        for (unsigned j = rr[0]; j < rr[0] + rr[1]; j++) {
            unsigned fl = m->chunks[j].flags;
            if (hzf && hz_covers(fl))
                continue;
            if (!(fl & CHUNK_GRASS) && pvs_chunk(k, (unsigned)ci, j) && dg_shown(k, j)) {  /* (as miss_scan) */
                *miss = 1;
                if (hz_covers(fl))
                    *mcov = 1;
                else if (has_hz)
                    *miss = 2;
            }
        }
    }
}

/* the cell's draw list (MAP_TWO_PASS pre-pass): offsets from ce->first of the chunks [jb, je) that pass the level,
 * stream, draw-group and visibility-row filters, in chunk order, as the per-chunk loop takes them; *cut = triangles
 * the visibility rows left out */
unsigned mp_list_build(const MapInst *m, int k, int ci, int nd, int use_hz, const unsigned char *const *ib,
                           unsigned i0, const unsigned char *const *nbp, unsigned np, unsigned jb, unsigned je,
                           unsigned short *ls, unsigned *cut)
{
    const Cell *ce = &m->cells[ci];
    const MapCell *ctab = m->ctab;
    unsigned ln = 0, ct = 0;
    const unsigned char *nbb = nd ? nbp[0] : NULL;
    unsigned gid = nd ? ctab[ci].pad : 0, part = 0, pe = nd ? ctab[ci].near : 0;
    for (unsigned j = jb; j < je; j++) {
        const Chunk *c;
        unsigned no;
        if (j < (unsigned)ce->first + ce->count) {
            no = m->order[j], c = &m->chunks[no];
            if (use_hz && hz_covers(c->flags))
                continue;  /* the horizon stands for it */
            if (!(m->res_cells ? ib[m->citem[no] - i0] : m->streams))
                continue;  /* its streams are not in */
            if (nd && (c->flags & CHUNK_COARSE))
                continue;
        } else {
            unsigned nj = j - ce->first - ce->count;
            while (nj >= pe && part + 1 < np)  /* into the next part's blob */
                part++, nbb = nbp[part], gid = ctab[ci + part].pad, pe += ctab[ci + part].near & ~CELL_PART;
            if (!nbb)
                continue;  /* an unseen part that is not in (pvs.c) */
            c = (const Chunk *)nbb + (nj - (pe - (ctab[ci + part].near & ~CELL_PART)));
            no = c->flags & CHUNK_GRASS ? gid + (unsigned)(c - (const Chunk *)nbb) : c->flags >> 8;
        }
        if (!dg_shown(k, no))  /* MSB draw groups (drawgrp.c) */
            continue;
        if (!pvs_chunk(k, (unsigned)ci, no)) {  /* behind something (pvs.c) */
            ct += c->tris;
            continue;
        }
        ls[ln++] = (unsigned short)(j - ce->first);
    }
    *cut = ct;
    return ln;
}
#ifdef MAP_TWO_PASS_CHECK
unsigned mp_chk_lists, mp_chk_bad;
#endif
#endif

void mp_build_index(MapInst *mi)
{
    if (mi->indexed == mi->map_bin)
        return;
    const Chunk *chunks = mi->chunks;
    free(mi->order), free(mi->sky), free(mi->cells), free(mi->tiles), free(mi->tcell);
    mi->tiles = NULL, mi->tcell = NULL, mi->ntiles = 0;
    unsigned n = mi->hdr->chunks;
    unsigned (*key)[3] = malloc(n * sizeof *key);
    unsigned short *order = mi->order = malloc(n * sizeof *order);
    unsigned short *sky = mi->sky = malloc(n * sizeof *sky);
    int m = 0, nsky = 0;
    for (unsigned i = 0; i < n; i++) {
        const Chunk *c = &chunks[i];
        if (c->flags & CHUNK_SKY) {
            sky[nsky++] = i;
            continue;
        }
        unsigned k = 0;
        for (int a = 0; a < 3; a++)
            k = k << 10 | (((int)((c->mn[a] + c->mx[a]) * 0.5f / CELL + 512)) & 1023);
        key[m][0] = k, key[m][1] = (unsigned)c->tex, key[m++][2] = i;
    }
    mi->nsky = nsky;
    /* (the exporter's cell table when there is one: sized for one cell per chunk, Firelink's 12179 chunks took 633 KB
     * of heap for ~1 % of that many cells until 2026-11-13) */
    Cell *cells = mi->cells = calloc((mi->ctab ? mi->nctab : (unsigned)m) + 1, sizeof *cells);
    int ncells = 0;
    if (mi->ctab) {  /* DSM5: the exporter's cells, resident chunks already grouped per cell (sky chunks after them) */
        const MapCell *ctab = mi->ctab;
        for (unsigned i = 0; i < mi->nctab; i++) {
            Cell *ce = &cells[ncells++];
            memcpy(ce->mn, ctab[i].mn, sizeof ce->mn), memcpy(ce->mx, ctab[i].mx, sizeof ce->mx);
            ce->first = ctab[i].first, ce->count = ctab[i].count, ce->flags = 0, ce->near_on = 0, ce->hz_on = 0;
            for (unsigned j = ce->first; j < ce->first + ce->count; j++)
                order[j] = j, ce->flags |= chunks[j].flags & (CHUNK_BLEND | CHUNK_BACKDROP);
        }
        m = 0;
    }
    qsort(key, m, sizeof *key, cell_key_cmp);
    for (int j = 0; j < m; j++) {
        const Chunk *c = &chunks[key[j][2]];
        order[j] = key[j][2];
        if (!j || key[j][0] != key[j - 1][0]) {
            Cell *ce = &cells[ncells++];
            memcpy(ce->mn, c->mn, sizeof ce->mn), memcpy(ce->mx, c->mx, sizeof ce->mx);
            ce->first = j, ce->count = 0, ce->flags = 0, ce->near_on = 0;
        }
        Cell *ce = &cells[ncells - 1];
        ce->count++;
        ce->flags |= c->flags & (CHUNK_BLEND | CHUNK_BACKDROP);
        for (int a = 0; a < 3; a++) {
            ce->mn[a] = c->mn[a] < ce->mn[a] ? c->mn[a] : ce->mn[a];
            ce->mx[a] = c->mx[a] > ce->mx[a] ? c->mx[a] : ce->mx[a];
        }
    }
    free(key);
    mi->ncells = ncells;
    mi->indexed = mi->map_bin;
    tile_index(mi);
}

/* ---- world blocks (src/world.c): one MapInst per loaded map block ---- */
/* a world block's dg.bin, at boot (src/world.c world_systems; the pack read waits on the IOP): kept for the session
 * (Burg west 44 KB), handed to drawgrp.c each time the block turns on */
static void *blk_dg[MAP_INST], *blk_pvs[MAP_INST];
/* texcache slot of instance k > 0: the first one free when it turns on. More instances than TC_BLOCKS (the Asylum is
 * a fifth block) are fine: world.c has at most WORLD_SLOTS lite sets loaded, and only loaded blocks are instances */
static signed char tslot[MAP_INST];
static const void *blk_tex_sh[MAP_INST];  /* map_block_shared: the map's maptex.bin per block */

int map_tex_slot(int k)
{
    return k > 0 && k < MAP_INST && mp_inst[k].on ? tslot[k] : -1;
}

/* the collision block of instance k (0 for the boot area), -1 while it is off */
int mp_inst_col_block(int k)
{
    return k == 0 ? mp_inst[0].col_block : mp_inst[k].on ? mp_inst[k].col_block : -1;
}

int map_inst_of(int col_block)
{
    if (!mp_inst[0].on)  /* single area: everything is instance 0 */
        return col_block == 0 ? 0 : -1;
    for (int k = 0; k < MAP_INST; k++)
        if ((k == 0 || mp_inst[k].on) && mp_inst[k].col_block == col_block)
            return k;
    return -1;
}

static int split_k = -1;
static const unsigned char *split_b, *split_c;
static unsigned split_boff, split_coff;
void map_block_split(int k, const void *b, unsigned b_off, const void *c, unsigned c_off)
{
    split_k = k, split_b = b, split_boff = b_off, split_c = c, split_coff = c_off;
}

int map_block_init(int k, const void *tab, const void *res, int col_block)
{
    if (k < 0 || k >= MAP_INST)
        return 0;
    const Header *h = tab;
    if (!h || memcmp(h->magic, "DSM5", 4) || !h->cell_off)
        return 0;
    MapInst *m = &mp_inst[k];
    if (k) {  /* a fresh instance: no index yet */
        m->indexed = NULL, m->order = m->sky = NULL, m->cells = NULL, m->ncells = m->nsky = 0;
        m->tiles = NULL, m->tcell = NULL, m->ntiles = 0;
    }
    /* tab = the 80-byte header + map.bin from the chunk table on (tools/mapsplit.py): offsets >= chunk_off resolve
     * against a base chunk_off - 80 bytes before it */
    const unsigned char *map_bin = m->map_bin = (const unsigned char *)tab + sizeof(Header) - h->chunk_off;
    /* pieces (world.c WORLD_TAB_SPLIT): piece base - its offset in maptab.bin, in map.bin coordinates as map_bin */
    const unsigned char *tex_bin = m->tex_bin =
        split_k == k && split_b ? split_b - split_boff + sizeof(Header) - h->chunk_off : map_bin;
    const unsigned char *cel_bin = m->cel_bin =
        split_k == k && split_c ? split_c - split_coff + sizeof(Header) - h->chunk_off : map_bin;
    split_k = -1, split_b = split_c = NULL;
    const Header *hdr = m->hdr = h;
    m->ctab = (const MapCell *)(cel_bin + hdr->cell_off + 16);
    m->nctab = *(const unsigned *)(cel_bin + hdr->cell_off);
    m->ckeys = cell_keys(m->ctab, m->nctab);
    m->pkt = ((const unsigned *)(cel_bin + hdr->cell_off))[1] & 1 ? VU1_PKT : 0;
    m->chunks = (const Chunk *)(map_bin + hdr->chunk_off);
    m->fog_rows = *(const unsigned *)(tex_bin + hdr->fog_off);
    m->fog_ntris = ((const unsigned *)(tex_bin + hdr->fog_off))[1];
    m->fog_row = (const FogRow *)(tex_bin + hdr->fog_off + 16);
    m->fog_tri = (const unsigned char *)(m->fog_row + m->fog_rows);
    m->sky_rows = SKY_ROWS ? ((const unsigned *)(tex_bin + hdr->fog_off))[2] : 0;
    m->sky_row = (const SkyRow *)(m->fog_tri + ((m->fog_ntris + 3) & ~3u));
    m->navv = (const float *)(tex_bin + hdr->navv_off);
    m->navt = (const unsigned short *)(tex_bin + hdr->navt_off);
    m->streams = res, m->res_cells = 0, m->citem = NULL;
    mp_hz_parse(m);
    m->ntex = hdr->textures < MAX_TEX ? hdr->textures : MAX_TEX;
    m->on = 1, m->col_block = col_block, m->hz_only = 0;
    if (!k) {  /* the boot block: fog / light rows and the texture pool as map_init / map_upload */
        float f0[FOG_NOW] = {hdr->fog_begin, hdr->fog_end, hdr->fog_max, hdr->fog_rgb[0], hdr->fog_rgb[1],
                             hdr->fog_rgb[2], 64, 64, 64, 96, 96, 96, 0.40f * 127, 0.80f * 127, -0.45f * 127};
        memcpy(mp_fog_now, f0, sizeof mp_fog_now);
        mp_fog_area = 0;
        mp_pfog_row = m->fog_row, mp_pfog_rows = m->fog_rows;
        if (m->fog_rows)
            mp_fog_ease(1);
        m->tex_base = 0;
    } else {
        int used = 0;
        for (int j = 1; j < MAP_INST; j++)
            if (j != k && mp_inst[j].on)
                used |= 1 << tslot[j];
        tslot[k] = 0;
        while (tslot[k] < TC_BLOCKS - 1 && used >> tslot[k] & 1)
            tslot[k]++;
        tc_block_shared(tslot[k], blk_tex_sh[k]);  /* the map's shared textures (map_block_shared) */
        m->tex_base = used >> tslot[k] & 1 ? -1 : tc_block_add(tslot[k], tex_bin, tex_bin + hdr->tex_off, m->ntex);
        if (m->tex_base < 0) {
            printf("map: block %d: no texture slot / CLUT VRAM\n", k);
            m->on = 0;
            return 0;
        }
        mp_tex_res(k);
        dg_block_init(k, blk_dg[k], hdr->chunks, (unsigned)col_block_tris(col_block));  /* per-block draw groups */
        pvs_block_init(k, blk_pvs[k], hdr->chunks, m->nctab);  /* and visibility (src/pvs.c) */
    }
    return 1;
}

void map_block_shared(int k, const void *buf)
{
    if (k > 0 && k < MAP_INST)
        blk_tex_sh[k] = buf;
}

void map_block_bump(int k, const void *blob)
{
    if (k < 0 || k >= MAP_INST || (k && !mp_inst[k].on) || !mp_inst[k].hdr)  /* (0: the boot block, world.c) */
        return;
    int on = bump_block(k, blob, mp_inst[k].hdr->chunks);
    if (blob)
        printf("map: block %d relief %s\n", k, on ? "on" : "off");
}

static void block_drop(int k, int keep_dg)
{
    if (k <= 0 || k >= MAP_INST || !mp_inst[k].on)
        return;
    MapInst *m = &mp_inst[k];
    bump_block(k, NULL, 0);
    mapstream_close(k);  /* before the slot's textures go: it hands their mip 0 back (tex_hook_set) */
    tc_block_drop(tslot[k]);
    if (!keep_dg)
        dg_block_drop(k);
    pvs_block_drop(k);
    free(m->order), free(m->sky), free(m->cells), free(m->tiles), free(m->tcell);
    m->indexed = NULL, m->order = m->sky = NULL, m->cells = NULL, m->ncells = m->nsky = 0;
    m->tiles = NULL, m->tcell = NULL, m->ntiles = 0;
    if (mp_pfog_row == m->fog_row)  /* the player's rows were this block's: back to the boot block's */
        mp_pfog_row = mp_inst[0].fog_row ? mp_inst[0].fog_row : NULL, mp_pfog_rows = mp_inst[0].fog_row ? mp_inst[0].fog_rows : 0,
        mp_fog_area = 0;
    free(m->citem), free(m->rrange), free(m->rflag);
    m->rrange = NULL, m->rflag = NULL, m->nritem = 0;
    m->hdr = NULL, m->ctab = NULL, m->ckeys = NULL, m->nctab = 0, m->streams = NULL, m->ntex = 0, m->res_cells = 0, m->citem = NULL;
    m->hz_cell = NULL, m->hz_chunk = NULL, m->hz_base = NULL;
    m->on = 0;
    mapstream_select(0);
}

void map_block_drop(int k)
{
    block_drop(k, 0);
    if (k > 0 && k < MAP_INST)
        mp_inst[k].hz_only = 0, mp_inst[k].hz_cell = NULL, mp_inst[k].hz_chunk = NULL, mp_inst[k].hz_base = NULL;
}
void map_block_lean(int k, int keep_hz)
{
    if (k <= 0 || k >= MAP_INST || !mp_inst[k].on)
        return;
    MapInst *m = &mp_inst[k];
    const HzCell *hc = m->hz_cell;
    const Chunk *hk = m->hz_chunk;
    const unsigned char *hb = m->hz_base;
    unsigned n = m->nctab;
    block_drop(k, 1);
    if (keep_hz && hc)  /* (they lie in maptab's last piece, which world.c keeps) */
        m->hz_cell = hc, m->hz_chunk = hk, m->hz_base = hb, m->nctab = n, m->hz_only = 1;
}

/* a kept-only block (hz_only): every horizon chunk in view, as the shown draw groups pick them */
unsigned mp_draw_hz_only(int k, const float *e)
{
    const MapInst *m = &mp_inst[k];
    unsigned n = 0;
    for (unsigned ci = 0; ci < m->nctab; ci++) {
        const HzCell *hc = &m->hz_cell[ci];
        for (unsigned j = 0; j < hc->count; j++) {
            const Chunk *c = &m->hz_chunk[hc->first + j];
            if (!dg_mask_shown(k, (int)(c->flags >> 16 & 0xFF)))
                continue;
            int cls = mp_visible(c, e, 0);
            if (cls)
                n += mp_draw_hz(m, c, cls, 0, 0);
        }
    }
    return n;
}

void map_pvs_init(const void *blob)
{
    if (mp_inst[0].hdr)
        pvs_block_init(0, blob, mp_inst[0].hdr->chunks, mp_inst[0].nctab);
}

void map_block_dg(int k, void *pack)
{
    if (k > 0 && k < MAP_INST && pack && !blk_dg[k])
        blk_dg[k] = pack_load(pack, "dg.bin", NULL);
#ifndef NO_PVS
    if (k > 0 && k < MAP_INST && pack && !blk_pvs[k])
        blk_pvs[k] = pack_load(pack, "pvs.bin", NULL);
#endif
}

/* bytes of the streams of resident chunks [a, b) (export_map.py writes pos, st, colour per chunk, each 16-byte
 * aligned, chunk after chunk in table order, each after a VIF code qword: VU1_PKT): [*lo, return) in mapres.bin */
static unsigned chunk_range(const Chunk *chunks, unsigned a, unsigned b, unsigned *lo)
{
    unsigned l = ~0u, h = 0;
    for (unsigned j = a; j < b; j++) {
        const Chunk *c = &chunks[j];
        unsigned e = c->nv ? (CHUNK_IDX(c) + c->tris * 4u + 15) & ~15u  /* the triangle stream is the last */
                           : (c->col + c->tris * 12 + 15) & ~15u;  /* colour u8x4 per corner, the last stream */
        l = c->pos - 16 < l ? c->pos - 16 : l;  /* from pos's code qword (VU1_PKT sends from there) */
        h = e > h ? e : h;
    }
    *lo = l == ~0u ? 0 : l;
    return h;
}

/* World blocks: the selected instance k's resident level as mapstream items (mapstream_res_open) instead of one
 * mapres.bin in a full pool: per cell the streams of its resident chunks (grouped by cell in the chunk table) in
 * items of whole chunks up to RES_ITEM bytes, the sky chunks' the same way, and each texture's mip 0 (and mip 1,
 * MIP1_OUT) that tools/mapsplit.py moved into mapres.bin. A block without it draws its near cells only. RES_ITEM: one cell up to
 * 2.2 MB as one item found no hole with 4 MB of the arena free (largest hole 0.9-1 MB; ISO runs 2026-11-05); ASSUMED
 * size, small enough to fit the holes near cells leave. */
#define RES_ITEM (64u << 10)  /* one page of the arena (mapstream.c MS_PAGE, plan step 6; 128 KB before) */
static void res_open(int k, const void *pack)
{
    MapInst *mi = &mp_inst[k];
    const Header *hdr = mi->hdr;
    const Chunk *chunks = mi->chunks;
    const MapCell *ctab = mi->ctab;
    unsigned nctab = mi->nctab;
    unsigned roff, rsize;
    if (!pack_entry_raw((Pack *)pack, "mapres.bin", &roff, &rsize) || !rsize)
        return;
    unsigned n = hdr->chunks + 1 + 2 * (unsigned)mi->ntex;  /* (mip 0 and 1 per texture) */
    MapResItem *it = malloc(n * sizeof *it);
    unsigned short *citem = mi->citem = malloc((hdr->chunks + 1) * sizeof *citem);
    unsigned (*rr)[3] = mi->rrange = malloc(n * sizeof *rr);
    unsigned char *rf = mi->rflag = calloc(n, 1);
    if (!it || !citem || !rr || !rf || n > 65535) {
        free(it), free(citem), free(rr), free(rf);
        mi->citem = NULL, mi->rrange = NULL, mi->rflag = NULL;
        return;
    }
    unsigned m = 0, bad = 0, last = 0;
    for (unsigned i = 0; i <= nctab; i++) {  /* cells, then (i = nctab) the sky chunks after them (res_order) */
        unsigned a = i < nctab ? ctab[i].first : last, e = i < nctab ? a + ctab[i].count : hdr->chunks;
        if (i < nctab)
            last = e > last ? e : last;
        for (unsigned j = a; j < e;) {  /* whole chunks [j, g) up to RES_ITEM bytes (at least one) */
            unsigned lo, hi = chunk_range(chunks, j, j + 1, &lo), g = j + 1, l2, h2;
            /* coarse chunks in items of their own (MS_RES_COARSE: they go while the cell's near level is in), and
             * foliage too (MS_RES_GRASS: only near the player) */
            unsigned coarse = chunks[j].flags & (CHUNK_COARSE | CHUNK_GRASS);
            while (g < e && (chunks[g].flags & (CHUNK_COARSE | CHUNK_GRASS)) == coarse &&
                   (h2 = chunk_range(chunks, g, g + 1, &l2)) - lo <= RES_ITEM && l2 >= lo)
                hi = h2 > hi ? h2 : hi, g++;
            unsigned far = 0, cov = i < nctab && mi->hz_cell && (mi->hz_cell[i].flags & 1);
            for (unsigned c = j; c < g; c++)
                citem[c] = (unsigned short)m, far |= chunks[c].flags & CHUNK_BACKDROP, cov &= hz_covers(chunks[c].flags);
            rr[m][0] = j, rr[m][1] = i < nctab ? g - j : 0, rr[m][2] = i;
            rf[m] = (cov ? RI_COV : 0) | (coarse & CHUNK_GRASS ? RI_GRASS : 0);
            it[m++] = (MapResItem){roff + lo, lo, hi - lo,
                                   i < nctab ? i | (far ? MS_RES_FAR : 0) |
                                                   ((coarse & CHUNK_COARSE) && ctab[i].near ? MS_RES_COARSE : 0) |
                                                   (coarse & CHUNK_GRASS ? MS_RES_GRASS : 0) | (cov ? MS_RES_LOD : 0)
                                             : MS_RES_SKY};
            bad |= hi > rsize;
            j = g;
        }
    }
    const unsigned char *te = mi->tex_bin + hdr->tex_off;
    for (int t = 0; t < mi->ntex; t++) {  /* texmap.py entry: u16 w, h; u8 psm; ...; u32 pix[0] at +16 */
        const unsigned char *e = te + 32 * t;
        unsigned w = *(const unsigned short *)e, h = *(const unsigned short *)(e + 2);
        for (int l = 0; l < 2; l++) {  /* mip 0, and mip 1 when it streams too (mapsplit.py MIP1_OUT) */
            unsigned p = *(const unsigned *)(e + 16 + 4 * l), wl = w >> l, hl = h >> l;
            if (!(p & TC_RES))
                continue;
            unsigned b = e[4] == GS_PSM_T4 ? wl * hl / 2 : wl * hl;
            rr[m][0] = rr[m][1] = rr[m][2] = 0;
            it[m++] = (MapResItem){roff + (p & ~TC_RES), p & ~TC_RES, b, MS_RES_TEX | (l ? MS_RES_MIP1 : 0) | (unsigned)t};
            bad |= (p & ~TC_RES) + b > rsize;
        }
    }
    if (bad)
        printf("map: block %d: mapres.bin (%u KB) shorter than its chunk / texture records\n", k, rsize >> 10);
    else if (mi->nritem = m, mapstream_res_open(k, it, nctab, m)) {
        mi->res_cells = 1;
        mapstream_tex_hooks(mp_tex_hook_age, mp_tex_hook_set);
    }
    if (!mi->res_cells)
        free(citem), free(rr), free(rf), mi->citem = NULL, mi->rrange = NULL, mi->rflag = NULL, mi->nritem = 0;
    free(it);
}

int map_block_stream_open(int k, const void *pack)
{
    if (k < 0 || k >= MAP_INST || !mp_inst[k].on)
        return 0;
    const MapInst *m = &mp_inst[k];
    mapstream_select(k);
    int r = m->ctab ? mapstream_open_pack(k, pack, m->ctab, m->nctab) : 0;
    mapstream_vis_hooks(mp_vis, mp_vis_gen);
    if (r && !m->streams)
        res_open(k, pack);
    mapstream_select(0);  /* mapstream_open_pack leaves instance k selected */
    return r;
}
