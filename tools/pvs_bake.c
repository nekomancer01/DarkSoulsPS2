/* Occlusion bake (2026-11-06): pvs.bin, a sampled potentially visible set per camera cell over PVS units (one MSB part
 * within one 32 m map cell; tools/export_map.py write_pvsin), for src/pvs.c.
 *
 *   pvs_bake <pvsin.bin> <dg.bin> <collision.bin> <pvs.bin> [--res N] [--cell M] [--per-cell N] [--cams N]
 *            [--cluster M] [--validate N] [--spot ex,ey,ez,tx,ty,tz,px,py,pz ...] [--threads N]
 *            [--with <pvsin.bin> <dg.bin> <collision.bin> ...]
 *
 * Player positions are taken on every walkable collision triangle (2 m lattice); each gets a few cameras the way
 * main.c places them (target = feet + 1.42 m, orbit 4 m at a random yaw / pitch, pulled in by a collision ray with
 * 0.3 m margin). Cameras are bucketed by the cell (--cell, 8 m) of the eye; up to --per-cell of them render an ID cube
 * map each: the chunks map.c would draw from there (the draw groups of the collision part under the player, near
 * level within 26 m of its cell, coarse level beyond 22 m, kept chunks always, nothing beyond 250 m), occluders
 * (static opaque map pieces) first into depth, then every drawn chunk tested against it with a small depth bias.
 * A unit with a passing pixel is visible from the cell. Rows are OR'd with their 6 neighbours (missed samples) and
 * a camera in an unsampled cell takes the union of its stored neighbours, so the runtime falls back to "everything"
 * (no row) only well away from where the camera can be. Sky and backdrop units are always visible.
 *
 * --with: the other world blocks (all in one world frame). Their walkable floors within 250 m place cameras too, and
 * while the player stands on one, that block's chunks (its own draw groups) occlude; this block's chunks then only
 * get tested (its draw groups there are not known: map.c keeps the set of the last part stood on), never hide
 * anything. Without it a camera in a neighbour (Firelink seen from the Burg wall) had no row and drew all.
 *
 * --validate N: N fresh cameras (another seed) render the full scene; every pixel whose unit their cell's row leaves
 * out counts as missing. --spot: triangles a 4:3 camera at e looking at t (player at p) draws with and without the
 * PVS (offline triangle counts at test spots).
 *
 * A unit is an MSB part within one --cluster cube (32 m: one map cell; 0: whole parts). Whole parts and 64 m clusters
 * culled much less at world_cross (97 / 79 % of the triangles kept against 62 %), and merging similar rows to a byte
 * budget lost most of the culling too; the tables stay small by storing only sampled cells (a missing cell takes the
 * union of its stored neighbours at runtime) and giving no bit to units every cell or no cell sees.
 *
 * pvs.bin: a chunk file 'PVS ' (ps2/proto/src/chunk.h, written with tools/native/chunkw.h; src/pvs.c reads it in
 * place), little endian:
 *   PHDR v1 (28 B): row bits, chunks, cell table entries, camera cells (0: no rows, everything drawn), rows, row bytes,
 *           f32 cell size
 *   EOFF v1 u16 x (entries + 1): start of each entry's unit list in ULST (the last: its length)
 *   ULST v1 u16 unit lists: the unit's bit in a row, 0xFFFF seen from every cell, 0xFFFE from none
 *   LOC  v1 u8 per chunk (map.bin / dg.bin order): index into its entry's list, 0xFF always drawn (sky)
 *   KEYS v1 u32 key per camera cell (sorted; x + 512 | y + 512 << 10 | z + 512 << 20 of floor(eye / cell))
 *   ROWO v1 u16 row per camera cell
 *   ROWS v1 rows: row bytes each (bit b & 7 of byte b >> 3: the unit of row bit b visible)
 *   (before Polaris 3.3: one 'PVS3' blob, the same sections behind a 32-byte header, padded to 4 bytes)
 * dg.bin (input): tools/drawgroups.py's chunk file 'DGRP', read with ps2/proto/src/chunk.c */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native/chunkw.h"
#include "../ps2/proto/src/chunk.h"
#ifdef _OPENMP
#include <omp.h>
#endif

#define NEAR_IN 22.0f   /* map.c NEAR_DRAW_R - NEAR_HYST: near level drawn from here in */
#define NEAR_OUT 26.0f  /* NEAR_DRAW_R + NEAR_HYST: and still out to here */
#define CULL_FAR 250.0f
#define CAM_H 1.42f     /* LockCamParam chrOrgOffset_Y */
#define CAM_D 4.0f
#define COL_SOLID 0x1FFu
#define CHUNK_BLEND 2
#define CHUNK_SKY 8
#define CHUNK_BACKDROP 128

typedef struct { uint32_t unit, flags, first, ntri, entry; float mn[3], mx[3]; } InChunk;
typedef struct { int32_t key[3]; uint32_t part; float mn[3], mx[3]; } InUnit;
typedef struct { float v[9]; } Tri9;

static InChunk *ch;
static InUnit *un;
static uint32_t *cunit;       /* per chunk its input unit (part within a map cell): the cell box */
static unsigned ncunits_all;  /* input units of every block (cell boxes) */
static float cluster = 32.0f, cell_grid = 32.0f;
static Tri9 *tri;
static unsigned nch, nun, ntri;
/* blocks: 0 = the one baked, 1.. its neighbours (--with): their chunks (unit FOREIGN) occlude while the player stands
 * on them, their collision gives player positions; only block 0's units are recorded */
#define MAXB 32  /* blocks: the one baked + its --with neighbours (world.h WORLD_MAX; 16 until the 32-block world) */
#define FOREIGN 0xFFFFFFFEu
static int nblk = 1;
static unsigned char *bshown[MAXB];  /* [coll][piece] per block, NULL: no draw groups */
static unsigned bnp[MAXB], bnc[MAXB];
static unsigned char *cblk, *cdg;    /* per chunk: block, dg.bin piece mask */
static unsigned nself;               /* block 0's chunks: [0, nself) */
static unsigned char *always;        /* per unit */
static float cellsz = 8.0f;
static int R = 192;

/* ---- collision ---- */
static float (*cv)[3];
typedef struct { uint32_t v[3]; uint8_t hf, blk, dg, pad; } CTri;  /* every block's collision in one world frame */
static CTri *ct;
static unsigned ncv, nct;
static float gx0, gz0;
#define GC 4.0f
static int gw, gd;
static unsigned *gcell, *glist;

static void *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *b = malloc(*n + 16);
    if (fread(b, 1, *n, f) != *n) { perror(p); exit(1); }
    fclose(f);
    return b;
}

/* append block blk's collision.bin; returns its first triangle */
static unsigned load_col(const char *p, int blk)
{
    size_t n;
    unsigned char *b = slurp(p, &n);
    unsigned *h = (unsigned *)b;
    float *hf = (float *)b;
    unsigned nv = h[2], nt = h[3], v0 = ncv, t0 = nct;
    float org[3] = {hf[4], hf[5], hf[6]}, sc = hf[7];
    if (h[1] == 2) {  /* DCOL v2 (tools/export_collision.py write_v2): fragments; triangles back at their v1 numbers */
        unsigned nfrag = h[14];
        const uint16_t *pat = (const uint16_t *)(b + h[18]), *gid = (const uint16_t *)(b + h[20]);
        cv = realloc(cv, (ncv + nv) * sizeof *cv), ct = realloc(ct, (nct + nt) * sizeof *ct);
        unsigned vi = v0;
        for (unsigned fi = 0; fi < nfrag; fi++) {
            const unsigned char *fr = b + h[13] + 32 * fi;
            const int16_t *lo = (const int16_t *)fr;
            uint16_t ntri = *(const uint16_t *)(fr + 12), nvert = *(const uint16_t *)(fr + 14);
            uint32_t tri0 = *(const uint32_t *)(fr + 16), data = *(const uint32_t *)(fr + 20);
            const uint16_t *vs = (const uint16_t *)(b + data);
            const unsigned char *ts = b + data + ((nvert * 6u + 3) & ~3u);
            for (unsigned i = 0; i < nvert; i++)
                for (int k = 0; k < 3; k++)
                    cv[vi + i][k] = org[k] + (lo[k] + vs[i * 3 + k]) * sc;
            for (unsigned i = 0; i < ntri; i++) {
                CTri *t = &ct[t0 + gid[tri0 + i]];
                for (int k = 0; k < 3; k++)
                    t->v[k] = vi + ts[i * 4 + k];
                t->hf = (uint8_t)(pat[ts[i * 4 + 3]] & 0xff), t->blk = (uint8_t)blk, t->dg = 0xFF, t->pad = 0;
            }
            vi += nvert;
        }
        ncv = vi, nct += nt;
        free(b);
        return t0;
    }
    const short *vs = (const short *)(b + h[13]);
    const struct { uint16_t v[3]; uint8_t hf, mat; } *ts = (const void *)(b + h[14]);
    cv = realloc(cv, (ncv + nv) * sizeof *cv), ct = realloc(ct, (nct + nt) * sizeof *ct);
    for (unsigned i = 0; i < nv; i++)
        for (int k = 0; k < 3; k++)
            cv[v0 + i][k] = org[k] + vs[i * 3 + k] * sc;
    for (unsigned i = 0; i < nt; i++) {
        CTri *t = &ct[t0 + i];
        for (int k = 0; k < 3; k++)
            t->v[k] = v0 + ts[i].v[k];
        t->hf = ts[i].hf, t->blk = (uint8_t)blk, t->dg = 0xFF, t->pad = 0;
    }
    ncv += nv, nct += nt;
    free(b);
    return t0;
}

static void build_grid(void)
{
    float lo[2] = {1e9, 1e9}, hi[2] = {-1e9, -1e9};
    for (unsigned i = 0; i < ncv; i++) {
        lo[0] = fminf(lo[0], cv[i][0]), hi[0] = fmaxf(hi[0], cv[i][0]);
        lo[1] = fminf(lo[1], cv[i][2]), hi[1] = fmaxf(hi[1], cv[i][2]);
    }
    gx0 = lo[0], gz0 = lo[1];
    gw = (int)((hi[0] - lo[0]) / GC) + 1, gd = (int)((hi[1] - lo[1]) / GC) + 1;
    unsigned *cnt = calloc((size_t)gw * gd + 1, sizeof *cnt);
    for (int pass = 0; pass < 2; pass++) {
        for (unsigned t = 0; t < nct; t++) {
            float a0 = 1e9, a1 = -1e9, b0 = 1e9, b1 = -1e9;
            for (int k = 0; k < 3; k++) {
                float *v = cv[ct[t].v[k]];
                a0 = fminf(a0, v[0]), a1 = fmaxf(a1, v[0]), b0 = fminf(b0, v[2]), b1 = fmaxf(b1, v[2]);
            }
            int x0 = (int)((a0 - gx0) / GC), x1 = (int)((a1 - gx0) / GC), z0 = (int)((b0 - gz0) / GC), z1 = (int)((b1 - gz0) / GC);
            for (int z = z0; z <= z1; z++)
                for (int x = x0; x <= x1; x++) {
                    unsigned c = (unsigned)z * gw + x;
                    if (pass)
                        glist[gcell[c] + --cnt[c]] = t;
                    else
                        cnt[c]++;
                }
        }
        if (!pass) {
            gcell = malloc(((size_t)gw * gd + 1) * sizeof *gcell);
            unsigned s = 0;
            for (size_t c = 0; c < (size_t)gw * gd; c++)
                gcell[c] = s, s += cnt[c];
            gcell[(size_t)gw * gd] = s;
            glist = malloc((s + 1) * sizeof *glist);
            for (size_t c = 0; c < (size_t)gw * gd; c++)
                cnt[c] = gcell[c + 1] - gcell[c];
        }
    }
    free(cnt);
}

