/* In-ELF test runner: script parser + interpreter, per-frame stats mailbox (testscript.h has the script syntax). */
#define NEWLIB_PORT_AWARE  /* fio calls, as pack.c (same host: device) */
#include <kernel.h>
#include <fileio.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "testscript.h"
#include "collision.h"
#include "objects.h"
#include "map.h"
#include "vu1.h"
#include "enemy.h"
#include "save.h"
#include "event.h"

volatile TestMailbox test_mb __attribute__((aligned(64))) = {.magic = 0x4D545354, .version = 1};

enum { C_SPAWN, C_CAM, C_CAMOFF, C_STICK, C_RSTICK, C_CIRCLE, C_R1, C_WAIT, C_FRAMES, C_APPROACH, C_FREEZE, C_STATS, C_PITCH,
       C_SHOT, C_LOG, C_LOOP, C_END, C_GAME, C_EV };

typedef struct {
    unsigned short op, line;
    float a[4];
    char *text;  /* shot name / log text / "obj" for approach */
} Cmd;

/* The idle demo (was DEMO[] in main.c): walk, run, R1 combo, roll, dash, backstep. A Circle tap released with the
 * stick centred is a backstep (ESD slot 10), with the stick held a roll (slot 15). */
static const char demo_script[] =
    "wait 1\nstick 0 0.4\nwait 2\nstick 0.3 1\nwait 2\nstick 0 0\nr1\nwait 0.95\nr1\nwait 0.85\nr1\nwait 1.65\n"
    "stick 0 1\ncircle 1\nwait 0.15\nstick 0 0\ncircle 0\nwait 1.5\nstick -0.5 1\ncircle 1\nwait 2.5\n"
    "stick 0 0\ncircle 0\nrstick 0.6 0\nwait 1\nrstick 0 0\nstick 1 0\ncircle 1\nwait 0.15\ncircle 0\nwait 0.1\n"
    "stick 0 0\nwait 0.9\nr1\nwait 2.05\nloop\n";

typedef struct {
    Cmd *cmd;
    int n, pc;
    float t;        /* time in the current wait / approach */
    int frames;     /* frames in the current frames-wait */
    Input held;     /* sticks / Circle held between commands */
    int cam_on;
    float cam_yaw, cam_pitch;
    int done;
} Prog;

static Prog script, demo;
static int frozen, shot_wait, skipmiss;

