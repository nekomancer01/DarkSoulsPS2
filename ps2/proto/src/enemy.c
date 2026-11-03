/* Enemies and NPCs (enemy.h): data from tools/export_chr.py / export_enemies.py, AI from DSR's Lua scripts (ai.c on
 * the lua0 VM), behaviour from enemyCommon.esd.
 *
 * Frame of one character: combat sync (hits taken -> the ESD's damage env functions) -> AI (ai.c: goals -> one
 * action request, locomotion, facing, guard pose) -> ESD (enemyCommon: turns the request into anims, c1_0 / c1_3) ->
 * anim time, root motion of the action anim or locomotion (walk / run anims at their own root-motion speed) over
 * col_walk -> turning (NpcParam turnVellocity, not during TAE flag 7) -> pose -> attack sweeps (TAE AttackBehavior
 * windows, AtkParam_Npc dummy-poly capsule).
 * Physics from the chr hkx ragdoll (chr.bin v6, src/ragdoll.c): weapons hit the ragdoll's capsules (pelvis, spine,
 * head, limbs) instead of one cylinder, and a dead character falls into a simulated ragdoll at the death anim's TAE
 * flag 12 (or its end). Movement: towards a destination the AI names (MoveToSomewhere, approaches, BackToHome) the
 * route follows the navmesh (src/nav.c); walking stops at ledges, root motion and pushes can carry a character off
 * one, and it falls under gravity.
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
#include "progress.h"
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
#include "ev_chr.h"
#include "npc.h"
#include "sound.h"
#include "ragdoll.h"
#include "nav.h"

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
#define CORPSE_TIME 12.0f   /* seconds a dead enemy stays (ragdoll); ASSUMED, DSR keeps it until the map reloads */
#define BLEND_DEFAULT 0.15f
/* Character types stream by distance: model data in RAM and its textures in VRAM while a placement is within
 * LOAD_DIST of the player, both freed beyond UNLOAD_DIST (chr.bin stays resident: ~40 KB per type). Only the types
 * near the player hold VRAM, so every world block's roster fits (TECH_DEBT world-4) */
#define LOAD_DIST 60.0f
#define UNLOAD_DIST 75.0f
#define ANIM_LOD_DIST 15.0f  /* beyond this, a character's pose is resampled every other frame */
#define TEAM_NEUTRAL 2      /* NpcParam teamType of the bonfire dummy, NPCs and Frampt */

/* chr.bin v6 (tools/export_chr.py) */
typedef struct {
    char magic[4];
    unsigned version, anims, windows, variants, anim_off, win_off, root_off, var_off, hide_bits;
    unsigned upper[2];  /* bones of the guard pose layer: the first Spine bone and its descendants */
    unsigned rag_off, rag_n;     /* ragdoll bodies (RagBody, src/ragdoll.h) */
    float rag_reach, rag_top;    /* their bind-pose bounds around the root */
} ChrHdr;
_Static_assert(sizeof(ChrHdr) == 64, "export_chr.py chr.bin header");
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
#define F_RAGDOLL 12     /* the ragdoll takes over (ASSUMED, export_chr.py FLAGS) */
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
    Model *model;   /* NULL while the type is unloaded */
    int loaded;     /* model data in RAM (types load / unload by the player's distance to their placements) */
    float near_d;   /* player's distance to the nearest placement of this type (last frame) */
    int drawable;   /* the model has triangles (c1000, the bonfire dummy, has none) */
    unsigned retry; /* enemy_update frame before which a failed load is not tried again (each try waits on the IOP) */
    float walk_speed, run_speed;
    const RagBody *rag;  /* hkx ragdoll bodies (hit capsules, death ragdoll); NULL none (c1000) */
    int nrag;
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
    EvChr *ev;  /* what events set for its entity id (ev_chr.c), NULL none */
    int ev_team, ev_sp, ev_rot, ev_replan, ev_reset;  /* ev's values / serials last applied */
    int sp_on;  /* in an event's special wait (2004[09]): its idle anim loops, no AI */
    int no_award;  /* killed by an event that gives no souls (2004[04] arg 2 = 0) */
    unsigned char rec[ENEMY_REC];  /* its enemies.bin record and seed index: enemy_respawn places it again from them */
    int seed_i;
    /* physics */
    float vy, fall_y0;  /* airborne: vertical speed, height it left the ground at */
    int air;
    Ragdoll *rag;       /* from rag_pool while its ragdoll is simulated, NULL otherwise */
    int rag_done;       /* the ragdoll ran (the pose is its last, kept) */
    /* navmesh route (src/nav.c): next corner towards the AI's destination, replanned every NAV_REPLAN s */
    float nav_wp[3], nav_dest[3], nav_t;
    int nav_ok;
} Enemy;

