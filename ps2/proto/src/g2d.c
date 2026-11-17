/* g2d.c: the 2D packet writer (g2d.h). */
#include <dmaKit.h>
#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <gif_tags.h>
#include "g2d.h"
#include "gs.h"
#include "mem.h"

/* Buffer sizes. 2 x (WORLD + UI) + 2 x LOAD = the 2 x 240 KB gsKit's draw queue gave up (render.h R_GSQ_OS), WORLD and
 * UI from the frame region (mem.h): the heap at the arena's boot is the same total. Measured peak of the old single queue: 143 KB a frame (r_gsq_peak,
 * Taurus route, 2026-11-13) */
#define G2D_WORLD_KB 40  /* measured 9 KB (bonfire effects, ot2d_check 2026-11-18) */
#define G2D_UI_KB 168  /* measured ~125 KB with the debug overlay (gsKit's whole queue: 130 KB there, 143 KB Taurus) */
#define G2D_LOAD_KB 32  /* x 2, static: the loading screen's atlas sprites and text */

typedef struct {
    u64 *base, *p, *end;  /* p: the next free qword (2 u64 each); base[0..1]: the DMA tag */
    unsigned peak, cut, pending;
    const char *cut_by;
} Stream;
static Stream st[G2D_N];
static G2dStream cur = G2D_UI;
static GSGLOBAL *gsg;
static u64 *load_buf[2];
static int load_side;
/* the register cache (g2d.h): ~0 = unknown */
static u64 c_test, c_alpha, c_clamp, c_tex1;

static u64 *last_tag;  /* the REGLIST tag the next object of the same shape may join (reglist) */
static u64 last_regs;
static int last_nreg;

static void cache_clear(void) { c_test = c_alpha = c_clamp = c_tex1 = ~0ull; }

static void open_stream(Stream *s, u64 *buf, unsigned kb)
{
    s->base = buf, s->end = buf + kb * 1024 / 8;
    s->p = buf + 2;  /* the DMA tag, filled at close */
    s->pending = 0;
}

void g2d_init(GSGLOBAL *gs)
{
    gsg = gs;
    for (int i = 0; i < 2; i++)
        load_buf[i] = mem_static(G2D_LOAD_KB * 1024, 64);
    open_stream(&st[G2D_LOAD], load_buf[0], G2D_LOAD_KB);
    cache_clear();
}

void g2d_frame_begin(void)
{
    last_tag = NULL;
    open_stream(&st[G2D_WORLD], mem_frame(G2D_WORLD_KB * 1024, 64), G2D_WORLD_KB);
    open_stream(&st[G2D_UI], mem_frame(G2D_UI_KB * 1024, 64), G2D_UI_KB);
    cur = G2D_WORLD;
    cache_clear();
}

G2dStream g2d_current(void) { return cur; }

void g2d_select(G2dStream s)
{
    if (s != cur)
        cache_clear(), last_tag = NULL;  /* another stream: what the GS holds between them is not ours to know */
    cur = s;
}

/* room for qw qwords in the current stream, else the primitive is cut */
static u64 *take(int qw, const char *who)
{
    Stream *s = &st[cur];
    if (!s->base || s->p + 2 * qw + 2 > s->end) {  /* (+1: the closing EOP tag) */
        if (!s->cut++ || !s->cut_by)
            printf("TEST error: g2d: stream %d full (%u KB), %s cut (primitives cut so far %u)\n", cur,
                   s->base ? (unsigned)((char *)s->end - (char *)s->base) >> 10 : 0, who, s->cut);
        s->cut_by = who;
        return NULL;
    }
    u64 *p = s->p;
    s->p += 2 * qw;
    s->pending = 1;
    last_tag = NULL;  /* (reglist sets it again after its own take) */
    return p;
}

/* A+D writes, skipping the cached ones */
static void ad(int n, const u64 *rv)
{
    if (!st[cur].base) {  /* before the first frame (boot state: main.c's test / clamp): sent at once, the channel is idle */
        static u64 pk[2 * 8] __attribute__((aligned(64)));
        pk[0] = GIF_SET_TAG(n, 1, 0, 0, 0, 1), pk[1] = 0xE;
        memcpy(pk + 2, rv, 16 * n);
        SyncDCache(pk, pk + 2 * (n + 1));
        dmaKit_wait_fast();
        dmaKit_send(DMA_CHANNEL_GIF, pk, n + 1);
        dmaKit_wait_fast();
        return;
    }
    u64 *p = take(n + 1, "state");
    if (!p)
        return;
    p[0] = GIF_SET_TAG(n, 0, 0, 0, 0, 1), p[1] = 0xE;
    memcpy(p + 2, rv, 16 * n);
}

