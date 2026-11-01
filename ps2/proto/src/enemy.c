/* Enemies and NPCs (enemy.h): data from tools/export_chr.py / export_enemies.py, AI from DSR's Lua scripts (ai.c on
 * the lua0 VM), behaviour from enemyCommon.esd.
 *
 * Frame of one character: combat sync (hits taken -> the ESD's damage env functions) -> AI (ai.c: goals -> one
 * action request, locomotion, facing, guard pose) -> ESD (enemyCommon: turns the request into anims, c1_0 / c1_3) ->
 * anim time, root motion of the action anim or locomotion (walk / run anims at their own root-motion speed) over
 * col_walk -> turning (NpcParam turnVellocity, not during TAE flag 7) -> pose -> attack sweeps (TAE AttackBehavior
 * windows, AtkParam_Npc dummy-poly capsule).
 * Each placement uses its own chr.bin variant (NpcParam + ThinkParam row): HP, defence, capsule, model masks
 * (#NN# meshes), animIdOffset (a01_ / a02_ anims tried first), think values. teamType 2 characters (bonfire dummy,
 * NPCs, Frampt) ignore the player until hit. */
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "enemy.h"
#include "model.h"
#include "vu1.h"
#include "ai.h"
#include "esd.h"
#include "combat.h"
#include "collision.h"
#include "pack.h"
#include "mapstream.h"
#include "drawgrp.h"
#include "render.h"

#define PI 3.14159265f
#define MAX_TYPES 20  /* the boot area's + every world block's (enemy_block_types, at boot) */
#define ACTIVE_DIST 45.0f   /* characters farther from the player than this are frozen (DSR: by draw group / distance) */
/* DSR runs and draws a character only while its MSB DrawGroups meet those of the collision part under the player
 * (dg.bin per block, src/drawgrp.c: dg_mask_shown). On top of that, at most ACTIVE_MAX update a frame, the nearest
 * (3D) first; the rest stay frozen where they are. ASSUMED budget cap (EE time ~0.6 ms per active character), TECH_DEBT
 * world-4. Burg entry of the world build, 2026-11-01 (tests/block_enemies.txt): draw groups leave up to 30 characters
 * active there (20-25 fps uncapped); at 16 every shot holds 30 fps (was 10 before draw groups) */
#ifndef ACTIVE_MAX
#define ACTIVE_MAX 16
#endif
/* at most this many drawn a frame, the nearest to the eye (~0.7 ms of EE each), of those in the view frustum and
 * shown by draw group; farther ones are not drawn. ASSUMED budget cap, TECH_DEBT world-4 */
#ifndef DRAW_MAX
#define DRAW_MAX 16
#endif
#define DRAW_DIST 40.0f
#define DRAW_FADE 8.0f      /* m before DRAW_DIST over which a character fades into the fog colour (vu1_skin_fade) */
#define CORPSE_TIME 6.0f    /* seconds a dead enemy stays visible (DSR dissolves the corpse) */
#define BLEND_DEFAULT 0.15f
/* Character types stream by distance (EE RAM): model data in RAM while a placement is within LOAD_DIST of the
 * player, freed beyond UNLOAD_DIST (textures stay in VRAM from boot, chr.bin stays resident: ~40 KB per type) */
#define LOAD_DIST 60.0f
#define UNLOAD_DIST 75.0f
#define ANIM_LOD_DIST 15.0f  /* beyond this, a character's pose is resampled every other frame */
#define TEAM_NEUTRAL 2      /* NpcParam teamType of the bonfire dummy, NPCs and Frampt */

/* chr.bin v4 (tools/export_chr.py) */
typedef struct {
    char magic[4];
    unsigned version, anims, windows, variants, anim_off, win_off, root_off, var_off, hide_bits;
    unsigned upper[2];  /* bones of the guard pose layer: the first Spine bone and its descendants */
} ChrHdr;
typedef struct {
    int npc, think, hp, logic, battle, anim_offset;
    unsigned show_bits;
    float radius, height, turn, def_phys, poise;
    float eye_dist, eye_ang_x, eye_ang_y, ear_dist, ear_cut, near_d, mid_d, far_d, out_d, sight_forget, sound_forget;
    int goal_action_caution, goal_id_caution, goal_action_find, goal_id_find;
    float call_range, call_min_dist;
    int team_type;
    float excel[AI_EXCEL_N];
} ChrVar;
typedef struct { int id; float length; unsigned first_win, nwin, first_root, nroot; } ChrAnim;
typedef struct {
    short type, bone_a, bone_b, dmg_level;
    int atk_param;
    float start, end, phys, radius, poise, a[3], b[3], phys_rate;
} ChrWin;
#define W_ATTACK -1
#define F_PARRY 5
#define F_NO_TURN 7
#define F_INVINCIBLE 8
#define F_STEP_A 11      /* 11 / 79 -> ctx+0x19C bit1: f118 (steps) answers inside them */
#define F_AI_COMBO 23    /* the AI may cancel / chain from here (IsEnableCancelMove) */
#define F_SUPER_ARMOR 24
#define F_STEP_B 79
#define F_COMBO 86       /* -> ctx+0x19C bit4: f120 (next attack of a combo) answers inside it */

typedef struct {
    char name[8];
    const char *dir;  /* pack directory under PACK_ROOT: CHR_DIR, or a world block's "<block>/" (its own variants) */
    unsigned chr_size;
    const ChrHdr *h;
    const ChrVar *vars;
    const ChrAnim *anims;
    const ChrWin *wins;
    const float (*root)[3];
    Model *model;   /* stripped (no streams / anims) while the type is unloaded: keeps the VRAM texture handles */
    int loaded;     /* model data in RAM (types load / unload by the player's distance to their placements) */
    float near_d;   /* player's distance to the nearest placement of this type (last frame) */
    int drawable;   /* the model has triangles (c1000, the bonfire dummy, has none) */
    unsigned retry; /* enemy_update frame before which a failed load is not tried again (each try waits on the IOP) */
    float walk_speed, run_speed;
} ChrType;

typedef struct {
    ChrType *ct;
    const ChrVar *var;
    AiThink think;
    int npc, thinkid, entity, index;
    V3 pos, home;
    float yaw, home_yaw;
    BoneLocal pose[MAX_BONES], from[MAX_BONES];
    float blend, blend_time;
    /* action anim (attacks, steps, damage, death) started by the ESD; -1 = none (locomotion / idle) */
    int act, act_slot, act_layer, act_info;
    float act_t, act_prev_t;
    int act_loop;
    unsigned act_serial;
    /* locomotion */
    int loco_enabled, loco_anim;
    float loco_t;
    Ai ai;
    AiFrame frame;
    EsdCtx esd;
    CombatChr cc;
    int hits_seen, damaged, registered, hostile;
    float dmg_latch;  /* a hit stays visible to the ESD (f206 / f236) until group 0 reacts or this runs out */
    int dmg_state0;
    int req;  /* the AI's request this frame (ctx+0x528 in the exe): attack / step / combo anim, -1 none */
    float dead_t;
    int block;  /* world block (src/world.c) of the placement: 0 the boot area */
    int dg;     /* its MSB DrawGroups: piece-mask index in the block's dg.bin (drawgrp.c), -1 always shown */
} Enemy;

