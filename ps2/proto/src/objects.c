/* Breakable objects (objects.h). Layout of objects.bin: tools/export_objects.py. One model per object type: bone 0 =
 * the intact object (group bit intact_bits), bones 1..n = its pre-fractured pieces (broken_bits), all roots, so the
 * pose of each bone is its world transform (mdl_draw with the identity placement).
 * Break rules from ObjectParam (pots: hp 1, defense 0): any weapon hit breaks. Rolls / dashes into an object and
 * landing on it also break it (DSR breaks pots that way; isBreakByPlayerCollide 0 means walking does not), and
 * walking pushes the player out of its cylinder (collision-1 for breakables).
 * After a break the pieces fly off (ASSUMED speeds: no DSR source found), settle (physics.c), lie for PIECE_LIFE s
 * and sink into the ground over SINK_TIME s (stand-in for DSR's fade; VU1 skinning has no alpha). Broken objects stay
 * broken until the area reloads (DSR keeps them in map:/breakobj/<map>.breakobj; not modelled). */
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "objects.h"
#include "physics.h"
#include "model.h"
#include "player.h"
#include "combat.h"
#include "collision.h"

#define MAX_TYPES 16           /* the boot area's + every world block's (objects_block_types) */
#define MAX_INST 128
#define MAX_PIECES 32          /* per type */
#define MAX_BODIES 96          /* piece budget (3 pots); the oldest broken object's pieces go first */
#define PIECE_LIFE 8.0f        /* ASSUMED */
#define SINK_TIME 2.0f
#define SINK_DEPTH 0.5f
#define DRAW_DIST 40.0f
#define PUSH_OUT 2.0f          /* ASSUMED: m/s away from the hit */
#define PUSH_ALONG 2.5f        /* ASSUMED: m/s along the hit direction */
#define PUSH_UP 1.5f           /* ASSUMED */
#define SPIN 6.0f              /* ASSUMED: rad/s */

typedef struct {
    int id, pieces, hp, defense;
    float cyl_r, cyl_y0, cyl_y1, fr, rest;
    unsigned intact_bits, broken_bits;
    const float *piece;        /* 20 floats each */
    Model *model;
} Type;

typedef struct {
    int type;
    float pos[3], q[4];
    int broken;                /* 0 intact, 1 pieces live, 2 gone (also a free slot: block -1) */
    int block;                 /* world block (src/world.c) it belongs to; 0 the boot area, -1 a free slot */
    int entity;                /* MSB entity id (EMEVD addresses it: objects_entity_*), -1 none */
    int off;                   /* switched off by an event: not drawn, no collision, cannot break */
    float t;                   /* seconds since the break */
    short body[MAX_PIECES];
} Inst;

static Type types[MAX_TYPES];
static Inst inst[MAX_INST];
static int ntypes, ninst;
/* world blocks' placements (objects_block_types at boot; types already loaded): type index, pos, q */
typedef struct {
    unsigned short type;
    float pos[3], q[4];
    int entity;
} Part;
static Part *blk_part[OBJ_BLOCKS];
static int blk_nparts[OBJ_BLOCKS];
static Body bodies[MAX_BODIES];
ObjectsStats objects_stats;

static void q_mul(const float *a, const float *b, float *o)
{
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x, o[1] = y, o[2] = z, o[3] = w;
}

static void q_rot(const float *q, const float *v, float *o)
{
    float t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
    o[0] = v[0] + q[3] * t[0] + q[1] * t[2] - q[2] * t[1];
    o[1] = v[1] + q[3] * t[1] + q[2] * t[0] - q[0] * t[2];
    o[2] = v[2] + q[3] * t[2] + q[0] * t[1] - q[1] * t[0];
}

