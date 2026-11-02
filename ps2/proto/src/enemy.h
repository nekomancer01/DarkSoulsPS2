/* Enemies: MSB enemy parts of the area (enemies.bin) with their character packs (data/c####.pak: model.bin +
 * chr.bin from tools/export_chr.py), the shared enemy ESD (enemyCommon.esd, data/common.pak), DSR's Lua AI on the
 * lua0 VM (ai.c), locomotion + root motion over the map collision, and attack hit tests against the player. */
#ifndef ENEMY_H
#define ENEMY_H

#include <gsKit.h>
#include "player.h"

#define MAX_ENEMIES 64

/* Spawn every placement whose character pack exists (enemies.bin from the area pack, kept by the caller); loads the
 * character packs, enemyCommon's ESD and the AI scripts (aiCommon from common.pak + the area's map_lua, both lua0
 * packs; map_lua may be freed afterwards), uploads textures. Returns the number spawned. */
int enemy_init(GSGLOBAL *gs, const void *placements, const void *map_lua, int map_lua_size);
/* World blocks (src/world.c): at boot, block b's enemies.bin + map Lua (character packs from PACK_ROOT dir/) (types loaded and stripped, placements kept,
 * Lua run into the shared VM); enemy_block_add places them when the block turns on (before event_block_on), _remove
 * takes them out when it turns off (slots reused; an index is valid while its block is on) */
#define ENEMY_BLOCKS 4
int enemy_block_types(GSGLOBAL *gs, int block, const char *dir, const void *placements, const void *map_lua,
                      int map_lua_size);
void enemy_block_add(int block);
void enemy_block_remove(int block);
/* Bonfire rest / death: every placed enemy back at its placement, alive, AI from the start (event state kept) */
void enemy_respawn(void);
/* Perception, AI, ESD, animation, movement and hits for every enemy near the player */
void enemy_update(Player *pl, float dt);
/* Skin + draw the visible enemies into the current VU1 chain (after vu1_begin, like player_draw) */
void enemy_draw(void);
/* HUD line: nearest enemy's state, HP and what the hit tests did */
void enemy_status(char *buf, int n);

/* Position, capsule height and facing of placement `index` (test camera, ENEMY_VIEW); 0 if no such placement */
int enemy_view(int index, V3 *pos, float *height, float *yaw);
/* EE RAM free (sbrk headroom + malloc free list), for the HUD / load messages */
unsigned enemy_ram_free(void);
/* EMEVD (src/event.c), by MSB entity id: placement index (-1 = not in the roster) with position, dead, HP ratio and
 * whether it is updated (enabled and near); enable / disable (not updated, drawn or hit) per cause bit (1 enable
 * state, 2 backread removed: off while either is set), kill (HP 0, death anim) */
int enemy_entity(int entity, V3 *pos, int *dead, float *hp_ratio, int *active);  /* c0000 stand-ins (npc.c) too */
int enemy_entity_enable(int entity, int on, int bit);
int enemy_entity_kill(int entity);
/* A hit of hp from the player (test scripts: "ev chr ID damage HP"); 1 if it has such a character */
int enemy_entity_damage(int entity, int hp);
/* ev_chr.c: its record for that entity was created (placed characters read it from then on) */
struct EvChr;
void enemy_entity_bind(int entity, struct EvChr *c);
/* Totals for the HUD: enemies updated / drawn this frame */
extern int enemy_active, enemy_drawn;

#endif
