/* DSR's loading screen (loading.h; loading.bin format at the top of tools/export_loading.py) */
#include <dmaKit.h>
#include <delaythread.h>
#include <kernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drb.h"
#include "items.h"
#include "loading.h"
#include "hitch.h"
#include "pack.h"
#include "render.h"
#include "texcache.h"
#include "ticks.h"

#define LD_Z 0xFFFF  /* nearest: the far depth of field in main.c flip leaves it sharp, as the HUD */
/* FrpgMenuDlgNowLoading (vtable 0x1413ca258): update FUN_1406fec50, constructor FUN_1406fe8c0 */
#define FADE_IN 0.5f   /* state 1: alpha +0x210 += dt until it passes 0.5 (DAT_141615c68), then state 2 */
#define HOLD_A 0.25f   /* state 2: +0x210 = 0.25; state 3 (FUN_14071b790): -= dt until < 0, then 4 (closed) */
#define HINT_S 10.0f   /* +0x22c = 10.0 (constructor): the next hint of the table after this many seconds */
#define STEPS 100      /* +0x208 / +0x20c step 0..99 (soul_0..soul_99), icon_soul1 at step +0x208 */
/* ASSUMED: one step per DSR frame at 60 fps (the update counts calls, not time) */
#define STEP_HZ 60.0f
/* the game around it: FUN_1402518d0 fades layer 2 to black over 0.5 s when the load starts (the loading state goes
 * on with it); after the end FUN_14024f1b0 sets +0x130 = 0.1 s, then FUN_14024f6b0 fades layer 2 clear over 0.5 s */
#define PRE_FADE 0.5f
#define POST_DELAY 0.1f
#define POST_FADE 0.5f
#define FRAME_TICKS (294912000u / 30)  /* at most 30 fps, as the game */

typedef struct {
    char magic[4];
    u32 version;
    u16 aw, ah;
    u32 clut_off, pix_off, nspr, spr_off, nnodes, node_off, glyph_off;
    u16 font_h, line_h;
    u32 nanim, anim_off, nhints, hint_off, str_bytes, str_off;
} LdHeader;
typedef struct { u16 u, v, w, h; u8 clut, pad; u16 pad2; } LdSprite;
typedef struct { s16 x0, y0, x1, y1; u8 kind, align; s16 spr; u8 r, g, b, a; } LdNode;
typedef struct { u16 u, v, w, h; s16 pre, adv; } LdGlyph;
typedef struct { u32 id; u16 icon, pad; u32 name, caption; } LdHint;
enum { K_SPRITE, K_RECT, K_NAME, K_CAPTION, K_ICON, K_ANIM };

LoadingStats loading_stats;

static GSGLOBAL *gs;
static void (*present)(GSGLOBAL *);
static u32 disp_vram;
static int depth, held, shown;
static Pack lp;
static int lp_open;
static unsigned char *keep;  /* header .. strings of loading.bin while open */
static unsigned keep_bytes;
static const LdHeader *H;
static const LdSprite *spr;
static const LdNode *nodes;
static const LdGlyph *gly;
static const u16 *anim;
static const LdHint *hints;
static const char *strs;
static GSTEXTURE tex[2], icon_t;
static u32 base, need;
static int top;            /* 1: at the unallocated top of VRAM (boot, before the map pool exists) */
static unsigned t0, last;  /* EE ticks: open, last frame drawn */
static float a;            /* the dialog's alpha state (+0x210) */
static int st;             /* 1 fading in, 2 shown, 3 fading out */
static float hint_t;
static int hint, hint_next = -1, icon_ok, icon_reading, saved_hint = 1000;
static unsigned char icon_buf[4096] __attribute__((aligned(64)));
static float post_t = -1;


void loading_init(GSGLOBAL *g, void (*p)(GSGLOBAL *), unsigned disp)
{
#ifdef LOADING_OFF  /* A/B builds (make EE_DBGINFOFLAGS=-DLOADING_OFF): no loading screen, loads leave the frame waiting */
    return;
#endif
    gs = g, present = p, disp_vram = disp;
    srand(ticks());  /* FUN_1406fea10 draws the first hint at random (MT19937 in DSR) */
}