int objects_init(GSGLOBAL *gs, const void *blob)
{
    const unsigned *h = blob;
    ntypes = ninst = 0;
    memset(bodies, 0, sizeof bodies);
    if (!blob || memcmp(blob, "DSRB", 4) || (h[1] != 1 && h[1] != 2))
        return 0;
    const unsigned char *b = blob;
    unsigned ps = h[1] == 2 ? 48 : 32;  /* part record (export_objects.py: version 2 adds the entity id) */
    for (unsigned k = 0; k < h[2] && ntypes < MAX_TYPES; k++) {
        const unsigned *t = (const unsigned *)(b + h[4] + 64 * k);
        const float *f = (const float *)t;
        Type *ty = &types[ntypes];
        ty->id = (int)t[0];
        ty->pieces = t[3] < MAX_PIECES ? (int)t[3] : MAX_PIECES;
        ty->piece = (const float *)(b + t[4]);
        ty->hp = (int)t[5], ty->defense = (int)t[6];
        ty->cyl_r = f[7], ty->cyl_y0 = f[8], ty->cyl_y1 = f[9], ty->fr = f[10], ty->rest = f[11];
        ty->intact_bits = t[12], ty->broken_bits = t[13];
        ty->model = mdl_load(b + t[1]);
        if (!ty->model || mdl_bones(ty->model) != ty->pieces + 1)
            continue;
        if (mdl_upload(ty->model, gs) < 0)
            continue;
        ntypes++;
    }
    for (unsigned k = 0; k < h[3] && ninst < MAX_INST; k++) {
        const unsigned char *p = b + h[5] + ps * k;
        int ti = *(const unsigned short *)p;
        if (ti >= ntypes)
            continue;
        Inst *in = &inst[ninst++];
        memset(in, 0, sizeof *in);
        memset(in->body, 0xFF, sizeof in->body);  /* -1: no body */
        in->type = ti;
        memcpy(in->pos, p + 4, 12);
        memcpy(in->q, p + 16, 16);
        in->entity = ps == 48 ? *(const int *)(p + 32) : -1;
    }
    for (int i = 0; i < ninst; i++)
        inst[i].block = 0;
    objects_stats.instances = ninst;
    return ninst;
}

int objects_block_types(GSGLOBAL *gs, int block, const void *blob)
{
    const unsigned *h = blob;
    if (block <= 0 || block >= OBJ_BLOCKS || !blob || memcmp(blob, "DSRB", 4) || (h[1] != 1 && h[1] != 2))
        return 0;
    const unsigned char *b = blob;
    unsigned ps = h[1] == 2 ? 48 : 32;
    int map[MAX_TYPES * 2];
    for (unsigned k = 0; k < h[2] && k < MAX_TYPES * 2; k++) {
        const unsigned *t = (const unsigned *)(b + h[4] + 64 * k);
        const float *f = (const float *)t;
        map[k] = -1;
        for (int j = 0; j < ntypes; j++)  /* the same object model in another block: one type */
            if (types[j].id == (int)t[0])
                map[k] = j;
        if (map[k] >= 0 || ntypes == MAX_TYPES)
            continue;
        Type *ty = &types[ntypes];
        ty->id = (int)t[0];
        ty->pieces = t[3] < MAX_PIECES ? (int)t[3] : MAX_PIECES;
        ty->hp = (int)t[5], ty->defense = (int)t[6];
        ty->cyl_r = f[7], ty->cyl_y0 = f[8], ty->cyl_y1 = f[9], ty->fr = f[10], ty->rest = f[11];
        ty->intact_bits = t[12], ty->broken_bits = t[13];
        float *pc = malloc(ty->pieces * 20 * sizeof *pc);  /* the blob goes after this call */
        ty->model = mdl_load(b + t[1]);
        if (!pc || !ty->model || mdl_bones(ty->model) != ty->pieces + 1 || mdl_upload(ty->model, gs) < 0 ||
            !mdl_compact(ty->model, t[2])) {
            printf("objects: block %d: o%04d did not load\n", block, ty->id);
            if (ty->model)
                mdl_free(ty->model);
            free(pc);
            continue;
        }
        memcpy(pc, b + t[4], ty->pieces * 20 * sizeof *pc);
        ty->piece = pc;
        map[k] = ntypes++;
    }
    free(blk_part[block]);
    blk_part[block] = malloc(h[3] * sizeof(Part));
    blk_nparts[block] = 0;
    for (unsigned k = 0; blk_part[block] && k < h[3]; k++) {
        const unsigned char *p = b + h[5] + ps * k;
        int ti = *(const unsigned short *)p;
        if (ti >= (int)h[2] || ti >= MAX_TYPES * 2 || map[ti] < 0)
            continue;
        Part *pt = &blk_part[block][blk_nparts[block]++];
        pt->type = map[ti];
        memcpy(pt->pos, p + 4, 12);
        memcpy(pt->q, p + 16, 16);
        pt->entity = ps == 48 ? *(const int *)(p + 32) : -1;
    }
    printf("objects: block %d: %d placements, %d types loaded\n", block, blk_nparts[block], ntypes);
    return blk_nparts[block];
}

static void retire(Inst *in);

