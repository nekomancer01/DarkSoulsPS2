/* EMEVD odds and ends for src/event.c: the temporary event flags DSR clears on a load / rest, save requests, status
 * messages and banners (EE log + the EVT HUD line until A11 has the FMG text), bonfire registration (2009[03]) with
 * the light / rest use of the action button (the rest itself: src/bonfire.c). */
#ifndef EV_MISC_H
#define EV_MISC_H

#include <tamtypes.h>

/* Temporary flags of every area, as DSR's flag manager clears them (FUN_1404f1fb0 on an area load, FUN_1404f20b0 when
 * the event manager restarts its events): 1AAB5000-1AAB6999 of all 18 areas and 5000-5999; load = 1 also 4000-4999 */
void ev_temp_flags_reset(int load);

/* 2007[03] Display Status Message (kind 0, FMG message id) / 2007[02] Display Banner (kind 1, banner type) */
void ev_message(int kind, s32 id);
/* " MSG <id>" / " BNR <type>" for the EVT HUD line while the last one is shown (3 s), else "" */
const char *ev_message_hud(void);
void ev_misc_update(float dt);

/* 2003[12] Handle Boss Defeat: the exe posts game message 0x36 / 4050 for the entity (FUN_1404ab4b0: the "VICTORY
 * ACHIEVED" text and the boss-kill bookkeeping, ASSUMED meaning); the port logs it, shows banner " DEF<entity>" and
 * counts it (tests). Flags, fog walls and the bar are the scripts' own instructions. */
void ev_boss_defeat(s32 entity);
int ev_boss_defeats(void);

/* 2009[03] Register Bonfire (flag, bonfire object entity, reaction distance m, angle deg, kindling) at the
 * object's position; set = the event set it came from (dropped with its block). A non-zero kindling raises the
 * bonfire's kindle points to 20 (10 Estus). Kindle points live in prog.kindle (saved), as DSR saves its bonfire list. */
void ev_bonfire_register(int set, s32 flag, s32 entity, const float pos[3], float dist, float angle, int kindle);
void ev_bonfire_drop(int set);
void ev_bonfire_move(int from, int to);  /* the events of a map moved to another block's set */
int ev_bonfire_count(void);
/* Kindle stage 0..4 (0 unlit) of a registered bonfire, -1 when it is not registered (no fire: 2009[03] creates it) */
int ev_bonfire_level(s32 entity);
int ev_bonfire_points(s32 entity);
void ev_bonfire_add_points(s32 entity, int n);  /* capped 0..40 */
int ev_bonfire_estus(s32 entity);                /* Estus charges a rest there gives (CalcCorrectGraph 150) */
int ev_bonfire_pos(s32 entity, float pos[3]);
int ev_bonfire_nearest(const float pos[3], float out[3]);  /* the registered bonfire nearest in x / z (tests: go bonfire) */    /* object position; 0 if not registered */
void ev_bonfire_light(s32 entity);               /* lighting: its flag on, 10 points */
/* The registered bonfire whose prompt shows for the player at pos / yaw (talk ESD reach), 0 none */
s32 ev_bonfire_near(const float pos[3], float yaw);
/* Action button at pos / yaw: lights or sits at the bonfire in reach (src/bonfire.c); 1 if the press was used */
int ev_bonfire_action(const float pos[3], float yaw);

#endif
