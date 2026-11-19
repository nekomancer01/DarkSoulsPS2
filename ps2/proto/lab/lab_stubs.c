/* lab_stubs.c: what the Lab's simulate modules call outside the simulate phase (Polaris 1.7b). Each stub counts its
 * calls ("stubs" command): a simulate module reaching a render, sound, streaming or UI call is a layer violation of
 * the Polaris 0.4 boundary map. Stubs with a behaviour of their own say so. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lab.h"
#include "player.h"
#include "pack.h"
#include "render.h"
#include "vu1.h"
#include "frame.h"

FramePhase frame_phase;
int test_god;
float enemy_player_push[2];

/* events, bonfire, enemies, world (1.7c brings events and enemies in) */
void bonfire_death(const Player *p) { (void)p, LAB_COUNT("bonfire_death"); }
void bonfire_revive(Player *p) { (void)p, LAB_COUNT("bonfire_revive"); }
int bonfire_spawn(V3 *pos, float *yaw) { (void)pos, (void)yaw, LAB_COUNT("bonfire_spawn"); return 0; }
int event_push_player(float pos[3], float r, float h) { (void)pos, (void)r, (void)h; return 0; }  /* fog walls: none */
int world_current(void) { return -1; }
int world_hold(const float *feet) { (void)feet; return 0; }
void loading_hold(void) { LAB_COUNT("loading_hold"); }
void save_request(void) { LAB_COUNT("save_request"); }
int test_active(void) { return 0; }
void snd_anim(unsigned owner, int model, int anim, float t, const float pos[3])
{
    (void)owner, (void)model, (void)anim, (void)t, (void)pos, LAB_COUNT("snd_anim (sound from simulate)");
}
/* map: the navmesh floor is the fallback where collision has none; the Lab has collision only (counted when asked) */
int map_floor(float x, float z, float y, float *out_y) { (void)x, (void)z, (void)y, (void)out_y, LAB_COUNT("map_floor"); return 0; }
void map_spawn(V3 *pos, float *yaw) { (void)pos, (void)yaw, LAB_COUNT("map_spawn"); }
void map_char_light(V3 pos, float amb[3], float sun[3], float dir[3])
{
    (void)pos, LAB_COUNT("map_char_light (render from simulate)");
    for (int i = 0; i < 3; i++)
        amb[i] = sun[i] = dir[i] = 0;
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
void r_camera_basis(V3 *eye, V3 *right, V3 *up, V3 *fwd, float *focal, float *focal_y, float *zscale)
{
    LAB_COUNT("r_camera_basis (render from simulate)");
    *eye = *right = *up = *fwd = v3(0, 0, 0), *focal = *focal_y = *zscale = 1;
}
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
