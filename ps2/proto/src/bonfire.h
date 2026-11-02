/* A4: bonfire use and the death loop. The action button at a bonfire (src/ev_misc.c, talk ESD reach) lights it (a00
 * 7698, BONFIRE LIT) or sits down (7700 / 7710 / 7720): HP and Estus to the bonfire's kindle count, enemies respawned,
 * respawn point = its spawn point; the rest menu (Kindle, Reverse Hollowing, Leave; others listed, not built) until
 * the player stands up. Details at the top of src/bonfire.c. A death drops a bloodstain with the
 * souls and humanity held (the older stain is lost), puts the player at the respawn point and resets the world as a
 * rest does; walking over the stain gives them back. */
#ifndef BONFIRE_H
#define BONFIRE_H

#include "player.h"

/* Once after the save is loaded: the booted area ("m10_02_00_00"), for the bloodstain's area check */
void bonfire_init(const char *area);
/* Rest at the bonfire object `entity` at once (no anims): respawn point, HP, Estus, enemies (events: event_rest) */
void bonfire_rest(s32 entity);
/* Action button at bonfire `entity` (ev_misc.c): light (unlit) or sit; 1 if the player could start it */
int bonfire_use(s32 entity);
int bonfire_busy(void);  /* lighting / sitting / in the menu: movement and the action button are the bonfire's */
/* main.c: pad buttons pressed this frame (libpad PAD_*) for the rest menu / dialogs; 1 while the bonfire has the
 * player (no movement input) */
int bonfire_input(unsigned short pressed);
/* hud.c, inside hud_begin / hud_end: prompt, rest menu, dialogs, BONFIRE LIT */
void bonfire_hud(float dt);
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
 * bloodstain), humanity N, hollow 0|1, bf pad up|down|left|right|cross|circle (a rest-menu button), bf pick INDEX
 * (menu cursor onto the talk ESD's menu index: 8 Kindle, 11 Reverse Hollowing, 9 Leave), expect estus|estusmax|hp|stain|stainx|stainz|souls|humanity|lastbonfire|
 * hollow|bfstate|menu V, expect points|level ENTITY V.
 * 1 if the command was one of these. */
int bonfire_test_cmd(const char *cmd, int *pass, int *fail);

#endif
