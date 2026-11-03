/* Player progression state (progress.h) */
#include <stdlib.h>
#include <string.h>
#include "progress.h"
#include "progress_data.h"
#include "items.h"

#define START_ESTUS 5  /* ASSUMED (TECH_DEBT save-5): Estus charges before kindling; DSR hands the flask out in the Asylum */

Progress prog;
static unsigned changes;

void progress_new_game(void)
{
    memset(&prog, 0, sizeof prog);
    prog.souls = START_SOULS;
    prog.humanity = START_HUMANITY;
    prog.level = START_LEVEL;
    memcpy(prog.stats, start_stats, sizeof prog.stats);
    prog.estus = prog.estus_max = START_ESTUS;
    prog.chr_type = CHR_HOLLOW;  /* ASSUMED (TECH_DEBT a4-6): a new DS1 character is Hollow until it reverses it */
    for (int i = 0; i < EQ_SLOTS; i++)
        prog.equip[i] = -1;
    for (int i = 0; i < (int)(sizeof start_equip / sizeof start_equip[0]) && i < EQ_SLOTS; i++)
        prog.equip[i] = start_equip[i];
    for (int i = 0; i < EQ_QUICK1; i++)  /* equipped items are in the inventory too */
        if (prog.equip[i] >= 0)
            inv_add((i < EQ_HELM ? ITEM_WEAPON : i < EQ_RING1 ? ITEM_PROTECTOR : ITEM_ACCESSORY) | prog.equip[i], 1);
    inv_add(ITEM_GOODS | ESTUS_ID, 1);
    prog.equip[EQ_QUICK1] = ESTUS_ID;
    for (int i = 0; i < START_ITEMS; i++)
        inv_add(ITEM_GOODS | start_items[i].id, start_items[i].count);
    changes++;
}

void progress_add_souls(int n)
{
    long long s = (long long)prog.souls + n;
    prog.souls = s < 0 ? 0 : s > 999999999 ? 999999999 : (int)s;
    changes++;
}

int inv_count(u32 id)
{
    int n = 0;
    for (int i = 0; i < prog.inv_count; i++)
        if (prog.inv[i].id == id)
            n += prog.inv[i].num;
    return n;
}

/* DSR's inventory: goods and ammunition stack in one entry up to their maxNum (items.c item_max), every weapon /
 * armour / ring is an entry of its own (durability per piece). More than a stack holds is lost here (DSR sends it to
 * the Bottomless Box: TECH_DEBT inv-1). The list stays in pickup order; the menus sort it (sortId) for display. */
int inv_add(u32 id, int n)
{
    int max = item_max(id);
    changes++;
    if (n < 0) {  /* take from the newest entries first */
        for (int i = prog.inv_count - 1; i >= 0 && n < 0; i--) {
            if (prog.inv[i].id != id)
                continue;
            int take = -n < prog.inv[i].num ? -n : prog.inv[i].num;
            prog.inv[i].num -= take, n += take;
            if (!prog.inv[i].num) {
                memmove(&prog.inv[i], &prog.inv[i + 1], (prog.inv_count - 1 - i) * sizeof prog.inv[0]);
                prog.inv_count--;
            }
        }
        return inv_count(id);
    }
    if (max > 1) {
        int i;
        for (i = 0; i < prog.inv_count && prog.inv[i].id != id; i++)
            ;
        if (i == prog.inv_count) {
            if (prog.inv_count == INV_MAX)
                return -1;
            prog.inv[prog.inv_count++] = (InvItem){id, 0, 0};
        }
        int c = prog.inv[i].num + n;
        prog.inv[i].num = c > max ? max : c;
        return prog.inv[i].num;
    }
    for (; n > 0; n--) {
        if (prog.inv_count == INV_MAX)
            return -1;
        prog.inv[prog.inv_count++] = (InvItem){id, 1, 0};  /* dur 0: at its param's durabilityMax */
    }
    return inv_count(id);
}

/* equipment slot -> what fits (EquipParamWeapon slot flags, protector slot, rings, quick items) */
static u32 slot_cat(int slot)
{
    return slot < EQ_HELM ? ITEM_WEAPON : slot < EQ_RING1 ? ITEM_PROTECTOR : slot < EQ_QUICK1 ? ITEM_ACCESSORY
                                                                                            : ITEM_GOODS;
}