void *g2d_close(G2dStream which)
{
    Stream *s = &st[which];
    if (!s->base || s->p == s->base + 2)
        return NULL;
    s->p[0] = GIF_SET_TAG(0, 1, 0, 0, 0, 0), s->p[1] = 0;  /* EOP: the path may switch after it */
    s->p += 2;
    unsigned qw = (unsigned)(s->p - s->base) / 2 - 1;
    s->base[0] = DMA_TAG(qw, 0, DMA_END, 0, 0, 0), s->base[1] = 0;
    unsigned bytes = (unsigned)((char *)s->p - (char *)s->base);
    if (bytes > s->peak)
        s->peak = bytes;
    SyncDCache(s->base, s->p);
    void *r = s->base;
    s->p = s->base + 2;  /* sent once */
    return r;
}

void g2d_flush_load(void)
{
    void *c = g2d_close(G2D_LOAD);
    if (c) {
        dmaKit_wait_fast();
        dmaKit_send_chain(DMA_CHANNEL_GIF, c, 0);
        dmaKit_wait_fast();
    }
    load_side ^= 1;  /* the other buffer: this one is not read any more, but keep two as the frame region does */
    open_stream(&st[G2D_LOAD], load_buf[load_side], G2D_LOAD_KB);
    cache_clear();
}


/* ---- gsKit's state calls ---- */

void g2d_set_test(GSGLOBAL *gs, u64 which)
{
    GSTEST *t = gs->Test;
    if (which == GS_ATEST_ON)
        t->ATE = 1;
    else if (which == GS_ATEST_OFF)
        t->ATE = 0;
    else if (which == GS_ZTEST_ON)
        t->ZTE = 1, t->ZTST = 2;
    else if (which == GS_ZTEST_OFF)
        t->ZTE = 1, t->ZTST = 1;
    u64 v = GS_SETREG_TEST(t->ATE, t->ATST, t->AREF, t->AFAIL, t->DATE, t->DATM, t->ZTE, t->ZTST);
    if (v == c_test)
        return;
    c_test = v;
    const u64 rv[2] = {v, GSR_TEST};
    ad(1, rv);
}

void g2d_set_primalpha(GSGLOBAL *gs, u64 alpha)
{
    gs->PrimAlpha = alpha;
    if (alpha == c_alpha)
        return;
    c_alpha = alpha;
    const u64 rv[4] = {alpha, GSR_ALPHA, 0, 0x49 /* PABE off */};
    ad(2, rv);
}

void g2d_set_clamp(GSGLOBAL *gs, u8 mode)
{
    u64 v = mode == GS_CMODE_REPEAT ? GS_CLAMP_REPEAT : GS_CLAMP_CLAMP;
    if (v == c_clamp)
        return;
    c_clamp = v;
    const u64 rv[2] = {v, GSR_CLAMP};
    ad(1, rv);
}

/* ---- primitives: gsKit's REGLIST shapes ---- */

static inline u64 xy(const GSGLOBAL *g, float x, float y, int z)
{
    int ix = (int)(x * 16.0f) + g->OffsetX, iy = (int)(y * 16.0f) + g->OffsetY;
    ix = ix < 0 ? 0 : ix >= 4096 * 16 ? 4096 * 16 - 1 : ix;
    iy = iy < 0 ? 0 : iy >= 4096 * 16 ? 4096 * 16 - 1 : iy;
    return (u64)ix | (u64)iy << 16 | (u64)(u32)z << 32;
}
static inline int uv1(float f, int imax)
{
    int i = (int)(f * 16.0f);
    i = i < 0 ? 0 : i > imax ? imax : i;
    return i >= 1024 * 16 ? 1024 * 16 - 1 : i;
}
static inline u64 prim(const GSGLOBAL *g, int type, int iip, int tme, int fst)
{
    return GS_SETREG_PRIM(type, iip, tme, 0, g->PrimAlphaEnable, g->PrimAAEnable, fst, 0, 0);
}
static inline int log2up(u32 v)
{
    int n = 0;
    while ((1u << n) < v)
        n++;
    return n;
}

/* REGLIST register lists (4 bits each, first register lowest): gsKit's layouts */
#define RL(a, b, c, d, e, f, g, h, i, j) ((u64)(a) | (u64)(b) << 4 | (u64)(c) << 8 | (u64)(d) << 12 | (u64)(e) << 16 | \
                                          (u64)(f) << 20 | (u64)(g) << 24 | (u64)(h) << 28 | (u64)(i) << 32 | (u64)(j) << 36)
