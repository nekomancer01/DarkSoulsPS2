/* DSR's item database (tools/export_items.py, menu.pak): the inventory's stack sizes and kinds at all times
 * (items.bin, ~3 KB), names / stats / icons / descriptions while a menu is open (itemx.bin, desc.bin, icons.bin).
 * Item ids as progress.h: category nibble | param row; a weapon's last two digits are its reinforcement level. */
#ifndef ITEMS_H
#define ITEMS_H

#include <tamtypes.h>
#include "progress.h"

/* Inventory tabs, the order of DSR's eight (menu.drb I01_tab_select) */
enum { TAB_CONSUME, TAB_MATERIAL, TAB_KEY, TAB_SPELL, TAB_WEAPON, TAB_AMMO, TAB_ARMOR, TAB_RING, TAB_COUNT };
/* EquipParamGoods.goodsType */
enum { GOODS_NORMAL = 0, GOODS_KEY = 1, GOODS_MATERIAL = 2, GOODS_HUMANITY = 4, GOODS_SPELL = 5 };
/* items.bin goods flags */
#define IF_CONSUME 1  /* isConsume: using it takes one */
#define IF_DROP 2     /* isDrop: can be dropped / discarded */
#define IF_DEPOSIT 4  /* isDeposit: fits the Bottomless Box */
#define IF_ONLY_ONE 8 /* isOnlyOne */
#define IF_EQUIP 16   /* isEquip: goes in a quick item slot */
/* itemx.bin weapon slot bits (row x1) */
#define WS_RIGHT 1
#define WS_LEFT 2
#define WS_BOTH 4
#define WS_ARROW 8
#define WS_BOLT 16

#define ITEM_LEVEL(id) (ITEM_CAT(id) == ITEM_WEAPON ? (int)((id) % 100) : 0)
#define ITEM_BASE(id) (ITEM_CAT(id) == ITEM_WEAPON ? (id) - (id) % 100 : (id))

/* itemx.bin row (72 B, export_items.py) */
typedef struct {
    u32 id, sort, name, text;
    u16 help_len, desc_len, icon;
    u8 tab, sub;
    u16 weight, dur, x0, x1, x2, pad;
    s16 st[16];
    u32 pad2;
} ItemRow;
/* ReinforceParamWeapon row: atk rates phys / mag / fire / thunder, correct rates str / dex / int / fai, all x1000 */
typedef struct {
    u32 id;
    u16 atk[4], cor[4];
} ItemReinforce;

int items_init(void);              /* boot: items.bin from menu.pak; 0 when missing (every item then stacks to 99) */
int item_max(u32 id);              /* stack size: goods maxNum, ammunition 99, else 1 (each weapon / armour / ring
                                    * is its own entry, as in DSR's inventory) */
int item_goods_type(u32 id);       /* goodsType of a goods id, -1 other */
int item_goods_flags(u32 id);      /* IF_* of a goods id, 0 other */
int item_tab(u32 id);              /* TAB_* from the id alone (goodsType / ammo row / category) */

/* While a menu is open: names, stats, icons. alloc gives the buffer (src/drb.c ui_alloc: the near-cell arena).
 * 1 once loaded (call each frame until then: the read runs in the background), -1 failed. */
int items_detail_step(void *(*alloc)(unsigned), void (*release)(void *, unsigned));
void items_detail_free(void);
const ItemRow *item_row(u32 id);   /* NULL while not loaded or unknown */
const ItemReinforce *item_reinforce(int type, int level);
/* "Dagger+3"; "" unknown */
const char *item_name(u32 id, char *buf, int n);
/* Help line and long description into buf (64-byte aligned): help, NUL, description, NUL, read from desc.bin in the
 * background (~0.5 KB). request: 1 started, 0 the read slot is busy (try next frame), -1 no text / too long. done: 1
 * finished (*desc = the description's offset in buf, the help line starts at 0), 0 pending, -1 failed. */
int item_text_request(u32 id, char *buf, int n);
int item_text_done(int *desc);
/* Icons (icons.bin, stored raw): index of icon id, -1 none; read cell k (CLUT 1 KB + pixels) into dst (64-byte
 * aligned, item_icon_bytes()) in the background: 1 started, 0 busy (try next frame) */
int item_icon_index(int icon);
int item_icon_bytes(void);
int item_icon_size(void);
int item_icon_read(int k, void *dst);
int item_icon_done(void);          /* 1 when the read finished (dst valid), 0 pending, -1 failed */

#endif
