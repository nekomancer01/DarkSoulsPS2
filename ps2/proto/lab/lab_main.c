/* lab_main.c: the Lab (Polaris 1.7b-c): the simulate phase on the host, driven over stdin / stdout.
 *
 * One command a line, test-script names where one exists (src/testscript.c), answers one line each:
 *   boot ROOT AREA          load from ROOT/data as main.c does: c0000 (gamedata, the player's model and ESD), the world
 *                           table, AREA.pak's collision, objects and events, the other blocks' types (world_systems)
 *   spawn X Y Z YAW         place the player (as the test command, without the streaming wait)
 *   cam YAW PITCH           the camera's yaw and pitch (they steer movement)
 *   stick MX MY             the left stick for the next steps (as the test command)
 *   input MX MY CX CY BITS  a whole Input for the next steps (BITS: the trace's button word, frame.c record_frame)
 *   fixeddt MS              dt of a step (default 33.333)
 *   step N                  run N frames of the simulate phase: the player (phase_sim_a), the world update (phase_world,
 *                           settled: lab_main.c world_step), objects, events, world items, the bonfire (phase_sim_b)
 *   replay TRACE            run every frame of a recorded trace (option record), printing a state line per frame
 *   read                    "state FRAME X Y Z YAW STATE" (the format of option record's .state log)
 *   stubs                   the stub calls so far, "stub presentation|readback N NAME" (lab.h: readback = a layer
 *                           violation the simulation depends on)
 *   quit
 * Script mode (Polaris 1.7c): `lab ROOT AREA SCRIPT [MAX_FRAMES]` boots as main.c does with a test script (test_init,
 * save_boot, save_apply_player), runs the script through src/testscript.c's own interpreter frame by frame (input,
 * camera, then the simulate phase unless frozen, test_frame_end, save_tick) until it ends, prints the stub
 * report and "ok"; its output is the console's TEST lines. tools/lab.py run reads them. `lab ROOT AREA SCRIPT 0 states`
 * also prints each frame's state line (the format of option record's .state log).
 * The camera follow and its collision ray are not simulated (the trace carries the camera's yaw / pitch per frame;
 * nothing in simulate reads the camera's position except through r_camera_basis, a read-back stub). Enemies: 1.7c-3. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lab.h"
#include "pack.h"
#include "player.h"
#include "gamedata.h"
#include "items.h"
#include "chrstat.h"
#include "model.h"
#include "collision.h"
#include "status.h"
#include "combat.h"
#include "event.h"
#include "flags.h"
#include "progress.h"
#include "frame.h"
#include "save.h"
#include "map.h"
#include "world.h"
#include "objects.h"
#include "pickup.h"
#include "bonfire.h"
#include "testscript.h"
#include <libpad.h>

const char *lab_root = ".", *lab_area = "m10_02_00_00";
int lab_noenemies;
static Player pl;
static Input in;
static float yaw, pitch, dt = 1.0f / 30;
static unsigned frame;

#define NSTUB 128
static struct { const char *name; unsigned n; int readback; } stub[NSTUB];
void lab_count(const char *name, int readback)
{
    int i;
    for (i = 0; i < NSTUB && stub[i].name && strcmp(stub[i].name, name); i++)
        ;
    if (i < NSTUB)
        stub[i].name = name, stub[i].n++, stub[i].readback = readback;
}

static void world_step(void);
/* replay: the console's world_hold held the player this frame (trace flag 4: a block or collision tile still loading
 * there). The Lab never waits for data, so it takes the hold from the trace (link option --wrap=world_hold). */
static int lab_held;
int __real_world_hold(const float *feet);
int __wrap_world_hold(const float *feet) { return __real_world_hold(feet) || lab_held; }
/* frame.h frame_clock: the simulation clock in EE ticks, by frames (option fixeddt's clock on the console) */
static unsigned lab_clock;
unsigned frame_clock(void) { return lab_clock; }