#define R_PRIM 0x0
#define R_RGBAQ 0x1
#define R_UV 0x3
#define R_XYZ2 0x5
#define R_TEX0 0x6
#define R_NOP 0xF
#define REGS_SPRITE RL(R_PRIM, R_RGBAQ, R_XYZ2, R_XYZ2, 0, 0, 0, 0, 0, 0)
#define REGS_SPRITE_TEX RL(R_TEX0, R_PRIM, R_RGBAQ, R_UV, R_XYZ2, R_UV, R_XYZ2, R_NOP, 0, 0)
#define REGS_TRIANGLE RL(R_PRIM, R_RGBAQ, R_XYZ2, R_XYZ2, R_XYZ2, R_NOP, 0, 0, 0, 0)
#define REGS_QUAD RL(R_PRIM, R_RGBAQ, R_XYZ2, R_XYZ2, R_XYZ2, R_XYZ2, 0, 0, 0, 0)
#define REGS_QUAD_GOURAUD RL(R_PRIM, R_RGBAQ, R_XYZ2, R_RGBAQ, R_XYZ2, R_RGBAQ, R_XYZ2, R_RGBAQ, R_XYZ2, R_RGBAQ)
#define REGS_TRI_GOURAUD RL(R_PRIM, R_RGBAQ, R_XYZ2, R_RGBAQ, R_XYZ2, R_RGBAQ, R_XYZ2, R_NOP, 0, 0)
#define REGS_LINE RL(R_PRIM, R_RGBAQ, R_XYZ2, R_XYZ2, 0, 0, 0, 0, 0, 0)

/* one REGLIST object: nreg (even) registers (4 bits each in regs), values in v. Objects of the same shape that follow
 * each other share one tag (NLOOP + 1), as gsKit batches them: a flat sprite is 2 qwords, not 3 */
static void reglist(int nreg, u64 regs, const u64 *v, const char *who)
{
    Stream *s = &st[cur];
    int qw = nreg / 2;
    if (last_tag && s->p == last_tag + 2 + 2 * qw * (int)(last_tag[0] & 0x7FFF) && last_regs == regs &&
        last_nreg == nreg && (last_tag[0] & 0x7FFF) < 0x7FFF) {
        u64 *tag = last_tag, *p = take(qw, who);
        if (!p)
            return;
        tag[0]++;  /* NLOOP + 1 (the low 15 bits) */
        memcpy(p, v, 8 * nreg);
        last_tag = tag;
        return;
    }
    u64 *p = take(1 + qw, who);
    if (!p)
        return;
    p[0] = GIF_SET_TAG(1, 0, 0, 0, 1, nreg), p[1] = regs;
    memcpy(p + 2, v, 8 * nreg);
    last_tag = p, last_regs = regs, last_nreg = nreg;
}

void g2d_prim_sprite(GSGLOBAL *gs, float x1, float y1, float x2, float y2, int z, u64 color)
{
    const u64 v[4] = {prim(gs, GS_PRIM_PRIM_SPRITE, 0, 0, 0), color, xy(gs, x1, y1, z), xy(gs, x2, y2, z)};
    reglist(4, REGS_SPRITE, v, "sprite");
}

void g2d_prim_sprite_texture(GSGLOBAL *gs, const GSTEXTURE *t, float x1, float y1, float u1, float v1, float x2,
                             float y2, float u2, float v2, int z, u64 color)
{
    u64 tex1 = (u64)t->Filter << 5 | (u64)t->Filter << 6;
    if (tex1 != c_tex1) {
        c_tex1 = tex1;
        const u64 rv[2] = {tex1, GSR_TEX1};
        ad(1, rv);
    }
    u64 tex0 = GS_SETREG_TEX0(t->Vram / 256, t->TBW, t->PSM, log2up(t->Width), log2up(t->Height), gs->PrimAlphaEnable, 0,
                              t->VramClut / 256, t->ClutPSM, t->ClutStorageMode, 0, GS_CLUT_STOREMODE_LOAD);
    int wu = t->Width * 16, hv = t->Height * 16;
    const u64 v[8] = {tex0, prim(gs, GS_PRIM_PRIM_SPRITE, 0, 1, 1), color,
                      (u64)uv1(u1, wu) | (u64)uv1(v1, hv) << 16, xy(gs, x1, y1, z),
                      (u64)uv1(u2, wu) | (u64)uv1(v2, hv) << 16, xy(gs, x2, y2, z), 0};
    reglist(8, REGS_SPRITE_TEX, v, "sprite_texture");
}

