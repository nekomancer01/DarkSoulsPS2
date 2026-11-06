/* Effects runtime: the area's FFX particle systems (fx.bin, layout in tools/export_effects.py) as EE billboards.
 *
 * Each placed instance (bonfire o0200: fire at the part, light at its dummy poly 100; MSB SFX regions) runs its effect's particle systems
 * while the camera is within FX_RANGE: every system emits per_emit particles each interval up to max_alive, in its
 * emitter shape (point / disc / sphere / box / cone / square) along its axis, then the particles move (speed,
 * acceleration, drag), age through the exported size and colour curves (8 samples over the life) and play
 * their flipbook. A newly active instance is prewarmed by one particle life. Particles face the camera; additive
 * ones are unfogged (fog would add its colour), alpha ones are sorted far to near and fogged. All of them are depth
 * tested against the map / characters without writing Z (render.c tri_list, R_BLEND).
 * Effect lights (FFX action 24, e.g. the bonfire's 90006) light characters by distance with DSR's cubic falloff and
 * the light's flicker (amb samples precomputed in map_char_light's units, 0x80 = 1.0, for the area's tone map). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <malloc.h>
#include "fx.h"
#include "drawgrp.h"  /* dg_entity_hidden: the list of entities events switched off (event-5) */
#include "ev_misc.h"  /* ev_bonfire_level: a bonfire instance shows by its bonfire's state */
#include "vu1.h"

/* FX_VU1 1 (default): particles away from the near plane and the guard band go to VU1 (vu1fx.vsm, renderer plan
 * PR 7): the EE sends one 2-qword vertex per particle, VU1 does the curves, roll, flipbook, projection and the sprite;
 * the rest keep the EE billboards below. FX_VU1=0 builds draw them all on the EE (A/B; TECH_DEBT fx-14) */
#ifndef FX_VU1
#define FX_VU1 1
#endif

#define FX_MAX 384        /* particle pool (all instances) */
#define FX_RANGE 40.0f    /* m: instances farther from the eye stop emitting (their particles finish) */
#define FX_BATCH 48       /* particles per GS packet */
#define FX_CURVE 8
#define FX_TEX_BUCKETS 64 /* additive draw buckets (textures beyond go through the sort) */

typedef struct {
    char magic[4];
    u32 version, n_tex, n_sys, n_eff, n_inst, n_light, n_ref;
    u32 off_tex, off_sys, off_eff, off_inst, off_light, off_ref, pad[2];
} FxHeader;

typedef struct { u16 w, h, fpr, nframes; u32 pix, clut; } FxTex;

/* = export_effects.SYS_FMT */
typedef struct {
    u16 tex;
    u8 blend, shape;          /* blend 0 additive, 1 alpha; shape 0 point 1 disc 2 sphere 3 box 4 cone 5 square */
    u16 per_emit, max_alive;
    float life, life_min, interval, delay;
    float org[3], axis[3], ext[3];   /* effect frame: offset, emission axis, shape half extents */
    float spread;                    /* Degree: largest angle from the axis, rad */
    float speed[3];                  /* speed = s0 x (s1 + s2 U) */
    float drag, turn, turn_dt;       /* 1/s; turn / turn_dt: movement 84 wobble, unused (0, TECH_DEBT fx-6) */
    float accel[3];                  /* world, m/s^2 */
    float size[4];                   /* half extents x, y x (size[2] + size[3] U) */
    float rot[2], spin[3];           /* rot = r0 + r1 U; spin = s0 x (s1 + s2 U) rad/s */
    float kx[FX_CURVE], ky[FX_CURVE];  /* size multiplier over the life */
    u8 rgba[FX_CURVE][4];            /* colour over the life, 0x80 = 1.0 */
    float frame[2], rate;            /* first frame = f0 + f1 U; frames per second */
    float total;                     /* emissions (-1 = forever) */
    float spread_k, dist, dist_sqrt; /* Spread, circle Distribution, 1 = sqrt of the radius (DistributionMode 0) */
    u16 fpr, nframes;
    float normal[3];                 /* circle plane normal (the system frame's local Z) */
    u8 pad2[4];
} FxSys;

typedef struct { u16 first_ref, n_ref, first_light, n_light; } FxEff;
typedef struct { u16 sys, pad; } FxRef;
/* frame: columns local x, y, z; entity: MSB SFX entity id (0 none): off while events deleted it (fx_entity_*), or
 * the bonfire object of a bonfire instance. flags (= export_effects.BF_*): 0 always; FXI_FIRE | kindle index: the fire
 * while the bonfire is at that index; FXI_LIGHT: its light while lit; FXI_BURST | n: one-shot (fx_bonfire_burst) */
typedef struct { u16 eff, flags; float pos[3], frame[9]; s32 entity; float pad; } FxInst;
#define FXI_FIRE 0x100
#define FXI_LIGHT 0x200
#define FXI_BURST 0x400
/* Emission time of a bonfire burst: 90007 (ignite) emits for ever in its data; its end comes from the FFX state
 * machine, which fx.c does not run. ASSUMED 0.5 s (TECH_DEBT fx-3) */
#define FX_BURST_T 0.5f
#define FX_BURSTS 8
typedef struct {
    u16 flags, pad;
    float off[3], radius, begin, period;
    float mean[3], pad1[5];
    u8 amb[FX_CURVE][3];
    float pad2;
} FxLight;

typedef struct {
    float p[3], v[3];
    float age, life, sx, sy, rot, spin, f0;
    u16 sys, inst;
    u8 set, pad[3];  /* the fx.bin (set) sys / inst index into */
} Particle;

