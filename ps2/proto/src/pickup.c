/* World items: treasure shines, enemy drops, the pickup anim and the item get dialog (see pickup.h) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drb.h"
#include "ui_req.h"
#include "event.h"
#include "fx.h"
#include "hud.h"
#include "items.h"
#include "menu.h"
#include "model.h"
#include "pickup.h"
#include "progress.h"

PickupStats pickup_stats;

/* ---- enemy drops (exe MapItemManImpl) ---- */
/* FUN_1403f9050: a 31st record removes the oldest (count kept <= 0x1d + 1) */
#define DROP_MAX 30
/* Pickup reach: as event.c's treasures (ASSUMED 1.5 m, +-2 m in height: TECH_DEBT event-4) */
#define REACH 1.5f
/* Shine above the corpse: ChrShineTreasureSlot sits at the corpse's dummy poly 90 (FUN_1403bf520), which the port
 * does not export; ASSUMED 0.3 m above the feet (the corpse lies on the ground), TECH_DEBT item-1 */
#define DROP_UP 0.3f
typedef struct {
    float pos[3];
    PickItem it[PICK_ITEMS];
    int n, shine;
    unsigned seq;
} Drop;
static Drop drops[DROP_MAX];
static int ndrops;
static unsigned drop_seq;

static void drop_remove(int i)
{
    pickup_shine_del(drops[i].shine);
    drops[i] = drops[--ndrops];
}

int pickup_shine_add(const float pos[3]) { return fx_shine_add(0, pos); }
void pickup_shine_del(int h)
{
    if (h >= 0)
        fx_shine_del(h);
}

static void drop_lot(s32 npc, s32 lot, const float pos[3]);
void pickup_enemy_drop(s32 npc, const float pos[3])
{
    s32 lot = event_npc_lot(npc);
    if (lot > 0)
        drop_lot(npc, lot, pos);
}

static void drop_lot(s32 npc, s32 lot, const float pos[3])
{
    Drop d;
    memset(&d, 0, sizeof d);
    d.n = event_lot_roll(lot, d.it, PICK_ITEMS);
    pickup_stats.rolled++;
    if (d.n <= 0) {
        printf("pickup: npc %d lot %d dropped nothing\n", (int)npc, (int)lot);
        return;
    }
    if (ndrops == DROP_MAX) {  /* the oldest goes */
        int o = 0;
        for (int i = 1; i < ndrops; i++)
            o = drops[i].seq < drops[o].seq ? i : o;
        drop_remove(o);
    }
    memcpy(d.pos, pos, sizeof d.pos);
    float sp[3] = {pos[0], pos[1] + DROP_UP, pos[2]};
    d.shine = pickup_shine_add(sp);
    d.seq = ++drop_seq;
    drops[ndrops++] = d;
    printf("pickup: npc %d lot %d dropped %d item(s), first %08x x%d, at %.1f %.1f %.1f\n", (int)npc, (int)lot, d.n,
           (unsigned)d.it[0].item, d.it[0].num, pos[0], pos[1], pos[2]);
}

int pickup_action(Player *p)
{
    int best = -1;
    float bd = REACH * REACH;
    for (int i = 0; i < ndrops; i++) {
        float dx = drops[i].pos[0] - p->pos.x, dz = drops[i].pos[2] - p->pos.z, d = dx * dx + dz * dz;
        if (d <= bd && fabsf(drops[i].pos[1] - p->pos.y) <= 2)
            bd = d, best = i;
    }
    if (best < 0)
        return 0;
    printf("pickup: drop at %.1f %.1f %.1f picked up\n", drops[best].pos[0], drops[best].pos[1], drops[best].pos[2]);
    event_lot_give(drops[best].it, drops[best].n);  /* flags set now: a one-off drop does not come again */
    drop_remove(best);
    pickup_anim(p);
    pickup_stats.taken++;
    return 1;
}

/* ---- pickup anim ---- */
/* The pickup anim is not traced in the exe (TECH_DEBT item-2). 8000 (ObjActParam row 0, the generic ObjAct) is a lever
 * pull (Ame's playtest 2026-11-17). a00 7503, an event-layer anim of the player ESD (c1_3(3, 7503, ...)), is the one
 * c0000 anim that starts standing, crouches and reaches the right hand to the floor (R_Hand 0.18 m above the feet, the
 * left at 0.41, pelvis 0.49 at its lowest; 2.5 s): ASSUMED the item pickup */