/* segment a -> b against solid collision: smallest t in (0, 1], or 2 */
static float col_ray(const float *a, const float *b)
{
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, best = 2;
    int x0 = (int)floorf((fminf(a[0], b[0]) - gx0) / GC), x1 = (int)floorf((fmaxf(a[0], b[0]) - gx0) / GC);
    int z0 = (int)floorf((fminf(a[2], b[2]) - gz0) / GC), z1 = (int)floorf((fmaxf(a[2], b[2]) - gz0) / GC);
    for (int z = z0 < 0 ? 0 : z0; z <= z1 && z < gd; z++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < gw; x++) {
            unsigned c = (unsigned)z * gw + x;
            for (unsigned j = gcell[c]; j < gcell[c + 1]; j++) {
                unsigned t = glist[j];
                if (!(COL_SOLID >> ct[t].hf & 1))
                    continue;
                float *p0 = cv[ct[t].v[0]], *p1 = cv[ct[t].v[1]], *p2 = cv[ct[t].v[2]];
                float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
                float pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
                float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
                if (fabsf(det) < 1e-9f)
                    continue;
                float inv = 1 / det, tv[3] = {a[0] - p0[0], a[1] - p0[1], a[2] - p0[2]};
                float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
                if (u < 0 || u > 1)
                    continue;
                float q[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
                float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
                if (v < 0 || u + v > 1)
                    continue;
                float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
                if (tt > 0 && tt <= 1 && tt < best)
                    best = tt;
            }
        }
    return best;
}

/* the solid collision triangle under (x, y, z): the highest hit between y + 0.5 and y - 3 (map_set_player), -1 */
static int col_floor(float x, float y, float z)
{
    int gx = (int)floorf((x - gx0) / GC), gz = (int)floorf((z - gz0) / GC), best = -1;
    if (gx < 0 || gz < 0 || gx >= gw || gz >= gd)
        return -1;
    float by = -1e9;
    unsigned c = (unsigned)gz * gw + gx;
    for (unsigned j = gcell[c]; j < gcell[c + 1]; j++) {
        unsigned t = glist[j];
        if (!(COL_SOLID >> ct[t].hf & 1))
            continue;
        float *a = cv[ct[t].v[0]], *b = cv[ct[t].v[1]], *e = cv[ct[t].v[2]];
        float d = (b[0] - a[0]) * (e[2] - a[2]) - (e[0] - a[0]) * (b[2] - a[2]);
        if (fabsf(d) < 1e-9f)
            continue;
        float u = ((x - a[0]) * (e[2] - a[2]) - (e[0] - a[0]) * (z - a[2])) / d;
        float v = ((b[0] - a[0]) * (z - a[2]) - (x - a[0]) * (b[2] - a[2])) / d;
        if (u < 0 || v < 0 || u + v > 1)
            continue;
        float hy = a[1] + u * (b[1] - a[1]) + v * (e[1] - a[1]);
        if (hy <= y + 0.5f && hy >= y - 3 && hy > by)
            by = hy, best = (int)t;
    }
    return best;
}

/* ---- rendering ---- */
/* Occlusion pre-tests (2026-11-07, build speed): the depth buffer holds 1 / depth and only grows, so the smallest value
 * of each TILE x TILE pixel tile (tmin, refreshed now and then: a stale one is never above the true one) bounds every
 * pixel under it. A triangle or a chunk whose nearest point lies clearly behind that bound over its whole screen
 * rectangle can neither write depth (mode 0) nor pass the biased test (mode 1), so it is skipped without changing any
 * result: only work that cannot change a pixel or a unit's mark is left out (margins PT_MARGIN cover the rounding of
 * the interpolated depth and of the projection). PVS_PRETEST=0 turns them off (A/B: pvs.bin must stay identical). */
#define TILE 8
#define PT_MARGIN 1e-3f
static int pretest = 1;
static int TW;  /* tiles per side */
typedef struct {
    float *zb;
    unsigned *seen;  /* per unit: stamp of the sample that saw it */
    unsigned *ib;    /* validate: unit per pixel */
    unsigned stamp;
    float *tmin;     /* per tile: lower bound of zb over it (valid when tvalid) */
    int tvalid;
} Ctx;

static void tiles_refresh(Ctx *c)
{
    for (int ty = 0; ty < TW; ty++)
        for (int tx = 0; tx < TW; tx++) {
            float m = 1e30f;
            int y1 = (ty + 1) * TILE < R ? (ty + 1) * TILE : R, x1 = (tx + 1) * TILE < R ? (tx + 1) * TILE : R;
            for (int y = ty * TILE; y < y1; y++)
                for (int x = tx * TILE; x < x1; x++)
                    m = c->zb[y * R + x] < m ? c->zb[y * R + x] : m;
            c->tmin[ty * TW + tx] = m;
        }
    c->tvalid = 1;
}

/* whether nothing with 1 / depth at most iz over pixels [x0, x1] x [y0, y1] can change the buffer (mode 0) or pass the
 * biased test (mode 1); needs valid tiles */
static int hidden_rect(const Ctx *c, int x0, int y0, int x1, int y1, float iz, int mode)
{
    iz *= 1 + PT_MARGIN;
    for (int ty = y0 / TILE; ty <= y1 / TILE; ty++)
        for (int tx = x0 / TILE; tx <= x1 / TILE; tx++) {
            float z = c->tmin[ty * TW + tx];
            if (z <= 0)
                return 0;
            if (mode == 0 ? !(iz < z * (1 - PT_MARGIN)) : !(iz < z / (1.01f + 0.02f * z) * (1 - PT_MARGIN)))
                return 0;
        }
    return 1;
}

static const float FB[6][9] = {{1, 0, 0, 0, 0, -1, 0, 1, 0}, {-1, 0, 0, 0, 0, 1, 0, 1, 0}, {0, 0, 1, 1, 0, 0, 0, 1, 0},
                               {0, 0, -1, -1, 0, 0, 0, 1, 0}, {0, 1, 0, 1, 0, 0, 0, 0, -1}, {0, -1, 0, 1, 0, 0, 0, 0, 1}};

static inline float box_d2(const float *mn, const float *mx, const float *p)
{
    float d2 = 0;
    for (int k = 0; k < 3; k++) {
        float o = p[k] < mn[k] ? mn[k] - p[k] : p[k] > mx[k] ? p[k] - mx[k] : 0;
        d2 += o * o;
    }
    return d2;
}

#ifdef __AVX2__
#include <immintrin.h>
/* One row of raster()'s modes 0 / 1 in AVX2, eight pixels at a time (2026-11-15, build speed): the scalar loop's exact
 * operations in its order (separate multiplies and adds, no FMA; !(w < 0) as not-less-than, which holds for NaN as the
 * scalar test does), lanes past x1 masked off; rows average 14 pixels. Asylum bake 426 -> 400 CPU s. PVS_SIMD=0: the
 * scalar loop (A/B: pvs.bin must stay identical). The depth buffer has 8 spare floats after its last row for the last
 * group's reads. */
static void row_avx2(float *zr, int x0, int x1, float py, float ax, float ay, float az, float bx, float by, float bz,
                     float ex, float ey, float ez, float ia, int mode, int *any, int *pass)
{
    const __m256 vbx = _mm256_set1_ps(bx), vex = _mm256_set1_ps(ex), vax = _mm256_set1_ps(ax);
    const __m256 ey_py = _mm256_set1_ps(ey - py), by_py = _mm256_set1_ps(by - py), ay_py = _mm256_set1_ps(ay - py);
    const __m256 via = _mm256_set1_ps(ia), one = _mm256_set1_ps(1.0f), zero = _mm256_setzero_ps(), half = _mm256_set1_ps(0.5f);
    const __m256 vaz = _mm256_set1_ps(az), vbz = _mm256_set1_ps(bz), vez = _mm256_set1_ps(ez);
    const __m256 k101 = _mm256_set1_ps(1.01f), k002 = _mm256_set1_ps(0.02f);
    const __m256i lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    __m256 anyv = zero, passv = zero;
    for (int x = x0; x <= x1; x += 8) {
        __m256i xi = _mm256_add_epi32(_mm256_set1_epi32(x), lane);
        __m256i inr = _mm256_cmpgt_epi32(_mm256_set1_epi32(x1 + 1), xi);  /* lanes x .. x1 */
        __m256 px = _mm256_add_ps(_mm256_cvtepi32_ps(xi), half);
        __m256 w0 = _mm256_mul_ps(_mm256_sub_ps(_mm256_mul_ps(_mm256_sub_ps(vbx, px), ey_py),
                                                _mm256_mul_ps(by_py, _mm256_sub_ps(vex, px))), via);
        __m256 w1 = _mm256_mul_ps(_mm256_sub_ps(_mm256_mul_ps(_mm256_sub_ps(vex, px), ay_py),
                                                _mm256_mul_ps(ey_py, _mm256_sub_ps(vax, px))), via);
        __m256 w2 = _mm256_sub_ps(_mm256_sub_ps(one, w0), w1);
        __m256 cov = _mm256_and_ps(_mm256_and_ps(_mm256_cmp_ps(w0, zero, _CMP_NLT_UQ), _mm256_cmp_ps(w1, zero, _CMP_NLT_UQ)),
                                   _mm256_cmp_ps(w2, zero, _CMP_NLT_UQ));
        cov = _mm256_and_ps(cov, _mm256_castsi256_ps(inr));
        __m256 iz = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(w0, vaz), _mm256_mul_ps(w1, vbz)), _mm256_mul_ps(w2, vez));
        __m256 z = _mm256_loadu_ps(zr + x);  /* (lanes past x1 read the next pixels, or the buffer's 8 spare floats) */
        if (mode == 0) {
            __m256 win = _mm256_and_ps(cov, _mm256_cmp_ps(iz, z, _CMP_GT_OQ));
            _mm256_storeu_ps(zr + x, _mm256_blendv_ps(z, iz, win));  /* (other lanes write back what they read) */
        } else {
            __m256 lim = _mm256_div_ps(z, _mm256_add_ps(k101, _mm256_mul_ps(k002, z)));
            __m256 ok = _mm256_or_ps(_mm256_cmp_ps(z, zero, _CMP_EQ_OQ), _mm256_cmp_ps(iz, lim, _CMP_GE_OQ));
            passv = _mm256_or_ps(passv, _mm256_and_ps(cov, ok));
        }
        anyv = _mm256_or_ps(anyv, cov);
    }
    *any |= _mm256_movemask_ps(anyv) != 0;
    *pass |= _mm256_movemask_ps(passv) != 0;
}
#endif
static int simd = 1;
/* mode 0: write depth; 1: test with bias, mark unit; 2: plain z test, write depth + unit id */
static void raster(Ctx *c, float (*p)[3], unsigned unit, int mode)
{
    float q[8][3];
    int m = 0;
    const float nz = 0.1f;
    for (int i = 0; i < 3; i++) {
        float *a = p[i], *b = p[(i + 1) % 3];
        if (a[2] >= nz)
            memcpy(q[m++], a, 12);
        if ((a[2] >= nz) != (b[2] >= nz)) {
            float t = (nz - a[2]) / (b[2] - a[2]);
            for (int k = 0; k < 3; k++)
                q[m][k] = a[k] + t * (b[k] - a[k]);
            m++;
        }
    }
    if (m < 3)
        return;
    float s[8][3];
    for (int i = 0; i < m; i++)
        s[i][0] = (q[i][0] / q[i][2] * 0.5f + 0.5f) * R, s[i][1] = (q[i][1] / q[i][2] * 0.5f + 0.5f) * R, s[i][2] = 1.0f / q[i][2];
    if (mode != 2 && c->tvalid) {  /* the whole polygon behind the tiles' bound: no fan triangle changes anything */
        float fx0 = s[0][0], fx1 = s[0][0], fy0 = s[0][1], fy1 = s[0][1], iz = s[0][2];
        for (int i = 1; i < m; i++)
            fx0 = fminf(fx0, s[i][0]), fx1 = fmaxf(fx1, s[i][0]), fy0 = fminf(fy0, s[i][1]), fy1 = fmaxf(fy1, s[i][1]),
            iz = fmaxf(iz, s[i][2]);
        if (fx1 < 0 || fy1 < 0 || fx0 >= R || fy0 >= R)
            return;
        if (hidden_rect(c, fx0 < 0 ? 0 : (int)fx0, fy0 < 0 ? 0 : (int)fy0, fx1 >= R ? R - 1 : (int)fx1,
                        fy1 >= R ? R - 1 : (int)fy1, iz, mode))
            return;
    }
    for (int t = 1; t + 1 < m; t++) {
        float *a = s[0], *b = s[t], *e = s[t + 1];
        float area = (b[0] - a[0]) * (e[1] - a[1]) - (b[1] - a[1]) * (e[0] - a[0]);
        if (fabsf(area) < 1e-12f)
            continue;
        float fx0 = fminf(a[0], fminf(b[0], e[0])), fx1 = fmaxf(a[0], fmaxf(b[0], e[0]));
        float fy0 = fminf(a[1], fminf(b[1], e[1])), fy1 = fmaxf(a[1], fmaxf(b[1], e[1]));
        if (fx1 < 0 || fy1 < 0 || fx0 >= R || fy0 >= R)
            continue;
        int x0 = fx0 < 0 ? 0 : (int)fx0, x1 = fx1 >= R ? R - 1 : (int)fx1;
        int y0 = fy0 < 0 ? 0 : (int)fy0, y1 = fy1 >= R ? R - 1 : (int)fy1;
        float ia = 1 / area;
        int hit = 0;
        if (mode != 2) {
            /* modes 0 / 1: the same per-pixel arithmetic without branches, so the compiler vectorises the row (the
             * depth buffer keeps the maximum, so a row's order does not matter; mode 1 only asks whether any pixel
             * passes) */
            const float ax = a[0], ay = a[1], az = a[2], bx = b[0], by = b[1], bz = b[2], ex = e[0], ey = e[1], ez = e[2];
            for (int y = y0; y <= y1; y++) {
                const float py = y + 0.5f;
                float *zr = &c->zb[y * R];
                int any = 0, pass = 0;
#ifdef __AVX2__
                if (simd) {
                    row_avx2(zr, x0, x1, py, ax, ay, az, bx, by, bz, ex, ey, ez, ia, mode, &any, &pass);
                    goto row_done;
                }
#endif
                for (int x = x0; x <= x1; x++) {
                    float px = x + 0.5f;
                    float w0 = ((bx - px) * (ey - py) - (by - py) * (ex - px)) * ia;
                    float w1 = ((ex - px) * (ay - py) - (ey - py) * (ax - px)) * ia;
                    float w2 = 1 - w0 - w1;
                    int cov = !(w0 < 0) & !(w1 < 0) & !(w2 < 0);
                    float iz = w0 * az + w1 * bz + w2 * ez;
                    float z = zr[x];
                    if (mode == 0) {
                        zr[x] = cov & (iz > z) ? iz : z;
                    } else {
                        /* passes when no farther than 1 % + 2 cm behind the occluder depth (decals, coplanar pieces) */
                        pass |= cov & ((z == 0) | (iz >= z / (1.01f + 0.02f * z)));
                    }
                    any |= cov;
                }
#ifdef __AVX2__
            row_done:
#endif
                if (pass) {
                    c->seen[unit] = c->stamp;
                    return;
                }
                hit |= any;
            }
        } else {  /* mode 2 (validate): as above; within a triangle each pixel is visited once, so order is kept */
            const float ax = a[0], ay = a[1], az = a[2], bx = b[0], by = b[1], bz = b[2], ex = e[0], ey = e[1], ez = e[2];
            for (int y = y0; y <= y1; y++) {
                const float py = y + 0.5f;
                float *zr = &c->zb[y * R];
                unsigned *ir = &c->ib[y * R];
                for (int x = x0; x <= x1; x++) {
                    float px = x + 0.5f;
                    float w0 = ((bx - px) * (ey - py) - (by - py) * (ex - px)) * ia;
                    float w1 = ((ex - px) * (ay - py) - (ey - py) * (ax - px)) * ia;
                    float w2 = 1 - w0 - w1;
                    int win = !(w0 < 0) & !(w1 < 0) & !(w2 < 0) & (w0 * az + w1 * bz + w2 * ez > zr[x]);
                    float iz = w0 * az + w1 * bz + w2 * ez;
                    zr[x] = win ? iz : zr[x];
                    ir[x] = win ? unit : ir[x];
                }
            }
        }
        /* a triangle that covers no pixel centre: its first vertex's pixel stands for it (sub-pixel slivers) */
        if (!hit && mode == 1) {
            int x = (int)a[0], y = (int)a[1];
            if (x >= 0 && y >= 0 && x < R && y < R) {
                float z = c->zb[y * R + x];
                if (z == 0 || a[2] >= z / (1.01f + 0.02f * z)) {
                    c->seen[unit] = c->stamp;
                    return;
                }
            }
        }
    }
}

