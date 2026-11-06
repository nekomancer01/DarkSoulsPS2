/* DSR -> PS2 character prototype: the Warrior with a Longsword on a white plane.
 * Controls (DualShock 2): left stick move (light tilt walks), Circle tap = roll / hold = dash, R1 = attack,
 * right stick = camera, L3 = helm off / on (debug), Select = debug HUD. A test script from argv (test=<file>) drives a
 * test run instead. The idle demo walk (testscript.c) is test-only: "option demo" in a script, or a build with
 * make EE_DBGINFOFLAGS=-DIDLE_DEMO. */
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <gsKit.h>
#include <dmaKit.h>
#include "render.h"
#include "player.h"
#include "model.h"
#include "map.h"
#include "bump.h"
#include "drawgrp.h"
#include "vu1.h"
#include "collision.h"
#include "combat.h"
#include "pack.h"
#include "enemy.h"
#include "objects.h"
#include "fx.h"
#include "hud.h"
#include "menu.h"
#include "items.h"
#include "prof.h"  /* PROF_* hooks: empty except in make PROFILE=1 (tools-hwperf) */
#include "testscript.h"
#include "debug_hud.h"  /* Select overlay, debug world drawing */
#include "world.h"
#include "save.h"  /* A3: progression + event flags, loaded at boot, saved on request / autosave */
#include "event.h"  /* A2: EMEVD runtime */
#include "bonfire.h"  /* A4: rest, death, bloodstain */
#include "loading.h"  /* DSR's loading screen over blocking loads */
#include "sound.h"  /* A12: SPU2 sound (src/sound.c, iop/dsrsnd) */
#include "gamedata.h"  /* player / combat / progress values from c0000.pak (the ELF holds no game data) */
#include <unistd.h>

#define PI 3.14159265f
#ifndef AREA
#define AREA "m10_02_00_00"  /* Firelink Shrine: map.bin + collision.bin in data/<area>.pak; make MAP=<area> sets it */
#endif

extern char _end[];       /* end of code + data + bss (linker) */
extern char _stack_size[]; /* linker symbol (its address is the size): crt0 puts the stack at the top of the 32 MB */

/* EE RAM free between the heap's end and the stack (bytes) */
/* between heap and stack, plus the heap's free list (blocks freed after load, e.g. R2's player blob) */
static unsigned ram_free(void)
{
    struct mallinfo mi = mallinfo();
    return 32 * 1024 * 1024 - (unsigned)_stack_size - (unsigned)sbrk(0) + mi.fordblks;
}

/* EE COP0 Count: 294.912 MHz */
static inline unsigned cpu_ticks(void)
{
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

static char pad_buf[256] __attribute__((aligned(64)));
static int pad_ok;

static void pad_setup(void)
{
    SifInitRpc(0);
    if (SifLoadModule("rom0:SIO2MAN", 0, NULL) < 0 || SifLoadModule("rom0:PADMAN", 0, NULL) < 0)
        return;
    padInit(0);
    if (!padPortOpen(0, 0, pad_buf))
        return;
    pad_ok = 1;
}

static float axis(unsigned char v)
{
    float f = (v - 128) / 127.0f;
    return fabsf(f) < 0.18f ? 0 : f;  /* dead zone */
}

/* Returns 1 if the pad produced any input this frame */
static int pad_input(Input *in, unsigned short *prev)
{
    struct padButtonStatus st;
    int state = pad_ok ? padGetState(0, 0) : -1;
    if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1)
        return 0;
    if (!padRead(0, 0, &st))
        return 0;
    static int dual;
    if (!dual) {  /* analog sticks need DualShock mode */
        padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
        dual = 1;
    }
    unsigned short btn = 0xFFFF ^ st.btns;
    in->mx = axis(st.ljoy_h);
    in->my = -axis(st.ljoy_v);
    in->cx = axis(st.rjoy_h);
    in->cy = axis(st.rjoy_v);
    /* d-pad works as a digital left stick (keyboard-mapped emulators) */
    if (btn & PAD_UP) in->my = 1;
    if (btn & PAD_DOWN) in->my = -1;
    if (btn & PAD_LEFT) in->mx = -1;
    if (btn & PAD_RIGHT) in->mx = 1;
    in->r1 = (btn & PAD_R1) && !(*prev & PAD_R1);
    in->circle = (btn & PAD_CIRCLE) != 0;
    if ((btn & PAD_CROSS) && !(*prev & PAD_CROSS))
        event_action();  /* DSR's action button (ObjActs, treasures, fog walls: src/event.c) */
    *prev = btn;
    return btn || in->mx || in->my || in->cx || in->cy;
}

/* F9: Z-only clear at the start of the map chain (PATH2, no gsKit queue / DMA wait): the sky dome covers every pixel,
 * so the colour clear was a wasted full-screen pass. FRAME_1 with all colour bits masked, depth test ALWAYS, 64-pixel
 * sprite strips at Z 0 (gsKit_clear's page-friendly pattern), then FRAME_1 back. F9_HOLES test builds clear the
 * colour to magenta instead, so any pixel the map leaves uncovered shows; so does a test script with "option holes". */
static int holes;

