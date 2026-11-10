/* DS1 HUD from DSR's own assets (menu.bin, tools/export_menu.py): HP / stamina bars, humanity counter, quick slots,
 * soul counter, YOU DIED banner (while hp < 0); text in DSFont24. Drawn through gsKit after the 3D scene. */
#ifndef HUD_H
#define HUD_H

#include <gsKit.h>
#include "player.h"

/* menu.bin layout entries, in tools/export_menu.py's LAYOUT order + its two menu pieces (640x448 pixels) */
enum {
    HUD_BAR_BG_HP, HUD_BAR_BG_SP, HUD_PLATE_HP, HUD_PLATE_SP, HUD_FILL_HP, HUD_FILL_SP, HUD_FRAME_HP, HUD_FRAME_SP,
    HUD_END_HP, HUD_END_SP, HUD_BAR_START, HUD_BAR_START1, HUD_SLOT_L_PLATE, HUD_SLOT_L_ICON, HUD_SLOT_R_PLATE,
    HUD_SLOT_R_ICON, HUD_SLOT_I_FRAME, HUD_SLOT_I_PLATE, HUD_SLOT_I_ICON, HUD_SLOT_I_COUNT, HUD_ITEM_NAME,
    HUD_SOUL_PLATE, HUD_SOUL_LINE, HUD_SOUL_TEXT, HUD_DIED_BG, HUD_DIED_GRAD0, HUD_DIED_GRAD1, HUD_DIED_BAR,
    HUD_DIED_TEXT, HUD_LIT_BG, HUD_LIT_GRAD0, HUD_LIT_GRAD1, HUD_LIT_BAR, HUD_LIT_TEXT, HUD_REV_TEXT,
    HUD_HUM_BG, HUD_HUM_RING1, HUD_HUM_RING0,
    HUD_HUM_NUM,                   /* + d: units digit d (humanNum_0d); + 10 + d: tens digit d (humanNum_1d) */
    HUD_HUM_STONE = HUD_HUM_NUM + 20, HUD_MENU_PANEL, HUD_MENU_RULE,
    HUD_LAYOUT_COUNT
};
/* menu.bin strings: the Warrior's kit (CharaInitParam 3000 + Estus), then menu words from the FMGs */
enum {
    STR_WEP_R, STR_WEP_L, STR_HELM, STR_ARMOR, STR_GAUNT, STR_LEG, STR_ESTUS, STR_DARKSIGN, STR_CRYSTAL,
    STR_INVENTORY, STR_EQUIPMENT, STR_STATUS, STR_WEAPONS, STR_ARMOR_WORD, STR_LEVEL, STR_HP, STR_STAMINA, STR_SOULS,
    /* bonfire (Event_text_.fmg): prompts, menu entries in AddTalkListData order, dialogs */
    STR_BF_LIGHT, STR_BF_REST, STR_BF_LEVEL_UP, STR_BF_REINFORCE_WEP, STR_BF_REINFORCE_ARMOR, STR_BF_REPAIR,
    STR_BF_ATTUNE, STR_BF_BOX, STR_BF_WARP, STR_BF_COVENANT, STR_BF_KINDLE, STR_BF_REVERSE_HOLLOWING, STR_BF_LEAVE,
    STR_BF_ASK_KINDLE, STR_BF_NO_HUMANITY, STR_BF_KINDLE_MAX, STR_BF_KINDLE_RITE, STR_BF_KINDLE_HOLLOW,
    STR_BF_NOT_HOLLOW, STR_BF_NO_HUMANITY2, STR_BF_ASK_REVERSE,
    STR_COUNT
};
#define KIT_COUNT 9  /* icons: one per STR_WEP_R..STR_CRYSTAL */

typedef struct { int sprite; short x, y, w, h; unsigned char r, g, b, a; } HudRect;

/* Parse menu.bin and upload its two atlases (194 KB VRAM); 0 on success, -1 bad data / VRAM full */
int hud_init(GSGLOBAL *gs, const void *menu_bin);
/* In-game HUD for the player; souls / Estus charges from the progression state (progress.h) */
void hud_draw(const Player *p, float dt);

/* EMEVD 2003[11] Display Boss Health Bar: slot 0 / 1 shows the HP of character `entity` (MSB entity id) under the
 * NpcParam name id `name` (FMG text is A11's: the id is drawn for now); entity < 0 hides that slot, slot < 0 every slot
 * (the exe's "off" takes no slot: FUN_1407d0970 case 11) */
void hud_boss_bar(int slot, int entity, int name);
int hud_boss_bar_entity(int slot);  /* -1 none (tests) */

/* Helpers for menu.c / drb.c */
GSGLOBAL *hud_gs(void);
const char *hud_string(int s);
/* sprite (from a layout entry or hud_icon) at x, y, w x h; colour 0x80 = 1.0, a 0x80 = opaque */
void hud_sprite(int sprite, float x, float y, float w, float h, int r, int g, int b, int a);
void hud_fill(float x, float y, float w, float h, int r, int g, int b, int a);
/* DRB GouraudRect: c = r, g, b, a (a 0..0x80) of the top left, top right, bottom right, bottom left corners; a scales */
void hud_gradient(float x, float y, float w, float h, const unsigned char c[16], int a);
int hud_icon(int kit);
void hud_layout(int layout, int a);  /* a layout entry (HUD_*) at its place, alpha a (0x80 = as exported) */
/* DSFont24 text; returns the width. align: 0 left, 1 right of x */
float hud_text(float x, float y, const char *s, int r, int g, int b, int align);
float hud_text_width(const char *s);
/* hud_text with alpha (0..0x80) and glyph scales sx, sy (1 = as drawn) */
float hud_text_ex(float x, float y, const char *s, int r, int g, int b, int a, int align, float sx, float sy);
int hud_line_height(void);
/* Begin / end a batch of HUD draws: alpha blending on, alpha and depth tests off (and back) */
void hud_begin(void);
void hud_end(void);

#endif
