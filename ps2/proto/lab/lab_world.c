/* lab_world.c: what the Lab's static world (Polaris 1.7c-2: world.c's blocks, collision, objects, events, the
 * bonfire, world items) reaches outside the simulate phase. The map's drawing, the streamer's pools, the UI, effects,
 * sound and the HUD are stubs here; enemies come in 1.7c-3. Each stub is one of lab.h's two kinds: presentation
 * (LAB_COUNT, expected) or read-back (LAB_READBACK, a result the simulation depends on). The streamer's memory is the
 * host heap, and every read completes at once (lab_stubs.c pack_req_*): what world.c wants is in before simulate. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lab.h"
#include "player.h"
#include "render.h"
#include "drb.h"
#include "mapstream.h"
#include "ev_chr.h"

/* ---- the map (render) and its streamer (memory: the host heap) ---- */
int dg_entity(int id, int on) { (void)id, (void)on, LAB_COUNT("dg_entity (map parts shown / hidden)"); return 1; }
int dg_entity_drawn(int id) { (void)id, LAB_COUNT("dg_entity_drawn (test query)"); return -1; }
void map_block_bump(int k, const void *blob) { (void)k, (void)blob; }
void map_block_dg(int k, void *pack) { (void)k, (void)pack; }
void map_block_dg_set(int k, const void *dg, const void *pvs) { (void)k, (void)dg, (void)pvs; }
void map_block_drop(int k) { (void)k; }
int map_block_init(int k, const void *tab, const void *res, int col_block) { (void)k, (void)tab, (void)res, (void)col_block; return 1; }
void map_block_lean(int k, int keep_hz) { (void)k, (void)keep_hz; }
void map_block_shared(int k, const void *buf) { (void)k, (void)buf; }
void map_block_split(int k, const void *b, unsigned b_off, const void *c, unsigned c_off) { (void)k, (void)b, (void)b_off, (void)c, (void)c_off; }
int map_block_stream_open(int k, const void *pack) { (void)k, (void)pack; return 1; }
void map_boot_lean(void) {}
int map_far_set(const void *blob) { (void)blob; return 1; }
MapStreamStats mapstream_stats;
void *mapstream_alloc_pool(unsigned n, int pool) { (void)pool; return malloc(n); }
void *mapstream_alloc_soft(unsigned n) { return malloc(n); }
int mapstream_arena(void) { return 1; }
int mapstream_busy(const float pos[3], float r, float r_focus) { (void)pos, (void)r, (void)r_focus; return 0; }
void mapstream_focus(int k) { (void)k; }
void mapstream_hold(int on) { (void)on; }
unsigned mapstream_lend_early(void) { return 0; }
void mapstream_pool(unsigned max) { (void)max; }
int mapstream_pool_prepare(int pool, unsigned n) { (void)pool, (void)n; return 1; }
void mapstream_pools(unsigned struct_bytes, unsigned misc_bytes) { (void)struct_bytes, (void)misc_bytes; }
int mapstream_pools_on(void) { return 0; }  /* (no pools: lite sets come from mapstream_alloc, the heap) */
int mapstream_pools_on_build(void) { return 0; }
int mapstream_pool_wait(void) { return 0; }
int mapstream_reading(int k) { (void)k; return 0; }
void mapstream_reserve(unsigned extra) { (void)extra; }
void mapstream_settle(int on) { (void)on; }
void mapstream_update(const float pos[3]) { (void)pos; }
void pack_pump(void) {}
void hitch_block_read(unsigned bytes) { (void)bytes; }
void vu1_wait(void) {}
M3 m3_rot_y(float a) { float c = cosf(a), s = sinf(a); M3 r = {{c, 0, s, 0, 1, 0, -s, 0, c}}; return r; }
void r_box(V3 pos, M3 rot, V3 c, V3 h, int r, int g, int b) { (void)pos, (void)rot, (void)c, (void)h, (void)r, (void)g, (void)b; }

