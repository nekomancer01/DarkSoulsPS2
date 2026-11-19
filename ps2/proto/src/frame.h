/* frame.h: the frame loop in phases (Polaris 1.3). main.c boots, then frame_run loops over the phases below.
 *
 * Phases, in order:
 *   INPUT     pad and test input, UI input (bonfire / Start menu), the camera's yaw and pitch
 *   SIM_A     the player (update, status, hit sweeps, fog-wall push), enemy type streaming, the camera
 *   WORLD     block streaming, the loading screen's close, sound, the player's floor and draw groups
 *             (map_set_player: what enemies and events read about visibility is final here)
 *   EMIT_MAP  the map chain (Z clear, map, kick). It reads only the camera, the world / streamer state and the
 *             visibility result, all final before it starts. VU1 draws it while SIM_B runs on the EE
 *   SIM_B     enemies, objects, events, item pickup, bonfire. Nothing here reads a result EMIT_MAP produced (it runs
 *             the same with EMIT_MAP skipped: FRAME_SKIP_MAP, a runtime switch for that check). What SIM_B changes that
 *             the map pass reads (event-switched map parts, breakables, the player light on map colours) shows one
 *             frame late, as before the split
 *   EMIT      characters, effects (fx_update is emit preparation: particle data built while VU1 draws), the bloodstain,
 *             debug world drawing, HUD, menus, the loading overlay
 *   PRESENT   the effects chain kick, the VU1 wait, the frame-region swap, gsKit's queue, the flip; then test hooks,
 *             saves and the periodic logs
 *
 * The rule: simulate phases (INPUT, SIM_A, WORLD, SIM_B) write no draw packets, and emit phases change no game state.
 * Presentation-only animation (bar smoothing, fades, the loading screen's timers) may step in emit. Checks, counted per
 * entry point in a "frame:" line each hitch period: a draw entry (vu1_begin and each system's draw call) outside an
 * emit phase, a simulate entry (each system's update call) inside one, and UI requests made from simulate (ui_req.h,
 * report only: the screen stack in Polaris 7.1 owns them). FRAME_STRICT builds also check every vu1_add* call and
 * print a TEST error with the caller. */
#ifndef FRAME_H
#define FRAME_H

#include <gsKit.h>
#include "player.h"

typedef enum { FP_BOOT, FP_INPUT, FP_SIM_A, FP_WORLD, FP_EMIT_MAP, FP_SIM_B, FP_EMIT, FP_PRESENT, FP_N } FramePhase;
extern FramePhase frame_phase;

/* Entry points the checks count (frame.c names them in the "frame:" line) */
typedef enum {
    FE_VU1_BEGIN, FE_VU1_ADD, FE_MAP_DRAW, FE_CHR_DRAW, FE_FX_DRAW, FE_HUD_DRAW, FE_MENU_DRAW, FE_N_DRAW,
    FE_PLAYER_UPDATE = FE_N_DRAW, FE_ENEMY_UPDATE, FE_OBJ_UPDATE, FE_EVENT_UPDATE, FE_WORLD_UPDATE, FE_N
} FrameEntry;
/* a draw entry: counted when the phase is not an emit phase */
void frame_draw_check(FrameEntry e);
/* a simulate entry: counted when the phase is an emit phase */
void frame_sim_check(FrameEntry e);
static inline int frame_emitting(void) { return frame_phase == FP_EMIT_MAP || frame_phase == FP_EMIT; }
/* ui_req.h: one UI request, counted when made from a simulate phase */
void frame_ui_request(void);
#ifdef FRAME_STRICT
#define FRAME_DRAW_STRICT(e) frame_draw_check(e)
#else
#define FRAME_DRAW_STRICT(e) ((void)0)
#endif
/* one "frame:" line (hitch.c, each period) */
void frame_log(void);

/* Determinism for tests (Polaris 1.7a), set by test-script options:
 *   option fixeddt MS   every frame's dt is exactly MS milliseconds (frame pacing stays real)
 *   option seed N       rand() seeded with N (the loading screen's hint pick too)
 *   option record NAME  each frame's input after the pad and test layers (+ dt, frame number, camera) to
 *                       host:save/NAME (build/save/), its simulate state (position, yaw, state) to NAME.state
 * With fixeddt, frame_clock() runs on frames (frames x dt, and it stops while a test shot is frozen): the map's
 * player-light blend steps by frames, and effects and HUD smoothing hold still on frozen frames. The work a real-time
 * budget cuts (lua0's collector step, event.c's instance runs) reads frame_budget_clock() instead, which with fixeddt
 * counts work: each read inside a frame adds FRAME_DET_READ_US, so the budgets still cut in (the deferred-event and
 * partial-sweep paths of real play are taken) and cut at the same point every run. What depends on the
 * disc (a block or an enemy model arriving) stays as non-deterministic as the drive: record traces at spots where
 * everything needed is resident, after a settle wait. */
extern float frame_det_dt;       /* 0: off */
extern unsigned frame_det_seed;  /* the seed when frame_det_seeded */
extern int frame_det_seeded;     /* option seed N given (N may be 0) */
void frame_det_record(const char *path);
/* the recorder's buffers (272 KB), taken by option fixeddt and option record alike so a deterministic run's heap is the
 * same recorded or not; 0 (TEST error) when the heap has no room */
int frame_det_buffers(void);
/* the simulation clock in EE ticks: ticks() normally, frames x fixeddt with fixeddt */
unsigned frame_clock(void);
/* the clock a per-frame work budget reads (elapsed = two reads apart): ticks() normally; with fixeddt the frame's
 * clock plus FRAME_DET_READ_US per read so far this frame (deterministic work units) */
#ifndef FRAME_DET_READ_US
#define FRAME_DET_READ_US 40  /* ASSUMED: about one EMEVD instance run or 64 collector sweeps on the EE (a map's
                               * constructors: 300+ runs in 12-20 ms) */
#endif
unsigned frame_budget_clock(void);

/* What main.c hands the loop: the boot's results and its platform helpers */
typedef struct {
    GSGLOBAL *gs;
    Player *pl;
    int test_opts, model_ok, map_ok, col_ok, enemies, breakables, vram_free;
    unsigned t_load;
    const char *area;  /* the boot area (snd_map's fallback when no world block is current) */
    int (*pad_input)(Input *in, unsigned short *prev);
    void (*z_clear)(GSGLOBAL *gs);
    void (*flip)(GSGLOBAL *gs);
    unsigned (*ram_free)(void);
} FrameBoot;
void frame_run(const FrameBoot *b);  /* never returns */

#endif