static ChrType types[MAX_TYPES];
static int ntypes;
static Enemy enemies[MAX_ENEMIES];
static int nenemies;
/* world blocks' placements (enemies.bin records, 36 B), kept from boot; placed while the block is on */
static unsigned char *blk_rec[ENEMY_BLOCKS];
static int blk_n[ENEMY_BLOCKS];
#define EV_FREE 0x80      /* ev_off: a slot of a block that turned off (reused by the next enemy_block_add) */
#define ENT_FREE (-0x7fffffff)
static unsigned char ev_off[MAX_ENEMIES];  /* EMEVD (src/event.c): bit 0 disabled (Change Character Enable State),
                                            * bit 1 backread removed (Set Character Backread State); off if either */
static EsdProgram esd_prog;
static int esd_ok;
int enemy_active, enemy_drawn;
static int hits_on_player;
static unsigned frame;  /* enemy_update calls (anim LOD parity) */

/* ---- data ---- */

static const ChrAnim *anim_info(const ChrType *t, int id)
{
    for (unsigned i = 0; i < t->h->anims; i++)
        if (t->anims[i].id == id)
            return &t->anims[i];
    return NULL;
}

/* Root-motion speed of a looping anim (m/s), from its first and last sample */
static float loop_speed(const ChrType *t, int id)
{
    const ChrAnim *a = anim_info(t, id);
    if (!a || a->nroot < 2 || a->length <= 0)
        return 0;
    const float *r0 = t->root[a->first_root], *r1 = t->root[a->first_root + a->nroot - 1];
    return sqrtf((r1[0] - r0[0]) * (r1[0] - r0[0]) + (r1[1] - r0[1]) * (r1[1] - r0[1])) / ((a->nroot - 1) / 30.0f);
}

/* Temporary model.bin buffers of types streamed in after boot come from the near-cell arena (mapstream_alloc drops
 * the farthest cells): in the heap each load left a hole of the blob's size below the type's kept data, and cycling
 * two world blocks' 18 types shredded it (OUT OF RAM with 3 MB free, 2026-11-01) */
static const Player *last_player;
static void *arena_get(unsigned n)
{
    static const float none[3];
    void *p = mapstream_alloc(n, last_player ? &last_player->pos.x : none);
    return p ? p : memalign(128, n);  /* the cells in it are all being drawn: the heap after all */
}
static void arena_put(void *p, unsigned n)
{
    if (mapstream_owns(p))
        mapstream_free(p, n);
    else
        free(p);
}

/* (Re)load a type's model.bin: VU1 streams + compact anims; textures uploaded the first time only */
static int load_model(ChrType *t, GSGLOBAL *gs)
{
    char path[64];
    snprintf(path, sizeof path, PACK_ROOT "%s%.8s.pak", t->dir, t->name);
    Pack pk;
    if (!pack_open(&pk, path)) {
        printf("enemy: %s: no pack\n", path);
        return 0;
    }
    unsigned size = 0;
    int in_arena = mapstream_ready();
    void *mb = in_arena ? pack_load_with(&pk, "model.bin", &size, arena_get, arena_put) : pack_load(&pk, "model.bin", &size);
    pack_close(&pk);
#define FREE_MB() (in_arena ? arena_put(mb, (size + 127) & ~127u) : free(mb))
    Model *m = mb ? mdl_load(mb) : NULL;
    if (!m) {
        FREE_MB();
        printf("enemy: %s: model.bin did not load\n", t->name);
        return 0;
    }
    const ChrType *twin = NULL;  /* the same character from another world block's packs: same textures */
    for (int i = 0; i < ntypes && !t->model; i++)
        if (&types[i] != t && types[i].model && !strncmp(types[i].name, t->name, 8))
            twin = &types[i];
    if (!t->model && twin) {
        mdl_share_textures(m, twin->model);
        t->drawable = ((const unsigned *)mb)[3] > 0;
    } else if (!t->model) {
        int vram = mdl_upload(m, gs);
        if (vram < 0) {
            printf("enemy: %s: textures do not fit in VRAM\n", t->name);
            mdl_free(m), FREE_MB();
            return 0;
        }
        t->drawable = ((const unsigned *)mb)[3] > 0;  /* model.bin header: tris */
    } else {
        mdl_share_textures(m, t->model);
    }
    if (!mdl_compact(m, size)) {
        /* out of RAM: the type stays unloaded with its old stripped model (VRAM handles). Freeing that one before the
         * compact left t->model dangling and the retry 2 s later freed it again (manual-play crash in _free_r,
         * 2026-11-01) */
        printf("enemy: %.5s: no RAM to keep the model, RAM free %uK\n", t->name, enemy_ram_free() >> 10);
        if (t->model) {
            mdl_free(m);
        } else {
            mdl_strip(m);  /* a first load: keep the texture handles (VRAM is never freed) */
            t->model = m;
        }
        FREE_MB();
        return 0;
    }
    FREE_MB();
    if (t->model)
        mdl_free(t->model);
    t->model = m;
    t->loaded = 1;
    return 1;
#undef FREE_MB
}

static ChrType *load_type(GSGLOBAL *gs, const char *name, const char *dir)
{
    for (int i = 0; i < ntypes; i++)
        if (!strncmp(types[i].name, name, 8) && !strcmp(types[i].dir, dir))
            return &types[i];
    if (ntypes >= MAX_TYPES)
        return NULL;
    char path[64];
    snprintf(path, sizeof path, PACK_ROOT "%s%.8s.pak", dir, name);
    Pack pk;
    if (!pack_open(&pk, path))
        return NULL;
    unsigned csize = 0;
    void *cb = pack_load(&pk, "chr.bin", &csize);
    pack_close(&pk);
    for (int i = 0; cb && i < ntypes; i++)  /* the same character with the same variants in another block: one type */
        if (!strncmp(types[i].name, name, 8) && types[i].chr_size == csize && !memcmp(types[i].h, cb, csize)) {
            free(cb);
            return &types[i];
        }
    const ChrHdr *h = cb;
    if (!h || memcmp(h->magic, "DSRC", 4) || h->version != 5 || !h->variants) {
        printf("enemy: %s: bad chr.bin\n", name);
        free(cb);
        return NULL;
    }
    ChrType *t = &types[ntypes];
    memset(t, 0, sizeof *t);
    memcpy(t->name, name, 8);
    t->dir = dir;
    t->chr_size = csize;
    if (!load_model(t, gs)) {
        free(cb);
        return NULL;
    }
    ntypes++;
    t->h = h;
    t->vars = (const ChrVar *)((const char *)cb + h->var_off);
    t->anims = (const ChrAnim *)((const char *)cb + h->anim_off);
    t->wins = (const ChrWin *)((const char *)cb + h->win_off);
    t->root = (const float (*)[3])((const char *)cb + h->root_off);
    t->walk_speed = loop_speed(t, 200);
    t->run_speed = loop_speed(t, 500);
    t->near_d = 1e9f;
    printf("enemy: %s %u variants, %u anims, VRAM free %d, RAM free %uK\n", name, h->variants, h->anims,
           4 * 1024 * 1024 - gs->CurrentPointer, enemy_ram_free() >> 10);
    return t;
}

