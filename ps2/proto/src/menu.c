/* Start menu: DSR's in-game menus on its own layouts (drb.c screens from menu.drb), driven by the inventory and
 * equipment in progress.c and the item database in items.c.
 *
 * Screens (menu.drb dialog names): R00 the icon bar Start opens (Inventory, Equipment, Status, System), I02
 * Equipment (slot grid I02-00, item pick list + detail I02-02, character columns R02-0x), I01 Inventory (eight tabs,
 * four-row list, detail, use / drop popup I01-01, yes / no F05), C01 Status, F04 System list. The game keeps
 * running behind them, as in DSR. Text fills three ways: <?tags?> inside FMG strings (tag_text), text elements with
 * no string by their element name (text_param_vitality ...), and anonymous values (text_param3) by the label text on
 * their row (label_value: "HP", "Equip Load", "Poise" ...). */
#include <libpad.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "menu.h"
#include "drb.h"
#include "hud.h"
#include "items.h"
#include "progress.h"
#include "combat.h"
#include "combat_data.h"
#include "save.h"

enum { M_CLOSED, M_BAR, M_EQUIP, M_PICK, M_INV, M_POPUP, M_CONFIRM, M_STATUS, M_SYSTEM, M_SYS_CONFIRM };
/* R00 icon order (icon_inventory, icon_equip, icon_status, icon_qwc) and their Ingame_menu FMG names */
enum { BAR_INV, BAR_EQUIP, BAR_STATUS, BAR_SYSTEM, BAR_N };
static const char *const bar_label[BAR_N] = {"Inventory", "Equipment", "Status", "System"};  /* Ingame_menu 110000 /
                                                                                              * 110001 / 110003 / 110006 */
#define LIST_MAX INV_MAX
#define TEXT_BYTES 2048

static int mode, want_mode, bar_sel;
static int eq_sel;                 /* I02 slot cursor: index into EQ_CELLS */
static int pick_sel, pick_top, pick_n;
static u32 pick_ids[LIST_MAX + 1];  /* 0 = remove */
static int inv_tab, inv_sel, inv_top, inv_n, popup_sel, confirm_sel, sys_sel, status_page;
static u16 inv_list[LIST_MAX];      /* prog.inv indices of the tab, DSR order */
static const Player *player;
static float pos_keep[3];
static int detail_ok;               /* itemx.bin loaded */
/* item text (help line + description) of the item under the cursor */
static char text_buf[TEXT_BYTES] __attribute__((aligned(64)));
static u32 text_want, text_have;
static int text_desc = -1, text_pending;
static char msg[96];               /* one-line status after an action */
static float msg_t;

/* I02 slot grid: DRB cursor element -> progress.h slot (cursor0_*: right hand, quick items; cursor1_*: left hand,
 * arrows, bolts; cursor2_*: armour, rings) */
static const struct { const char *cell, *button; int slot; int row, col; } EQ_CELLS[] = {
    {"cursor0_0", "button0_0", EQ_WEP_R1, 0, 0}, {"cursor0_1", "button0_1", EQ_WEP_R2, 0, 1},
    {"cursor0_2", "button0_2", EQ_QUICK1, 0, 2}, {"cursor0_3", "button0_3", EQ_QUICK2, 0, 3},
    {"cursor0_4", "button0_4", EQ_QUICK3, 0, 4}, {"cursor0_5", "button0_5", EQ_QUICK4, 0, 5},
    {"cursor0_6", "button0_6", EQ_QUICK5, 0, 6},
    {"cursor1_0", "button1_0", EQ_WEP_L1, 1, 0}, {"cursor1_1", "button1_1", EQ_WEP_L2, 1, 1},
    {"cursor1_2", "button1_2", EQ_ARROW, 1, 2}, {"cursor1_3", "button1_3", EQ_ARROW2, 1, 3},
    {"cursor1_4", "button1_4", EQ_BOLT, 1, 4}, {"cursor1_5", "button1_5", EQ_BOLT2, 1, 5},
    {"cursor2_0", "button2_0", EQ_HELM, 2, 0}, {"cursor2_1", "button2_1", EQ_ARMOR, 2, 1},
    {"cursor2_2", "button2_2", EQ_GAUNT, 2, 2}, {"cursor2_3", "button2_3", EQ_LEG, 2, 3},
    {"cursor2_4", "button2_4", EQ_RING1, 2, 4}, {"cursor2_5", "button2_5", EQ_RING2, 2, 5},
};
#define EQ_N (int)(sizeof EQ_CELLS / sizeof EQ_CELLS[0])
/* slot names, Ingame_menu FMG 112011 ... */
static const char *slot_name(int slot)
{
    switch (slot) {
    case EQ_WEP_R1: case EQ_WEP_R2: return "Right Hand";
    case EQ_WEP_L1: case EQ_WEP_L2: return "Left Hand";
    case EQ_ARROW: case EQ_ARROW2: return "Arrow";
    case EQ_BOLT: case EQ_BOLT2: return "Bolt";
    case EQ_HELM: return "Head";
    case EQ_ARMOR: return "Chest";
    case EQ_GAUNT: return "Hands";
    case EQ_LEG: return "Legs";
    case EQ_RING1: case EQ_RING2: return "Ring";
    default: return "Usable Item";
    }
}

static int node_of(const char *path) { return drb_find(-1, path); }

/* what a screen's mode needs: screen name, icon cells */
static const char *screen_of(int m, int *icons)
{
    *icons = 0;
    switch (m) {
    case M_BAR: return "R00";
    case M_EQUIP: case M_PICK: *icons = 30; return "I02";
    case M_INV: case M_POPUP: case M_CONFIRM: *icons = 16; return "I01";
    case M_STATUS: return "C01";
    case M_SYSTEM: case M_SYS_CONFIRM: return "F04";
    default: return "";
    }
}

