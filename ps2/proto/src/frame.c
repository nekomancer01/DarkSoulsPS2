/* frame.c: the frame loop, one function per phase (frame.h has the phases and the rule). Moved from main.c as it was:
 * the same calls in the same order, except map_set_player, which moved from the start of the map pass to the end of
 * WORLD (it updates the draw groups enemies and events read; no packet depends on the order). */
#define NEWLIB_PORT_AWARE  /* fio calls (the trace recorder), as save.c */
#include <math.h>
#include <stdio.h>
#include <dmaKit.h>
#include <fileio.h>
#include <stdlib.h>
#include <libpad.h>
#include "frame.h"
#include "g2d.h"
#include "render.h"
#include "mem.h"
#include "ot.h"
#include "cvar.h"
#include "model.h"
#include "map.h"
#include "vu1.h"
#include "collision.h"
#include "combat.h"
#include "status.h"
#include "enemy.h"
#include "objects.h"
#include "fx.h"
#include "hud.h"
#include "menu.h"
#include "lpass.h"
#include "prof.h"
#include "hitch.h"
#include "testscript.h"
#include "pickup.h"
#include "debug_hud.h"
#include "world.h"
#include "save.h"
#include "event.h"
#include "bonfire.h"
#include "loading.h"
#include "sound.h"
#include "ticks.h"

#define CAM_WALL (R_NEAR > 0.25f ? R_NEAR + 0.05f : 0.3f)  /* camera margin in front of walls (m) */
#ifndef FRAME_SKIP_MAP
#define FRAME_SKIP_MAP cv_FRAME_SKIP_MAP  /* runtime switch, default 0 (src/cvar.h): EMIT_MAP writes no packets */
#endif

/* ---- the checks (frame.h) ---- */

FramePhase frame_phase = FP_BOOT;
static unsigned bad_draw[FE_N], bad_sim[FE_N], ui_from_sim;
static const char *const entry_name[FE_N] = {"vu1_begin", "vu1_add", "map_draw", "chr_draw", "fx_draw", "hud_draw",
                                            "menu_draw", "player_update", "enemy_update", "obj_update", "event_update",
                                            "world_update"};

void frame_draw_check(FrameEntry e)
{
    if (frame_phase == FP_BOOT || frame_emitting() || frame_phase == FP_PRESENT)
        return;
    bad_draw[e]++;
#ifdef FRAME_STRICT
    if (bad_draw[e] <= 4)
        printf("TEST error: frame: %s in a simulate phase (%d), caller 0x%08x\n", entry_name[e], frame_phase,
               (unsigned)__builtin_return_address(0));
#endif
}

void frame_sim_check(FrameEntry e)
{
    if (!frame_emitting())
        return;
    bad_sim[e]++;
#ifdef FRAME_STRICT
    if (bad_sim[e] <= 4)
        printf("TEST error: frame: %s in an emit phase (%d), caller 0x%08x\n", entry_name[e], frame_phase,
               (unsigned)__builtin_return_address(0));
#endif
}

void frame_ui_request(void)
{
    if (frame_phase != FP_BOOT && !frame_emitting() && frame_phase != FP_PRESENT)
        ui_from_sim++;
}

void frame_log(void)
{
    char buf[256];
    int n = 0;
    for (int e = 0; e < FE_N && n < (int)sizeof buf - 32; e++)
        if (bad_draw[e] || bad_sim[e])
            n += snprintf(buf + n, sizeof buf - n, " %s %u/%u", entry_name[e], bad_draw[e], bad_sim[e]);
    buf[n] = 0;
    printf("frame: out of phase (draw in simulate / simulate in emit):%s%s | ui requests from simulate %u%s\n",
           n ? "" : " none", buf, ui_from_sim, FRAME_SKIP_MAP ? " | map emit skipped" : "");
}

/* ---- determinism (frame.h) ---- */

float frame_det_dt;
unsigned frame_det_seed;
int frame_det_seeded;
static unsigned det_clock;
static V3 rec_end;  /* the player at the last frame's end (record_frame: moved since?) */
static float rec_end_yaw;
static int rec = -1, rec_state = -1;  /* fio descriptors (host: save/, writable in test runs: pcsx2-run.sh) */
static unsigned rec_frame;
static char rec_line[96];

