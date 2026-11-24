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
#include "lpass.h"
#include "drawgrp.h"
#include "collision.h"
#include "map.h"
#include "pack.h"

/* ---- the map's instances (mapblock.c), as far as the draw groups need them (Polaris 1.7c-4) ----
 * drawgrp.c runs as on the console (dg_player, dg_mask_shown, dg_entity): each block's dg.bin is set up when the map
 * turns the block on, with the chunk count of its maptab header (the first two words of map_i.h's Header) and the
 * triangles of its collision, as mapblock.c map_block_init does; instance k = world block k = collision block k. */
static int inst_on[MAP_INST], inst_col[MAP_INST];
static unsigned inst_chunks[MAP_INST];
unsigned map_chunk_count(void) { return inst_chunks[0]; }
static void *blk_dg[MAP_INST];
static unsigned blk_dg_n[MAP_INST];
int mp_inst_col_block(int k) { return k == 0 ? inst_col[0] : k > 0 && k < MAP_INST && inst_on[k] ? inst_col[k] : -1; }
int map_inst_of(int col_block)
{
    for (int k = 0; k < MAP_INST; k++)
        if ((k == 0 || inst_on[k]) && inst_col[k] == col_block)
            return k;
    return -1;
}
int map_block_init(int k, const void *tab, const void *res, int col_block)
{
    const unsigned *h = tab;
    (void)res;
    if (k < 0 || k >= MAP_INST || !h || memcmp(h, "DSM5", 4))
        return 0;
    if (k)
        dg_block_init(k, blk_dg[k], blk_dg_n[k], h[1], (unsigned)col_block_tris(col_block));
    inst_on[k] = 1, inst_col[k] = col_block, inst_chunks[k] = h[1];
    return 1;
}
/* mapblock.c: map_block_lean turns the instance off and keeps its draw groups; map_block_drop forgets them, leaned
 * before or not (their dg.bin goes with the block's lite set: PR #274) */
void map_block_drop(int k)
{
    if (k <= 0 || k >= MAP_INST)
        return;
    dg_block_drop(k);
    inst_on[k] = 0, blk_dg[k] = NULL, blk_dg_n[k] = 0;
}
void map_block_lean(int k, int keep_hz) { (void)keep_hz; if (k > 0 && k < MAP_INST) inst_on[k] = 0; }
void map_block_dg_set(int k, const void *dg, unsigned dg_size, const void *pvs, unsigned pvs_size)
{
    (void)pvs, (void)pvs_size;
    if (k > 0 && k < MAP_INST)
        blk_dg[k] = (void *)dg, blk_dg_n[k] = dg_size;
}
void map_block_dg(int k, void *pack)
{
    if (k > 0 && k < MAP_INST && pack && !blk_dg[k])
        blk_dg[k] = pack_load(pack, "dg.bin", &blk_dg_n[k]);
}

/* ---- the map (render) and its streamer (memory: the host heap) ---- */
void map_block_bump(int k, const void *blob) { (void)k, (void)blob; }
void map_block_shared(int k, const void *buf) { (void)k, (void)buf; }
void map_block_split(int k, const void *b, unsigned b_off, const void *c, unsigned c_off) { (void)k, (void)b, (void)b_off, (void)c, (void)c_off; }
int map_block_stream_open(int k, const void *pack) { (void)k, (void)pack; return 1; }
void map_boot_lean(void) {}
int map_far_set(const void *blob) { (void)blob; return 1; }
MapStreamStats mapstream_stats;
void *mapstream_alloc_pool(unsigned n, int pool) { (void)pool; return malloc(n); }
void *mapstream_alloc_soft(unsigned n) { return malloc(n); }
int mapstream_arena(void) { return 1; }
/* A warp's settle (world.c) loops while the streamer still reads the near cells at the new spot, which on the console
 * is never none: the first check after mapstream_settle(1) says busy, so the settle takes the steps the console's does
 * at least once (a step while warping is when kept-only blocks lean and go: tests/ev_chr.txt's far warp turns the Burg
 * west block off on the console). Afterwards nothing is ever still reading. */
static int settle_busy;
int mapstream_busy(const float pos[3], float r, float r_focus)
{
    (void)pos, (void)r, (void)r_focus;
    return settle_busy > 0 ? settle_busy-- : 0;
}
void mapstream_focus(int k) { (void)k; }
void mapstream_hold(int on) { (void)on; }
void mapstream_sources(const float *pos, int n) { (void)pos, (void)n; }
unsigned mapstream_lend_early(void) { return 0; }
void mapstream_pool(unsigned max) { (void)max; }
int mapstream_pool_prepare(int pool, unsigned n) { (void)pool, (void)n; return 1; }
void mapstream_pools(unsigned struct_bytes, unsigned misc_bytes) { (void)struct_bytes, (void)misc_bytes; }
int mapstream_pools_on(void) { return 0; }  /* (no pools: lite sets come from mapstream_alloc, the heap) */
void mapstream_pool_need(unsigned bytes) { (void)bytes; }
int mapstream_pools_on_build(void) { return 0; }
int mapstream_pool_wait(void) { return 0; }
int mapstream_reading(int k) { (void)k; return 0; }
void mapstream_reserve(unsigned extra) { (void)extra; }
void mapstream_settle(int on) { settle_busy = on ? 1 : 0; }
void mapstream_update(const float pos[3]) { (void)pos; }
void pack_pump(void) {}
void hitch_block_read(unsigned bytes) { (void)bytes; }
void vu1_wait(void) {}
void ot_wait(void) {}
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


/* ---- what enemy.c / ai.c / npc.c reach outside simulate (Polaris 1.7c-3) ---- */
unsigned lpass_t[LG_N];
void *mapstream_alloc_gentle(unsigned n) { return malloc(n); }
char pack_error[96];
void snd_chr_load(const char *model) { (void)model; }
void snd_chr_unload(const char *model) { (void)model; }
void vu1_skin_fade(float fmax) { (void)fmax; }