static void z_clear(GSGLOBAL *gs)
{
    u64 r[2 * 40];
    int n = 0;
    u64 frame = GS_SETREG_FRAME_1(gs->ScreenBuffer[gs->ActiveBuffer & 1] / 8192, gs->Width / 64, gs->PSM, 0);
#define REG(v, a) (r[2 * n] = (v), r[2 * n + 1] = (a), n++)
#ifdef F9_HOLES
    holes = 1;
#endif
    REG(holes ? frame : frame | 0xFFFFFFFFull << 32, 0x4C);
    REG(GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 1), 0x47);  /* ZTE, ZTST ALWAYS */
    REG(GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0), 0x4E);
    REG(GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0), 0x00);
    REG(GS_SETREG_RGBAQ(0xFF, 0x00, 0xFF, 0x80, 0), 0x01);
    for (int x = 0; x < gs->Width; x += 64) {
        int x1 = x + 64 < gs->Width ? x + 64 : gs->Width;
        REG(GS_SETREG_XYZ2((x << 4) + gs->OffsetX, gs->OffsetY, 0), 0x05);
        REG(GS_SETREG_XYZ2((x1 << 4) + gs->OffsetX, (gs->Height << 4) + gs->OffsetY, 0), 0x05);
    }
    REG(frame, 0x4C);
#undef REG
    vu1_gs_regs(n, r);
}

/* V1b: one 448-line draw buffer (gsKit, single-buffered) and a 224-line display buffer. flip(): after vsync, a GS
 * sprite copy resamples the draw buffer into the display buffer in 64-pixel strips: bilinear at v = 2y + 1 averages
 * line pairs (a 2:1 box filter, stable at 30 fps). Same scheme as Jak II / R&C 1 (notes/research/ps2-optimisation.md
 * §11).
 * The 224 lines go out as NTSC 240p (non-interlaced, DISPLAY in progressive raster lines). They were shown as 480i
 * FRAME mode (SMODE2 INT 1, FFMD 1: both fields scan the same 224 lines, the odd one half a line lower), which holds
 * no more detail but makes emulators deinterlace: PCSX2 forces its adaptive deinterlacer on FFMD 1 output and moves
 * every other field down by one output line, so upscaled frames came out soft, combed on motion and bobbing
 * (TECH_DEBT render-7). A full-height 480i frame (FFMD 0) deinterlaces cleanly but needs a 448-line display buffer:
 * +224 KB VRAM. -DV1B_INTERLACED keeps the old 480i FFMD 1 output (real-hardware A/B, TVs without 240p input);
 * V1B_FIELD test builds point-sample the current field's lines instead (sharper at 60 fps, shimmers when a frame
 * spans two fields) and need it. */
#if defined(V1B_FIELD) && !defined(V1B_INTERLACED)
#define V1B_INTERLACED
#endif
#define DISP_H (R_SCREEN_H / 2)
static u32 disp_buf;

static void display_init(GSGLOBAL *gs)
{
    disp_buf = gsKit_vram_alloc(gs, gsKit_texture_size(R_SCREEN_W, DISP_H, gs->PSM), GSKIT_ALLOC_SYSBUFFER);
    /* gsKit set up 480i for its 448-line buffer: StartY / DH are interlaced raster lines (DY 50, DH 448) */
#ifdef V1B_INTERLACED
    SetGsCrt(GS_INTERLACED, GS_MODE_NTSC, GS_FRAME);
    int dy = gs->StartY + gs->StartYOffset, dh = 2 * DISP_H;  /* FFMD 1: DH counts both fields' lines */
#else
    SetGsCrt(GS_NONINTERLACED, GS_MODE_NTSC, GS_FRAME);
    int dy = (gs->StartY + gs->StartYOffset) / 2, dh = DISP_H;  /* 25: gsKit's own 240p centring of 224 lines */
#endif
    GS_SET_PMODE(0, 1, 0, 1, 0, 0x80);
    GS_SET_DISPFB2(disp_buf / 8192, R_SCREEN_W / 64, gs->PSM, 0, 0);
    GS_SET_DISPLAY2(gs->StartX + gs->StartXOffset, dy, gs->MagH, 0, gs->DW - 1, dh - 1);
}

/* Frame cap (user, 2026-10-31: 30 fps target): flip at most every FPS_CAP_FIELDS vsyncs. A frame that finished early
 * waits an extra field, so pacing stays an even 30 fps instead of alternating 60 / 30. FPS_CAP_FIELDS=1 uncaps. */
#ifndef FPS_CAP_FIELDS
#define FPS_CAP_FIELDS 2
#endif
#define FIELD_TICKS (294912000u / 60)  /* EE ticks per NTSC field (59.94 Hz, rounded) */

/* Far depth of field (DSR DofBank, map_far_dof: blur = mul x saturate((z - begin) / (end - begin)) of view depth z;
 * the linear ramp is ASSUMED, TECH_DEBT render-13). DSR blurs the distance (Firelink row 20: 10 -> 100 m, 60 %), which
 * is what keeps its far walls steady; drawn sharp at 512 px, 2-3 px wide pillar faces step and crawl as the camera
 * moves (render-12). Done in flip() after the sharp copy, as DOF_BANDS depth bands, farthest first: the draw
 * buffer's alpha bit marks pixels beyond the band's depth (a Z-tested sprite writing alpha only: GEQUAL, larger Z is
 * nearer, so a sprite at the band's Z passes where the scene is farther), the mark is point-copied into the
 * display buffer's alpha, and two samples of the draw buffer, offset +-DOF_DX px / +-1 display line, are
 * blended over the marked pixels (DATE). The marks only grow from band to band, so a pixel beyond band i gets bands
 * N..i: per-band strengths are set so the product reaches the ramp's value. The HUD writes the nearest Z (hud.c),
 * so it is never marked. */