/* whether map.c draws chunk i from eye e with the player on block pb, collision mask cm: 0 no, 1 yes (only tested:
 * block 0 seen from another block, whose draw groups there are unknown, never hides anything), 2 yes and occluding */
static inline int drawn(unsigned i, const float *e, int pb, int cm)
{
    const InChunk *k = &ch[i];
    int b = cblk[i];
    if (k->unit == 0xFFFFFFFFu || (k->flags >> 8 & CHUNK_SKY) || (b && b != pb))
        return 0;
    if (b == pb && cm >= 0 && bshown[b] && !bshown[b][(size_t)cm * bnp[b] + cdg[i]])
        return 0;
    if (!(k->flags >> 8 & CHUNK_BACKDROP) && box_d2(k->mn, k->mx, e) > CULL_FAR * CULL_FAR)
        return 0;
    if (k->flags & 6) {
        float d2 = box_d2(un[cunit[i]].mn, un[cunit[i]].mx, e);
        if ((k->flags & 2) && d2 > NEAR_OUT * NEAR_OUT)
            return 0;
        if ((k->flags & 4) && d2 < NEAR_IN * NEAR_IN)
            return 0;
    }
    return b == pb && (k->flags & 1) ? 2 : 1;
}

static int box_in_face(const float *mn, const float *mx, const float *e, const float *fb)
{
    int out[5] = {1, 1, 1, 1, 1};
    for (int c = 0; c < 8; c++) {
        float d[3] = {(c & 1 ? mx[0] : mn[0]) - e[0], (c & 2 ? mx[1] : mn[1]) - e[1], (c & 4 ? mx[2] : mn[2]) - e[2]};
        float x = d[0] * fb[3] + d[1] * fb[4] + d[2] * fb[5], y = d[0] * fb[6] + d[1] * fb[7] + d[2] * fb[8];
        float z = d[0] * fb[0] + d[1] * fb[1] + d[2] * fb[2];
        out[0] &= z < 0.1f, out[1] &= x > z, out[2] &= -x > z, out[3] &= y > z, out[4] &= -y > z;
    }
    return !(out[0] | out[1] | out[2] | out[3] | out[4]);
}

static void draw_chunk(Ctx *c, unsigned i, const float *e, const float *fb, int mode)
{
    const InChunk *k = &ch[i];
    for (unsigned t = k->first; t < k->first + k->ntri; t++) {
        float p[3][3];
        const float *v = tri[t].v;
        for (int j = 0; j < 3; j++) {
            float d[3] = {v[3 * j] - e[0], v[3 * j + 1] - e[1], v[3 * j + 2] - e[2]};
            p[j][0] = d[0] * fb[3] + d[1] * fb[4] + d[2] * fb[5];
            p[j][1] = d[0] * fb[6] + d[1] * fb[7] + d[2] * fb[8];
            p[j][2] = d[0] * fb[0] + d[1] * fb[1] + d[2] * fb[2];
        }
        raster(c, p, k->unit, mode);
        if (mode == 1 && c->seen[k->unit] == c->stamp)
            return;
    }
}

#define NEAR_FIRST 32.0f
/* the chunks drawn from camera e (player on block pb, mask cm) into list, bit 31 = occluder, bit 30 = within
 * NEAR_FIRST m (drawn first); their count */
static unsigned *blist[MAXB], bnl[MAXB];  /* per player block: its chunks and block 0's, ascending (drawn() skips the rest) */

static void build_blists(void)
{
    if (getenv("PVS_BLIST") && !strcmp(getenv("PVS_BLIST"), "0"))
        return;
    for (int b = 0; b < nblk; b++) {
        blist[b] = malloc(sizeof(unsigned) * (nch ? nch : 1)), bnl[b] = 0;
        for (unsigned j = 0; j < nch; j++)
            if (!cblk[j] || cblk[j] == b)
                blist[b][bnl[b]++] = j;
    }
}

static unsigned draw_list(const float *e, int pb, int cm, unsigned *list)
{
    unsigned n = 0;
    /* the same chunks in the same order as the full loop: a chunk of another block than 0 and pb is never drawn */
    const int bl = pb >= 0 && pb < nblk && blist[pb];
    const unsigned m = bl ? bnl[pb] : nch;
    for (unsigned jj = 0; jj < m; jj++) {
        const unsigned j = bl ? blist[pb][jj] : jj;
        int d = drawn(j, e, pb, cm);
        if (d)
            list[n++] = j | (d == 2 ? 0x80000000u : 0) |
                        (box_d2(ch[j].mn, ch[j].mx, e) < NEAR_FIRST * NEAR_FIRST ? 0x40000000u : 0);
    }
    return n;
}

/* box_in_face for all six faces at once (bit f: face f): the FB axes are signed unit axes, so each face's x / y / z of a
 * corner is a component of d or its negation, exactly the value the dot product gives (adding zeros changes no compare) */
