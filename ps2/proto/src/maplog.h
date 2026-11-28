/* map.c's frame timers, counters and log lines (src/maplog.c): the "map: pass ms" / "map: plan ms" breakdown, the
 * MAP_DBG / MAP_PROF debug counters, the pop-in metric and the per-frame log (FrameLog). Map files only. */
#ifndef MAPLOG_H
#define MAPLOG_H
#include "map_i.h"

/* Chunks that touch the near plane or guard band also go to VU1, which skips the triangles that need clipping;
 * those (and only those) are clipped on the EE and drawn through gsKit */
#ifdef MAP_PROF  /* profiling build: map_vu1_batches / map_ee_tris show visibility / EE-clip time in 0.01 ms */
extern unsigned mp_prof_vis, mp_prof_clip;
#define PROF_BEGIN unsigned _t0 = ticks();
#define PROF_END(acc) acc += ticks() - _t0;
#else
#define PROF_BEGIN
#define PROF_END(acc)
#endif

/* map_ms breakdown (budget doc proposals 1 and 6): COP0 Count ticks
 * per part of map_draw this frame. cull = frustum / distance tests, pvs_eye, the index; tex = texcache (tc_get,
 * tc_mip, tc_begin / tc_end: uploads go into the chain there); chain = VIF / GIF packets (vu1_add_*, registers);
 * bump = relief; plan = the rest (per-cell level of detail, near parts, draw groups, visibility rows, effects sort).
 * Timed in one frame of MAP_PASS_EVERY only: each reading of the Count register costs ~0.15 us in PCSX2 and the
 * ~5000 a timed frame takes add ~0.8 ms to it. Logged every MAP_PASS_LOG frames as "map: pass ms" (average of the
 * timed frames, and the timed frame with the largest total). */
#define MAP_PASS_EVERY 8
#define MAP_PASS_LOG 120
extern int mp_on;
enum { MP_CULL, MP_TEX, MP_CHAIN, MP_BUMP, MP_N };
/* plan split (the same log, a "map: plan ms" line): per visible cell the resident-miss scan, the near level, the
 * chunk loop's own work (draw groups, visibility rows, streams), the rest of the cell loop, the effects; exclusive of
 * the parts above */
enum { MF_MISS, MF_NEAR, MF_CHUNKS, MF_FX, MF_N };
extern unsigned mf_t[MF_N];
extern unsigned mp_mc_cells, mp_mc_vis, mp_mc_tiles, mp_mc_tvis, mp_mc_chunks;


/* "map: other ms" (the same log): what plan other holds, by part, in ticks per timed frame. The parts are the map
 * functions' own work outside the MP_ / MF_ regions: the sky call, the backdrop pictures, the far panorama, the
 * horizon of a kept-only block, the tile / cell prologue (tests, horizon box, item lookups), the horizon chunk loop of
 * a cell, the fades, mp_zs_end (the replayed near pass); rest = plan other less their sum. A part subtracts mp_sum()
 * only (no MO_ region holds an MF_ region). Every timer read costs time inside the interval it ends: mo_reads[p] counts
 * the reads whose cost lands in part p (its own 2, plus 1 per MP_ pair nested in it: the pair's other read is inside
 * the interval mp_sum() removes), mp_cull_reads those of the MP_CULL pairs; mp_tick_n counts every read of the frame,
 * mp_tick_us is the measured cost of one (mp_tick_calibrate, map_init) and the "_c" values subtract it. */
enum { MO_SKY, MO_PICT, MO_FAR, MO_HZONLY, MO_PRO, MO_HZCELL, MO_FADES, MO_ZSEND, MO_LVL, MO_SETUP, MO_N };
extern unsigned mo_t[MO_N], mo_reads[MO_N], mp_cull_reads, mp_tick_n, mp_np;
extern unsigned mp_wall;  /* a timed frame's wall clock outside MP_ / MF_ in the three map_draw_* functions (the check line) */
extern float mp_tick_us;  /* one ticks() read, us (mp_tick_calibrate: 1000 back-to-back reads at map init) */
void mp_tick_calibrate(void);
/* operation counts of a frame ("map: work" line), counted on every frame: box depth calls, tc_level calls, sqrtf calls
 * of map_draw's files (almost all draw_chunk_'s), far panorama chunks tested, horizon chunks tested */
