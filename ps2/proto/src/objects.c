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
#include <stdlib.h>
#include <string.h>
#include "objects.h"
#include "physics.h"
#include "model.h"
#include "player.h"
#include "combat.h"
#include "collision.h"
#include "vmath.h"
#include "ticks.h"

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
    int tex_refs;              /* blocks on that place it (its textures are in VRAM while > 0); the boot area's: 1 */
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
/* a block's breakable types and their model.bin offsets in its objects.bin (textures from there when it turns on) */
static struct { short type; unsigned off; } blk_tm[OBJ_BLOCKS][MAX_TYPES];
static int blk_ntm[OBJ_BLOCKS];
static GSGLOBAL *obj_gs;
static Body bodies[MAX_BODIES];
ObjectsStats objects_stats;

/* ---- animated objects (objects.bin v3: doors, levers, gates, chests). Events and ObjActs play their Havok anims
 * (objects_anim_*: src/ev_obj.c); each hkx body is a box on a bone that moves with the pose and pushes the player.
 * Without an anim they hold the reference pose (closed), or the first frame of their MSB init anim (ASSUMED). ---- */
#define MAX_ATYPES 16
#define MAX_AINST 48
#define AMAX_BONES 32  /* export_objects.py keeps types within this (root bone mask) */
#define STEP_OVER 0.4f /* ASSUMED: object boxes whose top is this close above the feet are walked over, not pushed */
typedef struct { int bone; float c[3], q[4], half[3]; unsigned pad; } ABody;  /* objects.bin body record (48 B) */
typedef struct {
    int id, nbodies;
    unsigned roots;    /* bit per root bone: the placement rotation goes into their pose */
    ABody *body;       /* own copy (a block's blob is freed after objects_block_types) */
    Model *model;
} AType;
typedef struct {
    int type, block, entity, off;
    float pos[3], q[4];
    int anim, anim_id, loop, playing;  /* model anim index (-1 reference pose) and its id; playing: t advances */
    float t;
} AInst;
typedef struct { unsigned short type; float pos[3], q[4]; int entity, init; } APart;
/* Objects the events animate but the port keeps static (elevator o1460: A9): anim lengths only, so an event waiting
 * for one takes DSR's time; the anim state is kept for tests */
#define MAX_ASTATIC 8
typedef struct { int entity, n, block, anim, playing; float t, len, alen[8]; int aid[8]; } AStatic;
static AStatic astatic[MAX_ASTATIC];
static AType atypes[MAX_ATYPES];
static AInst ainst[MAX_AINST];
static int natypes, nainst;
static APart *ablk_part[OBJ_BLOCKS];
static int ablk_nparts[OBJ_BLOCKS];

/* objects.bin v3 animated types of a pack (block 0: the boot area, blob kept; compact: a block's, blob freed after) and
 * its placements; returns the placements */