/* Own GIF chain for uploads (as drb.c: gsKit_texture_send from the update phase hung PATH3) */
static u64 up_chain[22] __attribute__((aligned(64)));
static void tex_send(const void *mem, int w, int h, u32 vram, int psm, int tbw)
{
    int qw = (psm == GS_PSM_T8 ? w * h : w * h * 4) / 16;
    u64 *p = up_chain;
    while (*(volatile u32 *)0x1000A000 & 0x100)  /* D2_CHCR STR */
        ;
    p[0] = DMA_TAG(6, 0, DMA_CNT, 0, 0, 0);
    p[1] = 0;
    p[2] = GIF_TAG(4, 0, 0, 0, GSKIT_GIF_FLG_PACKED, 1);
    p[3] = GIF_AD;
    p[4] = GS_SETREG_BITBLTBUF(0, 0, 0, vram / 256, tbw, psm);
    p[5] = GS_BITBLTBUF;
    p[6] = GS_SETREG_TRXPOS(0, 0, 0, 0, 0);
    p[7] = GS_TRXPOS;
    p[8] = GS_SETREG_TRXREG(w, h);
    p[9] = GS_TRXREG;
    p[10] = GS_SETREG_TRXDIR(0);
    p[11] = GS_TRXDIR;
    p[12] = GIF_TAG(qw, 0, 0, 0, GSKIT_GIF_FLG_IMAGE, 0);
    p[13] = 0;
    p[14] = DMA_TAG(qw, 0, DMA_REF, 0, (u32)mem, 0);
    p[15] = 0;
    p[16] = DMA_TAG(2, 0, DMA_END, 0, 0, 0);
    p[17] = 0;
    p[18] = GIF_TAG(1, 1, 0, 0, GSKIT_GIF_FLG_PACKED, 1);
    p[19] = GIF_AD;
    p[20] = 0;
    p[21] = GS_TEXFLUSH;
    SyncDCache((void *)mem, (char *)mem + qw * 16 - 1);
    SyncDCache(up_chain, (char *)up_chain + sizeof up_chain - 1);
    *(volatile u32 *)0x1000A020 = 0;
    *(volatile u32 *)0x1000A030 = (u32)up_chain & 0x0FFFFFFF;
    *(volatile u32 *)0x1000A000 = 0x105;
    while (*(volatile u32 *)0x1000A000 & 0x100)
        ;
}

/* GS local -> local copy of whole 8 KB pages (both page aligned: the bytes land in the same layout whatever their
 * PSM), as CT32 64 x 32-pixel pages stacked at TBW 1 */
static void vram_copy(u32 src, u32 dst, u32 bytes)
{
    static u64 chain[14] __attribute__((aligned(64)));
    u64 *p = chain;
    while (*(volatile u32 *)0x1000A000 & 0x100)  /* D2_CHCR STR */
        ;
    p[0] = DMA_TAG(6, 0, DMA_END, 0, 0, 0);
    p[1] = 0;
    p[2] = GIF_TAG(5, 1, 0, 0, GSKIT_GIF_FLG_PACKED, 1);
    p[3] = GIF_AD;
    p[4] = GS_SETREG_BITBLTBUF(src / 256, 1, GS_PSM_CT32, dst / 256, 1, GS_PSM_CT32);
    p[5] = GS_BITBLTBUF;
    p[6] = GS_SETREG_TRXPOS(0, 0, 0, 0, 0);
    p[7] = GS_TRXPOS;
    p[8] = GS_SETREG_TRXREG(64, bytes / 256);
    p[9] = GS_TRXREG;
    p[10] = GS_SETREG_TRXDIR(2);
    p[11] = GS_TRXDIR;
    p[12] = 0;
    p[13] = GS_TEXFLUSH;
    SyncDCache(chain, (char *)chain + sizeof chain - 1);
    *(volatile u32 *)0x1000A020 = 0;
    *(volatile u32 *)0x1000A030 = (u32)chain & 0x0FFFFFFF;
    *(volatile u32 *)0x1000A000 = 0x105;
    while (*(volatile u32 *)0x1000A000 & 0x100)
        ;
}