void objects_block_add(int block)
{
    if (block <= 0 || block >= OBJ_BLOCKS)
        return;
    int i = 0;
    for (int k = 0; k < blk_nparts[block]; k++) {
        while (i < ninst && inst[i].block >= 0)  /* a free slot, else the end */
            i++;
        if (i == MAX_INST)
            break;
        Inst *in = &inst[i];
        if (i == ninst)
            ninst++;
        memset(in, 0, sizeof *in);
        memset(in->body, 0xFF, sizeof in->body);
        in->type = blk_part[block][k].type;
        in->block = block;
        in->entity = blk_part[block][k].entity;
        memcpy(in->pos, blk_part[block][k].pos, 12);
        memcpy(in->q, blk_part[block][k].q, 16);
    }
    objects_stats.instances = 0;
    for (int j = 0; j < ninst; j++)
        objects_stats.instances += inst[j].block >= 0;
}

void objects_block_remove(int block)
{
    if (block <= 0)
        return;
    for (int i = 0; i < ninst; i++)
        if (inst[i].block == block) {
            retire(&inst[i]);
            inst[i].block = -1;
            objects_stats.instances--;
        }
    while (ninst && inst[ninst - 1].block < 0)
        ninst--;
}

static void retire(Inst *in)
{
    Type *ty = &types[in->type];
    for (int j = 0; j < ty->pieces; j++)
        if (in->body[j] >= 0)
            bodies[in->body[j]].active = 0, in->body[j] = -1;
    in->broken = 2;
}

/* Free body slots, retiring the oldest broken objects until n are free; returns 1 when there is room */
static int make_room(int n)
{
    for (;;) {
        int free = 0;
        for (int i = 0; i < MAX_BODIES; i++)
            free += !bodies[i].active;
        if (free >= n)
            return 1;
        Inst *oldest = 0;
        for (int i = 0; i < ninst; i++)
            if (inst[i].broken == 1 && (!oldest || inst[i].t > oldest->t))
                oldest = &inst[i];
        if (!oldest)
            return 0;
        retire(oldest);
    }
}

void objects_break(int i, const float *dir)
{
    if (i < 0 || i >= ninst || inst[i].broken || inst[i].off || inst[i].block < 0)
        return;
    Inst *in = &inst[i];
    Type *ty = &types[in->type];
    in->broken = 1, in->t = 0;
    objects_stats.broken++;
    if (!make_room(ty->pieces)) {
        in->broken = 2;
        return;
    }
    float dx = dir ? dir[0] : 0, dz = dir ? dir[2] : 0, dl = sqrtf(dx * dx + dz * dz);
    if (dl > 1e-4f)
        dx /= dl, dz /= dl;
    float mid = (ty->cyl_y0 + ty->cyl_y1) * 0.5f;
    /* one floor query for all pieces: the pot's own floor (pieces re-query after moving 0.5 m) */
    ColHit fh;
    int floor_ok = col_floor(in->pos[0], in->pos[2], in->pos[1] + 1.0f, in->pos[1] - 3.0f, COL_SOLID, &fh);
    int slot = 0;
    for (int j = 0; j < ty->pieces; j++) {
        while (bodies[slot].active)
            slot++;
        const float *pc = ty->piece + 20 * j;
        Body *bd = &bodies[slot];
        memset(bd, 0, sizeof *bd);
        float rp[3];
        q_rot(in->q, pc, rp);
        for (int k = 0; k < 3; k++)
            bd->p[k] = in->pos[k] + rp[k];
        q_mul(in->q, pc + 3, bd->q);
        memcpy(bd->half, pc + 7, 12);
        memcpy(bd->c, pc + 10, 12);
        bd->inv_mass = pc[13];
        memcpy(bd->inv_i, pc + 14, 12);
        bd->fr = ty->fr, bd->rest = ty->rest;
        float hm = bd->half[0] > bd->half[1] ? bd->half[0] : bd->half[1];
        bd->rs = 0.6f * (hm > bd->half[2] ? hm : bd->half[2]);
        bd->group = i, bd->nocol = 0.3f, bd->active = 1;
        if (floor_ok) {
            bd->fl_ok = 1, bd->fl_age = 1;
            bd->fl_o[0] = in->pos[0], bd->fl_o[1] = fh.y, bd->fl_o[2] = in->pos[2];
            memcpy(bd->fl_n, fh.n, sizeof fh.n);
            bd->fl_x = in->pos[0], bd->fl_z = in->pos[2];
        }
        /* outward from the object's axis, along the hit, up with height; spin from a hash of the piece */
        float ox = rp[0], oz = rp[2], ol = sqrtf(ox * ox + oz * oz);
        if (ol > 1e-4f)
            ox /= ol, oz /= ol;
        float up = (pc[1] - ty->cyl_y0) / (mid - ty->cyl_y0 + 1e-3f);
        bd->v[0] = ox * PUSH_OUT + dx * PUSH_ALONG;
        bd->v[1] = PUSH_UP * (0.5f + 0.5f * up);
        bd->v[2] = oz * PUSH_OUT + dz * PUSH_ALONG;
        unsigned hsh = (unsigned)(j * 2654435761u + i * 40503u);
        for (int k = 0; k < 3; k++, hsh = hsh * 1103515245u + 12345u)
            bd->w[k] = SPIN * ((int)((hsh >> 16) & 0xFF) - 128) / 128.0f;
        in->body[j] = slot;
    }
    phys_wake(bodies, MAX_BODIES, in->pos, 2.0f);
}

