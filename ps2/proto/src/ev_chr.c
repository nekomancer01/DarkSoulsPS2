/* EMEVD character ops (ev_chr.h): one EvChr record per entity an event touched; enemy.c / npc.c read it */
#include <stdio.h>
#include <string.h>

#include "enemy.h"
#include "ev_chr.h"

#define PLAYER_ENTITY 10000
#define HIT_WINDOW 0.25f  /* s a hit stays visible to 4[01] (event_update may defer an instance by a frame; ASSUMED) */

static EvChr chr[EVC_MAX];
static int nchr, full_logged;
static float player_hit_t = 1e6f;
static int player_hit_by;

static void init_rec(EvChr *c, int entity)
{
    memset(c, 0, sizeof *c);
    c->entity = entity, c->team = -1, c->hit_t = 1e6f;
    for (int i = 0; i < 5; i++)
        c->sp[i] = -1;
    for (int i = 0; i < EVC_SLOTS; i++)
        c->ai_req[i] = -1;
}

EvChr *ev_chr_get(int entity, int create)
{
    if (entity <= 0)
        return NULL;
    for (int i = 0; i < nchr; i++)
        if (chr[i].entity == entity)
            return &chr[i];
    if (!create)
        return NULL;
    if (nchr == EVC_MAX) {
        if (!full_logged++)
            printf("EVT chr: table full (%d), entity %d keeps no state\n", EVC_MAX, entity);
        return NULL;
    }
    EvChr *c = &chr[nchr++];
    init_rec(c, entity);
    enemy_entity_bind(entity, c);  /* placed characters of that id read it from now on */
    return c;
}

/* Record of a character op: the player (10000) has none (its state is player.c's) */
static EvChr *rec(int entity)
{
    return entity == PLAYER_ENTITY ? NULL : ev_chr_get(entity, 1);
}

int ev_chr_enable(int entity, int on, int bit)
{
    EvChr *c = rec(entity);
    if (c)
        c->off = on ? c->off & ~bit : c->off | bit;
    return enemy_entity_enable(entity, on, bit) > 0 || c;
}

int ev_chr_kill(int entity, int award)
{
    EvChr *c = rec(entity);
    if (c)
        c->dead = 1;
    return enemy_entity_kill(entity, award) || c;
}

int ev_chr_ai(int entity, int on)
{
    EvChr *c = rec(entity);
    return c ? (c->ai_off = !on, 1) : 0;
}

int ev_chr_team(int entity, int team, int end_wait)
{
    EvChr *c = rec(entity);
    if (!c)
        return 0;
    c->team = (signed char)team;
    if (end_wait && c->sp[0] >= 0) {  /* 2004[44]: and leave the special wait (its cancel anim, if any) */
        c->sp[0] = -1;
        c->sp_serial++;
    }
    return 1;
}

int ev_chr_flag(int op, int entity, int on)
{
    EvChr *c = rec(entity);
    if (!c)
        return 0;
    switch (op) {
    case EVC_IMMORTAL: c->immortal = on; break;
    case EVC_INVINCIBLE: c->invincible = on; break;
    case EVC_GRAVITY_OFF: c->no_gravity = on; break;  /* enemies do not fall in the port yet: kept, no effect */
    case EVC_MAPHIT_OFF: c->no_maphit = on; break;
    case EVC_ALWAYS_ON: c->always_on = on; break;
    }
    return 1;
}

int ev_chr_special_wait(int entity, const int anims[5])
{
    EvChr *c = rec(entity);
    if (!c)
        return 0;
    memcpy(c->sp, anims, sizeof c->sp);
    c->sp_serial++;
    return 1;
}

int ev_chr_home(int entity, const float pos[3])
{
    EvChr *c = rec(entity);
    if (!c)
        return 0;
    memcpy(c->home, pos, sizeof c->home);
    c->has_home = 1;
    return 1;
}

int ev_chr_rotate(int entity, const float target[3])
{
    EvChr *c = rec(entity);
    if (!c)
        return 0;
    memcpy(c->rot_to, target, sizeof c->rot_to);
    c->rot_serial++;
    return 1;
}

int ev_chr_ai_cmd(int entity, int cmd, int slot)
{
    EvChr *c = rec(entity);
    if (!c || slot < 0 || slot >= EVC_SLOTS)
        return 0;
    c->ai_req[slot] = cmd;
    return 1;
}

int ev_chr_ai_logic(int entity, int logic)
{
    EvChr *c = rec(entity);
    if (!c)
        return 0;
    c->ai_logic = logic;
    c->replan++;
    return 1;
}

int ev_chr_replan(int entity)
{
    EvChr *c = rec(entity);
    return c ? (c->replan++, 1) : 0;
}

int ev_chr_dispmask(int entity, int bit, int mode)
{
    EvChr *c = rec(entity);
    if (!c || bit < 0 || bit > 31)
        return 0;
    unsigned m = 1u << bit;
    int on = mode == 2 ? !(c->disp_on & m) : mode;  /* CHANGE: from what an event last set (ASSUMED) */
    c->disp_on = on ? c->disp_on | m : c->disp_on & ~m;
    c->disp_off = on ? c->disp_off & ~m : c->disp_off | m;
    return 1;
}