static void set_mode(int m)
{
    int icons;
    const char *want = screen_of(m, &icons);
    if (m == M_CLOSED) {
        drb_close();
        items_detail_free();
        detail_ok = 0;
    } else if (strcmp(drb_screen(), want))
        drb_open(want, icons);
    mode = m;
    text_want = text_have = 0;
}

/* ---- inventory lists ---- */

static const ItemRow *row_of(u32 id)
{
    if (id == (ITEM_GOODS | ESTUS_ID) && prog.estus > 0)
        return item_row(ITEM_GOODS | (ESTUS_ID + 1));  /* the filled flask's name and icon */
    return item_row(id);
}

static int by_sort(const void *a, const void *b)
{
    const InvItem *x = &prog.inv[*(const u16 *)a], *y = &prog.inv[*(const u16 *)b];
    const ItemRow *rx = item_row(x->id), *ry = item_row(y->id);
    u32 sx = rx ? rx->sort : ~0u, sy = ry ? ry->sort : ~0u;
    if (sx != sy)
        return sx < sy ? -1 : 1;
    if (x->id != y->id)
        return x->id < y->id ? -1 : 1;
    return *(const u16 *)a - *(const u16 *)b;
}

/* the tab's entries in DSR order (param sortId; ASSUMED: TECH_DEBT menu-2) */
static void inv_build(void)
{
    inv_n = 0;
    for (int i = 0; i < prog.inv_count; i++)
        if (item_tab(prog.inv[i].id) == inv_tab)
            inv_list[inv_n++] = i;
    qsort(inv_list, inv_n, sizeof inv_list[0], by_sort);
    if (inv_sel >= inv_n)
        inv_sel = inv_n ? inv_n - 1 : 0;
}

static void pick_build(int slot)
{
    pick_n = 0;
    pick_ids[pick_n++] = 0;  /* empty the slot (DSR lists the bare hand / no item first) */
    u16 idx[LIST_MAX];
    int n = 0;
    for (int i = 0; i < prog.inv_count; i++) {
        u32 id = prog.inv[i].id;
        int dup = 0;
        for (int k = 0; k < n; k++)
            dup |= prog.inv[idx[k]].id == id;
        if (!dup && (equip_fits(slot, id) || equip_item(slot) == id))
            idx[n++] = i;
    }
    qsort(idx, n, sizeof idx[0], by_sort);
    for (int k = 0; k < n; k++)
        pick_ids[pick_n++] = prog.inv[idx[k]].id;
    pick_sel = 0;
    for (int k = 0; k < pick_n; k++)
        if (pick_ids[k] == equip_item(slot) && pick_ids[k])
            pick_sel = k;
    pick_top = pick_sel > 4 ? pick_sel - 4 : 0;
}

static u32 cursor_item(void)
{
    switch (mode) {
    case M_EQUIP: return equip_item(EQ_CELLS[eq_sel].slot);
    case M_PICK: return pick_ids[pick_sel];
    case M_INV: case M_POPUP: case M_CONFIRM: return inv_n ? prog.inv[inv_list[inv_sel]].id : 0;
    default: return 0;
    }
}

/* ---- derived character values (DS1 rules; formulas marked where assumed) ---- */

static int equip_weight_x100(void)
{
    int w = 0;
    for (int k = 0; k < EQ_QUICK1; k++) {
        const ItemRow *r = item_row(equip_item(k));
        if (r)
            w += r->weight;
    }
    return w;
}

static int armor_sum(int st)
{
    int s = 0;
    for (int k = EQ_HELM; k <= EQ_LEG; k++) {
        const ItemRow *r = item_row(equip_item(k));
        if (r)
            s += r->st[st];
    }
    return s;
}

/* attunement slots by ATT (DS1: 1 slot at 10, then 12, 14, 16, 19, 23, 28, 34, 41, 50) */
static int attune_slots(int att)
{
    static const u8 at[] = {10, 12, 14, 16, 19, 23, 28, 34, 41, 50};
    int n = 0;
    for (unsigned i = 0; i < sizeof at; i++)
        n += att >= at[i];
    return n;
}

/* a weapon's attack rating at its reinforcement level: base x ReinforceParamWeapon rate (stat scaling not yet:
 * TECH_DEBT menu-4) */
static int weapon_ar(u32 id, int k)
{
    const ItemRow *r = item_row(id);
    if (!r)
        return 0;
    const ItemReinforce *rf = item_reinforce(r->x2, ITEM_LEVEL(id));
    return rf ? r->st[k] * rf->atk[k] / 1000 : r->st[k];
}

static char vbuf[64];
static const char *num(int v) { snprintf(vbuf, sizeof vbuf, "%d", v); return vbuf; }

static const char *weight_text(int x100)
{
    snprintf(vbuf, sizeof vbuf, "%d.%d", x100 / 100, x100 % 100 / 10);
    return vbuf;
}

static const char *tag_text(int node, const char *tag, void *ctx);

