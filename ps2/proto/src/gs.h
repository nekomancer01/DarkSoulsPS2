/* gs.h: the GS's state and memory in one place (Polaris 1.1).
 *
 * Registers. Every GS register value the engine writes is named here: the A+D register addresses (GSR_*), the state
 * words the frame uses (depth / alpha tests, blend equations, filters, wrapping, TEXA), the FRAME / ZBUF words built
 * from the screen buffers, the image-transfer header words, and the display registers (gs_display_init in gs.c). Code
 * outside gs.h / gs.c writes GS state through these names; tools/ci/gsown.py (CI job "hygiene") fails on a raw
 * GS_SETREG_<state register>, GS_SET_<display register>, SetGsCrt or gsKit state call anywhere else. map.c keeps its
 * own literals until Polaris 4.1 splits it (streaming work is live there); gs.c asserts they equal the names.
 * Two call sites that write nearly the same word keep two names (the packets stay byte-identical); TECH_DEBT render-16
 * lists the near-duplicates to merge with the order table (Polaris 1.4).
 *
 * VRAM. Every allocation is charged to an owner (GsOwner) and the owner is stored with it (a 4-bit owner per 256-byte
 * GS block), so a free credits the owner that paid. The budgets are gs_budget.h's rows; the rows plus page slack plus
 * never-taken pages sum to 4096 KB. Over-budget reports ("TEST warn: gs vram ..."); since Polaris 2.7 the streaming
 * owners (characters, objects, effects) are refused past their row (gs_vram_admit).
 * The loading screen's VRAM is borrowed (from the map pool, or the free top at boot), so it is counted on its own
 * (gs_vram_borrow) and checked to be back at 0 after each loading screen. */
#ifndef GS_H
#define GS_H

#include <gsKit.h>
#include "gs_budget.h"

/* A+D register addresses (context 1) */
enum {
    GSR_PRIM = 0x00, GSR_RGBAQ = 0x01, GSR_ST = 0x02, GSR_UV = 0x03, GSR_XYZ2 = 0x05, GSR_TEX0 = 0x06,
    GSR_CLAMP = 0x08, GSR_TEX1 = 0x14, GSR_MIPTBP1 = 0x34, GSR_TEXA = 0x3B, GSR_FOGCOL = 0x3D, GSR_TEXFLUSH = 0x3F,
    GSR_ALPHA = 0x42, GSR_DTHE = 0x45, GSR_TEST = 0x47, GSR_FRAME = 0x4C, GSR_ZBUF = 0x4E, GSR_BITBLTBUF = 0x50,
    GSR_TRXPOS = 0x51, GSR_TRXREG = 0x52, GSR_TRXDIR = 0x53,
};

/* TEST (depth GEQUAL: larger Z is nearer, main.c) */
#define GS_TEST_ZALWAYS (GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 1))      /* no alpha test, depth ALWAYS (clears, copies) */
#define GS_TEST_ZGE (GS_SETREG_TEST(0, 0, 0, 0, 0, 0, 1, 2))          /* no alpha test, depth GEQUAL (blended draws) */
#define GS_TEST_CUTOUT (GS_SETREG_TEST(1, 5, 0x40, 0, 0, 0, 1, 2))    /* alpha GEQUAL 0x40 else keep, depth GEQUAL */
#define GS_TEST_ZALWAYS_DATE (GS_SETREG_TEST(0, 0, 0, 0, 1, 1, 1, 1)) /* + destination alpha test: alpha bit 1 only */
/* ALPHA: (A - B) x C / 128 + D, A/B/D 0 Cs 1 Cd 2 zero, C 0 As 1 Ad 2 FIX */
#define GS_ALPHA_MIX (GS_SETREG_ALPHA(0, 1, 0, 1, 0))        /* Cs x As + Cd x (1 - As): gsKit's default */
#define GS_ALPHA_ADD (GS_SETREG_ALPHA(0, 2, 0, 1, 0))        /* Cs x As + Cd */
#define GS_ALPHA_FIX(f) (GS_SETREG_ALPHA(0, 1, 2, 1, f))     /* Cs x f + Cd x (1 - f) */
#define GS_ALPHA_RELIEF_DARK (GS_SETREG_ALPHA(1, 2, 2, 2, 0))  /* bump.c: Cd x (1 - ...) relief passes */
#define GS_ALPHA_RELIEF_LIT (GS_SETREG_ALPHA(1, 2, 0, 2, 0))
/* TEX1 / CLAMP / TEXA */
#define GS_TEX1_POINT (GS_SETREG_TEX1(0, 0, 0, 0, 0, 0, 0))
#define GS_TEX1_LINEAR (GS_SETREG_TEX1(0, 0, 1, 1, 0, 0, 0))
#define GS_TEX1_FILTER(f) (GS_SETREG_TEX1(0, 0, f, f, 0, 0, 0))
/* a mip chain: max level, MMIN, L / K (vu1.c vu1_mip) */
static inline u64 gs_tex1_mip(int mxl, int mmin, int k) { return GS_SETREG_TEX1(0, mxl, 1, mmin, 0, 0, k); }
/* MIPTBP1: levels 1-3 base pointers (TBW 2: the PSMT8 width texcache.c gives every level) */
static inline u64 gs_miptbp1(u32 tbp1, u32 tbp2, u32 tbp3) { return GS_SETREG_MIPTBP1(tbp1, 2, tbp2, 2, tbp3, 2); }
#define GS_CLAMP_REPEAT (GS_SETREG_CLAMP(0, 0, 0, 0, 0, 0))
#define GS_CLAMP_CLAMP (GS_SETREG_CLAMP(1, 1, 0, 0, 0, 0))
#define GS_TEXA_GSKIT (GS_SETREG_TEXA(0x80, 0, 0x80))  /* gsKit's: 16-bit alpha bit 0 -> 0x80, 1 -> 0x80 */
#define GS_TEXA_BIT (GS_SETREG_TEXA(0, 0, 0x80))       /* alpha bit 0 -> 0, 1 -> 0x80 (DoF marks, map textures) */
/* PRIM words of the full-screen passes */
#define GS_PRIM_SPRITE_FLAT (GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0))
#define GS_PRIM_SPRITE_UV (GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0))      /* textured, UV */
#define GS_PRIM_SPRITE_UV_ABE (GS_SETREG_PRIM(GS_PRIM_PRIM_SPRITE, 0, 1, 0, 1, 0, 1, 0, 0))  /* + alpha blend */