/* The player's SpEffects (items, rings, curses) are A6's: only what events put on it is known here */
int ev_chr_speffect(int entity, int id, int on)
{
    EvChr *c = ev_chr_get(entity, 1);
    if (!c)
        return 0;
    int k, free_k = -1;
    for (k = 0; k < 4 && c->speffect[k] != id; k++)
        if (!c->speffect[k] && free_k < 0)
            free_k = k;
    if (on && k == 4 && free_k >= 0)
        c->speffect[free_k] = id;
    else if (!on && k < 4)
        c->speffect[k] = 0;
    return 1;
}

int ev_chr_part(int entity, int part, int hp)
{
    EvChr *c = rec(entity);
    if (!c)
        return 0;
    c->part_id = part, c->part_hp = hp;
    return 1;
}

int ev_chr_anim_reset(int entity)
{
    EvChr *c = rec(entity);
    return c ? (c->anim_reset++, 1) : 0;
}

int ev_chr_damaged(int entity, int attacker)
{
    if (entity == PLAYER_ENTITY)
        return player_hit_t < HIT_WINDOW && (attacker <= 0 || attacker == player_hit_by);
    EvChr *c = ev_chr_get(entity, 1);  /* from now on its hits are recorded (ev_chr_hit keeps only known ids) */
    if (!c || enemy_entity(entity, NULL, NULL, NULL, NULL) < 0)
        return -1;
    return c->hit_t < HIT_WINDOW && (attacker <= 0 || attacker == PLAYER_ENTITY);  /* only the player hits them */
}

int ev_chr_has_speffect(int entity, int id)
{
    EvChr *c = ev_chr_get(entity, 0);
    for (int k = 0; c && k < 4; k++)
        if (c->speffect[k] == id)
            return 1;
    return 0;
}

int ev_chr_part_hp(int entity, int part, int *hp)
{
    EvChr *c = ev_chr_get(entity, 0);
    if (!c || c->part_id != part)
        return -1;
    *hp = c->part_hp;
    return 1;
}

void ev_chr_hit(int entity)
{
    EvChr *c = ev_chr_get(entity, 0);
    if (c)
        c->hit_t = 0;
}

void ev_chr_player_hit(int attacker)
{
    player_hit_t = 0, player_hit_by = attacker;
}

void ev_chr_update(float dt)
{
    player_hit_t += dt;
    for (int i = 0; i < nchr; i++)
        chr[i].hit_t += dt;
}

/* "chr ID enable|ai|immortal|invincible|cmd SLOT V|team V|hit" and "expect chr ID off|dead|team|ai|aireq SLOT|
 * immortal|sp|placed|active|dead|hp V" */
int ev_chr_test(const char *cmd, int *pass, int *fail)
{
    char f[16] = "";
    int id = 0, v = 0, slot = 0;
    if (sscanf(cmd, "chr %d cmd %d %d", &id, &slot, &v) == 3) {
        ev_chr_ai_cmd(id, v, slot);
    } else if (sscanf(cmd, "chr %d damage %d", &id, &v) == 2) {  /* a player hit of v HP (combat skipped) */
        printf("TEST ev chr %d damage %d: %d\n", id, v, enemy_entity_damage(id, v));
        return 1;
    } else if (sscanf(cmd, "chr %d %15s %d", &id, f, &v) == 3) {
        if (!strcmp(f, "enable"))
            ev_chr_enable(id, v, 1);
        else if (!strcmp(f, "ai"))
            ev_chr_ai(id, v);
        else if (!strcmp(f, "team"))
            ev_chr_team(id, v, 0);
        else if (!strcmp(f, "immortal"))
            ev_chr_flag(EVC_IMMORTAL, id, v);
        else
            return 0;
    } else if (sscanf(cmd, "expect chr %d aireq %d %d", &id, &slot, &v) == 3 ||
               sscanf(cmd, "expect chr %d %15s %d", &id, f, &v) == 3) {
        EvChr *c = ev_chr_get(id, 0);
        V3 p;
        int dead = 0, act = 0, got = -999, idx = enemy_entity(id, &p, &dead, NULL, &act);
        float hp = 0;
        enemy_entity(id, NULL, NULL, &hp, NULL);
        if (!f[0])
            got = c ? c->ai_req[slot & 3] : -1, strcpy(f, "aireq");
        else if (!strcmp(f, "off"))
            got = c ? c->off : 0;
        else if (!strcmp(f, "team"))
            got = c ? c->team : -1;
        else if (!strcmp(f, "ai"))
            got = c ? !c->ai_off : 1;
        else if (!strcmp(f, "immortal"))
            got = c && c->immortal;
        else if (!strcmp(f, "sp"))
            got = c ? c->sp[0] : -1;
        else if (!strcmp(f, "placed"))  /* in the roster (enemy.c) or a c0000 stand-in (npc.c) */
            got = idx >= 0;
        else if (!strcmp(f, "active"))  /* updated this frame: enabled, loaded and near (enemy.c) */
            got = idx >= 0 ? act : -999;
        else if (!strcmp(f, "dead"))
            got = idx >= 0 ? dead : -999;
        else if (!strcmp(f, "hp"))  /* HP ratio in percent */
            got = idx >= 0 ? (int)(hp * 100 + 0.5f) : -999;
        else
            return 0;
        int ok = got == v;
        *pass += ok, *fail += !ok;
        printf("TEST ev expect chr %d %s %d: %d %s\n", id, f, v, got, ok ? "PASS" : "FAIL");
        return 1;
    } else
        return 0;
    printf("TEST ev %s\n", cmd);
    return 1;
}
