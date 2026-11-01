/* Player progression state (progress.h) */
#include <stdlib.h>
#include <string.h>
#include "progress.h"
#include "progress_data.h"

#define START_ESTUS 5  /* ASSUMED (TECH_DEBT save-5): Estus charges before kindling; DSR hands the flask out in the Asylum */
#define ESTUS_ID 200   /* EquipParamGoods 200 Estus Flask */

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
        inv_add(ITEM_GOODS | start_items[i][0], start_items[i][1]);
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
    for (int i = 0; i < prog.inv_count; i++)
        if (prog.inv[i].id == id)
            return prog.inv[i].num;
    return 0;
}

int inv_add(u32 id, int n)
{
    int i;
    for (i = 0; i < prog.inv_count && prog.inv[i].id != id; i++)
        ;
    if (i == prog.inv_count) {
        if (n <= 0)
            return 0;
        if (prog.inv_count == INV_MAX)
            return -1;
        prog.inv[prog.inv_count++] = (InvItem){id, 0, 0};
    }
    int c = prog.inv[i].num + n;
    changes++;
    if (c <= 0) {  /* gone: keep the list packed in pickup order */
        memmove(&prog.inv[i], &prog.inv[i + 1], (--prog.inv_count - i) * sizeof prog.inv[0]);
        return 0;
    }
    prog.inv[i].num = c > 0xFFFF ? 0xFFFF : c;
    return prog.inv[i].num;
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