/* value of an anonymous text element by the FMG label on its row */
static const char *label_value(const char *label)
{
    const Player *p = player;
    if (!strcmp(label, "HP"))
        return p ? (snprintf(vbuf, sizeof vbuf, "%d/%d", p->hp > 0 ? p->hp : 0, p->hp_max), vbuf) : "";
    if (!strcmp(label, "Stamina"))
        return p ? (snprintf(vbuf, sizeof vbuf, "%d/%d", p->stamina, p->stamina_max), vbuf) : "";
    if (!strcmp(label, "Equip Load")) {
        int w = equip_weight_x100(), mx = 40 + prog.stats[ST_END];  /* DS1: 40 + Endurance */
        snprintf(vbuf, sizeof vbuf, "%d.%d/%d.0", w / 100, w % 100 / 10, mx);
        return vbuf;
    }
    if (!strcmp(label, "Poise")) return num(armor_sum(11));
    if (!strcmp(label, "Item Discovery"))  /* ASSUMED (TECH_DEBT menu-4): 100 + 1 per humanity up to 10, no rings */
        return num(100 + (prog.humanity < 10 ? prog.humanity : 10));
    if (!strcmp(label, "Attunement Slots")) return num(attune_slots(prog.stats[ST_ATT]));
    if (!strcmp(label, "Bleed Resist")) return num(armor_sum(9));
    if (!strcmp(label, "Poison Resist")) return num(armor_sum(7));
    if (!strcmp(label, "Venom Resist") || !strcmp(label, "Toxic Resist")) return num(armor_sum(8));
    if (!strcmp(label, "Curse Resist"))  /* FUN_1402dddd0 type 3: CalcCorrectGraph 113 at min(humanity, 99) + armour */
        return num((int)combat_graph(graph_113, prog.humanity < 99 ? prog.humanity : 99) + armor_sum(10));
    if (!strcmp(label, "Physical Def.") || !strcmp(label, "PhysDef") || !strcmp(label, "Physical")) return num(armor_sum(0));
    if (!strcmp(label, "VS strike") || !strcmp(label, "Strike")) return num(armor_sum(1));
    if (!strcmp(label, "VS slash") || !strcmp(label, "Slash")) return num(armor_sum(2));
    if (!strcmp(label, "VS thrust") || !strcmp(label, "Thrust")) return num(armor_sum(3));
    if (!strcmp(label, "Magic DEF") || !strcmp(label, "MagDEF") || !strcmp(label, "Magic")) return num(armor_sum(4));
    if (!strcmp(label, "Flame DEF") || !strcmp(label, "FireDEF") || !strcmp(label, "Fire")) return num(armor_sum(5));
    if (!strcmp(label, "Lightning DEF") || !strcmp(label, "LitngDEF") || !strcmp(label, "Lightning")) return num(armor_sum(6));
    if (!strcmp(label, "R Weapon 1")) return num(weapon_ar(equip_item(EQ_WEP_R1), 0));
    if (!strcmp(label, "R Weapon 2")) return num(weapon_ar(equip_item(EQ_WEP_R2), 0));
    if (!strcmp(label, "L Weapon 1")) return num(weapon_ar(equip_item(EQ_WEP_L1), 0));
    if (!strcmp(label, "L Weapon 2")) return num(weapon_ar(equip_item(EQ_WEP_L2), 0));
    if (!strcmp(label, "Level")) return num(prog.level);
    if (!strcmp(label, "Souls")) return num(prog.souls);
    if (!strcmp(label, "Humanity")) return num(prog.humanity);
    if (!strcmp(label, "Vitality")) return num(prog.stats[ST_VIT]);
    if (!strcmp(label, "Attunement")) return num(prog.stats[ST_ATT]);
    if (!strcmp(label, "Endurance")) return num(prog.stats[ST_END]);
    if (!strcmp(label, "Strength")) return num(prog.stats[ST_STR]);
    if (!strcmp(label, "Dexterity")) return num(prog.stats[ST_DEX]);
    if (!strcmp(label, "Resistance")) return num(prog.stats[ST_RES]);
    if (!strcmp(label, "Intelligence")) return num(prog.stats[ST_INT]);
    if (!strcmp(label, "Faith")) return num(prog.stats[ST_FAI]);
    return NULL;
}

/* label of each anonymous text element: the FMG text element left of it on its row, in the same dialog. Built when a
 * screen opens. */
static s16 *label_of;
static int label_nodes;
static void labels_build(void)
{
    free(label_of);
    label_of = 0;
    label_nodes = 0;
    int root = node_of(drb_screen());
    const DrbNode *rn = drb_node(root);
    if (!rn)
        return;
    label_nodes = rn->end;
    label_of = malloc(label_nodes * sizeof *label_of);
    if (!label_of)
        return;
    for (int i = 0; i < label_nodes; i++)
        label_of[i] = -1;
    for (int d = 0; d < label_nodes; d++) {  /* every dialog: pair its direct text children */
        const DrbNode *dn = drb_node(d);
        if (!dn || dn->type != DRB_DIALOG)
            continue;
        for (int v = d + 1; v < dn->end; v = drb_node(v)->end) {
            const DrbNode *vn = drb_node(v);
            if (vn->type != DRB_TEXT || vn->spr >= 0)
                continue;
            int best = -1, bx = -10000;
            float vy = (vn->y0 + vn->y1) / 2.0f;
            for (int l = d + 1; l < dn->end; l = drb_node(l)->end) {
                const DrbNode *ln = drb_node(l);
                if (ln->type != DRB_TEXT || ln->spr < 0 || strstr(drb_string(ln->spr), "<?"))
                    continue;
                float ly = (ln->y0 + ln->y1) / 2.0f;
                if (ly - vy > 6 || vy - ly > 6 || ln->x0 > vn->x0 || ln->x0 <= bx)
                    continue;
                best = l, bx = ln->x0;
            }
            label_of[v] = best;
        }
    }
}

static unsigned hash(const char *s)
{
    unsigned h = 0x811C9DC5u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 0x01000193u;
    return h;
}

