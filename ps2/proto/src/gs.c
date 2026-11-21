/* gs.c: GS display setup and the VRAM owner table (gs.h). */
#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include "gs.h"

/* the GS words map.c wrote by hand before Polaris 4.1 moved the map files onto gs.h's names: the names keep those values */
_Static_assert((GS_SETREG_TEST(1, 5, 0x40, 0, 0, 0, 1, 2)) == GS_TEST_CUTOUT, "map.c TEST (cut-out)");
_Static_assert((GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 1)) == GS_TEST_ZALWAYS, "map.c TEST (Z clear)");
_Static_assert((GS_SETREG_ALPHA(0, 2, 0, 1, 0)) == GS_ALPHA_ADD, "map.c ALPHA (additive chunks, sky)");
_Static_assert((GS_SETREG_ALPHA(0, 1, 0, 1, 0)) == GS_ALPHA_MIX, "map.c ALPHA (blended chunks)");
_Static_assert((GS_SETREG_ALPHA(0, 1, 2, 1, 0x80)) == GS_ALPHA_FIX(0x80), "map.c ALPHA (horizon / near fades)");
_Static_assert((GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0)) == GS_PRIM_SPRITE_FLAT, "map.c PRIM (Z clear)");
_Static_assert(GSR_ALPHA == 0x42 && GSR_ZBUF == 0x4E && GSR_TEST == 0x47 && GSR_FRAME == 0x4C && GSR_XYZ2 == 0x05 &&
                   GSR_PRIM == 0x00,
               "map.c register addresses");

void gs_display_init(GSGLOBAL *g, u32 disp_buf, int disp_w, int disp_h, int interlaced)
{
    /* gsKit set up 480i for its 448-line buffer: StartY / DH are interlaced raster lines (DY 50, DH 448) */
    int dy, dh;
    if (interlaced) {
        SetGsCrt(GS_INTERLACED, GS_MODE_NTSC, GS_FRAME);
        dy = g->StartY + g->StartYOffset, dh = 2 * disp_h;  /* FFMD 1: DH counts both fields' lines */
    } else {
        SetGsCrt(GS_NONINTERLACED, GS_MODE_NTSC, GS_FRAME);
        dy = (g->StartY + g->StartYOffset) / 2, dh = disp_h;  /* 25: gsKit's own 240p centring of 224 lines */
    }
    GS_SET_PMODE(0, 1, 0, 1, 0, 0x80);
    GS_SET_DISPFB2(disp_buf / 8192, disp_w / 64, g->PSM, 0, 0);
    GS_SET_DISPLAY2(g->StartX + g->StartXOffset, dy, g->MagH, 0, g->DW - 1, dh - 1);
}

/* ---- VRAM owners ---- */

#define GS_BLOCKS (4 * 1024 * 1024 / 256)
const char *const gs_owner_name[GSO_N] = {"frame", "world", "world.pool", "world.relief", "world.clut", "player", "chr",
                                         "obj",   "fx",    "ui"};
static const GsOwner parent[GSO_N] = {GSO_FRAME, GSO_WORLD, GSO_WORLD, GSO_WORLD, GSO_WORLD,
                                      GSO_CHR_PLAYER, GSO_CHR, GSO_OBJ, GSO_FX, GSO_UI};
static const unsigned budget_kb[GSO_N] = {GSB_FRAME_KB, GSB_WORLD_KB, 0, 0, 0, GSB_CHR_PLAYER_KB, GSB_CHR_KB, GSB_OBJ_KB,
                                          GSB_FX_KB, GSB_UI_KB};
_Static_assert(GSB_FRAME_KB + GSB_WORLD_KB + GSB_CHR_PLAYER_KB + GSB_CHR_KB + GSB_OBJ_KB + GSB_FX_KB + GSB_UI_KB +
                       GSB_HOLES_KB + GSB_FREE_KB ==
                   4096,
               "gs_budget.h rows must sum to the GS's 4096 KB");
static unsigned char own[GS_BLOCKS / 2];  /* owner + 1 of the allocation starting at each block (4 bits), 0 = none */
static unsigned used[GSO_N], peak[GSO_N], fold_peak[GSO_N];  /* blocks */
static unsigned char warned[GSO_N];
static unsigned refused[GSO_N], refused_kb[GSO_N];  /* gs_vram_admit refusals per (parent) owner */
static int borrowed;

static unsigned own_get(unsigned b) { return own[b >> 1] >> ((b & 1) * 4) & 15; }
static void own_set(unsigned b, unsigned v) { own[b >> 1] = (own[b >> 1] & ~(15 << ((b & 1) * 4))) | v << ((b & 1) * 4); }

static unsigned folded(GsOwner p)
{
    unsigned n = 0;
    for (int i = 0; i < GSO_N; i++)
        n += parent[i] == p ? used[i] : 0;
    return n;
}