/* VRAM: atlas (TBW 8), the icon (one PSMT8 page, TBW 2), three CLUTs (atlas 0 / 1, icon) */
static u32 icon_vram(void) { return base + ((H->aw * H->ah + 8191) & ~8191u); }
static u32 clut_vram(int i) { return icon_vram() + 8192 + i * 1024; }

static int vram_place(void)
{
    need = ((H->aw * H->ah + 8191) & ~8191u) + 8192 + 8192;
    u32 b = tc_pin(need);
    if (b) {
        base = b, top = 0;
        return 1;
    }
    /* boot: no map pool yet; the top of VRAM is unallocated until the boot's last uploads (loading_tick moves the
     * screen into the pool once it exists, and stops drawing the atlas if the allocations reach it first) */
    b = (4u * 1024 * 1024 - need) & ~8191u;
    if ((u32)gs->CurrentPointer > b)
        return 0;
    base = b, top = 1;
    return 1;
}

static int upload(const unsigned char *file)
{
    if (!vram_place()) {
        printf("loading: no VRAM for %u KB (VRAM pointer %u KB)\n", need >> 10, (unsigned)gs->CurrentPointer >> 10);
        return 0;
    }
    for (int i = 0; i < 2; i++) {
        memset(&tex[i], 0, sizeof tex[i]);
        tex[i].Width = H->aw, tex[i].Height = H->ah;
        tex[i].PSM = GS_PSM_T8, tex[i].ClutPSM = GS_PSM_CT32, tex[i].ClutStorageMode = GS_CLUT_STORAGE_CSM1;
        tex[i].Filter = GS_FILTER_LINEAR, tex[i].TBW = H->aw / 64;
        tex[i].Vram = base, tex[i].VramClut = clut_vram(i);
    }
    FlushCache(0);
    tex_send(file + H->pix_off, H->aw, H->ah, base, GS_PSM_T8, H->aw / 64);
    tex_send(file + H->clut_off, 16, 16, clut_vram(0), GS_PSM_CT32, 1);
    tex_send(file + H->clut_off + 1024, 16, 16, clut_vram(1), GS_PSM_CT32, 1);
    loading_stats.vram_kb = need >> 10;
    return 1;
}

/* loading.bin: read, upload, keep the tables and strings */
static int read_file(void)
{
    if (!lp_open && !(lp_open = pack_open(&lp, PACK_ROOT "menu.pak")))
        return 0;
    unsigned size = 0;
    unsigned char *file = pack_load_with(&lp, "loading.bin", &size, ui_alloc, ui_free);
    if (!file)
        return 0;
    const LdHeader *h = (const LdHeader *)file;
    int ok = !memcmp(h->magic, "DSLD", 4) && h->version == 1 && h->pix_off + h->aw * h->ah <= size;
    if (ok && !keep) {
        keep_bytes = (h->clut_off + 127) & ~127u;
        keep = ui_alloc(keep_bytes);
        if (keep)
            memcpy(keep, file, h->clut_off);
    }
    if (ok && keep) {
        H = (const LdHeader *)keep;
        spr = (const LdSprite *)(keep + H->spr_off);
        nodes = (const LdNode *)(keep + H->node_off);
        gly = (const LdGlyph *)(keep + H->glyph_off);
        anim = (const u16 *)(keep + H->anim_off);
        hints = (const LdHint *)(keep + H->hint_off);
        strs = (const char *)(keep + H->str_off);
        ok = upload(file);
    } else if (ok)
        printf("loading: no RAM for the tables\n");
    ui_free(file, (size + 127) & ~127u);
    loading_stats.ram_kb = keep ? keep_bytes >> 10 : 0;
    return ok && keep;
}

/* The hint's icon (icons.bin cell: CLUT 1 KB + square pixels) into VRAM; 1 started or done */
static int icon_request(int k, int wait)
{
    int ix = item_icon_index(hints[k].icon);
    if (ix < 0 || item_icon_bytes() > (int)sizeof icon_buf)
        return -1;
    if (!item_icon_read(ix, icon_buf))
        return 0;
    icon_reading = 1;
    if (wait)
        pack_async_wait();
    return 1;
}

