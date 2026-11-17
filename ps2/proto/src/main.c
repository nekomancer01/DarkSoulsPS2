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
#include "mem.h"
#include "frame.h"
#include "g2d.h"
#include "player.h"
#ifndef DPAD_STICK
#define DPAD_STICK 0  /* 1 = the d-pad moves (the old path, keyboard-mapped emulators) */
#endif
#include "model.h"
#include "map.h"
#include "bump.h"
#include "drawgrp.h"
#include "vu1.h"
#include "collision.h"
#include "combat.h"
#include "chrstat.h"
#include "status.h"
#include "pack.h"
#include "enemy.h"
#include "objects.h"
#include "fx.h"
#include "hud.h"
#include "menu.h"
#include "items.h"
#include "lpass.h" /* "logic: pass ms" breakdown (Burg plan B6) */
#include "prof.h"  /* PROF_* hooks: empty except in make PROFILE=1 (tools-hwperf) */
#include "hitch.h"  /* per-frame worst case, hitches, churn, RAM over time ("hitch:" EE log lines) */
#include "testscript.h"
#include "pickup.h"
#include "debug_hud.h"  /* Select overlay, debug world drawing */
#include "world.h"
#include "save.h"  /* A3: progression + event flags, loaded at boot, saved on request / autosave */
#include "event.h"  /* A2: EMEVD runtime */
#include "bonfire.h"  /* A4: rest, death, bloodstain */
#include "loading.h"  /* DSR's loading screen over blocking loads */
#include "sound.h"  /* A12: SPU2 sound (src/sound.c, iop/dsrsnd) */
#include "gamedata.h"  /* player / combat / progress values from c0000.pak (the ELF holds no game data) */
#include <unistd.h>
#include "ticks.h"

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
#if DPAD_STICK
    /* DPAD_STICK=1 (the old path): the d-pad works as a digital left stick (keyboard-mapped emulators) */
    if (btn & PAD_UP) in->my = 1;
    if (btn & PAD_DOWN) in->my = -1;
    if (btn & PAD_LEFT) in->mx = -1;
    if (btn & PAD_RIGHT) in->mx = 1;
#else
    /* the d-pad as in DSR: left / right switch the hand's weapon, down cycles the quick items, up the spells */
    in->dpad = ((btn & PAD_UP) && !(*prev & PAD_UP) ? 1 : 0) | ((btn & PAD_DOWN) && !(*prev & PAD_DOWN) ? 2 : 0) |
               ((btn & PAD_LEFT) && !(*prev & PAD_LEFT) ? 4 : 0) | ((btn & PAD_RIGHT) && !(*prev & PAD_RIGHT) ? 8 : 0);
#endif
    in->r1 = (btn & PAD_R1) && !(*prev & PAD_R1);
    in->circle = (btn & PAD_CIRCLE) != 0;
    in->r1_held = (btn & PAD_R1) != 0, in->r2_held = (btn & PAD_R2) != 0;
    in->l1_held = (btn & PAD_L1) != 0, in->l2_held = (btn & PAD_L2) != 0;
    in->r3 = (btn & PAD_R3) && !(*prev & PAD_R3);
    in->square = (btn & PAD_SQUARE) && !(*prev & PAD_SQUARE);
    in->triangle = (btn & PAD_TRIANGLE) && !(*prev & PAD_TRIANGLE);
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
    u64 frame = gs_frame(gs->ScreenBuffer[gs->ActiveBuffer & 1], gs->Width, gs->PSM, 0);
#define REG(v, a) (r[2 * n] = (v), r[2 * n + 1] = (a), n++)
    REG(holes ? frame : frame | 0xFFFFFFFFull << 32, GSR_FRAME);
    REG(GS_TEST_ZALWAYS, GSR_TEST);  /* ZTE, ZTST ALWAYS */
    REG(gs_zbuf(gs, 0), GSR_ZBUF);
    REG(GS_PRIM_SPRITE_FLAT, GSR_PRIM);
    REG(GS_SETREG_RGBAQ(0xFF, 0x00, 0xFF, 0x80, 0), GSR_RGBAQ);
    for (int x = 0; x < gs->Width; x += 64) {
        int x1 = x + 64 < gs->Width ? x + 64 : gs->Width;
        REG(GS_SETREG_XYZ2((x << 4) + gs->OffsetX, gs->OffsetY, 0), GSR_XYZ2);
        REG(GS_SETREG_XYZ2((x1 << 4) + gs->OffsetX, (gs->Height << 4) + gs->OffsetY, 0), GSR_XYZ2);
    }
    REG(frame, GSR_FRAME);
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
    disp_buf = gs_vram_fixed(gs, gsKit_texture_size(R_SCREEN_W, DISP_H, gs->PSM), GSKIT_ALLOC_SYSBUFFER, GSO_FRAME,
                             "display buffer");