static int boot(const char *root, const char *area, const char *script)
{
    lab_root = strdup(root);
    lab_area = strdup(area);
    char test_arg[600];
    char *argv[3] = {"lab", "save=off", NULL};
    if (script) {  /* main.c's order: the script first (its options pick the save device), then the save */
        snprintf(test_arg, sizeof test_arg, "test=%s", script);
        argv[1] = test_arg;
        lab_noenemies = (test_init(2, argv) & TEST_NOENEMIES) != 0;
        if (!test_active())
            return printf("error script %s\n", script), 0;
    }
    if (!gamedata_init())
        return printf("error gamedata\n"), 0;
    items_init();
    save_boot(2, argv, area);  /* flags + progress: a new game unless the script's "option save host" finds one */
    chrstat_boot();
    Pack pk;
    unsigned msize = 0;
    void *model_blob = pack_open(&pk, PACK_ROOT "c0000.pak") ? pack_load(&pk, "model.bin", &msize) : NULL;
    pack_close(&pk);
    if (!model_init(model_blob))
        return printf("error model\n"), 0;
    /* main.c's boot of the area: the world table, the boot block's collision, breakables, events, world items, the
     * other blocks' types */
    int world = world_init(area);
    char path[128];
    snprintf(path, sizeof path, PACK_ROOT "%s.pak", area);
    void *col = NULL, *obj = NULL, *ev = NULL, *evc = NULL;
    if (pack_open(&pk, path)) {
        col = pack_load(&pk, "collision.bin", NULL);
        obj = pack_load(&pk, "objects.bin", NULL);
        ev = pack_load(&pk, "events.bin", NULL);
        evc = pack_load(&pk, "evcommon.bin", NULL);
    }
    pack_close(&pk);
    if (world)
        world_boot_map(), world_stream_open();
    if (!col || !col_init(col))
        return printf("error collision %s\n", area), 0;
    objects_init(NULL, obj);
    if (ev && event_init(evc))
        event_block_on(0, ev);
    pickup_init(NULL, NULL, 0);
    world_systems(NULL);
    combat_sweep_hook = objects_sweep_hook;
    status_init();
    player_init(&pl);
    map_spawn(&pl.pos, &pl.yaw);  /* main.c's boot order: the area's spawn, the saved position, the script */
    pl.on_map = 1;
    save_apply_player(&pl);
    bonfire_init(area);
    world_step();  /* the boot position's blocks (main.c: the first frame's world update, a warp, settles them) */
    return 1;
}

/* frame.c phase_world's world update, then the same again until everything the feet want is in: the Lab's reads
 * complete at once (lab_stubs.c), so no block, collision or event set is ever "still loading" when simulate runs */
static void world_step(void)
{
    frame_phase = FP_WORLD;
    world_update(&pl.pos.x, dt);
    for (int g = 0; g < 64 && !world_settled(&pl.pos.x); g++) {
        WorldStats w = world_stats;
        world_update(&pl.pos.x, 0);
        if (w.loads == world_stats.loads && w.unloads == world_stats.unloads && w.reading == world_stats.reading &&
            w.loaded == world_stats.loaded)
            break;  /* a wanted block waits for something only time or the player's moving frees (a slot): the
                     * console's own rule, not a read in flight */
    }
}

/* One frame of simulate in frame.c's order: phase_sim_a's player, phase_world's world update, phase_sim_b's objects,
 * events, world items and bonfire (enemies: 1.7c-3) */
static void step(void)
{
    lab_clock += (unsigned)(dt * 294912000.0f);
    frame_phase = FP_SIM_A;
    player_update(&pl, &in, yaw, dt);
    status_update(&pl, dt);
    combat_player_frame(&pl, player_attack_anim(&pl), pl.t, dt);
    event_push_player((float *)&pl.pos, COL_PLAYER_R, COL_PLAYER_H);
    world_step();
    frame_phase = FP_SIM_B;
    objects_update(&pl, dt);
    event_update(&pl, dt);
    if (PICKUP)
        pickup_update(dt);
    bonfire_update(&pl, dt);
    frame++;
}

static void state_line(void)
{
    printf("state %u %.4f %.4f %.4f %.4f %s\n", frame, pl.pos.x, pl.pos.y, pl.pos.z, pl.yaw, player_state_name(&pl));
}

static void stubs(void)
{
    for (int i = 0; i < NSTUB && stub[i].name; i++)
        printf("stub %s %u %s\n", stub[i].readback ? "readback" : "presentation", stub[i].n, stub[i].name);
}

/* Script mode: frame.c's order for the parts the Lab has (phase_input's script and camera, phase_sim_a's player, the
 * frame's end) */
static void run_script(unsigned max_frames, int states)
{
    dt = frame_det_dt > 0 ? frame_det_dt : 1.0f / 30;  /* (no fixeddt: the console's 30 fps frame) */
    for (frame = 0; test_active() && frame < max_frames; frame++) {
        in = (Input){0};
        unsigned short pressed = test_pad();
        /* "pad" presses go to the UI only (frame.c phase_input: bonfire_input / menu_update), never to the player
         * (the script's input replaces the frame's). The bonfire's menu is game logic (bonfire.c) and runs here; the
         * Start menu does not: Start outside the bonfire fails the script. Select is the debug overlay. */
        int ui = bonfire_input(pressed);  /* the bonfire's own menu and dialogs (frame.c: before the Start menu) */
        if (!ui && (pressed & PAD_START))
            lab_unsupported("pad start", "the Start menu (UI)");
        else if (!ui && pressed)
            LAB_COUNT("pad (no menu open: no effect)");
        test_input(&in, &pl, &yaw, &pitch, dt, 0);
        cam_input(&pl, &in, &yaw, &pitch, dt);
        test_mb.shot_ack = test_mb.shot_seq;  /* shots: nothing to capture, the host answers at once */
        if (!test_frozen()) {
            unsigned f = frame;
            step();
            frame = f;
        }
        test_frame_end(&pl, dt, dt, 0, 0, 0, 0, 0);
        save_tick(&pl, dt);
        if (states)  /* the frame's state line, as option record's .state log (lab.py run --state) */
            state_line();
    }
    if (test_active())
        printf("TEST error lab: still running after %u frames\n", max_frames);
    printf("lab frames %u pos %.4f %.4f %.4f yaw %.4f state %s\n", frame, pl.pos.x, pl.pos.y, pl.pos.z, pl.yaw,
           player_state_name(&pl));
    stubs();
    printf("ok\n");
}


