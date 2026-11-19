/* lab_main.c: the Lab (Polaris 1.7b): the player's simulate phase on the host, driven over stdin / stdout.
 *
 * One command a line, test-script names where one exists (src/testscript.c), answers one line each:
 *   boot ROOT AREA          load from ROOT/data: c0000 (gamedata, the player's model and ESD), AREA.pak (collision)
 *   spawn X Y Z YAW         place the player (as the test command, without the streaming wait)
 *   cam YAW PITCH           the camera's yaw and pitch (they steer movement)
 *   stick MX MY             the left stick for the next steps (as the test command)
 *   input MX MY CX CY BITS  a whole Input for the next steps (BITS: the trace's button word, frame.c record_frame)
 *   fixeddt MS              dt of a step (default 33.333)
 *   step N                  run N frames of the simulate phase's player part (frame.c phase_sim_a)
 *   replay TRACE            run every frame of a recorded trace (option record), printing a state line per frame
 *   read                    "state FRAME X Y Z YAW STATE" (the format of option record's .state log)
 *   stubs                   the stub calls so far (render, sound, IOP: layer violations to list)
 *   quit
 * The camera follow and its collision ray are not simulated: the trace carries the camera's yaw / pitch per frame. */
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

const char *lab_root = ".";
static Player pl;
static Input in;
static float yaw, pitch, dt = 1.0f / 30;
static unsigned frame;

#define NSTUB 128
static struct { const char *name; unsigned n; } stub[NSTUB];
void lab_count(const char *name)
{
    int i;
    for (i = 0; i < NSTUB && stub[i].name && strcmp(stub[i].name, name); i++)
        ;
    if (i < NSTUB)
        stub[i].name = name, stub[i].n++;
}

static int boot(const char *root, const char *area)
{
    lab_root = strdup(root);
    if (!gamedata_init())
        return printf("error gamedata\n"), 0;
    flags_init();
    progress_new_game();  /* (save_boot without a save: a new game) */
    items_init();
    chrstat_boot();
    Pack pk;
    unsigned msize = 0;
    void *model_blob = pack_open(&pk, PACK_ROOT "c0000.pak") ? pack_load(&pk, "model.bin", &msize) : NULL;
    pack_close(&pk);
    if (!model_init(model_blob))
        return printf("error model\n"), 0;
    char path[128];
    snprintf(path, sizeof path, PACK_ROOT "%s.pak", area);
    void *col = pack_open(&pk, path) ? pack_load(&pk, "collision.bin", NULL) : NULL;
    pack_close(&pk);
    if (!col || !col_init(col))
        return printf("error collision %s\n", area), 0;
    status_init();
    player_init(&pl);
    pl.on_map = 1;  /* (main.c: the player stands on the map once it is up) */
    return 1;
}

static void step(void)
{
    frame_phase = FP_SIM_A;
    player_update(&pl, &in, yaw, dt);
    status_update(&pl, dt);
    combat_player_frame(&pl, player_attack_anim(&pl), pl.t, dt);
    event_push_player((float *)&pl.pos, COL_PLAYER_R, COL_PLAYER_H);
    frame++;
}

static void state_line(void)
{
    printf("state %u %.4f %.4f %.4f %.4f %s\n", frame, pl.pos.x, pl.pos.y, pl.pos.z, pl.yaw, player_state_name(&pl));
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
        if (!(u[10] & 2))  /* (frozen frames run no simulate) */
            step();
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
    if (argc >= 3 && !boot(argv[1], argv[2]))
        return 1;
    while (fgets(line, sizeof line, stdin)) {
        char cmd[32], a[256];
        float v[6];
        if (sscanf(line, "%31s", cmd) != 1)
            continue;
        const char *rest = line + strlen(cmd) + (line[strlen(cmd)] ? 1 : 0);
        if (!strcmp(cmd, "boot") && sscanf(rest, "%255s %31s", a, cmd) == 2)
            printf(boot(a, cmd) ? "ok\n" : "");
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
                printf("stub %s %u\n", stub[i].name, stub[i].n);
            printf("ok\n");
        } else if (!strcmp(cmd, "quit"))
            break;
        else
            printf("error unknown %s\n", cmd);
    }
    return 0;
}