static int anim_load(GSGLOBAL *gs, int block, const void *blob, int compact)
{
    const unsigned *h = blob;
    if (block < 0 || block >= OBJ_BLOCKS || h[1] < 3)
        return 0;
    const unsigned char *b = blob;
    for (unsigned k = 0; k < h[12]; k++) {  /* static ones: lengths only (block -2 - b: block b's, not placed) */
        int j = 0;
        while (j < MAX_ASTATIC && astatic[j].entity)
            j++;
        if (j == MAX_ASTATIC)
            break;
        const int *r = (const int *)(b + h[13] + 72 * k);
        AStatic *a = &astatic[j];
        memset(a, 0, sizeof *a);
        a->entity = r[0], a->n = r[1] < 8 ? r[1] : 8, a->block = -2 - block, a->anim = -1;  /* placed by anim_add */
        for (int i = 0; i < a->n; i++)
            a->aid[i] = r[2 + 2 * i], memcpy(&a->alen[i], &r[3 + 2 * i], 4);
    }
    if (!h[6])
        return 0;
    Model *tex = h[11] ? mdl_load(b + h[11]) : NULL;  /* the pack's shared textures (one bone, no triangles) */
    if (tex && mdl_upload(tex, gs) < 0)
        printf("objects: block %d: animated object textures did not fit in VRAM\n", block);
    int map[MAX_ATYPES];
    for (unsigned k = 0; k < h[6] && k < MAX_ATYPES; k++) {
        const unsigned *t = (const unsigned *)(b + h[8] + 32 * k);
        map[k] = -1;
        for (int j = 0; j < natypes; j++)
            if (atypes[j].id == (int)t[0])
                map[k] = j;
        if (map[k] >= 0 || natypes == MAX_ATYPES)
            continue;
        AType *ty = &atypes[natypes];
        memset(ty, 0, sizeof *ty);
        ty->id = (int)t[0], ty->nbodies = (int)t[3], ty->roots = t[6];
        ty->body = malloc(ty->nbodies * sizeof(ABody) + 1);
        ty->model = mdl_load_tex(b + t[1], tex);
        if (!ty->body || !ty->model || mdl_bones(ty->model) > AMAX_BONES || mdl_upload(ty->model, gs) < 0 ||
            (compact && !mdl_compact(ty->model, t[2]))) {
            printf("objects: block %d: animated o%04d did not load\n", block, ty->id);
            if (ty->model)
                mdl_free(ty->model);
            free(ty->body);
            continue;
        }
        if (tex)
            mdl_share_textures(ty->model, tex);
        memcpy(ty->body, b + h[10] + 48 * t[4], ty->nbodies * sizeof(ABody));
        map[k] = natypes++;
    }
    if (tex)
        mdl_free(tex);  /* its VRAM textures stay (now the types') */
    free(ablk_part[block]);
    ablk_part[block] = malloc(h[7] * sizeof(APart) + 1);
    ablk_nparts[block] = 0;
    for (unsigned k = 0; ablk_part[block] && k < h[7]; k++) {
        const unsigned char *p = b + h[9] + 48 * k;
        int ti = *(const unsigned short *)p;
        if (ti >= (int)h[6] || ti >= MAX_ATYPES || map[ti] < 0)
            continue;
        APart *pt = &ablk_part[block][ablk_nparts[block]++];
        pt->type = map[ti];
        memcpy(pt->pos, p + 4, 12);
        memcpy(pt->q, p + 16, 16);
        memcpy(&pt->entity, p + 32, 4);
        memcpy(&pt->init, p + 36, 4);
    }
    printf("objects: block %d: %d animated placements, %d animated types\n", block, ablk_nparts[block], natypes);
    return ablk_nparts[block];
}

static void anim_add(int block)
{
    for (int j = 0; j < MAX_ASTATIC; j++)
        if (astatic[j].entity && (astatic[j].block == -2 - block || astatic[j].block == block))
            astatic[j].block = block, astatic[j].anim = -1, astatic[j].playing = 0;
    int i = 0;
    for (int k = 0; k < ablk_nparts[block]; k++) {
        while (i < nainst && ainst[i].block >= 0)
            i++;
        if (i == MAX_AINST)
            break;
        if (i == nainst)
            nainst++;
        const APart *pt = &ablk_part[block][k];
        AInst *in = &ainst[i];
        memset(in, 0, sizeof *in);
        in->type = pt->type, in->block = block, in->entity = pt->entity;
        memcpy(in->pos, pt->pos, 12);
        memcpy(in->q, pt->q, 16);
        in->anim = pt->init >= 0 ? mdl_anim(atypes[pt->type].model, pt->init) : -1;  /* held at its first frame */
        in->anim_id = in->anim >= 0 ? pt->init : -1;
    }
}

/* Pose of an animated instance with the placement rotation in its root bones (drawn / placed at pos, yaw -pi =
 * no further rotation) */
static void anim_pose(const AInst *in, BoneLocal *pose)
{
    const AType *ty = &atypes[in->type];
    mdl_sample(ty->model, in->anim, in->t, in->loop, pose);
    for (int b = 0; b < mdl_bones(ty->model); b++)
        if (ty->roots >> b & 1) {
            float q[4], t[3];
            q_mul(in->q, pose[b].q, q);
            q_rot(in->q, pose[b].t, t);
            memcpy(pose[b].q, q, 16);
            memcpy(pose[b].t, t, 12);
        }
}

/* Push a vertical capsule out of the animated objects' boxes. Each box: its world centre and axes from the bone; the
 * push works in the horizontal plane on the box's footprint (its projected extents along a horizontal-ish box axis
 * and the perpendicular: exact for upright boxes such as doors and gates, a bound for tilted ones) */