static inline unsigned ticks(void)
{
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

static char *dupstr(const char *s, int n)
{
    char *d = malloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* Parses text (modified in place) into p; returns option bits */
static int parse(Prog *p, char *text)
{
    static const struct { const char *name; int op, args; } ops[] = {
        {"spawn", C_SPAWN, 4}, {"cam", C_CAM, 2},   {"stick", C_STICK, 2}, {"rstick", C_RSTICK, 2},
        {"circle", C_CIRCLE, 1}, {"r1", C_R1, 0},   {"wait", C_WAIT, 1},   {"frames", C_FRAMES, 1},
        {"approach", C_APPROACH, 3}, {"freeze", C_FREEZE, 1}, {"pitch", C_PITCH, 1}, {"stats", C_STATS, 0}, {"shot", C_SHOT, 0},
        {"log", C_LOG, 0}, {"loop", C_LOOP, 0}, {"end", C_END, 0},
        /* game state (save.c save_test_cmd): event flags, souls, items, checks, save / load */
        {"flag", C_GAME, 0}, {"flags", C_GAME, 0}, {"souls", C_GAME, 0}, {"item", C_GAME, 0}, {"check", C_GAME, 0},
        {"save", C_GAME, 0}, {"load", C_GAME, 0}, {"wipe", C_GAME, 0},
        /* EMEVD runtime (event.c event_test_cmd): action, expect flag / obj / hit / treasure / x / y / z, warp, trace */
        {"ev", C_EV, 0},
    };
    int opts = 0, cap = 0, line = 0;
    memset(p, 0, sizeof *p);
    for (char *s = text; *s;) {
        char *e = s;
        while (*e && *e != '\n')
            e++;
        char *next = *e ? e + 1 : e;
        *e = 0;
        line++;
        char *h = strchr(s, '#');
        if (h)
            *h = 0;
        char word[16];
        int used = 0;
        if (sscanf(s, " %15s%n", word, &used) == 1) {
            char *rest = s + used;
            while (*rest == ' ' || *rest == '\t')
                rest++;
            int len = strlen(rest);
            while (len && (rest[len - 1] == '\r' || rest[len - 1] == ' '))
                rest[--len] = 0;
            if (!strcmp(word, "option")) {
                opts |= !strcmp(rest, "noenemies") ? TEST_NOENEMIES : !strcmp(rest, "hud") ? TEST_HUD
                      : !strcmp(rest, "holes") ? TEST_HOLES : !strcmp(rest, "demo") ? TEST_DEMO : 0;
                skipmiss |= !strcmp(rest, "skipmiss");
                if (!strncmp(rest, "save ", 5))
                    save_select(rest + 5);  /* "option save mc|host [name]": saves on in this run (save.h) */
            } else {
                unsigned k;
                for (k = 0; k < sizeof ops / sizeof ops[0] && strcmp(word, ops[k].name); k++)
                    ;
                if (k == sizeof ops / sizeof ops[0]) {
                    printf("TEST error line %d: unknown command '%s'\n", line, word);
                } else {
                    if (p->n == cap) {
                        cap = cap ? cap * 2 : 64;
                        p->cmd = realloc(p->cmd, cap * sizeof *p->cmd);
                    }
                    Cmd *c = &p->cmd[p->n++];
                    memset(c, 0, sizeof *c);
                    c->op = ops[k].op;
                    c->line = line;
                    c->a[3] = NAN;  /* spawn without yaw keeps the facing */
                    if (c->op == C_CAM && !strcmp(rest, "off"))
                        c->op = C_CAMOFF;
                    else if (c->op == C_SHOT || c->op == C_LOG || c->op == C_EV)
                        c->text = dupstr(rest, len);
                    else if (c->op == C_GAME)  /* the whole line: 8-digit flag ids do not fit a float */
                        c->text = dupstr(s + used - strlen(word), rest + len - (s + used - strlen(word)));
                    else if (c->op == C_APPROACH && !strncmp(rest, "obj", 3)) {
                        c->text = "obj";
                        sscanf(rest + 3, "%f %f", &c->a[2], &c->a[3]);
                    } else {
                        int got = sscanf(rest, "%f%*[ ,]%f%*[ ,]%f%*[ ,]%f", &c->a[0], &c->a[1], &c->a[2], &c->a[3]);
                        if (ops[k].args && got < (ops[k].args > 3 ? 3 : ops[k].args))
                            printf("TEST error line %d: '%s' needs %d numbers\n", line, word, ops[k].args);
                    }
                }
            }
        }
        s = next;
    }
    return opts;
}

int test_init(int argc, char **argv)
{
    char tmp[sizeof demo_script];
    memcpy(tmp, demo_script, sizeof tmp);
    parse(&demo, tmp);  /* demo has no strings that outlive tmp (no shot / log) */
    printf("TEST mailbox 0x%08x size %d\n", (unsigned)&test_mb, (int)sizeof test_mb);
    const char *path = 0;
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "test=", 5))
            path = argv[i] + 5;
    if (!path)
        return 0;
    char full[128];
    snprintf(full, sizeof full, "%s%s", strchr(path, ':') ? "" : "host:", path);
    int fd = fioOpen(full, O_RDONLY);
    if (fd < 0) {
        printf("TEST error: cannot open %s\n", full);
        return 0;
    }
    int size = fioLseek(fd, 0, SEEK_END);
    fioLseek(fd, 0, SEEK_SET);
    char *text = malloc(size + 1);
    int got = text ? fioRead(fd, text, size) : -1;
    fioClose(fd);
    if (got != size) {
        printf("TEST error: read %s\n", full);
        free(text);
        return 0;
    }
    text[size] = 0;
    int opts = parse(&script, text);  /* shot / log strings are copies; text is only scanned */
    free(text);
    printf("TEST script %s: %d commands, options 0x%x\n", full, script.n, opts);
    test_mb.state = script.n ? 1 : 0;
    return opts;
}

int test_active(void) { return script.n && !script.done; }
int test_frozen(void) { return frozen || shot_wait; }