/* FB's rows as (axis, sign): every row of FB is one signed unit axis, so a face's x / y / z of a corner is exactly
 * +-(corner - eye)[axis], and "every corner fails test t" is a comparison of the per-axis extremes (the corners take
 * each axis' min or max independently): x > z at all 8 corners <=> min x > max z. The same answers as the per-corner
 * loop with 6 x 4 comparisons instead of 6 x 8 x 9 products (box_faces was 5 % of a cold world build's CPU samples,
 * 2026-11-24); a box with a non-finite corner keeps the loop (0 x inf). PVS_BOXF_LOOP=1: always the loop. */
static int fb_ax[6][3], fb_sg[6][3], fb_fast = -1;

static void fb_axes(void)
{
    const char *lp = getenv("PVS_BOXF_LOOP");
    fb_fast = !(lp && *lp == '1');
    for (int f = 0; f < 6 && fb_fast; f++)
        for (int r = 0; r < 3; r++) {  /* r: 0 z, 1 x, 2 y */
            int nz = 0;
            for (int k = 0; k < 3; k++)
                if (FB[f][3 * r + k] != 0) {
                    nz++, fb_ax[f][r] = k, fb_sg[f][r] = FB[f][3 * r + k] > 0 ? 1 : -1;
                    if (FB[f][3 * r + k] != 1 && FB[f][3 * r + k] != -1)
                        nz = 9;
                }
            if (nz != 1)
                fb_fast = 0;
        }
}

static unsigned box_faces_loop(const float *mn, const float *mx, const float *e);

static unsigned box_faces(const float *mn, const float *mx, const float *e)
{
    if (fb_fast < 0)
        fb_axes();
    float lo[3], hi[3];
    for (int k = 0; k < 3; k++)
        lo[k] = mn[k] - e[k], hi[k] = mx[k] - e[k];
    if (!fb_fast || !isfinite(lo[0]) || !isfinite(lo[1]) || !isfinite(lo[2]) || !isfinite(hi[0]) || !isfinite(hi[1]) ||
        !isfinite(hi[2]))
        return box_faces_loop(mn, mx, e);
    unsigned m = 0;
    for (int f = 0; f < 6; f++) {
        float rmin[3], rmax[3];
        for (int r = 0; r < 3; r++) {
            int a = fb_ax[f][r];
            if (fb_sg[f][r] > 0)
                rmin[r] = lo[a], rmax[r] = hi[a];
            else
                rmin[r] = -hi[a], rmax[r] = -lo[a];
        }
        float zmax = rmax[0];
        int out = zmax < 0.1f || rmin[1] > zmax || -rmax[1] > zmax || rmin[2] > zmax || -rmax[2] > zmax;
        m |= (unsigned)!out << f;
    }
    return m;
}

static unsigned box_faces_loop(const float *mn, const float *mx, const float *e)
{
    /* box_in_face per face, with the corners' offsets taken once: every FB entry is 0 or +-1, so its dot products are
     * exactly a signed component (a zero's sign never decides a comparison) */
    float d[8][3];
    for (int c = 0; c < 8; c++)
        d[c][0] = (c & 1 ? mx[0] : mn[0]) - e[0], d[c][1] = (c & 2 ? mx[1] : mn[1]) - e[1], d[c][2] = (c & 4 ? mx[2] : mn[2]) - e[2];
    unsigned m = 0;
    for (int f = 0; f < 6; f++) {
        const float *fb = FB[f];
        int out0 = 1, out1 = 1, out2 = 1, out3 = 1, out4 = 1;
        for (int c = 0; c < 8; c++) {
            float x = d[c][0] * fb[3] + d[c][1] * fb[4] + d[c][2] * fb[5], y = d[c][0] * fb[6] + d[c][1] * fb[7] + d[c][2] * fb[8];
            float z = d[c][0] * fb[0] + d[c][1] * fb[1] + d[c][2] * fb[2];
            out0 &= z < 0.1f, out1 &= x > z, out2 &= -x > z, out3 &= y > z, out4 &= -y > z;
        }
        m |= (unsigned)!(out0 | out1 | out2 | out3 | out4) << f;
    }
    return m;
}

/* per chunk: the box of its own triangles (tight, from the vertices themselves), for the chunk pre-tests */
static float (*tb)[2][3];

/* whether chunk i, seen from e through face fb, lies clearly behind the tiles' bound (see hidden_rect) */
static int chunk_hidden(const Ctx *c, unsigned i, const float *e, const float *fb, int mode)
{
    float zmin = 1e30f, x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    const float *mn = tb[i][0], *mx = tb[i][1];
    for (int k = 0; k < 8; k++) {
        float d[3] = {(k & 1 ? mx[0] : mn[0]) - e[0], (k & 2 ? mx[1] : mn[1]) - e[1], (k & 4 ? mx[2] : mn[2]) - e[2]};
        float x = d[0] * fb[3] + d[1] * fb[4] + d[2] * fb[5], y = d[0] * fb[6] + d[1] * fb[7] + d[2] * fb[8];
        float z = d[0] * fb[0] + d[1] * fb[1] + d[2] * fb[2];
        if (z < 0.2f)  /* near the camera plane (clipping): never judged */
            return 0;
        float sx = (x / z * 0.5f + 0.5f) * R, sy = (y / z * 0.5f + 0.5f) * R;
        zmin = fminf(zmin, z), x0 = fminf(x0, sx), x1 = fmaxf(x1, sx), y0 = fminf(y0, sy), y1 = fmaxf(y1, sy);
    }
    x0 -= 1, y0 -= 1, x1 += 1, y1 += 1;  /* rounding of the projection */
    if (x1 < 0 || y1 < 0 || x0 >= R || y0 >= R)
        return 1;
    return hidden_rect(c, x0 < 0 ? 0 : (int)x0, y0 < 0 ? 0 : (int)y0, x1 >= R ? R - 1 : (int)x1, y1 >= R ? R - 1 : (int)y1,
                       1.0f / zmin * (1 + PT_MARGIN), mode);
}

/* every block-0 unit visible from eye e: c->seen[u] == c->stamp. Occluders within NEAR_FIRST m go first (bit 30 of
 * the list), then the tiles bound the rest; the order of depth writes changes nothing (the buffer keeps the maximum). */
static void sample(Ctx *c, const float *e, unsigned *list, unsigned nlist, unsigned char *fm, const unsigned char *row)
{
    /* row (the cell's row so far, or NULL): its units need no test; a face with nothing left to test draws nothing */
#define SEEN_ROW(u) (row && (row[(u) >> 3] >> ((u) & 7) & 1))
    for (unsigned j = 0; j < nlist; j++) {
        unsigned i = list[j] & 0x3FFFFFFFu;
        fm[j] = (unsigned char)box_faces(ch[i].mn, ch[i].mx, e);
    }
    for (int f = 0; f < 6; f++) {
        const float *fb = FB[f];
        memset(c->zb, 0, sizeof(float) * R * R);
        c->tvalid = 0;
        unsigned n = 0;
        unsigned nt = 0;
        for (unsigned j = 0; j < nlist; j++)
            if (fm[j] >> f & 1) {
                unsigned i = list[j] & 0x3FFFFFFFu;
                list[nlist + n++] = list[j];
                nt += i < nself && !SEEN_ROW(ch[i].unit);
            }
        if (!nt)
            continue;
        for (int pass = 0; pass < 2; pass++) {
            unsigned since = 0;
            for (unsigned j = 0; j < n; j++) {
                unsigned v = list[nlist + j], i = v & 0x3FFFFFFFu;
                if (!(v >> 31) || (int)(v >> 30 & 1) != !pass)
                    continue;
                if (pretest && pass && (++since & 127) == 0)
                    tiles_refresh(c);
                if (c->tvalid && chunk_hidden(c, i, e, fb, 0))
                    continue;
                draw_chunk(c, i, e, fb, 0);
            }
            if (pretest)
                tiles_refresh(c);
        }
        for (unsigned j = 0; j < n; j++) {
            unsigned i = list[nlist + j] & 0x3FFFFFFFu;
            if (i < nself && c->seen[ch[i].unit] != c->stamp && !SEEN_ROW(ch[i].unit) && !(c->tvalid && chunk_hidden(c, i, e, fb, 1)))
                draw_chunk(c, i, e, fb, 1);
        }
    }
#undef SEEN_ROW
}

/* ---- samples ---- */
typedef struct { float e[3]; int cm, b; } Cam;
static inline uint32_t rnd(uint64_t *s)
{
    *s ^= *s << 13, *s ^= *s >> 7, *s ^= *s << 17;
    return (uint32_t)(*s >> 16);
}
static inline float frnd(uint64_t *s) { return (rnd(s) & 0xFFFFFF) / 16777216.0f; }
static inline uint32_t cell_key(const float *e)
{
    int x = (int)floorf(e[0] / cellsz) + 512, y = (int)floorf(e[1] / cellsz) + 512, z = (int)floorf(e[2] / cellsz) + 512;
    if (x < 0 || y < 0 || z < 0 || x > 1023 || y > 1023 || z > 1023)
        return 0xFFFFFFFFu;
    return (uint32_t)x | (uint32_t)y << 10 | (uint32_t)z << 20;
}

static float (*pp)[3];  /* player positions */
static int *pcm, *pbk;
static unsigned npp;
static float sbox[2][3];  /* block 0's chunks (backdrop left out): players within CULL_FAR of it count */

static void players(void)
{
    unsigned cap = 1 << 16;
    pp = malloc(cap * sizeof *pp), pcm = malloc(cap * sizeof *pcm), pbk = malloc(cap * sizeof *pbk);
    /* hash of 2 m x 1.5 m buckets */
    unsigned hcap = 1 << 22;
    uint64_t *hs = calloc(hcap, sizeof *hs);
    for (unsigned t = 0; t < nct; t++) {
        if (!(COL_SOLID >> ct[t].hf & 1))
            continue;
        float *a = cv[ct[t].v[0]], *b = cv[ct[t].v[1]], *e = cv[ct[t].v[2]];
        if (ct[t].blk && box_d2(sbox[0], sbox[1], a) > CULL_FAR * CULL_FAR)
            continue;  /* a neighbour's floor too far to see block 0 from */
        float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {e[0] - a[0], e[1] - a[1], e[2] - a[2]};
        float n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 1e-9f || fabsf(n[1]) / nl < 0.6f)
            continue;  /* walls and steep slopes */
        float x0 = fminf(a[0], fminf(b[0], e[0])), x1 = fmaxf(a[0], fmaxf(b[0], e[0]));
        float z0 = fminf(a[2], fminf(b[2], e[2])), z1 = fmaxf(a[2], fmaxf(b[2], e[2]));
        float d = u[0] * w[2] - w[0] * u[2];
        int any = 0;
        for (int pass = 0; pass < 2 && !any; pass++)
            for (float z = pass ? (z0 + z1) / 2 : ceilf(z0 / 2) * 2; z <= (pass ? (z0 + z1) / 2 : z1); z += 2)
                for (float x = pass ? (x0 + x1) / 2 : ceilf(x0 / 2) * 2; x <= (pass ? (x0 + x1) / 2 : x1); x += 2) {
                    float bu = ((x - a[0]) * w[2] - w[0] * (z - a[2])) / d, bv = (u[0] * (z - a[2]) - (x - a[0]) * u[2]) / d;
                    if (!pass && (bu < 0 || bv < 0 || bu + bv > 1))
                        continue;
                    float y = a[1] + bu * u[1] + bv * w[1];
                    int64_t kx = (int64_t)floorf(x / 2), kz = (int64_t)floorf(z / 2), ky = (int64_t)floorf(y / 1.5f);
                    uint64_t key = ((uint64_t)(kx + (1 << 20)) << 42 | (uint64_t)(ky + (1 << 20)) << 21 | (uint64_t)(kz + (1 << 20))) + 1;
                    uint64_t h = key * 0x9E3779B97F4A7C15ull;
                    unsigned s = (unsigned)(h >> 40) & (hcap - 1);
                    while (hs[s] && hs[s] != key)
                        s = (s + 1) & (hcap - 1);
                    any = 1;
                    if (hs[s])
                        continue;
                    hs[s] = key;
                    if (npp == cap)
                        cap *= 2, pp = realloc(pp, cap * sizeof *pp), pcm = realloc(pcm, cap * sizeof *pcm),
                        pbk = realloc(pbk, cap * sizeof *pbk);
                    pp[npp][0] = x, pp[npp][1] = y, pp[npp][2] = z;
                    int ft = col_floor(x, y + 0.05f, z);
                    unsigned f = ft >= 0 ? (unsigned)ft : t;
                    pbk[npp] = ct[f].blk, pcm[npp] = ct[f].dg == 0xFF ? -1 : ct[f].dg;
                    npp++;
                }
    }
    free(hs);
}