#ifdef V1B_INTERLACED
    gs_display_init(gs, disp_buf, R_SCREEN_W, DISP_H, 1);
#else
    gs_display_init(gs, disp_buf, R_SCREEN_W, DISP_H, 0);
#endif
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
    (REG(GS_SETREG_XYZ2(gs->OffsetX, gs->OffsetY, z), GSR_XYZ2),                         \
     REG(GS_SETREG_XYZ2((R_SCREEN_W << 4) + gs->OffsetX, (R_SCREEN_H << 4) + gs->OffsetY, z), GSR_XYZ2))
    const u64 draw_a = gs_frame(gs->ScreenBuffer[0], R_SCREEN_W, gs->PSM, 0x00FFFFFF);
    const u64 disp_a = gs_frame(disp_buf, R_SCREEN_W, gs->PSM, 0x00FFFFFF);
    const u64 tex = GS_SETREG_TEX0(gs->ScreenBuffer[0] / 256, R_SCREEN_W / 64, gs->PSM, 9, 9, 1, 1, 0, 0, 0, 0, 0);
    /* clear the draw buffer's alpha bits (Z always, not written) */
    REG(draw_a, GSR_FRAME);
    REG(GS_TEST_ZALWAYS, GSR_TEST);
    REG(GS_PRIM_SPRITE_FLAT, GSR_PRIM);
    REG(GS_SETREG_RGBAQ(0, 0, 0, 0, 0), GSR_RGBAQ);
    FULL(0);
    REG(GS_TEXA_BIT, DOF_TEXA);  /* the copied alpha bit: 0 -> 0, 1 -> 0x80 */
    REG(0, GSR_DTHE);  /* DTHE off: the 16-bit dither would add its pattern again on every blended tap */
    for (int i = DOF_BANDS; i >= 1; i--) {
        /* a pixel between bands i and i + 1 gets bands 1..i: 1 - prod (1 - a_j) = mul x i / N */
        float z = b0 + (b1 - b0) * (i - 0.5f) / DOF_BANDS, s = mul * i / DOF_BANDS, s0 = mul * (i - 1) / DOF_BANDS;
        float a = 1 - (1 - s) / (1 - s0);
        float x = 1 - sqrtf(1 - a);  /* two taps blended in turn: 1 - (1 - x)^2 = a */
        int fix = (int)(x * 128 + 0.5f);
        /* mark: alpha 1 where the scene lies beyond z */
        REG(draw_a, GSR_FRAME);
        REG(GS_TEST_ZGE, GSR_TEST);
        REG(GS_PRIM_SPRITE_FLAT, GSR_PRIM);
        REG(GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0), GSR_RGBAQ);
        FULL((u32)(65535.0f * R_NEAR / z));
        REG(0, GSR_TEXFLUSH);  /* TEXFLUSH: the marks are read back as texture */
        /* marks into the display buffer's alpha (point sampled, line 2y) */
        REG(disp_a, GSR_FRAME);
        REG(GS_TEST_ZALWAYS, GSR_TEST);
        REG(tex, GSR_TEX0);
        REG(GS_TEX1_POINT, GSR_TEX1);
        REG(GS_PRIM_SPRITE_UV, GSR_PRIM);
        for (int x0 = 0; x0 < R_SCREEN_W; x0 += 64) {
            REG(GS_SETREG_UV((x0 << 4) + 8, 8), GSR_UV);
            REG(GS_SETREG_XYZ2((x0 << 4) + gs->OffsetX, gs->OffsetY, 0), GSR_XYZ2);
            REG(GS_SETREG_UV(((x0 + 64) << 4) + 8, 8 + (R_SCREEN_H << 4)), GSR_UV);
            REG(GS_SETREG_XYZ2(((x0 + 64) << 4) + gs->OffsetX, (DISP_H << 4) + gs->OffsetY, 0), GSR_XYZ2);
        }
        /* two offset bilinear taps over the marked pixels: Cd + (Cs - Cd) x fix / 128 */
        REG(gs_frame(disp_buf, R_SCREEN_W, gs->PSM, 0), GSR_FRAME);
        REG(GS_TEST_ZALWAYS_DATE, GSR_TEST);
        REG(GS_SETREG_TEX0(gs->ScreenBuffer[0] / 256, R_SCREEN_W / 64, gs->PSM, 9, 9, 0, 1, 0, 0, 0, 0, 0), GSR_TEX0);
        REG(GS_TEX1_LINEAR, GSR_TEX1);
        REG(GS_ALPHA_FIX(fix), GSR_ALPHA);
        REG(GS_PRIM_SPRITE_UV_ABE, GSR_PRIM);
        for (int t = 0; t < 2; t++) {
            /* the sprite moves, not its UV (UV is unsigned: a negative start wraps): tap 0 shows texel x - DOF_DX,
             * lines 2y - 2 / 2y - 1 at (x, y), tap 1 texel x + DOF_DX, lines 2y + 2 / 2y + 3; the sharp copy has
             * x, lines 2y / 2y + 1. The uncovered edge column / line keeps the sharp copy. */
            int dx = (t ? -DOF_DX : DOF_DX) << 4, dy = t ? -16 : 16;
            for (int x0 = 0; x0 < R_SCREEN_W; x0 += 64) {
                REG(GS_SETREG_UV(x0 << 4, 16), GSR_UV);
                REG(GS_SETREG_XYZ2((x0 << 4) + dx + gs->OffsetX, dy + gs->OffsetY, 0), GSR_XYZ2);
                REG(GS_SETREG_UV((x0 + 64) << 4, 16 + (R_SCREEN_H << 4)), GSR_UV);
                REG(GS_SETREG_XYZ2(((x0 + 64) << 4) + dx + gs->OffsetX, (DISP_H << 4) + dy + gs->OffsetY, 0), GSR_XYZ2);
            }
        }
    }
    REG(GS_TEXA_GSKIT, DOF_TEXA);  /* gsKit's */
    REG(1, GSR_DTHE);  /* DTHE back on (main: gs->Dithering) */
    REG(gs->PrimAlpha, GSR_ALPHA);