static void icon_collect(void)
{
    if (!icon_reading)
        return;
    int r = item_icon_done();
    if (!r)
        return;
    icon_reading = 0;
    if (hint_next >= 0)
        hint = hint_next, hint_next = -1, loading_stats.hint = hint;
    icon_ok = r > 0;
    if (!icon_ok)
        return;
    int px = item_icon_size();
    memset(&icon_t, 0, sizeof icon_t);
    icon_t.Width = px, icon_t.Height = px;
    icon_t.PSM = GS_PSM_T8, icon_t.ClutPSM = GS_PSM_CT32, icon_t.ClutStorageMode = GS_CLUT_STORAGE_CSM1;
    icon_t.Filter = GS_FILTER_LINEAR, icon_t.TBW = 2;
    icon_t.Vram = icon_vram(), icon_t.VramClut = clut_vram(2);
    tex_send(icon_buf, 16, 16, icon_t.VramClut, GS_PSM_CT32, 1);
    tex_send(icon_buf + 1024, px, px, icon_t.Vram, GS_PSM_T8, 2);
}

static void release(void)
{
    if (icon_reading) {  /* a read into icon_buf is in flight: let it land */
        pack_async_wait();
        item_icon_done();
        icon_reading = 0;
    }
    if (keep)
        ui_free(keep, keep_bytes), keep = 0;
    H = 0;
    if (!top)
        tc_unpin();
    loading_stats.vram_kb = loading_stats.ram_kb = 0;
}

static void open_screen(void)
{
    t0 = last = ticks();
    a = 0, st = 1, hint_t = 0, icon_ok = 0, hint_next = -1;
    post_t = -1;
    shown = 1;
    loading_stats.opens++;
    pack_async_free();  /* a background read (enemy model, menu icon) holds the slot no longer */
    if (!read_file()) {
        printf("loading: no loading.bin, black screen only\n");
        return;
    }
    /* FUN_1406fea10: the first hint at (last one + rand) % 62 (the first time: rand % 62) */
    unsigned r = (unsigned)rand();
    hint = (int)((saved_hint < H->nhints ? saved_hint : 0) + r % H->nhints) % H->nhints;
    loading_stats.hint = hint;
    if (icon_request(hint, 1) > 0)
        icon_collect();
    printf("loading: open, hint %d (item %u), %u KB VRAM at %u KB%s, %u KB RAM\n", hint, (unsigned)hints[hint].id,
           need >> 10, base >> 10, top ? " (top)" : "", keep_bytes >> 10);
}

static u64 rgba(int r, int g, int b, int al) { return GS_SETREG_RGBAQ(r, g, b, al, 0); }

static void sprite(const LdSprite *s, float x0, float y0, float x1, float y1, u64 c)
{
    gsKit_prim_sprite_texture(gs, &tex[s->clut], R_HX(x0), y0, s->u + 0.5f, s->v + 0.5f, R_HX(x1), y1,
                              s->u + s->w - 0.5f, s->v + s->h - 0.5f, LD_Z, c);
}

static float text_width(const char *s, int n)
{
    float w = 0;
    for (int i = 0; i < n; i++)
        if (s[i] >= 32 && s[i] < 127)
            w += gly[s[i] - 32].adv;
    return w;
}

static void text_line(float x, float y, const char *s, int n, float sx, u64 c)
{
    for (int i = 0; i < n; i++) {
        if (s[i] < 32 || s[i] >= 127)
            continue;
        const LdGlyph *g = &gly[s[i] - 32];
        if (s[i] != ' ')
            gsKit_prim_sprite_texture(gs, &tex[0], R_HX(x + g->pre * sx), y, g->u, g->v, R_HX(x + (g->pre + g->w) * sx),
                                      y + g->h, g->u + g->w, g->v + g->h, LD_Z, c);
        x += g->adv * sx;
    }
}