/* Returns 0 when there is no collision floor under the point */
static int teleport(Player *pl, float x, float y, float z, float yaw)
{
    ColHit h;
    int ok = col_floor(x, z, y + 3, y - 10, COL_SOLID, &h);
    pl->pos = v3(x, ok ? h.y : y, z);
    if (yaw == yaw)
        pl->yaw = yaw;
    pl->vel = v3(0, 0, 0);
    pl->airborne = 0;
    pl->fall_y0 = pl->pos.y;
    printf("TEST spawn %.2f %.2f %.2f floor %s %.2f\n", x, y, z, ok ? "hit" : "MISS", pl->pos.y);
    return ok;
}

static void seg_reset(void)
{
    test_mb.seg_frames = 0;
    test_mb.seg_ms_avg = test_mb.seg_ms_max = test_mb.seg_map_max = 0;
    test_mb.seg_ram_min = 0x7FFFFFFF;
    test_mb.seg_chain_map_max = test_mb.seg_chain_chr_max = 0;
}

/* Runs commands until one waits; returns 0 when the program has ended */
static int step(Prog *p, Input *in, Player *pl, float dt, int is_script)
{
    in->r1 = 0;
    for (int guard = 0; p->pc < p->n && guard < 64; guard++) {
        Cmd *c = &p->cmd[p->pc];
        if (is_script)
            test_mb.line = c->line;
        switch (c->op) {
        case C_SPAWN:
            if (!teleport(pl, c->a[0], c->a[1], c->a[2], c->a[3]) && skipmiss) {
                /* "option skipmiss" (autotest): a point outside the build's collision -> on to the next spawn */
                while (p->pc + 1 < p->n && p->cmd[p->pc + 1].op != C_SPAWN && p->cmd[p->pc + 1].op != C_END)
                    p->pc++;
                printf("TEST skip line %d\n", c->line);
            }
            break;
        case C_CAM: p->cam_on = 1, p->cam_yaw = c->a[0], p->cam_pitch = c->a[1]; break;
        case C_CAMOFF: p->cam_on = 0; break;
        case C_PITCH: p->cam_on = 1, p->cam_pitch = c->a[0]; break;  /* yaw as last set (cam / approach) */
        case C_STICK: p->held.mx = c->a[0], p->held.my = c->a[1]; break;
        case C_RSTICK: p->held.cx = c->a[0], p->held.cy = c->a[1]; break;
        case C_CIRCLE: p->held.circle = c->a[0] != 0; break;
        case C_R1: in->r1 = 1; break;
        case C_FREEZE: frozen = c->a[0] != 0; break;
        case C_STATS: seg_reset(); break;
        case C_LOG: printf("TEST log %s\n", c->text); break;
        case C_GAME: save_test_cmd(c->text, pl); break;
        case C_EV: event_test_cmd(c->text); break;
        case C_LOOP: p->pc = -1; break;
        case C_END:
            p->pc = p->n;
            continue;
        case C_WAIT:
            if ((p->t += dt) < c->a[0] - 0.0001f)
                return 1;
            p->t = 0;
            break;
        case C_FRAMES:
            if (++p->frames < (int)c->a[0])
                return 1;
            p->frames = 0;
            break;
        case C_APPROACH: {
            float tgt[3] = {c->a[0], pl->pos.y, c->a[1]}, f[3] = {pl->pos.x, pl->pos.y, pl->pos.z};
            int have = c->text ? objects_nearest(f, tgt) >= 0 : 1;
            float dx = tgt[0] - f[0], dz = tgt[2] - f[2];
            if (have && dx * dx + dz * dz > c->a[2] * c->a[2] && (p->t += dt) < 10) {
                p->cam_on = 1;
                p->cam_yaw = atan2f(dx, dz);
                in->mx = 0, in->my = c->a[3];
                return 1;
            }
            p->t = 0;
            break;
        }
        case C_SHOT:
            if (!shot_wait) {
                strncpy((char *)test_mb.shot_name, c->text, sizeof test_mb.shot_name - 1);
                test_mb.shot_seq++;
                shot_wait = 1;
                p->t = 0;
                printf("TEST shot %s seq %u frames %u fps %.1f ms_avg %.2f ms_max %.2f map_max %.2f ram_min %d vram %d "
                       "chain_map %d chain_chr %d tris %d vu1 %d ee %d pos %.2f %.2f %.2f\n",
                       c->text, test_mb.shot_seq, test_mb.seg_frames, test_mb.seg_ms_avg > 0 ? 1000 / test_mb.seg_ms_avg : 0,
                       test_mb.seg_ms_avg, test_mb.seg_ms_max, test_mb.seg_map_max, test_mb.seg_ram_min,
                       test_mb.vram_free, test_mb.seg_chain_map_max, test_mb.seg_chain_chr_max, test_mb.tris,
                       test_mb.vu1_batches, test_mb.ee_tris, pl->pos.x, pl->pos.y, pl->pos.z);
            }
            /* hold until the host acks (PINE): 3 s without a host, 60 s once the host has written shot_seq | 0x80000000
             * (capturing: screenshot + GS dump); frame time, as COP0 Count wraps every 14.6 s */
            if (test_mb.shot_ack != test_mb.shot_seq &&
                (p->t += dt) < (test_mb.shot_ack == (test_mb.shot_seq | 0x80000000u) ? 60 : 3)) {
                test_mb.state = 3;
                return 1;
            }
            shot_wait = 0;
            p->t = 0;
            test_mb.state = 1;
            seg_reset();
            break;
        }
        p->pc++;
    }
    return p->pc < p->n;
}

