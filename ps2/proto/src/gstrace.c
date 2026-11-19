/* gstrace.c: the effective-draw decoder (Polaris 1.6 gate, OT_HASH builds only). A GIF stream (a DMA chain of CNT / END
 * tags with GIF packets: PACKED, REGLIST, IMAGE) is decoded into the GS register writes it makes, and every primitive
 * the GS would draw is reduced to its effective state at draw time (PRIM, TEX0 / TEX1 / CLAMP, ALPHA, TEST, ZBUF,
 * SCISSOR, TEXA, FOGCOL) and its vertices (XYZ with the window offset, RGBAQ, UV or ST + Q, fog). Old and new 2D paths
 * must produce the same list, in order, on alternating frozen frames: gsKit and g2d skip different redundant writes,
 * so the raw register traces may differ while the draws do not. */
#ifdef OT_HASH
#include <stdio.h>
#include <string.h>
#include "gstrace.h"

enum { R_PRIM = 0x00, R_RGBAQ = 0x01, R_ST = 0x02, R_UV = 0x03, R_XYZF2 = 0x04, R_XYZ2 = 0x05, R_TEX0 = 0x06,
       R_CLAMP = 0x08, R_FOG = 0x0A, R_XYZF3 = 0x0C, R_XYZ3 = 0x0D, R_AD = 0x0E, R_NOP = 0x0F, R_TEX1 = 0x14,
       R_PRMODE = 0x1B, R_TEXA = 0x3B, R_FOGCOL = 0x3D, R_SCISSOR = 0x40, R_ALPHA = 0x42, R_TEST = 0x47,
       R_ZBUF = 0x4E };

/* the state the decoder carries across streams (the GS keeps its registers) */
static u64 reg[0x80];
typedef struct { u64 xyz, rgbaq, uv, st; unsigned fog; } Vtx;
static Vtx q[3];
static int nq, strip_n;
static float cur_q = 1.0f;

static unsigned hash, hash_ui, prims, prims_ui;
static int dump_left;

static int to_ui;  /* the primitive being hashed has no fog: the 2D layer's (HUD, menus, text) */
static void mix(u64 v)
{
    unsigned *h = to_ui ? &hash_ui : &hash;
    *h = (*h ^ (u32)v) * 16777619u;
    *h = (*h ^ (u32)(v >> 32)) * 16777619u;
}

static void emit(int nv)
{
    u64 prim = reg[R_PRIM];
    int tme = prim >> 4 & 1, ctx = prim >> 9 & 1;
    to_ui = 1;  /* the 2D layer draws at Z 0xFFFF (HUD_Z, DRB_Z, LD_Z, render.c ZMAX): every vertex there */
    for (int i = 0; i < nv; i++)
        to_ui &= (q[i].xyz >> 32 & 0xFFFFFF) == 0xFFFF;
    prims_ui += to_ui;
    u64 st[] = {prim & 0x7FF, tme ? reg[R_TEX0 + ctx] : 0, tme ? reg[R_TEX1 + ctx] : 0, tme ? reg[R_CLAMP + ctx] : 0,
                prim >> 6 & 1 ? reg[R_ALPHA + ctx] : 0, reg[R_TEST + ctx], reg[R_ZBUF + ctx], reg[R_SCISSOR + ctx],
                tme ? reg[R_TEXA] : 0, prim >> 5 & 1 ? reg[R_FOGCOL] : 0};
    for (unsigned i = 0; i < sizeof st / sizeof *st; i++)
        mix(st[i]);
    for (int i = 0; i < nv; i++) {
        mix(q[i].xyz), mix(q[i].rgbaq);
        if (tme)
            mix(prim >> 8 & 1 ? q[i].uv : q[i].st);
        if (prim >> 5 & 1)
            mix(q[i].fog);
    }
    prims++;
    if (dump_left > 0 && to_ui) {
        dump_left--;
        printf("gstrace: prim %03llx tex0 %016llx tex1 %llx clamp %llx alpha %llx test %llx zbuf %llx",
               (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2],
               (unsigned long long)st[3], (unsigned long long)st[4], (unsigned long long)st[5],
               (unsigned long long)st[6]);
        for (int i = 0; i < nv; i++)
            printf(" | xyz %llx rgba %llx uv %llx st %llx", (unsigned long long)q[i].xyz,
                   (unsigned long long)q[i].rgbaq, (unsigned long long)q[i].uv, (unsigned long long)q[i].st);
        printf("\n");
    }
}