static ChrType types[MAX_TYPES];
static int ntypes;
static Enemy enemies[MAX_ENEMIES];
static int nenemies;
/* world blocks' placements (enemies.bin records, ENEMY_REC B), kept from boot; placed while the block is on */
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
/* Ragdolls simulated at once (~3 KB each; ~0.3 ms of EE each while moving, ASSUMED budget): a death beyond this takes
 * the slot of the oldest one, which keeps its last pose */
#define RAG_POOL 4
static Ragdoll rag_pool[RAG_POOL];
static Enemy *rag_owner[RAG_POOL];
#define NAV_REPLAN 0.4f      /* s between route searches of one character (1.5 s after one that found no way) */
#define NAV_SEARCH_MAX 1     /* route searches per frame, all characters (<= ~1.5 ms of EE each, host x 30 ASSUMED) */
#define FALL_KILL_Y (-250.0f)  /* as the player (collision-4) */

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

/* the loaded type that draws with t's VRAM textures (a same-name twin of another block shares them), or NULL */
static const ChrType *tex_holder(const ChrType *t)
{
    for (int i = 0; i < ntypes; i++)
        if (&types[i] != t && types[i].loaded && !strncmp(types[i].name, t->name, 8))
            return &types[i];
    return NULL;
}

static void unload_model(ChrType *t)
{
    if (!t->model)
        return;
    t->loaded = 0;
    if (!tex_holder(t)) {
        mdl_free_textures(t->model);
        char m[6] = {0};
        snd_chr_unload(memcpy(m, t->name, 5));  /* its sound bank (src/sound.c) */
    }
    mdl_free(t->model);
    t->model = NULL;
}

/* A type's model from its loaded model.bin (mb: size bytes in a buffer of alloc from the arena or the heap, freed
 * here): VU1 streams + compact anims, textures uploaded (or a loaded twin's) */
static int model_from(ChrType *t, GSGLOBAL *gs, void *mb, unsigned size, unsigned alloc, int in_arena)
{
#define FREE_MB() (in_arena ? arena_put(mb, alloc) : free(mb))
    Model *m = mb ? mdl_load(mb) : NULL;
    if (!m) {
        FREE_MB();
        printf("enemy: %s: model.bin did not load\n", t->name);
        return 0;
    }
    const ChrType *twin = tex_holder(t);  /* the same character from another world block's packs: same textures */
    if (twin) {
        mdl_share_textures(m, twin->model);
    } else if (mdl_upload(m, gs) < 0) {
        printf("enemy: %s: textures do not fit in VRAM (free %d)\n", t->name, r_vram_avail(gs));
        mdl_free(m), FREE_MB();
        return 0;
    }
    t->drawable = ((const unsigned *)mb)[3] > 0;  /* model.bin header: tris */
    if (!mdl_compact(m, size)) {
        printf("enemy: %.5s: no RAM to keep the model, RAM free %uK\n", t->name, enemy_ram_free() >> 10);
        if (!twin)
            mdl_free_textures(m);
        mdl_free(m), FREE_MB();
        return 0;
    }
    FREE_MB();
    t->model = m;
    t->loaded = 1;
    char sm[6] = {0};
    snd_chr_load(memcpy(sm, t->name, 5));  /* its sound bank into SPU2 RAM (src/sound.c) */
    return 1;
#undef FREE_MB
}

static void model_path(char *path, unsigned cap, const ChrType *t)
{
    snprintf(path, cap, PACK_ROOT "%s%.8s.pak", t->dir, t->name);
}

/* Load a type's model.bin now (boot: the loading screen) */
static int load_model(ChrType *t, GSGLOBAL *gs)
{
    char path[64];
    model_path(path, sizeof path, t);
    Pack pk;
    if (!pack_open(&pk, path)) {
        printf("enemy: %s: no pack\n", path);
        return 0;
    }
    unsigned size = 0;
    int in_arena = mapstream_ready();
    void *mb = in_arena ? pack_load_with(&pk, "model.bin", &size, arena_get, arena_put) : pack_load(&pk, "model.bin", &size);
    pack_close(&pk);
    return model_from(t, gs, mb, size, (size + 127) & ~127u, in_arena);
}

/* After boot a model.bin loads in the background (pack_job_*), one type at a time: from cdrom0: the synchronous
 * open + load took 0.2-1.3 s per type, each a frame stall (33 loads, 10.5 s of stalls in one tests/world_cross.txt
 * run from the ISO, 2026-11-03) */
static GSGLOBAL *gsg;  /* enemy_init's GS context (tentative definition: set below) */
static PackJob job;
static ChrType *job_t;  /* the type job loads, NULL when idle */
static int job_arena;
static void *heap_get(unsigned n) { return memalign(128, n); }
static void heap_put(void *p, unsigned n) { (void)n; free(p); }

static void job_begin(ChrType *t)
{
    char path[64];
    model_path(path, sizeof path, t);
    job_arena = mapstream_ready();
    pack_job_start(&job, path, "model.bin", job_arena ? arena_get : heap_get, job_arena ? arena_put : heap_put);
    job_t = t;
}

