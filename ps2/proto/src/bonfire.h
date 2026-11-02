/* A4: bonfire rest and the death loop. A rest (src/ev_misc.c, the action button at a lit bonfire) refills HP and Estus,
 * respawns the enemies and makes the bonfire's spawn point the respawn point. A death drops a bloodstain with the
 * souls and humanity held (the older stain is lost), puts the player at the respawn point and resets the world as a
 * rest does; walking over the stain gives them back. */
#ifndef BONFIRE_H
#define BONFIRE_H

#include "player.h"

/* Once after the save is loaded: the booted area ("m10_02_00_00"), for the bloodstain's area check */
void bonfire_init(const char *area);
/* Rest at the bonfire object `entity` (src/ev_misc.c): respawn point, HP, Estus, enemies (events: event_rest) */
void bonfire_rest(s32 entity);
/* player.c respawn() of a death (HP < 0): bloodstain at the last spot on the ground, souls / humanity to 0. Before
 * the player moves. */
void bonfire_death(const Player *p);
/* The respawn point (prog.respawn) if its entity is loaded: feet position and yaw. 0 = use the map's spawn. */
int bonfire_spawn(V3 *pos, float *yaw);
/* After the respawn: HP / Estus refill, enemies, events, as a rest */
void bonfire_revive(Player *p);
/* Once a frame after the player moved: last grounded spot, bloodstain pickup */
void bonfire_update(Player *p, float dt);
/* The bloodstain marker (r_box; DSR draws an SFX) */
void bonfire_draw(void);
/* Test scripts ("ev <cmd>"): hurt N (player damage), stain go OFFSET (teleport onto / OFFSET m east of the
 * bloodstain), expect estus|hp|stain|stainx|stainz|souls|humanity|lastbonfire V.
 * 1 if the command was one of these. */
int bonfire_test_cmd(const char *cmd, int *pass, int *fail);

#endif