/* one line per \n (the FMG text has its own breaks), a line wider than the box squeezed to fit (as drb.c) */
static void text_box(const LdNode *n, const char *s, float lh, u64 c)
{
    float y = n->align & DRB_MIDDLE ? (n->y0 + n->y1 - H->font_h) / 2.0f : n->y0, bw = n->x1 - n->x0;
    while (*s && y + H->font_h <= n->y1 + 2) {
        const char *e = strchr(s, '\n');
        int len = e ? e - s : (int)strlen(s);
        float w = text_width(s, len), sx = w > bw && bw > 8 ? (bw / w < 0.5f ? 0.5f : bw / w) : 1;
        text_line(n->align & DRB_CENTER ? (n->x0 + n->x1 - w * sx) / 2 : n->x0, y, s, len, sx, c);
        y += lh;
        if (!e)
            break;
        s = e + 1;
    }
}

static void draw(float t)
{
    /* the dialog's alpha: the exe's 0..1 value x 2 x 255 into an alpha that is opaque at 0x80 (ASSUMED scale: in the
     * shown state, 0.25, DSR draws the hint at full brightness: reference captures 2026-11-03, TECH_DEBT load-2) */
    int al = (int)(a * 2 * 255);
    al = al < 0 ? 0 : al > 0x80 ? 0x80 : al;
    gsKit_set_test(gs, GS_ATEST_OFF);
    gsKit_set_test(gs, GS_ZTEST_OFF);
    gs->PrimAlphaEnable = GS_SETTING_OFF;
    /* the last game frame fading to black under the screen (fade layer 2), black at boot */
    float k = disp_vram && loading_stats.opens > 1 ? 1 - t / PRE_FADE : 0;
    if (k > 0) {
        GSTEXTURE d;
        memset(&d, 0, sizeof d);
        d.Width = R_SCREEN_W, d.Height = R_SCREEN_H / 2, d.PSM = gs->PSM, d.Vram = disp_vram, d.TBW = R_SCREEN_W / 64;
        d.Filter = GS_FILTER_LINEAR;
        int c = (int)(0x80 * k);
        gsKit_prim_sprite_texture(gs, &d, 0, 0, 0, 0, R_SCREEN_W, R_SCREEN_H, R_SCREEN_W, R_SCREEN_H / 2, LD_Z,
                                  rgba(c, c, c, 0x80));
    } else
        gsKit_prim_sprite(gs, 0, 0, R_SCREEN_W, R_SCREEN_H, LD_Z, rgba(0, 0, 0, 0x80));
    gs->PrimAlphaEnable = GS_SETTING_ON;
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
    int atlas_ok = H && !(top && (u32)gs->CurrentPointer > base);
    for (unsigned i = 0; atlas_ok && i < H->nnodes; i++) {
        const LdNode *n = &nodes[i];
        int na = n->a * al >> 7;
        u64 c = rgba((n->r + 1) >> 1, (n->g + 1) >> 1, (n->b + 1) >> 1, na);
        switch (n->kind) {
        case K_SPRITE:
            sprite(&spr[n->spr], n->x0, n->y0, n->x1, n->y1, c);
            break;
        case K_RECT:
            gsKit_prim_sprite(gs, R_HX(n->x0), n->y0, R_HX(n->x1), n->y1, LD_Z, c);
            break;
        case K_ANIM:
            sprite(&spr[anim[(unsigned)(t * STEP_HZ) % (H->nanim ? H->nanim : 1)]], n->x0, n->y0, n->x1, n->y1, c);
            break;
        case K_ICON:
            if (icon_ok) {  /* icons.bin's square cell over the box's height, centred (DSR's 160 x 180 cell) */
                float h = n->y1 - n->y0, w = h * (640.0f / 1280) / (448.0f / 720), cx = (n->x0 + n->x1) / 2.0f;
                gsKit_prim_sprite_texture(gs, &icon_t, R_HX(cx - w / 2), n->y0, 0.5f, 0.5f, R_HX(cx + w / 2), n->y1,
                                          icon_t.Width - 0.5f, icon_t.Height - 0.5f, LD_Z, c);
            }
            break;
        case K_NAME:
            text_box(n, strs + hints[hint].name, H->font_h, c);
            break;
        case K_CAPTION:
            text_box(n, strs + hints[hint].caption, H->line_h, c);
            break;
        }
    }
    gs->PrimAlphaEnable = GS_SETTING_OFF;
    gsKit_set_test(gs, GS_ZTEST_ON);
    gsKit_set_test(gs, GS_ATEST_ON);
    gsKit_queue_exec(gs);
    present(gs);
    loading_stats.frames++;
}