/* ---- the loading screen, effects, sound ---- */
void loading_begin(void) { LAB_COUNT("loading_begin"); }
void loading_end(void) {}
void loading_tick(int can_read) { (void)can_read; }
int fx_block_load(GSGLOBAL *gs, int block, const void *blob, unsigned size) { (void)gs, (void)block, (void)blob, (void)size; return 1; }
void fx_block_on(int block, int on, const void *blob) { (void)block, (void)on, (void)blob; }
void fx_bonfire_burst(int entity, int n) { (void)entity, (void)n, LAB_COUNT("fx_bonfire_burst"); }
int fx_entity_particles(int id, int kill) { (void)id, (void)kill, LAB_COUNT("fx_entity_particles"); return 0; }
static int shines;
int fx_shine_add(int effect, const float pos[3]) { (void)effect, (void)pos; return ++shines; }
int fx_shine_count(void) { return shines; }  /* (the live count: pickup.c adds / removes them) */
void fx_shine_del(int h) { (void)h, shines -= shines > 0; }
int fx_shine_init(GSGLOBAL *gs, const void *blob, unsigned size) { (void)gs, (void)blob, (void)size; return 1; }
int snd_entity(int entity, int on) { (void)entity, (void)on, LAB_COUNT("snd_entity"); return 0; }
int snd_play(int type, int id, const float pos[3], unsigned owner) { (void)type, (void)id, (void)pos, (void)owner, LAB_COUNT("snd_play"); return 0; }

/* ---- UI: no menu, dialog screen or HUD in the Lab ---- */
void frame_ui_request(void) {}
void *ui_alloc(unsigned n) { return malloc(n); }
void ui_free(void *p, unsigned n) { (void)n, free(p); }
int menu_open(void) { return 0; }
/* the dialog screens (drb.c): one open at a time, ready at once (the screen's load is the only wait the Lab leaves
 * out); nothing is drawn, so finding nodes fails and the item dialog's rows show nothing */
static char drb_open_name[16];
int drb_child(int parent, const char *name) { (void)parent, (void)name; return -1; }
void drb_close(void) { drb_open_name[0] = 0; }
void drb_draw(int node, float dx, float dy, int alpha, DrbTextFn fn, void *ctx) { (void)node, (void)dx, (void)dy, (void)alpha, (void)fn, (void)ctx; }
int drb_find(int root, const char *path) { (void)root, (void)path; return -1; }
int drb_icon(int icon, float x, float y, float w, float h, int alpha) { (void)icon, (void)x, (void)y, (void)w, (void)h, (void)alpha; return 0; }
const DrbNode *drb_node(int i) { (void)i; return NULL; }
void drb_open(const char *screen, int rows)
{
    (void)rows, LAB_COUNT("drb_open (dialog screen: ready at once, nothing drawn)");
    snprintf(drb_open_name, sizeof drb_open_name, "%s", screen);
}
int drb_ready(void) { return drb_open_name[0] != 0; }
const char *drb_screen(void) { return drb_open_name; }
void drb_show(int node, int on) { (void)node, (void)on; }
int drb_step(void) { return drb_open_name[0] ? 1 : 0; }
void hud_begin(void) {}
void hud_end(void) {}
void hud_boss_bar(int slot, int entity, int name) { (void)slot, (void)entity, (void)name, LAB_COUNT("hud_boss_bar"); }
int hud_boss_bar_entity(int slot) { (void)slot; return -1; }
void hud_fill(float x, float y, float w, float h, int r, int g, int b, int a) { (void)x, (void)y, (void)w, (void)h, (void)r, (void)g, (void)b, (void)a; }
void hud_layout(int layout, int a) { (void)layout, (void)a; }
int hud_line_height(void) { return 16; }
void hud_prompt(float cx, float y, int btn, const char *text) { (void)cx, (void)y, (void)btn, (void)text; }
const char *hud_string(int s) { (void)s; return ""; }
float hud_text(float x, float y, const char *s, int r, int g, int b, int align) { (void)y, (void)s, (void)r, (void)g, (void)b, (void)align; return x; }
float hud_text_width(const char *s) { (void)s; return 0; }

