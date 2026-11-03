/* DSR menu screens from menu.drb (tools/export_screens.py scr_<name>.bin in menu.pak): one screen at a time, its
 * dialog tree at DSR's own layout (640x448), its sprite atlas and an item icon cache in VRAM lent by the map texture
 * pool while it is open (texcache.c tc_pin), its memory from the near-cell arena (mapstream_alloc). menu.c drives the
 * screens; this file loads, uploads and draws them. Text uses the HUD font (hud.c). */
#ifndef DRB_H
#define DRB_H

#include <tamtypes.h>

enum { DRB_NULL, DRB_DIALOG, DRB_SPRITE, DRB_RECT, DRB_TEXT };
/* text alignment bits (DRB Text +30) */
#define DRB_RIGHT 1
#define DRB_CENTER 2
#define DRB_BOTTOM 4
#define DRB_MIDDLE 8

typedef struct {
    s16 x0, y0, x1, y1;  /* 640x448 */
    u8 type, align;
    u16 end;             /* index after its subtree */
    s16 spr;             /* sprite (DRB_SPRITE) or string (DRB_TEXT), -1 none */
    u8 font, flags;      /* flags 1: hidden by default (a template the code places, or a "bk_" backup) */
    u8 r, g, b, a;       /* a 0..0x80 */
    u32 hash;
} DrbNode;

/* Memory for menus: the near-cell arena when it exists (far map cells give way), else the heap */
void ui_set_pos(const float pos[3]);
void *ui_alloc(unsigned n);
void ui_free(void *p, unsigned n);

/* Start loading screen `name` ("I02"); icons: item icon cache cells (0 none). Closes the open one. */
void drb_open(const char *name, int icons);
/* Once a frame in the update phase (before map_draw): load, borrow VRAM, upload. 1 when the screen can be drawn,
 * 0 still loading, -1 failed (logged). */
int drb_step(void);
void drb_close(void);
int drb_ready(void);
const char *drb_screen(void);  /* name of the open screen, "" none */

/* Node by path of element names under root (-1: the screen's root dialogs, path starting with the root's name), e.g.
 * drb_find(-1, "I02/I02-00/button0_0"); -1 if missing */
int drb_find(int root, const char *path);
int drb_child(int parent, const char *name);  /* direct or nested: the first node of that name in parent's subtree */
const DrbNode *drb_node(int i);
const char *drb_string(int s);
/* Runtime visibility: 1 show (also a default-hidden template), 0 hide, -1 back to its default */
void drb_show(int node, int on);

/* Text of a <?tag?> element: return NULL to leave it empty */
typedef const char *(*DrbTextFn)(int node, const char *tag, void *ctx);
/* Draw node's subtree moved by dx, dy (alpha 0x80 = as exported). Default-hidden nodes are skipped unless shown, the
 * node itself is drawn even when hidden (draw a template at a place: dx, dy = place - template origin). */
void drb_draw(int node, float dx, float dy, int alpha, DrbTextFn fn, void *ctx);
/* Move node (a cursor template) so its centre sits on node at's, and show it; it is then drawn in place by drb_draw in
 * the DRB's order (under the texts that follow it). at < 0 hides it. Kept until changed or the screen closes. */
void drb_place(int node, int at);
/* One node's sprite / rect stretched over x, y, w, h */
void drb_draw_at(int node, float x, float y, float w, float h, int alpha);
/* Text in a node's box with its colour / alignment (or override r, g, b >= 0) */
void drb_text(int node, float dx, float dy, const char *s, int alpha);
/* Item icon (EquipParam*.iconId) over x, y, w, h from the cache: requested in the background if missing (drawn once
 * it arrived); 0 when not drawn yet */
int drb_icon(int icon, float x, float y, float w, float h, int alpha);

/* Button names for the key guide tags (<?conclusion?> ...) */
const char *drb_button_tag(const char *tag);

typedef struct { int vram_kb, ram_kb, icons_loaded, icon_misses; } DrbStats;
extern DrbStats drb_stats;

#endif