/* ---- ESD hooks (enemyCommon.esd; ids from tools/esd.py dumps, meanings from how the ESD uses them) ---- */

static int in_window(const Enemy *e, int type)
{
    if (e->act < 0 || !e->act_info)
        return 0;
    const ChrAnim *a = &e->ct->anims[e->act_info - 1];
    for (unsigned i = 0; i < a->nwin; i++) {
        const ChrWin *w = &e->ct->wins[a->first_win + i];
        if (w->type == type && e->act_t >= w->start && e->act_t < w->end)
            return 1;
    }
    return 0;
}

static float act_length(const Enemy *e)
{
    if (e->act_info)
        return e->ct->anims[e->act_info - 1].length;
    return e->act_slot >= 0 ? mdl_anim_length(e->ct->model, e->act_slot) : 0;
}

static int act_ended(const Enemy *e) { return e->act < 0 || (!e->act_loop && e->act_t >= act_length(e)); }

static int enemy_env(EsdCtx *c, int id, const float *a, int n, float *out)
{
    Enemy *e = c->chr;
    (void)n;
    float v = 0;
    switch (id) {
    case 0:    v = act_ended(e); break;                      /* action anim finished (group 1 -> state 0) */
    case 105:  v = -1; break;                                /* AI special action request (1000-1700): none */
    /* the AI's one request (exe: ctx float +0x528, reset to -1 every frame, written by the AI), gated by TAE flags */
    case 106:  v = e->req; break;                            /* action request (attacks 3000-4112) */
    case 107:  v = -1; break;                                /* = req with flag 23 && ctx+0x31C bit5 (unknown): events */
    case 118:  v = in_window(e, F_STEP_A) || in_window(e, F_STEP_B) ? e->req : -1; break;  /* steps 700-723 */
    case 119:  v = 0; break;                                 /* AI ctrl (ChrCtrl+0x258) +0x10 */
    case 120:  v = in_window(e, F_COMBO) ? e->req : -1; break;  /* next combo anim inside the TAE flag 86 window */
    case 200:  v = 0; break;                                 /* falling (state 1, anim 1500): never here */
    case 201:  v = 1; break;                                 /* on ground */
    case 202:  v = 99999; break;                             /* scripted event request: none */
    case 205:  v = 0; break;
    case 206:  v = !e->damaged; break;                       /* 0 on the frame a hit landed (damage branches) */
    case 222:  {  /* hit direction, as esd_env_move.c reads the reaction poses: 3 front, 2 behind, 0 / 1 sides */
        float d = e->cc.hit_dir;
        v = fabsf(d) < PI / 4 ? 3 : fabsf(d) > 3 * PI / 4 ? 2 : d > 0 ? 0 : 1;
        break;
    }
    case 224:  v = 0; break;                                 /* fall distance */
    case 236:  v = e->damaged ? e->cc.damage_level : -1; break;  /* damage level of this frame's hit */
    case 237:  v = 0; break;                                 /* damage reaction class */
    case 285:  v = 0; break;                                 /* damage info 4 (2 / 6: guarding variants) */
    case 1000: v = e->cc.hp; break;                          /* HP (below 0 once dead) */
    case 1001: v = 100; break;                               /* stamina */
    case 1003: {                                             /* RandInt(min, max) */
        int lo = (int)a[0], hi = (int)a[1];
        v = hi > lo ? lo + rand() % (hi - lo + 1) : lo;
        break;
    }
    default:   v = 0; break;                                 /* f247 / f264 hover, f250+ ladders, f305 SpEffects... */
    }
    *out = v;
    return 1;
}

/* Anim slot for an ESD anim id: NpcParam animIdOffset's version (a01_ = +10000, a02_ = +20000) when exported */
static int anim_slot(const Enemy *e, int anim, int *full)
{
    int s = -1;
    if (e->var->anim_offset > 0 && (s = mdl_anim(e->ct->model, anim + e->var->anim_offset)) >= 0) {
        *full = anim + e->var->anim_offset;
        return s;
    }
    *full = anim;
    return mdl_anim(e->ct->model, anim);
}

static void start_action(Enemy *e, int anim, int layer, float blend, int loop)
{
    memcpy(e->from, e->pose, sizeof e->pose);
    e->blend = 0, e->blend_time = blend > 0 ? blend : 0.05f;
    e->act = anim, e->act_layer = layer, e->act_loop = loop;
    int full;
    e->act_slot = anim_slot(e, anim, &full);
    const ChrAnim *ai = anim_info(e->ct, full);
    e->act_info = ai ? (int)(ai - e->ct->anims) + 1 : 0;
    e->act_t = e->act_prev_t = 0;
    e->act_serial++;
}

static int enemy_cmd(EsdCtx *c, int bank, int id, const float *a, int n)
{
    Enemy *e = c->chr;
    if (bank != 1)
        return 0;
    switch (id) {
    case 0:    /* c1_0(anim, layer, blend, loop): play / stop (-1) an anim on a layer */
    case 3:    /* c1_3(weapon sel, anim, layer, blend, loop) */
    {
        int k = id == 3 ? 1 : 0;
        if (n < k + 2)
            return 1;
        int anim = (int)a[k], layer = (int)a[k + 1];
        if (anim >= 0)
            start_action(e, anim, layer, n > k + 2 ? a[k + 2] : 0.1f, n > k + 3 ? a[k + 3] != 0 : 0);
        else if (e->act >= 0 && e->act_layer == layer)
            e->act = -1, e->act_info = 0, e->blend = 0, e->blend_time = BLEND_DEFAULT,
            memcpy(e->from, e->pose, sizeof e->pose);
        return 1;
    }
    case 101:  /* SetLocomotionEnabled */
        e->loco_enabled = n > 0 && a[0] != 0;
        return 1;
    }
    return 1;  /* c1_11 additive layers, c1_100-144 context flags, c1_9000 debug print: no effect here */
}