void loading_tick(int can_read)
{
    if (!shown || !gs)
        return;
    unsigned now = ticks();
    if (now - last < FRAME_TICKS) {
        DelayThread(500);  /* pack.c's wait loop polls the IOP: not in a tight loop */
        return;
    }
    float dt = (now - last) / 294912000.0f, t = (now - t0) / 294912000.0f;
    last = now;
    if (dt > 0.1f)
        dt = 0.1f;  /* a long synchronous step (a pack open) is one frame, as DSR's update would see it */
    if (st == 1 && (a += dt) > FADE_IN)
        st = 2, a = HOLD_A;
    else if (st == 3)
        a -= dt;
    if (can_read && H) {
        u32 pool;
        if (top && (pool = tc_pin(need))) {  /* boot: the map pool exists now: move there, before the boot's last uploads */
            if ((u32)gs->CurrentPointer <= base) {  /* still intact at the top: one GS copy, no second read */
                vram_copy(base, pool, need);
                base = pool, top = 0;
                for (int i = 0; i < 2; i++)
                    tex[i].Vram = base, tex[i].VramClut = clut_vram(i);
                icon_t.Vram = icon_vram(), icon_t.VramClut = clut_vram(2);
            } else {  /* the boot's allocations reached it: read loading.bin again */
                tc_unpin();
                top = 0;
                unsigned char *k = keep;
                keep = 0;
                int ok = read_file();
                if (k)
                    ui_free(k, keep_bytes);
                if (ok && icon_request(hint, 1) > 0)
                    icon_collect();
            }
            printf("loading: moved to the map pool at %u KB\n", base >> 10);
        }
        icon_collect();
        if ((hint_t += dt) >= HINT_S && hint_next < 0 && !icon_reading) {
            hint_t = 0;
            int nx = (hint + 1) % H->nhints;
            if (icon_request(nx, 0) > 0)
                hint_next = nx;
            else
                hint = nx, icon_ok = 0, loading_stats.hint = hint;
        }
    }
    draw(t);
}

static void pack_hook(void) { loading_tick(0); }

void loading_begin(void)
{
    hitch_loadscreen();  /* this frame waits behind a loading screen: not a hitch (src/hitch.c) */
    if (depth++ || !gs)
        return;
    open_screen();
    pack_wait_hook = pack_hook;
    loading_tick(1);
}

void loading_end(void)
{
    if (!depth || --depth || !shown)
        return;
    /* the screen stays until its fade-in is done (state 2): DSR's load only starts after its 0.5 s fade to black */
    while (shown && H && st == 1)
        loading_tick(1);
    st = 3;
    while (shown && a > 0)
        loading_tick(1);
    pack_wait_hook = 0;
    loading_stats.shown_ms = (ticks() - t0) / 294912;
    printf("loading: closed after %u ms, %u frames\n", loading_stats.shown_ms, loading_stats.frames);
    saved_hint = hint;
    release();
    shown = 0;
    post_t = 0;
}

void loading_show(float s)
{
    loading_begin();
    unsigned t = ticks();
    while ((ticks() - t) / 294912000.0f < s)
        loading_tick(1);
    loading_end();
}

void loading_hold(void)
{
    if (!held) {
        held = 1;
        loading_begin();
    }
}

void loading_frame(void)
{
    if (held) {
        held = 0;
        loading_end();
    }
}

void loading_overlay(float dt)
{
    if (post_t < 0 || !gs)
        return;
    float k = post_t < POST_DELAY ? 1 : 1 - (post_t - POST_DELAY) / POST_FADE;
    post_t += dt;
    if (k <= 0) {
        post_t = -1;
        return;
    }
    gs->PrimAlphaEnable = GS_SETTING_ON;
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
    gsKit_set_test(gs, GS_ZTEST_OFF);
    gsKit_prim_sprite(gs, 0, 0, R_SCREEN_W, R_SCREEN_H, LD_Z, rgba(0, 0, 0, (int)(0x80 * k)));
    gsKit_set_test(gs, GS_ZTEST_ON);
    gs->PrimAlphaEnable = GS_SETTING_OFF;
}
