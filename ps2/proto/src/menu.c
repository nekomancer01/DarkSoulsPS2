#include <libpad.h>
#include <stdio.h>
#include "menu.h"
#include "hud.h"
#include "progress.h"

/* Rows: 6 equipment slots, then the inventory (Estus, Darksign, crystal). Counts: CharaInitParam 3000 itemNum + the
 * Estus Flask's charges. */
#define ROWS 9
static const int ROW_KIT[ROWS] = {STR_WEP_R, STR_WEP_L, STR_HELM, STR_ARMOR, STR_GAUNT, STR_LEG,
                                  STR_ESTUS, STR_DARKSIGN, STR_CRYSTAL};
static const int ROW_COUNT[ROWS] = {0, 0, 0, 0, 0, 0, 5, 1, 1};
#ifdef MENU_TEST  /* screenshot build (make EE_DBGINFOFLAGS=-DMENU_TEST): the menu starts open */
static int open = 1, cursor;
#else
static int open, cursor;
#endif

int menu_update(unsigned short pressed)
{
    if (pressed & PAD_START)
        open = !open;
    else if (open && (pressed & PAD_CIRCLE))
        open = 0;
    if (!open)
        return 0;
    if (pressed & PAD_UP)
        cursor = (cursor + ROWS - 1) % ROWS;
    if (pressed & PAD_DOWN)
        cursor = (cursor + 1) % ROWS;
    return 1;
}

void menu_draw(const Player *p)
{
    if (!open)
        return;
    hud_begin();
    const HudRect *pn = hud_rect(HUD_MENU_PANEL), *rule = hud_rect(HUD_MENU_RULE);
    hud_fill(0, 0, 640, 448, 0, 0, 0, 0x40);  /* dim the scene */
    hud_sprite(pn->sprite, pn->x, pn->y, pn->w, pn->h, 0x80, 0x80, 0x80, 0x78);
    int lh = hud_line_height();
    float x0 = pn->x + 24, x1 = pn->x + pn->w / 2 + 12, y = pn->y + 12;
    hud_text(x0, y, hud_string(STR_EQUIPMENT), 0x90, 0x84, 0x60, 0);
    hud_text(x1, y, hud_string(STR_INVENTORY), 0x90, 0x84, 0x60, 0);
    hud_sprite(rule->sprite, pn->x + 12, y + lh + 2, pn->w - 24, rule->h, 0x80, 0x80, 0x80, 0x80);
    float row_h = 34, top = y + lh + 12;
    char buf[48];
    for (int i = 0; i < ROWS; i++) {
        int inv = i >= 6;
        float x = inv ? x1 : x0, ry = top + (inv ? i - 6 : i) * row_h;
        if (i == cursor)
            hud_fill(x - 4, ry - 1, pn->w / 2 - 32, row_h - 2, 0x60, 0x50, 0x30, 0x40);
        hud_sprite(hud_icon(ROW_KIT[i]), x, ry, 30, 30, 0x80, 0x80, 0x80, 0x80);
        int c = i == cursor ? 0x80 : 0x68;
        hud_text(x + 36, ry + (30 - lh) / 2, hud_string(ROW_KIT[i]), c, c, c, 0);
        if (ROW_COUNT[i]) {
            snprintf(buf, sizeof buf, "%d", ROW_COUNT[i]);
            hud_text(x + pn->w / 2 - 40, ry + (30 - lh) / 2, buf, c, c, c, 1);
        }
    }
    /* status block under the inventory: CharaInitParam 3000 soul level, live HP / stamina, souls */
    float sy = top + 4 * row_h;
    hud_text(x1, sy, hud_string(STR_STATUS), 0x90, 0x84, 0x60, 0);
    hud_sprite(rule->sprite, x1 - 4, sy + lh + 2, pn->w / 2 - 32, rule->h, 0x80, 0x80, 0x80, 0x80);
    struct { int s; int a, b; } st[4] = {{STR_LEVEL, prog.level, -1}, {STR_HP, p->hp > 0 ? p->hp : 0, p->hp_max},
                                         {STR_STAMINA, p->stamina, p->stamina_max}, {STR_SOULS, prog.souls, -1}};  /* progress.c (A3) */
    for (int i = 0; i < 4; i++) {
        float ry = sy + lh + 10 + i * (lh + 4);
        hud_text(x1, ry, hud_string(st[i].s), 0x70, 0x70, 0x70, 0);
        if (st[i].b >= 0)
            snprintf(buf, sizeof buf, "%d / %d", st[i].a, st[i].b);
        else
            snprintf(buf, sizeof buf, "%d", st[i].a);
        hud_text(x1 + pn->w / 2 - 40, ry, buf, 0x80, 0x80, 0x80, 1);
    }
    hud_end();
}