#ifndef DOF_BANDS
#define DOF_BANDS 3
#endif
#define DOF_DX 1  /* px: horizontal offset of the blur taps, whole texels (half-texel offsets band in PCSX2; ASSUMED, judged
                   * against the DSR frame at the render-12 spot) */
#define DOF_TEXA 0x3B

static int dof_regs(GSGLOBAL *gs, u64 *p)
{
    float b0, b1, mul;
    if (!map_far_dof(&b0, &b1, &mul))
        return 0;
    int n = 0;
#define REG(v, a) (*p++ = (v), *p++ = (a), n++)
#define FULL(z)                                                                    \
    (REG(GS_SETREG_XYZ2(gs->OffsetX, gs->OffsetY, z), 0x05),                         \
     REG(GS_SETREG_XYZ2((R_SCREEN_W << 4) + gs->OffsetX, (R_SCREEN_H << 4) + gs->OffsetY, z), 0x05))
    const u64 draw_a = GS_SETREG_FRAME_1(gs->ScreenBuffer[0] / 8192, R_SCREEN_W / 64, gs->PSM, 0x00FFFFFF);
    const u64 disp_a = GS_SETREG_FRAME_1(disp_buf / 8192, R_SCREEN_W / 64, gs->PSM, 0x00FFFFFF);
    const u64 tex = GS_SETREG_TEX0(gs->ScreenBuffer[0] / 256, R_SCREEN_W / 64, gs->PSM, 9, 9, 1, 1, 0, 0, 0, 0, 0);
    /* clear the draw buffer's alpha bits (Z always, not written) */
    REG(draw_a, 0x4C);
    REG(GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 1), 0x47);
    REG(GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0), 0x00);
    REG(GS_SETREG_RGBAQ(0, 0, 0, 0, 0), 0x01);
    FULL(0);
    REG(GS_SETREG_TEXA(0, 0, 0x80), DOF_TEXA);  /* the copied alpha bit: 0 -> 0, 1 -> 0x80 */
    REG(0, 0x45);  /* DTHE off: the 16-bit dither would add its pattern again on every blended tap */
    for (int i = DOF_BANDS; i >= 1; i--) {
        /* a pixel between bands i and i + 1 gets bands 1..i: 1 - prod (1 - a_j) = mul x i / N */
        float z = b0 + (b1 - b0) * (i - 0.5f) / DOF_BANDS, s = mul * i / DOF_BANDS, s0 = mul * (i - 1) / DOF_BANDS;
        float a = 1 - (1 - s) / (1 - s0);
        float x = 1 - sqrtf(1 - a);  /* two taps blended in turn: 1 - (1 - x)^2 = a */
        int fix = (int)(x * 128 + 0.5f);
        /* mark: alpha 1 where the scene lies beyond z */
        REG(draw_a, 0x4C);
        REG(GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 2), 0x47);
        REG(GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0), 0x00);
        REG(GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0), 0x01);
        FULL((u32)(65535.0f * R_NEAR / z));
        REG(0, 0x3F);  /* TEXFLUSH: the marks are read back as texture */
        /* marks into the display buffer's alpha (point sampled, line 2y) */
        REG(disp_a, 0x4C);
        REG(GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 1), 0x47);
        REG(tex, 0x06);
        REG(GS_SETREG_TEX1(0, 0, 0, 0, 0, 0, 0), 0x14);
        REG(GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0), 0x00);
        for (int x0 = 0; x0 < R_SCREEN_W; x0 += 64) {
            REG(GS_SETREG_UV((x0 << 4) + 8, 8), 0x03);
            REG(GS_SETREG_XYZ2((x0 << 4) + gs->OffsetX, gs->OffsetY, 0), 0x05);
            REG(GS_SETREG_UV(((x0 + 64) << 4) + 8, 8 + (R_SCREEN_H << 4)), 0x03);
            REG(GS_SETREG_XYZ2(((x0 + 64) << 4) + gs->OffsetX, (DISP_H << 4) + gs->OffsetY, 0), 0x05);
        }
        /* two offset bilinear taps over the marked pixels: Cd + (Cs - Cd) x fix / 128 */
        REG(GS_SETREG_FRAME_1(disp_buf / 8192, R_SCREEN_W / 64, gs->PSM, 0), 0x4C);
        REG(GS_SETREG_TEST(0, 0, 0, 0, 1, 1, 1, 1), 0x47);
        REG(GS_SETREG_TEX0(gs->ScreenBuffer[0] / 256, R_SCREEN_W / 64, gs->PSM, 9, 9, 0, 1, 0, 0, 0, 0, 0), 0x06);
        REG(GS_SETREG_TEX1(0, 0, 1, 1, 0, 0, 0), 0x14);
        REG(GS_SETREG_ALPHA(0, 1, 2, 1, fix), 0x42);
        REG(GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 1, 0, 1, 0, 1, 0, 0), 0x00);
        for (int t = 0; t < 2; t++) {
            /* the sprite moves, not its UV (UV is unsigned: a negative start wraps): tap 0 shows texel x - DOF_DX,
             * lines 2y - 2 / 2y - 1 at (x, y), tap 1 texel x + DOF_DX, lines 2y + 2 / 2y + 3; the sharp copy has
             * x, lines 2y / 2y + 1. The uncovered edge column / line keeps the sharp copy. */
            int dx = (t ? -DOF_DX : DOF_DX) << 4, dy = t ? -16 : 16;
            for (int x0 = 0; x0 < R_SCREEN_W; x0 += 64) {
                REG(GS_SETREG_UV(x0 << 4, 16), 0x03);
                REG(GS_SETREG_XYZ2((x0 << 4) + dx + gs->OffsetX, dy + gs->OffsetY, 0), 0x05);
                REG(GS_SETREG_UV((x0 + 64) << 4, 16 + (R_SCREEN_H << 4)), 0x03);
                REG(GS_SETREG_XYZ2(((x0 + 64) << 4) + dx + gs->OffsetX, (DISP_H << 4) + dy + gs->OffsetY, 0), 0x05);
            }
        }
    }
    REG(GS_SETREG_TEXA(0x80, 0, 0x80), DOF_TEXA);  /* gsKit's */
    REG(1, 0x45);  /* DTHE back on (main: gs->Dithering) */
    REG(gs->PrimAlpha, 0x42);