static Cam camera(unsigned i, uint64_t *s)
{
    Cam c;
    float tg[3] = {pp[i][0], pp[i][1] + CAM_H, pp[i][2]};
    float yaw = frnd(s) * 6.2831853f, pitch = -0.35f + frnd(s) * 1.25f;
    float d[3] = {-sinf(yaw) * cosf(pitch) * CAM_D, sinf(pitch) * CAM_D, -cosf(yaw) * cosf(pitch) * CAM_D};
    float e[3] = {tg[0] + d[0], tg[1] + d[1], tg[2] + d[2]};
    float t = col_ray(tg, e), dist = CAM_D;
    if (t <= 1)
        dist = t * CAM_D - 0.3f > 0.2f ? t * CAM_D - 0.3f : 0.2f;
    for (int k = 0; k < 3; k++)
        c.e[k] = tg[k] + d[k] * dist / CAM_D;
    c.cm = pcm[i], c.b = pbk[i];
    return c;
}

/* ---- cell table ---- */
typedef struct { uint32_t key; unsigned n, cap; Cam *cam; unsigned char *row; int sampled; } Cell;
static Cell *cells;
static unsigned ncell, cellcap, hcap2;
static int *chash;
static int cell_find(uint32_t key, int add)
{
    uint64_t h = (uint64_t)key * 0x9E3779B97F4A7C15ull;
    unsigned s = (unsigned)(h >> 40) & (hcap2 - 1);
    while (chash[s] >= 0 && cells[chash[s]].key != key)
        s = (s + 1) & (hcap2 - 1);
    if (chash[s] >= 0 || !add)
        return chash[s];
    if (ncell == cellcap)
        cellcap = cellcap ? cellcap * 2 : 1024, cells = realloc(cells, cellcap * sizeof *cells);
    memset(&cells[ncell], 0, sizeof *cells);
    cells[ncell].key = key;
    chash[s] = (int)ncell;
    return (int)ncell++;
}