/* item detail panels (data_simple_*, list rows list_arms / list_armor / list_item): text elements by name */
static u32 detail_id;
static const char *detail_text(unsigned h)
{
    u32 id = detail_id;
    const ItemRow *r = row_of(id);
    static char nb[64];
    if (!r)
        return NULL;
    if (h == hash("text_param_name")) return item_name(id == (ITEM_GOODS | ESTUS_ID) && prog.estus > 0 ? id + 1 : id, nb, sizeof nb);
    if (h == hash("text_param_weight")) return weight_text(r->weight);
    if (h == hash("text_param_durable") || h == hash("text_param_durable_1"))
        return r->dur ? num(r->dur) : NULL;  /* current and maximum are separate elements (not tracked: both max) */
    if (ITEM_CAT(id) == ITEM_WEAPON) {
        static const char *const atk[4] = {"text_param_attack_p", "text_param_attack_m", "text_param_attack_f",
                                           "text_param_attack_t"};
        for (int k = 0; k < 4; k++)
            if (h == hash(atk[k]))
                return num(weapon_ar(id, k));
        static const char *const req[4] = {"text_param_proper_strength", "text_param_proper_agility",
                                           "text_param_proper_magic", "text_param_proper_faith"};
        for (int k = 0; k < 4; k++)
            if (h == hash(req[k]))
                return num(r->st[8 + k]);
        if (h == hash("text_param_stability") || h == hash("text_param_guard_stamina")) return num(r->x0);
    }
    if (ITEM_CAT(id) == ITEM_PROTECTOR) {
        static const char *const def[4] = {"text_param_defense_p", "text_param_defense_m", "text_param_defense_f",
                                           "text_param_defense_t"};
        static const int defk[4] = {0, 4, 5, 6};
        for (int k = 0; k < 4; k++)
            if (h == hash(def[k]))
                return num(r->st[defk[k]]);
    }
    if (h == hash("text_param_caption") && text_have == id && text_desc >= 0)
        return text_buf;  /* the help line */
    if (h == hash("text_number") || h == hash("text_param_num")) {
        int c = id == (ITEM_GOODS | ESTUS_ID) ? prog.estus : inv_count(id);
        return item_max(id) > 1 || id == (ITEM_GOODS | ESTUS_ID) ? num(c) : NULL;
    }
    return NULL;
}

static const char *tag_text(int node, const char *tag, void *ctx)
{
    (void)ctx;
    const DrbNode *n = drb_node(node);
    if (!tag) {  /* a text element with no FMG string */
        const char *s = detail_text(n->hash);
        if (s)
            return s;
        if (label_of && node < label_nodes && label_of[node] >= 0)
            return label_value(drb_string(drb_node(label_of[node])->spr));
        return NULL;
    }
    if (!strcmp(tag, "lineHelp")) {
        u32 id = cursor_item();
        if (mode == M_BAR)
            return bar_label[bar_sel];
        if (msg_t > 0)
            return msg;
        if (id && text_have == id && text_desc >= 0)
            return text_buf;
        return mode == M_EQUIP ? slot_name(EQ_CELLS[eq_sel].slot) : "";
    }
    if (!strcmp(tag, "keyGuide")) {
        switch (mode) {
        case M_EQUIP: return "X: Select  O: Back  L1/R1: Page";
        case M_PICK: return "X: Equip  O: Back";
        case M_INV: return "X: Menu  O: Back  L1/R1: Category";
        case M_POPUP: case M_CONFIRM: case M_SYS_CONFIRM: return "X: Enter  O: Back";
        default: return "O: Back";
        }
    }
    if (!strcmp(tag, "pcName")) return "";  /* character names come with character creation (TECH_DEBT menu-5) */
    if (!strcmp(tag, "pcLevel")) return num(prog.level);
    if (!strcmp(tag, "souls")) return num(prog.souls);
    if (!strcmp(tag, "pcVitality")) return num(prog.stats[ST_VIT]);
    if (!strcmp(tag, "pcWill")) return num(prog.stats[ST_ATT]);
    if (!strcmp(tag, "pcEndur")) return num(prog.stats[ST_END]);
    if (!strcmp(tag, "pcStrength")) return num(prog.stats[ST_STR]);
    if (!strcmp(tag, "pcDexter")) return num(prog.stats[ST_DEX]);
    if (!strcmp(tag, "pcDurability")) return num(prog.stats[ST_RES]);
    if (!strcmp(tag, "pcIntellect")) return num(prog.stats[ST_INT]);
    if (!strcmp(tag, "pcForce")) return num(prog.stats[ST_FAI]);
    if (!strcmp(tag, "pcHeroic")) return num(prog.humanity);
    if (!strcmp(tag, "pcHP")) return label_value("HP");
    if (!strcmp(tag, "pcMaxHp")) return player ? num(player->hp_max) : "";
    if (!strcmp(tag, "pcMaxStamina")) return player ? num(player->stamina_max) : "";
    if (!strcmp(tag, "equipWeight")) return weight_text(equip_weight_x100());
    if (!strcmp(tag, "maxEquipWeight")) return num(40 + prog.stats[ST_END]);
    if (!strcmp(tag, "magicSlotNum")) return num(attune_slots(prog.stats[ST_ATT]));
    if (!strcmp(tag, "pcItemOccur")) return label_value("Item Discovery");
    if (!strcmp(tag, "defSAToughnessTotal")) return label_value("Poise");
    if (!strcmp(tag, "defPhysicalTotal")) return label_value("Physical Def.");
    if (!strcmp(tag, "defBluntTotal")) return label_value("VS strike");
    if (!strcmp(tag, "defCutTotal")) return label_value("VS slash");
    if (!strcmp(tag, "defStabTotal")) return label_value("VS thrust");
    if (!strcmp(tag, "defMagicTotal")) return label_value("Magic DEF");
    if (!strcmp(tag, "defFireTotal")) return label_value("Flame DEF");
    if (!strcmp(tag, "defThunderboltTotal")) return label_value("Lightning DEF");
    if (!strcmp(tag, "resistBleedingTotal")) return label_value("Bleed Resist");
    if (!strcmp(tag, "resistPoisonTotal")) return label_value("Poison Resist");
    if (!strcmp(tag, "resistPlagueTotal")) return label_value("Venom Resist");
    if (!strcmp(tag, "resistCurseTotal")) return label_value("Curse Resist");
    if (!strcmp(tag, "atckR1")) return label_value("R Weapon 1");
    if (!strcmp(tag, "atckR2")) return label_value("R Weapon 2");
    if (!strcmp(tag, "atckL1")) return label_value("L Weapon 1");
    if (!strcmp(tag, "atckL2")) return label_value("L Weapon 2");
    return NULL;  /* other tags (equipMagicInfo*, lvUp*, ...) come with attunement / level up */
}

