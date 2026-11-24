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


extern unsigned mp_t[MP_N];
static inline unsigned mp_sum(void) { return mp_t[0] + mp_t[1] + mp_t[2] + mp_t[3]; }
#define MP_BEGIN unsigned _mp0 = mp_on ? ticks() : 0;
#define MP_END(p) if (mp_on) mp_t[p] += ticks() - _mp0;
#define MP_SPLIT(p) do { if (mp_on) { unsigned _mp1 = ticks(); mp_t[p] += _mp1 - _mp0; _mp0 = _mp1; } } while (0)
#define MF_BEGIN unsigned _mf0 = mp_on ? ticks() : 0, _mfn = mp_on ? mp_sum() : 0;
#define MF_END(p) if (mp_on) mf_t[p] += ticks() - _mf0 - (mp_sum() - _mfn);
#define MF_AGAIN if (mp_on) _mf0 = ticks(), _mfn = mp_sum();

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