static const int NB[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

/* the full-unit row a camera in cell key sees: its own, else the union of its stored neighbours (into tmp), else NULL
 * (everything drawn); as src/pvs.c */
static const unsigned char *row_at(uint32_t key, unsigned char *tmp, unsigned rb)
{
    if (key == 0xFFFFFFFFu)
        return NULL;
    int c = cell_find(key, 0);
    if (c >= 0)
        return cells[c].row;
    int x = key & 1023, y = key >> 10 & 1023, z = key >> 20 & 1023, any = 0;
    memset(tmp, 0, rb);
    for (int d = 0; d < 6; d++) {
        int nx = x + NB[d][0], ny = y + NB[d][1], nz = z + NB[d][2];
        if (nx < 0 || ny < 0 || nz < 0 || nx > 1023 || ny > 1023 || nz > 1023)
            continue;
        int j = cell_find((uint32_t)nx | (uint32_t)ny << 10 | (uint32_t)nz << 20, 0);
        if (j >= 0) {
            any = 1;
            for (unsigned b = 0; b < rb; b++)
                tmp[b] |= cells[j].row[b];
        }
    }
    return any ? tmp : NULL;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = cells[*(const unsigned *)a].key, y = cells[*(const unsigned *)b].key;
    return x < y ? -1 : x > y;
}

/* append a pvsin.bin's chunks, units and triangles as block blk (0: the baked block; others: units FOREIGN) */
static int load_in(const unsigned char *in, size_t n, int blk)
{
    const unsigned *h = (const unsigned *)in;
    unsigned c = h[1], u = h[2], t = h[3], nnv = h[4], nnt = h[5];
    const unsigned char *p = in + 32;
    const InChunk *ic = (const InChunk *)p;
    p += (size_t)c * sizeof *ic;
    const InUnit *iu = (const InUnit *)p;
    p += (size_t)u * sizeof *iu;
    unsigned nparts = *(const unsigned *)p;
    p += 4;
    const unsigned char *pn0 = p;
    for (unsigned i = 0; i < nparts; i++)
        p += strlen((const char *)p) + 1;
    p += (4 - ((p - pn0 + 4) & 3)) & 3;
    p += (size_t)nnv * 12 + (size_t)nnt * 12;
    if (p + (size_t)t * sizeof(Tri9) > in + n)
        return 0;
    unsigned c0 = nch, u0 = ncunits_all, t0 = ntri;
    ch = realloc(ch, (nch + c) * sizeof *ch), cunit = realloc(cunit, (nch + c) * sizeof *cunit);
    cblk = realloc(cblk, nch + c), cdg = realloc(cdg, nch + c);
    un = realloc(un, (ncunits_all + u) * sizeof *un), tri = realloc(tri, (ntri + t) * sizeof *tri);
    memcpy(ch + c0, ic, c * sizeof *ic);
    memcpy(un + u0, iu, u * sizeof *iu);
    memcpy(tri + t0, p, t * sizeof(Tri9));
    for (unsigned i = 0; i < c; i++) {
        InChunk *k = &ch[c0 + i];
        k->first += t0;
        cunit[c0 + i] = k->unit == 0xFFFFFFFFu ? 0 : u0 + k->unit;
        if (blk && k->unit != 0xFFFFFFFFu)
            k->unit = FOREIGN;
        cblk[c0 + i] = (unsigned char)blk, cdg[c0 + i] = 0;
    }
    nch += c, ncunits_all += u, ntri += t;
    if (!blk)
        nun = u;
    return 1;
}

/* block blk's dg.bin: its chunks [c0, c0 + nc), collision triangles [t0, t0 + nt) */
static void load_dg(const char *path, int blk, unsigned c0, unsigned nc, unsigned t0, unsigned nt)
{
    size_t dn;
    unsigned char *dgb = slurp(path, &dn);
    ChunkFile f;  /* dg.bin: chunk file 'DGRP' (tools/drawgroups.py) */
    unsigned szh, szp, szc, szcd, sztd;
    const unsigned *dh = NULL;
    const unsigned (*pm)[4] = NULL, (*cmk)[4] = NULL;
    const unsigned char *cd = NULL, *td = NULL;
    if (chunk_open(&f, dgb, (unsigned)dn, CHUNK_ID('D', 'G', 'R', 'P'), path)) {
        dh = chunk_get(&f, CHUNK_ID('D', 'H', 'D', 'R'), 1, &szh);
        pm = chunk_get(&f, CHUNK_ID('P', 'M', 'S', 'K'), 1, &szp);
        cmk = chunk_get(&f, CHUNK_ID('C', 'M', 'S', 'K'), 1, &szc);
        cd = chunk_get(&f, CHUNK_ID('C', 'H', 'D', 'G'), 1, &szcd);
        td = chunk_get(&f, CHUNK_ID('T', 'R', 'D', 'G'), 1, &sztd);
    }
    if (!dh || !pm || !cmk || !cd || !td) {
        fprintf(stderr, "pvs_bake: %s: not a dg.bin this bake reads\n", path);
        exit(1);
    }
    /* DHDR: pieces, colls, chunks, tris, ... */
    if (dh[2] < nc || dh[3] != nt || szcd != dh[2] || sztd != dh[3]) {
        fprintf(stderr, "pvs_bake: %s: %u chunks / %u collision tris for %u / %u: draw groups ignored\n", path, dh[2],
                dh[3], nc, nt);
        return;
    }
    unsigned np = dh[0], ncl = dh[1];
    if (szp != 16 * np || szc != 16 * ncl) {
        fprintf(stderr, "pvs_bake: %s: mask chunks do not match DHDR\n", path);
        exit(1);
    }
    bshown[blk] = malloc((size_t)ncl * np + 1), bnp[blk] = np, bnc[blk] = ncl;
    for (unsigned c = 0; c < ncl; c++)
        for (unsigned i = 0; i < np; i++)
            bshown[blk][(size_t)c * np + i] = (pm[i][0] & cmk[c][0]) | (pm[i][1] & cmk[c][1]) |
                                              (pm[i][2] & cmk[c][2]) | (pm[i][3] & cmk[c][3]) ? 1 : 0;
    for (unsigned i = 0; i < nc; i++)
        cdg[c0 + i] = cd[i];
    for (unsigned i = 0; i < nt; i++)
        ct[t0 + i].dg = td[i];
}

#ifdef PVS_GPU
#include "pvs_gpu.h"
/* the sampling on the GPU (tools/pvs_gpu.c): cameras in batches; per camera the CPU makes the same draw list and face
 * masks as sample() (occluders of every block, then the block-0 chunks to test, per cube face), the GPU draws and
 * returns the units seen. 0 when no GPU (PVS_GPU=0, no EGL device, a software renderer): the CPU path runs. */
static unsigned *g_cf, *g_cn, *g_cu;  /* per chunk for the GPU: first triangle, triangles, unit */
static int gpu_setup(int res)
{
    const char *g = getenv("PVS_GPU");
    if (g && *g == '0')
        return 0;
    g_cf = malloc(sizeof(unsigned) * (nch ? nch : 1)), g_cn = malloc(sizeof(unsigned) * (nch ? nch : 1));
    g_cu = malloc(sizeof(unsigned) * (nch ? nch : 1));
    for (unsigned i = 0; i < nch; i++)
        g_cf[i] = ch[i].first, g_cn[i] = ch[i].ntri, g_cu[i] = ch[i].unit < nun ? ch[i].unit : 0;
    if (!gpu_init(res, (const float *)tri, ntri, g_cf, g_cn, g_cu, nch, nun)) {
        printf("pvs_bake: no GPU (OpenGL 4.5 over EGL): sampling on the CPU\n");
        return 0;
    }
    return 1;
}

/* the nearest (far = 0) or farthest (far = 1) depth of box mn..mx along cube face f's axis from e (FB[f]'s z axis is a
 * signed unit axis) */
static inline float face_z(const float *mn, const float *mx, const float *e, int f, int far)
{
    for (int k = 0; k < 3; k++)
        if (FB[f][k] != 0) {
            float a = FB[f][k] > 0 ? mn[k] - e[k] : e[k] - mx[k], b = FB[f][k] > 0 ? mx[k] - e[k] : e[k] - mn[k];
            return far ? b : a;
        }
    return 0;
}

static void gpu_bake(int per_cell, unsigned long long *tested)
{
    const char *oc = getenv("PVS_OCC_CULL");
    const int occ_cull = !(oc && *oc == '0');
    int W = gpu_words();
    /* every camera sampled: (cell, camera) */
    unsigned ncam = 0;
    for (unsigned i = 0; i < ncell; i++)
        ncam += cells[i].n < (unsigned)per_cell ? cells[i].n : (unsigned)per_cell;
    unsigned *cc = malloc(sizeof(unsigned) * (ncam ? ncam : 1)), *cs = malloc(sizeof(unsigned) * (ncam ? ncam : 1)), k = 0;
    /* camera s of every cell before camera s + 1 of any (PVS_ROUNDS=0: a cell's cameras in a row): a test chunk whose
     * unit the cell's row already has is not drawn again (the row is the OR of its cameras), so later rounds draw
     * little more than what no camera of the cell has seen yet. The same rows */
    const char *rv = getenv("PVS_ROUNDS");
    const int rounds = !(rv && *rv == '0');
    if (rounds) {
        for (unsigned s2 = 0; s2 < (unsigned)per_cell; s2++)
            for (unsigned i = 0; i < ncell; i++)
                if (s2 < cells[i].n)
                    cc[k] = i, cs[k] = s2, k++;
    } else
        for (unsigned i = 0; i < ncell; i++) {
            unsigned m = cells[i].n < (unsigned)per_cell ? cells[i].n : (unsigned)per_cell;
            for (unsigned s2 = 0; s2 < m; s2++)
                cc[k] = i, cs[k] = s2, k++;
        }
    enum { B = 64 };  /* cameras per batch (pvs_gpu.c LCAMS): its command lists stay small under tools/q's address cap */
    unsigned *lst[B], nlst[B];
    unsigned *off = malloc(sizeof(unsigned) * (12 * B + 1));
    float (*eye)[3] = malloc(sizeof(float[3]) * B);
    uint32_t *bits = malloc(sizeof(uint32_t) * (size_t)B * (size_t)W);
    unsigned *idx = NULL;
    size_t idxcap = 0;
    int pend = -1, pnb = 0, inst = gpu_instanced();
    /* a collected batch's bits into its cells' rows */
#define take(b0_, nb_) do { \
        for (int c = 0; c < (nb_); c++) { \
            Cell *ce = &cells[cc[(b0_) + c]]; \
            const uint32_t *bc = bits + (size_t)c * W; \
            for (unsigned u = 0; u < nun; u++) \
                if (bc[u >> 5] >> (u & 31) & 1) \
                    ce->row[u >> 3] |= 1 << (u & 7); \
        } \
        *tested += (unsigned long long)(nb_); \
    } while (0)
    for (unsigned b0 = 0; b0 < ncam; b0 += B) {
        int nb = ncam - b0 < B ? (int)(ncam - b0) : B;
#pragma omp parallel
        {
            unsigned *list = malloc(sizeof(unsigned) * (nch ? nch : 1));
            unsigned char *fm = malloc(nch + 1), *om = malloc(nch + 1);
#pragma omp for schedule(dynamic, 1)
            for (int c = 0; c < nb; c++) {
                const Cam *cm = &cells[cc[b0 + c]].cam[cs[b0 + c]];
                const unsigned char *crow = rounds ? cells[cc[b0 + c]].row : NULL;  /* (written only between batches) */
                unsigned nl = draw_list(cm->e, cm->b, cm->cm, list), n = 0;
                /* bit 30 (the CPU path's near-first flag, unused here) = not tested: a unit the cell has already seen.
                 * Such a chunk stays in the list only as an occluder */
                {
                    unsigned w2 = 0;
                    for (unsigned j = 0; j < nl; j++) {
                        unsigned i = list[j] & 0x3FFFFFFFu, v = list[j] & ~0x40000000u;
                        if (crow && i < nself && (crow[ch[i].unit >> 3] >> (ch[i].unit & 7) & 1)) {
                            if (!(v >> 31))
                                continue;
                            v |= 0x40000000u;
                        }
                        list[w2++] = v;
                    }
                    nl = w2;
                }
                for (unsigned j = 0; j < nl; j++)  /* (instanced: face 0's lists stand for all six) */
                    fm[j] = inst ? 1 : (unsigned char)box_faces(ch[list[j] & 0x3FFFFFFFu].mn, ch[list[j] & 0x3FFFFFFFu].mx, cm->e);
                unsigned *o = malloc(sizeof(unsigned) * (12 * (size_t)nl + 13)), cnt[12] = {0};
                /* occluder faces (om): only faces with something to test, and only occluders not wholly behind every
                 * test of that face (they cannot hide a test fragment: the depth image keeps the nearest occluder and
                 * a test passes in front of it). The same bits; PVS_OCC_CULL=0 draws them all */
                float tz[6];
                for (int f = 0; f < 6; f++)
                    tz[f] = -1e30f;
                for (unsigned j = 0; j < nl; j++) {
                    unsigned i = list[j] & 0x3FFFFFFFu;
                    if (i >= nself || (list[j] >> 30 & 1) || !ch[i].ntri)
                        continue;
                    for (int f = 0; f < 6; f++)
                        if (fm[j] >> f & 1)
                            tz[f] = fmaxf(tz[f], inst ? 1e30f : face_z(ch[i].mn, ch[i].mx, cm->e, f, 1));
                }
                for (unsigned j = 0; j < nl; j++) {
                    unsigned i = list[j] & 0x3FFFFFFFu;
                    om[j] = 0;
                    if (!(list[j] >> 31) || !ch[i].ntri)
                        continue;
                    for (int f = 0; f < 6; f++)
                        if ((fm[j] >> f & 1) &&
                            (!occ_cull || (tz[f] > -1e30f && (inst || face_z(ch[i].mn, ch[i].mx, cm->e, f, 0) <= tz[f] * 1.001f + 0.01f))))
                            om[j] |= (unsigned char)(1u << f);
                }
                /* list q = 2 f (occluders of face f) or 2 f + 1 (tests): counted, then filled */
                for (unsigned j = 0; j < nl; j++) {
                    unsigned i = list[j] & 0x3FFFFFFFu;
                    if (!ch[i].ntri)
                        continue;
                    for (int f = 0; f < 6; f++) {
                        if (om[j] >> f & 1) cnt[2 * f]++;
                        if ((fm[j] >> f & 1) && i < nself && !(list[j] >> 30 & 1)) cnt[2 * f + 1]++;
                    }
                }
                unsigned st[13];
                st[0] = 13;
                for (int q = 0; q < 12; q++)
                    st[q + 1] = st[q] + cnt[q];
                unsigned wr[12];
                for (int q = 0; q < 12; q++)
                    wr[q] = st[q];
                for (unsigned j = 0; j < nl; j++) {
                    unsigned i = list[j] & 0x3FFFFFFFu;
                    if (!ch[i].ntri)
                        continue;
                    for (int f = 0; f < 6; f++) {
                        if (om[j] >> f & 1) o[wr[2 * f]++] = i;
                        if ((fm[j] >> f & 1) && i < nself && !(list[j] >> 30 & 1)) o[wr[2 * f + 1]++] = i;
                    }
                }
                for (int q = 0; q < 13; q++)
                    o[q] = st[q];  /* header: the 12 lists' starts and the end, from 13 */
                n = st[12];
                lst[c] = o, nlst[c] = n;
                memcpy(eye[c], cm->e, sizeof eye[c]);
            }
            free(list), free(fm), free(om);
        }
        size_t tot = 0;
        for (int c = 0; c < nb; c++)
            tot += nlst[c] - 13;
        if (tot > idxcap)
            idxcap = tot + tot / 2, idx = realloc(idx, sizeof(unsigned) * (idxcap ? idxcap : 1));
        size_t w = 0;
        for (int c = 0; c < nb; c++) {
            const unsigned *o = lst[c];
            for (int q = 0; q < 12; q++)
                off[12 * c + q] = (unsigned)(w + o[q] - 13);
            memcpy(idx + w, o + 13, sizeof(unsigned) * (nlst[c] - 13));
            w += nlst[c] - 13;
            free(lst[c]);
        }
        off[12 * nb] = (unsigned)w;
        /* the previous batch's bits (the GPU drew it while the lists above were made), then this batch's draws */
        if (pend >= 0)
            { gpu_collect(bits); take(pend, pnb); }
        gpu_submit(nb, (const float (*)[3])eye, FB, off, idx);
        pend = (int)b0, pnb = nb;
    }
    if (pend >= 0)
        { gpu_collect(bits); take(pend, pnb); }
#undef take
    free(cc), free(cs), free(off), free(eye), free(bits), free(idx);
}
#endif

int main(int argc, char **argv)
{
#ifdef PVS_GPU
    if (argc == 2 && !strcmp(argv[1], "--gpu-probe"))  /* tools/pvs_gpu_server.py: is there a GPU to bake on? */
        return gpu_probe();
#endif
    if (argc < 5) {
        fprintf(stderr, "usage: pvs_bake pvsin.bin dg.bin collision.bin pvs.bin [--res N] [--cell M] [--per-cell N] "
                        "[--cams N] [--cluster M] [--validate N] [--spot e,t,p ...] [--threads N]\n");
        return 2;
    }
    int per_cell = 16, cams = 3, validate = 0, vres = 0, nspot = 0, nwith = 0, gpu_res = 0, gpu_pc = 0;
    const char *with[MAXB][3];
    const char *spot[64];
    for (int a = 5; a < argc; a++) {
        if (!strcmp(argv[a], "--res") && a + 1 < argc) R = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--cell") && a + 1 < argc) cellsz = (float)atof(argv[++a]);
        else if (!strcmp(argv[a], "--per-cell") && a + 1 < argc) per_cell = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--cams") && a + 1 < argc) cams = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--validate") && a + 1 < argc) validate = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--vres") && a + 1 < argc) vres = atoi(argv[++a]);  /* validate at this resolution (default --res) */
        else if (!strcmp(argv[a], "--gpu-res") && a + 1 < argc) gpu_res = atoi(argv[++a]);  /* on the GPU: this resolution, */
        else if (!strcmp(argv[a], "--gpu-per-cell") && a + 1 < argc) gpu_pc = atoi(argv[++a]);  /* this many cameras per cell */
        else if (!strcmp(argv[a], "--cluster") && a + 1 < argc) cluster = (float)atof(argv[++a]);
        else if (!strcmp(argv[a], "--spot") && a + 1 < argc && nspot < 64) spot[nspot++] = argv[++a];
        else if (!strcmp(argv[a], "--with") && a + 3 < argc && nwith < MAXB - 1)
            with[nwith][0] = argv[a + 1], with[nwith][1] = argv[a + 2], with[nwith][2] = argv[a + 3], nwith++, a += 3;
