/* lab_stubs.c: what the Lab's simulate modules call outside the simulate phase (Polaris 1.7b). Each stub counts its
 * calls ("stubs" command): a simulate module reaching a render, sound, streaming or UI call is a layer violation of
 * the Polaris 0.4 boundary map. Stubs with a behaviour of their own say so. */
#include "mapfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lab.h"
#include "player.h"
#include "pack.h"
#include "render.h"
#include "vu1.h"
#include "frame.h"
#include "fx.h"
#include "chrlight.h"
#include "shadow.h"
#include "hitch.h"
#include "map.h"
#include "enemy.h"
#include "objects.h"
#include "testscript.h"

FramePhase frame_phase;
/* ticks.h in the Lab: 1 ms of EE ticks per read. Deterministic (the same calls give the same times on every run), and
 * a loop with a time cap (world.c settle: 30 s) ends as on the console instead of spinning for good */
unsigned lab_ticks(void)
{
    static unsigned t;
    return t += 294912;
}

/* events, bonfire, enemies, world (1.7c brings events and enemies in) */
void loading_hold(void) { LAB_COUNT("loading_hold"); }
void snd_anim(unsigned owner, int model, int anim, float t, const float pos[3])
{
    (void)owner, (void)model, (void)anim, (void)t, (void)pos, LAB_COUNT("snd_anim (sound from simulate)");
}
/* map: the navmesh floor is the fallback where collision has none; the Lab has collision only (counted when asked) */
int map_floor(float x, float z, float y, float *out_y) { (void)x, (void)z, (void)y, (void)out_y, LAB_READBACK("map_floor (navmesh floor fallback)"); return 0; }
/* the area's spawn point, from its map.bin header as map.c reads it (the first two fields of map_i.h's Header) */
void map_spawn(V3 *pos, float *yaw)
{
    static float kept[4];
    static int have;
    if (!have) {
        Pack pk;
        char path[128];
        snprintf(path, sizeof path, PACK_ROOT "%s.pak", lab_area);
        unsigned n = 0;
        void *blob = pack_open(&pk, path) ? pack_load(&pk, "map.bin", &n) : NULL;
        pack_close(&pk);
        const unsigned *h = mapf_payload(blob, n, "map.bin", MAPF_MAP, NULL);  /* (a chunk file: src/mapfile.h) */
        if (!h) {
            free(blob);
            return (void)LAB_READBACK("map_spawn (no map.bin)");
        }
        memcpy(kept, h + 5, sizeof kept);  /* (magic, chunks, textures, nav_verts, nav_tris, then spawn[3], spawn_yaw) */
        free(blob);
        have = 1;
    }
    *pos = v3(kept[0], kept[1], kept[2]), *yaw = kept[3];
}void map_char_light(V3 pos, float amb[3], float sun[3], float dir[3])
{
    (void)pos, LAB_COUNT("map_char_light (render from simulate)");
    for (int i = 0; i < 3; i++)
        amb[i] = sun[i] = dir[i] = 0;
}
/* shadow.c (not built here): render only */
void shadow_add(V3 feet, float r, float fade) { (void)feet, (void)r, (void)fade; }
int shadow_sil_begin(V3 feet) { (void)feet; return 0; }  /* 0: no silhouette pass in the Lab */
void shadow_sil_end(void) {}
/* chrlight.c (not built here): the old path's single call, so the count stays */
void chrl_pick(V3 pos, float amb[3], float sun[3], float dir[3], int *np, float pdir[2][3], float pcol[2][3])
{
    (void)pdir, (void)pcol, *np = 0;
    map_char_light(pos, amb, sun, dir);
}
void map_char_spec(float env_up[3], float env_down[3], float spec_i[3], float spec_dir[3])
{
    LAB_COUNT("map_char_spec (render from simulate)");
    for (int i = 0; i < 3; i++)
        env_up[i] = env_down[i] = spec_i[i] = spec_dir[i] = 0;
}
/* memory: the streamer's arena and the regions are the heap here */
void *mapstream_alloc(unsigned n, const float pos[3]) { (void)pos; return malloc(n); }
void *mapstream_alloc_pool_small(unsigned n, int pool) { (void)pool; return malloc(n); }
void mapstream_free(void *p, unsigned n) { (void)n, free(p); }
unsigned mapstream_heap_release(unsigned bytes) { (void)bytes; return 0; }
int mapstream_owns(const void *p) { (void)p; return 1; }  /* (so callers free through mapstream_free) */
int mapstream_in_items(const void *p) { (void)p; return 0; }
int mapstream_ready(void) { return 1; }
void mem_release(void *p) { free(p); }
/* paged animation reads (model.c): synchronous here */
static int last_res;
int pack_req_read(Pack *p, unsigned offset, void *dst, unsigned bytes)
{
    extern int lab_pack_read(Pack *p, unsigned offset, void *dst, unsigned bytes);
    last_res = lab_pack_read(p, offset, dst, bytes);
    return 1;
}
int pack_req_read_first(Pack *p, unsigned offset, void *dst, unsigned bytes) { return pack_req_read(p, offset, dst, bytes); }
int pack_req_done(int t, int *result) { (void)t, *result = last_res; return 1; }
void pack_req_cancel(int t) { (void)t; }
int pack_read_async(Pack *p, unsigned offset, void *dst, unsigned bytes) { return pack_req_read(p, offset, dst, bytes); }
int pack_async_done(int *result) { *result = last_res; return 1; }
void pack_async_wait(void) {}
/* background entry loads (items.c's itemx.bin, enemy chr.bin): loaded at once here, done at the first step */
void pack_job_start(PackJob *j, const char *path, const char *name, void *(*get)(unsigned), void (*put)(void *, unsigned))
{
    Pack p;
    unsigned raw = 0;
    j->out = NULL, j->raw = 0, j->state = -1;
    if (pack_open(&p, path)) {
        j->out = pack_load_with(&p, name, &raw, get, put);
        j->raw = raw, j->alloc = (raw + 127) & ~127u, j->state = j->out ? 1 : -1;
        pack_close(&p);
    }
}
int pack_job_step(PackJob *j) { return j->state; }
void pack_job_cancel(PackJob *j) { (void)j; }
/* render and VU1: nothing to draw */
int r_vram_avail(const GSGLOBAL *gs) { (void)gs; return 4 << 20; }
u32 r_vram_tex(GSGLOBAL *gs, int w, int h, int psm, GsOwner o) { (void)gs, (void)w, (void)h, (void)psm, (void)o; return 256; }
u32 r_vram_clut(GSGLOBAL *gs, int clut_psm, GsOwner o) { (void)gs, (void)clut_psm, (void)o; return 256; }
void r_vram_free_clut(u32 vram, int clut_psm) { (void)vram, (void)clut_psm; }
void r_vram_free_tex(u32 vram, int w, int h, int psm) { (void)vram, (void)w, (void)h, (void)psm; }
void g2d_tex_upload(GSGLOBAL *gs, GSTEXTURE *t) { (void)gs, (void)t; }
void dmaKit_wait_fast(void) {}
void vu1_skin_add_pkt(GSTEXTURE *t, int st, int cull, int tris, const float *tint, const void *pa, const void *pb,
                      const void *nrm, const void *st_or_col, int spec_k, const void *ms, const void *pkt, int pkt_bytes)
{
    (void)t, (void)st, (void)cull, (void)tris, (void)tint, (void)pa, (void)pb, (void)nrm, (void)st_or_col, (void)spec_k,
        (void)ms, (void)pkt, (void)pkt_bytes;
}
void vu1_skin_begin(const void *palette, int bones, const unsigned char *win, int nwin) { (void)palette, (void)bones, (void)win, (void)nwin; }
void vu1_skin_light(const float amb[3], const float sun[3]) { (void)amb, (void)sun; }
void vu1_skin_lights(const float sun_dir[3], int np, const float (*dir)[3], const float (*col)[3])
{
    (void)sun_dir, (void)np, (void)dir, (void)col;
}
/* the skin streams' layout as vu1.c builds it (model.c fills the streams; the VIF codes are left zero here) */
int vu1_skin_packet(void *buf, int tris, int textured, int ms, void *streams[5])
{
    int n = tris * 3, q16 = (n * 8 + 15) / 16, q8 = (n * 4 + 15) / 16, at = 0;
    u32 *w = buf;
    for (int s = 0; s < 5; s++) {
        if (s == 3 && !ms) {
            if (streams)
                streams[3] = NULL;
            continue;
        }
        if (w && streams)
            streams[s] = w + at * 4 + 4;
        at += 1 + (s == 3 || (s == 4 && !textured) ? q8 : q16);
    }
    return at * 16;
}
void vu1_skin_spec(const float env_up[3], const float env_down[3], const float spec_i[3], const float ls[3])
{
    (void)env_up, (void)env_down, (void)spec_i, (void)ls;
}
void vu1_skin_window(const void *palette, const unsigned char *win, int n) { (void)palette, (void)win, (void)n; }
void vu1_skin_tint(const float tex[3], const float flat[3]) { (void)tex, (void)flat; }
u32 vu1_skin_hdr(void *buf, GSTEXTURE *t, int st, int cull, int tris, int spec_k, int ms)
{
    (void)buf, (void)t, (void)st, (void)cull, (void)tris, (void)spec_k, (void)ms;
    return 0;
}
void *vu1_skin_seg_batch(void *at, const void *hdr, const void *pkt, int pkt_bytes, u32 prev_entry)
{
    (void)hdr, (void)pkt, (void)pkt_bytes, (void)prev_entry;
    return (char *)at + 32;
}
void *vu1_skin_seg_end(void *at, u32 entry) { (void)entry; return (char *)at + 16; }
int vu1_skin_can_call(int textured) { (void)textured; return 0; }
void vu1_skin_call(const void *seg, unsigned last_cbp) { (void)seg, (void)last_cbp; }

/* ---- the test-script runner (Polaris 1.7c): what testscript.c reaches outside the player ---- */
void lab_unsupported(const char *cmd, const char *needs)
{
    printf("TEST error line %d: '%s' needs %s (not in the Lab)\n", (int)test_mb.line, cmd, needs);
    lab_count(cmd, 1);
}
void loading_show(float s) { (void)s, LAB_COUNT("loading_show"); }
/* determinism options: dt and seed are the runner's; the recorder is the console's (replay its traces with "replay") */
float frame_det_dt;
unsigned frame_det_seed;
int frame_det_seeded;
void frame_det_record(const char *path) { (void)path, LAB_COUNT("frame_det_record (option record)"); }
int frame_det_buffers(void) { return 1; }
/* the stats testscript.c prints per shot: zero here (nothing is drawn or streamed) */
FxStats fx_stats;
Vu1Stats vu1_stats;
unsigned hitch_acc[HS_N];
int map_vu1_batches, map_ee_tris;
char _stack_size[1];
int pack_cdrom;
int pack_disc_stats(void *out) { (void)out; return 0; }
void pack_disc_poll(void) {}
void trail_tick(float dt) { (void)dt; }
void trail_sample(const float base[3], const float tip[3]) { (void)base, (void)tip; }