typedef struct { float timer; int emitted, alive; } EmitState;

FxStats fx_stats;
static const FxHeader *hdr;
static const FxTex *tex;
static const FxSys *sys;
static const FxEff *eff;
static const FxRef *ref;
static const FxInst *inst;
static const FxLight *light;
static GSTEXTURE *gtex;
static EmitState *emit;       /* per instance, per system ref of its effect */
static int *emit_first;       /* instance -> first EmitState */
static u8 *inst_on;
static u32 *tkey;             /* per texture: content hash (pixels + CLUT), the key of the shared VRAM copy */
static float *vtab;           /* FX_VU1: per system 8 curve keys x {(kx, ky, 0, 0), (r, g, b, a)} (vu1_fx_add) */
static float *kmax;           /* per system: largest size multiplier of its curves */
/* one set per fx.bin: 0 the boot area (fx_init), k world block k (fx_block_load at boot, on while the block is) */
typedef struct {
    const FxHeader *hdr;
    const FxTex *tex;
    const FxSys *sys;
    const FxEff *eff;
    const FxRef *ref;
    const FxInst *inst;
    const FxLight *light;
    GSTEXTURE *gtex;
    EmitState *emit;
    int *emit_first;
    u8 *inst_on;
    u32 *tkey;
    float *vtab, *kmax;
    int on;
} FxSet;
static FxSet fset[FX_SETS];
static int cur_set;
static struct { int set, inst; float t; } bursts[FX_BURSTS];  /* bursts emitting (t > 0) */

/* the globals above describe set k */
static void bind(int k)
{
    FxSet *c = &fset[cur_set];
    c->hdr = hdr, c->tex = tex, c->sys = sys, c->eff = eff, c->ref = ref, c->inst = inst, c->light = light;
    c->gtex = gtex, c->emit = emit, c->emit_first = emit_first, c->inst_on = inst_on, c->tkey = tkey;
    c->vtab = vtab, c->kmax = kmax;
    const FxSet *n = &fset[cur_set = k];
    hdr = n->hdr, tex = n->tex, sys = n->sys, eff = n->eff, ref = n->ref, inst = n->inst, light = n->light;
    gtex = n->gtex, emit = n->emit, emit_first = n->emit_first, inst_on = n->inst_on, tkey = n->tkey;
    vtab = n->vtab, kmax = n->kmax;
}
static Particle pool[FX_MAX];
static int n_alive;
static float clock;
static u32 seed = 0x2545F491;

static inline float rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) * (1.0f / 16777216.0f);
}

static GSGLOBAL *fx_gs;
/* Effect textures in VRAM, shared by content: the Burg blocks' fx.bin repeat most of each other's (and Firelink's)
 * textures (4 sets: 471 KB of textures, 214 KB different) */
#define FX_VTEX 64
static struct { u32 key, vram, clut; u16 w, h; int refs; } vtex[FX_VTEX];

static void free_tex(void)
{
    for (u32 i = 0; i < hdr->n_tex; i++) {
        for (int k = 0; gtex[i].Vram && k < FX_VTEX; k++)
            if (vtex[k].refs && vtex[k].vram == gtex[i].Vram && !--vtex[k].refs) {
                r_vram_free_tex(vtex[k].vram, vtex[k].w, vtex[k].h, GS_PSM_T8);
                r_vram_free_clut(vtex[k].clut, GS_PSM_CT32);
            }
        gtex[i].Vram = gtex[i].VramClut = 0;
    }
}

/* the bound set's textures to VRAM from b (its fx.bin), or another set's copy of the same texture; 0 = VRAM full
 * (none kept) */
static int upload_tex(GSGLOBAL *gs, const u8 *b)
{
    FlushCache(0);
    for (u32 i = 0; i < hdr->n_tex; i++) {
        GSTEXTURE *t = &gtex[i];
        t->Width = tex[i].w;
        t->Height = tex[i].h;
        t->PSM = GS_PSM_T8;
        t->ClutPSM = GS_PSM_CT32;
        t->ClutStorageMode = GS_CLUT_STORAGE_CSM1;
        t->Filter = GS_FILTER_LINEAR;
        int k = 0, free_k = -1;
        while (k < FX_VTEX && !(vtex[k].refs && vtex[k].key == tkey[i] && vtex[k].w == t->Width && vtex[k].h == t->Height))
            free_k = free_k < 0 && !vtex[k].refs ? k : free_k, k++;
        if (k < FX_VTEX) {
            vtex[k].refs++;
            t->Vram = vtex[k].vram, t->VramClut = vtex[k].clut;
            continue;
        }
        t->Vram = free_k >= 0 ? r_vram_tex(gs, t->Width, t->Height, t->PSM) : GSKIT_ALLOC_ERROR;
        t->VramClut = t->Vram != GSKIT_ALLOC_ERROR ? r_vram_clut(gs, t->ClutPSM) : GSKIT_ALLOC_ERROR;
        if (t->Vram == GSKIT_ALLOC_ERROR || t->VramClut == GSKIT_ALLOC_ERROR) {
            r_vram_free_tex(t->Vram, t->Width, t->Height, t->PSM);
            t->Vram = 0;
            free_tex();
            return 0;
        }
        t->Mem = (u32 *)(b + tex[i].pix);
        t->Clut = (u32 *)(b + tex[i].clut);
        gsKit_texture_upload(gs, t);
        t->Mem = t->Clut = NULL;
        vtex[free_k].key = tkey[i], vtex[free_k].vram = t->Vram, vtex[free_k].clut = t->VramClut;
        vtex[free_k].w = t->Width, vtex[free_k].h = t->Height, vtex[free_k].refs = 1;
    }
    return 1;
}