/* ---- input ---- */

static void say(const char *s)
{
    snprintf(msg, sizeof msg, "%s", s);
    msg_t = 2.5f;
}

static int move(int sel, int n, int d)
{
    if (n <= 0)
        return 0;
    sel += d;
    return sel < 0 ? n - 1 : sel >= n ? 0 : sel;
}

/* I02 grid: the nearest cell in direction (dr, dc) */
static int eq_move(int sel, int dr, int dc)
{
    int r = EQ_CELLS[sel].row, c = EQ_CELLS[sel].col;
    if (dc) {
        for (int i = 0; i < EQ_N; i++)
            if (EQ_CELLS[i].row == r && EQ_CELLS[i].col == c + dc)
                return i;
        return sel;
    }
    int nr = r + dr, best = sel, bd = 99;
    if (nr < 0 || nr > 2)
        return sel;
    for (int i = 0; i < EQ_N; i++)
        if (EQ_CELLS[i].row == nr) {
            int d = abs(EQ_CELLS[i].col - c);
            if (d < bd)
                bd = d, best = i;
        }
    return best;
}

int menu_update(unsigned short pressed)
{
    if (msg_t > 0)
        msg_t -= 1 / 30.0f;
    ui_set_pos(pos_keep);
    if (mode != M_CLOSED) {
        int st = drb_step();
        if (st > 0 && !label_of)
            labels_build();
        if (st < 0) {  /* screen failed (logged): give up rather than trap input */
            set_mode(M_CLOSED);
            return 0;
        }
        if (mode >= M_EQUIP && !detail_ok)
            detail_ok = items_detail_step(ui_alloc, ui_free) > 0;
        /* help line of the item under the cursor */
        u32 id = cursor_item();
        if (detail_ok && id != text_have && !text_pending) {
            text_want = id;
            if (id && item_text_request(id, text_buf, sizeof text_buf) > 0)
                text_pending = 1;
            else if (!id || item_text_request(id, text_buf, sizeof text_buf) < 0)
                text_have = id, text_desc = -1;
        }
        if (text_pending) {
            int r = item_text_done(&text_desc);
            if (r) {
                text_pending = 0;
                text_have = text_want;
                if (r < 0)
                    text_desc = -1;
            }
        }
    }
    if (mode == M_CLOSED) {
        if (pressed & PAD_START) {
            bar_sel = 0;
            set_mode(M_BAR);
            free(label_of), label_of = 0;
            return 1;
        }
        return 0;
    }
    if (pressed & PAD_START) {  /* Start closes every menu, as in DSR */
        set_mode(M_CLOSED);
        return 1;
    }
    int screen_changed = 0;
    int back = pressed & PAD_CIRCLE, ok = pressed & PAD_CROSS;
    switch (mode) {
    case M_BAR:
        if (pressed & PAD_LEFT) bar_sel = move(bar_sel, BAR_N, -1);
        if (pressed & PAD_RIGHT) bar_sel = move(bar_sel, BAR_N, 1);
        if (back) { set_mode(M_CLOSED); return 1; }
        if (ok) {
            static const int to[BAR_N] = {M_INV, M_EQUIP, M_STATUS, M_SYSTEM};
            inv_sel = inv_top = 0, sys_sel = 0;
            set_mode(to[bar_sel]);
            screen_changed = 1;
        }
        break;
    case M_EQUIP:
        if (pressed & PAD_UP) eq_sel = eq_move(eq_sel, -1, 0);
        if (pressed & PAD_DOWN) eq_sel = eq_move(eq_sel, 1, 0);
        if (pressed & PAD_LEFT) eq_sel = eq_move(eq_sel, 0, -1);
        if (pressed & PAD_RIGHT) eq_sel = eq_move(eq_sel, 0, 1);
        if (pressed & PAD_L1) status_page = (status_page + 2) % 3;
        if (pressed & PAD_R1) status_page = (status_page + 1) % 3;
        if (ok && detail_ok) { pick_build(EQ_CELLS[eq_sel].slot); mode = M_PICK; }
        if (back) { set_mode(M_BAR); screen_changed = 1; }
        break;
    case M_PICK:
        if (pressed & PAD_UP) pick_sel = move(pick_sel, pick_n, -1);
        if (pressed & PAD_DOWN) pick_sel = move(pick_sel, pick_n, 1);
        if (pick_sel < pick_top) pick_top = pick_sel;
        if (pick_sel > pick_top + 4) pick_top = pick_sel - 4;
        if (ok) {
            int slot = EQ_CELLS[eq_sel].slot;
            if (!equip_set(slot, pick_ids[pick_sel]))
                say("Cannot equip");
            mode = M_EQUIP;
        }
        if (back) mode = M_EQUIP;
        break;
    case M_INV:
        if (pressed & PAD_L1) { inv_tab = move(inv_tab, TAB_COUNT, -1); inv_sel = inv_top = 0; }
        if (pressed & PAD_R1) { inv_tab = move(inv_tab, TAB_COUNT, 1); inv_sel = inv_top = 0; }
        inv_build();
        if (pressed & PAD_UP) inv_sel = move(inv_sel, inv_n, -1);
        if (pressed & PAD_DOWN) inv_sel = move(inv_sel, inv_n, 1);
        if (inv_sel < inv_top) inv_top = inv_sel;
        if (inv_sel > inv_top + 3) inv_top = inv_sel - 3;
        if (ok && inv_n) { popup_sel = 0; mode = M_POPUP; }
        if (back) { set_mode(M_BAR); screen_changed = 1; }
        break;
    case M_POPUP:
        if (pressed & PAD_UP) popup_sel = move(popup_sel, 2, -1);
        if (pressed & PAD_DOWN) popup_sel = move(popup_sel, 2, 1);
        if (back) mode = M_INV;
        if (ok) {
            u32 id = cursor_item();
            if (popup_sel == 0) {  /* Use Item */
                if (!inv_use(id))
                    say("Cannot use this item here");
                mode = M_INV;
            } else if (ITEM_CAT(id) == ITEM_GOODS && !(item_goods_flags(id) & IF_DROP)) {
                say("Cannot discard this item");
                mode = M_INV;
            } else {
                confirm_sel = 1;  /* DSR's yes / no starts on No for a discard (ASSUMED: TECH_DEBT menu-3) */
                mode = M_CONFIRM;
            }
        }
        break;
    case M_CONFIRM:
        if (pressed & (PAD_LEFT | PAD_RIGHT)) confirm_sel ^= 1;
        if (back) mode = M_POPUP;
        if (ok) {
            if (!confirm_sel)
                inv_discard(cursor_item(), 1);
            mode = M_INV;
        }
        break;
    case M_STATUS:
        if (back) { set_mode(M_BAR); screen_changed = 1; }
        break;
    case M_SYSTEM:
        if (pressed & PAD_UP) sys_sel = move(sys_sel, 2, -1);
        if (pressed & PAD_DOWN) sys_sel = move(sys_sel, 2, 1);
        if (back) { set_mode(M_BAR); screen_changed = 1; }
        if (ok && sys_sel == 1) { confirm_sel = 1; mode = M_SYS_CONFIRM; }
        break;
    case M_SYS_CONFIRM:
        if (pressed & (PAD_LEFT | PAD_RIGHT)) confirm_sel ^= 1;
        if (back) mode = M_SYSTEM;
        if (ok) {
            if (!confirm_sel) {  /* Quit Game: DSR saves and returns to the title (no title screen yet: menu-5) */
                save_write(player);
                set_mode(M_CLOSED);
                return 1;
            }
            mode = M_SYSTEM;
        }
        break;
    }
    if (screen_changed)
        free(label_of), label_of = 0;
    (void)want_mode;
    return 1;
}

