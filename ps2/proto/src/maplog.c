/* map.c's frame timers, counters and log lines (maplog.h) */
#include "maplog.h"
#include "mapvis.h"
#include "mappass.h"
#include "testscript.h"

int mp_on;  /* this frame is timed */
unsigned mf_t[MF_N];
unsigned mp_mc_cells, mp_mc_vis, mp_mc_tiles, mp_mc_tvis, mp_mc_chunks;  /* per frame: cells tested / visible, tiles tested / visible, chunks reached */
unsigned mp_t[MP_N];
#ifdef MAP_PROF
unsigned mp_prof_vis, mp_prof_clip;
#endif
#ifdef MAP_DBG
const char *const mp_dbg_name[D_NR] = {"drawn", "dg", "pvs", "cfar", "csmall", "cview", "far", "small", "view", "grass",
                                           "tex", "lod"};
unsigned mp_dbg_c[D_NR], mp_dbg_t[D_NR];
unsigned char mp_dbg_why[2][DBG_IDS];
int mp_dbg_cur;

void mp_dbg_begin(void)
{
    static int dbg_init;
    if (!dbg_init++)
        memset(mp_dbg_why, 255, sizeof mp_dbg_why);
}
#endif
unsigned short mp_pop_fr;
static unsigned pop_total;
unsigned mp_hzx[3][3];

/* "map: pass ms": the breakdown over 30 frames (average, and the frame with the largest map_draw) */
void mp_log_pass_ms(unsigned mp_t0)
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
               mp_mc_tiles / MAP_PASS_LOG, mp_mc_tvis / MAP_PASS_LOG, mp_mc_cells / MAP_PASS_LOG, mp_mc_vis / MAP_PASS_LOG,
               mp_mc_chunks / MAP_PASS_LOG);
        mp_mc_cells = mp_mc_vis = mp_mc_tiles = mp_mc_tvis = mp_mc_chunks = 0, nt = 0;
#undef MPMS
#undef MPMS1
        memset(acc, 0, sizeof acc), memset(wst, 0, sizeof wst), wtot = 0, memset(facc, 0, sizeof facc);
    }
}

/* map_draw's log lines and debug-build reports for one frame */
void mp_log_frame(const FrameLog *lg, const float e[3], int drawn)
{
#ifdef MAP_PANO_R  /* panorama capture: the camera basis for tools/pano.py, every 8 frames */
    {
        static unsigned pf;
        if (!(++pf & 7))
            printf("PANOCAM %.3f %.3f %.3f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.2f\n", mp_cam[0], mp_cam[1],
                   mp_cam[2], mp_cam[3], mp_cam[4], mp_cam[5], mp_cam[6], mp_cam[7], mp_cam[8], mp_cam[9], mp_cam[10], mp_cam[11], mp_cam_f, 0.0f, 0.0f, 0.0f);
    }
#endif
#ifdef MAP_DBG
    {
        static unsigned fr;
        mp_dbg_t[D_DRAWN] = drawn;
        if (!(++fr % MAP_DBG)) {
            char line[320];
            int k = snprintf(line, sizeof line, "MAPDBG %u", fr);
            for (int r = 0; r < D_NR; r++)
                k += snprintf(line + k, sizeof line - k, " %s %u/%u", mp_dbg_name[r], mp_dbg_c[r], mp_dbg_t[r]);
            puts(line);
        }
        memset(mp_dbg_c, 0, sizeof mp_dbg_c), memset(mp_dbg_t, 0, sizeof mp_dbg_t);
#ifdef MAP_DBG_FLIP
        {  /* chunks drawn last frame, not now: by this frame's step (view = left the frustum: expected when turning) */
            unsigned cnt[D_NR + 1] = {0}, tris[D_NR + 1] = {0}, on = 0;
            const unsigned char *now = mp_dbg_why[mp_dbg_cur], *was = mp_dbg_why[!mp_dbg_cur];
            char line[400];
            int k = snprintf(line, sizeof line, "FLIP %u", fr);
            for (unsigned i = 0; i < DBG_IDS; i++) {
                if (was[i] == D_DRAWN && now[i] != D_DRAWN) {
                    int r = now[i] < D_NR ? now[i] : D_NR;
                    cnt[r]++;
                    if (mp_inst[0].hdr && i < mp_inst[0].hdr->chunks)
                        tris[r] += mp_inst[0].chunks[i].tris;
                    if (r != D_VIEW && r != D_CELL_VIEW && r != D_SWAP && r != D_NR && k < 300)
                        k += snprintf(line + k, sizeof line - k, " [%u %s]", i, mp_dbg_name[r]);
                } else if (was[i] != D_DRAWN && now[i] == D_DRAWN)
                    on++;
            }
            k += snprintf(line + k, sizeof line - k, " | on %u off", on);
            for (int r = 1; r <= D_NR; r++)
                if (cnt[r])
                    k += snprintf(line + k, sizeof line - k, " %s %u/%u", r < D_NR ? mp_dbg_name[r] : "unseen", cnt[r],
                                  tris[r]);
            puts(line);
        }
#endif
        mp_dbg_cur = !mp_dbg_cur;
        memset(mp_dbg_why[mp_dbg_cur], 255, DBG_IDS);
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
                       hzf_max, hzt_max, mp_hzx[0][0], mp_hzx[0][1], mp_hzx[0][2], mp_hzx[1][0], mp_hzx[1][1], mp_hzx[1][2]);
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
                           (double)e[1], (double)e[2], lg->k_drawn[0], mp_pano_drawn());
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
        v += mp_prof_vis, c += mp_prof_clip;
        mp_prof_vis = mp_prof_clip = 0;
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

/* MAP_POP_LOG (default on in test runs; 0 = off): one line per pop-in (a cell that swaps coarse -> near in view
 * after waiting): its in-view wait, the whole wait, its distance, why it waited (mapstream_why per in-view draw:
 * unseen / refused / plan cut / disc / settle), and the draws since the last settle (a warp, a bonfire, a death) */
#ifndef MAP_POP_LOG
#define MAP_POP_LOG 1
#endif
unsigned short mp_pop_settle;  /* mp_pop_fr of the last draw during a settle */
static unsigned pop_lines;
void mp_pop_note(int k, int ci, const Cell *ce, float d, unsigned w)
{
    if (!MAP_POP_LOG || !test_active() || pop_lines >= 200)
        return;
    pop_lines++;
    printf("map: pop cell %d.%d at %.1f m: waited %u draws in view (%u in all), unseen %u refused %u cut %u disc %u "
           "settle %u, %u draws after a settle\n", k, ci, (double)d, ce->win, w, ce->why[0], ce->why[1], ce->why[2],
           ce->why[3], ce->why[4], (unsigned)(unsigned short)(mp_pop_fr - mp_pop_settle));
}