/* Squared distance from point p to segment a-b */
static float pt_seg_d2(const float *p, const float *a, const float *b)
{
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2], t = l2 > 0 ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float d[3] = {ap[0] - ab[0] * t, ap[1] - ab[1] * t, ap[2] - ab[2] * t};
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

int objects_sweep(const float a[3], const float b[3], float r, float damage, const float src[3])
{
    int n = 0;
    for (int i = 0; i < ninst; i++) {
        Inst *in = &inst[i];
        if (in->broken || in->off)
            continue;
        Type *ty = &types[in->type];
        if (damage - ty->defense < ty->hp)
            continue;
        /* capsule vs the vertical cylinder: sample the blade at 5 points against the axis segment */
        float lo[3] = {in->pos[0], in->pos[1] + ty->cyl_y0, in->pos[2]}, hi[3] = {in->pos[0], in->pos[1] + ty->cyl_y1, in->pos[2]};
        float reach = r + ty->cyl_r;
        int hit = 0;
        for (int s = 0; s <= 4 && !hit; s++) {
            float k = s / 4.0f, p[3] = {a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k};
            hit = pt_seg_d2(p, lo, hi) < reach * reach;
        }
        if (!hit)
            continue;
        float dir[3] = {in->pos[0] - src[0], 0, in->pos[2] - src[2]};
        objects_break(i, dir);
        n++;
    }
    return n;
}

int objects_sweep_hook(const float a[3], const float b[3], float r, const CombatAttack *atk)
{
    return objects_sweep(a, b, r, atk->phys * atk->atk_phys_rate, atk->src_pos);
}

int objects_nearest(const float p[3], float *out)
{
    int best = -1;
    float bd = 1e30f;
    for (int i = 0; i < ninst; i++) {
        float dx = inst[i].pos[0] - p[0], dy = inst[i].pos[1] - p[1], dz = inst[i].pos[2] - p[2];
        float d = dx * dx + dy * dy * 4 + dz * dz;
        if (!inst[i].broken && !inst[i].off && d < bd)
            bd = d, best = i;
    }
    if (best >= 0 && out)
        memcpy(out, inst[best].pos, 12);
    return best;
}

int objects_push_capsule(float pos[3], float r, float h)
{
    int touched = 0;
    for (int i = 0; i < ninst; i++) {
        Inst *in = &inst[i];
        if (in->broken || in->off)
            continue;
        Type *ty = &types[in->type];
        if (pos[1] + h < in->pos[1] + ty->cyl_y0 || pos[1] > in->pos[1] + ty->cyl_y1 - 0.1f)
            continue;
        float dx = pos[0] - in->pos[0], dz = pos[2] - in->pos[2], d2 = dx * dx + dz * dz, rr = r + ty->cyl_r;
        if (d2 >= rr * rr)
            continue;
        float d = sqrtf(d2);
        if (d < 1e-4f)
            dx = 1, dz = 0, d = 1;
        pos[0] = in->pos[0] + dx / d * rr;
        pos[2] = in->pos[2] + dz / d * rr;
        touched = 1;
    }
    return touched;
}

static unsigned ticks(void)
{
#ifdef _EE
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));  /* COP0 Count, 294.912 MHz */
    return c;
#else
    return 0;
#endif
}

