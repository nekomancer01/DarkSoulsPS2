/* EMEVD odds and ends (ev_misc.h): temporary flag reset, messages, bonfires */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bonfire.h"
#include "combat_data.h"
#include "ev_misc.h"
#include "event.h"
#include "flags.h"
#include "progress.h"
#include "save.h"

/* The 18 area codes of DSR's flag groups 1 / 5 / 6 / 7 (flags.c AREA_CODES, exe table DAT_141369fc0: 18 records each) */
static const u16 AREAS[18] = {0, 100, 101, 102, 110, 120, 121, 130, 131, 132, 140, 141, 150, 151, 160, 170, 180, 181};

/* FUN_1404f20b0 / FUN_1404f1fb0: memset of the 1000-flag chunks 5 (and 4) of group 0's one record and chunks 5 + 6
 * of every group-1 record */
void ev_temp_flags_reset(int load)
{
    flag_range_set(load ? 4000 : 5000, 5999, 0);
    for (int i = 0; i < 18; i++) {
        u32 base = 10000000u + AREAS[i] * 10000u;
        flag_range_set(base + 5000, base + 6999, 0);
    }
}

/* ---- messages ---- */
static s32 msg_id;
static int msg_kind;
static float msg_t;

void ev_message(int kind, s32 id)
{
    msg_kind = kind, msg_id = id, msg_t = 3.0f;  /* ASSUMED on-screen time; the text itself is A11's (FMG) */
    printf("EVT %s %d\n", kind == 2 ? "boss defeated" : kind ? "banner" : "status message", (int)id);
}

static int defeats;

void ev_boss_defeat(s32 entity)
{
    defeats++;
    ev_message(2, entity);
}

int ev_boss_defeats(void) { return defeats; }

const char *ev_message_hud(void)
{
    static char buf[24];
    if (msg_t <= 0)
        return "";
    snprintf(buf, sizeof buf, " %s%d", msg_kind == 2 ? "DEF" : msg_kind ? "BNR" : "MSG", (int)msg_id);
    return buf;
}

void ev_misc_update(float dt)
{
    if (msg_t > 0)
        msg_t -= dt;
}

/* ---- bonfires ---- */
#define BONFIRE_MAX 16
typedef struct { s32 flag, entity; float pos[3]; u8 set, used; } Bonfire;
static Bonfire bonfires[BONFIRE_MAX];

/* The talk ESD of every bonfire (t1xx1xx / t1xx2xx, all the same, on the c1000 dummy 0.5 m above the object): the
 * prompt shows within GetDistanceToPlayer 2.0 m (unlit, "Light bonfire") / 3.4 m (lit, "Rest at bonfire"), player
 * facing within 45 deg of it (GetRelativeAngleBetweenPlayerAndSelf), GetPlayerYDistance <= 1 m */
#define TALK_DUMMY_Y 0.5f
#define REACH_UNLIT 2.0f
#define REACH_LIT 3.4f
#define REACH_ANGLE 45.0f
#define REACH_DY 1.0f
/* Kindle points: the exe's caps (DAT_141a3372c / 30 / 34 = 40, FUN_1407a3380); lighting and each kindling add 10
 * (FUN_1407a4380); 2009[03] with a non-zero kindling raises the bonfire to 20 (FUN_1404ccec0 -> FUN_1407a3b70) */
#define KINDLE_CAP 40
#define KINDLE_STEP 10
#define KINDLE_STANDARD 20

static int kindle_slot(s32 entity, int create)
{
    for (int i = 0; i < KINDLE_SAVE_MAX; i++)
        if (prog.kindle[i].entity == (u32)entity)
            return i;
    for (int i = 0; create && i < KINDLE_SAVE_MAX; i++)
        if (!prog.kindle[i].entity) {
            prog.kindle[i].entity = entity, prog.kindle[i].points = 0;
            return i;
        }
    return -1;
}

int ev_bonfire_points(s32 entity)
{
    int k = kindle_slot(entity, 0);
    return k < 0 ? 0 : prog.kindle[k].points;
}

void ev_bonfire_add_points(s32 entity, int n)
{
    int k = kindle_slot(entity, 1);
    if (k < 0)
        return;
    int v = prog.kindle[k].points + n;
    prog.kindle[k].points = v < 0 ? 0 : v > KINDLE_CAP ? KINDLE_CAP : v;
    progress_touch();
}

/* CalcCorrectGraph 150 (回復スポットの最大補充数): stageMaxVal 0 / 10 / 20 / 30 / 40 -> stage 0..4, Estus
 * stageMaxGrowVal 0 / 5 / 10 / 15 / 20, read as steps (FUN_1407a40b0 / FUN_1407a41b0: points clamped to the stage
 * range, the first stage whose value is >= the points) */
static int stage_of(int points)
{
    const float *g = graph_150;
    float v = points < g[0] ? g[0] : points > g[4] ? g[4] : points;
    for (int i = 0; i < 5; i++)
        if (v <= g[i])
            return i;
    return 4;
}