/* ---- spawn ---- */

static void think_of(AiThink *th, const ChrVar *v)
{
    th->npc = v->npc, th->think = v->think, th->logic = v->logic, th->battle = v->battle;
    th->eye_dist = v->eye_dist, th->eye_ang_x = v->eye_ang_x, th->eye_ang_y = v->eye_ang_y;
    th->ear_dist = v->ear_dist, th->ear_cut = v->ear_cut;
    th->near_d = v->near_d, th->mid_d = v->mid_d, th->far_d = v->far_d, th->out_d = v->out_d;
    th->sight_forget = v->sight_forget, th->sound_forget = v->sound_forget;
    th->goal_action_caution = v->goal_action_caution, th->goal_id_caution = v->goal_id_caution;
    th->goal_action_find = v->goal_action_find, th->goal_id_find = v->goal_id_find;
    th->call_range = v->call_range, th->call_min_target_dist = v->call_min_dist;
    memcpy(th->excel, v->excel, sizeof th->excel);
}

static GSGLOBAL *gsg;

unsigned enemy_ram_free(void)
{
    struct mallinfo mi = mallinfo();
    return 32 * 1024 * 1024 - 128 * 1024 - (unsigned)sbrk(0) + mi.fordblks;
}

/* enemies.bin record r (type t) into slot i of world block `block`; seed_i: its index in the file (AI seed) */
static void place(ChrType *t, const unsigned char *r, int i, int block, int seed_i)
{
    Enemy *e = &enemies[i];
    memset(e, 0, sizeof *e);
    ev_off[i] = 0;  /* a reused slot inherits no event state */
    e->ct = t;
    e->block = block;
    memcpy(&e->npc, r + 8, 4), memcpy(&e->thinkid, r + 12, 4), memcpy(&e->entity, r + 16, 4);
    e->var = &t->vars[0];
    for (unsigned v = 0; v < t->h->variants; v++)
        if (t->vars[v].npc == e->npc && t->vars[v].think == e->thinkid)
            e->var = &t->vars[v];
    e->index = i;
    float f[4];
    memcpy(f, r + 20, 16);
    e->pos = e->home = v3(f[0], f[1], f[2]);
    e->yaw = e->home_yaw = f[3];
    e->dg = dg_char(block, f);
    e->act = -1;
    int full;
    e->loco_anim = anim_slot(e, 0, &full);
    if (t->loaded)  /* else the pose is sampled once the type streams in */
        mdl_sample(t->model, e->loco_anim, 0, 1, e->pose);
    e->blend = 1;
    e->hostile = e->var->team_type != TEAM_NEUTRAL;
    think_of(&e->think, e->var);
    ai_init(&e->ai, &e->think, e->index, e->entity ^ (seed_i * 7919));
    CombatChr *cc = &e->cc;
    memcpy(cc->pos, &e->pos, 12);
    cc->yaw = e->yaw, cc->radius = e->var->radius, cc->height = e->var->height, cc->team = TEAM_ENEMY;
    cc->hp = cc->hp_max = e->var->hp, cc->def_phys = e->var->def_phys;
    cc->poise_max = e->var->poise;
    if (esd_ok) {
        esd_ctx_init(&e->esd, &esd_prog, ESD_ENEMY, e, enemy_env, enemy_cmd);
        esd_start(&e->esd);
    }
}

static int inited;  /* enemy_init ran (not with the test option noenemies): the VM / ESD exist */

int enemy_init(GSGLOBAL *gs, const void *placements, const void *map_lua, int map_lua_size)
{
    gsg = gs;
    inited = 1;
    const unsigned char *b = placements;
    if (!b || memcmp(b, "DSRN", 4))
        return 0;
    Pack pk;
    void *eb = NULL, *lb = NULL;
    unsigned lsize = 0;
    if (pack_open(&pk, PACK_ROOT CHR_DIR "common.pak")) {
        eb = pack_load(&pk, "esd.bin", 0);
        lb = pack_load(&pk, "ai.lua0", &lsize);  /* aiCommon.luabnd (tools/lua50.py --pack) */
    }
    pack_close(&pk);
    esd_ok = esd_load(&esd_prog, eb);
#ifndef NO_AI_SCRIPTS
    ai_scripts(lb, (int)lsize, map_lua, map_lua_size);
#endif
    free(lb);  /* the VM keeps its own copy of the code */
    unsigned count = *(const unsigned *)(b + 4);
    for (unsigned i = 0; i < count && nenemies < MAX_ENEMIES; i++) {
        const unsigned char *r = b + 8 + 36 * i;
        char name[9] = {0};
        memcpy(name, r, 8);
#ifdef ENEMY_ONLY
#define STR_(x) #x
#define STR(x) STR_(x)
        if (!strstr(STR(ENEMY_ONLY), name))  /* test builds: DEFS=-DENEMY_ONLY=c2900_c2540 */
            continue;
#endif
        ChrType *t = load_type(gs, name, CHR_DIR);
        if (!t)
            continue;
        place(t, r, nenemies++, 0, i);
    }
    /* every type starts unloaded (textures stay in VRAM); enemy_update loads the ones near the player */
    for (int i = 0; i < ntypes; i++)
        mdl_strip(types[i].model), types[i].loaded = 0;
    printf("enemy: %d characters of %d types, RAM free %uK after unloading\n", nenemies, ntypes,
           enemy_ram_free() >> 10);
    return nenemies;
}

static char blk_dir[ENEMY_BLOCKS][32];
static ChrType *blk_type[ENEMY_BLOCKS][MAX_ENEMIES];  /* per kept placement (may be another block's identical type) */

int enemy_block_types(GSGLOBAL *gs, int block, const char *dir, const void *placements, const void *map_lua,
                      int map_lua_size)
{
    const unsigned char *b = placements;
    if (!inited || block <= 0 || block >= ENEMY_BLOCKS || !b || memcmp(b, "DSRN", 4))
        return 0;
    unsigned count = *(const unsigned *)(b + 4);
    snprintf(blk_dir[block], sizeof blk_dir[block], "%s/", dir);
    free(blk_rec[block]);
    blk_rec[block] = malloc(count * 36 + 1);
    blk_n[block] = 0;
    for (unsigned i = 0; blk_rec[block] && i < count && i < MAX_ENEMIES; i++) {
        const unsigned char *r = b + 8 + 36 * i;
        char name[9] = {0};
        memcpy(name, r, 8);
        int npc, think;
        memcpy(&npc, r + 8, 4), memcpy(&think, r + 12, 4);
        ChrType *t = NULL;
        for (int j = 0; j < ntypes && !t; j++)  /* a loaded type of this character with this placement's variant */
            for (unsigned v = 0; v < types[j].h->variants && !strncmp(types[j].name, name, 8); v++)
                if (types[j].vars[v].npc == npc && types[j].vars[v].think == think) {
                    t = &types[j];
                    break;
                }
        if (!t)
            t = load_type(gs, name, blk_dir[block]);  /* the IOP reads happen here, at boot, never while walking */
        if (!t)
            continue;
        blk_type[block][blk_n[block]] = t;
        if (t->loaded)
            mdl_strip(t->model), t->loaded = 0;
        memcpy(blk_rec[block] + 36 * blk_n[block]++, r, 36);
    }
#ifndef NO_AI_SCRIPTS
    if (map_lua)
        ai_scripts_add(map_lua, map_lua_size);  /* the block's map logic functions join the one VM */
#endif
    printf("enemy: block %d: %d placements, %d types, RAM free %uK\n", block, blk_n[block], ntypes,
           enemy_ram_free() >> 10);
    return blk_n[block];
}