/* 1 when the job finished this frame with t's model loaded, -1 failed, 0 otherwise */
static int job_poll(void)
{
    if (!job_t)
        return 0;
    int r = pack_job_step(&job);
    if (!r)
        return 0;
    ChrType *t = job_t;
    job_t = NULL;
    if (r > 0 && t->near_d > UNLOAD_DIST) {  /* walked away meanwhile */
        job.put(job.out, job.alloc);
        return 0;
    }
    if (r > 0 && model_from(t, gsg, job.out, job.raw, job.alloc, job_arena)) {
        printf("enemy: %.5s loaded, RAM free %uK, VRAM free %d\n", t->name, enemy_ram_free() >> 10, r_vram_avail(gsg));
        return 1;
    }
    t->near_d = 1e9f, t->retry = frame + 60;  /* out of RAM: again in 2 s, not every frame */
    return -1;
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
    if (!h || memcmp(h->magic, "DSRC", 4) || h->version != 6 || !h->variants) {
        printf("enemy: %s: bad chr.bin\n", name);
        free(cb);
        return NULL;
    }
    ChrType *t = &types[ntypes];
    memset(t, 0, sizeof *t);
    memcpy(t->name, name, 8);
    t->dir = dir;
    t->chr_size = csize;
    ntypes++;  /* its model.bin and textures load once a placement comes near (stream_types) */
    t->h = h;
    t->vars = (const ChrVar *)((const char *)cb + h->var_off);
    t->anims = (const ChrAnim *)((const char *)cb + h->anim_off);
    t->wins = (const ChrWin *)((const char *)cb + h->win_off);
    t->root = (const float (*)[3])((const char *)cb + h->root_off);
    t->rag = h->rag_n ? (const RagBody *)((const char *)cb + h->rag_off) : NULL;
    t->nrag = h->rag_n > RAG_MAX ? RAG_MAX : h->rag_n;
    t->walk_speed = loop_speed(t, 200);
    t->run_speed = loop_speed(t, 500);
    t->near_d = 1e9f;
    printf("enemy: %s %u variants, %u anims, RAM free %uK\n", name, h->variants, h->anims, enemy_ram_free() >> 10);
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
    *full = anim;
    if (!e->ct->model)  /* type not loaded: update_one picks the loco anim again once it is */
        return -1;
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

/* EMEVD state of its entity (ev_chr.c). placing: the character was just placed (a block turned on): what events did
 * while it was not there applies now (TECH_DEBT event-12): enable bits, death, a special wait; one-shot ops (turn,
 * replan, anim reset) are not replayed */
static void ev_bind(Enemy *e, EvChr *c, int placing)
{
    e->ev = c;
    if (!c)
        return;
    e->ev_team = placing ? -2 : c->team;
    e->ev_sp = c->sp_serial - (placing && c->sp[0] >= 0);
    e->ev_rot = c->rot_serial, e->ev_replan = c->replan, e->ev_reset = c->anim_reset;
    if (!placing)
        return;
    ev_off[e->index] = c->off;
    if (c->dead)
        e->cc.hp = -1, e->cc.dead = 1, e->dead_t = CORPSE_TIME + 1;  /* killed before: not shown again */
}

/* enemies.bin record r (type t) into slot i of world block `block`; seed_i: its index in the file (AI seed) */
static void rag_release(Enemy *e)
{
    for (int k = 0; k < RAG_POOL; k++)
        if (rag_owner[k] == e)
            rag_owner[k] = NULL, rag_pool[k].on = 0;
    e->rag = NULL;
}

/* combat.c hurtbox test (CombatChr.hit_test): the attack capsule against the ragdoll capsules in the current pose */
static int hit_test(CombatChr *c, const float a[3], const float b[3], float r)
{
    Enemy *e = c->owner;
    const ChrType *t = e->ct;
    if (!t->rag || !t->model)
        return 1;
    static float world[MAX_BONES][12];
    mdl_world(t->model, e->pose, e->pos, e->yaw, world);
    return rag_hit(t->rag, t->nrag, (const float (*)[12])world, a, b, r) >= 0;
}

static void place(ChrType *t, const unsigned char *r, int i, int block, int seed_i)
{
    Enemy *e = &enemies[i];
    rag_release(e);
    memset(e, 0, sizeof *e);
    ev_off[i] = 0;  /* a reused slot inherits no event state */
    e->ct = t;
    e->block = block;
    memcpy(e->rec, r, sizeof e->rec), e->seed_i = seed_i;
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
    if (t->rag) {  /* + margin for poses reaching past the bind pose (ASSUMED 0.5 m / 0.3 m) */
        cc->hit_test = hit_test, cc->owner = e;
        cc->reach = t->h->rag_reach + 0.5f, cc->top = t->h->rag_top + 0.3f;
    }
    if (esd_ok) {
        esd_ctx_init(&e->esd, &esd_prog, ESD_ENEMY, e, enemy_env, enemy_cmd);
        esd_start(&e->esd);
    }
    ev_bind(e, ev_chr_get(e->entity, 0), 1);
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
        const unsigned char *r = b + 8 + ENEMY_REC * i;
        char name[9] = {0};
        memcpy(name, r, 8);
#ifdef ENEMY_ONLY
#define STR_(x) #x
#define STR(x) STR_(x)
        if (!strstr(STR(ENEMY_ONLY), name))  /* test builds: DEFS=-DENEMY_ONLY=c2900_c2540 */
            continue;
#endif
        if (!strcmp(name, "c0000")) {  /* human NPCs: stand-ins (npc.c) */
            npc_keep(0, r);
            continue;
        }
        ChrType *t = load_type(gs, name, CHR_DIR);
        if (!t)
            continue;
        place(t, r, nenemies++, 0, i);
    }
    int ntri = nav_load(0, b, 0);
    /* every type starts unloaded; enemy_update loads the ones near the player */
    printf("enemy: %d characters of %d types, navmesh %d tris (%u B), RAM free %uK after unloading\n", nenemies,
           ntypes, ntri, nav_bytes(), enemy_ram_free() >> 10);
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
    blk_rec[block] = malloc(count * ENEMY_REC + 1);
    blk_n[block] = 0;
    for (unsigned i = 0; blk_rec[block] && i < count && i < MAX_ENEMIES; i++) {
        const unsigned char *r = b + 8 + ENEMY_REC * i;
        char name[9] = {0};
        memcpy(name, r, 8);
        int npc, think;
        memcpy(&npc, r + 8, 4), memcpy(&think, r + 12, 4);
        if (!strcmp(name, "c0000")) {
            npc_keep(block, r);
            continue;
        }
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
        memcpy(blk_rec[block] + ENEMY_REC * blk_n[block]++, r, ENEMY_REC);
    }
#ifndef NO_AI_SCRIPTS
    if (map_lua)
        ai_scripts_add(map_lua, map_lua_size);  /* the block's map logic functions join the one VM */
#endif
    int ntri = nav_load(block, b, 0);  /* kept for the session, as the placements */
    printf("enemy: block %d: %d placements, %d types, navmesh %d tris (%u B all blocks), RAM free %uK\n", block,
           blk_n[block], ntypes, ntri, nav_bytes(), enemy_ram_free() >> 10);
    return blk_n[block];
}