static int anim_push(float pos[3], float r, float h)
{
    int touched = 0;
    static BoneLocal pose[MAX_BONES];
    static float world[MAX_BONES][12];
    for (int i = 0; i < nainst; i++) {
        AInst *in = &ainst[i];
        if (in->block < 0 || in->off)
            continue;
        float dx = pos[0] - in->pos[0], dz = pos[2] - in->pos[2];
        if (dx * dx + dz * dz > 12 * 12)  /* ASSUMED: no object box reaches beyond 12 m of its origin */
            continue;
        const AType *ty = &atypes[in->type];
        anim_pose(in, pose);
        mdl_world(ty->model, pose, v3(in->pos[0], in->pos[1], in->pos[2]), -3.14159265f, world);
        for (int k = 0; k < ty->nbodies; k++) {
            const ABody *bd = &ty->body[k];
            const float *w = world[bd->bone < mdl_bones(ty->model) ? bd->bone : 0];
            V3 c = mdl_point(w, bd->c);
            float ax[3][3];  /* the box's axes in world: bone rotation x box rotation */
            for (int a = 0; a < 3; a++) {
                float e[3] = {a == 0, a == 1, a == 2}, l[3];
                q_rot(bd->q, e, l);
                for (int j = 0; j < 3; j++)
                    ax[a][j] = w[3 * j] * l[0] + w[3 * j + 1] * l[1] + w[3 * j + 2] * l[2];
            }
            float ey = 0;
            for (int a = 0; a < 3; a++)
                ey += fabsf(ax[a][1]) * bd->half[a];
            if (pos[1] + h < c.y - ey || pos[1] > c.y + ey || c.y + ey < pos[1] + STEP_OVER)
                continue;
            int m = fabsf(ax[0][1]) < 0.9f ? 0 : 2;  /* a horizontal-ish axis gives the footprint's direction */
            float ul = sqrtf(ax[m][0] * ax[m][0] + ax[m][2] * ax[m][2]);
            if (ul < 1e-4f)
                continue;
            float ux = ax[m][0] / ul, uz = ax[m][2] / ul, eu = 0, ev = 0;
            for (int a = 0; a < 3; a++) {
                eu += fabsf(ax[a][0] * ux + ax[a][2] * uz) * bd->half[a];
                ev += fabsf(-ax[a][0] * uz + ax[a][2] * ux) * bd->half[a];
            }
            float px = pos[0] - c.x, pz = pos[2] - c.z;
            float lu = px * ux + pz * uz, lv = -px * uz + pz * ux;
            float du = eu + r - fabsf(lu), dv = ev + r - fabsf(lv);
            if (du <= 0 || dv <= 0)
                continue;
            if (du < dv)
                lu += lu < 0 ? -du : du;
            else
                lv += lv < 0 ? -dv : dv;
            pos[0] = c.x + lu * ux - lv * uz;
            pos[2] = c.z + lu * uz + lv * ux;
            touched = 1;
        }
    }
    return touched;
}

static void anim_update(float dt)
{
    for (int j = 0; j < MAX_ASTATIC; j++)
        if (astatic[j].block >= 0 && astatic[j].playing && (astatic[j].t += dt) >= astatic[j].len)
            astatic[j].t = astatic[j].len, astatic[j].playing = 0;
    for (int i = 0; i < nainst; i++) {
        AInst *in = &ainst[i];
        if (in->block < 0 || !in->playing)
            continue;
        in->t += dt;
        float len = mdl_anim_length(atypes[in->type].model, in->anim);
        if (!in->loop && in->t >= len)
            in->t = len, in->playing = 0;
    }
}

static void anim_draw(V3 eye, V3 fwd)
{
    static BoneLocal pose[MAX_BONES];
    for (int i = 0; i < nainst; i++) {
        AInst *in = &ainst[i];
        if (in->block < 0 || in->off)
            continue;
        float dx = in->pos[0] - eye.x, dy = in->pos[1] - eye.y, dz = in->pos[2] - eye.z;
        if (dx * dx + dy * dy + dz * dz > DRAW_DIST * DRAW_DIST || dx * fwd.x + dy * fwd.y + dz * fwd.z < -6)
            continue;
        anim_pose(in, pose);
        mdl_draw(atypes[in->type].model, pose, v3(in->pos[0], in->pos[1], in->pos[2]), -3.14159265f, 0);
    }
}