void enemy_block_add(int block)
{
    if (block <= 0 || block >= ENEMY_BLOCKS)
        return;
    int i = 0, n = 0;
    for (int k = 0; k < blk_n[block]; k++) {
        const unsigned char *r = blk_rec[block] + 36 * k;
        ChrType *t = NULL;
        t = blk_type[block][k];
        if (!t)
            continue;
        while (i < nenemies && !(ev_off[i] & EV_FREE))
            i++;
        if (i == MAX_ENEMIES)
            break;
        if (i == nenemies)
            nenemies++;
        place(t, r, i, block, k);
        n++;
    }
    printf("enemy: block %d on: %d characters (%d slots), %d goals in use\n", block, n, nenemies, ai_goals_in_use());
}

void enemy_block_remove(int block)
{
    if (block <= 0)
        return;
    for (int i = 0; i < nenemies; i++) {
        Enemy *e = &enemies[i];
        if (e->block != block || (ev_off[i] & EV_FREE))
            continue;
        if (e->registered)
            combat_unregister(&e->cc), e->registered = 0;
        /* its goal tree goes back to the pool (Lua terminate runs, as at death): place() memsets the slot, and a live
         * tree was lost there. tests/world_blockoff.txt, 2026-11-01: ~100 of the 768 pool slots lost per Burg block
         * off (623 in use after 6 rounds), 49 steady with this */
        ai_reset(&e->ai);
        ev_off[i] = EV_FREE;  /* update / draw skip it (ev_off != 0); stream_types / status by dead_t */
        e->entity = ENT_FREE, e->dead_t = 1e9f, e->pos = v3(1e6f, 0, 1e6f), e->block = -1;
    }
    while (nenemies && (ev_off[nenemies - 1] & EV_FREE))
        nenemies--;
    printf("enemy: block %d off: %d slots, %d goals in use, RAM free %uK\n", block, nenemies, ai_goals_in_use(),
           enemy_ram_free() >> 10);
}

/* Load the types whose placements came within LOAD_DIST (all at once on the first frame, then one per frame), free
 * the ones beyond UNLOAD_DIST */
static void stream_types(const Player *pl)
{
    static int first = 1;
    for (int i = 0; i < ntypes; i++)
        types[i].near_d = 1e9f;
    for (int i = 0; i < nenemies; i++) {
        Enemy *e = &enemies[i];
        /* 3D: a placement on a level far above / below (the Burg under Firelink's bonfire, 32-41 m lower) does not hold
         * its type's model in RAM (2026-11-01: xz kept c2500 + c3491 loaded at the bonfire, 0.85 MB) */
        float d = sqrtf((e->pos.x - pl->pos.x) * (e->pos.x - pl->pos.x) + (e->pos.y - pl->pos.y) * (e->pos.y - pl->pos.y) +
                        (e->pos.z - pl->pos.z) * (e->pos.z - pl->pos.z));
        if (e->dead_t <= CORPSE_TIME && d < e->ct->near_d)
            e->ct->near_d = d;
    }
    int loads = 0;
    for (int i = 0; i < ntypes; i++) {
        ChrType *t = &types[i];
        if (t->loaded && t->near_d > UNLOAD_DIST) {
            mdl_strip(t->model), t->loaded = 0;
            printf("enemy: %.5s unloaded, RAM free %uK\n", t->name, enemy_ram_free() >> 10);
        } else if (!t->loaded && t->near_d < LOAD_DIST && (first || !loads) && frame >= t->retry) {
            loads++;
            if (load_model(t, gsg))
                printf("enemy: %.5s loaded, RAM free %uK\n", t->name, enemy_ram_free() >> 10);
            else
                t->near_d = 1e9f, t->retry = frame + 60;  /* out of RAM: again in 2 s, not every frame */
        }
    }
    first = 0;
}

/* ---- update ---- */

static float wrap(float a)
{
    while (a > PI)
        a -= 2 * PI;
    while (a < -PI)
        a += 2 * PI;
    return a;
}

/* Root-motion sample of the action anim at time t (x, z, yaw in model space, linear between 30 fps samples) */
static void root_at(const Enemy *e, float t, float *o)
{
    const ChrAnim *a = &e->ct->anims[e->act_info - 1];
    if (!a->nroot) {
        o[0] = o[1] = o[2] = 0;
        return;
    }
    float f = t * 30;
    if (f < 0)
        f = 0;
    if (f > a->nroot - 1)
        f = a->nroot - 1;
    int i = (int)f, j = i + 1 < (int)a->nroot ? i + 1 : i;
    float k = f - i;
    for (int c = 0; c < 3; c++)
        o[c] = e->ct->root[a->first_root + i][c] * (1 - k) + e->ct->root[a->first_root + j][c] * k;
}

static void move_by(Enemy *e, float dx, float dz)
{
    float p[3] = {e->pos.x, e->pos.y, e->pos.z};
    col_walk(p, dx, dz, e->var->radius, e->var->height, COL_STEP_UP, COL_STEP_DOWN, COL_SOLID, 0);
    e->pos = v3(p[0], p[1], p[2]);  /* no floor: stays at its height (no falling for enemies yet) */
}

static void attack_sweeps(Enemy *e)
{
    if (e->act < 0 || !e->act_info || e->cc.dead)
        return;
    const ChrAnim *a = &e->ct->anims[e->act_info - 1];
    static float world[MAX_BONES][12];
    int have_world = 0;
    for (unsigned i = 0; i < a->nwin; i++) {
        const ChrWin *w = &e->ct->wins[a->first_win + i];
        if (w->type != W_ATTACK || e->act_t < w->start || e->act_t >= w->end)
            continue;
        if (!have_world)
            mdl_world(e->ct->model, e->pose, e->pos, e->yaw, world), have_world = 1;
        V3 pa = mdl_point(world[w->bone_a], w->a), pb = mdl_point(world[w->bone_b], w->b);
        float fa[3] = {pa.x, pa.y, pa.z}, fb[3] = {pb.x, pb.y, pb.z};
        CombatAttack atk = {.team = TEAM_ENEMY, .atk_param = w->atk_param, .phys = w->phys, .atk_phys_rate = w->phys_rate,
                            .poise_dmg = w->poise, .damage_level = w->dmg_level,
                            .window = (e->index << 20) | ((e->act_serial & 0x3FFF) << 6) | (int)i};
        memcpy(atk.src_pos, &e->pos, 12);
        hits_on_player += combat_sweep(fa, fb, w->radius, &atk);
    }
}