static Bonfire *find(s32 entity)
{
    for (int i = 0; i < BONFIRE_MAX; i++)
        if (bonfires[i].used && bonfires[i].entity == entity)
            return &bonfires[i];
    return NULL;
}

int ev_bonfire_level(s32 entity)
{
    return find(entity) ? stage_of(ev_bonfire_points(entity)) : -1;
}

int ev_bonfire_estus(s32 entity)
{
    return (int)graph_150[5 + stage_of(ev_bonfire_points(entity))];
}

int ev_bonfire_nearest(const float pos[3], float out[3])
{
    float best = 1e30f;
    for (int i = 0; i < BONFIRE_MAX; i++) {
        Bonfire *b = &bonfires[i];
        float dx = b->pos[0] - pos[0], dz = b->pos[2] - pos[2];
        if (b->used && dx * dx + dz * dz < best)
            best = dx * dx + dz * dz, memcpy(out, b->pos, 12);
    }
    return best < 1e30f;
}

int ev_bonfire_pos(s32 entity, float pos[3])
{
    Bonfire *b = find(entity);
    if (b)
        memcpy(pos, b->pos, 12);
    return b != NULL;
}

void ev_bonfire_register(int set, s32 flag, s32 entity, const float pos[3], float dist, float angle, int kindle)
{
    Bonfire *b = find(entity);  /* the constructor registers again (Firelink: kindling 10, then 30) */
    for (int i = 0; i < BONFIRE_MAX && !b; i++)
        if (!bonfires[i].used)
            b = &bonfires[i];
    if (!b) {
        printf("EVT bonfire %d: list full\n", (int)entity);
        return;
    }
    /* reaction distance / angle: the exe keeps them (FUN_1404ccec0: +0x50, cos of the angle at +0x54) but the prompt
     * and the menu follow the talk ESD's own distances (REACH_*) */
    (void)dist, (void)angle;
    b->used = 1, b->set = set, b->flag = flag, b->entity = entity;
    memcpy(b->pos, pos, 12);
    if (kindle && ev_bonfire_points(entity) < KINDLE_STANDARD)
        ev_bonfire_add_points(entity, KINDLE_STANDARD - ev_bonfire_points(entity));
    if (flag_get(flag) && !ev_bonfire_points(entity))  /* lit in a save from before the kindle points were kept */
        ev_bonfire_add_points(entity, KINDLE_STEP);
    printf("EVT bonfire %d registered (flag %d %s, kindling %d, points %d)\n", (int)entity, (int)flag,
           flag_get(flag) ? "on" : "off", kindle, ev_bonfire_points(entity));
}

void ev_bonfire_light(s32 entity)
{
    Bonfire *b = find(entity);
    if (b)
        flag_set(b->flag, 1);  /* the bonfire's flag: its warp / map state in the events */
    if (!ev_bonfire_points(entity))
        ev_bonfire_add_points(entity, KINDLE_STEP);  /* Util_RequestLevelUpFirst -> FUN_1407a4380(entity, 0) */
    printf("EVT bonfire %d lit (points %d)\n", (int)entity, ev_bonfire_points(entity));
}

void ev_bonfire_drop(int set)
{
    for (int i = 0; i < BONFIRE_MAX; i++)
        if (bonfires[i].used && bonfires[i].set == set)
            bonfires[i].used = 0;
}

void ev_bonfire_move(int from, int to)
{
    for (int i = 0; i < BONFIRE_MAX; i++)
        if (bonfires[i].used && bonfires[i].set == from)
            bonfires[i].set = to;
}

int ev_bonfire_count(void)
{
    int n = 0;
    for (int i = 0; i < BONFIRE_MAX; i++)
        n += bonfires[i].used;
    return n;
}

s32 ev_bonfire_near(const float pos[3], float yaw)
{
    for (int i = 0; i < BONFIRE_MAX; i++) {
        Bonfire *b = &bonfires[i];
        if (!b->used)
            continue;
        float r = stage_of(ev_bonfire_points(b->entity)) ? REACH_LIT : REACH_UNLIT;
        float dx = b->pos[0] - pos[0], dy = b->pos[1] + TALK_DUMMY_Y - pos[1], dz = b->pos[2] - pos[2];
        if (dx * dx + dy * dy + dz * dz > r * r || fabsf(dy) > REACH_DY)
            continue;
        if (dx * dx + dz * dz > 0.01f) {
            float d = atan2f(dx, dz) - yaw;
            while (d > 3.14159265f) d -= 6.2831853f;
            while (d < -3.14159265f) d += 6.2831853f;
            if (fabsf(d) > REACH_ANGLE * 3.14159265f / 180)
                continue;
        }
        return b->entity;
    }
    return 0;
}

int ev_bonfire_action(const float pos[3], float yaw)
{
    if (bonfire_busy())
        return 1;  /* sitting / lighting: the button is the menu's (src/bonfire.c) */
    s32 e = ev_bonfire_near(pos, yaw);
    return e && bonfire_use(e);
}
