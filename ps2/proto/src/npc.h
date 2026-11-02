/* c0000 human NPCs (MSB enemy parts of model c0000: Crestfallen Warrior, Petrus, Solaire ...) as stand-ins until the
 * equipment-built look exists (TECH_DEBT enemy-14): drawn with the player's model in its idle pose, placed, enabled /
 * disabled, hit and killed like any character an event addresses, no AI. enemy.c hands them over: their enemies.bin
 * records (tools/export_enemies.py writes them after the other placements), block on / off, entity lookups. */
#ifndef NPC_H
#define NPC_H

#include "player.h"

#define NPC_MAX 48   /* stand-ins kept, every block (Firelink 14 + Burg 6 + Parish ...) */

/* A c0000 enemies.bin record (36 B) of world block `block` (0 = the boot area, placed at once) */
void npc_keep(int block, const unsigned char *rec);
void npc_block(int block, int on);
void npc_update(const Player *pl, float dt);
void npc_draw(void);
/* As the enemy_entity_* functions (enemy.h); -1 / 0 when no stand-in has that entity id */
int npc_entity(int entity, V3 *pos, int *dead, float *hp_ratio, int *active);
int npc_entity_enable(int entity, int on, int bit);
int npc_entity_kill(int entity);
int npc_entity_damage(int entity, int hp);
struct EvChr;
void npc_entity_bind(int entity, struct EvChr *c);
extern int npc_count, npc_drawn;

#endif