#undef FULL
#undef REG
    return n;
}

static void flip(GSGLOBAL *gs)
{
    static u64 pk[2 * 64 + 2 * 128 * DOF_BANDS] __attribute__((aligned(64)));
    static unsigned last_flip;
    gsKit_vsync_wait();
    for (int f = 1; f < FPS_CAP_FIELDS && cpu_ticks() - last_flip < (unsigned)(f * FIELD_TICKS + FIELD_TICKS / 2); f++)
        gsKit_vsync_wait();
    last_flip = cpu_ticks();
#ifdef V1B_FIELD
    int odd = (*GS_CSR >> 13) & 1, filter = 0, v0 = odd * 16 + 8;  /* texel centre of line 2y + field */
#else
    int filter = 1, v0 = 16;  /* between the centres of lines 2y and 2y + 1 */
#endif
    int n = 0;
    u64 *p = pk + 2;
#define REG(v, a) (*p++ = (v), *p++ = (a), n++)
    REG(0, 0x3F);  /* TEXFLUSH: the draw buffer is read as a texture right after being drawn */
    REG(GS_SETREG_FRAME_1(disp_buf / 8192, R_SCREEN_W / 64, gs->PSM, 0), 0x4C);
    REG(GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 1), 0x4E);
    REG(GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 1), 0x47);
    REG(GS_SETREG_TEX0(gs->ScreenBuffer[0] / 256, R_SCREEN_W / 64, gs->PSM, 9, 9, 0, 1, 0, 0, 0, 0, 0), 0x06);
    REG(GS_SETREG_TEX1(0, 0, filter, filter, 0, 0, 0), 0x14);
    REG(GS_SETREG_CLAMP(1, 1, 0, 0, 0, 0), 0x08);
    REG(GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0), 0x00);
    for (int x = 0; x < R_SCREEN_W; x += 64) {
        REG(GS_SETREG_UV(x << 4, v0), 0x03);
        REG(GS_SETREG_XYZ2((x << 4) + gs->OffsetX, gs->OffsetY, 0), 0x05);
        REG(GS_SETREG_UV((x + 64) << 4, v0 + (R_SCREEN_H << 4)), 0x03);
        REG(GS_SETREG_XYZ2(((x + 64) << 4) + gs->OffsetX, (DISP_H << 4) + gs->OffsetY, 0), 0x05);
    }
    {
        int k = dof_regs(gs, p);
        p += 2 * k, n += k;
    }
    /* back to the draw buffer and the state the frame expects (main's TEST, repeat wrapping) */
    REG(GS_SETREG_FRAME_1(gs->ScreenBuffer[0] / 8192, R_SCREEN_W / 64, gs->PSM, 0), 0x4C);
    REG(GS_SETREG_ZBUF(gs->ZBuffer / 8192, gs->PSMZ, 0), 0x4E);
    REG(GS_SETREG_TEST(1, 5, 0x40, 0, 0, 0, 1, 2), 0x47);
    REG(GS_SETREG_CLAMP(0, 0, 0, 0, 0, 0), 0x08);
#undef REG
    pk[0] = GIF_TAG(n, 1, 0, 0, 0, 1);
    pk[1] = 0xE;
    SyncDCache(pk, p);
    dmaKit_wait_fast();
    dmaKit_send(DMA_CHANNEL_GIF, pk, n + 1);
    dmaKit_wait_fast();
}

