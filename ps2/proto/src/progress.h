/* Player progression state that survives a reboot (saved by save.c): souls, humanity, level and stats, equipment,
 * inventory, Estus, last bonfire, play time. Bosses killed, NPC states, item pickups, doors, shortcuts and kindling
 * live in the event flags (flags.h), as in DSR.
 * New game = CharaInitParam of the starting class (tools/export_progress.py -> progress_data.h). */
#ifndef PROGRESS_H
#define PROGRESS_H

#include <tamtypes.h>

/* Item id category in the top nibble, as DSR passes item ids (FUN_1402d93f0 switches on 0 / 0x10000000 / 0x20000000 /
 * 0x40000000): weapon (EquipParamWeapon), protector, accessory (ring), goods */
#define ITEM_WEAPON 0x00000000u
#define ITEM_PROTECTOR 0x10000000u
#define ITEM_ACCESSORY 0x20000000u
#define ITEM_GOODS 0x40000000u
#define ITEM_CAT(id) ((id) & 0xF0000000u)
#define ITEM_ROW(id) ((id) & 0x0FFFFFFFu)

/* Progress.equip[] (param row ids, -1 = empty): CharaInitParam order */
enum { EQ_WEP_L1, EQ_WEP_R1, EQ_WEP_L2, EQ_WEP_R2, EQ_ARROW, EQ_BOLT, EQ_ARROW2, EQ_BOLT2, EQ_HELM, EQ_ARMOR, EQ_GAUNT,
       EQ_LEG, EQ_RING1, EQ_RING2, EQ_QUICK1, EQ_QUICK2, EQ_QUICK3, EQ_QUICK4, EQ_QUICK5, EQ_SLOTS };
enum { ST_VIT, ST_ATT, ST_END, ST_STR, ST_DEX, ST_RES, ST_INT, ST_FAI, ST_COUNT };

#define INV_MAX 256  /* ASSUMED (TECH_DEBT save-4): DSR's inventory holds far more entries */

typedef struct {
    u32 id;      /* ITEM_* | param row */
    u16 num;     /* stack count */
    u16 dur;     /* durability (weapons / armour), 0 = not tracked yet */
} InvItem;

/* Saved as is (save.c, SAVE_VERSION): append fields at the end only, and bump the version for any other change */
typedef struct {
    s32 souls, humanity, level;
    u8 stats[ST_COUNT];
    s32 hp;                  /* HP at save time; <= 0 = full on load */
    s32 estus, estus_max;    /* Estus charges (A4 sets the max from the bonfire's kindle level) */
    u32 bonfire;             /* last bonfire rested at: its MSB entity id (respawn point), 0 = the map's spawn */
    u32 map_id;              /* area of the saved position: 0xAABBCCDD for mAA_BB_CC_DD */
    float pos[3], yaw;
    u32 play_time;           /* seconds */
    s32 deaths;
    s32 equip[EQ_SLOTS];
    s32 inv_count;
    InvItem inv[INV_MAX];
} Progress;

extern Progress prog;

void progress_new_game(void);           /* CharaInitParam start values, empty flags are flags.c's */
void progress_add_souls(int n);         /* clamps at 0 and 999999999 */
int inv_count(u32 id);                  /* how many of an item are held */
int inv_add(u32 id, int n);             /* n < 0 removes; returns the new count, -1 if the inventory is full */
u32 progress_map_id(const char *area);  /* "m10_02_00_00" -> 0x0A020000 */
unsigned progress_changes(void);        /* bumped by every change above (autosave dirty check) */
void progress_touch(void);              /* direct writes to prog: mark changed */

#endif