void gs_vram_charge(GsOwner o, u32 vram, unsigned blocks)
{
    if (!vram || o >= GSO_N || vram / 256 >= GS_BLOCKS)  /* GSKIT_ALLOC_ERROR (0): nothing allocated */
        return;
    own_set(vram / 256, o + 1);
    used[o] += blocks;
    if (used[o] > peak[o])
        peak[o] = used[o];
    GsOwner p = parent[o];
    unsigned f = folded(p);
    if (f > fold_peak[p])
        fold_peak[p] = f;
    if (f / 4 > budget_kb[p] && !warned[p]) {  /* Polaris 1.1: report only (2.7 refuses) */
        warned[p] = 1;
        printf("TEST warn: gs vram %s %u KB over its %u KB budget (gs_budget.h)\n", gs_owner_name[p], f / 4, budget_kb[p]);
    } else if (f / 4 <= budget_kb[p])
        warned[p] = 0;
}

/* Polaris 2.7: the owners whose allocations come and go with the world (characters, objects, effects) are refused
 * past their gs_budget.h row, so one of them cannot starve the others or the UI; the rest (frame, world pools, the
 * player, UI) are placed once at boot or on a loading screen and keep reporting only. GS_VRAM_ENFORCE=0: report only
 * for every owner (the Polaris 1.1 behaviour). */
#ifndef GS_VRAM_ENFORCE
#define GS_VRAM_ENFORCE 1
#endif
int gs_vram_admit(GsOwner o, unsigned blocks)
{
    if (o >= GSO_N)
        return 1;
    GsOwner p = parent[o];
    if (!GS_VRAM_ENFORCE || !(p == GSO_CHR || p == GSO_OBJ || p == GSO_FX) || (folded(p) + blocks) / 4 <= budget_kb[p])
        return 1;
    refused[p]++, refused_kb[p] += blocks / 4;
    printf("TEST warn: gs vram refused %s %u KB: %u of its %u KB in use (gs_budget.h)\n", gs_owner_name[p], blocks / 4,
           folded(p) / 4, budget_kb[p]);
    return 0;
}

void gs_vram_credit(u32 vram, unsigned blocks)
{
    if (!vram || vram / 256 >= GS_BLOCKS)
        return;
    unsigned o = own_get(vram / 256);
    if (!o)
        return;
    own_set(vram / 256, 0);
    used[o - 1] -= blocks <= used[o - 1] ? blocks : used[o - 1];
}

#ifdef HEAPMAP
#define VF_N 32
static struct { u32 addr, size; const char *where; } vf[VF_N];
static unsigned nvf;
#endif

u32 gs_vram_fixed(GSGLOBAL *g, u32 size, u8 type, GsOwner o, const char *where)
{
    u32 before = g->CurrentPointer;
    u32 a = gsKit_vram_alloc(g, size, type);
    if (a != GSKIT_ALLOC_ERROR)
        gs_vram_charge(o, a, (g->CurrentPointer - before) / 256);  /* gsKit's rounding is the owner's */
#ifdef HEAPMAP
    if (a != GSKIT_ALLOC_ERROR && nvf < VF_N)
        vf[nvf].addr = a, vf[nvf].size = size, vf[nvf].where = where, nvf++;
#else
    (void)where;
#endif
    return a;
}

void gs_vram_borrow(int bytes) { borrowed += bytes; }
int gs_vram_borrowed(void) { return borrowed; }

unsigned gs_vram_kb(GsOwner o, int fold) { return (fold ? folded(o) : used[o]) / 4; }
unsigned gs_vram_peak_kb(GsOwner o, int fold) { return (fold ? fold_peak[o] : peak[o]) / 4; }

extern int r_vram_holes;  /* render.c: free blocks inside r_vram's pages */

void gs_vram_log(const GSGLOBAL *g, const char *why)
{
    char buf[512];
    int n = 0;
    unsigned sum = 0;
    for (int i = 0; i < GSO_N; i++) {
        sum += used[i];
        if (parent[i] == (GsOwner)i && refused[i])
            n += snprintf(buf + n, sizeof buf - n, " %s %u/%u refused %u (%u KB)", gs_owner_name[i], folded(i) / 4,
                          budget_kb[i], refused[i], refused_kb[i]);
        else if (parent[i] == (GsOwner)i)
            n += snprintf(buf + n, sizeof buf - n, " %s %u/%u", gs_owner_name[i], folded(i) / 4, budget_kb[i]);
        else
            n += snprintf(buf + n, sizeof buf - n, " (%s %u)", gs_owner_name[i], used[i] / 4);
        if (n >= (int)sizeof buf)
            break;
    }
    unsigned holes = r_vram_holes, never = (4 * 1024 * 1024 - g->CurrentPointer) / 256;
    printf("gsvram: %s:%s holes %u free %u load %d; sum %u KB\n", why, buf, holes / 4, never / 4, borrowed >> 10,
           (sum + holes + never) / 4);
}

#ifdef HEAPMAP
/* render.c r_vram_census: the gsKit_vram_alloc allocations with their call sites */
unsigned gs_vram_census_fixed(void)
{
    unsigned fixed = 0;
    for (unsigned i = 0; i < nvf; i++) {
        fixed += vf[i].size;
        printf("vrammap: fixed %s %u KB at 0x%x\n", vf[i].where, vf[i].size >> 10, vf[i].addr);
    }
    for (int i = 0; i < GSO_N; i++)
        printf("vrammap: owner %s %u KB peak %u KB budget %u KB\n", gs_owner_name[i], used[i] / 4, peak[i] / 4,
               budget_kb[i]);
    return fixed;
}
#endif