#ifdef _OPENMP
        else if (!strcmp(argv[a], "--threads") && a + 1 < argc) omp_set_num_threads(atoi(argv[++a]));
#endif
        else { fprintf(stderr, "pvs_bake: unknown option %s\n", argv[a]); return 2; }
    }
    size_t n;
    unsigned char *in = slurp(argv[1], &n);
    unsigned *h = (unsigned *)in;
    FILE *out = fopen(argv[4], "wb");
    if (!out) { perror(argv[4]); return 1; }
    if (memcmp(in, "PVI1", 4) || !h[1]) {  /* no input: a pvs.bin with no cells (everything drawn) */
        unsigned hd[7] = {0};  /* PHDR with no cells */
        ChunkW w;
        chunkw_begin(&w, "PVS ");
        chunkw_start(&w, "PHDR", 1), chunkw_put(&w, hd, sizeof hd), chunkw_end(&w);
        chunkw_finish(&w);
        fclose(out);
        if (!chunkw_save(&w, argv[4])) { perror(argv[4]); return 1; }
        printf("pvs_bake: %s has no chunks: empty pvs.bin\n", argv[1]);
        return 0;
    }
    unsigned nent = h[6];
    memcpy(&cell_grid, &h[7], 4);
    if (!load_in(in, n, 0)) { fprintf(stderr, "pvs_bake: %s is short\n", argv[1]); return 1; }
    if (nun >= 0xFFFF) { fprintf(stderr, "pvs_bake: %u units (u16)\n", nun); return 1; }
    nself = nch;
    /* visibility units: an MSB part within one --cluster m cube (pieces bigger than that come in clusters); the
     * input's units (a part within one 32 m map cell) keep only the cell box the near / coarse switch is judged on */
    {
        unsigned cap = 1024, nv = 0;
        int (*vk)[4] = malloc(cap * sizeof *vk);
        for (unsigned i = 0; i < nch; i++) {
            if (ch[i].unit == 0xFFFFFFFFu)
                continue;
            const InUnit *u = &un[ch[i].unit];
            int key[4] = {(int)u->part, 0, 0, 0};
            for (int k = 0; k < 3; k++)
                key[k + 1] = cluster > 0 ? (int)floorf(u->key[k] * cell_grid / cluster) : 0;
            unsigned v;
            for (v = 0; v < nv && memcmp(vk[v], key, sizeof key); v++)  /* ~1000 units: linear is fine */
                ;
            if (v == nv) {
                if (nv == cap)
                    cap *= 2, vk = realloc(vk, cap * sizeof *vk);
                memcpy(vk[nv++], key, sizeof key);
            }
            ch[i].unit = v;
        }
        nun = nv;
        free(vk);
    }
    unsigned t0 = load_col(argv[3], 0);
    load_dg(argv[2], 0, 0, nself, t0, nct - t0);
    for (int w = 0; w < nwith; w++) {
        size_t wn;
        unsigned char *wb = slurp(with[w][0], &wn);
        unsigned c0 = nch;
        if (memcmp(wb, "PVI1", 4) || !((unsigned *)wb)[1] || !load_in(wb, wn, nblk)) {
            fprintf(stderr, "pvs_bake: %s: no chunks, left out\n", with[w][0]);
            continue;
        }
        unsigned w0 = load_col(with[w][2], nblk);
        load_dg(with[w][1], nblk, c0, nch - c0, w0, nct - w0);
        printf("pvs_bake: neighbour %s: %u chunks, %u collision tris\n", with[w][0], nch - c0, nct - w0);
        nblk++;
    }
    build_grid();
    build_blists();
    TW = (R + TILE - 1) / TILE;
    const char *sd = getenv("PVS_SIMD");
    if (sd && *sd == '0')
        simd = 0;
    const char *pt = getenv("PVS_PRETEST");
    pretest = !(pt && !strcmp(pt, "0"));
    tb = malloc((nch + 1) * sizeof *tb);
    for (unsigned i = 0; i < nch; i++) {
        for (int k = 0; k < 3; k++)
            tb[i][0][k] = 1e30f, tb[i][1][k] = -1e30f;
        for (unsigned t = ch[i].first; t < ch[i].first + ch[i].ntri; t++)
            for (int j = 0; j < 9; j++)
                tb[i][0][j % 3] = fminf(tb[i][0][j % 3], tri[t].v[j]), tb[i][1][j % 3] = fmaxf(tb[i][1][j % 3], tri[t].v[j]);
    }
    for (int k = 0; k < 3; k++)
        sbox[0][k] = 1e9f, sbox[1][k] = -1e9f;
    for (unsigned i = 0; i < nself; i++)
        if (ch[i].unit != 0xFFFFFFFFu && !(ch[i].flags >> 8 & CHUNK_BACKDROP))
            for (int k = 0; k < 3; k++)
                sbox[0][k] = fminf(sbox[0][k], ch[i].mn[k]), sbox[1][k] = fmaxf(sbox[1][k], ch[i].mx[k]);
    always = calloc(nun, 1);
    for (unsigned i = 0; i < nself; i++)
        if (ch[i].unit != 0xFFFFFFFFu && (ch[i].flags >> 8 & CHUNK_BACKDROP))
            always[ch[i].unit] = 1;

#ifndef PVS_GPU
    (void)gpu_res, (void)gpu_pc;  /* (a build without the GPU path takes and ignores them) */
#else
    int on_gpu = gpu_setup(gpu_res > 0 ? gpu_res : R);
    if (on_gpu) {  /* the GPU's own resolution and cameras per cell (before the cells pick their cameras) */
        if (gpu_res > 0)
            R = gpu_res;
        if (gpu_pc > 0)
            per_cell = gpu_pc;
    }
#endif
    players();
    hcap2 = 1 << 20;
    chash = malloc(hcap2 * sizeof *chash);
    memset(chash, 0xFF, hcap2 * sizeof *chash);
    uint64_t seed = 0x1234567ull;
    unsigned ncam = 0;
    for (unsigned i = 0; i < npp; i++)
        for (int k = 0; k < cams; k++) {
            Cam c = camera(i, &seed);
            uint32_t key = cell_key(c.e);
            if (key == 0xFFFFFFFFu)
                continue;
            int cj = cell_find(key, 1);  /* may move cells */
            Cell *ce = &cells[cj];
            ncam++;
            if (ce->n < (unsigned)per_cell) {  /* reservoir: an even pick over every camera of the cell */
                if (ce->n == ce->cap)
                    ce->cap = ce->cap ? ce->cap * 2 : 4, ce->cam = realloc(ce->cam, ce->cap * sizeof *ce->cam);
                ce->cam[ce->n++] = c;
            } else {
                unsigned r = rnd(&seed) % (ce->n + 1);
                if (r < (unsigned)per_cell)
                    ce->cam[r] = c;
                ce->n++;
            }
        }
    unsigned rb = (nun + 7) / 8, nsampled = ncell;
    printf("pvs_bake: %u chunks, %u units, %u tris; %u player spots, %u cameras in %u cells of %.0f m\n", nself, nun,
           ntri, npp, ncam, ncell, cellsz);
    fflush(stdout);
    for (unsigned i = 0; i < ncell; i++)
        cells[i].row = calloc(rb, 1), cells[i].sampled = 1;
    unsigned long long tested = 0;
#ifdef PVS_GPU
    if (on_gpu) {
        gpu_bake(per_cell, &tested);
        goto sampled;
    }
#endif
    const char *rvc = getenv("PVS_ROUNDS");
    const int rounds_cpu = !(rvc && *rvc == '0');  /* (sample(): units the cell has seen are not tested again) */
#pragma omp parallel
    {
        Ctx c = {malloc(sizeof(float) * (R * R + 8)), calloc(nun, sizeof(unsigned)), NULL, 0, malloc(sizeof(float) * TW * TW), 0};
        unsigned *list = malloc(2 * nch * sizeof *list);
        unsigned char *fm = malloc(nch + 1);
#pragma omp for schedule(dynamic, 1) reduction(+ : tested)
        for (unsigned i = 0; i < nsampled; i++) {
            Cell *ce = &cells[i];
            unsigned m = ce->n < (unsigned)per_cell ? ce->n : (unsigned)per_cell;
            for (unsigned s = 0; s < m; s++) {
                const Cam *cm = &ce->cam[s];
                unsigned nl = draw_list(cm->e, cm->b, cm->cm, list);
                c.stamp++;
                sample(&c, cm->e, list, nl, fm, rounds_cpu ? ce->row : NULL);
                for (unsigned u = 0; u < nun; u++)
                    if (c.seen[u] == c.stamp)
                        ce->row[u >> 3] |= 1 << (u & 7);
                tested++;
            }
        }
        free(c.zb), free(c.seen), free(list), free(c.tmin), free(fm);
    }