unsigned frame_clock(void) { return frame_det_dt > 0 ? det_clock : ticks(); }
static unsigned det_work;  /* work reported this frame, in EE ticks (fixeddt) */
unsigned frame_budget_clock(void) { return frame_det_dt > 0 ? det_clock + det_work : ticks(); }
void frame_det_work(unsigned t)
{
    if (frame_det_dt > 0)
        det_work += t;
}

void frame_det_record(const char *path)
{
    char p1[96], p2[104];
    snprintf(p1, sizeof p1, "host:save/%s", path);  /* (build/save/, as test saves: save.c) */
    snprintf(p2, sizeof p2, "%s.state", p1);
    rec = fioOpen(p1, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
    rec_state = fioOpen(p2, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
    if (rec < 0 || rec_state < 0) {
        printf("TEST error: frame: cannot write the input trace %s\n", p1);
        rec = rec_state = -1;
        return;
    }
    /* header: magic, version, seed, fixed dt (us), record size */
    const unsigned h[5] = {0x54525344 /* DSRT */, 2, frame_det_seed, (unsigned)(frame_det_dt * 1e6f), 64};
    fioWrite(rec, h, sizeof h);
    rec_frame = 0;
    rec_end = v3(1e30f, 1e30f, 1e30f);  /* (the first record carries the start position) */
}

/* one 64-byte record a frame (version 2): 0 frame, 1 dt, 2-5 sticks, 6 buttons, 7 d-pad, 8-9 camera yaw / pitch (they
 * steer movement), 10 flags (1: the player was moved since the last frame's end: a test spawn, a warp, a respawn;
 * 2: frozen, the simulate phase does not run), 11-14 the player's position and yaw at the frame's start (flag 1) */
static void record_frame(const Input *in, float dt, float yaw, float pitch, const Player *pl, int frozen)
{
    unsigned char r[64] = {0};
    unsigned *u = (unsigned *)r;
    float *f = (float *)r;
    u[0] = rec_frame, f[1] = dt, f[2] = in->mx, f[3] = in->my, f[4] = in->cx, f[5] = in->cy;
    u[6] = (unsigned)in->r1 | (unsigned)in->circle << 1 | (unsigned)in->r1_held << 2 | (unsigned)in->r2_held << 3 |
           (unsigned)in->l1_held << 4 | (unsigned)in->l2_held << 5 | (unsigned)in->r3 << 6 | (unsigned)in->square << 7 |
           (unsigned)in->triangle << 8;
    u[7] = (unsigned)in->dpad, f[8] = yaw, f[9] = pitch;
    int moved = pl->pos.x != rec_end.x || pl->pos.y != rec_end.y || pl->pos.z != rec_end.z || pl->yaw != rec_end_yaw;
    u[10] = (unsigned)moved | (unsigned)frozen << 1;
    f[11] = pl->pos.x, f[12] = pl->pos.y, f[13] = pl->pos.z, f[14] = pl->yaw;
    fioWrite(rec, r, sizeof r);
}

/* ---- the loop ---- */

/* the order table's deferred entries (ot.h) */
static void run_flip(void *b) { ((const FrameBoot *)b)->flip(((const FrameBoot *)b)->gs); }

typedef struct {
    const FrameBoot *b;
    GSGLOBAL *gs;
    Player *pl;
    /* across frames */
    float cam_yaw, cam_pitch, cam_dist, cam_y, idle, avg_dt;
    float t_cull, t_char, t_wait;  /* ms, smoothed */
    unsigned last;
    int tris_drawn, hud_debug, vram_free;
    unsigned short prev;
    /* this frame */
    float dt, dt_real;
    Input in;
    int demo, frozen;
    V3 eye, target;
    unsigned t1;
} Frame;

static void phase_begin(Frame *f)
{
    PROF_FRAME();
    ot_frame_begin();
    g2d_frame_begin();  /* this frame's 2D buffers (frame region) */
#ifdef OT_HASH
    ot_hash_begin(test_frozen());  /* the draw-trace hashes of this frame (ot.h) */
#endif
    unsigned now = ticks();
    f->dt = (now - f->last) / 294912000.0f;
    f->dt_real = f->dt;  /* test stats: the frame time before the clamps */
    hitch_where(&f->pl->pos.x, world_current());  /* hitch / spike lines say where (src/hitch.c) */
    hitch_frame(f->dt_real);  /* the last pass of this loop, its sections (src/hitch.c) */
#ifdef SLOW_FRAME_LOG  /* make EE_DBGINFOFLAGS=-DSLOW_FRAME_LOG: frames over 100 ms in the EE log (ISO stalls) */
    if (f->dt_real > 0.1f)
        printf("DSRPROTO slow frame %d ms\n", (int)(f->dt_real * 1000));
#endif
    f->last = now;
    f->avg_dt += (f->dt - f->avg_dt) * 0.1f;  /* real frame time, before the clamps below */
    if (f->dt > 1.0f / 15)
        f->dt = 1.0f / 15;  /* below 15 fps the game slows down instead of skipping */
    if (f->dt < 1.0f / 60)
        f->dt = 1.0f / 60;
    if (frame_det_dt > 0) {  /* option fixeddt (frame.h): the frame's dt and the simulation clock, by frames */
        f->dt = frame_det_dt;
        if (!f->frozen)
            det_clock += (unsigned)(frame_det_dt * 294912000.0f);
        det_work = 0;
    }
}

static void phase_input(Frame *f)
{
    frame_phase = FP_INPUT;
    Player *pl = f->pl;
    float dt = f->dt;
    f->in = (Input){0};
    unsigned short held = f->prev;
    if (f->b->pad_input(&f->in, &f->prev))
        f->idle = 0;
    else
        f->idle += dt;
    unsigned short pressed = f->prev & ~held;
    pressed |= test_pad();  /* test script "pad" presses */
    if (pressed & PAD_SELECT)
        f->hud_debug = !f->hud_debug;
    if (pressed & PAD_L3) {  /* debug: helm off / on, to see the face (DSR's hide rule, model_helmet) */
        static int helm_off;
        model_helmet(!(helm_off = !helm_off));
    }
    int ui_took;
    MEM_LEGACY(UI) ui_took = bonfire_input(pressed) || menu_update(pressed);
    if (ui_took) {  /* bonfire / Start menu: no movement or attacks */
        f->in = (Input){0};
        f->idle = 0;
    }
    f->demo = f->idle > 5 && (f->b->test_opts & TEST_DEMO) && !test_active();  /* test-only ("option demo") */
    if (test_active() || f->demo)  /* test script (pad ignored), else the idle demo when a script asked for it */
        MEM_LEGACY(TEST) test_input(&f->in, pl, &f->cam_yaw, &f->cam_pitch, dt, f->demo);
    test_build_input(&f->in, pl, &f->cam_yaw, &f->cam_pitch, dt);  /* COL_TEST / ESD_TEST / CHAR_CLOSEUP / ENEMY_VIEW */

    cam_input(pl, &f->in, &f->cam_yaw, &f->cam_pitch, dt);  /* (src/cam_input.c: the Lab runs it too) */
}

static void phase_sim_a(Frame *f)
{
    frame_phase = FP_SIM_A;
    Player *pl = f->pl;
    float dt = f->dt;
    f->frozen = test_frozen();  /* test scripts freeze the world while a shot is taken (and for render-only runs) */
    PROF_GATE(test_mb.state, f->frozen);  /* PROFILE: whole-run percentiles over the running script's frames */
    HITCH_BEGIN(pre);
    if (!f->frozen) {
        PROF_MARK(PROF_PLAYER);
        LG_BEGIN(player);
        MEM_LEGACY(PLAYER) {
            frame_sim_check(FE_PLAYER_UPDATE);
            player_update(pl, &f->in, f->cam_yaw, dt);
            status_update(pl, dt);
            combat_player_frame(pl, player_attack_anim(pl), pl->t, dt);  /* hurtbox sync + weapon hit sweeps */
            /* fog walls / gates as the last frame's event_update left them: before the camera (it follows the feet) */
            event_push_player((float *)&pl->pos, COL_PLAYER_R, COL_PLAYER_H);  /* V3 as float[3]: &pl.pos.x trips gcc 15 -Wstringop-overflow */
        }
        LG_END(player, LG_PLAYER);
        PROF_MARK(PROF_ENEMY);
        LG_BEGIN(stream);
        MEM_LEGACY(ENEMY) enemy_stream(pl);  /* type loads: arena allocations and PATH3 texture uploads stay before the map kick */
        LG_END(stream, LG_STREAM);
    }
    HITCH_END(pre, HS_PRE);

    PROF_MARK(PROF_CAMERA);
    /* third-person orbit camera behind the player */
    /* LockCamParam row 0: camDistTarget 4.0 m, chrOrgOffset_Y 1.42 m */
    /* the follow height eases after the feet (steps, landings) instead of copying them; teleports snap */
    float ky = 1 - expf(-dt / 0.15f);
    f->cam_y = fabsf(pl->pos.y - f->cam_y) > 3 ? pl->pos.y : f->cam_y + (pl->pos.y - f->cam_y) * ky;
    f->target = v3(pl->pos.x, f->cam_y + 1.42f, pl->pos.z);
    float d = test_build_cam_dist(4.0f);
    f->eye = v3add(f->target, v3(-sinf(f->cam_yaw) * cosf(f->cam_pitch) * d, sinf(f->cam_pitch) * d,
                                 -cosf(f->cam_yaw) * cosf(f->cam_pitch) * d));
    /* camera collision: pull in at once in front of the first wall hit (CAM_WALL margin: the near plane plus
     * 0.05 m, 0.3 m with the old 0.25 m plane), ease back out at 4 m/s */
    float want = d;
    ColHit ch;
    float ray0[3] = {f->target.x, f->target.y, f->target.z}, ray1[3] = {f->eye.x, f->eye.y, f->eye.z};
    if (f->b->col_ok && col_ray(ray0, ray1, COL_CAMERA, &ch))
        want = ch.t * d - CAM_WALL > 0.2f ? ch.t * d - CAM_WALL : 0.2f;
    f->cam_dist = want < f->cam_dist ? want : f->cam_dist + 4.0f * dt > want ? want : f->cam_dist + 4.0f * dt;
    f->eye = v3add(f->target, v3scale(v3sub(f->eye, f->target), f->cam_dist / d));
    test_build_camera(&f->eye, &f->target);  /* ENEMY_VIEW check build */
    r_camera(f->eye, f->target);
}

static void phase_world(Frame *f)
{
    frame_phase = FP_WORLD;
    float dt = f->dt;
    PROF_MARK(PROF_STREAM);
    HITCH_BEGIN(world);
    MEM_LEGACY(WORLD) {
        frame_sim_check(FE_WORLD_UPDATE);
        world_update(&f->pl->pos.x, dt);  /* background block reads; turns blocks on / off (src/world.c) */
        loading_frame();
    }  /* the boot's / a death's loading screen closes once this frame's loads settled */
    HITCH_END(world, HS_WORLD);
    PROF_MARK(PROF_SOUND);
    HITCH_BEGIN(snd);
    MEM_LEGACY(SND) {
        snd_map(world_current() >= 0 ? world_map(world_current()) : f->b->area);  /* area bank, ambience (src/sound.c) */
        snd_update(dt);
    }  /* listener = this camera; emitters, voices, streams; one RPC to the IOP */
    HITCH_END(snd, HS_SND);
    PROF_MARK(PROF_STREAM);  /* (the STREAM pass again: before the split this ran inside the MAP pass) */
    if (f->b->map_ok)  /* the floor's draw groups (enemies and events read them in SIM_B) and the streamer's update */
        MEM_LEGACY(WORLD) map_set_player(f->pl->pos);
}

static void phase_emit_map(Frame *f)
{
    frame_phase = FP_EMIT_MAP;
    GSGLOBAL *gs = f->gs;
    PROF_MARK(PROF_MAP);
    HITCH_BEGIN(map);
    if (f->b->map_ok && !FRAME_SKIP_MAP) {
        /* Z clear and the map in one VU1 chain (PATH2 + PATH1), then gsKit (PATH3) for the rest */
        unsigned t0 = ticks();
        {
            ot_open(OT_SKY, OT_BLEND);  /* map.c's own order inside: sky, panorama, horizon, world, relief, blended chunks */
            f->b->z_clear(gs);
            frame_draw_check(FE_MAP_DRAW);
            MEM_LEGACY(WORLD) f->tris_drawn = map_draw(f->eye);
            ot_close();
            ot_flush();  /* VU1 draws the map while the EE runs the game logic and poses the characters below */
        }
        f->t_cull += ((ticks() - t0) / 294912.0f - f->t_cull) * 0.1f;
    } else if (!f->b->map_ok) {
        g2d_clear(gs, GS_SETREG_RGBAQ(0xFF, 0xFF, 0xFF, 0x80, 0));
        r_ground(40, 2);
        ot_g2d(OT_WORLD, OT_WORLD, G2D_WORLD);  /* before the character's VU1 chain draws on top */
        ot_flush();
        dmaKit_wait_fast();  /* (the WORLD stream is rebuilt in this frame's buffer from here on) */
    }
    HITCH_END(map, HS_MAP);
}

static void phase_sim_b(Frame *f)
{
    frame_phase = FP_SIM_B;
    Player *pl = f->pl;
    float dt = f->dt;
    HITCH_BEGIN(logic);
    if (!f->frozen) {  /* nothing here may touch what the map chain reads (mapstream cells, PATH2/3 GS registers) */
        PROF_MARK(PROF_ENEMY);
        LG_BEGIN_X(enemy);
        frame_sim_check(FE_ENEMY_UPDATE);
        MEM_LEGACY(ENEMY) enemy_update(pl, dt);  /* AI, enemyCommon ESD, movement, attack sweeps on the player */
        LG_END_X(enemy, LG_ENEMY);
        PROF_MARK(PROF_OBJECTS);
        LG_BEGIN(objects);
        frame_sim_check(FE_OBJ_UPDATE);
        MEM_LEGACY(OBJ) objects_update(pl, dt);  /* breakables: push-out, roll / landing breaks, piece physics */
        LG_END(objects, LG_OBJECTS);
        LG_BEGIN(events);
        MEM_LEGACY(EVENT) {
            frame_sim_check(FE_EVENT_UPDATE);
            event_update(pl, dt);    /* EMEVD instances (src/event.c); fog walls / gates push the player next frame */
            if (PICKUP)
                pickup_update(dt);    /* item get dialog (src/pickup.c) */
        }
        LG_END(events, LG_EVENTS);
        LG_BEGIN(bonfire);
        MEM_LEGACY(EVENT) bonfire_update(pl, dt);  /* bloodstain pickup (src/bonfire.c) */
        LG_END(bonfire, LG_BONFIRE);
    }
    HITCH_END(logic, HS_LOGIC);
    if (!f->frozen)  /* a frozen frame (test shots) runs no logic: not part of the averages */
        lpass_frame(hitch_acc[HS_PRE] + hitch_acc[HS_LOGIC]);
}

static void phase_emit(Frame *f)
{
    frame_phase = FP_EMIT;
    Player *pl = f->pl;
    float dt = f->dt;
    PROF_MARK(PROF_CHARS);
    f->t1 = ticks();
    {
        ot_open(OT_CHR, OT_CHR);
#ifndef MAP_PANO_R  /* panorama capture builds (render-distance, map.c): the far map only */
        frame_draw_check(FE_CHR_DRAW);
        player_draw(pl);  /* bone matrices on the EE, skinning + triangles on VU1 (vu1skin.vsm) */
        enemy_draw();      /* same path, one bone palette per enemy */
#endif
        ot_close();
        ot_open(OT_OBJ, OT_OBJ);  /* (the characters' buffer: one kick with them, it inherits their state) */
#ifndef MAP_PANO_R
        objects_draw();    /* breakables: intact pots or their pieces, one palette per object */
#endif
        ot_close();
        ot_flush();
    }
    HITCH_BEGIN(fx);
    MEM_LEGACY(FX) {
        fx_update(frame_det_dt > 0 && f->frozen ? 0 : dt, f->eye);  /* effects: EE particles while VU1 draws (time held on a
                                                                     * frozen frame with fixeddt) */
#ifndef MAP_PANO_R
        frame_draw_check(FE_FX_DRAW);
        fx_draw();
#endif
    }
    HITCH_END(fx, HS_FX);
    bonfire_draw();  /* the bloodstain marker */
    debug_draw_world();  /* COMBAT_DUMMY hurtbox, blade capsule */
    f->t_char += ((ticks() - f->t1) / 294912.0f - f->t_char) * 0.1f;

    /* HUD: DSR's bars, quick slots, souls, YOU DIED (hud.c); Start menu (menu.c); Select: debug overlay */
    PROF_MARK(PROF_HUD);
    HITCH_BEGIN(hud);
    unsigned mt_ui = mem_legacy_begin(MEMT_UI);
    g2d_select(G2D_UI);
#ifndef MAP_PANO_R
    frame_draw_check(FE_HUD_DRAW);
    hud_draw(pl, frame_det_dt > 0 && f->frozen ? 0 : dt);
#endif
    if (f->demo)
        r_text_outlined(30, 410, 2, "DEMO - PRESS ANY BUTTON");
    if (f->hud_debug) {
        const FrameBoot *b = f->b;
        const DebugHud dh = {f->cam_yaw, f->cam_pitch, f->cam_dist, f->cam_y, f->avg_dt, f->t_cull, f->t_char,
                             f->t_wait, b->t_load, b->ram_free(), f->tris_drawn, f->vram_free, b->model_ok, b->map_ok,
                             b->col_ok, b->enemies, b->breakables};
        HITCH_BEGIN(dbg);
        debug_hud_draw(&dh, pl, dt);
        HITCH_END(dbg, HS_DBG);
    }
    if (PICKUP)
        pickup_draw();  /* the item get dialog (menu.drb F25) */
    frame_draw_check(FE_MENU_DRAW);
    menu_draw(pl);
    loading_overlay(frame_det_dt > 0 && f->frozen ? 0 : dt);  /* the fade-in from black after a loading screen */
    mem_legacy_end(mt_ui);
    PROF_DRAW();
    HITCH_END(hud, HS_HUD);
}

static void phase_present(Frame *f)
{
    frame_phase = FP_PRESENT;
    GSGLOBAL *gs = f->gs;
    Player *pl = f->pl;
    PROF_MARK(PROF_VU1WAIT);
    hitch_acc[HS_CHR] += ticks() - f->t1;
    unsigned t2 = ticks();
    fx_kick();   /* FX_VU1: the particle chain (closes its segment), after the characters' */
    {
        ot_flush();
        ot_wait();  /* PATH1 must finish before gsKit's PATH3 packets draw over it */
    }
    mem_frame_swap();  /* the frame buffer two frames old: its chains are done (mem.h) */
    f->t_wait += ((ticks() - t2) / 294912.0f - f->t_wait) * 0.1f;
    hitch_acc[HS_WAIT] += ticks() - t2;
    PROF_MARK(PROF_FLIP);
    HITCH_BEGIN(flip);
    /* gsKit's queue: the EE-path triangles (BLEND), the effects' gsKit packets (FX), HUD and menus (UI); then the far
     * depth of field and the flip (POST). The queue's span starts below FX but runs after it: ot.h, TECH_DEBT render-18 */
    {
        ot_g2d(OT_BLEND, OT_FX, G2D_WORLD);  /* the EE-path triangles (map, effects) and debug lines */
        ot_g2d(OT_UI, OT_UI, G2D_UI);         /* HUD, menus, dialogs, text: before the far depth of field, as before 1.6 */
        ot_deferred(OT_UI, OT_UI, ot_gskit_leftover, gs);
        ot_deferred(OT_POST, OT_POST, run_flip, (void *)f->b);
        ot_flush();
    }
    if (f->vram_free >= 0)  /* character / block textures come and go (TECH_DEBT world-4) */
        f->vram_free = r_vram_avail(gs);
#ifdef OT_HASH
    ot_hash_frame(f->frozen);  /* (frozen test frames: the world holds still while a shot is taken) */
#endif
    HITCH_END(flip, HS_FLIP);
    HITCH_BEGIN(save);
    MEM_LEGACY(TEST) test_frame_end(pl, f->avg_dt, f->dt_real, f->t_cull, f->t_char, f->t_wait, f->tris_drawn,
                                    f->vram_free);
    MEM_LEGACY(SAVE) save_tick(pl, f->dt);  /* requested saves + autosave */
    static unsigned log_frames;
    if (++log_frames % 300 == 0)
        printf("DSRPROTO chain qw: map %d peak %d, chr %d peak %d, segments %d; CLUT loads %d + %d, batches %d\n",
               vu1_stats.last[0], vu1_stats.peak[0], vu1_stats.last[1], vu1_stats.peak[1], vu1_stats.segments,
               vu1_stats.clut_loads[0], vu1_stats.clut_loads[1], map_vu1_batches);
    if (log_frames % 300 == 0)  /* indexed map batches so far / EE-split clip triangles / batches clipped on VU1 */
        printf("DSRPROTO idx batches %d, clip tris %d, vu clip batches %d; EE ms so far: idx clip split %u, "
               "chain waits %u\n", vu1_stats.idx_batches, vu1_stats.idx_clip, vu1_stats.idx_vuclip,
               (unsigned)(vu1_stats.idx_clip_ticks / 294912),
               (unsigned)(vu1_stats.wait_ticks / 294912));
    if (log_frames % 300 == 0)  /* EE time in the batch builders so far (map and relief passes) */
        printf("DSRPROTO add ms so far: idx %u (%d calls), ex %u (%d calls)\n",
               (unsigned)(vu1_stats.add_idx_ticks / 294912), vu1_stats.add_idx_n,
               (unsigned)(vu1_stats.add_ex_ticks / 294912), vu1_stats.add_ex_n);
#ifdef VU1_TEX_PATH3
    if (log_frames % 300 == 0)
        printf("DSRPROTO tex path3 %d, path2 %d, gates %d\n", vu1_stats.tex_path3, vu1_stats.tex_path2,
               vu1_stats.tex_gates);
#endif
    HITCH_END(save, HS_SAVE);
}

void frame_run(const FrameBoot *b)
{
    Frame f = {0};
    f.b = b, f.gs = b->gs, f.pl = b->pl;
    f.cam_yaw = b->pl->yaw, f.cam_pitch = 0.25f, f.cam_dist = 4.0f, f.cam_y = -1e9f, f.idle = 99, f.avg_dt = 1.0f / 60;
    f.last = ticks();
    f.vram_free = b->vram_free;
    f.hud_debug = debug_hud_default(b->test_opts);  /* Select: fps / VU1 / collision / load overlay (debug_hud.c) */
    for (;;) {
        phase_begin(&f);
        phase_input(&f);
        if (rec >= 0)
            record_frame(&f.in, f.dt, f.cam_yaw, f.cam_pitch, f.pl, test_frozen()), rec_frame++;
        /* Frame order: the player, camera and world first, then the map chain is kicked and VU1 / the GS draw it
         * while the EE runs enemies, objects and events (refactor target 1, step A). What the map chain reads (camera,
         * draw groups, streamed cells) is settled before the kick; what those updates change shows from the next
         * frame */
        phase_sim_a(&f);
        phase_world(&f);
        phase_emit_map(&f);
        phase_sim_b(&f);
        phase_emit(&f);
        phase_present(&f);
        rec_end = f.pl->pos, rec_end_yaw = f.pl->yaw;
        if (rec_state >= 0) {
            int n = snprintf(rec_line, sizeof rec_line, "%u %.4f %.4f %.4f %.4f %s\n", rec_frame - 1, f.pl->pos.x,
                             f.pl->pos.y, f.pl->pos.z, f.pl->yaw, player_state_name(f.pl));
            fioWrite(rec_state, rec_line, n);
        }
    }
}