#define PICKUP_ANIM 7503
void pickup_anim(Player *p)
{
    if (p && !player_event_anim_playing(p) && player_event_anim(p, PICKUP_ANIM) <= 0)
        printf("pickup: anim %d not in this build\n", PICKUP_ANIM);
    else if (p && model_anim(PICKUP_ANIM) < 0)  /* in player.gd but not loaded (model.c MAX_ANIMS): bind pose */
        printf("pickup: anim %d has no model clip (model.c drops anims past MAX_ANIMS)\n", PICKUP_ANIM);
}

/* ---- item get dialog (menu.drb F25: rows F25_item0 at the bottom, then up; each row's F25_text_line_02 holds
 * text_name, text_num and icon) ---- */
#define ROWS 4  /* F25_item0..3 */
/* How long the dialog stays: ASSUMED 3 s on screen from the last item added (DSR closes it on a button or after a while; the
 * timing is not decoded, TECH_DEBT item-3) */
#define DIALOG_T 3.0f
static struct { u32 item; int num; } rows[ROWS];
static int nrows, opened, detail;
static float dlg_t;

void pickup_got(u32 item, int num)
{
    for (int i = 0; i < nrows; i++)
        if (rows[i].item == item) {
            rows[i].num += num;
            dlg_t = DIALOG_T;
            return;
        }
    if (nrows == ROWS) {  /* a fifth item: the oldest row scrolls out (ASSUMED) */
        memmove(rows, rows + 1, (ROWS - 1) * sizeof rows[0]);
        nrows--;
    }
    rows[nrows].item = item, rows[nrows].num = num, nrows++;
    dlg_t = DIALOG_T;
    pickup_stats.dialog_rows = nrows;
}

static void dialog_close(void)
{
    if (opened && !menu_open() && !strcmp(drb_screen(), "F25")) {
        ui_req_dialog_close();
        if (detail)
            items_detail_free();
    }
    opened = detail = 0, nrows = 0, dlg_t = 0;
    pickup_stats.dialog_rows = 0;
}

void pickup_update(float dt)
{
    pickup_stats.drops = ndrops;
    pickup_stats.shines = fx_shine_count();
    if (!nrows)
        return;
    if (menu_open()) {  /* the Start menu took the screen (drb holds one at a time): the dialog is over */
        opened = detail = 0, nrows = 0, dlg_t = 0;
        pickup_stats.dialog_rows = 0;
        return;
    }
    if (opened && drb_ready() && detail && (dlg_t -= dt) <= 0) {  /* its time runs once it shows (the screen and
                                                                   * itemx.bin load from the disc first: ~1-4 s) */
        dialog_close();
        return;
    }
    if (!opened) {
        ui_req_dialog_open("F25", ROWS);
        opened = 1;
    }
    if (ui_req_dialog_step() < 0) {
        printf("pickup: item get dialog failed to load\n");
        dialog_close();
        return;
    }
    if (!detail)
        detail = items_detail_step(ui_alloc, ui_free) > 0;
}

static int f25_name, f25_num;  /* text nodes of the row being drawn */
static const char *row_text(int node, const char *tag, void *ctx)
{
    (void)tag;
    const int *k = ctx;
    static char buf[48];
    if (node == f25_name)
        return item_name(rows[*k].item, buf, sizeof buf);
    if (node == f25_num)
        return snprintf(buf, sizeof buf, "%d", rows[*k].num), buf;
    return NULL;
}

static Player *prompt_pl;
static int drop_near(const Player *p)
{
    for (int i = 0; i < ndrops; i++) {
        float dx = drops[i].pos[0] - p->pos.x, dz = drops[i].pos[2] - p->pos.z;
        if (dx * dx + dz * dz <= REACH * REACH && fabsf(drops[i].pos[1] - p->pos.y) <= 2)
            return 1;
    }
    return 0;
}

void pickup_prompt_player(Player *p) { prompt_pl = p; }