static void update_one(Enemy *e, Player *pl, float dt)
{
    ChrType *t = e->ct;
    /* hits taken -> the damage env functions. Each ESD group steps once per frame, so a one-frame flag can be missed
     * (e.g. while group 0 leaves a hit-reaction state): latch it until group 0 changes state (0.2 s at most), and
     * keep a dead enemy flagged until a death anim plays */
    int new_hit = 0;
    if (e->cc.hit_count != e->hits_seen) {
        e->hits_seen = e->cc.hit_count;
        e->dmg_latch = 0.2f, e->dmg_state0 = esd_state(&e->esd, 0);
        new_hit = 1, e->hostile = 1;
    } else if (e->dmg_latch > 0) {
        e->dmg_latch -= dt;
        if (esd_state(&e->esd, 0) != e->dmg_state0)
            e->dmg_latch = 0;
    }
    e->damaged = e->dmg_latch > 0 || (e->cc.dead && e->act != 2200 && e->act != 2205);
    if (e->cc.dead) {
        if (e->registered)
            combat_unregister(&e->cc), e->registered = 0;
        if (e->dead_t == 0)
            ai_reset(&e->ai);
        e->dead_t += dt;
    }

    /* AI: what it sees this frame */
    AiFrame *f = &e->frame;
    f->pos = e->pos, f->home = e->home, f->yaw = e->yaw, f->home_yaw = e->home_yaw, f->radius = e->var->radius;
    f->hp = e->cc.hp, f->hp_max = e->cc.hp_max;
    f->target = pl->pos, f->target_yaw = pl->yaw, f->target_hp = pl->hp, f->target_hp_max = pl->hp_max;
    f->target_alive = pl->hp > 0 && e->hostile;
    f->target_attack = player_attack_anim(pl);
    int playing = e->act >= 0 && !act_ended(e);
    f->anim = playing ? e->act : -1;
    f->anim_t = e->act_t, f->anim_len = act_length(e);
    f->combo_window = in_window(e, F_COMBO);
    f->cancel_window = in_window(e, F_AI_COMBO);
    f->damaged = new_hit;
    f->dt = dt;
    const AiOut *o = &e->ai.out;
    if (!e->cc.dead)
        ai_update(&e->ai, f);
    e->req = e->cc.dead ? -1 : o->req;
    if (esd_ok) {
        e->esd.dt = dt;
        esd_update(&e->esd);
    }

    /* animation time and movement */
    if (e->act >= 0) {
        e->act_prev_t = e->act_t;
        e->act_t += dt;
        float len = act_length(e);
        if (e->act_loop && len > 0 && e->act_t >= len)
            e->act_t -= len, e->act_prev_t -= len;
        if (e->act_info && !e->cc.dead) {
            float r0[3], r1[3];
            root_at(e, e->act_prev_t, r0);
            root_at(e, e->act_t, r1);
            float dx = r1[0] - r0[0], dz = r1[1] - r0[1];
            /* model space -> world: the model faces -Z; place rotation is yaw + pi */
            float c = cosf(e->yaw + PI), s = sinf(e->yaw + PI);
            move_by(e, c * dx + s * dz, -s * dx + c * dz);
            e->yaw = wrap(e->yaw - (r1[2] - r0[2]));
        }
    }
    int moving = !e->cc.dead && (e->act < 0 || act_ended(e)) && e->loco_enabled && o->move;
    int loco = 0;
    if (moving) {
        float rel = wrap(o->move_yaw - e->yaw), speed = o->run ? t->run_speed : t->walk_speed;
        loco = o->run ? 500 : fabsf(rel) < PI / 4 ? 200 : fabsf(rel) > 3 * PI / 4 ? 201 : rel > 0 ? 202 : 203;
        move_by(e, sinf(o->move_yaw) * speed * dt, cosf(o->move_yaw) * speed * dt);
    } else if (o->guard > 0 && !e->cc.dead)
        loco = o->guard;  /* guard pose while standing (DoEzAction 9910 / Guard goal) */
    int full, la = anim_slot(e, loco, &full);
    if (la < 0)
        la = anim_slot(e, 0, &full);
    if (la != e->loco_anim) {
        if (e->act < 0)
            memcpy(e->from, e->pose, sizeof e->pose), e->blend = 0, e->blend_time = BLEND_DEFAULT;
        e->loco_anim = la;
    }
    e->loco_t += dt;
    /* turning: NpcParam turnVellocity, not inside TAE "disable turning" windows */
    if (o->face && !e->cc.dead && !in_window(e, F_NO_TURN)) {
        float d = wrap(o->face_yaw - e->yaw), step = e->var->turn * (PI / 180) * dt;
        e->yaw = wrap(e->yaw + (d > step ? step : d < -step ? -step : d));
    }

    /* pose: only for characters on screen or inside an attack window (the hit capsules read it); beyond
     * ANIM_LOD_DIST every other frame (CHR pass budget) */
    V3 eye, right, up, fwd;
    float fo, fy, zs;
    r_camera_basis(&eye, &right, &up, &fwd, &fo, &fy, &zs);
    V3 dv = v3sub(e->pos, eye);
    float z = dv.x * fwd.x + dv.y * fwd.y + dv.z * fwd.z, dd = dv.x * dv.x + dv.z * dv.z;
    int on_screen = z > -(e->var->height + 2) && dd < DRAW_DIST * DRAW_DIST;
    int attacking = e->act >= 0 && e->act_info && t->anims[e->act_info - 1].nwin > 0;
    if (!attacking && (!on_screen || (dd > ANIM_LOD_DIST * ANIM_LOD_DIST && ((frame + e->index) & 1)))) {
        memcpy(e->cc.pos, &e->pos, 12);
        e->cc.yaw = e->yaw;
        e->cc.invincible = in_window(e, F_INVINCIBLE);
        e->cc.super_armor = in_window(e, F_SUPER_ARMOR);
        e->blend += e->blend_time > 0 ? dt / e->blend_time : 1;
        return;
    }
    BoneLocal target[MAX_BONES];
    if (e->act >= 0 && e->act_slot >= 0)
        mdl_sample(t->model, e->act_slot, e->act_t, e->act_loop, target);
    else {
        mdl_sample(t->model, e->loco_anim, e->loco_t, 1, target);
        int gs = moving && o->guard > 0 ? anim_slot(e, o->guard, &full) : -1;
        if (gs >= 0) {  /* guard while walking: the guard pose on the spine and everything above it */
            BoneLocal g[MAX_BONES];
            mdl_sample(t->model, gs, e->loco_t, 1, g);
            for (int b = 0; b < mdl_bones(t->model) && b < 64; b++)
                if (t->h->upper[b >> 5] >> (b & 31) & 1)
                    target[b] = g[b];
        }
    }
    e->blend += e->blend_time > 0 ? dt / e->blend_time : 1;
    if (e->blend < 1)
        mdl_blend(t->model, e->from, target, e->blend, e->pose);
    else
        memcpy(e->pose, target, sizeof target);

    memcpy(e->cc.pos, &e->pos, 12);
    e->cc.yaw = e->yaw;
    e->cc.invincible = in_window(e, F_INVINCIBLE);
    e->cc.super_armor = in_window(e, F_SUPER_ARMOR);
    attack_sweeps(e);
}