void g2d_prim_triangle(GSGLOBAL *gs, float x1, float y1, float x2, float y2, float x3, float y3, int z, u64 color)
{
    const u64 v[6] = {prim(gs, GS_PRIM_PRIM_TRIANGLE, 0, 0, 0), color, xy(gs, x1, y1, z), xy(gs, x2, y2, z),
                      xy(gs, x3, y3, z), 0};
    reglist(6, REGS_TRIANGLE, v, "triangle");
}

void g2d_prim_quad(GSGLOBAL *gs, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, int z,
                   u64 color)
{
    const u64 v[6] = {prim(gs, GS_PRIM_PRIM_TRISTRIP, 0, 0, 0), color, xy(gs, x1, y1, z), xy(gs, x2, y2, z),
                      xy(gs, x3, y3, z), xy(gs, x4, y4, z)};
    reglist(6, REGS_QUAD, v, "quad");
}

void g2d_prim_quad_gouraud(GSGLOBAL *gs, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4,
                           int z, u64 c1, u64 c2, u64 c3, u64 c4)
{
    const u64 v[10] = {prim(gs, GS_PRIM_PRIM_TRISTRIP, 1, 0, 0), c1, xy(gs, x1, y1, z), c2, xy(gs, x2, y2, z), c3,
                       xy(gs, x3, y3, z), c4, xy(gs, x4, y4, z), c4};
    reglist(10, REGS_QUAD_GOURAUD, v, "quad_gouraud");
}

void g2d_prim_triangle_gouraud_3d(GSGLOBAL *gs, float x1, float y1, int z1, float x2, float y2, int z2, float x3,
                                  float y3, int z3, u64 c1, u64 c2, u64 c3)
{
    const u64 v[8] = {prim(gs, GS_PRIM_PRIM_TRIANGLE, 1, 0, 0), c1, xy(gs, x1, y1, z1), c2, xy(gs, x2, y2, z2), c3,
                      xy(gs, x3, y3, z3), 0};
    reglist(8, REGS_TRI_GOURAUD, v, "triangle_gouraud");
}

void g2d_prim_line_3d(GSGLOBAL *gs, float x1, float y1, int z1, float x2, float y2, int z2, u64 color)
{
    const u64 v[4] = {prim(gs, GS_PRIM_PRIM_LINE, 0, 0, 0), color, xy(gs, x1, y1, z1), xy(gs, x2, y2, z2)};
    reglist(4, REGS_LINE, v, "line");
}

void g2d_clear(GSGLOBAL *gs, u64 color)
{
    /* depth ALWAYS, 64-pixel strips over the draw buffer, the test back (as gsKit_clear) */
    u64 old = c_test;
    const u64 rv[2] = {GS_TEST_ZALWAYS, GSR_TEST};
    ad(1, rv);
    for (int x = 0; x < gs->Width; x += 64) {
        int x1 = x + 64 < gs->Width ? x + 64 : gs->Width;
        const u64 v[4] = {GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0), color, xy(gs, x, 0, 0),
                          xy(gs, x1, gs->Height, 0)};
        reglist(4, REGS_SPRITE, v, "clear");
    }
    c_test = ~0ull;
    if (old != ~0ull) {
        const u64 rb[2] = {old, GSR_TEST};
        ad(1, rb);
        c_test = old;
    }
}

u64 *g2d_raw(int qw)
{
    u64 *p = take(qw, "raw");
    cache_clear();  /* the caller writes registers of its own */
    return p;
}

void g2d_tex_upload(GSGLOBAL *gs, GSTEXTURE *t)
{
    static unsigned busy;
    if (mem_state == MEM_PLAY && (*(volatile u32 *)0x1000A000 & 0x100) && !busy++)  /* D2_CHCR STR: GIF DMA running */
        printf("TEST error: g2d: texture upload while the GIF channel is busy (in play)\n");
    gsKit_texture_upload(gs, t);
}

void g2d_log(void)
{
    extern unsigned r_gsq_peak;
    printf("g2d: peak KB world %u of %u, ui %u of %u, load %u of %u | cut world %u ui %u load %u | gsKit queue peak %u KB\n", st[G2D_WORLD].peak >> 10,
           G2D_WORLD_KB, st[G2D_UI].peak >> 10, G2D_UI_KB, st[G2D_LOAD].peak >> 10, G2D_LOAD_KB, st[G2D_WORLD].cut,
           st[G2D_UI].cut, st[G2D_LOAD].cut, r_gsq_peak >> 10);
}