void enemy_block_add(int block)
{
    if (block <= 0 || block >= ENEMY_BLOCKS)
        return;
    int i = 0, n = 0;
    for (int k = 0; k < blk_n[block]; k++) {
        const unsigned char *r = blk_rec[block] + ENEMY_REC * k;
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
    npc_block(block, 1);
    printf("enemy: block %d on: %d characters (%d slots), %d goals in use\n", block, n, nenemies, ai_goals_in_use());
}

/* Bonfire rest / death (src/bonfire.c): every placed character (all blocks that are on) back to its MSB placement with
 * full HP, its AI and ESD from the start, as when its block was placed. Event state stays (ev_bind: disabled ones stay
 * off, ones killed by an event stay dead); the restarted events set the rest again. c0000 NPCs (npc.c) are not
 * revived: DSR keeps a killed NPC dead through its event flags. */
void enemy_respawn(void)
{
    int n = 0;
    for (int i = 0; i < nenemies; i++) {
        Enemy *e = &enemies[i];
        if (ev_off[i] & EV_FREE)
            continue;
        if (e->registered)
            combat_unregister(&e->cc);
        ai_reset(&e->ai);  /* the goal tree back to the pool (place() memsets the slot) */
        unsigned char r[ENEMY_REC];
        memcpy(r, e->rec, sizeof r);
        n += e->cc.dead;
        place(e->ct, r, i, e->block, e->seed_i);
    }
    printf("enemy: respawn: %d characters, %d were dead, %d goals in use\n", nenemies, n, ai_goals_in_use());
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
        rag_release(e);
        e->ev = NULL;
        ev_off[i] = EV_FREE;  /* update / draw skip it (ev_off != 0); stream_types / status by dead_t */
        e->entity = ENT_FREE, e->dead_t = 1e9f, e->pos = v3(1e6f, 0, 1e6f), e->block = -1;
    }
    while (nenemies && (ev_off[nenemies - 1] & EV_FREE))
        nenemies--;
    npc_block(block, 0);
    printf("enemy: block %d off: %d slots, %d goals in use, RAM free %uK\n", block, nenemies, ai_goals_in_use(),
           enemy_ram_free() >> 10);
}