/* upload = 0: tables only, the textures go up with fx_block_on */
static int load_set(GSGLOBAL *gs, const void *blob, int upload)
{
    hdr = blob;
    if (!blob || memcmp(hdr->magic, "DSFX", 4) || hdr->version != 1 || sizeof(FxSys) != 280 ||
        sizeof(FxLight) != 88 || sizeof(FxInst) != 60) {
        printf("DSRPROTO fx: %s\n", blob ? "fx.bin bad" : "no fx.bin");
        hdr = 0;
        return 0;
    }
    const u8 *b = blob;
    tex = (const FxTex *)(b + hdr->off_tex);
    sys = (const FxSys *)(b + hdr->off_sys);
    eff = (const FxEff *)(b + hdr->off_eff);
    ref = (const FxRef *)(b + hdr->off_ref);
    inst = (const FxInst *)(b + hdr->off_inst);
    light = (const FxLight *)(b + hdr->off_light);
    gtex = calloc(hdr->n_tex, sizeof *gtex);
    tkey = calloc(hdr->n_tex ? hdr->n_tex : 1, sizeof *tkey);
    for (u32 i = 0; tkey && i < hdr->n_tex; i++) {  /* FNV-1a over the pixels and the CLUT */
        u32 k = 2166136261u;
        for (u32 j = 0; j < (u32)tex[i].w * tex[i].h; j++)
            k = (k ^ b[tex[i].pix + j]) * 16777619u;
        for (u32 j = 0; j < 1024; j++)
            k = (k ^ b[tex[i].clut + j]) * 16777619u;
        tkey[i] = k;
    }
    emit_first = calloc(hdr->n_inst + 1, sizeof *emit_first);
    inst_on = calloc(hdr->n_inst, 1);
    int ns = 0;
    for (u32 i = 0; i < hdr->n_inst; i++)
        emit_first[i] = ns, ns += eff[inst[i].eff].n_ref;
    emit_first[hdr->n_inst] = ns;
    emit = calloc(ns ? ns : 1, sizeof *emit);
    kmax = calloc(hdr->n_sys ? hdr->n_sys : 1, sizeof *kmax);
    vtab = FX_VU1 ? memalign(16, (hdr->n_sys ? hdr->n_sys : 1) * 64 * sizeof *vtab) : NULL;
    for (u32 i = 0; kmax && i < hdr->n_sys; i++)
        for (int k = 0; k < FX_CURVE; k++) {
            const FxSys *y = &sys[i];
            float m = y->kx[k] > y->ky[k] ? y->kx[k] : y->ky[k];
            kmax[i] = m > kmax[i] ? m : kmax[i];
            if (!vtab)
                continue;
            float *t = &vtab[64 * i + 8 * k];
            t[0] = y->kx[k], t[1] = y->ky[k], t[2] = t[3] = 0;
            for (int c = 0; c < 4; c++)
                t[4 + c] = y->rgba[k][c];
        }
    if (vtab)
        SyncDCache(vtab, vtab + 64 * hdr->n_sys);  /* vu1_fx_add sends the tables by DMA reference */
    if (!gtex || !tkey || !emit_first || !inst_on || !emit || !kmax || (FX_VU1 && !vtab)) {
        printf("DSRPROTO fx: out of RAM\n");
        hdr = 0;
        return 0;
    }
    if (upload && !upload_tex(gs, b)) {
        printf("DSRPROTO fx: VRAM full, effects off\n");
        hdr = 0;
        return 0;
    }
    int vram = 0;
    for (u32 i = 0; i < hdr->n_tex; i++)
        vram += tex[i].w * tex[i].h + 1024;
    printf("DSRPROTO fx: %u instances, %u effects, %u systems, %u lights, %u textures (%d KB VRAM%s)\n", hdr->n_inst,
           hdr->n_eff, hdr->n_sys, hdr->n_light, hdr->n_tex, vram / 1024, upload ? "" : " while on");
    return 1;
}

int fx_init(GSGLOBAL *gs, const void *blob)
{
    fx_gs = gs;
    bind(0);
    int r = load_set(gs, blob, 1);
    fset[0].on = r;
    bind(0);
    return r;
}

int fx_block_load(GSGLOBAL *gs, int block, const void *blob, unsigned size)
{
    if (block <= 0 || block >= FX_SETS || !blob)
        return 0;
    int was = cur_set;
    fx_gs = gs;
    bind(block);
    int r = load_set(gs, blob, 0);
    if (r) {  /* keep the tables (everything before the first pixels / CLUT); the pixels come with fx_block_on */
        const u8 *b = blob;
        u32 keep = size;
        for (u32 i = 0; i < hdr->n_tex; i++)
            keep = tex[i].clut < keep ? tex[i].clut : keep, keep = tex[i].pix < keep ? tex[i].pix : keep;
        u8 *t = memalign(16, keep);
        if (!t) {
            r = 0;
        } else {
            memcpy(t, b, keep);
            hdr = (const FxHeader *)t;
            tex = (const FxTex *)(t + hdr->off_tex), sys = (const FxSys *)(t + hdr->off_sys);
            eff = (const FxEff *)(t + hdr->off_eff), ref = (const FxRef *)(t + hdr->off_ref);
            inst = (const FxInst *)(t + hdr->off_inst), light = (const FxLight *)(t + hdr->off_light);
        }
    }
    if (!r)
        hdr = 0;
    fset[block].on = 0;
    bind(was);
    return r;
}