u32 equip_item(int slot)
{
    if (slot < 0 || slot >= EQ_SLOTS || prog.equip[slot] < 0)
        return 0;
    return slot_cat(slot) | (u32)prog.equip[slot];
}

int equip_fits(int slot, u32 id)
{
    if (slot < 0 || slot >= EQ_SLOTS || ITEM_CAT(id) != slot_cat(slot))
        return 0;
    const ItemRow *r = item_row(id);
    if (r) {
        switch (slot) {
        case EQ_WEP_L1: case EQ_WEP_L2: if (!(r->x1 & WS_LEFT)) return 0; break;
        case EQ_WEP_R1: case EQ_WEP_R2: if (!(r->x1 & WS_RIGHT)) return 0; break;
        case EQ_ARROW: case EQ_ARROW2: if (!(r->x1 & WS_ARROW)) return 0; break;
        case EQ_BOLT: case EQ_BOLT2: if (!(r->x1 & WS_BOLT)) return 0; break;
        case EQ_HELM: case EQ_ARMOR: case EQ_GAUNT: case EQ_LEG: if (r->x1 != slot - EQ_HELM) return 0; break;
        default: break;
        }
    }
    if (slot >= EQ_QUICK1 && !(item_goods_flags(id) & IF_EQUIP))
        return 0;
    /* every slot holding it needs a piece of its own (rings and quick items: DSR allows one slot per item) */
    int used = 0;
    for (int k = 0; k < EQ_SLOTS; k++)
        if (k != slot && equip_item(k) == id)
            used++;
    if ((slot >= EQ_RING1 && slot <= EQ_RING2) || slot >= EQ_QUICK1)
        return !used && inv_count(id) > 0;
    return inv_count(id) > used;
}

int equip_set(int slot, u32 id)
{
    if (slot < 0 || slot >= EQ_SLOTS)
        return 0;
    if (!id) {
        prog.equip[slot] = -1;
        changes++;
        return 1;
    }
    if (!equip_fits(slot, id))
        return 0;
    prog.equip[slot] = (s32)ITEM_ROW(id);
    changes++;
    return 1;
}

/* After items leave the inventory (use, discard, event removal): free the slots that point at what is gone */
void equip_check(void)
{
    for (int k = 0; k < EQ_SLOTS; k++) {
        u32 id = equip_item(k);
        if (!id || (id == (ITEM_GOODS | ESTUS_ID)))
            continue;
        int need = 0;
        for (int j = 0; j <= k; j++)
            need += equip_item(j) == id;
        if (inv_count(id) < need)
            prog.equip[k] = -1, changes++;
    }
}

int inv_use(u32 id)
{
    const ItemRow *r = item_row(id);
    int fl = item_goods_flags(id);
    if (ITEM_CAT(id) != ITEM_GOODS || inv_count(id) <= 0)
        return 0;
    int done = 0;
    if (item_goods_type(id) == GOODS_HUMANITY) {  /* Humanity: +1 (EquipParamGoods goodsType 4, behaviour by ESD) */
        prog.humanity = prog.humanity < 99 ? prog.humanity + 1 : 99;
        done = 1;
    } else if (r && (r->st[9] || r->st[10])) {  /* soul items: SpEffectParam.soul of refId */
        progress_add_souls((u16)r->st[9] | (int)(u16)r->st[10] << 16);
        done = 1;
    }
    if (done && (fl & IF_CONSUME)) {
        inv_add(id, -1);
        equip_check();
    }
    changes++;
    return done;
}

int inv_discard(u32 id, int n)
{
    if (!(ITEM_CAT(id) != ITEM_GOODS || (item_goods_flags(id) & IF_DROP)))
        return 0;
    inv_add(id, -n);
    equip_check();
    return 1;
}

u32 progress_map_id(const char *a)
{
    /* "mAA_BB_CC_DD" */
    if (!a || a[0] != 'm' || strlen(a) < 12)
        return 0;
    u32 id = 0;
    for (int k = 0; k < 4; k++)
        id = id << 8 | (u32)strtoul(a + 1 + 3 * k, 0, 10);
    return id;
}

unsigned progress_changes(void) { return changes; }
void progress_touch(void) { changes++; }
