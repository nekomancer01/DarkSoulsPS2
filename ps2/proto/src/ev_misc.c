/* EMEVD odds and ends (ev_misc.h): temporary flag reset, messages, bonfires */
#include <math.h>
#include <stdio.h>
#include <string.h>

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
    printf("EVT %s %d\n", kind ? "banner" : "status message", (int)id);
}

const char *ev_message_hud(void)
{
    static char buf[24];
    if (msg_t <= 0)
        return "";
    snprintf(buf, sizeof buf, " %s%d", msg_kind ? "BNR" : "MSG", (int)msg_id);
    return buf;
}

void ev_misc_update(float dt)
{
    if (msg_t > 0)
        msg_t -= dt;
}

/* ---- bonfires ---- */
#define BONFIRE_MAX 16
typedef struct { s32 flag, entity; float pos[3], dist, angle; s16 kindle; u8 set, used; } Bonfire;
static Bonfire bonfires[BONFIRE_MAX];

void ev_bonfire_register(int set, s32 flag, s32 entity, const float pos[3], float dist, float angle, int kindle)
{
    Bonfire *b = NULL;
    for (int i = 0; i < BONFIRE_MAX && !b; i++)  /* the constructor registers again (Firelink: kindling 10, then 30) */
        if (bonfires[i].used && bonfires[i].entity == entity)
            b = &bonfires[i];
    for (int i = 0; i < BONFIRE_MAX && !b; i++)
        if (!bonfires[i].used)
            b = &bonfires[i];
    if (!b) {
        printf("EVT bonfire %d: list full\n", (int)entity);
        return;
    }
    b->used = 1, b->set = set, b->flag = flag, b->entity = entity, b->dist = dist, b->angle = angle;
    b->kindle = kindle;
    memcpy(b->pos, pos, 12);
    printf("EVT bonfire %d registered (flag %d %s, kindling %d)\n", (int)entity, (int)flag,
           flag_get(flag) ? "lit" : "unlit", kindle);
}

void ev_bonfire_drop(int set)
{
    for (int i = 0; i < BONFIRE_MAX; i++)
        if (bonfires[i].used && bonfires[i].set == set)
            bonfires[i].used = 0;
}

int ev_bonfire_count(void)
{
    int n = 0;
    for (int i = 0; i < BONFIRE_MAX; i++)
        n += bonfires[i].used;
    return n;
}

int ev_bonfire_action(const float pos[3], float yaw)
{
    for (int i = 0; i < BONFIRE_MAX; i++) {
        Bonfire *b = &bonfires[i];
        if (!b->used)
            continue;
        float dx = b->pos[0] - pos[0], dz = b->pos[2] - pos[2], r = b->dist + 0.8f;  /* ASSUMED as ObjActs (event-4) */
        if (dx * dx + dz * dz > r * r || fabsf(b->pos[1] - pos[1]) > 2)
            continue;
        if (b->angle > 0 && b->angle < 180 && dx * dx + dz * dz > 0.01f) {  /* facing the bonfire */
            float d = atan2f(dx, dz) - yaw;
            while (d > 3.14159265f) d -= 6.2831853f;
            while (d < -3.14159265f) d += 6.2831853f;
            if (fabsf(d) > b->angle * 3.14159265f / 180)
                continue;
        }
        if (!flag_get(b->flag)) {
            flag_set(b->flag, 1);  /* lit: the flag the map's events and the kindling read */
            printf("EVT bonfire %d lit\n", (int)b->entity);
            ev_message(1, 2);      /* ASSUMED banner type 2 = "BONFIRE LIT" (TECH_DEBT event-16) */
            return 1;
        }
        /* rest: respawn point, events restarted / ended on rest, temporary flags cleared, autosave. HP / Estus refill
         * and respawning enemies are A4's (save-6, enemy-4). */
        prog.bonfire = b->entity;
        progress_touch();
        event_rest();
        save_request();
        printf("EVT bonfire %d rest\n", (int)b->entity);
        return 1;
    }
    return 0;
}