int main(int argc, char **argv)
{
    GSGLOBAL *gs = gsKit_init_global();
    gs->Mode = GS_MODE_NTSC;
    gs->Interlace = GS_INTERLACED;
    gs->Field = GS_FIELD;
    gs->Width = R_SCREEN_W;  /* V1c: 512 wide, stretched by the display */
    gs->Height = R_SCREEN_H;
    /* 16-bit dithered colour leaves ~1.15 MB more VRAM for textures than CT24 */
    gs->PSM = GS_PSM_CT16;
    gs->Dithering = GS_SETTING_ON;
    /* depth must match the colour buffer's pixel size: CT16 with a 24/32-bit Z broke depth tests */
    gs->PSMZ = GS_PSMZ_16S;
    gs->ZBuffering = GS_SETTING_ON;
    gs->DoubleBuffering = GS_SETTING_OFF;  /* V1b: own 224-line display buffer, see flip() */
    gs->PrimAAEnable = GS_SETTING_OFF;  /* AA1 on untextured triangles without blending left only vertex dots */
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    display_init(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gs->Test->ZTST = 2;  /* GEQUAL: larger Z is nearer */
    gsKit_set_test(gs, GS_ZTEST_ON);
    r_init(gs);
    vu1_init(gs);
    pad_setup();  /* also SifInitRpc, which the fio reads below need */
    int test_opts = test_init(argc, argv);  /* test script (host:<file> from argv), before anything it configures */
    holes = test_opts & TEST_HOLES;
#ifdef IDLE_DEMO
    test_opts |= TEST_DEMO;  /* demo walk build (make EE_DBGINFOFLAGS=-DIDLE_DEMO): the idle demo without a script */
#endif
    if (!gamedata_init()) {  /* player / combat / start values: nothing below runs without them */
        printf("DSRPROTO gamedata: missing or from another build; stopping\n");
        for (;;)
            SleepThread();
    }
    items_init();  /* menu.pak items.bin: stack sizes for the inventory rules (src/items.c), before the save loads */
    /* DSR's loading screen over the boot load (src/loading.c): drawn from inside the loads below, closed after the first
     * frame's world update (the boot position's blocks settle under it) */
    loading_init(gs, flip, disp_buf);
    loading_hold();
    save_boot(argc, argv, AREA);  /* flags + progression from the newest save (mc0: / host:), else a new game */
    /* sound: IOP modules, the player's bank and the boot area's into SPU2 RAM (the IOP reads them: no EE RAM) */
    if (snd_init())
        snd_map(AREA);
    loading_tick(1);
    /* assets from per-area / per-character LZ4 packs (tools/pack.py, src/pack.c); kept for the program's lifetime */
    unsigned t_load = cpu_ticks();
    Pack pk;
    unsigned model_size = 0;
    void *model_blob = pack_open(&pk, PACK_ROOT "c0000.pak") ? pack_load(&pk, "model.bin", &model_size) : 0;
    pack_close(&pk);
    void *map_blob = 0, *col_blob = 0, *enemy_blob = 0, *lua_blob = 0, *obj_blob = 0, *bump_blob = 0, *fx_blob = 0,
         *dg_blob = 0, *ev_blob = 0, *evc_blob = 0, *pvs_blob = 0;
    unsigned lua_size = 0;
    int world = world_init(AREA);  /* world build: blocks stream in by position (src/world.c, data/world.pak) */
    if (pack_open(&pk, PACK_ROOT AREA ".pak")) {
        map_blob = world ? NULL : pack_load(&pk, "map.bin", 0);  /* world: split map, world_boot_map */
#ifndef MAP_PANO_R  /* panorama capture builds (tools/pano.py): no relief or effects, their RAM goes to the far map */
        bump_blob = pack_load(&pk, "bump.bin", 0);  /* normal-map relief (tools/export_normal.py, src/bump.c) */
        fx_blob = pack_load(&pk, "fx.bin", 0);      /* effects: bonfire fire, MSB SFX (tools/export_effects.py, src/fx.c) */
#endif
        dg_blob = pack_load(&pk, "dg.bin", 0);      /* MSB draw groups per chunk (tools/drawgroups.py, src/drawgrp.c) */
#ifndef NO_PVS
        pvs_blob = pack_load(&pk, "pvs.bin", 0);    /* visibility per camera cell (tools/pvs_bake.c, src/pvs.c) */
#endif
        col_blob = pack_load(&pk, "collision.bin", 0);
        enemy_blob = pack_load(&pk, "enemies.bin", 0);  /* MSB enemy parts (tools/export_enemies.py) */
        obj_blob = pack_load(&pk, "objects.bin", 0);    /* breakable objects (tools/export_objects.py) */
        ev_blob = pack_load(&pk, "events.bin", 0);      /* EMEVD bytecode (tools/export_emevd.py, src/event.c) */
        evc_blob = pack_load(&pk, "evcommon.bin", 0);
        lua_blob = pack_load(&pk, "ai.lua0", &lua_size);  /* the area's AI scripts (tools/lua50.py --pack) */
    }
    pack_close(&pk);
    /* HUD / menu: DSR's menu sprites, font, icons and English text (tools/export_menu.py) */
    void *menu_blob = pack_open(&pk, PACK_ROOT "menu.pak") ? pack_load(&pk, "menu.bin", 0) : 0;
    pack_close(&pk);
    t_load = cpu_ticks() - t_load;
    printf("DSRPROTO mem: elf %u, packs %u (read %u), heap end %u, stack %u, free %u; load %u ms, IOP wait %u ms\n",
           (unsigned)_end - 0x100000, pack_stats.heap, pack_stats.read, (unsigned)sbrk(0), (unsigned)_stack_size,
           ram_free(), t_load / 294912, pack_stats.wait_ticks / 294912);
    int model_ok = model_init(model_blob);
#ifdef HELM_OFF
    if (model_ok)
        model_helmet(0);  /* test builds: start with the helm off (make EE_DBGINFOFLAGS=-DHELM_OFF) */
#endif
    int vram_free = model_ok ? model_upload(gs) : -1;
#ifndef R2_OFF
    unsigned ram_before = ram_free();
    if (vram_free >= 0 && model_compact(model_blob, model_size))  /* R2: verts / tris / UVs / pixels now live in the
                                                                     * VU1 streams and VRAM */
        printf("DSRPROTO player model compacted: RAM free %u -> %u\n", ram_before, ram_free());
#endif
    int map_ok = (world ? world_boot_map() : map_init(map_blob)) && map_upload(gs) == 0;
    if (world)
        world_stream_open();
    else
        map_stream_open(PACK_ROOT AREA ".pak");  /* DSM5: near cells stream from the area pack (src/mapstream.c) */
    loading_tick(1);  /* the loading screen moves into the map pool now, before the uploads below reach the top of VRAM */
    if (map_ok && !bump_init(gs, bump_blob, map_chunk_count()))
        printf("DSRPROTO bump: relief off\n");
    int col_ok = map_ok && col_init(col_blob);  /* collision.bin: without it the player walks on the navmesh */
    if (col_ok)
        dg_init(dg_blob, map_chunk_count(), (unsigned)col_tris());
    if (map_ok)
        map_pvs_init(pvs_blob);
    hud_init(gs, menu_blob);  /* 194 KB VRAM for its two atlases */
    /* enemies: character packs data/c####.pak + enemyCommon.esd (data/common.pak), placed from enemies.bin */
    /* "option noenemies" (breakables tests): no enemy roster, RAM / VRAM for the pots, no AI in the shots */
    int enemies = map_ok && !(test_opts & TEST_NOENEMIES) ? enemy_init(gs, enemy_blob, lua_blob, (int)lua_size) : 0;
    loading_tick(1);
    int breakables = map_ok ? objects_init(gs, obj_blob) : 0;  /* pots: intact + pre-fractured models */
    if (map_ok && col_ok && ev_blob && event_init(evc_blob))  /* after enemies + collision */
        event_block_on(0, ev_blob);
    else
        printf("DSRPROTO events: off\n");
    if (!(map_ok && fx_init(gs, fx_blob)))  /* effect textures to VRAM; the blob stays (particle systems, lights) */
        free(fx_blob);
    world_systems(gs);  /* other world blocks' enemy / breakable / effect types (before the near-cell arena) */
    loading_tick(1);
    combat_sweep_hook = objects_sweep_hook;  /* every weapon sweep (player and enemies) can break them */
    free(lua_blob);
    if (vram_free >= 0)
        vram_free = r_vram_avail(gs);
    printf("DSRPROTO vram: free %d (%d after the last page), r_vram %d blocks in %d pages (gsKit sizes: %d blocks)\n",
           r_vram_avail(gs), 4 * 1024 * 1024 - (int)gs->CurrentPointer, r_vram_used, r_vram_pages, r_vram_gskit);
    /* cut-out foliage: texels with CLUT alpha 0 are dropped (AFAIL keep = neither colour nor depth written) */
    gs->Test->ATE = 1;
    gs->Test->ATST = 5;  /* GEQUAL */
    gs->Test->AREF = 0x40;
    gs->Test->AFAIL = 0;
    gsKit_set_test(gs, GS_ATEST_ON);
    gsKit_set_clamp(gs, GS_CMODE_REPEAT);  /* DSR UVs run past 0..1 */

    Player pl;
    player_init(&pl);
    if (map_ok) {
        map_spawn(&pl.pos, &pl.yaw);
        pl.on_map = 1;
    }
    save_apply_player(&pl);  /* saved position (same area), HP, deaths */
    bonfire_init(AREA);      /* bloodstain / respawn point (src/bonfire.c, A4) */
    test_build_spawn(&pl, col_ok);  /* TEST_SPAWN / COMBAT_DUMMY check builds */
    float cam_yaw = pl.yaw, cam_pitch = 0.25f, cam_dist = 4.0f, idle = 99, avg_dt = 1.0f / 60;
    unsigned last = cpu_ticks();
    int tris_drawn = 0;
    float t_cull = 0, t_char = 0, t_wait = 0;  /* ms, smoothed */
    unsigned short prev = 0;
    int hud_debug = debug_hud_default(test_opts);  /* Select: fps / VU1 / collision / load overlay (debug_hud.c) */

    for (;;) {
        PROF_FRAME();
        unsigned now = cpu_ticks();
        float dt = (now - last) / 294912000.0f;
        float dt_real = dt;  /* test stats: the frame time before the clamps */
#ifdef SLOW_FRAME_LOG  /* make EE_DBGINFOFLAGS=-DSLOW_FRAME_LOG: frames over 100 ms in the EE log (ISO stalls) */
        if (dt_real > 0.1f)
            printf("DSRPROTO slow frame %d ms\n", (int)(dt_real * 1000));
#endif
        last = now;
        avg_dt += (dt - avg_dt) * 0.1f;  /* real frame time, before the clamps below */
        if (dt > 1.0f / 15)
            dt = 1.0f / 15;  /* below 15 fps the game slows down instead of skipping */
        if (dt < 1.0f / 60)
            dt = 1.0f / 60;
        Input in = {0};
        unsigned short held = prev;
        if (pad_input(&in, &prev))
            idle = 0;
        else
            idle += dt;
        unsigned short pressed = prev & ~held;
        pressed |= test_pad();  /* test script "pad" presses */
        if (pressed & PAD_SELECT)
            hud_debug = !hud_debug;
        if (pressed & PAD_L3) {  /* debug: helm off / on, to see the face (DSR's hide rule, model_helmet) */
            static int helm_off;
            model_helmet(!(helm_off = !helm_off));
        }
        if (bonfire_input(pressed) || menu_update(pressed)) {  /* bonfire / Start menu: no movement or attacks */
            in = (Input){0};
            idle = 0;
        }
        int demo = idle > 5 && (test_opts & TEST_DEMO) && !test_active();  /* test-only ("option demo") */
        if (test_active() || demo)  /* test script (pad ignored), else the idle demo when a script asked for it */
            test_input(&in, &pl, &cam_yaw, &cam_pitch, dt, demo);
        test_build_input(&in, &pl, &cam_yaw, &cam_pitch, dt);  /* COL_TEST / ESD_TEST / CHAR_CLOSEUP / ENEMY_VIEW */

        cam_yaw += in.cx * 2.5f * dt;
        cam_pitch += in.cy * 1.5f * dt;
        /* LockCamParam row 0: rotRangeMinX -40 degrees (camera below the target, looking up) */
        cam_pitch = cam_pitch < -0.698f ? -0.698f : cam_pitch > 1.2f ? 1.2f : cam_pitch;
        /* Frame order: the player, camera and world first, then the map chain is kicked and VU1 / the GS draw it while
         * the EE runs enemies, objects and events (refactor target 1, step A). What the map chain reads (camera, draw
         * groups, streamed cells) is settled before the kick; what those updates change shows from the next frame */
        int frozen = test_frozen();  /* test scripts freeze the world while a shot is taken (and for render-only runs) */
        if (!frozen) {
            PROF_MARK(PROF_PLAYER);
            player_update(&pl, &in, cam_yaw, dt);
            combat_player_frame(&pl, player_attack_anim(&pl), pl.t, dt);  /* hurtbox sync + weapon hit sweeps */
            /* fog walls / gates as the last frame's event_update left them: before the camera, which follows the feet */
            event_push_player((float *)&pl.pos, COL_PLAYER_R, COL_PLAYER_H);  /* V3 as float[3]: &pl.pos.x trips gcc 15 -Wstringop-overflow */
            PROF_MARK(PROF_ENEMY);
            enemy_stream(&pl);  /* type loads: arena allocations and PATH3 texture uploads stay before the map kick */
        }

        PROF_MARK(PROF_CAMERA);
        /* third-person orbit camera behind the player */
        /* LockCamParam row 0: camDistTarget 4.0 m, chrOrgOffset_Y 1.42 m */
        /* the follow height eases after the feet (steps, landings) instead of copying them; teleports snap */
        static float cam_y = -1e9f;
        float ky = 1 - expf(-dt / 0.15f);
        cam_y = fabsf(pl.pos.y - cam_y) > 3 ? pl.pos.y : cam_y + (pl.pos.y - cam_y) * ky;
        V3 target = v3(pl.pos.x, cam_y + 1.42f, pl.pos.z);
        float d = test_build_cam_dist(4.0f);
        V3 eye = v3add(target, v3(-sinf(cam_yaw) * cosf(cam_pitch) * d, sinf(cam_pitch) * d,
                                  -cosf(cam_yaw) * cosf(cam_pitch) * d));
        /* camera collision: pull in at once in front of the first wall hit (0.3 m margin, near plane 0.25 m), ease
         * back out at 4 m/s */
        float want = d;
        ColHit ch;
        float ray0[3] = {target.x, target.y, target.z}, ray1[3] = {eye.x, eye.y, eye.z};
        if (col_ok && col_ray(ray0, ray1, COL_CAMERA, &ch))
            want = ch.t * d - 0.3f > 0.2f ? ch.t * d - 0.3f : 0.2f;
        cam_dist = want < cam_dist ? want : cam_dist + 4.0f * dt > want ? want : cam_dist + 4.0f * dt;
        eye = v3add(target, v3scale(v3sub(eye, target), cam_dist / d));
        test_build_camera(&eye, &target);  /* ENEMY_VIEW check build */
        r_camera(eye, target);

        world_update(&pl.pos.x, dt);  /* background block reads; turns blocks on / off (src/world.c) */
        loading_frame();  /* the boot's / a death's loading screen closes once this frame's loads settled */
        snd_map(world_current() >= 0 ? world_map(world_current()) : AREA);  /* area bank, ambience (src/sound.c) */
        snd_update(dt);  /* listener = this camera; emitters, voices, streams; one RPC to the IOP */
        PROF_MARK(PROF_MAP);
        if (map_ok) {
            /* Z clear and the map in one VU1 chain (PATH2 + PATH1), then gsKit (PATH3) for the rest */
            unsigned t0 = cpu_ticks();
            vu1_begin(0);
            z_clear(gs);
            map_set_player(pl.pos);
            tris_drawn = map_draw(eye);
            vu1_kick();  /* VU1 draws the map while the EE runs the game logic and poses the characters below */
            t_cull += ((cpu_ticks() - t0) / 294912.0f - t_cull) * 0.1f;
        } else {
            gsKit_clear(gs, GS_SETREG_RGBAQ(0xFF, 0xFF, 0xFF, 0x80, 0));
            r_ground(40, 2);
            gsKit_queue_exec(gs);  /* before the character's VU1 chain draws on top */
            dmaKit_wait_fast();
        }
        if (!frozen) {  /* nothing here may touch what the map chain reads (mapstream cells, PATH2/3 GS registers) */
            PROF_MARK(PROF_ENEMY);
            enemy_update(&pl, dt);  /* AI, enemyCommon ESD, movement, attack sweeps on the player */
            PROF_MARK(PROF_OBJECTS);
            objects_update(&pl, dt);  /* breakables: push-out, roll / landing breaks, piece physics */
            event_update(&pl, dt);    /* EMEVD instances (src/event.c); fog walls / gates push the player next frame */
            bonfire_update(&pl, dt);  /* bloodstain pickup (src/bonfire.c) */
        }
        PROF_MARK(PROF_CHARS);
        unsigned t1 = cpu_ticks();
        vu1_begin(1);
#ifndef MAP_PANO_R  /* panorama capture builds (render-distance, map.c): the far map only */
        player_draw(&pl);  /* bone matrices on the EE, skinning + triangles on VU1 (vu1skin.vsm) */
        enemy_draw();      /* same path, one bone palette per enemy */
        objects_draw();    /* breakables: intact pots or their pieces, one palette per object */
#endif
        vu1_kick();
        fx_update(dt, eye);  /* effects: EE particles while VU1 draws; gsKit packets run after vu1_wait (blended last) */
#ifndef MAP_PANO_R
        fx_draw();
#endif
        bonfire_draw();  /* the bloodstain marker */
        debug_draw_world();  /* COMBAT_DUMMY hurtbox, blade capsule */
        t_char += ((cpu_ticks() - t1) / 294912.0f - t_char) * 0.1f;

        /* HUD: DSR's bars, quick slots, souls, YOU DIED (hud.c); Start menu (menu.c); Select: debug overlay */
        PROF_MARK(PROF_HUD);
#ifndef MAP_PANO_R
        hud_draw(&pl, dt);
#endif
        if (demo)
            r_text_outlined(30, 410, 2, "DEMO - PRESS ANY BUTTON");
        if (hud_debug) {
            const DebugHud dh = {cam_yaw, cam_pitch, cam_dist, cam_y, avg_dt, t_cull, t_char, t_wait, t_load, ram_free(),
                                 tris_drawn, vram_free, model_ok, map_ok, col_ok, enemies, breakables};
            debug_hud_draw(&dh, &pl, dt);
        }
        menu_draw(&pl);
        loading_overlay(dt);  /* the fade-in from black after a loading screen */
        PROF_DRAW();

        PROF_MARK(PROF_VU1WAIT);
        unsigned t2 = cpu_ticks();
        fx_kick();   /* FX_VU1: the particle chain, after the characters' */
        vu1_wait();  /* PATH1 must finish before gsKit's PATH3 packets draw over it */
        t_wait += ((cpu_ticks() - t2) / 294912.0f - t_wait) * 0.1f;
        PROF_MARK(PROF_FLIP);
        gsKit_queue_exec(gs);
        flip(gs);
        if (vram_free >= 0)  /* character / block textures come and go (TECH_DEBT world-4) */
            vram_free = r_vram_avail(gs);
        test_frame_end(&pl, avg_dt, dt_real, t_cull, t_char, t_wait, tris_drawn, vram_free);
        save_tick(&pl, dt);  /* requested saves + autosave */
        static unsigned log_frames;
        if (++log_frames % 300 == 0)
            printf("DSRPROTO chain qw: map %d peak %d, chr %d peak %d, segments %d; CLUT loads %d + %d, batches %d\n",
                   vu1_stats.last[0], vu1_stats.peak[0], vu1_stats.last[1], vu1_stats.peak[1], vu1_stats.segments,
                   vu1_stats.clut_loads[0], vu1_stats.clut_loads[1], map_vu1_batches);
        if (log_frames % 300 == 0)  /* indexed map batches so far / EE-split clip triangles / batches clipped on VU1 */
            printf("DSRPROTO idx batches %d, clip tris %d, vu clip batches %d; EE ms so far: idx clip split %u, "
                   "chain waits %u\n", vu1_stats.idx_batches, vu1_stats.idx_clip, vu1_stats.idx_vuclip,
                   (unsigned)(vu1_stats.idx_clip_ticks / 294912),
                   (unsigned)(vu1_stats.wait_ticks / 294912));
        if (log_frames % 300 == 0)  /* EE time in the batch builders so far (map and relief passes) */
            printf("DSRPROTO add ms so far: idx %u (%d calls), ex %u (%d calls)\n",
                   (unsigned)(vu1_stats.add_idx_ticks / 294912), vu1_stats.add_idx_n,
                   (unsigned)(vu1_stats.add_ex_ticks / 294912), vu1_stats.add_ex_n);
#ifdef VU1_TEX_PATH3
        if (log_frames % 300 == 0)
            printf("DSRPROTO tex path3 %d, path2 %d, gates %d\n", vu1_stats.tex_path3, vu1_stats.tex_path2,
                   vu1_stats.tex_gates);
#endif
    }
    return 0;
}