/* ---- drawing ---- */

/* template dialog `tpl` (e.g. a cursor) centred on node `at` */
static void show(int root, const char *path, int on)
{
    drb_show(root >= 0 ? drb_find(root, path) : node_of(path), on);
}

/* F05 yes / no: only the question box; F05-00's name plate (portrait, name, multiplayer record) is the talk variant */
static void draw_yes_no(const char *question)
{
    int f = drb_find(node_of("F05"), "F05-00");
    static const char *const plate[] = {"F22_BG", "Equip_frame_1", "Equip_frame_3", "object1", "text_name",
                                        "text_param_name"};
    for (unsigned k = 0; k < sizeof plate / sizeof plate[0]; k++)
        show(f, plate[k], 0);
    drb_place(drb_child(f, "cursor"), drb_child(f, confirm_sel ? "cursor_button0_1" : "cursor_button0_0"));
    drb_draw(f, 0, 0, 0x80, tag_text, 0);
    drb_text(drb_child(f, "text_question"), 0, 0, question, 0x80);
}

static void icon_at(u32 id, int node)
{
    const ItemRow *r = row_of(id);
    const DrbNode *n = drb_node(node);
    if (r && n)
        drb_icon(r->icon, n->x0, n->y0, n->x1 - n->x0, n->y1 - n->y0, 0x80);
}

static void draw_bar(void)
{
    int root = node_of("R00");
    static const char *const ch[BAR_N] = {"changeover_0", "changeover_1", "changeover_2", "changeover_3"};
    int r00 = drb_find(root, "R00-00");
    for (int k = 0; k < BAR_N; k++)
        show(r00, ch[k], k == bar_sel);
    show(r00, "plate_black", 0);
    drb_draw(root, 0, 0, 0x80, tag_text, 0);
    int help = drb_child(drb_find(root, "R04-02"), "text_help0");
    if (help >= 0)
        drb_text(help, 0, 0, bar_label[bar_sel], 0x80);
}

/* the R02 character columns: one page at a time (L1 / R1) */
static void status_pages(int root)
{
    static const char *const pg[3] = {"R02-00", "R02-04", "R02-03"};
    for (int k = 0; k < 3; k++)
        show(root, pg[k], k == status_page);
}

/* one list row (list<r> of a data_list): the variant that fits the item kind */
static void list_row(int list, u32 id)
{
    static const char *const kinds[] = {"list_item", "list_arms", "list_armor", "list_arrow", "list_magic"};
    int want = 0;
    int tab = item_tab(id);
    if (tab == TAB_WEAPON) want = 1;
    else if (tab == TAB_ARMOR) want = 2;
    else if (tab == TAB_AMMO) want = 3;
    else if (tab == TAB_SPELL) want = 4;
    for (int k = 0; k < 5; k++)
        drb_show(drb_child(list, kinds[k]), k == want);
    detail_id = id;
    drb_draw(list, 0, 0, 0x80, tag_text, 0);
}