static void vertex(u64 xyz, unsigned fog, int kick)
{
    Vtx v = {xyz, reg[R_RGBAQ], reg[R_UV], reg[R_ST], fog};
    int type = reg[R_PRIM] & 7;
    static const int need[8] = {1, 2, 2, 3, 3, 3, 2, 0};  /* point, line, line strip, tri, strip, fan, sprite */
    if (type == 7)
        return;
    if (nq < 3)
        q[nq++] = v;
    else if (type == 4)
        q[0] = q[1], q[1] = q[2], q[2] = v;  /* strip: the last two and this one */
    else if (type == 5)
        q[1] = q[2], q[2] = v;  /* fan: the first, the last and this one */
    else if (type == 2)
        q[0] = q[1], q[1] = v;
    if (nq >= need[type] && kick) {
        emit(need[type]);
        if (type == 0 || type == 1 || type == 3 || type == 6)
            nq = 0;
        else if (type == 2)
            q[0] = q[1], nq = 1;
    }
    (void)strip_n;
}

static void write(unsigned r, u64 v)
{
    switch (r) {
    case R_PRIM:
        reg[R_PRIM] = v, nq = 0;
        break;
    case R_XYZ2: vertex(v, 0, 1); break;
    case R_XYZ3: vertex(v, 0, 0); break;
    case R_XYZF2: vertex(v & 0x00FFFFFFFFFFFFFFull, (unsigned)(v >> 56), 1); break;
    case R_XYZF3: vertex(v & 0x00FFFFFFFFFFFFFFull, (unsigned)(v >> 56), 0); break;
    default:
        if (r < 0x80)
            reg[r] = v;
    }
}

/* one PACKED qword for register r */
static void packed(unsigned r, const u64 *w)
{
    u64 lo = w[0], hi = w[1];
    switch (r) {
    case R_AD: write((unsigned)(hi & 0xFF), lo); break;
    case R_RGBAQ: {
        u64 v = (lo & 0xFF) | (lo >> 32 & 0xFF) << 8 | (hi & 0xFF) << 16 | (hi >> 32 & 0xFF) << 24;
        union { float f; u32 u; } qq = {cur_q};
        write(R_RGBAQ, v | (u64)qq.u << 32);
        break;
    }
    case R_ST: {
        union { u32 u; float f; } qq = {(u32)hi};
        cur_q = qq.f;
        reg[R_ST] = lo;
        reg[R_RGBAQ] = (reg[R_RGBAQ] & 0xFFFFFFFFull) | (u64)(u32)hi << 32;
        break;
    }
    case R_UV: write(R_UV, (lo & 0x3FFF) | (lo >> 32 & 0x3FFF) << 16); break;
    case R_XYZF2: case R_XYZ2: {
        int adc = (int)(hi >> 47 & 1);
        u64 x = lo & 0xFFFF, y = lo >> 32 & 0xFFFF;
        if (r == R_XYZF2) {
            u64 z = hi >> 4 & 0xFFFFFF, f = hi >> 36 & 0xFF;
            write(adc ? R_XYZF3 : R_XYZF2, x | y << 16 | z << 32 | f << 56);
        } else
            write(adc ? R_XYZ3 : R_XYZ2, x | y << 16 | (hi & 0xFFFFFFFF) << 32);
        break;
    }
    case R_FOG: write(R_FOG, (hi >> 36 & 0xFF) << 56); break;
    case R_NOP: break;
    default: write(r, lo);
    }
}

/* the GIF packets of one DMA payload */
static void gif(const u64 *p, const u64 *end)
{
    while (p + 2 <= end) {
        u64 t0 = p[0], t1 = p[1];
        unsigned nloop = t0 & 0x7FFF, pre = t0 >> 46 & 1, flg = t0 >> 58 & 3, nreg = t0 >> 60 & 15;
        nreg = nreg ? nreg : 16;
        if (pre)
            write(R_PRIM, t0 >> 47 & 0x7FF);
        p += 2;
        if (flg == 0) {
            for (unsigned i = 0; i < nloop; i++)
                for (unsigned k = 0; k < nreg && p + 2 <= end; k++, p += 2)
                    packed((unsigned)(t1 >> (4 * k) & 15), p);
        } else if (flg == 1) {
            unsigned n = nloop * nreg;
            for (unsigned i = 0; i < n && p + i < end; i++) {
                unsigned r = (unsigned)(t1 >> (4 * (i % nreg)) & 15);
                if (r != R_NOP && r != R_AD)
                    write(r, p[i]);
            }
            p += (n + 1) & ~1u;
        } else {
            p += 2 * nloop;  /* IMAGE */
        }
    }
}

void gstrace_begin(int dump) { hash = hash_ui = 2166136261u, prims = prims_ui = 0, dump_left = dump; }

void gstrace_chain(const void *start, const void *end)
{
    const u64 *p = start, *e = end;
    while (p + 2 <= e) {
        unsigned qwc = p[0] & 0xFFFF, id = p[0] >> 28 & 7;
        const u64 *d = p + 2, *de = d + 2 * qwc;
        gif(d, de > e ? e : de);
        p = de;
        if (id == 7)
            break;
    }
}

void gstrace_packets(const void *start, const void *end) { gif(start, end); }

unsigned gstrace_end(unsigned *n, unsigned *ui_hash, unsigned *ui_n)
{
    *n = prims - prims_ui, *ui_hash = hash_ui, *ui_n = prims_ui;
    return hash;
}
#endif
