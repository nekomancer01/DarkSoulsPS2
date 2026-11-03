/* EMEVD character ops (bank 2004 and the character conditions of bank 4) for src/event.c: AI on / off, AI commands
 * (ai:GetEventRequest), replanning, team type, immortality, invincibility, special idle anims, home, turning, display
 * mask, map collision, always-active, damaged-by. One record per entity an event touched, kept while the entity's
 * world block is unloaded and applied when its characters are placed again (TECH_DEBT event-12): enemy.c reads it at
 * place() and every frame through Enemy.ev, npc.c (c0000 stand-ins) the same way. Semantics from the EMEDF names
 * (common.emedf) and how DSR's scripts use them; ASSUMED where the exe was not checked (TECH_DEBT event-15). */
#ifndef EV_CHR_H
#define EV_CHR_H

#define EVC_MAX 128     /* entities with event state at once, all maps (Firelink ~40, Burg ~60 at a time) */
#define EVC_SLOTS 4     /* AI command slots (2004[17] slot 0..3) */

typedef struct EvChr {
    int entity;
    unsigned char off;       /* enemy.c ev_off bits: 1 disabled (2004[05]), 2 backread removed (2004[29]) */
    unsigned char dead;      /* killed by 2004[04] */
    signed char team;        /* EMEVD TeamType (2004[02] / [44]), -1 = NpcParam's */
    unsigned char ai_off;    /* 2004[01] logic off: no AI step (the character stands, the ESD still runs) */
    unsigned char immortal;  /* 2004[12]: HP never below 1 */
    unsigned char invincible;/* 2004[15]: no damage */
    unsigned char no_gravity, no_maphit, always_on;  /* 2004[10] / [31] / [35] */
    unsigned char has_home;
    float home[3];           /* 2004[13]: region the character returns to */
    int sp[5];               /* 2004[09] special wait: idle, damage, cancel, death, return anim (-1 none) */
    int sp_serial;           /* +1 per 2004[09] / [44]: the character starts or ends its wait when this moves */
    int ai_req[EVC_SLOTS];   /* 2004[17] AI command per slot, -1 none (ai:GetEventRequest) */
    int replan;              /* +1 per 2004[20] logic replanning / [16] target clear */
    int ai_logic;            /* 2004[19] logic id, 0 = NpcThinkParam's */
    unsigned disp_on, disp_off;  /* 2004[26] display-mask bits forced on / off */
    int speffect[4];         /* SpEffects an event put on it (2004[08] / [21]); 0 = free */
    int part_id, part_hp;    /* 2004[22] event part and its HP (part hits: A8) */
    float hit_t;             /* seconds since the player last damaged it (4[01]), large = never */
    float rot_to[3];         /* 2004[14] turn: face this point, once per rot_serial */
    int rot_serial, anim_reset;
} EvChr;

/* Record of an entity (create: make one if missing; NULL when the table is full or entity <= 0) */
EvChr *ev_chr_get(int entity, int create);
/* Event ops: 1 = handled, 0 = not for a character the port has (event.c logs it as a stub) */
int ev_chr_enable(int entity, int on, int bit);
int ev_chr_kill(int entity, int award);  /* award: the kill gives souls / humanity points */
int ev_chr_ai(int entity, int on);
int ev_chr_team(int entity, int team, int end_wait);
int ev_chr_flag(int op, int entity, int on);  /* EVC_IMMORTAL .. EVC_ALWAYS_ON */
int ev_chr_special_wait(int entity, const int anims[5]);
int ev_chr_home(int entity, const float pos[3]);
int ev_chr_rotate(int entity, const float target[3]);
int ev_chr_ai_cmd(int entity, int cmd, int slot);
int ev_chr_ai_logic(int entity, int logic);
int ev_chr_replan(int entity);
int ev_chr_dispmask(int entity, int bit, int mode);
int ev_chr_speffect(int entity, int id, int on);
int ev_chr_part(int entity, int part, int hp);
int ev_chr_anim_reset(int entity);
enum { EVC_IMMORTAL, EVC_INVINCIBLE, EVC_GRAVITY_OFF, EVC_MAPHIT_OFF, EVC_ALWAYS_ON };
/* Conditions: 4[01] damaged (attacker 10000 = the player), 4[05] SpEffect, 4[06] event part HP vs v (cmp: event.c
 * ComparisonType), 4[03] character type; -1 = no such character (the condition is false) */
int ev_chr_damaged(int entity, int attacker);
int ev_chr_has_speffect(int entity, int id);
int ev_chr_part_hp(int entity, int part, int *hp);
/* Hooks: a character took a hit from the player (enemy.c / npc.c), the player took one from `attacker` */
void ev_chr_hit(int entity);
void ev_chr_player_hit(int attacker);
/* Once a frame (event_update): ages the damaged-by windows */
void ev_chr_update(float dt);
/* Test scripts ("ev chr ..." / "ev expect chr ..."): see event_test_cmd */
int ev_chr_test(const char *cmd, int *pass, int *fail);

#endif