/* Character capsules push each other apart (DSR: Havok character proxies); only the enemy moves, through col_walk */
static void separate(Enemy *e, V3 other, float other_r, float share)
{
    if (e->cc.dead || fabsf(e->pos.y - other.y) > e->var->height)
        return;
    float dx = e->pos.x - other.x, dz = e->pos.z - other.z, d = sqrtf(dx * dx + dz * dz);
    float min = e->var->radius + other_r;
    if (d >= min)
        return;
    if (d < 1e-3f)
        dx = sinf(e->yaw + PI), dz = cosf(e->yaw + PI), d = 1;  /* exactly on top: back off along its facing */
    float k = (min - d) * share / d;
    move_by(e, dx * k, dz * k);
}

/* The player's noise for the AI's hearing (DSR: sound events from TAE / footsteps; radii here are guesses) */
static void player_sounds(const Player *pl)
{
    float r = 0;
    switch (pl->state) {
    case ST_MOVE: r = pl->speed > 4.5f ? 10 : pl->speed > 2.5f ? 6 : 2; break;
    case ST_ROLL:
    case ST_LAND: r = 8; break;
    case ST_ATTACK: r = 8; break;
    default: break;
    }
    if (r > 0 && pl->hp > 0)
        ai_sound(pl->pos, r);
}

void enemy_update(Player *pl, float dt)
{
    last_player = pl;
    enemy_active = 0;
    if (++frame == 1 || frame == 120)
        printf("enemy: frame %u, Lua heap %u, %d characters, RAM free %uK\n", frame, ai_lua_heap, nenemies,
               enemy_ram_free() >> 10);
    player_sounds(pl);
    stream_types(pl);
    static Ai *team[MAX_ENEMIES];
    static V3 tpos[MAX_ENEMIES];
    for (int i = 0; i < nenemies; i++)
        team[i] = &enemies[i].ai, tpos[i] = enemies[i].pos;
    ai_team(team, tpos, nenemies);
    Enemy *act[MAX_ENEMIES];
    int na = 0;
    float d3[MAX_ENEMIES], lim = 1e30f;
    int nc = 0;
    for (int i = 0; i < nenemies; i++) {
        V3 d = v3sub(enemies[i].pos, pl->pos);
        d3[i] = d.x * d.x + d.y * d.y + d.z * d.z;
        nc += enemies[i].ct->loaded && d.x * d.x + d.z * d.z <= ACTIVE_DIST * ACTIVE_DIST &&
              enemies[i].dead_t <= CORPSE_TIME && !ev_off[i] && dg_mask_shown(enemies[i].block, enemies[i].dg);
    }
    if (nc > ACTIVE_MAX) {  /* the ACTIVE_MAX-th smallest distance of the candidates */
        float c[MAX_ENEMIES];
        int m = 0;
        for (int i = 0; i < nenemies; i++) {
            V3 d = v3sub(enemies[i].pos, pl->pos);
            if (enemies[i].ct->loaded && d.x * d.x + d.z * d.z <= ACTIVE_DIST * ACTIVE_DIST &&
                enemies[i].dead_t <= CORPSE_TIME && !ev_off[i] && dg_mask_shown(enemies[i].block, enemies[i].dg))
                c[m++] = d3[i];
        }
        for (int k = 0; k < ACTIVE_MAX; k++)  /* partial selection sort */
            for (int j = k + 1; j < m; j++)
                if (c[j] < c[k]) {
                    float t = c[k];
                    c[k] = c[j], c[j] = t;
                }
        lim = c[ACTIVE_MAX - 1];
    }
    for (int i = 0; i < nenemies; i++) {
        Enemy *e = &enemies[i];
        V3 d = v3sub(e->pos, pl->pos);
        int on = e->ct->loaded && d.x * d.x + d.z * d.z <= ACTIVE_DIST * ACTIVE_DIST && e->dead_t <= CORPSE_TIME &&
                 !ev_off[i] && dg_mask_shown(e->block, e->dg) && d3[i] <= lim;
        /* combat slots only for characters near the player (COMBAT_MAX_CHRS is smaller than the roster) */
        int want = on && !e->cc.dead && e->ct->drawable;
        if (want && !e->registered)
            e->registered = combat_register(&e->cc) >= 0;
        else if (!want && e->registered)
            combat_unregister(&e->cc), e->registered = 0;
        if (!on)
            continue;
        update_one(e, pl, dt);
        act[na++] = e;
    }
    enemy_active = na;
    ai_end_frame();
#ifdef ENEMY_HIT_TEST
    /* test build (make DEFS=-DENEMY_HIT_TEST): a Longsword-like hit on the nearest live enemy every 2 s, to see the
     * ESD's damage reactions (f206 / f236 -> 2000-series anims) and the death (HP < 0 -> 2200) without pad input */
    static float hit_clock;
    if ((hit_clock += dt) > 2.0f) {
        hit_clock = 0;
        Enemy *n = NULL;
        float nd = 1e9f;
        for (int i = 0; i < na; i++) {
            V3 d = v3sub(act[i]->pos, pl->pos);
            if (!act[i]->cc.dead && act[i]->registered && d.x * d.x + d.z * d.z < nd)
                nd = d.x * d.x + d.z * d.z, n = act[i];
        }
        static int win = 0x7F000000;
        CombatAttack atk = {.team = TEAM_PLAYER, .atk_param = 0, .phys = 141, .atk_phys_rate = 1, .poise_dmg = 20,
                            .damage_level = 1, .window = win++};
        if (n) {
            memcpy(atk.src_pos, &pl->pos, 12);
            combat_apply(&n->cc, &atk);
        }
    }
#endif
    for (int i = 0; i < na; i++) {
        if (!act[i]->ct->drawable)
            continue;
        separate(act[i], pl->pos, COL_PLAYER_R, 1.0f);
        for (int j = i + 1; j < na; j++) {
            if (!act[j]->ct->drawable)
                continue;
            V3 pi = act[i]->pos;
            separate(act[i], act[j]->pos, act[j]->var->radius, 0.5f);
            separate(act[j], pi, act[i]->var->radius, 0.5f);
        }
        memcpy(act[i]->cc.pos, &act[i]->pos, 12);
    }
}