void objects_update(Player *p, float dt)
{
    if (!ninst)
        return;
    unsigned t0 = ticks();
    /* the player against intact objects: rolls / dashes and landings break them, anything else is pushed out */
    if (p && p->state != ST_DEAD) {
        float f[3] = {p->pos.x, p->pos.y, p->pos.z};
        int breaker = player_breaks_objects(p);
        int landing = p->airborne && p->vel.y < -2.0f;
        for (int i = 0; i < ninst; i++) {
            Inst *in = &inst[i];
            if (in->broken || in->off)
                continue;
            Type *ty = &types[in->type];
            float dx = in->pos[0] - f[0], dz = in->pos[2] - f[2], rr = 0.4f + ty->cyl_r + 0.1f;
            if (dx * dx + dz * dz >= rr * rr)
                continue;
            float top = in->pos[1] + ty->cyl_y1, bottom = in->pos[1] + ty->cyl_y0;
            if (f[1] > top + 0.3f || f[1] + 1.5f < bottom)
                continue;
            float dir[3] = {dx, 0, dz};
            if (breaker || (landing && f[1] > top - 0.5f))
                objects_break(i, dir);
        }
        if (objects_push_capsule(f, 0.4f, 1.5f))
            p->pos.x = f[0], p->pos.z = f[2];
    }
    int pieces = 0;
    for (int i = 0; i < ninst; i++) {
        Inst *in = &inst[i];
        if (in->broken != 1)
            continue;
        in->t += dt;
        Type *ty = &types[in->type];
        int sinking = in->t > PIECE_LIFE;
        for (int j = 0; j < ty->pieces; j++) {
            if (in->body[j] < 0)
                continue;
            Body *bd = &bodies[in->body[j]];
            if (sinking)
                bd->asleep = 1, bd->p[1] -= SINK_DEPTH / SINK_TIME * dt;
            pieces++;
        }
        if (in->t > PIECE_LIFE + SINK_TIME)
            retire(in);
    }
    objects_stats.awake = phys_step(bodies, MAX_BODIES, dt);
    objects_stats.pieces = pieces;
    objects_stats.queries = phys_queries;
    objects_stats.us = (int)((ticks() - t0) / 295);
}

void objects_draw(void)
{
    if (!ninst)
        return;
    V3 eye, right, up, fwd;
    float focal, focal_y, zscale;
    r_camera_basis(&eye, &right, &up, &fwd, &focal, &focal_y, &zscale);
    static BoneLocal pose[MAX_BONES];
    for (int i = 0; i < ninst; i++) {
        Inst *in = &inst[i];
        Type *ty = &types[in->type];
        if (in->broken == 2 || in->off)
            continue;
        float dx = in->pos[0] - eye.x, dy = in->pos[1] - eye.y, dz = in->pos[2] - eye.z;
        if (dx * dx + dy * dy + dz * dz > DRAW_DIST * DRAW_DIST || dx * fwd.x + dy * fwd.y + dz * fwd.z < -3)
            continue;
        memcpy(pose[0].q, in->q, 16);
        memcpy(pose[0].t, in->pos, 12);
        for (int j = 0; j < ty->pieces; j++) {
            if (in->broken && in->body[j] >= 0) {
                Body *bd = &bodies[in->body[j]];
                memcpy(pose[1 + j].q, bd->q, 16);
                memcpy(pose[1 + j].t, bd->p, 12);
            } else {
                memcpy(&pose[1 + j], &pose[0], sizeof pose[0]);
            }
        }
        mdl_hide(ty->model, in->broken ? ty->intact_bits : ty->broken_bits);
        /* yaw -pi: mdl_world's placement is the identity, so each bone's pose is its world transform */
        mdl_draw(ty->model, pose, v3(0, 0, 0), -3.14159265f, 0);
    }
}

/* EMEVD hooks (src/event.c): breakables by MSB entity id, in every loaded block */
int objects_entity(int entity, float *pos, int *broken)
{
    for (int i = 0; i < ninst; i++)
        if (inst[i].block >= 0 && inst[i].entity == entity && entity != -1) {
            if (pos)
                memcpy(pos, inst[i].pos, 12);
            if (broken)
                *broken = inst[i].broken != 0;
            return i;
        }
    return -1;
}

int objects_entity_break(int entity)
{
    int n = 0;
    for (int i = 0; i < ninst; i++)
        if (inst[i].block >= 0 && inst[i].entity == entity && entity != -1 && !inst[i].broken && !inst[i].off)
            objects_break(i, NULL), n++;
    return n;
}

int objects_entity_restore(int entity)
{
    int n = 0;
    for (int i = 0; i < ninst; i++)
        if (inst[i].block >= 0 && inst[i].entity == entity && entity != -1 && inst[i].broken) {
            retire(&inst[i]);  /* its pieces go */
            inst[i].broken = 0, inst[i].t = 0, n++;
            objects_stats.broken--;
        }
    return n;
}

int objects_entity_enable(int entity, int on)
{
    int n = 0;
    for (int i = 0; i < ninst; i++)
        if (inst[i].block >= 0 && inst[i].entity == entity && entity != -1)
            inst[i].off = !on, n++;
    return n;
}

#ifdef OBJ_HOST  /* host test harness (fndb/work/ptest/obj): the piece bodies */
Body *objects_bodies(int *n) { *n = MAX_BODIES; return bodies; }
#endif