void fx_block_on(int block, int on, const void *blob)
{
    if (block <= 0 || block >= FX_SETS || !fset[block].hdr || fset[block].on == on)
        return;
    for (int i = 0; !on && i < n_alive;) {  /* its particles go */
        if (pool[i].set == block)
            pool[i] = pool[--n_alive];
        else
            i++;
    }
    int was = cur_set;
    bind(block);
    memset(inst_on, 0, hdr->n_inst);  /* prewarmed again when it comes back */
    memset(emit, 0, emit_first[hdr->n_inst] * sizeof *emit);
    if (!on)
        free_tex();  /* its particles are gone: nothing draws them after this frame */
    else if (!blob || !upload_tex(fx_gs, blob))
        printf("DSRPROTO fx: block %d: %s, effects off (VRAM free %d)\n", block, blob ? "VRAM full" : "no fx.bin",
               r_vram_avail(fx_gs)), on = 0;
    fset[block].on = on;
    bind(was);
}

int fx_entity_particles(int id, int kill)
{
    int was = cur_set, found = 0, n = 0;
    for (int k = 0; k < FX_SETS; k++) {
        bind(k);
        if (!hdr)
            continue;
        for (u32 ii = 0; ii < hdr->n_inst; ii++)
            found |= inst[ii].entity == id;
        for (int i = 0; i < n_alive;) {
            if (pool[i].set != k || inst[pool[i].inst].entity != id) {
                i++;
            } else if (kill) {
                pool[i] = pool[--n_alive];
                n++;
            } else {
                i++, n++;
            }
        }
    }
    bind(was);
    return found ? n : -1;
}

static void frame_apply(const FxInst *in, const float l[3], float out[3])
{
    for (int k = 0; k < 3; k++)
        out[k] = in->frame[k] * l[0] + in->frame[3 + k] * l[1] + in->frame[6 + k] * l[2];
}

/* u, w: unit vectors perpendicular to n and each other */
static void basis(const float *n, float *u, float *w)
{
    float t[3] = {1, 0, 0};
    if (fabsf(n[0]) > 0.9f)
        t[0] = 0, t[1] = 1;
    w[0] = n[1] * t[2] - n[2] * t[1];
    w[1] = n[2] * t[0] - n[0] * t[2];
    w[2] = n[0] * t[1] - n[1] * t[0];
    float l = 1.0f / sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    for (int k = 0; k < 3; k++)
        w[k] *= l;
    u[0] = w[1] * n[2] - w[2] * n[1];
    u[1] = w[2] * n[0] - w[0] * n[2];
    u[2] = w[0] * n[1] - w[1] * n[0];
}

static void spawn(int ii, int si)
{
    if (n_alive >= FX_MAX)
        return;
    const FxInst *in = &inst[ii];
    const FxSys *s = &sys[si];
    Particle *p = &pool[n_alive++];
    float l[3] = {0, 0, 0};
    float r = s->ext[0] > s->ext[2] ? s->ext[0] : s->ext[2];
    switch (s->shape) {
    case 1: {  /* circle in the system's local XY plane; radius bias from Distribution (FXEmitterUtility.inl) */
        float a = rnd() * 6.2831853f, u[3], w[3];
        float d = s->dist <= 0 ? powf(rnd(), 1 + s->dist) : 1 - powf(rnd(), 1 - s->dist);
        if (s->dist_sqrt > 0)
            d = sqrtf(d);
        d *= r;
        basis(s->normal, u, w);
        for (int k = 0; k < 3; k++)
            l[k] = (u[k] * cosf(a) + w[k] * sinf(a)) * d;
        break;
    }
    case 2: {
        float x, y, z;
        do
            x = rnd() * 2 - 1, y = rnd() * 2 - 1, z = rnd() * 2 - 1;
        while (x * x + y * y + z * z > 1);
        l[0] = x * r, l[1] = y * r, l[2] = z * r;
        break;
    }
    case 3:
    case 5:
        for (int k = 0; k < 3; k++)
            l[k] = (rnd() * 2 - 1) * s->ext[k];
        break;
    }
    for (int k = 0; k < 3; k++)
        l[k] += s->org[k];
    float wp[3];
    frame_apply(in, l, wp);
    /* direction (FUN_140f639e0): angle from the axis Degree x U^(1 + Spread) (Spread <= 0) or
     * Degree x (1 - U^(1 - Spread)), turned by a uniform angle around it */
    float dir[3] = {s->axis[0], s->axis[1], s->axis[2]};
    if (s->spread > 0) {
        float t = s->spread_k <= 0 ? s->spread * powf(rnd(), 1 + s->spread_k)
                                   : s->spread * (1 - powf(rnd(), 1 - s->spread_k));
        float a = rnd() * 6.2831853f, u[3], w[3], ct = cosf(t), st = sinf(t);
        basis(s->axis, u, w);
        for (int k = 0; k < 3; k++)
            dir[k] = s->axis[k] * ct + (u[k] * cosf(a) + w[k] * sinf(a)) * st;
    }
    float wd[3];
    frame_apply(in, dir, wd);
    float sp = s->speed[0] * (s->speed[1] + s->speed[2] * rnd());
    for (int k = 0; k < 3; k++)
        p->p[k] = in->pos[k] + wp[k], p->v[k] = wd[k] * sp;
    p->age = 0;
    p->life = s->life_min + (s->life - s->life_min) * rnd();
    float k = s->size[2] + s->size[3] * rnd();
    p->sx = s->size[0] * k, p->sy = s->size[1] * k;
    p->rot = s->rot[0] + s->rot[1] * rnd();
    p->spin = s->spin[0] * (s->spin[1] + s->spin[2] * rnd());
    p->f0 = s->frame[0] + s->frame[1] * rnd();
    p->sys = si, p->inst = ii, p->set = cur_set;
}