static void draw_equip(void)
{
    int root = node_of("I02");
    int grid = drb_find(root, "I02-00"), pick = drb_find(root, "I02-02");
    status_pages(root);
    for (int k = 3; k <= 7; k++) {
        char nm[8];
        snprintf(nm, sizeof nm, "I02-0%d", k);
        show(root, nm, 0);
    }
    drb_show(grid, mode == M_EQUIP);
    drb_show(pick, 0);
    drb_place(drb_child(grid, "cursor"), drb_child(grid, EQ_CELLS[eq_sel].cell));
    detail_id = 0;
    drb_draw(root, 0, 0, 0x80, tag_text, 0);
    if (mode == M_EQUIP) {
        for (int i = 0; i < EQ_N; i++) {
            int b = drb_child(grid, EQ_CELLS[i].button);
            u32 id = equip_item(EQ_CELLS[i].slot);
            if (id)
                icon_at(id, drb_child(b, "icon"));
            detail_id = id;
            int tn = drb_child(b, "text_number");
            const char *c = id ? detail_text(drb_node(tn) ? drb_node(tn)->hash : 0) : NULL;
            if (c)
                drb_text(tn, 0, 0, c, 0x80);
        }
        char nb[64];
        u32 id = equip_item(EQ_CELLS[eq_sel].slot);
        drb_text(drb_child(grid, "text_category"), 0, 0, slot_name(EQ_CELLS[eq_sel].slot), 0x80);
        drb_text(drb_child(grid, "text_item-name"), 0, 0, id ? item_name(row_of(id) != item_row(id) ? id + 1 : id, nb, sizeof nb) : "", 0x80);
    } else {  /* M_PICK: the pick list in I02-02's rows */
        drb_show(pick, 1);
        int dl = drb_child(pick, "data_list");
        show(pick, "sort_cursor", 0), show(pick, "sort_list_cursor", 0);
        char cn[16];
        snprintf(cn, sizeof cn, "cursor%d_0", pick_sel - pick_top);
        drb_place(drb_child(pick, "cursor"), drb_child(pick, cn));
        snprintf(cn, sizeof cn, "list_cursor%d_0", pick_sel - pick_top);
        drb_place(drb_child(pick, "list_cursor"), drb_child(pick, cn));
        for (int k = 0; k < 6; k++) {  /* the item detail panels share the list's place: DSR shows one or the other */
            static const char *const dp[6] = {"data_simple_item", "data_simple_arms", "data_simple_armor",
                                              "data_simple_arrow", "data_simple_magic", "data_add_effect"};
            show(pick, dp[k], 0);
        }
        for (int r = 0; r < 5; r++) {  /* rows drawn one by one below (list_row), empty ones not at all */
            char nm[12];
            snprintf(nm, sizeof nm, "list%d", r);
            drb_show(drb_child(dl, nm), 0);
        }
        for (int t = 0; t < 5; t++) {
            char nm[12];
            snprintf(nm, sizeof nm, "data_tab_%d", t);
            show(pick, nm, 0);
        }
        detail_id = 0;
        drb_draw(pick, 0, 0, 0x80, tag_text, 0);
        for (int r = 0; r < 5; r++) {
            char nm[12];
            snprintf(nm, sizeof nm, "list%d", r);
            int ln = drb_child(dl, nm);
            snprintf(nm, sizeof nm, "button%d_0", r);
            int bn = drb_child(pick, nm);
            int k = pick_top + r;
            if (k >= pick_n || ln < 0)
                continue;
            u32 id = pick_ids[k];
            if (id) {
                list_row(ln, id);
                icon_at(id, drb_child(bn, "icon"));
            } else
                drb_text(drb_child(ln, "text_param_name"), 0, 0, "(Remove)", 0x80);
        }
    }
}

