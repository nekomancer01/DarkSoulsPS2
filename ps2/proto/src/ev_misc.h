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

/* 2009[03] Register Bonfire (lit flag, bonfire object entity, reaction distance m, angle deg, kindling) at the
 * object's position; set = the event set it came from (dropped with its block) */
void ev_bonfire_register(int set, s32 flag, s32 entity, const float pos[3], float dist, float angle, int kindle);
void ev_bonfire_drop(int set);
/* Action button at pos / yaw: a registered bonfire in reach is lit (its flag off) or rested at. 1 if one was used. */
int ev_bonfire_action(const float pos[3], float yaw);
int ev_bonfire_count(void);

#endif