static void step(float dt, int emit_on)
{
    int was = cur_set;
    for (int i = 0; i < n_alive;) {
        Particle *p = &pool[i];
        if (p->set != cur_set)
            bind(p->set);
        const FxSys *s = &sys[p->sys];
        p->age += dt;
        if (p->age >= p->life) {
            int es = emit_first[p->inst];
            for (int r = 0; r < eff[inst[p->inst].eff].n_ref; r++)
                if (ref[eff[inst[p->inst].eff].first_ref + r].sys == p->sys) {
                    emit[es + r].alive--;
                    break;
                }
            *p = pool[--n_alive];
            continue;
        }
        float dr = 1 - s->drag * dt;
        dr = dr < 0 ? 0 : dr;
        for (int k = 0; k < 3; k++) {
            p->v[k] = (p->v[k] + s->accel[k] * dt) * dr;
            p->p[k] += p->v[k] * dt;
        }
        p->rot += p->spin * dt;
        i++;
    }
    for (int k = 0; emit_on && k < FX_SETS; k++) {
        if (!fset[k].on)
            continue;
        bind(k);
    for (u32 ii = 0; ii < hdr->n_inst; ii++) {
        if (!inst_on[ii])
            continue;
        const FxEff *e = &eff[inst[ii].eff];
        for (int r = 0; r < e->n_ref; r++) {
            EmitState *es = &emit[emit_first[ii] + r];
            int si = ref[e->first_ref + r].sys;
            const FxSys *s = &sys[si];
            if (s->total >= 0 && es->emitted >= (int)s->total)
                continue;
            es->timer += dt;
            if (es->timer < s->delay)
                continue;
            while (es->timer - s->delay >= s->interval) {
                es->timer -= s->interval;
                es->emitted++;
                for (int n = 0; n < s->per_emit && es->alive < s->max_alive; n++, es->alive++)
                    spawn(ii, si);
            }
        }
    }
    }
    bind(was);
}

static int burst_left(int ii)
{
    for (int b = 0; b < FX_BURSTS; b++)
        if (bursts[b].t > 0 && bursts[b].set == cur_set && bursts[b].inst == ii)
            return 1;
    return 0;
}

/* instance ii of the bound set shown by its event / bonfire state */
static int inst_state(int ii)
{
    const FxInst *in = &inst[ii];
    if (!in->flags)
        return !(in->entity && dg_entity_hidden(in->entity));  /* Delete Map SFX (event.c) */
    int lv = ev_bonfire_level(in->entity);  /* -1: not registered (2009[03] creates the fire) */
    if (in->flags & FXI_FIRE)
        return lv == (in->flags & 0xFF);
    if (in->flags & FXI_LIGHT)
        return lv >= 1;
    return burst_left(ii);
}

void fx_bonfire_burst(int entity, int n)
{
    int was = cur_set;
    for (int k = 0; k < FX_SETS; k++) {
        if (!fset[k].on)
            continue;
        bind(k);
        for (u32 ii = 0; ii < hdr->n_inst; ii++) {
            if (inst[ii].entity != entity || inst[ii].flags != (FXI_BURST | n))
                continue;
            int b = 0;
            while (b < FX_BURSTS - 1 && bursts[b].t > 0 && !(bursts[b].set == k && bursts[b].inst == (int)ii))
                b++;
            bursts[b].set = k, bursts[b].inst = ii, bursts[b].t = FX_BURST_T;
            memset(&emit[emit_first[ii]], 0, (emit_first[ii + 1] - emit_first[ii]) * sizeof *emit);
            inst_on[ii] = 2;  /* on without the prewarm (fx_update) */
        }
    }
    bind(was);
}

void fx_update(float dt, V3 eye)
{
    int any = 0;
    for (int k = 0; k < FX_SETS; k++)
        any |= fset[k].on;
    if (!any)
        return;
    unsigned t0;
    __asm__ volatile("mfc0 %0, $9" : "=r"(t0));
    if (dt > 0.1f)
        dt = 0.1f;
    clock += dt;
    int active = 0;
    for (int k = 0; k < FX_SETS; k++) {
        if (!fset[k].on)
            continue;
        bind(k);
    for (u32 ii = 0; ii < hdr->n_inst; ii++) {
        float dx = inst[ii].pos[0] - eye.x, dy = inst[ii].pos[1] - eye.y, dz = inst[ii].pos[2] - eye.z;
        int on = dx * dx + dy * dy + dz * dz < FX_RANGE * FX_RANGE && inst_state(ii);
        if (on && !inst_on[ii] && !(inst[ii].flags & FXI_BURST)) {  /* prewarm: one life of the longest system in 8 steps */
            inst_on[ii] = 1;
            const FxEff *e = &eff[inst[ii].eff];
            float warm = 0;
            for (int r = 0; r < e->n_ref; r++) {
                float l = sys[ref[e->first_ref + r].sys].life;
                warm = l < 30 && l > warm ? l : warm;
            }
            for (int n = 0; n < 8; n++)
                step(warm / 8, 1);
        }
        inst_on[ii] = on;
        active += on;
    }
    }
    bind(0);
    for (int b = 0; b < FX_BURSTS; b++)
        bursts[b].t -= dt;
    step(dt, 1);
    fx_stats.active = active;
    fx_stats.particles = n_alive;
    unsigned t1;
    __asm__ volatile("mfc0 %0, $9" : "=r"(t1));
    fx_stats.ms = (t1 - t0) / 294912.0f;
}