void enemy_draw(void)
{
    V3 eye, right, up, fwd;
    float f, fy, zs;
    r_camera_basis(&eye, &right, &up, &fwd, &f, &fy, &zs);
    enemy_drawn = 0;
    float d3[MAX_ENEMIES], lim = 1e30f;
    int nd = 0;
    for (int i = 0; i < nenemies; i++) {
        Enemy *e = &enemies[i];
        V3 d = v3sub(e->pos, eye);
        /* view cone: a box around the capsule with room for limbs and weapons (ASSUMED 1.5 m; poses are not bounded) */
        float R = e->var->radius + 1.5f;
        const float mn[3] = {e->pos.x - R, e->pos.y - 0.5f, e->pos.z - R},
                    mx[3] = {e->pos.x + R, e->pos.y + e->var->height + 1.0f, e->pos.z + R};
        d3[i] = !e->ct->loaded || !e->ct->drawable || e->dead_t > CORPSE_TIME || ev_off[i] ||
                        d.x * d.x + d.z * d.z > DRAW_DIST * DRAW_DIST || !dg_mask_shown(e->block, e->dg) ||
                        !r_box_classify(mn, mx)
                    ? -1
                    : d.x * d.x + d.y * d.y + d.z * d.z;
        if (d3[i] >= 0)
            nd++;
    }
    if (nd > DRAW_MAX) {  /* the DRAW_MAX-th smallest distance */
        float c[MAX_ENEMIES];
        int m = 0;
        for (int i = 0; i < nenemies; i++)
            if (d3[i] >= 0)
                c[m++] = d3[i];
        for (int k = 0; k < DRAW_MAX; k++)
            for (int j = k + 1; j < m; j++)
                if (c[j] < c[k]) {
                    float t = c[k];
                    c[k] = c[j], c[j] = t;
                }
        lim = c[DRAW_MAX - 1];
    }
    for (int i = 0; i < nenemies; i++) {
        Enemy *e = &enemies[i];
        V3 d = v3sub(e->pos, eye);
        if (d3[i] < 0 || d3[i] > lim)
            continue;
        mdl_hide(e->ct->model, e->ct->h->hide_bits & ~e->var->show_bits);  /* #NN# meshes of other variants */
        /* fade into the fog over the last DRAW_FADE m instead of popping at DRAW_DIST (render-distance; ASSUMED) */
        float fd = (DRAW_DIST - sqrtf(d.x * d.x + d.z * d.z)) / DRAW_FADE;
        vu1_skin_fade(fd < 1 ? 255 * fd : 255);
        mdl_draw(e->ct->model, e->pose, e->pos, e->yaw, 0);
        enemy_drawn++;
    }
}

int enemy_view(int index, V3 *pos, float *height, float *yaw)
{
    if (index < 0 || index >= nenemies)
        return 0;
    *pos = enemies[index].pos, *height = enemies[index].var->height, *yaw = enemies[index].yaw;
    return 1;
}

void enemy_status(char *buf, int n)
{
    if (pack_error[0]) {  /* a failed load stays on the HUD (pack.c) */
        snprintf(buf, n, "%s", pack_error);
        return;
    }
    const Enemy *best = NULL;
    float bd = 1e9f;
    for (int i = 0; i < nenemies && last_player; i++) {
        const Enemy *e = &enemies[i];
        V3 d = v3sub(e->pos, last_player->pos);
        float dd = d.x * d.x + d.z * d.z;
        if (e->dead_t <= CORPSE_TIME && e->ct->loaded && e->ct->drawable && dd < bd)
            bd = dd, best = e;
    }
    if (!best) {
        snprintf(buf, n, "ENEMIES %d LUA %uK E%d", nenemies, ai_lua_heap >> 10, ai_lua_errors);
        return;
    }
    /* name, AI goal, distance, HP, action anim, ESD states (group 0 / 1), hits landed on the player, Lua heap */
    snprintf(buf, n, "%.5s %s %dM HP%d A%d S%d/%d PH%d L%uK E%d", best->ct->name,
             best->cc.dead ? "DEAD" : ai_goal_name(&best->ai), (int)sqrtf(bd), best->cc.hp, best->act,
             esd_state(&best->esd, 0), esd_state(&best->esd, 1), hits_on_player, ai_lua_heap >> 10, ai_lua_errors);
}

/* EMEVD hooks (src/event.c): placements by MSB entity id */
int enemy_entity(int entity, V3 *pos, int *dead, float *hp_ratio, int *active)
{
    for (int i = 0; i < nenemies; i++) {
        const Enemy *e = &enemies[i];
        if (e->entity != entity)
            continue;
        V3 d = last_player ? v3sub(e->pos, last_player->pos) : v3(0, 0, 0);
        if (pos) *pos = e->pos;
        if (dead) *dead = e->cc.dead;
        if (hp_ratio) *hp_ratio = e->cc.hp_max > 0 ? (float)e->cc.hp / e->cc.hp_max : 1;
        if (active) *active = !ev_off[i] && e->ct->loaded && d.x * d.x + d.z * d.z <= ACTIVE_DIST * ACTIVE_DIST;
        return i;
    }
    return -1;
}

int enemy_entity_enable(int entity, int on, int bit)
{
    int n = 0;
    for (int i = 0; i < nenemies; i++)
        if (enemies[i].entity == entity) {
            int was = ev_off[i] != 0;
            ev_off[i] = on ? ev_off[i] & ~bit : ev_off[i] | bit, n++;
            if (was != (ev_off[i] != 0))
                printf("enemy: %d %.5s (placement %d) %s by an event\n", entity, enemies[i].ct->name, i,
                       ev_off[i] ? "off" : "on");
            on = !ev_off[i];
            if (!on && enemies[i].registered)
                combat_unregister(&enemies[i].cc), enemies[i].registered = 0;
        }
    return n;
}

int enemy_entity_kill(int entity)
{
    for (int i = 0; i < nenemies; i++)
        if (enemies[i].entity == entity && !enemies[i].cc.dead) {
            enemies[i].cc.hp = -1, enemies[i].cc.dead = 1;  /* below 0: the ESD death test f1000() < 0 */
            return 1;
        }
    return 0;
}