static void draw_inventory(void)
{
    int root = node_of("I01");
    int lst = drb_find(root, "I02-02");  /* -> I01_03: tabs, list, detail */
    status_pages(root);
    for (int k = 4; k <= 9; k++) {
        char nm[8];
        snprintf(nm, sizeof nm, "I01-0%d", k);
        show(root, nm, 0);
    }
    show(root, "I01-01", 0);
    show(lst, "repository_tab_select", 0), show(lst, "repair_tab_select", 0), show(lst, "I01_arrow_test", 0);
    int tabs = drb_child(lst, "tab_select");
    for (int t = 0; t < TAB_COUNT; t++) {
        char nm[20];
        snprintf(nm, sizeof nm, "tab_changeover_%d", t);
        show(tabs, nm, t == inv_tab);
        snprintf(nm, sizeof nm, "blackout0_%d", t);
        show(tabs, nm, 0);
    }
    int dl = drb_child(lst, "data_list");
    for (int r = 0; r < 4; r++) {
        char nm[12];
        snprintf(nm, sizeof nm, "list%d", r);
        drb_show(drb_child(dl, nm), 0);
        snprintf(nm, sizeof nm, "button%d_0", r);
        drb_show(drb_child(lst, nm), 0);
    }
    show(lst, "data_simple_arms", 0), show(lst, "data_simple_armor", 0), show(lst, "data_simple_item", 0);
    show(lst, "data_simple_arrow", 0), show(lst, "data_simple_magic", 0), show(lst, "data_add_effect", 0);
    char tc[20];
    snprintf(tc, sizeof tc, "tab_cursor0_%d", inv_tab);
    drb_place(drb_child(tabs, "tab_cursor"), drb_child(tabs, tc));
    snprintf(tc, sizeof tc, "cursor%d_0", inv_sel - inv_top);
    drb_place(drb_child(lst, "cursor"), inv_n ? drb_child(lst, tc) : -1);
    snprintf(tc, sizeof tc, "list_cursor%d_0", inv_sel - inv_top);
    drb_place(drb_child(lst, "list_cursor"), inv_n ? drb_child(lst, tc) : -1);
    detail_id = 0;  /* item texts only in the rows drawn below, not in stray template elements */
    drb_draw(root, 0, 0, 0x80, tag_text, 0);
    for (int r = 0; r < 4; r++) {
        int k = inv_top + r;
        if (k >= inv_n)
            break;
        u32 id = prog.inv[inv_list[k]].id;
        char nm[12];
        snprintf(nm, sizeof nm, "list%d", r);
        int ln = drb_child(dl, nm);
        snprintf(nm, sizeof nm, "button%d_0", r);
        int bn = drb_child(lst, nm);
        drb_show(bn, 1);
        show(bn, "grayout", 0), show(bn, "icon_owner0", 0), show(bn, "text_number_reposit", 0), show(bn, "text_wepCate", 0);
        detail_id = id;
        drb_draw(bn, 0, 0, 0x80, tag_text, 0);
        icon_at(id, drb_child(bn, "icon"));
        list_row(ln, id);
    }
    /* the data_simple_* detail panels take the list's place in DSR (one or the other), so they stay hidden here */
    if (mode == M_POPUP) {
        int pop = drb_find(root, "I01-01");
        if (pop < 0)
            pop = node_of("I01-01");
        show(pop, "grayout_drop00", 0), show(pop, "grayout_drop01", 0);
        show(pop, "button2_0", 0), show(pop, "button3_0", 0);  /* Change Order / Reset Order: not yet (menu-2) */
        char nm[12];
        snprintf(nm, sizeof nm, "cursor%d_0", popup_sel);
        drb_place(drb_child(pop, "cursor"), drb_child(pop, nm));
        drb_draw(pop, 0, 0, 0x80, tag_text, 0);
    }
    if (mode == M_CONFIRM) {
        char nb[64];
        char q[96];
        snprintf(q, sizeof q, "Discard %s?", item_name(cursor_item(), nb, sizeof nb));
        draw_yes_no(q);
    }
}

static void draw_status(void)
{
    int root = node_of("C01");
    for (int k = 5; k <= 12; k++) {
        char nm[8];
        snprintf(nm, sizeof nm, "C01-%02d", k);
        show(root, nm, 0);
    }
    int c4 = drb_find(root, "C01-04");
    show(c4, "data_simple_arms", 0), show(c4, "data_simple_catalyst", 0), show(c4, "data_simple_bow", 0);
    int c3 = drb_find(root, "C01-03");
    show(c3, "cursor0", 0), show(c3, "cursor1", 0);
    drb_show(drb_child(c3, "text_param-name5"), 0);  /* "ReqSouls" and its cost: the Level Up screen's */
    drb_show(drb_child(c3, "text_param_cost"), 0);
    for (int k = 0; k < 10; k++) {
        char nm[12];
        snprintf(nm, sizeof nm, "arrow%d_0", k);
        show(c3, nm, 0);  /* level-up arrows: the bonfire's Level Up screen */
    }
    drb_draw(root, 0, 0, 0x80, tag_text, 0);
    static const struct { const char *name; int st; } vals[] = {
        {"text_param_vitality", ST_VIT}, {"text_param_will", ST_ATT}, {"text_param_endure", ST_END},
        {"text_param_strength", ST_STR}, {"text_param_dexter", ST_DEX}, {"text_param_durability", ST_RES},
        {"text_param_intellect", ST_INT}, {"text_param_force", ST_FAI}};
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++)
        drb_text(drb_child(c3, vals[i].name), 0, 0, num(prog.stats[vals[i].st]), 0x80);
    drb_text(drb_child(c3, "text_param_level"), 0, 0, num(prog.level), 0x80);
    drb_text(drb_child(c3, "text_param_soul"), 0, 0, num(prog.souls), 0x80);
    drb_text(drb_child(c3, "text_param_heroic"), 0, 0, num(prog.humanity), 0x80);
}

static void draw_system(void)
{
    int root = node_of("F04");
    int lst = drb_find(root, "F04-01");
    show(root, "F04-00", 0);
    for (int k = 2; k <= 10; k++) {
        char nm[12];
        snprintf(nm, sizeof nm, "button%d_0", k);
        show(lst, nm, 0);
    }
    char nm[12];
    snprintf(nm, sizeof nm, "cursor%d_0", sys_sel);
    drb_place(drb_child(lst, "cursor"), drb_child(lst, nm));
    drb_draw(lst, 0, 0, 0x80, tag_text, 0);
    drb_text(drb_child(lst, "button0_0"), 0, 0, "Options", 0x80);   /* Ingame_menu 115000 */
    drb_text(drb_child(lst, "button1_0"), 0, 0, "Quit Game", 0x80); /* Ingame_menu 115002 */
    if (mode == M_SYS_CONFIRM)
        draw_yes_no("Quit game?");
}

void menu_draw(const Player *p)
{
    player = p;
    if (p)
        pos_keep[0] = p->pos.x, pos_keep[1] = p->pos.y, pos_keep[2] = p->pos.z;
    if (mode == M_CLOSED || !drb_ready())
        return;
    hud_begin();
    switch (mode) {
    case M_BAR: draw_bar(); break;
    case M_EQUIP: case M_PICK: draw_equip(); break;
    case M_INV: case M_POPUP: case M_CONFIRM: draw_inventory(); break;
    case M_STATUS: draw_status(); break;
    case M_SYSTEM: case M_SYS_CONFIRM: draw_system(); break;
    }
    hud_end();
}

int menu_open(void) { return mode != M_CLOSED; }