#ifdef PVS_GPU
sampled:
#endif
    /* dilation: each row ORs its 6 neighbours' (missed samples). Only sampled cells are stored: the runtime takes the
     * union of a missing cell's stored neighbours (pvs.c), "everything" when it has none */
    unsigned char **raw = malloc(nsampled * sizeof *raw);
    for (unsigned i = 0; i < nsampled; i++)
        raw[i] = malloc(rb), memcpy(raw[i], cells[i].row, rb);
    for (unsigned i = 0; i < nsampled; i++) {
        uint32_t k = cells[i].key;
        int x = k & 1023, y = k >> 10 & 1023, z = k >> 20 & 1023;
        for (int d = 0; d < 6; d++) {
            int nx = x + NB[d][0], ny = y + NB[d][1], nz = z + NB[d][2];
            if (nx < 0 || ny < 0 || nz < 0 || nx > 1023 || ny > 1023 || nz > 1023)
                continue;
            int j = cell_find((uint32_t)nx | (uint32_t)ny << 10 | (uint32_t)nz << 20, 0);
            if (j >= 0)
                for (unsigned b = 0; b < rb; b++)
                    cells[i].row[b] |= raw[j][b];
        }
    }
    for (unsigned i = 0; i < ncell; i++)
        for (unsigned u = 0; u < nun; u++)
            if (always[u])
                cells[i].row[u >> 3] |= 1 << (u & 7);
    /* units in every row or in none carry no bit (lossless): the rows hold the others */
    unsigned *vbit = malloc(nun * sizeof *vbit), nv = 0, nalw = 0, nnev = 0;
    for (unsigned u = 0; u < nun; u++) {
        unsigned c = 0;
        for (unsigned i = 0; i < ncell; i++)
            c += cells[i].row[u >> 3] >> (u & 7) & 1;
        vbit[u] = c == ncell ? (nalw++, 0xFFFFu) : !c ? (nnev++, 0xFFFEu) : nv++;
    }
    unsigned vb = (nv + 7) / 8;
    unsigned char *crow = calloc((size_t)ncell * (vb ? vb : 1), 1);
    for (unsigned i = 0; i < ncell; i++)
        for (unsigned u = 0; u < nun; u++)
            if (vbit[u] < 0xFFFE && (cells[i].row[u >> 3] >> (u & 7) & 1))
                crow[(size_t)i * vb + (vbit[u] >> 3)] |= 1 << (vbit[u] & 7);
    /* unique rows */
    unsigned *ord = malloc(ncell * sizeof *ord), *rowid = malloc(ncell * sizeof *rowid), nrow = 0;
    unsigned char **rows = malloc(ncell * sizeof *rows);
    for (unsigned i = 0; i < ncell; i++)
        ord[i] = i;
    qsort(ord, ncell, sizeof *ord, cmp_u32);
    {
        unsigned hc = 1;
        while (hc < 2 * ncell)
            hc <<= 1;
        int *hh = malloc(hc * sizeof *hh);
        memset(hh, 0xFF, hc * sizeof *hh);
        for (unsigned i = 0; i < ncell; i++) {
            unsigned char *r = crow + (size_t)i * vb;
            uint64_t hv = 1469598103934665603ull;
            for (unsigned b = 0; b < vb; b++)
                hv = (hv ^ r[b]) * 1099511628211ull;
            unsigned s = (unsigned)(hv >> 20) & (hc - 1);
            while (hh[s] >= 0 && memcmp(rows[hh[s]], r, vb))
                s = (s + 1) & (hc - 1);
            if (hh[s] < 0)
                hh[s] = (int)nrow, rows[nrow++] = r;
            rowid[i] = (unsigned)hh[s];
        }
        free(hh);
    }
    if (nrow >= 0xFFFF) { fprintf(stderr, "pvs_bake: %u rows (u16)\n", nrow); return 1; }
    double vis = 0;
    for (unsigned i = 0; i < ncell; i++) {
        unsigned c = 0;
        for (unsigned u = 0; u < nun; u++)
            c += cells[i].row[u >> 3] >> (u & 7) & 1;
        vis += c;
    }
    /* per cell table entry the list of its units, a byte per chunk into it */
    unsigned *eoff = calloc(nent + 1, sizeof *eoff), *ul = malloc((nself + 1) * sizeof *ul), nul = 0;
    unsigned char *loc = malloc(nself + 4);
    {
        unsigned *first = malloc(nent * sizeof *first);  /* chunks by entry */
        unsigned *next = malloc(nself * sizeof *next);
        for (unsigned e = 0; e < nent; e++)
            first[e] = ~0u;
        for (unsigned i = nself; i-- > 0;)
            if (ch[i].entry < nent)
                next[i] = first[ch[i].entry], first[ch[i].entry] = i;
        for (unsigned e = 0; e < nent; e++) {
            eoff[e] = nul;
            for (unsigned i = first[e]; i != ~0u; i = next[i]) {
                if (ch[i].unit == 0xFFFFFFFFu)
                    continue;
                unsigned j;
                for (j = eoff[e]; j < nul && ul[j] != ch[i].unit; j++)
                    ;
                if (j == nul)
                    ul[nul++] = ch[i].unit;
                if (j - eoff[e] >= 0xFF) { fprintf(stderr, "pvs_bake: cell entry %u has 255+ units\n", e); return 1; }
                loc[i] = (unsigned char)(j - eoff[e]);
            }
        }
        eoff[nent] = nul;
        for (unsigned i = 0; i < nself; i++)
            if (ch[i].unit == 0xFFFFFFFFu || ch[i].entry >= nent)
                loc[i] = 0xFF;
        free(first), free(next);
    }
    if (nul >= 0xFFFF) { fprintf(stderr, "pvs_bake: %u unit list entries (u16)\n", nul); return 1; }
    unsigned hd[7] = {nv, nself, nent, ncell, nrow, vb, 0};
    memcpy(&hd[6], &cellsz, 4);
    ChunkW w;
    chunkw_begin(&w, "PVS ");
    chunkw_start(&w, "PHDR", 1), chunkw_put(&w, hd, sizeof hd), chunkw_end(&w);
    chunkw_start(&w, "EOFF", 1);
    for (unsigned e = 0; e <= nent; e++) {
        uint16_t v = (uint16_t)eoff[e];
        chunkw_put(&w, &v, 2);
    }
    chunkw_end(&w);
    chunkw_start(&w, "ULST", 1);
    for (unsigned j = 0; j < nul; j++) {
        uint16_t v = (uint16_t)vbit[ul[j]];  /* row bit, 0xFFFF always, 0xFFFE never */
        chunkw_put(&w, &v, 2);
    }
    chunkw_end(&w);
    chunkw_start(&w, "LOC ", 1), chunkw_put(&w, loc, nself), chunkw_end(&w);
    chunkw_start(&w, "KEYS", 1);
    for (unsigned i = 0; i < ncell; i++)
        chunkw_put(&w, &cells[ord[i]].key, 4);
    chunkw_end(&w);
    chunkw_start(&w, "ROWO", 1);
    for (unsigned i = 0; i < ncell; i++) {
        uint16_t r = (uint16_t)rowid[ord[i]];
        chunkw_put(&w, &r, 2);
    }
    chunkw_end(&w);
    chunkw_start(&w, "ROWS", 1);
    for (unsigned i = 0; i < nrow; i++)
        chunkw_put(&w, rows[i], vb);
    chunkw_end(&w);
    chunkw_finish(&w);
    long sz = (long)w.len;
    fclose(out);
    if (!chunkw_save(&w, argv[4])) { perror(argv[4]); return 1; }
    free(w.buf);
    printf("pvs_bake: %llu cube maps at %d px, %u cells, %u rows of %u bits (%u units always seen, %u never), mean "
           "%.0f of %u units visible from a cell; pvs.bin %ld bytes\n", tested, R, ncell, nrow, nv, nalw, nnev,
           vis / (ncell ? ncell : 1), nun, sz);

    /* ---- validation: fresh cameras, the full scene, pixels whose unit the row leaves out ---- */
    if (validate) {
        if (vres > 0)
            R = vres;
        uint64_t s2 = 0xC0FFEE1234ull;
        unsigned long long px = 0, miss = 0, norow = 0, bad = 0;
        double worst = 0;
        float we[3] = {0};
#pragma omp parallel
        {
            Ctx c = {malloc(sizeof(float) * (R * R + 8)), NULL, malloc(sizeof(unsigned) * R * R), 0, NULL, 0};
            unsigned *list = malloc(2 * nch * sizeof *list);
            unsigned char *tmp = malloc(rb);
#pragma omp for schedule(dynamic, 4) reduction(+ : px, miss, norow, bad)
            for (int v = 0; v < validate; v++) {
                uint64_t s = s2 + (uint64_t)v * 0x9E3779B97F4A7C15ull;
                rnd(&s);
                unsigned pi = rnd(&s) % npp;
                Cam cm = camera(pi, &s);
                const unsigned char *row = row_at(cell_key(cm.e), tmp, rb);
                if (!row) {
                    norow++;
                    continue;
                }
                unsigned nl = draw_list(cm.e, cm.b, cm.cm, list);
                unsigned long long vp = 0, vm = 0;
                for (int f = 0; f < 6; f++) {
                    memset(c.zb, 0, sizeof(float) * R * R);
                    memset(c.ib, 0xFF, sizeof(unsigned) * R * R);
                    for (unsigned j = 0; j < nl; j++) {
                        unsigned i = list[j] & 0x3FFFFFFFu;
                        if (box_in_face(ch[i].mn, ch[i].mx, cm.e, FB[f]))
                            draw_chunk(&c, i, cm.e, FB[f], 2);
                    }
                    for (int q = 0; q < R * R; q++)
                        if (c.ib[q] < nun) {  /* block 0's pixels (a neighbour's: FOREIGN) */
                            vp++;
                            if (!(row[c.ib[q] >> 3] >> (c.ib[q] & 7) & 1))
                                vm++;
                        }
                }
                px += vp, miss += vm;
                if (vm) {
                    bad++;
#pragma omp critical
                    if ((double)vm / vp > worst)
                        worst = (double)vm / vp, memcpy(we, cm.e, sizeof we);
                }
            }
            free(c.zb), free(c.ib), free(list), free(tmp);
        }
        printf("pvs_bake: validate %d cameras (%llu outside every cell: drawn in full), %llu with missing pixels; "
               "missing %llu of %llu pixels = %.5f %%, worst %.3f %% at %.1f %.1f %.1f\n", validate, norow, bad, miss,
               px, 100.0 * miss / (px ? px : 1), 100 * worst, we[0], we[1], we[2]);
    }

    /* ---- spots: triangles in a 4:3 camera frustum with and without the PVS ---- */
    for (int si = 0; si < nspot; si++) {
        float v[9];
        if (sscanf(spot[si], "%f,%f,%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                   &v[8]) != 9) {
            fprintf(stderr, "pvs_bake: --spot ex,ey,ez,tx,ty,tz,px,py,pz\n");
            continue;
        }
        int ft = col_floor(v[6], v[7] + 0.05f, v[8]);
        int cm = ft >= 0 && ct[ft].dg != 0xFF ? ct[ft].dg : -1, pb = ft >= 0 ? ct[ft].blk : 0;
        float fw[3] = {v[3] - v[0], v[4] - v[1], v[5] - v[2]}, l = sqrtf(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
        for (int k = 0; k < 3; k++)
            fw[k] /= l;
        float rt[3] = {-fw[2], 0, fw[0]}, rl = sqrtf(rt[0] * rt[0] + rt[2] * rt[2]);
        rt[0] /= rl, rt[2] /= rl;
        float up[3] = {rt[1] * fw[2] - rt[2] * fw[1], rt[2] * fw[0] - rt[0] * fw[2], rt[0] * fw[1] - rt[1] * fw[0]};
        const float tx = 0.75f, ty = 0.5625f;  /* tan of the half angles, ASSUMED ~74 x 59 degrees */
        unsigned char *tmp = malloc(rb);
        const unsigned char *srow = row_at(cell_key(v), tmp, rb);
        unsigned long long t_all = 0, t_pvs = 0;
        unsigned c_all = 0, c_pvs = 0;
        for (unsigned j = 0; j < nself; j++) {
            if (!drawn(j, v, pb, cm))
                continue;
            int out[5] = {1, 1, 1, 1, 1};
            for (int c = 0; c < 8; c++) {
                float d[3] = {(c & 1 ? ch[j].mx[0] : ch[j].mn[0]) - v[0], (c & 2 ? ch[j].mx[1] : ch[j].mn[1]) - v[1],
                              (c & 4 ? ch[j].mx[2] : ch[j].mn[2]) - v[2]};
                float x = d[0] * rt[0] + d[1] * rt[1] + d[2] * rt[2], y = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
                float z = d[0] * fw[0] + d[1] * fw[1] + d[2] * fw[2];
                out[0] &= z < 0.25f, out[1] &= x > z * tx, out[2] &= -x > z * tx, out[3] &= y > z * ty, out[4] &= -y > z * ty;
            }
            if (out[0] | out[1] | out[2] | out[3] | out[4])
                continue;
            t_all += ch[j].ntri, c_all++;
            if (!srow || (srow[ch[j].unit >> 3] >> (ch[j].unit & 7) & 1))
                t_pvs += ch[j].ntri, c_pvs++;
        }
        printf("pvs_bake: spot %s: %s, player on block %d, draw groups %d: %u chunks %llu tris -> PVS %u chunks %llu "
               "tris (%.0f %%)\n", spot[si], !srow ? "no cell (drawn in full)" : "cell", pb, cm, c_all, t_all, c_pvs, t_pvs,
               100.0 * t_pvs / (t_all ? t_all : 1));
    }
    return 0;
}