typedef struct { float z; int i; } Order;

/* sin / cos of a (any range), polynomial per quadrant, |error| < 0.0012: enough for a sprite's roll */
static void fast_sincos(float a, float *sn, float *cs)
{
    float q = a * 0.63661977f;  /* quarter turns */
    int k = (int)(q < 0 ? q - 1 : q);
    float x = (q - k) * 1.5707963f, x2 = x * x;
    float s0 = x * (1 - x2 * (0.16605f - x2 * 0.00761f)), c0 = 1 - x2 * (0.49670f - x2 * 0.03705f);
    switch (k & 3) {
    case 0: *sn = s0, *cs = c0; break;
    case 1: *sn = c0, *cs = -s0; break;
    case 2: *sn = -s0, *cs = -c0; break;
    default: *sn = -c0, *cs = s0; break;
    }
}

static int by_depth(const void *a, const void *b)
{
    float d = ((const Order *)b)->z - ((const Order *)a)->z;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

/* FX_VU1: the VU1 batch being collected (one texture and mode, <= VU1_FX_SYS curve tables) and the fog terms */
static struct {
    GSTEXTURE *gt;
    const FxTex *tx;
    int mode, n, nsys, chain;
    const float *tab[VU1_FX_SYS];
    float fog[5];
    u64 fogcol, fogbase;
    float part[VU1_FX_N][8] __attribute__((aligned(16)));
} vb;
static int vu_drawn, vu_batches, vu_breaks[3];  /* breaks: texture / mode, full, tables */

static void vu_flush(void)
{
    if (!vb.n)
        return;
    if (vb.n & 1) {  /* the program takes particles in pairs: a zero-size copy of the last one draws nothing */
        memcpy(vb.part[vb.n], vb.part[vb.n - 1], sizeof vb.part[0]);
        vb.part[vb.n][4] = vb.part[vb.n][5] = 0;
        vb.n++;
    }
    if (!vb.chain) {  /* the particle chain: camera constants, the main fog row, the program's layout */
        vu1_begin(2);
        vu1_map_fog(vb.fog[0], vb.fog[1], vb.fog[2], vb.fog[3]);
        vu1_fx_begin();
        vb.chain = 1;
    }
    float *pt, *b = vu1_fx_add(vb.gt, vb.mode, vb.fogcol, vb.n, vb.nsys, vb.tab, &pt);
    if (b) {
        const FxTex *tx = vb.tx;
        int nf = tx->nframes ? tx->nframes : 1, fpr = tx->fpr ? tx->fpr : 1, rows = (nf + fpr - 1) / fpr;
        float cw = 1.0f / fpr, ch = 1.0f / rows, mu = 0.5f / vb.gt->Width, mv = 0.5f / vb.gt->Height;
        float du = cw - 2 * mu, dv = ch - 2 * mv;
        const float h[28] = {0, 0, 0, 0, cw, ch, 0.5f, 1.0f / fpr, mu - cw * 0.5f, mv, 0, (float)fpr,
                             du, dv, 0, vb.fog[4], du, 0, 0, 0, 0, dv, 0, 0, vb.fog[2], vb.fog[3], 0, 2048};
        memcpy(b, h, sizeof h);
        ((u32 *)b)[0] = vb.n;
        memcpy(pt, vb.part, vb.n * 8 * sizeof(float));
        vu_batches++;
    }
    vb.n = vb.nsys = 0;
}

/* p (system s of the bound set, view centre c) to the VU1 batch; 0 = not VU1-safe (near plane / guard band), drawn
 * by the EE path */
static int vu_add(const Particle *p, const FxSys *s, V3 c, int mode)
{
    float rad = (p->sx > p->sy ? p->sx : p->sy) * kmax[p->sys];  /* the largest half size its curves reach */
    V3 ext = v3(fabsf(c.x) + 1.42f * rad, fabsf(c.y) + 1.42f * rad, c.z - rad);  /* rolled corner <= sqrt 2 */
    if (ext.z <= R_NEAR + 0.05f || !r_point_inside(&ext))
        return 0;
    float t = p->age / p->life * (FX_CURVE - 1), rot = p->rot;
    t = t < FX_CURVE - 1.0001f ? t : FX_CURVE - 1.0001f;
    int k0 = (int)t;
    if (s->rgba[k0][3] + (s->rgba[k0 + 1][3] - s->rgba[k0][3]) * (t - k0) < 1)
        return 1;  /* invisible at this age (as the EE path) */
    GSTEXTURE *gt = &gtex[s->tex];
    const float *tab = &vtab[64 * p->sys];
    int k = 0;
    while (k < vb.nsys && vb.tab[k] != tab)
        k++;
    if (vb.n && (gt != vb.gt || mode != vb.mode || vb.n >= VU1_FX_N || k == VU1_FX_SYS)) {
        vu_breaks[gt != vb.gt || mode != vb.mode ? 0 : vb.n >= VU1_FX_N ? 1 : 2]++;
        vu_flush(), k = 0;
    }
    if (k == vb.nsys)
        vb.tab[vb.nsys++] = tab;
    vb.gt = gt, vb.tx = &tex[s->tex], vb.mode = mode;
    const FxTex *tx = vb.tx;
    int nf = tx->nframes ? tx->nframes : 1;
    int fr = (int)(p->f0 + s->rate * p->age) % nf;
    fr = fr < 0 ? fr + nf : fr;
    rot -= 6.2831853f * (int)(rot * 0.15915494f);  /* the program's sin / cos take -pi..pi */
    rot = rot > 3.1415927f ? rot - 6.2831853f : rot < -3.1415927f ? rot + 6.2831853f : rot;
    float *q = vb.part[vb.n++];
    q[0] = p->p[0], q[1] = p->p[1], q[2] = p->p[2], q[3] = 8 * k + t;
    q[4] = p->sx, q[5] = p->sy, q[6] = rot, q[7] = fr + 0.5f;
    vu_drawn++;
    return 1;
}

void fx_kick(void)
{
    if (!vb.chain)
        return;
    /* gsKit draws after this chain with its own ALPHA / ZBUF / FOGCOL (as tri_list leaves them) */
    u64 zbuf = GS_SETREG_ZBUF(fx_gs->ZBuffer / 8192, fx_gs->PSMZ, 0);
    const u64 r[6] = {fx_gs->PrimAlpha, 0x42, zbuf, 0x4E, vb.fogbase, 0x3D};
    vu1_gs_regs(3, r);
    vu1_kick();
    vb.chain = 0;
}

void fx_draw(void)
{
    bind(0);
    if (!n_alive) {
        fx_stats.drawn = fx_stats.batches = 0;
        return;
    }
    unsigned t0;
    __asm__ volatile("mfc0 %0, $9" : "=r"(t0));
    static Order ord[FX_MAX];
    static V3 cen[FX_MAX];
    /* alpha blended: sorted far to near; additive (order-free) after them, bucketed by texture: few GS packets */
    static int head[FX_TEX_BUCKETS], next[FX_MAX];
    int n = 0, nb = !hdr ? 0 : hdr->n_tex < FX_TEX_BUCKETS ? hdr->n_tex : FX_TEX_BUCKETS;  /* set 0's textures */
    for (int t = 0; t < nb; t++)
        head[t] = -1;
    for (int i = 0; i < n_alive; i++) {
        Particle *p = &pool[i];
        V3 c = r_view(v3(p->p[0], p->p[1], p->p[2]));
        float rad = (p->sx > p->sy ? p->sx : p->sy) * 3.75f;  /* largest size multiplier in the bonfire curves */
        if (c.z < R_NEAR - rad)
            continue;
        cen[i] = c;
        if (p->set != cur_set)
            bind(p->set);
        int t = sys[p->sys].tex;
        if (sys[p->sys].blend || t >= nb || p->set)  /* other sets' additive ones: sorted by set and texture */
            ord[n].z = sys[p->sys].blend ? c.z : -1e6f - t - 256.0f * p->set, ord[n].i = i, n++;
        else
            next[i] = head[t], head[t] = i;
    }
    bind(0);
    qsort(ord, n, sizeof *ord, by_depth);
    for (int t = 0; t < nb; t++)
        for (int i = head[t]; i >= 0; i = next[i])
            ord[n].z = -1e6f - t, ord[n].i = i, n++;
    float focal_px;
    {
        V3 e, rt, up, fw;
        float fy, zs;
        r_camera_basis(&e, &rt, &up, &fw, &focal_px, &fy, &zs);
    }
    vu_drawn = vu_batches = 0;
    memset(vu_breaks, 0, sizeof vu_breaks);
    if (FX_VU1)
        r_get_fog(vb.fog, &vb.fogcol, &vb.fogbase);
    static ClipVert cv[FX_BATCH * 6];
    static float sc[FX_BATCH * 6][3];
    int nv = 0, cur_tex = -1, cur_mode = 0, drawn = 0, batches = 0;
    GSTEXTURE *cur_gt = NULL;
    for (int o = 0; o <= n; o++) {
        const Particle *p = o < n ? &pool[ord[o].i] : 0;
        if (p && p->set != cur_set)
            bind(p->set);
        const FxSys *s = p ? &sys[p->sys] : 0;
        int mode = s ? (R_BLEND | (s->blend ? R_FOG : R_ADD)) : 0;
        if (nv && (!p || &gtex[s->tex] != cur_gt || mode != cur_mode || nv >= FX_BATCH * 6)) {
            r_tri_list(cur_gt, cur_mode, nv, cv, (const float (*)[3])sc);
            nv = 0, batches++;
        }
        if (!p)
            break;
        if (FX_VU1 && vu_add(p, s, cen[ord[o].i], s->blend ? VU1_FX_FOG : VU1_FX_ADD))
            continue;
        cur_tex = s->tex, cur_mode = mode, cur_gt = &gtex[s->tex];
        /* curves at the particle's age */
        float t = p->age / p->life * (FX_CURVE - 1);
        int k0 = (int)t;
        k0 = k0 > FX_CURVE - 2 ? FX_CURVE - 2 : k0;
        float f = t - k0;
        float kx = s->kx[k0] + (s->kx[k0 + 1] - s->kx[k0]) * f, ky = s->ky[k0] + (s->ky[k0 + 1] - s->ky[k0]) * f;
        float col[4];
        for (int c = 0; c < 4; c++)
            col[c] = s->rgba[k0][c] + (s->rgba[k0 + 1][c] - s->rgba[k0][c]) * f;
        if (col[3] < 1)
            continue;
        float hx = p->sx * kx, hy = p->sy * ky, cr = 1, sr = 0;
        if ((hx > hy ? hx : hy) * focal_px > 1.5f * cen[ord[o].i].z)  /* roll only matters above ~1.5 px */
            fast_sincos(p->rot, &sr, &cr);
        /* flipbook cell */
        const FxTex *tx = &tex[s->tex];
        int nf = tx->nframes ? tx->nframes : 1, fpr = tx->fpr ? tx->fpr : 1, rows = (nf + fpr - 1) / fpr;
        int fr = (int)(p->f0 + s->rate * p->age) % nf;
        fr = fr < 0 ? fr + nf : fr;
        float cw = (float)tx->w / fpr, ch = (float)tx->h / rows;
        float u0 = (fr % fpr) * cw, v0 = (fr / fpr) * ch;
        const float qx[4] = {-1, 1, 1, -1}, qy[4] = {1, 1, -1, -1};
        ClipVert q[4];
        float qs[4][3], s0[3], s1[3];
        V3 c = cen[ord[o].i];
        /* pulled toward the camera by its own half size, same screen size, so the quad is not cut by the surface it
         * rises from (bonfire ash). ASSUMED stand-in for DSR's per-pixel depth handling, TECH_DEBT fx-5 */
        float bias = hx > hy ? hx : hy, kz = c.z - bias > R_NEAR + 0.05f ? (c.z - bias) / c.z : 1;
        c = v3scale(c, kz), hx *= kz, hy *= kz;
        /* the corners share the centre's view depth: screen = centre + offset x pixels per metre */
        V3 ext = v3(fabsf(c.x) + 1.42f * hx, fabsf(c.y) + 1.42f * hy, c.z);  /* rolled unit corner <= sqrt 2 */
        int inside = r_point_inside(&ext);
        if (inside) {
            V3 c1 = v3(c.x + 1, c.y + 1, c.z);
            r_view_to_screen(&c, s0);
            r_view_to_screen(&c1, s1);
            s1[0] -= s0[0], s1[1] -= s0[1];
        }
        for (int j = 0; j < 4; j++) {
            /* roll first, then the size: corner = diag(hx, hy) x Rot(roll) x unit corner, as DSR's
             * FRPG_Sfx_SimpleSprite*_Particle.vpo with cb0 c38.x = 0 (every captured fire draw, Firelink + Burg
             * RenderDoc): a rolled tall flame stays tall (a sheared quad). Size-then-roll laid rolled flames on
             * their side (the "horizontal fires") */
            float ox = (qx[j] * cr - qy[j] * sr) * hx, oy = (qx[j] * sr + qy[j] * cr) * hy;
            q[j] = (ClipVert){c.x + ox, c.y + oy, c.z, u0 + (qx[j] > 0 ? cw - 0.5f : 0.5f),
                              v0 + (qy[j] > 0 ? 0.5f : ch - 0.5f), col[0], col[1], col[2], col[3]};
            if (inside)
                qs[j][0] = s0[0] + ox * s1[0], qs[j][1] = s0[1] + oy * s1[1], qs[j][2] = s0[2];
        }
        drawn++;
        if (!inside) {  /* near the camera: clipped triangles, own packets */
            r_tri_clip(&gtex[s->tex], &q[0], &q[1], &q[2], 0, mode);
            r_tri_clip(&gtex[s->tex], &q[0], &q[2], &q[3], 0, mode);
            continue;
        }
        static const int tri[6] = {0, 1, 2, 0, 2, 3};
        for (int j = 0; j < 6; j++) {
            cv[nv] = q[tri[j]];
            memcpy(sc[nv], qs[tri[j]], sizeof sc[nv]);
            nv++;
        }
    }
    bind(0);
    vu_flush();
    (void)cur_tex;
    fx_stats.drawn = drawn + vu_drawn;
    fx_stats.batches = batches + vu_batches;
    fx_stats.vu1 = vu_drawn;
    for (int k = 0; k < 3; k++)
        fx_stats.vu1_breaks[k] = vu_breaks[k];
    unsigned t1;
    __asm__ volatile("mfc0 %0, $9" : "=r"(t1));
    fx_stats.ms_draw = (t1 - t0) / 294912.0f;
    fx_stats.ms += fx_stats.ms_draw;
}

void fx_char_light(V3 pos, float amb[3])
{
    for (int k = 0; k < FX_SETS; k++) {
        if (!fset[k].on)
            continue;
        bind(k);
    for (u32 ii = 0; ii < hdr->n_inst; ii++) {
        const FxEff *e = &eff[inst[ii].eff];
        if (e->n_light && !inst_state(ii))  /* an unlit bonfire's light, a burst that is over */
            continue;
        for (int l = 0; l < e->n_light; l++) {
            const FxLight *L = &light[e->first_light + l];
            float w[3];
            frame_apply(&inst[ii], L->off, w);
            float dx = inst[ii].pos[0] + w[0] - pos.x, dy = inst[ii].pos[1] + w[1] - pos.y - 1.0f,
                  dz = inst[ii].pos[2] + w[2] - pos.z;  /* character centre ~1 m above the feet */
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (d >= L->radius)
                continue;
            float a = (L->radius - d) / (L->radius - L->begin);
            a = a > 1 ? 1 : a * a * a;  /* DSR: saturate(((end - d) / (end - begin))^3) */
            float ph = fmodf(clock / (L->period > 0 ? L->period : 1), 1.0f) * FX_CURVE;
            int k0 = (int)ph % FX_CURVE, k1 = (k0 + 1) % FX_CURVE;
            float f = ph - (int)ph;
            for (int c = 0; c < 3; c++)
                amb[c] += a * (L->amb[k0][c] + (L->amb[k1][c] - L->amb[k0][c]) * f);  /* map_char_light units */
        }
    }
    }
    bind(0);
}