int test_input(Input *in, Player *pl, float *cam_yaw, float *cam_pitch, float dt, int demo_on)
{
    Prog *p = script.n && !script.done ? &script : demo_on && !script.n ? &demo : 0;
    if (!p)
        return 0;
    Input mv = {0};
    if (!step(p, &mv, pl, dt, p == &script)) {
        p->done = 1;
        frozen = 0;
        test_mb.state = 2;
        printf("TEST DONE frames %u\n", test_mb.frame);
    }
    *in = p->held;
    in->r1 = mv.r1;
    if (mv.my)  /* approach drives the stick itself */
        in->mx = mv.mx, in->my = mv.my;
    if (p->cam_on)
        *cam_yaw = p->cam_yaw, *cam_pitch = p->cam_pitch;
    if (test_frozen())
        *in = (Input){0};
    return 1;
}

extern char _stack_size[];

void test_frame_end(const Player *pl, float avg_dt, float dt_real, float map_ms, float chr_ms, float vuw_ms, int tris,
                    int vram_free)
{
    volatile TestMailbox *m = &test_mb;
    m->frame++;
    m->pos[0] = pl->pos.x, m->pos[1] = pl->pos.y, m->pos[2] = pl->pos.z, m->yaw = pl->yaw;
    m->fps = 1 / avg_dt, m->frame_ms = avg_dt * 1000, m->map_ms = map_ms, m->chr_ms = chr_ms, m->vuw_ms = vuw_ms;
    if ((m->frame & 15) == 1) {  /* mallinfo walks the free list: every 16 frames */
        struct mallinfo mi = mallinfo();
        m->ram_free = 32 * 1024 * 1024 - (int)_stack_size - (int)sbrk(0) + mi.fordblks;
    }
    m->vram_free = vram_free;
    m->chain_map = vu1_stats.last[0], m->chain_map_peak = vu1_stats.peak[0];
    m->chain_chr = vu1_stats.last[1], m->chain_chr_peak = vu1_stats.peak[1], m->segments = vu1_stats.segments;
    m->tris = tris, m->vu1_batches = map_vu1_batches, m->ee_tris = map_ee_tris;
    m->enemies_active = enemy_active, m->enemies_drawn = enemy_drawn;
    m->hp = pl->hp, m->deaths = pl->deaths;
    if (shot_wait)
        return;  /* the frozen frames waiting for the host are not part of any segment */
    if (!m->seg_frames)
        seg_reset();
    float ms = dt_real * 1000;
    if (++m->seg_frames <= 2)  /* the first frames after a teleport / unfreeze carry its hitch */
        return;
    m->seg_ms_avg += (ms - m->seg_ms_avg) / (float)(m->seg_frames - 2);
    if (ms > m->seg_ms_max)
        m->seg_ms_max = ms;
    if (map_ms > m->seg_map_max)
        m->seg_map_max = map_ms;
    if (m->ram_free < m->seg_ram_min)
        m->seg_ram_min = m->ram_free;
    if (vu1_stats.last[0] > m->seg_chain_map_max)
        m->seg_chain_map_max = vu1_stats.last[0];
    if (vu1_stats.last[1] > m->seg_chain_chr_max)
        m->seg_chain_chr_max = vu1_stats.last[1];
}