static AInst *anim_find(int entity)
{
    for (int i = 0; entity != -1 && i < nainst; i++)
        if (ainst[i].block >= 0 && ainst[i].entity == entity)
            return &ainst[i];
    return NULL;
}

static AStatic *static_anim(int entity, int anim, float *len)
{
    for (int j = 0; entity != -1 && j < MAX_ASTATIC; j++)
        if (astatic[j].block >= 0 && astatic[j].entity == entity)
            for (int i = 0; i < astatic[j].n; i++)
                if (astatic[j].aid[i] == anim) {
                    *len = astatic[j].alen[i];
                    return &astatic[j];
                }
    return NULL;
}

float objects_anim_play(int entity, int anim, int loop)
{
    float len;
    AStatic *st = static_anim(entity, anim, &len);
    if (st) {  /* not drawn moving: the time passes all the same (loops end at once, ASSUMED harmless) */
        st->anim = anim, st->t = 0, st->len = loop ? 0 : len, st->playing = !loop;
        printf("objects: %d anim %d: static object (A9), %.2f s\n", entity, anim, len);
        return len;
    }
    AInst *in = anim_find(entity);
    int a = in ? mdl_anim(atypes[in->type].model, anim) : -1;
    if (a < 0)
        return -1;
    in->anim = a, in->anim_id = anim, in->t = 0, in->loop = loop != 0, in->playing = 1;
    return mdl_anim_length(atypes[in->type].model, a);
}

int objects_anim_end(int entity, int anim)
{
    float len;
    AStatic *st = static_anim(entity, anim, &len);
    if (st) {
        st->anim = anim, st->t = st->len = len, st->playing = 0;
        return 1;
    }
    AInst *in = anim_find(entity);
    int a = in ? mdl_anim(atypes[in->type].model, anim) : -1;
    if (a < 0)
        return 0;
    in->anim = a, in->anim_id = anim, in->loop = 0, in->playing = 0;
    in->t = mdl_anim_length(atypes[in->type].model, a);
    return 1;
}

int objects_anim_state(int entity, int *anim, float *t, int *playing)
{
    for (int j = 0; entity != -1 && j < MAX_ASTATIC; j++)
        if (astatic[j].block >= 0 && astatic[j].entity == entity) {
            if (anim)
                *anim = astatic[j].anim;
            if (t)
                *t = astatic[j].t;
            if (playing)
                *playing = astatic[j].playing;
            return 1;
        }
    AInst *in = anim_find(entity);
    if (!in)
        return 0;
    if (anim)
        *anim = in->anim_id;
    if (t)
        *t = in->t;
    if (playing)
        *playing = in->playing;
    return 1;
}

int objects_init(GSGLOBAL *gs, const void *blob)
{
    const unsigned *h = blob;
    ntypes = ninst = 0;
    memset(bodies, 0, sizeof bodies);
    if (!blob || memcmp(blob, "DSRB", 4) || h[1] < 1 || h[1] > 3)
        return 0;
    const unsigned char *b = blob;
    unsigned ps = h[1] >= 2 ? 48 : 32;  /* part record (export_objects.py: version 2 adds the entity id) */
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
        ty->tex_refs = 1;
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
    anim_load(gs, 0, blob, 0);
    anim_add(0);
    return ninst;
}

/* a breakable type of a block's objects.bin (b, type record t): model compacted (the blob is freed after
 * objects_block_types), textures not uploaded (objects_block_add does, from the objects.bin read with the block) */
static int load_block_type(int block, const unsigned char *b, const unsigned *t)
{
    const float *f = (const float *)t;
    Type *ty = &types[ntypes];
    memset(ty, 0, sizeof *ty);
    ty->id = (int)t[0];
    ty->pieces = t[3] < MAX_PIECES ? (int)t[3] : MAX_PIECES;
    ty->hp = (int)t[5], ty->defense = (int)t[6];
    ty->cyl_r = f[7], ty->cyl_y0 = f[8], ty->cyl_y1 = f[9], ty->fr = f[10], ty->rest = f[11];
    ty->intact_bits = t[12], ty->broken_bits = t[13];
    float *pc = malloc(ty->pieces * 20 * sizeof *pc);  /* the blob goes after objects_block_types */
    ty->model = mdl_load(b + t[1]);
    if (!pc || !ty->model || mdl_bones(ty->model) != ty->pieces + 1 || !mdl_compact(ty->model, t[2])) {
        printf("objects: block %d: o%04d did not load\n", block, ty->id);
        if (ty->model)
            mdl_free(ty->model);
        free(pc);
        return -1;
    }
    memcpy(pc, b + t[4], ty->pieces * 20 * sizeof *pc);
    ty->piece = pc;
    return ntypes++;
}