void pickup_draw(void)
{
    Player *p = prompt_pl;
    /* the action prompt (Event_text 10010103 "Pick up item", ASSUMED the treasure's one-line help: the exe's choice of
     * text is not traced), as the bonfire's: none while a dialog / menu / anim is up */
    if (p && p->hp > 0 && !nrows && !menu_open() && !player_event_anim_playing(p) &&
        (drop_near(p) || event_treasure_near((const float[3]){p->pos.x, p->pos.y, p->pos.z}))) {
        hud_begin();
        hud_prompt(320, 330, HUD_BTN_CROSS, hud_string(STR_PICKUP));
        hud_end();
    }
    if (!nrows || !opened || !drb_ready() || !detail || menu_open())
        return;
    int a = dlg_t < 0.25f ? (int)(0x80 * dlg_t / 0.25f) : 0x80;  /* fade out (ASSUMED) */
    hud_begin();
    for (int k = 0; k < nrows; k++) {
        char path[32];
        snprintf(path, sizeof path, "F25/F25-00/F25_item%d", k);
        int r = drb_find(-1, path);
        if (r < 0)
            continue;
        drb_show(drb_child(r, "F25_text_line_00"), 0);  /* the message-only line layouts */
        drb_show(drb_child(r, "F25_text_line_01"), 0);
        int line = drb_child(r, "F25_text_line_02");
        f25_name = drb_child(line, "text_name"), f25_num = drb_child(line, "text_num");
        drb_draw(r, 0, 0, a, row_text, &k);
        const ItemRow *ir = item_row(rows[k].item);
        int ic = drb_child(line, "icon");
        static int logged;
        int shown = 0;
        if (ir && ic >= 0) {
            const DrbNode *n = drb_node(ic);
            shown = drb_icon(ir->icon, n->x0, n->y0, n->x1 - n->x0, n->y1 - n->y0, a);
        }
        if (logged != (int)rows[k].item + shown) {  /* once per item and state */
            logged = (int)rows[k].item + shown;
            printf("pickup: dialog row %d item %08x icon %d node %d %s\n", k, (unsigned)rows[k].item, ir ? ir->icon : -1,
                   ic, shown ? "drawn" : "not drawn yet");
        }
    }
    hud_end();
}

/* ---- boot / tests ---- */
void pickup_init(GSGLOBAL *gs, void *blob, unsigned size)
{
    if (blob && !fx_shine_init(gs, blob, size))
        printf("pickup: no shine (VRAM / RAM), items still picked up\n");
    free(blob);
}

void pickup_test_cmd(const char *cmd)
{
    int npc, n;
    float x, y, z;
    if (sscanf(cmd, "drop %d %f %f %f", &npc, &x, &y, &z) == 4) {
        pickup_enemy_drop(npc, (const float[3]){x, y, z});
    } else if (sscanf(cmd, "droplot %d %f %f %f", &npc, &x, &y, &z) == 4) {  /* a drop of lot npc (any exported lot) */
        drop_lot(-1, npc, (const float[3]){x, y, z});
    } else if (sscanf(cmd, "droplot %d here", &npc) == 1 && prompt_pl) {  /* ... 0.5 m in front of the player */
        const Player *p = prompt_pl;
        drop_lot(-1, npc, (const float[3]){p->pos.x + 0.5f * sinf(p->yaw), p->pos.y, p->pos.z + 0.5f * cosf(p->yaw)});
    } else if (sscanf(cmd, "expect drops %d", &n) == 1) {
        printf("TEST pickup expect drops %d: %s (%d)\n", n, ndrops == n ? "PASS" : "FAIL", ndrops);
    } else if (sscanf(cmd, "expect dialog %d", &n) == 1) {
        printf("TEST pickup expect dialog %d: %s (%d)\n", n, nrows == n ? "PASS" : "FAIL", nrows);
    } else if (sscanf(cmd, "expect shines %d", &n) == 1) {
        int s = fx_shine_count();
        printf("TEST pickup expect shines %d: %s (%d)\n", n, s == n ? "PASS" : "FAIL", s);
    } else if (!strcmp(cmd, "summary")) {
        printf("TEST pickup summary: drops %d, shines %d, dialog rows %d, taken %d, rolled %d\n", ndrops,
               fx_shine_count(), nrows, pickup_stats.taken, pickup_stats.rolled);
    } else {
        printf("TEST pickup: unknown command '%s'\n", cmd);
    }
}
