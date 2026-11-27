/* World items: the treasure shine on MSB corpse treasures and enemy drops (exe FUN_1403f89b0: SFX 99000 +
 * ItemLotParam.lotItem_Rarity on the object / corpse, all of which draw 99001), the action-button pickup (the player's
 * pickup anim, then DSR's item get dialog: menu.drb F25, FrpgMenuDlgItemGet FUN_140727f00), and enemy drops (exe
 * FUN_1403fc600: NpcParam.itemLotId_1 rolled on the death, one record per item at the corpse; MapItemMan keeps 30,
 * FUN_1403f9050 drops the oldest past that). Treasures themselves live in src/event.c (their state, get flags, the
 * lot draw); this file owns the shines' placement, the drops and the dialog. */
#ifndef PICKUP_H
#define PICKUP_H

#include <tamtypes.h>
#include <gsKit.h>
#include "player.h"


/* One item of a lot draw (src/event.c event_lot_roll): the item id (progress.h category | row), count, the get flag
 * its pickup sets (<= 0 none) */
typedef struct { u32 item; s32 flag; u16 num; u8 rarity, pad; } PickItem;
#define PICK_ITEMS 8  /* items per drop: one per row of a lot run (DSR's corpse lots have up to 4 rows) */

/* Boot: the shine set from menu.pak (shine.bin, freed here once its textures are up) */
void pickup_init(GSGLOBAL *gs, void *shine_blob, unsigned size);
/* Shine handles (src/fx.c fx_shine_*): -1 when the shine is off or every instance is in use */
int pickup_shine_add(const float pos[3]);
void pickup_shine_del(int h);
/* An item given by a pickup or an event (AwardItemLot): shown in the item get dialog */
void pickup_got(u32 item, int num);
/* The player picks something up (a treasure, a drop): the pickup anim */
void pickup_anim(Player *p);
/* Enemy death (src/enemy.c, once per death): NpcParam npc's drop lot rolled, a drop at pos when it gave something */
void pickup_enemy_drop(s32 npc, const float pos[3]);
/* Action button (src/event.c use_action, after treasures): the nearest drop in reach taken; 1 if one was */
int pickup_action(Player *p);
/* Once a frame: the dialog's time */
void pickup_update(float dt);
/* After the HUD: the item get dialog while it shows */
void pickup_draw(void);
/* The player the Pick up item prompt is checked for (main.c, once) */
void pickup_prompt_player(Player *p);
/* Tests ("pickup <cmd>"): drop NPC X Y Z (an enemy drop at a spot), droplot LOT X Y Z (a drop of any exported lot),
 * expect drops N, expect shines N, expect dialog N (rows shown),
 * summary */
void pickup_test_cmd(const char *cmd);

typedef struct { int drops, shines, dialog_rows, taken, rolled; } PickupStats;
extern PickupStats pickup_stats;

#endif