int objects_block_types(GSGLOBAL *gs, int block, const void *blob)
{
    const unsigned *h = blob;
    if (block <= 0 || block >= OBJ_BLOCKS || !blob || memcmp(blob, "DSRB", 4) || h[1] < 1 || h[1] > 3)
        return 0;
    obj_gs = gs;
    const unsigned char *b = blob;
    unsigned ps = h[1] >= 2 ? 48 : 32;
    int map[MAX_TYPES * 2];
    blk_ntm[block] = 0;
    for (unsigned k = 0; k < h[2] && k < MAX_TYPES * 2; k++) {
        const unsigned *t = (const unsigned *)(b + h[4] + 64 * k);
        map[k] = -1;
        for (int j = 0; j < ntypes; j++)  /* the same object model in another block: one type */
            if (types[j].id == (int)t[0])
                map[k] = j;
        if (map[k] < 0 && ntypes < MAX_TYPES)
            map[k] = load_block_type(block, b, t);
        if (map[k] >= 0 && blk_ntm[block] < MAX_TYPES)
            blk_tm[block][blk_ntm[block]].type = map[k], blk_tm[block][blk_ntm[block]++].off = t[1];
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
    return blk_nparts[block] + anim_load(gs, block, blob, 1);
}

static void retire(Inst *in);

void objects_block_add(int block, const void *blob)
{
    if (block <= 0 || block >= OBJ_BLOCKS)
        return;
    for (int k = 0; k < blk_ntm[block]; k++) {  /* textures of its types no other block on holds */
        Type *ty = &types[blk_tm[block][k].type];
        if (!ty->tex_refs++ && (!blob || mdl_upload_from(ty->model, (const unsigned char *)blob + blk_tm[block][k].off,
                                                          obj_gs) < 0))
            printf("objects: block %d: o%04d textures %s (VRAM free %d)\n", block, ty->id,
                   blob ? "do not fit in VRAM" : "missing", r_vram_avail(obj_gs));
    }
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
    anim_add(block);
}

void objects_block_remove(int block)
{
    if (block <= 0 || block >= OBJ_BLOCKS)
        return;
    for (int k = 0; k < blk_ntm[block]; k++) {
        Type *ty = &types[blk_tm[block][k].type];
        if (ty->tex_refs > 0 && !--ty->tex_refs)
            mdl_free_textures(ty->model);  /* its instances are gone with the block */
    }
    for (int i = 0; i < ninst; i++)
        if (inst[i].block == block) {
            retire(&inst[i]);
            inst[i].block = -1;
            objects_stats.instances--;
        }
    while (ninst && inst[ninst - 1].block < 0)
        ninst--;
    for (int i = 0; i < nainst; i++)
        if (ainst[i].block == block)
            ainst[i].block = -1;
    for (int j = 0; j < MAX_ASTATIC; j++)
        if (astatic[j].entity && astatic[j].block == block)
            astatic[j].block = -2 - block;  /* back to loaded, not placed */
    while (nainst && ainst[nainst - 1].block < 0)
        nainst--;
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
    int touched = anim_push(pos, r, h);
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


void objects_update(Player *p, float dt)
{
    anim_update(dt);
    if (!ninst && !nainst)
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
    if (!ninst && !nainst)
        return;
    V3 eye, right, up, fwd;
    float focal, focal_y, zscale;
    r_camera_basis(&eye, &right, &up, &fwd, &focal, &focal_y, &zscale);
    anim_draw(eye, fwd);
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
    for (int i = 0; i < nainst; i++)
        if (ainst[i].block >= 0 && ainst[i].entity == entity && entity != -1)
            ainst[i].off = !on, n++;
    return n;
}

#ifdef OBJ_HOST  /* host test harness (fndb/work/ptest/obj): the piece bodies */
Body *objects_bodies(int *n) { *n = MAX_BODIES; return bodies; }
#endif