/* FRAME / ZBUF from the screen buffers gsKit set up (byte addresses) */
static inline u64 gs_frame(u32 vram, int width, int psm, u32 mask)
{
    return GS_SETREG_FRAME_1(vram / 8192, width / 64, psm, mask);
}
static inline u64 gs_zbuf(const GSGLOBAL *g, int no_write) { return GS_SETREG_ZBUF(g->ZBuffer / 8192, g->PSMZ, no_write); }

/* Image transfer header words, in the order the GS takes them: BITBLTBUF, TRXPOS, TRXREG, TRXDIR (then the image) */
static inline u64 gs_blt_to(u32 tbp, int tbw, int psm) { return GS_SETREG_BITBLTBUF(0, 0, 0, tbp, tbw, psm); }
static inline u64 gs_blt(u32 sbp, int sbw, int spsm, u32 dbp, int dbw, int dpsm)
{
    return GS_SETREG_BITBLTBUF(sbp, sbw, spsm, dbp, dbw, dpsm);
}
#define GS_TRXPOS_0 (GS_SETREG_TRXPOS(0, 0, 0, 0, 0))
static inline u64 gs_trxreg(int w, int h) { return GS_SETREG_TRXREG(w, h); }
#define GS_TRXDIR_HOST (GS_SETREG_TRXDIR(0))   /* host -> local (upload) */
#define GS_TRXDIR_LOCAL (GS_SETREG_TRXDIR(2))  /* local -> local (copy) */

/* the 2D layer's state calls (g2d.c, Polaris 1.6: gsKit's queue is gone) */
void g2d_set_test(GSGLOBAL *gs, u64 which);
void g2d_set_primalpha(GSGLOBAL *gs, u64 alpha);
void g2d_set_clamp(GSGLOBAL *gs, u8 mode);
static inline void gs_test(GSGLOBAL *g, u64 which) { g2d_set_test(g, which); }
static inline void gs_primalpha(GSGLOBAL *g, u64 alpha) { g2d_set_primalpha(g, alpha); }
static inline void gs_clamp(GSGLOBAL *g, u8 mode) { g2d_set_clamp(g, mode); }

/* Display: CRT mode, PMODE, DISPFB2 and DISPLAY2 for the one 224-line display buffer (main.c's flip() fills it). */
void gs_display_init(GSGLOBAL *g, u32 disp_buf, int disp_w, int disp_h, int interlaced);

/* VRAM owners. The WORLD_* rows are WORLD's sub-rows (one budget for all of them). */
typedef enum {
    GSO_FRAME, GSO_WORLD, GSO_WORLD_POOL, GSO_WORLD_RELIEF, GSO_WORLD_CLUT, GSO_CHR_PLAYER, GSO_CHR, GSO_OBJ, GSO_FX,
    GSO_UI, GSO_N
} GsOwner;
extern const char *const gs_owner_name[GSO_N];
/* charge an allocation of `blocks` 256-byte blocks at byte address vram to an owner / credit it back on free
 * (the owner stored at the charge) */
void gs_vram_charge(GsOwner o, u32 vram, unsigned blocks);
void gs_vram_credit(u32 vram, unsigned blocks);
/* Polaris 2.7: may owner o take `blocks` more? 0 = refused (characters / objects / effects past their gs_budget.h row;
 * logged and counted per owner in the "gsvram:" line); the r_vram_* allocators ask before placing */
int gs_vram_admit(GsOwner o, unsigned blocks);
/* gsKit_vram_alloc charged to an owner: the bytes gsKit's pointer moved (its rounding included) */
u32 gs_vram_fixed(GSGLOBAL *g, u32 size, u8 type, GsOwner o, const char *where);
/* the loading screen's borrowed bytes (+ when it places itself, - when it lets go) */
void gs_vram_borrow(int bytes);
int gs_vram_borrowed(void);
/* KB in use per owner (sub-rows folded into WORLD when fold), and the high-water mark */
unsigned gs_vram_kb(GsOwner o, int fold);
unsigned gs_vram_peak_kb(GsOwner o, int fold);
/* one "gsvram:" log line: every owner's KB / budget, page slack, never-taken pages, sum (4096) */
void gs_vram_log(const GSGLOBAL *g, const char *why);
#ifdef HEAPMAP
unsigned gs_vram_census_fixed(void);  /* "vrammap:" lines of the gsKit allocations and owners; their bytes */
#endif

#endif