static void set_bits(unsigned b)
{
    in.r1 = b & 1, in.circle = b >> 1 & 1, in.r1_held = b >> 2 & 1, in.r2_held = b >> 3 & 1, in.l1_held = b >> 4 & 1;
    in.l2_held = b >> 5 & 1, in.r3 = b >> 6 & 1, in.square = b >> 7 & 1, in.triangle = b >> 8 & 1;
}

static void replay(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned h[5];
    if (!f || fread(h, 4, 5, f) != 5 || h[0] != 0x54525344 || h[1] != 2) {
        printf("error trace %s\n", path);
        if (f)
            fclose(f);
        return;
    }
    unsigned char r[64];
    int first = 1;
    while (fread(r, 64, 1, f) == 1) {
        const unsigned *u = (const unsigned *)r;
        const float *x = (const float *)r;
        frame = u[0], dt = x[1];
        in = (Input){0};
        in.mx = x[2], in.my = x[3], in.cx = x[4], in.cy = x[5];
        set_bits(u[6]);
        in.dpad = (int)u[7];
        yaw = x[8], pitch = x[9];
        if (u[10] & 1 || first)  /* moved since the last frame (a test spawn, a warp), or the start: as recorded */
            pl.pos = v3(x[11], x[12], x[13]), pl.yaw = x[14];
        lab_held = (u[10] & 4) != 0;  /* the console's world held the player (streaming): the Lab holds it the same */
        if (!(u[10] & 2))  /* (frozen frames run no simulate) */
            step();
        lab_held = 0;
        first = 0;
        frame = u[0];  /* (the .state log names the frame the input belongs to) */
        state_line();
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    char line[512];
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 3 && !boot(argv[1], argv[2], argc >= 4 ? argv[3] : NULL))
        return 1;
    if (argc >= 4) {
        run_script(argc >= 5 && atoi(argv[4]) > 0 ? (unsigned)atoi(argv[4]) : 30 * 60 * 30,
                   argc >= 6 && !strcmp(argv[5], "states"));
        return 0;
    }
    while (fgets(line, sizeof line, stdin)) {
        char cmd[32], a[256];
        float v[6];
        if (sscanf(line, "%31s", cmd) != 1)
            continue;
        const char *rest = line + strlen(cmd) + (line[strlen(cmd)] ? 1 : 0);
        if (!strcmp(cmd, "boot") && sscanf(rest, "%255s %31s", a, cmd) == 2)
            printf(boot(a, cmd, NULL) ? "ok\n" : "");
        else if (!strcmp(cmd, "spawn") && sscanf(rest, "%f %f %f %f", v, v + 1, v + 2, v + 3) == 4)
            pl.pos = v3(v[0], v[1], v[2]), pl.yaw = v[3], printf("ok\n");
        else if (!strcmp(cmd, "cam") && sscanf(rest, "%f %f", v, v + 1) == 2)
            yaw = v[0], pitch = v[1], printf("ok\n");
        else if (!strcmp(cmd, "stick") && sscanf(rest, "%f %f", v, v + 1) == 2)
            in.mx = v[0], in.my = v[1], printf("ok\n");
        else if (!strcmp(cmd, "input") && sscanf(rest, "%f %f %f %f %f", v, v + 1, v + 2, v + 3, v + 4) == 5)
            in.mx = v[0], in.my = v[1], in.cx = v[2], in.cy = v[3], set_bits((unsigned)v[4]), printf("ok\n");
        else if (!strcmp(cmd, "fixeddt") && sscanf(rest, "%f", v) == 1)
            dt = v[0] / 1000.0f, printf("ok\n");
        else if (!strcmp(cmd, "step")) {
            int n = atoi(rest);
            for (int i = 0; i < (n > 0 ? n : 1); i++)
                step();
            state_line();
        } else if (!strcmp(cmd, "replay") && sscanf(rest, "%255s", a) == 1)
            replay(a), printf("ok\n");
        else if (!strcmp(cmd, "read"))
            state_line();
        else if (!strcmp(cmd, "stubs")) {
            for (int i = 0; i < NSTUB && stub[i].name; i++)
                printf("stub %s %u %s\n", stub[i].readback ? "readback" : "presentation", stub[i].n, stub[i].name);
            printf("ok\n");
        } else if (!strcmp(cmd, "quit"))
            break;
        else
            printf("error unknown %s\n", cmd);
    }
    return 0;
}