#undef FULL
#undef REG
    return n;
}

static void flip(GSGLOBAL *gs)
{
    static u64 pk[2 * 64 + 2 * 128 * DOF_BANDS] __attribute__((aligned(64)));
    static unsigned last_flip;
    gsKit_vsync_wait();
    for (int f = 1; f < FPS_CAP_FIELDS && ticks() - last_flip < (unsigned)(f * FIELD_TICKS + FIELD_TICKS / 2); f++)
        gsKit_vsync_wait();
    last_flip = ticks();
#ifdef V1B_FIELD
    int odd = (*GS_CSR >> 13) & 1, filter = 0, v0 = odd * 16 + 8;  /* texel centre of line 2y + field */
#else
    int filter = 1, v0 = 16;  /* between the centres of lines 2y and 2y + 1 */
#endif
    int n = 0;
    u64 *p = pk + 2;
#define REG(v, a) (*p++ = (v), *p++ = (a), n++)
    REG(0, GSR_TEXFLUSH);  /* TEXFLUSH: the draw buffer is read as a texture right after being drawn */
    REG(gs_frame(disp_buf, R_SCREEN_W, gs->PSM, 0), GSR_FRAME);
    REG(gs_zbuf(gs, 1), GSR_ZBUF);
    REG(GS_TEST_ZALWAYS, GSR_TEST);
    REG(GS_SETREG_TEX0(gs->ScreenBuffer[0] / 256, R_SCREEN_W / 64, gs->PSM, 9, 9, 0, 1, 0, 0, 0, 0, 0), GSR_TEX0);
    REG(GS_TEX1_FILTER(filter), GSR_TEX1);
    REG(GS_CLAMP_CLAMP, GSR_CLAMP);
    REG(GS_PRIM_SPRITE_UV, GSR_PRIM);
    for (int x = 0; x < R_SCREEN_W; x += 64) {
        REG(GS_SETREG_UV(x << 4, v0), GSR_UV);
        REG(GS_SETREG_XYZ2((x << 4) + gs->OffsetX, gs->OffsetY, 0), GSR_XYZ2);
        REG(GS_SETREG_UV((x + 64) << 4, v0 + (R_SCREEN_H << 4)), GSR_UV);
        REG(GS_SETREG_XYZ2(((x + 64) << 4) + gs->OffsetX, (DISP_H << 4) + gs->OffsetY, 0), GSR_XYZ2);
    }
    {
        int k = dof_regs(gs, p);
        p += 2 * k, n += k;
    }
    /* back to the draw buffer and the state the frame expects (main's TEST, repeat wrapping) */
    REG(gs_frame(gs->ScreenBuffer[0], R_SCREEN_W, gs->PSM, 0), GSR_FRAME);
    REG(gs_zbuf(gs, 0), GSR_ZBUF);
    REG(GS_TEST_CUTOUT, GSR_TEST);
    REG(GS_CLAMP_REPEAT, GSR_CLAMP);
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
    GSGLOBAL *gs = gsKit_init_global_custom(R_GSQ_OS, R_GSQ_PER);  /* (render.h: 2.25 MB by default) */
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
    /* its draw and Z buffers from 0 (byte 1 = block 0: address 0 reads as GSKIT_ALLOC_ERROR) */
    gs_vram_charge(GSO_FRAME, 1, gs->CurrentPointer / 256);
    display_init(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gs->Test->ZTST = 2;  /* GEQUAL: larger Z is nearer */
    gs_test(gs, GS_ZTEST_ON);
    r_init(gs);
    vu1_init(gs);
    pad_setup();  /* also SifInitRpc, which the fio reads below need */
    mem_boot_init();  /* static and boot-temp regions (src/mem.h), before the first pack load */
    g2d_init(gs);     /* the 2D writer (src/g2d.h): the loading screen's buffers */
    int test_opts = test_init(argc, argv);  /* test script (host:<file> from argv), before anything it configures */
    holes = test_opts & TEST_HOLES;
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
    chrstat_boot();  /* HP, stamina, defence, attack from the saved stats and equipment (itemx.bin for a moment) */
    /* sound: IOP modules, the player's bank and the boot area's into SPU2 RAM (the IOP reads them: no EE RAM) */
    if (snd_init())
        snd_map(AREA);
    loading_tick(1);
    /* assets from per-area / per-character LZ4 packs (tools/pack.py, src/pack.c); kept for the program's lifetime */
    unsigned t_load = ticks();
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
        /* normal-map relief (tools/export_normal.py, src/bump.c). A world build streams the boot block's like every
         * block's (world.c bump_step, WORLD_BOOT_RELIEF): resident it kept 0.9 MB of the heap from the near arena
         * wherever the player went */
        bump_blob = world && WORLD_BOOT_RELIEF ? NULL : pack_load(&pk, "bump.bin", 0);
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
        lua_blob = pack_load_temp(&pk, "ai.lua0", &lua_size);  /* the area's AI scripts (tools/lua50.py --pack) */
    }
    pack_close(&pk);
    /* HUD / menu: DSR's menu sprites, font, icons and English text (tools/export_menu.py) */
    void *menu_blob = NULL, *shine_blob = NULL;
    unsigned shine_size = 0;
    if (pack_open(&pk, PACK_ROOT "menu.pak")) {
        menu_blob = pack_load_static(&pk, "menu.bin", 0);
        shine_blob = PICKUP ? pack_load(&pk, "shine.bin", &shine_size) : NULL;  /* treasure shine (src/pickup.c) */
        hud_help_init(pack_load_static(&pk, "help.bin", 0));  /* action prompt texts (kept) */
    }
    pack_close(&pk);
    t_load = ticks() - t_load;
    printf("DSRPROTO mem: elf %u, packs %u (read %u), heap end %u, stack %u, free %u; load %u ms, IOP wait %u ms\n",
           (unsigned)_end - 0x100000, pack_stats.heap, pack_stats.read, (unsigned)sbrk(0), (unsigned)_stack_size,
           ram_free(), t_load / 294912, pack_stats.wait_ticks / 294912);
    int model_ok = model_init(model_blob);
#ifdef HELM_OFF
    if (model_ok)
        model_helmet(0);  /* test builds: start with the helm off (make EE_DBGINFOFLAGS=-DHELM_OFF) */
#endif
    HM_TAG("player");  /* HEAPMAP: the player's textures in the VRAM census (render.c) */
    int vram_free = model_ok ? model_upload(gs) : -1;
    HM_TAG0();
#ifndef R2_OFF
    unsigned ram_before = ram_free();
    if (vram_free >= 0 && model_compact(model_blob, model_size))  /* R2: verts / tris / UVs / pixels now live in the
                                                                     * VU1 streams and VRAM */
        printf("DSRPROTO player model compacted: RAM free %u -> %u\n", ram_before, ram_free());
#endif
    int map_ok = (world ? world_boot_map() : map_init(map_blob)) && map_upload(gs) == 0;
    if (world)
        world_stream_open(), map_pano_open(PACK_ROOT AREA ".pak");
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
    pickup_init(gs, shine_blob, shine_size);  /* the treasure shine's textures (~54 KB VRAM); frees the blob */
    /* the area's AI pack (the VM has its copy since enemy_init) stays in boot-temp until mem_boot_done, after the arena:
     * where main freed it, so the arena is sized as before (src/mem.h) */
    int lua_heap = lua_blob && !mem_in_temp(lua_blob);  /* (boot-temp was full: the heap holds it) */
    mem_boot_seal();  /* before the arena is sized from the heap: boot-temp and the static tail are back (src/mem.h) */
    world_systems(gs);  /* other world blocks' enemy / breakable / effect types (before the near-cell arena) */
    mem_boot_done();
    if (lua_heap)
        free(lua_blob);
    lua_blob = NULL;
    loading_tick(1);
    combat_sweep_hook = objects_sweep_hook;
    combat_throw_hook = enemy_throw_try;
    status_init();  /* the player's poison / toxic / bleed / curse (combat_status_hook) */  /* the player's backstabs / ripostes (ThrowParam) */  /* every weapon sweep (player and enemies) can break them */
    if (vram_free >= 0)
        vram_free = r_vram_avail(gs);
    printf("DSRPROTO vram: free %d (%d after the last page), r_vram %d blocks in %d pages (gsKit sizes: %d blocks)\n",
           r_vram_avail(gs), 4 * 1024 * 1024 - (int)gs->CurrentPointer, r_vram_used, r_vram_pages, r_vram_gskit);
    gs_vram_log(gs, "boot");
    /* cut-out foliage: texels with CLUT alpha 0 are dropped (AFAIL keep = neither colour nor depth written) */
    gs->Test->ATE = 1;
    gs->Test->ATST = 5;  /* GEQUAL */
    gs->Test->AREF = 0x40;
    gs->Test->AFAIL = 0;
    gs_test(gs, GS_ATEST_ON);
    gs_clamp(gs, GS_CMODE_REPEAT);  /* DSR UVs run past 0..1 */

    Player pl;
    pickup_prompt_player(&pl);  /* the Pick up item prompt (src/pickup.c) */
    player_init(&pl);
    if (map_ok) {
        map_spawn(&pl.pos, &pl.yaw);
        pl.on_map = 1;
    }
    save_apply_player(&pl);  /* saved position (same area), HP, deaths */
    bonfire_init(AREA);      /* bloodstain / respawn point (src/bonfire.c, A4) */
    test_build_spawn(&pl, col_ok);  /* TEST_SPAWN / COMBAT_DUMMY check builds */
    const FrameBoot fb = {gs, &pl, test_opts, model_ok, map_ok, col_ok, enemies, breakables, vram_free, t_load, AREA,
                          pad_input, z_clear, flip, ram_free};
    frame_run(&fb);  /* the frame loop, in phases (src/frame.c) */
    return 0;
}