enum { MW_BOX, MW_TCL, MW_SQRT, MW_FAR, MW_HZ, MW_N };
extern unsigned mw_n[MW_N];
#define MW_ADD(i) (mw_n[i]++)
extern unsigned mp_t[MP_N];
static inline unsigned mp_sum(void) { return mp_t[0] + mp_t[1] + mp_t[2] + mp_t[3]; }
#define MP_BEGIN unsigned _mp0 = mp_on ? (mp_tick_n++, ticks()) : 0;
#define MP_END(p) if (mp_on) { mp_t[p] += ticks() - _mp0; mp_tick_n++, mp_np++; if ((p) == MP_CULL) mp_cull_reads += 2; }
#define MP_SPLIT(p) do { if (mp_on) { unsigned _mp1 = ticks(); mp_t[p] += _mp1 - _mp0; _mp0 = _mp1; mp_tick_n++; } } while (0)
#define MF_BEGIN unsigned _mf0 = mp_on ? (mp_tick_n++, ticks()) : 0, _mfn = mp_on ? mp_sum() : 0;
#define MF_END(p) if (mp_on) { mf_t[p] += ticks() - _mf0 - (mp_sum() - _mfn); mp_tick_n++; }
#define MF_AGAIN if (mp_on) _mf0 = ticks(), _mfn = mp_sum(), mp_tick_n++;
#define MO_BEGIN unsigned _mo0 = mp_on ? (mp_tick_n++, ticks()) : 0, _mos = mp_on ? mp_sum() : 0, _mon = mp_on ? mp_np : 0;
#define MO_END(p) if (mp_on) { unsigned _mo1 = ticks(); mo_t[p] += _mo1 - _mo0 - (mp_sum() - _mos); \
                               mo_reads[p] += 2 + mp_np - _mon; mp_tick_n++; }
#define MO_AGAIN if (mp_on) _mo0 = ticks(), _mos = mp_sum(), _mon = mp_np, mp_tick_n++;

/* the check: a map_draw_* function's wall clock outside the MP_ / MF_ regions (mp_wall) */
#define MW_SUM() (mp_sum() + mf_t[0] + mf_t[1] + mf_t[2] + mf_t[3])
#define WALL_BEGIN unsigned _mw0 = mp_on ? (mp_tick_n++, ticks()) : 0, _mws = mp_on ? MW_SUM() : 0;
#define WALL_END if (mp_on) { mp_wall += ticks() - _mw0 - (MW_SUM() - _mws); mp_tick_n++; }

/* counted calls (the files that include mapchunk.h before this: map.c, mappass.c, mapchunk.c; not mapvis.c, whose
 * definition of mp_box_depth the macro would rewrite). The declarations come first. */
#include "mapvis.h"
#ifdef MAPCHUNK_H
#define mp_box_depth(c, e, zmax, r2) (MW_ADD(MW_BOX), (mp_box_depth)(c, e, zmax, r2))
#define tc_level(tex, d) (MW_ADD(MW_TCL), (tc_level)(tex, d))
#define sqrtf(x) (MW_ADD(MW_SQRT), (sqrtf)(x))
#endif

#ifdef MAP_DBG  /* debug builds: chunks / tris dropped per step, printed every MAP_DBG frames (MAP_DBG_ALL: no
                 * draw-group, distance, subpixel or foliage culling, the reference render to compare with) */
enum { D_DRAWN, D_DG, D_PVS, D_CELL_FAR, D_CELL_SMALL, D_CELL_VIEW, D_FAR, D_SMALL, D_VIEW, D_GRASS, D_TEX, D_SWAP, D_NR };
extern const char *const mp_dbg_name[D_NR];
extern unsigned mp_dbg_c[D_NR], mp_dbg_t[D_NR];
extern int mp_vis_why;
/* per chunk (map.bin index, near chunks by their bump.bin number = same numbering): this / last frame's step,
 * 255 = not reached (a near chunk of a cell showing its coarse level); MAP_DBG_FLIP logs the chunks drawn last frame
 * and not now, by step */
#define DBG_IDS 32768
extern unsigned char mp_dbg_why[2][DBG_IDS];
extern int mp_dbg_cur;
#define DBG(r, n) (mp_dbg_c[r]++, mp_dbg_t[r] += (n))
#define DBGW(r, n, id) (DBG(r, n), (id) < DBG_IDS ? mp_dbg_why[mp_dbg_cur][id] = (r) : 0)
#else
#define DBG(r, n) ((void)0)
#define DBGW(r, n, id) ((void)0)
#endif
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

extern unsigned short mp_pop_fr;     /* map draws so far (pop-in metric, hitch.c level flips) */
extern unsigned short mp_pop_settle;  /* mp_pop_fr of the last draw during a settle */
/* "map: pass ms" (MAP_PASS_MS): mp_t0 = map_draw's start tick */
void mp_log_pass_ms(unsigned mp_t0);
/* map_draw's log lines and debug-build reports for one frame */
void mp_log_frame(const FrameLog *lg, const float e[3], int drawn);
/* MAP_POP_LOG: one line per pop-in */
void mp_pop_note(int k, int ci, const Cell *ce, float d, unsigned w);
/* the cells the horizon stood in for inside HZN_R this frame (instance, cell, distance: the log names up to three) */
extern unsigned mp_hzx[3][3];
#ifdef MAP_DBG
void mp_dbg_begin(void);  /* map_draw: the first frame's per-chunk steps */
#endif

#endif