/* Load the types whose placements came within LOAD_DIST (all at once on the first frame, then one at a time in the
 * background), free the ones beyond UNLOAD_DIST */
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
    job_poll();
    for (int i = 0; i < ntypes; i++) {
        ChrType *t = &types[i];
        if (t->loaded && t->near_d > UNLOAD_DIST) {
            unload_model(t);
            printf("enemy: %.5s unloaded, RAM free %uK, VRAM free %d\n", t->name, enemy_ram_free() >> 10,
                   r_vram_avail(gsg));
        } else if (!t->loaded && t->near_d < LOAD_DIST && !first && !job_t && frame >= t->retry) {
            job_begin(t);
        } else if (!t->loaded && t->near_d < LOAD_DIST && first && frame >= t->retry) {
            if (load_model(t, gsg))
                printf("enemy: %.5s loaded, RAM free %uK, VRAM free %d\n", t->name, enemy_ram_free() >> 10,
                       r_vram_avail(gsg));
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

/* A horizontal move over the map collision. walking: the AI's own locomotion, which stops at a ledge (DSR's AI keeps
 * to the navmesh); otherwise (root motion, push-outs) a move off a ledge leaves the character airborne and it falls
 * (fall_step). Returns 1 if it moved. */
static int move_by(Enemy *e, float dx, float dz, int walking)
{
    if (e->ev && e->ev->no_maphit) {  /* 2004[31]: through the map */
        e->pos.x += dx, e->pos.z += dz;
        return 1;
    }
    float p[3] = {e->pos.x, e->pos.y, e->pos.z};
    if (e->air) {  /* in the air: walls only, gravity sets y */
        col_move(p, dx, dz, e->var->radius, e->var->height, COL_STEP_UP, COL_SOLID);
        e->pos.x = p[0], e->pos.z = p[2];
        return 1;
    }
    if (col_walk(p, dx, dz, e->var->radius, e->var->height, COL_STEP_UP, COL_STEP_DOWN, COL_SOLID, 0)) {
        e->pos = v3(p[0], p[1], p[2]);
        return 1;
    }
    if (walking)
        return 0;  /* no floor within a step: stay on this side of the ledge */
    e->pos.x = p[0], e->pos.z = p[2];
    e->air = 1, e->vy = 0, e->fall_y0 = e->pos.y;
    return 1;
}

/* Gravity for an airborne character (as the player: 9.8 m/s^2, at most 50 m/s down, collision-2): lands on the
 * collision floor; a kill plane / lethal fall triangle or y below FALL_KILL_Y kills it; a landing takes the player's
 * fall damage as a share of its HP (ASSUMED for enemies: combat_fall_rate, 5 m .. 20 m) */
static void fall_step(Enemy *e, float dt)
{
    if (!e->air || (e->ev && e->ev->no_maphit))
        return;
    e->vy -= 9.8f * dt;
    if (e->vy < -50)
        e->vy = -50;
    float y = e->pos.y + e->vy * dt;
    ColHit hit;
    if (col_floor(e->pos.x, e->pos.z, e->pos.y + 0.05f, y, COL_SOLID | COL_DEATH, &hit)) {
        e->pos.y = hit.y, e->air = 0, e->vy = 0;
        float rate = (1u << hit.hit_filter) & COL_DEATH ? 1 : combat_fall_rate(e->fall_y0 - hit.y);
        if (rate > 0 && !e->cc.dead) {
            e->cc.hp -= (int)ceilf(rate * e->cc.hp_max);
            if (e->cc.hp <= 0)
                e->cc.hp = -1, e->cc.dead = 1;
        }
        return;
    }
    e->pos.y = y;
    if (y < FALL_KILL_Y && !e->cc.dead)
        e->cc.hp = -1, e->cc.dead = 1;
}

/* The way to dest over the navmesh: the next corner, searched again every NAV_REPLAN s or when dest moved > 1 m;
 * at most NAV_SEARCH_MAX searches a frame (the others keep their corner). 0 = no navmesh here (walk straight). */
static int nav_route(Enemy *e, V3 dest, V3 *wp)
{
    static unsigned searched_frame;
    static int searches;
    if (searched_frame != frame)
        searched_frame = frame, searches = 0;
    float from[3] = {e->pos.x, e->pos.y, e->pos.z}, to[3] = {dest.x, dest.y, dest.z};
    float mx = to[0] - e->nav_dest[0], mz = to[2] - e->nav_dest[2], moved = mx * mx + mz * mz;
    float dx = e->nav_wp[0] - from[0], dz = e->nav_wp[2] - from[2];
    int reached = dx * dx + dz * dz < 0.25f * 0.25f;
    if ((e->nav_t <= 0 || moved > 1 || reached || !e->nav_ok) && searches < NAV_SEARCH_MAX) {
        searches++;
        int r = nav_next(e->block, from, to, e->nav_wp);
        e->nav_ok = r > 0;
        memcpy(e->nav_dest, to, 12);
        e->nav_t = r == 1 ? 1.5f : NAV_REPLAN;
    }
    if (!e->nav_ok)
        return 0;
    *wp = v3(e->nav_wp[0], e->nav_wp[1], e->nav_wp[2]);
    return 1;
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
        int h = combat_sweep(fa, fb, w->radius, &atk);
        if (h)
            ev_chr_player_hit(e->entity);  /* EMEVD 4[01] with the player as target */
        hits_on_player += h;
    }
}

/* EMEVD team types the character fights the player in: enemy, strong enemy, angry ally, decoy, intruder (ASSUMED
 * from the TeamType names) */
static int team_hostile(int team) { return team == 6 || team == 7 || team == 9 || team == 10 || team == 13; }

/* What events changed since the last frame (ev_chr.c) */
static void ev_apply(Enemy *e, int new_hit)
{
    EvChr *v = e->ev;
    if (new_hit)
        v->hit_t = 0;  /* only the player hits enemies */
    if (v->team != e->ev_team) {
        e->ev_team = v->team;
        e->hostile = v->team >= 0 ? team_hostile(v->team) : e->var->team_type != TEAM_NEUTRAL;
    }
    if (v->has_home)
        e->home = v3(v->home[0], v->home[1], v->home[2]);
    if (v->ai_logic > 0)
        e->think.logic = v->ai_logic;
    memcpy(e->ai.ev_req, v->ai_req, sizeof e->ai.ev_req);
    if (v->replan != e->ev_replan)
        e->ev_replan = v->replan, ai_replan(&e->ai, 0);
    if (v->rot_serial != e->ev_rot) {
        e->ev_rot = v->rot_serial;
        e->yaw = atan2f(v->rot_to[0] - e->pos.x, v->rot_to[2] - e->pos.z);  /* ASSUMED instant */
    }
    if (v->anim_reset != e->ev_reset) {
        e->ev_reset = v->anim_reset;
        e->act = -1, e->act_info = 0, e->sp_on = 0;
    }
    if (v->sp_serial != e->ev_sp) {
        e->ev_sp = v->sp_serial;
        if (v->sp[0] >= 0)
            start_action(e, v->sp[0], 0, 0.2f, 1), e->sp_on = 1;
        else if (e->sp_on) {  /* wait over: its cancel anim (the skeletons' 9061 getting up), or back to idle */
            e->sp_on = 0;
            if (v->sp[2] >= 0)
                start_action(e, v->sp[2], 0, 0.2f, 0);
            else
                e->act = -1, e->act_info = 0;
        }
    }
    if (e->sp_on && new_hit) {  /* a hit ends the wait (its damage anim if it has one; ASSUMED) */
        e->sp_on = 0;
        if (v->sp[1] >= 0)
            start_action(e, v->sp[1], 0, 0.1f, 0);
    }
}

static int is_death_anim(int id) { return (id % 10000) >= 2200 && (id % 10000) < 2300; }

/* A ragdoll slot: a free one, else one at rest, else the oldest; its last owner keeps the pose it has */
static Ragdoll *rag_take(Enemy *e)
{
    int k = -1;
    for (int i = 0; i < RAG_POOL && k < 0; i++)
        if (!rag_owner[i])
            k = i;
    for (int i = 0; i < RAG_POOL && k < 0; i++)
        if (rag_pool[i].asleep)
            k = i;
    for (int i = 0; i < RAG_POOL; i++)
        if (k < 0 || (!rag_pool[k].asleep && rag_pool[i].age > rag_pool[k].age))
            k = i;
    if (rag_owner[k])
        rag_owner[k]->rag = NULL;
    rag_owner[k] = e;
    return &rag_pool[k];
}

/* The death ragdoll. Starts at the death anim's TAE flag 12 window (or the anim's end), at once when the character
 * dies in the air, or 2.5 s after death when no death anim plays (killed mid-action); from then on the bodies drive
 * the pose. Returns 1 once started. */
static int dead_ragdoll(Enemy *e, float dt)
{
    ChrType *t = e->ct;
    if (!t->rag || !t->model || !e->cc.dead)
        return 0;
    if (!e->rag_done) {
        int death = e->act >= 0 && is_death_anim(e->act);
        if (!(e->air || (death && (in_window(e, F_RAGDOLL) || act_ended(e))) || (!death && e->dead_t > 2.5f)))
            return 0;
        static float world[MAX_BONES][12], prev[MAX_BONES][12], bind[MAX_BONES][12];
        static BoneLocal tmp[MAX_BONES];
        mdl_world(t->model, e->pose, e->pos, e->yaw, world);
        if (e->act >= 0 && e->act_slot >= 0 && e->act_t > dt) {
            mdl_sample(t->model, e->act_slot, e->act_t - dt, e->act_loop, tmp);
            mdl_world(t->model, tmp, e->pos, e->yaw, prev);
        } else
            memcpy(prev, world, sizeof world);
        mdl_sample(t->model, -1, 0, 0, tmp);
        mdl_world(t->model, tmp, e->pos, e->yaw, bind);
        e->rag = rag_take(e);
        rag_start(e->rag, t->rag, t->nrag, (const float (*)[12])bind, (const float (*)[12])world,
                  (const float (*)[12])prev, dt);
        if (e->air)  /* falling: keep the fall speed */
            for (int k = 0; k < e->rag->n; k++)
                e->rag->v[k][1] += e->vy;
        printf("enemy: %.5s %d ragdoll (%s, anim %d at %.2f s), %d bodies\n", t->name, e->entity,
               e->air ? "in the air" : !death ? "no death anim" : in_window(e, F_RAGDOLL) ? "flag 12" : "anim end",
               e->act, e->act_t, e->rag->n);
        e->rag_done = 1, e->air = 0;
    }
    if (e->rag && !e->rag->asleep) {
        rag_step(e->rag, dt);
        rag_pose(e->rag, t->model, e->pose, e->pos, e->yaw, e->pose);
        if (e->rag->asleep)
            printf("enemy: %.5s %d ragdoll at rest after %.1f s\n", t->name, e->entity, e->rag->age);
    }
    return 1;
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
        if (e->dead_t == 0) {
            ai_reset(&e->ai);
            /* FUN_14032f900, once per death: souls (NpcParam getSoul) and humanity points (FUN_14032fb30) */
            if (!e->no_award)
                progress_enemy_killed(e->rec[36] | e->rec[37] << 8, e->rec[38], e->rec[39]);
        }
        e->dead_t += dt;
    }

    if (e->ev)
        ev_apply(e, new_hit);

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
    if (!e->cc.dead && !e->sp_on && !(e->ev && e->ev->ai_off))
        ai_update(&e->ai, f);
    else if (!e->cc.dead)
        e->ai.out = (AiOut){.req = -1, .guard = -1};  /* logic off / special wait: stands still */
    e->req = e->cc.dead ? -1 : o->req;
    /* a destination: the way there over the navmesh replaces the straight line */
    if (e->nav_t > 0)
        e->nav_t -= dt;
    if (o->move && o->dest && !e->cc.dead) {
        V3 wp;
        if (nav_route(e, o->dest_pos, &wp) && fabsf(wp.x - e->pos.x) + fabsf(wp.z - e->pos.z) > 0.05f) {
            float yaw = atan2f(wp.x - e->pos.x, wp.z - e->pos.z);
            if (o->face && fabsf(wrap(o->face_yaw - o->move_yaw)) < 1e-3f)
                e->ai.out.face_yaw = yaw;  /* it was facing along its move */
            e->ai.out.move_yaw = yaw;
        }
    }
    if (esd_ok) {
        e->esd.dt = dt;
        esd_update(&e->esd);
    }
    if (e->cc.dead && dead_ragdoll(e, dt)) {  /* the ragdoll owns the pose now */
        memcpy(e->cc.pos, &e->pos, 12);
        e->blend = 1;
        return;
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
            move_by(e, c * dx + s * dz, -s * dx + c * dz, 0);
            e->yaw = wrap(e->yaw - (r1[2] - r0[2]));
        }
    }
    int moving = !e->cc.dead && !e->air && (e->act < 0 || act_ended(e)) && e->loco_enabled && o->move;
    int loco = 0;
    if (moving) {
        float rel = wrap(o->move_yaw - e->yaw), speed = o->run ? t->run_speed : t->walk_speed;
        loco = o->run ? 500 : fabsf(rel) < PI / 4 ? 200 : fabsf(rel) > 3 * PI / 4 ? 201 : rel > 0 ? 202 : 203;
        if (!move_by(e, sinf(o->move_yaw) * speed * dt, cosf(o->move_yaw) * speed * dt, 1))
            moving = 0, loco = 0, e->nav_t = 0;  /* at a ledge: stands (and searches the route again) */
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
    {  /* TAE sound events of the action anim, else the locomotion loop (src/sound.c) */
        int acting = e->act >= 0 && !act_ended(e), la_ok = e->loco_anim >= 0 && t->model;
        float len = la_ok ? mdl_anim_length(t->model, e->loco_anim) : 0;
        snd_anim(SND_OWNER_ENEMY(e->index), atoi(e->ct->name + 1), acting ? e->act : la_ok ? mdl_anim_id(t->model, e->loco_anim) : -1,
                 acting ? e->act_t : len > 0 ? fmodf(e->loco_t, len) : e->loco_t, (const float *)&e->pos);
    }
    fall_step(e, dt);
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
        e->cc.invincible = in_window(e, F_INVINCIBLE) || (e->ev && e->ev->invincible);
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
    e->cc.invincible = in_window(e, F_INVINCIBLE) || (e->ev && e->ev->invincible);
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
    move_by(e, dx * k, dz * k, 1);
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

/* Within ACTIVE_DIST of the player, or always updated by an event (2004[35]) */
static int near(const Enemy *e, V3 d)
{
    return d.x * d.x + d.z * d.z <= ACTIVE_DIST * ACTIVE_DIST || (e->ev && e->ev->always_on);
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
    for (int i = 0; i < nenemies; i++) {  /* 2004[12] immortal: HP stays >= 1 (an event's kill still kills) */
        Enemy *e = &enemies[i];
        if (e->ev && e->ev->immortal && !e->ev->dead && e->cc.hp < 1 && e->dead_t == 0)
            e->cc.hp = 1, e->cc.dead = 0;
    }
    npc_update(pl, dt);
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
        nc += enemies[i].ct->loaded && near(&enemies[i], d) &&
              enemies[i].dead_t <= CORPSE_TIME && !ev_off[i] && dg_mask_shown(enemies[i].block, enemies[i].dg);
    }
    if (nc > ACTIVE_MAX) {  /* the ACTIVE_MAX-th smallest distance of the candidates */
        float c[MAX_ENEMIES];
        int m = 0;
        for (int i = 0; i < nenemies; i++) {
            V3 d = v3sub(enemies[i].pos, pl->pos);
            if (enemies[i].ct->loaded && near(&enemies[i], d) &&
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
    /* the player walking into ragdolls kicks them (its speed from the last frame) */
    static V3 last_pos;
    float pvel[3] = {0, 0, 0}, feet[3] = {pl->pos.x, pl->pos.y, pl->pos.z};
    if (dt > 0)
        pvel[0] = (pl->pos.x - last_pos.x) / dt, pvel[2] = (pl->pos.z - last_pos.z) / dt;
    if (pvel[0] * pvel[0] + pvel[2] * pvel[2] > 100)
        pvel[0] = pvel[2] = 0;  /* a warp, not a walk */
    last_pos = pl->pos;
    for (int k = 0; k < RAG_POOL; k++)
        if (rag_owner[k])
            rag_push(&rag_pool[k], feet, COL_PLAYER_R, COL_PLAYER_H, pvel);
    for (int i = 0; i < nenemies; i++) {
        Enemy *e = &enemies[i];
        V3 d = v3sub(e->pos, pl->pos);
        int on = e->ct->loaded && near(e, d) && e->dead_t <= CORPSE_TIME &&
                 !ev_off[i] && dg_mask_shown(e->block, e->dg) && d3[i] <= lim;
        if (!on && e->rag)
            rag_release(e);  /* gone or frozen: the slot goes back */
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
        unsigned show = e->var->show_bits;
        if (e->ev)  /* 2004[26] display mask */
            show = (show | e->ev->disp_on) & ~e->ev->disp_off;
        mdl_hide(e->ct->model, e->ct->h->hide_bits & ~show);  /* #NN# meshes of other variants */
        /* fade into the fog over the last DRAW_FADE m instead of popping at DRAW_DIST (render-distance; ASSUMED) */
        float fd = (DRAW_DIST - sqrtf(d.x * d.x + d.z * d.z)) / DRAW_FADE;
        vu1_skin_fade(fd < 1 ? 255 * fd : 255);
        mdl_draw(e->ct->model, e->pose, e->pos, e->yaw, 0);
        enemy_drawn++;
    }
    npc_draw();
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
    int k = npc_entity(entity, pos, dead, hp_ratio, active);  /* a c0000 stand-in */
    return k < 0 ? -1 : MAX_ENEMIES + k;
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
    return n + npc_entity_enable(entity, on, bit);
}

int enemy_entity_kill(int entity, int award)
{
    for (int i = 0; i < nenemies; i++)
        if (enemies[i].entity == entity && !enemies[i].cc.dead) {
            enemies[i].cc.hp = -1, enemies[i].cc.dead = 1;  /* below 0: the ESD death test f1000() < 0 */
            enemies[i].no_award = !award;
            return 1;
        }
    return npc_entity_kill(entity);
}

int enemy_entity_damage(int entity, int hp)
{
    ev_chr_hit(entity);
    for (int i = 0; i < nenemies; i++) {
        Enemy *e = &enemies[i];
        if (e->entity != entity || e->cc.dead || (e->ev && e->ev->invincible))
            continue;
        e->cc.hp -= hp, e->cc.hit_count++;  /* update_one sees a hit (hostile, damage reaction) */
        if (e->cc.hp <= 0)
            e->cc.dead = 1;
        if (e->ev && e->ev->immortal && e->cc.hp < 1)
            e->cc.hp = 1, e->cc.dead = 0;
        return 1;
    }
    return npc_entity_damage(entity, hp);
}

void enemy_entity_bind(int entity, EvChr *c)
{
    for (int i = 0; i < nenemies; i++)
        if (enemies[i].entity == entity && !(ev_off[i] & EV_FREE))
            ev_bind(&enemies[i], c, 0);
    npc_entity_bind(entity, c);
}
