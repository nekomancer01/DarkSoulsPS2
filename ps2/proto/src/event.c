/* EMEVD runtime (see event.h; bytecode layout in tools/export_emevd.py). Semantics from soulstruct's DS1 event docs
 * (condition groups, RestartType, FlagType, RangeState, MultiplayerState enums) and the decoded scripts. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tamtypes.h>

#include "bonfire.h"
#include "collision.h"
#include "drawgrp.h"
#include "enemy.h"
#include "ev_chr.h"
#include "ev_misc.h"
#include "ev_obj.h"
#include "event.h"
#include "flags.h"
#include "hud.h"
#include "fx.h"
#include "objects.h"
#include "progress.h"
#include "save.h"
#include "world.h"

/* Same order as OPS in tools/export_emevd.py (the op byte of each instruction) */
enum Op {
    OP_UNSUPPORTED, OP_C_GROUP, OP_C_ELAPSED_S, OP_C_ELAPSED_F, OP_C_FLAG, OP_C_FLAG_RANGE, OP_C_AREA, OP_C_RADIUS,
    OP_C_ITEM, OP_C_ACTION, OP_C_MP, OP_C_ALL_AREA, OP_C_IN_MAP, OP_C_MP_EVENT, OP_C_COUNT_FLAGS, OP_C_TENDENCY,
    OP_C_EVENT_VALUE, OP_C_DROPPED_AREA, OP_C_DROPPED, OP_C_ITEM_BBOX, OP_C_CYCLE, OP_C_ACTION_LINE,
    OP_C_ACTION_BOSS, OP_C_ONLINE, OP_C_MP_COUNT, OP_C_DEAD, OP_C_DAMAGED, OP_C_HP, OP_C_CHR_TYPE, OP_C_SPEFFECT,
    OP_C_PART_HP, OP_C_BACKREAD, OP_C_EVENT_MSG, OP_C_CLASS, OP_C_COVENANT, OP_C_OBJ_DESTROYED, OP_C_OBJACT,
    OP_C_ON_HIT, OP_SKIP_COND_U, OP_END_COND_U, OP_SKIP, OP_END, OP_SKIP_CMP, OP_END_CMP, OP_SKIP_COND_C,
    OP_END_COND_C, OP_WAIT_NET, OP_WAIT_S, OP_WAIT_F, OP_WAIT_RAND, OP_SKIP_FLAG, OP_END_FLAG, OP_SKIP_FLAG_RANGE,
    OP_SKIP_MP, OP_END_MP, OP_SKIP_IN_MAP, OP_INIT, OP_NET_SYNC, OP_PREFETCH, OP_SAVE_REQ, OP_CUTSCENE,
    OP_CUTSCENE_WARP, OP_SET_FLAG, OP_EVENT_STATE, OP_BOSS_BAR, OP_BOSS_DEFEAT, OP_NAVMESH, OP_MP_TRIGGER,
    OP_RAND_FLAG, OP_ANIM, OP_BATCH_FLAGS, OP_RESPAWN, OP_REMOVE_ITEM, OP_SUMMON_SIGN, OP_MSG_VIS, OP_ACHIEVE,
    OP_VAGRANT, OP_EV_INC, OP_EV_CLEAR, OP_SNUGGLY_NEXT, OP_SNUGGLY_SPAWN, OP_AWARD_LOT, OP_UNK_2003_47,
    OP_ERASE_SIGN, OP_AI_STATE, OP_TEAM, OP_KILL, OP_CHR_ENABLE, OP_EZSTATE, OP_SPEFFECT, OP_SPECIAL_ANIM,
    OP_GRAVITY, OP_IMMORTAL, OP_HOME, OP_ROTATE, OP_INVINCIBLE, OP_AI_TARGET, OP_AI_CMD, OP_AI_ID, OP_AI_REPLAN,
    OP_CLEAR_SPEFFECT, OP_NPC_PART, OP_DISPMASK, OP_HITMASK, OP_NET_AUTH, OP_BACKREAD, OP_HPBAR, OP_MAPHITS,
    OP_NET_RATE, OP_DEF_BACKREAD, OP_HELLKITE, OP_FORCE_TREASURE, OP_WARP_FLOOR, OP_SHORT_WARP, OP_WARP_COPY,
    OP_ANIM_RESET, OP_TEAM_EXIT, OP_HUMANITY, OP_OBJ_DESTROY, OP_OBJ_RESTORE, OP_OBJ_ENABLE, OP_TREASURE,
    OP_OBJACT_INIT, OP_OBJACT_STATE, OP_OBJ_ANIM, OP_OBJ_REDESTROY, OP_DAMAGE_OBJ, OP_OBJ_INVULN, OP_OBJACT_IDX,
    OP_REROLL, OP_SFX_DEL, OP_SFX_SPAWN, OP_SFX_ONESHOT, OP_SFX_OBJ, OP_SFX_OBJ_DEL, OP_DIALOG, OP_STATUS_MSG,
    OP_LADDER, OP_BONFIRE, OP_MP_BUFF, OP_BOSS_ENTRY, OP_SE, OP_SOUND_STATE, OP_HIT, OP_MAP_PART, OP_BANNER,
    OP_COUNT
};
static const char *const op_names[OP_COUNT] = {
    "UNSUPPORTED", "C_GROUP", "C_ELAPSED_S", "C_ELAPSED_F", "C_FLAG", "C_FLAG_RANGE", "C_AREA", "C_RADIUS", "C_ITEM",
    "C_ACTION", "C_MP", "C_ALL_AREA", "C_IN_MAP", "C_MP_EVENT", "C_COUNT_FLAGS", "C_TENDENCY", "C_EVENT_VALUE",
    "C_DROPPED_AREA", "C_DROPPED", "C_ITEM_BBOX", "C_CYCLE", "C_ACTION_LINE", "C_ACTION_BOSS", "C_ONLINE",
    "C_MP_COUNT", "C_DEAD", "C_DAMAGED", "C_HP", "C_CHR_TYPE", "C_SPEFFECT", "C_PART_HP", "C_BACKREAD",
    "C_EVENT_MSG", "C_CLASS", "C_COVENANT", "C_OBJ_DESTROYED", "C_OBJACT", "C_ON_HIT", "SKIP_COND_U", "END_COND_U",
    "SKIP", "END", "SKIP_CMP", "END_CMP", "SKIP_COND_C", "END_COND_C", "WAIT_NET", "WAIT_S", "WAIT_F", "WAIT_RAND",
    "SKIP_FLAG", "END_FLAG", "SKIP_FLAG_RANGE", "SKIP_MP", "END_MP", "SKIP_IN_MAP", "INIT", "NET_SYNC", "PREFETCH",
    "SAVE_REQ", "CUTSCENE", "CUTSCENE_WARP", "SET_FLAG", "EVENT_STATE", "BOSS_BAR", "BOSS_DEFEAT", "NAVMESH",
    "MP_TRIGGER", "RAND_FLAG", "ANIM", "BATCH_FLAGS", "RESPAWN", "REMOVE_ITEM", "SUMMON_SIGN", "MSG_VIS", "ACHIEVE",
    "VAGRANT", "EV_INC", "EV_CLEAR", "SNUGGLY_NEXT", "SNUGGLY_SPAWN", "AWARD_LOT", "UNK_2003_47", "ERASE_SIGN",
    "AI_STATE", "TEAM", "KILL", "CHR_ENABLE", "EZSTATE", "SPEFFECT", "SPECIAL_ANIM", "GRAVITY", "IMMORTAL", "HOME",
    "ROTATE", "INVINCIBLE", "AI_TARGET", "AI_CMD", "AI_ID", "AI_REPLAN", "CLEAR_SPEFFECT", "NPC_PART", "DISPMASK",
    "HITMASK", "NET_AUTH", "BACKREAD", "HPBAR", "MAPHITS", "NET_RATE", "DEF_BACKREAD", "HELLKITE", "FORCE_TREASURE",
    "WARP_FLOOR", "SHORT_WARP", "WARP_COPY", "ANIM_RESET", "TEAM_EXIT", "HUMANITY", "OBJ_DESTROY", "OBJ_RESTORE",
    "OBJ_ENABLE", "TREASURE", "OBJACT_INIT", "OBJACT_STATE", "OBJ_ANIM", "OBJ_REDESTROY", "DAMAGE_OBJ",
    "OBJ_INVULN", "OBJACT_IDX", "REROLL", "SFX_DEL", "SFX_SPAWN", "SFX_ONESHOT", "SFX_OBJ", "SFX_OBJ_DEL", "DIALOG",
    "STATUS_MSG", "LADDER", "BONFIRE", "MP_BUFF", "BOSS_ENTRY", "SE", "SOUND_STATE", "HIT", "MAP_PART", "BANNER",
};
/* ops that do nothing in DSR offline single player either (network, achievements, multiplayer signs) */
static int op_noop(int op)
{
    return op == OP_NET_SYNC || op == OP_PREFETCH || op == OP_MP_TRIGGER || op == OP_SUMMON_SIGN ||
           op == OP_ACHIEVE || op == OP_VAGRANT || op == OP_ERASE_SIGN || op == OP_NET_AUTH || op == OP_NET_RATE ||
           op == OP_MP_BUFF || op == OP_BOSS_ENTRY || op == OP_WAIT_NET;
}

/* ---- file layout (tools/export_emevd.py) ---- */
typedef struct { char magic[4]; u32 version; u8 map[4]; u32 flags; u32 sec[9][2]; } EvHdr;
enum { S_EVENTS, S_INSTRS, S_SUBSTS, S_ENTS, S_RANGES, S_COLS, S_TREASURES, S_OBJACTS, S_LOTS };
typedef struct { u32 id; u8 rest, pad; u16 ninstr; u32 ioff, sfirst, scount; } EvEvent;
typedef struct { u8 op, nw; u16 bank, id, layer; } EvInstr;
typedef struct { u16 instr, dst, src; u8 fmt, n; } EvSubst;
typedef struct { s32 id; u8 kind, shape; u16 index; float pos[3], yaw, dim[3]; u32 count, pad[2]; } EvEnt;
typedef struct { u32 first, count; } EvRange;
typedef struct { float c[3], half[3], yaw; s32 entity; } EvCol;
typedef struct { s32 entity; float pos[3]; s32 lot[5]; u8 in_chest, start_disabled; u16 pad; } EvTreasure;
typedef struct {
    s32 entity, part;
    float pos[3], yaw, dist;
    s32 param, flag;
    u8 state, pad;
    s16 anim;            /* ObjActParam objAnimId: the object's anim on use (-1 none) */
    u8 sp_type, sp_type2;  /* key: spQualifiedType (1 = a good in the inventory), spQualifiedType2 */
    u16 pad2;
    s32 sp_id, sp_id2, pass_flag;  /* the goods; spQualifiedPassEventFlag opens it without them */
} EvObjAct;
typedef struct { s32 id, flag; u16 first; u8 n, pad; } EvLot;  /* ItemLotParam row: getItemFlagId, its weighted items */
typedef struct { s32 item, flag; u16 point; u8 num, cat; } EvLotItem;  /* cat = lotItemCategory >> 28, 15 = none */
enum { ENT_OBJECT = 2, ENT_ENEMY = 3, ENT_COLLISION = 5, ENT_REGION = 10 };
enum { SH_POINT, SH_CIRCLE, SH_SPHERE, SH_CYLINDER, SH_RECT, SH_BOX };
#define PLAYER_ENTITY 10000

typedef struct {
    const u8 *blob;
    const EvHdr *h;
    const EvEvent *ev;
    const EvEnt *ent;
    int nev, nent, block, live;  /* live: this set runs the map's events (another block of the same map: entities only) */
    u8 *ent_off;                 /* per entity: 1 = disabled by an event (objects, map parts, SFX, hits) */
    u8 *tr_state;                /* per treasure: bit 0 on, bit 1 taken */
    u8 *oa_state;                /* per objact: bit 0 usable, bit 1 used this frame, bit 2 its object shows the state
                                  * flag's end pose (objact_flags) */
    u16 *ev_live;                /* per event: instances alive (InitializeEvent's duplicate check scans only then) */
} Set;
static Set sets[EV_SETS];

#define SEC(s, k, T) ((const T *)((s)->blob + (s)->h->sec[k][0]))
#define NSEC(s, k) ((int)(s)->h->sec[k][1])

/* ---- instances ---- */
#define GROUPS 15  /* -7..7 */
enum { I_FREE, I_RUN, I_MAIN, I_WAIT_T, I_WAIT_F, I_DONE, I_STOPPED };
typedef struct { u16 off, idx; s8 group; u8 pad; } Reg;
typedef struct {
    u8 state, set, nreg, creg;  /* conditions registered / allocated (reg grows: common event 719 has 74) */
    u16 ev, pc;
    u32 pcoff;          /* byte offset of instruction pc in the set's instrs */
    s32 slot;
    float wait, cond_t; /* wait left (s or frames); time since the current conditions were registered */
    u32 cond_f;
    u16 cvalid, cval;   /* compiled group results (bit g + 7) from the last MAIN */
    u8 argn, flag_only;  /* MAIN waits only on flags / inventory / constants: re-evaluated when one of those changed */
    u8 warped;           /* warped the player to another map: finishes even if its block unloads (event_block_off) */
    u32 seen;            /* state_key() at the last evaluation */
    Reg *reg;
    u8 *args;
} Inst;
static Inst *inst;
static int ninst;  /* high-water mark */
static int free_hint;  /* no free slot below this */
#define EV_BUDGET_US 2000  /* event_update time per frame before the rest waits for the next (33.3 ms frames) */

EventStats event_stats;
static Player *pl;
static float dtime;
static int act_press, act_used, trace = 1;
static u8 stub_seen[(OP_COUNT + 7) / 8];
static int test_pass, test_fail;
static u32 rng = 0x2545F491;

/* EMEVD ItemType (0 weapon, 1 armor, 2 ring, 3 good) -> progress.h item id */
static u32 item_id(int type, s32 row)
{
    static const u32 cat[4] = {ITEM_WEAPON, ITEM_PROTECTOR, ITEM_ACCESSORY, ITEM_GOODS};
    return cat[type & 3] | ((u32)row & 0x0FFFFFFFu);
}

static u32 rnd(void)
{
    rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
    return rng;
}

/* ---- entities ---- */
static const EvEnt *find_ent(const Set *s, s32 id)
{
    int lo = 0, hi = s->nent - 1;
    while (lo <= hi) {  /* sorted by id; several records may share one (first wins) */
        int m = (lo + hi) / 2;
        if (s->ent[m].id < id)
            lo = m + 1;
        else if (s->ent[m].id > id)
            hi = m - 1;
        else {
            while (m > 0 && s->ent[m - 1].id == id)
                m--;
            return &s->ent[m];
        }
    }
    return NULL;
}

/* Entity anywhere (the instance's own set first); *sp = its set */
static const EvEnt *ent_any(int own, s32 id, Set **sp)
{
    for (int k = -1; k < EV_SETS; k++) {
        Set *s = &sets[k < 0 ? own : k];
        if (!s->blob || (k >= 0 && k == own))
            continue;
        const EvEnt *e = find_ent(s, id);
        if (e) {
            if (sp)
                *sp = s;
            return e;
        }
    }
    return NULL;
}

static int ent_pos(int own, s32 id, float out[3])
{
    if (id == PLAYER_ENTITY) {
        memcpy(out, &pl->pos, 12);
        return 1;
    }
    V3 p;
    if (enemy_entity(id, &p, NULL, NULL, NULL) >= 0) {
        memcpy(out, &p, 12);
        return 1;
    }
    const EvEnt *e = ent_any(own, id, NULL);
    if (!e)
        return 0;
    memcpy(out, e->pos, 12);
    return 1;
}

int event_entity(s32 id, float pos[3], float *yaw)
{
    const EvEnt *e = ent_any(0, id, NULL);
    if (!e)
        return 0;
    memcpy(pos, e->pos, 12);
    *yaw = e->yaw;
    return 1;
}

/* MSB region shapes: box / cylinder from pos.y up by height, tested against the character's capsule (feet .. feet +
 * COL_PLAYER_H: the Burg fog-wall regions sit 0.2 m above the hkx floor); yaw turns local x/z into world as in the
 * exporter's colliders (ASSUMED convention, TECH_DEBT event-3) */
static int in_region(const EvEnt *r, const float p[3])
{
    const float ch = COL_PLAYER_H;
    float dx = p[0] - r->pos[0], dy = p[1] - r->pos[1], dz = p[2] - r->pos[2];
    float c = cosf(r->yaw), s = sinf(r->yaw);
    float lx = dx * c - dz * s, lz = dx * s + dz * c;
    switch (r->shape) {
    case SH_SPHERE: return dx * dx + dy * dy + dz * dz <= r->dim[0] * r->dim[0];
    case SH_CIRCLE: return dx * dx + dz * dz <= r->dim[0] * r->dim[0];
    case SH_CYLINDER: return dx * dx + dz * dz <= r->dim[0] * r->dim[0] && dy + ch >= 0 && dy <= r->dim[1];
    case SH_RECT: return fabsf(lx) <= r->dim[0] * 0.5f && fabsf(lz) <= r->dim[1] * 0.5f;
    case SH_BOX:
        return fabsf(lx) <= r->dim[0] * 0.5f && fabsf(lz) <= r->dim[1] * 0.5f && dy + ch >= 0 && dy <= r->dim[2];
    }
    return 0;  /* points have no inside */
}

static int ent_on(const Set *s, const EvEnt *e) { return !s->ent_off[e - s->ent]; }

/* ---- map of the player ---- */
static Set *player_set(void)
{
    int b = world_current();
    if (b < 0)
        b = 0;
    for (int k = 1; k < EV_SETS; k++)
        if (sets[k].blob && sets[k].block == b)
            return &sets[k];
    return NULL;
}

/* ---- instruction access ---- */
static const EvInstr *instr_at(const Set *s, u32 off) { return (const EvInstr *)(s->blob + s->h->sec[S_INSTRS][0] + off); }
static u32 instr_next(const Set *s, u32 off) { return off + 8 + 4 * instr_at(s, off)->nw; }

/* Words of instruction idx at off with the instance's substitutions applied (into w, 64 words max) */
static const EvInstr *load_instr(const Inst *in, u32 off, int idx, u32 *w)
{
    const Set *s = &sets[in->set];
    const EvInstr *ins = instr_at(s, off);
    const EvEvent *ev = &s->ev[in->ev];
    int nw = ins->nw > 64 ? 64 : ins->nw;
    memcpy(w, ins + 1, 4 * nw);
    const EvSubst *su = SEC(s, S_SUBSTS, EvSubst) + ev->sfirst;
    for (u32 k = 0; k < ev->scount; k++) {
        if (su[k].instr != idx || su[k].dst + 4 > 4 * nw + 4)
            continue;
        const u8 *a = in->args + su[k].src;
        if (su[k].src + su[k].n > in->argn)
            continue;  /* arg past the instance's blob: keep the literal */
        u8 *d = (u8 *)w + su[k].dst;
        if (su[k].fmt == 7) {
            if (su[k].dst + su[k].n <= 4 * nw)
                memcpy(d, a, su[k].n);
            continue;
        }
        s32 v = 0;
        switch (su[k].fmt) {  /* EDF types 0 u8, 1 u16, 2 u32, 3 s8, 4 s16, 5 s32, 6 f32 */
        case 0: v = a[0]; break;
        case 1: v = a[0] | a[1] << 8; break;
        case 3: v = (s8)a[0]; break;
        case 4: v = (s16)(a[0] | a[1] << 8); break;
        default: memcpy(&v, a, 4); break;
        }
        memcpy(d, &v, 4);
    }
    return ins;
}

static float wf(u32 w) { float f; memcpy(&f, &w, 4); return f; }
static int cmp(int type, s32 a, s32 b)
{
    switch (type) {  /* ComparisonType */
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a > b;
    case 3: return a < b;
    case 4: return a >= b;
    case 5: return a <= b;
    }
    return 0;
}
static int cmpf(int type, float a, float b)
{
    switch (type) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a > b;
    case 3: return a < b;
    case 4: return a >= b;
    case 5: return a <= b;
    }
    return 0;
}

static u32 ev_id(const Inst *in) { return sets[in->set].ev[in->ev].id; }
/* FlagType: 0 absolute, 1 this event's flag (+ id), 2 this event slot's flag */
static u32 flag_of(const Inst *in, int type, s32 id)
{
    return type == 1 ? ev_id(in) + id : type == 2 ? ev_id(in) + in->slot + id : (u32)id;
}
static int range_state(int state, u32 a, u32 b)
{
    if (b < a) { u32 t = a; a = b; b = t; }
    int on = flag_range_count(a, b), n = b - a + 1;
    switch (state) {  /* RangeState */
    case 0: return on == n;
    case 1: return on == 0;
    case 2: return on > 0;
    case 3: return on < n;
    }
    return 0;
}
/* MultiplayerState: offline single player is the host and single player */
static int mp_state(int s) { return s == 0 || s == 3; }

static void stub(const Inst *in, int op)
{
    event_stats.stubs++;
    if (!(stub_seen[op >> 3] >> (op & 7) & 1) || trace > 1) {
        stub_seen[op >> 3] |= 1 << (op & 7);
        printf("EVT %u/%d stub %s\n", (unsigned)ev_id(in), (int)in->slot, op_names[op]);
    }
}

/* ---- action button targets ---- */
static int action_reach(const Inst *in, const u32 *w, int line)
{
    /* args: group, entity type (0 object, 1 region, 2 character), entity, angle, dummy, distance, msg, type, pad[,
     * line segment entity] */
    float p[3];
    s32 id = (s32)w[2];
    float dist = wf(w[5]), ang = wf(w[3]);
    Set *s = NULL;
    const EvEnt *e = ent_any(in->set, id, &s);
    if (w[1] == 1 && e && e->kind == ENT_REGION && (dist <= 0 || line)) {
        if (!in_region(e, (const float[3]){pl->pos.x, pl->pos.y, pl->pos.z}))
            return 0;
    } else {
        if (!ent_pos(in->set, id, p))
            return 0;
        float dx = p[0] - pl->pos.x, dz = p[2] - pl->pos.z, dy = p[1] - pl->pos.y;
        if (dx * dx + dz * dz > dist * dist || fabsf(dy) > 2.0f)  /* ASSUMED +-2 m height window (event-4) */
            return 0;
        if (ang > 0 && ang < 180 && dx * dx + dz * dz > 0.01f) {  /* player facing the target within the angle */
            float d = atan2f(dx, dz) - pl->yaw;
            while (d > 3.14159265f) d -= 6.2831853f;
            while (d < -3.14159265f) d += 6.2831853f;
            if (fabsf(d) > ang * 3.14159265f / 180)
                return 0;
        }
    }
    event_stats.prompt = (s32)w[6];
    return 1;
}

static int eval_group(Inst *in, int g, int depth);

/* Conditions whose value changes only with the event flags or the inventory (or never, in the port today): an
 * instance waiting on nothing else sleeps until state_key() moves */
static int static_cond(int op)
{
    switch (op) {
    case OP_C_GROUP: case OP_C_FLAG: case OP_C_FLAG_RANGE: case OP_C_COUNT_FLAGS: case OP_C_EVENT_VALUE: case OP_C_MP:
    case OP_C_ONLINE: case OP_C_CYCLE: case OP_C_CLASS: case OP_C_COVENANT: case OP_C_ITEM: case OP_C_ITEM_BBOX:
    case OP_C_TENDENCY: case OP_C_MP_EVENT: case OP_C_MP_COUNT: case OP_C_DROPPED_AREA: case OP_C_DROPPED:
    case OP_C_EVENT_MSG: case OP_C_CHR_TYPE:
        return 1;
    }
    return 0;
}
static u32 state_key(void) { return flags_changes() + progress_changes() * 7919u; }

/* One registered condition */
static int eval_cond(Inst *in, const Reg *r, int depth)
{
    u32 w[64];
    const EvInstr *ins = load_instr(in, sets[in->set].ev[in->ev].ioff + r->off, r->idx, w);
    s32 *iw = (s32 *)w;
    Set *s = NULL;
    const EvEnt *e;
    float p[3], q[3];
    switch (ins->op) {
    case OP_C_GROUP: return eval_group(in, (s8)iw[2], depth + 1) == (iw[1] != 0);
    case OP_C_ELAPSED_S: return in->cond_t >= wf(w[1]);
    case OP_C_ELAPSED_F: return (s32)in->cond_f >= iw[1];
    case OP_C_FLAG: return flag_get(flag_of(in, iw[2], iw[3])) == (iw[1] != 0);
    case OP_C_FLAG_RANGE: return range_state(iw[1], flag_of(in, iw[2], iw[3]), flag_of(in, iw[2], iw[4]));
    case OP_C_AREA:
        e = ent_any(in->set, iw[3], &s);
        return e && ent_pos(in->set, iw[2], p) && in_region(e, p) == (iw[1] != 0);
    case OP_C_ALL_AREA:
        e = ent_any(in->set, iw[2], &s);
        return e && in_region(e, (const float[3]){pl->pos.x, pl->pos.y, pl->pos.z}) == (iw[1] != 0);
    case OP_C_RADIUS:
        if (!ent_pos(in->set, iw[2], p) || !ent_pos(in->set, iw[3], q))
            return 0;
        return ((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]) <=
                wf(w[4]) * wf(w[4])) == (iw[1] != 0);
    case OP_C_ITEM: case OP_C_ITEM_BBOX: return (inv_count(item_id(iw[1], iw[2])) > 0) == (iw[3] != 0);
    case OP_C_ACTION: case OP_C_ACTION_LINE: case OP_C_ACTION_BOSS:
        if (!action_reach(in, w, ins->op != OP_C_ACTION))
            return 0;
        if (!act_press || act_used)
            return 0;
        act_used = 1;
        return 1;
    case OP_C_MP: return mp_state(iw[1]);
    case OP_C_IN_MAP: {
        Set *ps = player_set();
        int in_map = ps && ps->h->map[0] == (u8)iw[2] && ps->h->map[1] == (u8)iw[3];
        return in_map == (iw[1] != 0);
    }
    case OP_C_MP_EVENT: case OP_C_DROPPED_AREA: case OP_C_DROPPED: case OP_C_EVENT_MSG: case OP_C_MP_COUNT:
        return 0;  /* nothing of that kind happens offline / in the port yet */
    /* characters (ev_chr.c); the player's SpEffects are A6's: false as before */
    case OP_C_DAMAGED: return ev_chr_damaged(iw[1], iw[2]) == 1;
    case OP_C_SPEFFECT: return iw[1] != PLAYER_ENTITY && ev_chr_has_speffect(iw[1], iw[2]) == (iw[3] != 0);
    case OP_C_PART_HP: { int hp; return ev_chr_part_hp(iw[1], iw[2], &hp) > 0 && cmp(iw[4], hp, iw[3]); }
    case OP_C_OBJ_DESTROYED: {  /* 5[00] (group, destroyed state, object entity): breakables (objects.c) */
        int broken = 0;
        objects_entity(iw[2], NULL, &broken);
        return broken == (iw[1] != 0);
    }
    case OP_C_COUNT_FLAGS: {
        u32 a = flag_of(in, iw[1], iw[2]), b = flag_of(in, iw[1], iw[3]);
        return cmp(iw[4], flag_range_count(a < b ? a : b, a < b ? b : a), iw[5]);
    }
    case OP_C_TENDENCY: return cmp(iw[2], 0, iw[3]);  /* world tendency 0 (TECH_DEBT event-7) */
    case OP_C_EVENT_VALUE: return cmp(iw[3], flag_value_get(iw[1], iw[2]), iw[4]);
    case OP_C_CYCLE: return cmp(iw[1], 0, iw[2]);     /* NG */
    case OP_C_ONLINE: return iw[1] == 0;               /* offline */
    case OP_C_DEAD: {
        int dead = 0;
        if (iw[1] == PLAYER_ENTITY)
            dead = pl->hp < 0;
        else
            enemy_entity(iw[1], NULL, &dead, NULL, NULL);
        return dead == (iw[2] != 0);
    }
    case OP_C_HP: {
        float r = 1;
        if (iw[1] == PLAYER_ENTITY)
            r = pl->hp_max > 0 ? (float)pl->hp / pl->hp_max : 1;
        else
            enemy_entity(iw[1], NULL, NULL, &r, NULL);
        return cmpf(iw[2], r, wf(w[3]));
    }
    case OP_C_CHR_TYPE: return iw[1] == PLAYER_ENTITY && iw[2] == prog.chr_type;  /* 0 human, 8 Hollow (A4) */
    case OP_C_BACKREAD: {
        int act = 0;
        if (iw[1] == PLAYER_ENTITY)
            act = 1;
        else
            enemy_entity(iw[1], NULL, NULL, NULL, &act);
        return act == (iw[2] != 0);
    }
    case OP_C_CLASS: return iw[1] == 0;     /* Warrior (TECH_DEBT event-7) */
    case OP_C_COVENANT: return iw[1] == 0;  /* none */
    case OP_C_OBJACT:
        for (int k = 0; k < EV_SETS; k++) {
            Set *t = &sets[k];
            if (!t->blob)
                continue;
            const EvObjAct *oa = SEC(t, S_OBJACTS, EvObjAct);
            for (int i = 0; i < NSEC(t, S_OBJACTS); i++)
                if (oa[i].entity == iw[1] && t->oa_state[i] & 2)
                    return 1;
        }
        return 0;
    case OP_C_ON_HIT: {
        ColHit h;
        e = ent_any(in->set, iw[1], &s);
        if (!e || e->kind != ENT_COLLISION || !col_floor(pl->pos.x, pl->pos.z, pl->pos.y + 0.5f, pl->pos.y - 0.5f,
                                                         COL_SOLID, &h) || h.block != s->block)
            return 0;
        const EvRange *rg = SEC(s, S_RANGES, EvRange) + e->index;
        for (u32 k = 0; k < e->count; k++)
            if ((u32)h.tri >= rg[k].first && (u32)h.tri < rg[k].first + rg[k].count)
                return 1;
        return 0;
    }
    }
    return 0;
}

/* AND groups (> 0, and MAIN): every registered condition; OR (< 0): any; no conditions: false */
static int eval_group(Inst *in, int g, int depth)
{
    if (depth > 8)
        return 0;
    int n = 0, and = g >= 0;
    for (int i = 0; i < in->nreg; i++) {
        if (in->reg[i].group != g)
            continue;
        n++;
        int v = eval_cond(in, &in->reg[i], depth);
        if (and && !v)
            return 0;
        if (!and && v)
            return 1;
    }
    return n && and;
}

static void clear_conds(Inst *in)
{
    in->nreg = 0;
    in->cond_t = 0, in->cond_f = 0;
}

/* MAIN passed: every group's result is kept for the compiled tests, the conditions are cleared */
static void resolve(Inst *in)
{
    in->cvalid = in->cval = 0;
    for (int g = -7; g <= 7; g++) {
        int used = 0;
        for (int i = 0; i < in->nreg && !used; i++)
            used = in->reg[i].group == g;
        if (!used)
            continue;
        in->cvalid |= 1 << (g + 7);
        if (g == 0 || eval_group(in, g, 0))
            in->cval |= 1 << (g + 7);
    }
    clear_conds(in);
    event_stats.fired++;
    event_stats.last_id = ev_id(in);
    if (trace)
        printf("EVT %u/%d fired\n", (unsigned)ev_id(in), (int)in->slot);
}

/* ---- instance lifetime ---- */
static Inst *find_inst(int set, int ev, s32 slot)
{
    if (!sets[set].ev_live[ev])
        return NULL;
    for (int i = 0; i < ninst; i++)
        if (inst[i].state != I_FREE && inst[i].set == set && inst[i].ev == ev && inst[i].slot == slot)
            return &inst[i];
    return NULL;
}

static void restart(Inst *in)
{
    in->state = I_RUN, in->pc = 0, in->pcoff = 0;
    clear_conds(in);
    in->cvalid = in->cval = 0;
}

static int start(int set, int ev, s32 slot, const u8 *args, int argn)
{
    if (find_inst(set, ev, slot))
        return 0;  /* already running: DSR ignores a second InitializeEvent of the same event + slot */
    int i;
    for (i = free_hint < ninst ? free_hint : ninst; i < ninst && inst[i].state != I_FREE; i++)
        ;
    free_hint = i + 1;
    if (i == EV_MAX_INST) {
        printf("EVT out of instances: %u/%d not started\n", (unsigned)sets[set].ev[ev].id, (int)slot);
        return 0;
    }
    if (i == ninst)
        ninst++;
    Inst *in = &inst[i];
    memset(in, 0, sizeof *in);
    in->set = set, in->ev = ev, in->slot = slot;
    sets[set].ev_live[ev]++;
    if (argn > 0) {
        in->args = malloc(argn);
        if (in->args) {
            memcpy(in->args, args, argn);
            in->argn = argn > 255 ? 255 : argn;
        }
    }
    restart(in);
    event_stats.bytes += sizeof *in + in->argn;
    return 1;
}

static void free_inst(Inst *in)
{
    event_stats.bytes -= sizeof *in + in->argn + in->creg * sizeof(Reg);
    free(in->args);
    free(in->reg);
    sets[in->set].ev_live[in->ev]--;
    if (in - inst < free_hint)
        free_hint = in - inst;
    memset(in, 0, sizeof *in);
}

/* End (type 0): the event's slot flag goes on; Default / End-on-rest instances are freed, Restart-on-rest ones kept
 * for event_rest. Restart (type 1): from the top. */
static void finish(Inst *in, int type)
{
    if (type == 1) {
        restart(in);
        return;
    }
    if (ev_id(in) > 98 && in->slot >= 0)  /* FUN_1407d4230: id + slot on, not for ids up to 98 (constructors 0 / 50) */
        flag_set(ev_id(in) + in->slot, 1);
    event_stats.ended++;
    if (trace > 1)
        printf("EVT %u/%d end\n", (unsigned)ev_id(in), (int)in->slot);
    if (sets[in->set].ev[in->ev].rest == 1)
        in->state = I_DONE;
    else
        free_inst(in);
}

static void lookup_start(int own, u32 id, s32 slot, const u8 *args, int argn)
{
    for (int k = -1; k < EV_SETS; k++) {
        int si = k < 0 ? own : k;
        if (!sets[si].blob || (k >= 0 && k == own) || !sets[si].live)
            continue;
        for (int e = 0; e < sets[si].nev; e++)
            if (sets[si].ev[e].id == id) {
                start(si, e, slot, args, argn);
                return;
            }
    }
    printf("EVT InitializeEvent %u: no such event\n", (unsigned)id);
}

/* ---- world effects ---- */
static void set_hit(Set *s, const EvEnt *e, int on)
{
    const EvRange *rg = SEC(s, S_RANGES, EvRange) + e->index;
    for (u32 k = 0; k < e->count; k++)
        col_tris_enable(s->block, rg[k].first, rg[k].count, on);
    s->ent_off[e - s->ent] = !on;
}

/* Every record of an entity id in every set (objects / hits / SFX / map parts) */
static int set_entity(int own, s32 id, int on, int kind)
{
    int n = 0;
    for (int k = 0; k < EV_SETS; k++) {
        Set *s = &sets[k];
        if (!s->blob)
            continue;
        const EvEnt *e = find_ent(s, id);
        for (; e && e < s->ent + s->nent && e->id == id; e++) {
            if (kind && e->kind != kind)
                continue;
            if (e->kind == ENT_COLLISION)
                set_hit(s, e, on);
            else
                s->ent_off[e - s->ent] = !on;
            n++;
        }
    }
    (void)own;
    return n;
}

static void warp_to(int own, s32 dest)
{
    Set *s = NULL;
    const EvEnt *e = ent_any(own, dest, &s);
    float p[3];
    if (!e && !ent_pos(own, dest, p)) {
        printf("EVT warp: entity %d not loaded\n", (int)dest);
        return;
    }
    if (e)
        memcpy(p, e->pos, 12);
    ColHit h;
    int ok = col_floor(p[0], p[2], p[1] + 2, p[1] - 5, COL_SOLID, &h);
    pl->pos = v3(p[0], ok ? h.y : p[1], p[2]);
    if (e)
        pl->yaw = e->yaw + 3.14159265f;  /* MSB rotY -> player yaw, as export_map.py's spawn (+ pi) */
    pl->vel = v3(0, 0, 0), pl->airborne = 0, pl->fall_y0 = pl->pos.y;
    printf("EVT warp player -> %d (%.2f %.2f %.2f)\n", (int)dest, pl->pos.x, pl->pos.y, pl->pos.z);
}

static const EvLot *find_lot(s32 id, const EvLotItem **items)
{
    for (int k = 0; k < EV_SETS; k++) {
        const Set *s = &sets[k];
        if (!s->blob)
            continue;
        const EvLot *l = SEC(s, S_LOTS, EvLot);
        for (int i = 0; i < NSEC(s, S_LOTS); i++)
            if (l[i].id == id) {
                *items = (const EvLotItem *)(l + NSEC(s, S_LOTS)) + l[i].first;  /* the item pool after the rows */
                return &l[i];
            }
    }
    return NULL;
}

/* An item's get flag: its own getItemFlagId0N, else the row's (< 0: none) */
static s32 lot_flag(const EvLot *l, const EvLotItem *it) { return it->flag ? it->flag : l->flag; }
/* Something of the row is still to be had: an item whose flag is off or absent */
static int lot_left(const EvLot *l, const EvLotItem *it)
{
    for (int k = 0; k < l->n; k++)
        if (lot_flag(l, &it[k]) <= 0 || !flag_get(lot_flag(l, &it[k])))
            return 1;
    return 0;
}

/* ItemLotParam draw as DSR's FUN_1403f5d90 (called through FUN_1403fc940 for AwardItemLot and treasures): the lot and
 * every row after it (id + 1, ... while one exists) each roll one of their items, weighted by lotItemBasePoint,
 * leaving out items whose get flag is on; the chosen item is given unless it is "nothing" (lotItemNum 0, id or
 * category < 0); the get flags of what was given go on after the whole run (rows of one corpse share one flag). Luck
 * (enableLuck x item discovery) and cumulate counters (cumulateLotPoint / cumulateNumFlagId) are left out: TECH_DEBT
 * event-6. */
static void award_lot(s32 id)
{
    const EvLotItem *it;
    const EvLot *l = find_lot(id, &it);
    if (!l) {
        printf("EVT item lot %d: not exported\n", (int)id);
        return;
    }
    s32 got_flag[16];
    int ngot = 0;
    for (s32 r = id; l; l = find_lot(++r, &it)) {
        int w[8], total = 0, n = l->n < 8 ? l->n : 8;
        for (int k = 0; k < n; k++) {
            s32 f = lot_flag(l, &it[k]);
            w[k] = f > 0 && flag_get(f) ? 0 : it[k].point;
            total += w[k];
        }
        if (!total)
            continue;
        int roll = (int)(rnd() % (u32)total), k = 0;
        while (k < n - 1 && roll >= w[k])
            roll -= w[k++];
        const EvLotItem *g = &it[k];
        if (!g->num || g->item < 0 || g->cat == 15) {
            printf("EVT item lot %d: nothing\n", (int)r);
            continue;
        }
        u32 item = (u32)g->cat << 28 | ((u32)g->item & 0x0FFFFFFFu);  /* category = progress.h ITEM_* */
        int held = inv_add(item, g->num);
        printf("EVT item lot %d: item %08x x%d (now %d), flag %d\n", (int)r, (unsigned)item, g->num, held,
               (int)lot_flag(l, g));
        if (lot_flag(l, g) > 0 && ngot < 16)
            got_flag[ngot++] = lot_flag(l, g);
    }
    for (int i = 0; i < ngot; i++)
        flag_set(got_flag[i], 1);
}

static void treasure_state(s32 entity, int on)
{
    for (int k = 0; k < EV_SETS; k++) {
        Set *s = &sets[k];
        if (!s->blob)
            continue;
        const EvTreasure *t = SEC(s, S_TREASURES, EvTreasure);
        for (int i = 0; i < NSEC(s, S_TREASURES); i++)
            if (t[i].entity == entity)
                s->tr_state[i] = (s->tr_state[i] & ~1) | (on != 0);
    }
}

static void objact_state(s32 part, int on)
{
    for (int k = 0; k < EV_SETS; k++) {
        Set *s = &sets[k];
        if (!s->blob)
            continue;
        const EvObjAct *oa = SEC(s, S_OBJACTS, EvObjAct);
        for (int i = 0; i < NSEC(s, S_OBJACTS); i++)
            if (oa[i].part == part)
                s->oa_state[i] = (s->oa_state[i] & ~1) | (on != 0);
    }
}

/* ---- the interpreter ---- */
static void skip(Inst *in, int n)
{
    const Set *s = &sets[in->set];
    int lim = s->ev[in->ev].ninstr;
    for (; n > 0 && in->pc < lim; n--, in->pc++)
        in->pcoff = instr_next(s, s->ev[in->ev].ioff + in->pcoff) - s->ev[in->ev].ioff;
}

/* Runs one instance until it waits or ends (instruction budget per frame) */
static void run(Inst *in)
{
    Set *s = &sets[in->set];
    const EvEvent *ev = &s->ev[in->ev];
    if (in->state == I_MAIN) {
        in->cond_t += dtime, in->cond_f++;
        if (in->flag_only && in->seen == state_key())
            return;
        in->seen = state_key();
        if (!eval_group(in, 0, 0))
            return;
        resolve(in);
        in->state = I_RUN;
    } else if (in->state == I_WAIT_T || in->state == I_WAIT_F) {
        in->wait -= in->state == I_WAIT_T ? dtime : 1;
        if (in->wait > 0)
            return;
        in->state = I_RUN;
    }
    u32 w[64];
    for (int budget = 256; in->state == I_RUN && budget > 0; budget--) {
        if (in->pc >= ev->ninstr) {
            finish(in, 0);
            return;
        }
        const EvInstr *ins = load_instr(in, ev->ioff + in->pcoff, in->pc, w);
        s32 *iw = (s32 *)w;
        int op = ins->op, k = in->pc;
        u32 off = in->pcoff;
        in->pc++, in->pcoff = instr_next(s, ev->ioff + off) - ev->ioff;
        if (op >= OP_C_GROUP && op <= OP_C_ON_HIT) {  /* condition: register in its group; MAIN waits for it */
            if (in->nreg == in->creg) {
                int n = in->creg ? in->creg * 2 : 4;
                Reg *g = in->creg < 128 ? realloc(in->reg, n * sizeof *g) : NULL;
                if (!g) {
                    printf("EVT %u/%d: too many conditions\n", (unsigned)ev->id, (int)in->slot);
                    continue;
                }
                in->reg = g, in->creg = n;
                event_stats.bytes += (n - in->nreg) * sizeof *g;
            }
            Reg *r = &in->reg[in->nreg++];
            r->off = off, r->idx = k, r->group = (s8)iw[0];
            if (r->group == 0) {
                if (eval_group(in, 0, 0))
                    resolve(in);
                else {
                    in->state = I_MAIN;
                    in->seen = state_key();
                    in->flag_only = 1;
                    for (int i = 0; i < in->nreg; i++)
                        in->flag_only &= static_cond(instr_at(s, ev->ioff + in->reg[i].off)->op);
                }
            }
            continue;
        }
        switch (op) {
        case OP_UNSUPPORTED:
            event_stats.unsupported++;
            if (trace)
                printf("EVT %u/%d unsupported %d[%02d]\n", (unsigned)ev->id, (int)in->slot, ins->bank, ins->id);
            break;
        /* control: lines skipped, End type 0 end / 1 restart */
        case OP_SKIP_COND_U: if (eval_group(in, (s8)iw[2], 0) == (iw[1] != 0)) skip(in, iw[0]); break;
        case OP_END_COND_U: if (eval_group(in, (s8)iw[2], 0) == (iw[1] != 0)) { finish(in, iw[0]); return; } break;
        case OP_SKIP_COND_C:
        case OP_END_COND_C: {
            int g = (s8)iw[2] + 7, v = g >= 0 && g < GROUPS && (in->cval >> g & 1);
            if (v == (iw[1] != 0)) {
                if (op == OP_END_COND_C) { finish(in, iw[0]); return; }
                skip(in, iw[0]);
            }
            break;
        }
        case OP_SKIP: skip(in, iw[0]); break;
        case OP_END: finish(in, iw[0]); return;
        case OP_SKIP_CMP: if (cmp(iw[1], iw[2], iw[3])) skip(in, iw[0]); break;
        case OP_END_CMP: if (cmp(iw[1], iw[2], iw[3])) { finish(in, iw[0]); return; } break;
        case OP_WAIT_NET: break;
        case OP_WAIT_S: in->wait = wf(w[0]), in->state = I_WAIT_T, clear_conds(in); break;
        case OP_WAIT_F: in->wait = iw[0], in->state = I_WAIT_F, clear_conds(in); break;
        case OP_WAIT_RAND:
            in->wait = wf(w[0]) + (wf(w[1]) - wf(w[0])) * (rnd() & 0xFFFF) / 65535.0f;
            in->state = I_WAIT_T, clear_conds(in);
            break;
        case OP_SKIP_FLAG: if (flag_get(flag_of(in, iw[2], iw[3])) == (iw[1] != 0)) skip(in, iw[0]); break;
        case OP_END_FLAG:
            if (flag_get(flag_of(in, iw[2], iw[3])) == (iw[1] != 0)) { finish(in, iw[0]); return; }
            break;
        case OP_SKIP_FLAG_RANGE:
            if (range_state(iw[1], flag_of(in, iw[2], iw[3]), flag_of(in, iw[2], iw[4]))) skip(in, iw[0]);
            break;
        case OP_SKIP_MP: if (mp_state(iw[1])) skip(in, iw[0]); break;
        case OP_END_MP: if (mp_state(iw[1])) { finish(in, iw[0]); return; } break;
        case OP_SKIP_IN_MAP: {
            Set *ps = player_set();
            int inside = ps && ps->h->map[0] == (u8)iw[2] && ps->h->map[1] == (u8)iw[3];
            if (inside == (iw[1] != 0))
                skip(in, iw[0]);
            break;
        }
        /* actions */
        case OP_INIT: lookup_start(in->set, w[1], iw[0], (const u8 *)(w + 2), 4 * (ins->nw - 2)); break;
        case OP_SET_FLAG:
            flag_set(iw[0], iw[1] == 2 ? !flag_get(iw[0]) : iw[1] != 0);
            if (trace > 1)
                printf("EVT %u/%d flag %d = %d\n", (unsigned)ev->id, (int)in->slot, (int)iw[0], flag_get(iw[0]));
            break;
        case OP_EVENT_STATE:
            for (int e = 0; e < EV_SETS; e++)
                for (int j = 0; sets[e].blob && j < sets[e].nev; j++)
                    if (sets[e].ev[j].id == w[0]) {
                        Inst *t = find_inst(e, j, iw[1]);
                        if (!t)
                            continue;
                        /* FUN_1407cb090 -> the instance's FUN_1407d4230: 0 ends it, 1 runs it again from the top;
                         * both set its flag (id + slot, ids > 98) */
                        if (iw[2] == 0)
                            t->state = I_STOPPED;
                        else
                            restart(t);
                        if (w[0] > 98 && iw[1] >= 0)
                            flag_set(w[0] + iw[1], 1);
                    }
            break;
        case OP_RAND_FLAG:
            if (w[1] >= w[0])
                flag_set(w[0] + rnd() % (w[1] - w[0] + 1), iw[2] != 0);
            break;
        case OP_BATCH_FLAGS:
            if (iw[2] == 2)
                for (s32 f = iw[0]; f <= iw[1]; f++)
                    flag_set(f, !flag_get(f));
            else
                flag_range_set(iw[0], iw[1], iw[2] != 0);
            break;
        case OP_REMOVE_ITEM: inv_add(item_id(iw[0], iw[1]), -iw[2]); break;
        case OP_EV_INC: {
            u32 v = flag_value_get(iw[0], iw[1]);
            if (v < w[2])
                flag_value_set(iw[0], iw[1], v + 1);
            break;
        }
        case OP_EV_CLEAR: flag_value_set(iw[0], iw[1], 0); break;
        case OP_AWARD_LOT: award_lot(iw[0]); break;
        case OP_SAVE_REQ: save_request(); break;
        case OP_BOSS_BAR: /* 2003[11] (on, entity, slot, NpcParam name id) */
            hud_boss_bar(iw[0] ? iw[2] : -1, iw[0] ? iw[1] : -1, iw[3]);
            printf("EVT %u/%d boss bar %s %d (slot %d, name %d)\n", (unsigned)ev->id, (int)in->slot,
                   iw[0] ? "on" : "off", (int)iw[1], (int)iw[2], (int)iw[3]);
            break;
        case OP_BOSS_DEFEAT: ev_boss_defeat(iw[0]); break;
        case OP_RESPAWN:  /* 2003[23] Set Player Respawn Point (an MSB SpawnPoint): where a death puts the player */
            prog.respawn = iw[0];
            progress_touch();
            break;
        case OP_STATUS_MSG: ev_message(0, iw[0]); break;
        case OP_BANNER: ev_message(1, iw[0]); break;
        case OP_BONFIRE: {  /* 2009[03] (flag, bonfire object, reaction distance, angle, kindling) */
            float p[3];
            if (ent_pos(in->set, iw[1], p))
                ev_bonfire_register(in->set, iw[0], iw[1], p, wf(w[2]), wf(w[3]), iw[4]);
            else
                stub(in, op);
            break;
        }
        case OP_KILL:
            if (iw[0] == PLAYER_ENTITY) {
                pl->hp = -1;  /* death camera / respawn as a lethal fall (player.c) */
                printf("EVT %u/%d kills the player\n", (unsigned)ev->id, (int)in->slot);
            } else
                ev_chr_kill(iw[0]);
            break;
        case OP_CHR_ENABLE: ev_chr_enable(iw[0], iw[1] != 0, 1); break;
        case OP_BACKREAD: ev_chr_enable(iw[0], iw[1] == 0, 2); break;  /* own bit: not an enable */
        /* character ops (ev_chr.c): stub = the player or a full record table */
        case OP_AI_STATE: if (!ev_chr_ai(iw[0], iw[1] != 0)) stub(in, op); break;
        case OP_TEAM: case OP_TEAM_EXIT: if (!ev_chr_team(iw[0], (s8)iw[1], op == OP_TEAM_EXIT)) stub(in, op); break;
        case OP_SPEFFECT: case OP_CLEAR_SPEFFECT: if (!ev_chr_speffect(iw[0], iw[1], op == OP_SPEFFECT)) stub(in, op); break;
        case OP_SPECIAL_ANIM: if (!ev_chr_special_wait(iw[0], iw + 1)) stub(in, op); break;
        case OP_GRAVITY: if (!ev_chr_flag(EVC_GRAVITY_OFF, iw[0], iw[1] != 0)) stub(in, op); break;
        case OP_IMMORTAL: if (!ev_chr_flag(EVC_IMMORTAL, iw[0], iw[1] != 0)) stub(in, op); break;
        case OP_INVINCIBLE: if (!ev_chr_flag(EVC_INVINCIBLE, iw[0], iw[1] != 0)) stub(in, op); break;
        case OP_MAPHITS: if (!ev_chr_flag(EVC_MAPHIT_OFF, iw[0], iw[1] != 0)) stub(in, op); break;
        case OP_DEF_BACKREAD: if (!ev_chr_flag(EVC_ALWAYS_ON, iw[0], iw[1] != 0)) stub(in, op); break;
        case OP_HOME: { float p[3]; if (!ent_pos(in->set, iw[1], p) || !ev_chr_home(iw[0], p)) stub(in, op); break; }
        case OP_ROTATE: {
            float p[3];
            if (!ent_pos(in->set, iw[1], p))
                stub(in, op);
            else if (iw[0] == PLAYER_ENTITY)  /* the player faces the target at once (fog-wall entry: 1012897) */
                pl->yaw = atan2f(p[0] - pl->pos.x, p[2] - pl->pos.z);
            else if (!ev_chr_rotate(iw[0], p))
                stub(in, op);
            break;
        }
        case OP_AI_CMD: if (!ev_chr_ai_cmd(iw[0], iw[1], iw[2])) stub(in, op); break;
        case OP_AI_ID: if (!ev_chr_ai_logic(iw[0], iw[1])) stub(in, op); break;
        case OP_AI_REPLAN: case OP_AI_TARGET: if (!ev_chr_replan(iw[0])) stub(in, op); break;
        case OP_NPC_PART: if (!ev_chr_part(iw[0], iw[1], iw[3])) stub(in, op); break;
        case OP_DISPMASK: if (!ev_chr_dispmask(iw[0], iw[1], iw[2])) stub(in, op); break;
        case OP_ANIM_RESET: if (!ev_chr_anim_reset(iw[0])) stub(in, op); break;
        case OP_WARP_FLOOR: case OP_SHORT_WARP: case OP_WARP_COPY:
            if (iw[0] == PLAYER_ENTITY)
                warp_to(in->set, iw[2]);
            else
                stub(in, op);
            break;
        /* object ops (ev_obj.c): stub = no breakable / animated object of that id (elevators: A9) */
        case OP_OBJ_ENABLE:
            set_entity(in->set, iw[0], iw[1] != 0, ENT_OBJECT);
            ev_obj_enable(iw[0], iw[1] != 0);
            dg_entity(iw[0], iw[1] != 0);  /* a static object baked into map.bin: its chunks (drawgrp.c) */
            break;
        case OP_OBJ_DESTROY: case OP_OBJ_REDESTROY: if (!ev_obj_destroy(iw[0])) stub(in, op); break;
        case OP_OBJ_RESTORE: if (!ev_obj_restore(iw[0])) stub(in, op); break;
        case OP_OBJ_ANIM: if (!ev_obj_end_anim(iw[0], iw[1])) stub(in, op); break;
        case OP_ANIM: {  /* 2003[18] (entity, anim, loop, wait for completion, skip transition): objects here */
            float len = 0;
            if (iw[0] == PLAYER_ENTITY)  /* the player (fog-wall walk 7410): player.c, the exported anims only */
                len = player_event_anim(pl, iw[1]);
            if (!len && !ev_obj_anim(iw[0], iw[1], iw[2] != 0, &len))
                stub(in, op);  /* enemies / NPCs: A5 */
            else if (iw[3] && !iw[2])
                in->wait = len, in->state = I_WAIT_T, clear_conds(in);
            break;
        }
        case OP_TREASURE: treasure_state(iw[0], iw[1] != 0); break;
        case OP_OBJACT_INIT: objact_state(iw[0], 1); break;
        case OP_OBJACT_STATE: objact_state(iw[0], iw[2] != 0); break;
        case OP_OBJACT_IDX: objact_state(iw[0], iw[3] != 0); break;
        case OP_HIT: set_entity(in->set, iw[0], iw[1] != 0, ENT_COLLISION); break;
        /* map SFX (fx.c) and map pieces (map chunks keyed by entity, drawgrp.c) by entity id: event-5 */
        case OP_SFX_DEL:  /* 2006[01] (entity, root only): root only = the emitter stops, its particles live on */
            set_entity(in->set, iw[0], 0, 0);
            dg_entity(iw[0], 0);
            fx_entity_particles(iw[0], iw[1] == 0);
            break;
        case OP_SFX_SPAWN: set_entity(in->set, iw[0], 1, 0); dg_entity(iw[0], 1); break;
        case OP_MAP_PART: set_entity(in->set, iw[0], iw[1] != 0, 1); dg_entity(iw[0], iw[1] != 0); break;
        case OP_CUTSCENE_WARP:
            /* 2002[04] (cutscene, flags, region, AA, BB): the remo is A13's, so it is skipped and only its warp runs. A
             * region of another map of this world comes with the map's events.bin (export_emevd.foreign_warp_targets);
             * setting the player there makes world.c settle the destination block like DSR's loading screen (E7) */
            printf("EVT %u/%d cutscene %d skipped, warp to m%02d_%02d point %d\n", (unsigned)ev->id, (int)in->slot,
                   (int)iw[0], (int)(u8)iw[3], (int)(u8)iw[4], (int)iw[2]);
            if (!ent_any(in->set, iw[2], NULL))
                printf("EVT area transition to m%02d_%02d: not in this world build (A1)\n", (int)(u8)iw[3],
                       (int)(u8)iw[4]);
            else
                warp_to(in->set, iw[2]), in->warped = 1;
            break;
        default:
            if (!op_noop(op))
                stub(in, op);
            break;
        }
    }
}

/* ---- public ---- */
static int load_set(Set *s, const void *blob, int block)
{
    const EvHdr *h = blob;
    if (!h || memcmp(h->magic, "EVB1", 4) || h->version != 3) {  /* export_emevd.py VERSION */
        printf("EVT bad events blob\n");
        return 0;
    }
    memset(s, 0, sizeof *s);
    s->blob = blob, s->h = h, s->block = block;
    s->ev = SEC(s, S_EVENTS, EvEvent), s->nev = NSEC(s, S_EVENTS);
    s->ent = SEC(s, S_ENTS, EvEnt), s->nent = NSEC(s, S_ENTS);
    s->ent_off = calloc(s->nent + 1, 1);
    s->tr_state = calloc(NSEC(s, S_TREASURES) + 1, 1);
    s->oa_state = calloc(NSEC(s, S_OBJACTS) + 1, 1);
    s->ev_live = calloc(s->nev + 1, sizeof *s->ev_live);
    const EvTreasure *t = SEC(s, S_TREASURES, EvTreasure);
    for (int i = 0; i < NSEC(s, S_TREASURES); i++) {
        int left = 0;
        for (int k = 0; k < 5; k++) {
            const EvLotItem *it;
            const EvLot *l = t[i].lot[k] > 0 ? find_lot(t[i].lot[k], &it) : NULL;
            left |= l && lot_left(l, it);
        }
        s->tr_state[i] = (t[i].start_disabled ? 0 : 1) | (left ? 0 : 2);  /* picked up before: taken */
    }
    const EvObjAct *oa = SEC(s, S_OBJACTS, EvObjAct);
    for (int i = 0; i < NSEC(s, S_OBJACTS); i++) {
        int done = oa[i].flag > 0 && flag_get(oa[i].flag);
        /* ASSUMED: usable until its flag is on (event-4); ObjActs without an entity flag (Burg door o1312: 0) until
         * their state flag is */
        s->oa_state[i] = oa[i].entity > 0 ? !flag_get(oa[i].entity) : !done;
    }
    s->live = 1;
    return 1;
}

static void start_ctor(int k)
{
    for (int e = 0; e < sets[k].nev; e++)  /* preconstructor 50 first, then constructor 0 */
        if (sets[k].ev[e].id == 50)
            start(k, e, 0, NULL, 0);
    for (int e = 0; e < sets[k].nev; e++)
        if (sets[k].ev[e].id == 0)
            start(k, e, 0, NULL, 0);
}

int event_init(const void *common_blob)
{
    if (!inst)
        inst = calloc(EV_MAX_INST, sizeof *inst);
    if (!inst || !load_set(&sets[0], common_blob, -1))
        return 0;
    event_stats.prompt = -1;
    ev_temp_flags_reset(1);  /* the game load (save_boot ran before): DSR's area-load reset, FUN_1404f1fb0 */
    start_ctor(0);
    printf("EVT common: %d events, instance pool %u B\n", sets[0].nev, (unsigned)(EV_MAX_INST * sizeof *inst));
    return 1;
}

void event_block_on(int block, const void *blob)
{
    int k;
    for (k = 1; k < EV_SETS && sets[k].blob; k++)
        ;
    if (!blob || k == EV_SETS || !inst || !load_set(&sets[k], blob, block))
        return;
    Set *s = &sets[k];
    for (int j = 1; j < EV_SETS; j++)  /* another block of the same map runs its events: this one adds entities */
        if (j != k && sets[j].blob && sets[j].live && !memcmp(sets[j].h->map, s->h->map, 4))
            s->live = 0;
    /* No flag reset here: DSR clears the temporary flags of every map at a load or rest (ev_temp_flags_reset), not
     * when a map streams in, so what happened in a block lasts until the next rest (event-1) */
    if (s->live)
        start_ctor(k);
    int act = 0;
    for (int i = 0; i < ninst; i++)
        act += inst[i].state != I_FREE;
    printf("EVT block %d: m%02d_%02d %d events, %d entities, %d colliders, %d treasures, %d objacts%s; %d instances\n",
           block, s->h->map[0], s->h->map[1], s->nev, s->nent, NSEC(s, S_COLS), NSEC(s, S_TREASURES),
           NSEC(s, S_OBJACTS), s->live ? "" : " (entities only)", act);
}

static inline unsigned ticks(void);

/* Same events in both sets (blocks of one map: export_emevd.py writes the map's events, instrs and substs alike) */
static int same_events(const Set *a, const Set *b)
{
    for (int k = S_EVENTS; k <= S_SUBSTS; k++) {
        unsigned n = k == S_EVENTS ? NSEC(a, k) * sizeof(EvEvent) : k == S_INSTRS ? (unsigned)NSEC(a, k)
                     : NSEC(a, k) * sizeof(EvSubst);
        if (NSEC(a, k) != NSEC(b, k) || memcmp(a->blob + a->h->sec[k][0], b->blob + b->h->sec[k][0], n))
            return 0;
    }
    return 1;
}

void event_block_off(int block)
{
    for (int k = 1; k < EV_SETS; k++) {
        Set *s = &sets[k];
        if (!s->blob || s->block != block)
            continue;
        /* event-10: the map's live set goes, another loaded block of the same map takes its running events over (DSR
         * runs one event manager per map, whichever part of it is loaded), else they end with the block */
        int heir = -1;
        for (int j = 1; j < EV_SETS && s->live && heir < 0; j++)
            if (j != k && sets[j].blob && !sets[j].live && !memcmp(sets[j].h->map, s->h->map, 4) && same_events(s, &sets[j]))
                heir = j;
        int moved = 0;
        for (int i = 0; i < ninst; i++) {
            if (inst[i].state == I_FREE || inst[i].set != k)
                continue;
            /* an event that warped the player out of this map still runs its lines after the warp (Asylum
             * 11810000: flag 11810000, respawn point, save request) before its map goes: waits pass at once */
            for (int n = 0; inst[i].warped && heir < 0 && n < 16 && inst[i].state != I_FREE && inst[i].state != I_DONE &&
                            inst[i].state != I_STOPPED && inst[i].state != I_MAIN; n++) {
                if (inst[i].state == I_WAIT_T || inst[i].state == I_WAIT_F)
                    inst[i].state = I_RUN;
                run(&inst[i]);
            }
            if (inst[i].state == I_FREE)
                continue;
            if (heir >= 0)
                inst[i].set = heir, moved++;
            else
                free_inst(&inst[i]);
        }
        if (heir >= 0) {
            memcpy(sets[heir].ev_live, s->ev_live, (s->nev + 1) * sizeof *s->ev_live);
            sets[heir].live = 1;
            ev_bonfire_move(k, heir);
            printf("EVT block %d off: m%02d_%02d events (%d instances) go on in block %d\n", block, s->h->map[0],
                   s->h->map[1], moved, sets[heir].block);
        }
        free(s->ent_off), free(s->tr_state), free(s->oa_state), free(s->ev_live);
        memset(s, 0, sizeof *s);
        ev_bonfire_drop(k);
    }
}

/* event-14: a warp's load (world.c settle) is DSR's loading screen: the constructors of the blocks it turned on and
 * whatever they start run to their first wait now, not over the next frames' EV_BUDGET_US */
void event_flush(void)
{
    if (!inst || !pl)
        return;
    unsigned t0 = ticks();
    int runs = 0;
    for (int pass = 0; pass < 16; pass++) {
        int any = 0;
        for (int i = 0; i < ninst; i++)
            if (inst[i].state == I_RUN) {
                run(&inst[i]);
                any = 1, runs++;
            }
        if (!any)
            break;
    }
    if (runs)
        printf("EVT flush: %d instance runs in %u us\n", runs, (ticks() - t0) / 295);
}

void event_action(void) { act_press = 1; }

static inline unsigned ticks(void)
{
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

/* ObjActParam key (spQualifiedType 1 = a good in the inventory, either of the two; ASSUMED: other types unmet, TECH_DEBT
 * event-11); spQualifiedPassEventFlag (on once the door was opened from the other side) opens it as well */
static int objact_key(const EvObjAct *oa)
{
    if (!oa->sp_type && !oa->sp_type2)
        return 1;
    if (oa->pass_flag > 0 && flag_get(oa->pass_flag))
        return 1;
    return (oa->sp_type == 1 && inv_count(ITEM_GOODS | (u32)oa->sp_id) > 0) ||
           (oa->sp_type2 == 1 && inv_count(ITEM_GOODS | (u32)oa->sp_id2) > 0);
}

/* Action button on ObjActs (chests, levers, doors: their flag, the object's anim) and treasures (item lots) */
/* ObjAct state flags (MSB event flag: 6AABBnnn) on: the object holds the end of its ObjAct anim (an opened door after a
 * load, or one a constructor opens: Firelink's o1465 door, flag 61020120 set by m10_02 event 0); checked every frame
 * because constructors set them after the block's load (ASSUMED: DSR applies them when the flag is on, event-11) */
static void objact_flags(void)
{
    for (int k = 0; k < EV_SETS; k++) {
        Set *s = &sets[k];
        if (!s->blob)
            continue;
        const EvObjAct *oa = SEC(s, S_OBJACTS, EvObjAct);
        for (int i = 0; i < NSEC(s, S_OBJACTS); i++)
            if (!(s->oa_state[i] & 4) && oa[i].flag > 0 && flag_get(oa[i].flag)) {
                s->oa_state[i] |= 4;
                ev_obj_objact(oa[i].part, oa[i].anim, 1);
            }
    }
}

static void use_action(void)
{
    const float *pp = &pl->pos.x;
    for (int k = 0; k < EV_SETS && !act_used; k++) {
        Set *s = &sets[k];
        if (!s->blob)
            continue;
        const EvObjAct *oa = SEC(s, S_OBJACTS, EvObjAct);
        for (int i = 0; i < NSEC(s, S_OBJACTS) && !act_used; i++) {
            float dx = oa[i].pos[0] - pp[0], dz = oa[i].pos[2] - pp[2], r = oa[i].dist + 0.8f;  /* ASSUMED event-4 */
            if (!(s->oa_state[i] & 1) || dx * dx + dz * dz > r * r || fabsf(oa[i].pos[1] - pp[1]) > 2)
                continue;
            act_used = 1;
            if (!objact_key(&oa[i])) {  /* locked: DSR shows actionFailedMsgId; the ObjAct stays usable */
                printf("EVT objact %d (part %d) locked: needs good %d / %d or flag %d\n", (int)oa[i].entity,
                       (int)oa[i].part, (int)oa[i].sp_id, (int)oa[i].sp_id2, (int)oa[i].pass_flag);
                continue;
            }
            s->oa_state[i] = 2 | 4;  /* used: fired this frame, not usable again until an event says so; plays its anim */
            for (int j = 0; j < EV_SETS; j++) {  /* the same part in another block of the map: playing already, so
                                                  * objact_flags must not jump it to the end pose */
                const EvObjAct *o2 = sets[j].blob ? SEC(&sets[j], S_OBJACTS, EvObjAct) : NULL;
                for (int m = 0; o2 && m < NSEC(&sets[j], S_OBJACTS); m++)
                    if (o2[m].part == oa[i].part)
                        sets[j].oa_state[m] |= 4;
            }
            if (oa[i].entity > 0)
                flag_set(oa[i].entity, 1);
            if (oa[i].flag > 0)
                flag_set(oa[i].flag, 1);
            ev_obj_objact(oa[i].part, oa[i].anim, 0);
            printf("EVT objact %d (part %d, ObjActParam %d) used\n", (int)oa[i].entity, (int)oa[i].part,
                   (int)oa[i].param);
        }
    }
    for (int k = 0; k < EV_SETS && !act_used; k++) {
        Set *s = &sets[k];
        if (!s->blob)
            continue;
        const EvTreasure *t = SEC(s, S_TREASURES, EvTreasure);
        for (int i = 0; i < NSEC(s, S_TREASURES) && !act_used; i++) {
            float dx = t[i].pos[0] - pp[0], dz = t[i].pos[2] - pp[2];
            if (s->tr_state[i] != 1 || dx * dx + dz * dz > 1.5f * 1.5f || fabsf(t[i].pos[1] - pp[1]) > 2)
                continue;  /* ASSUMED 1.5 m pickup reach (event-4) */
            s->tr_state[i] |= 2;
            act_used = 1;
            printf("EVT treasure at %.1f %.1f %.1f picked up\n", t[i].pos[0], t[i].pos[1], t[i].pos[2]);
            for (int j = 0; j < 5; j++)
                if (t[i].lot[j] > 0)
                    award_lot(t[i].lot[j]);
        }
    }
    if (!act_used && ev_bonfire_action((const float[3]){pl->pos.x, pl->pos.y, pl->pos.z}, pl->yaw))  /* 2009[03] */
        act_used = 1;
}

void event_update(Player *p, float dt)
{
    if (!inst)
        return;
    unsigned t0 = ticks();
    pl = p, dtime = dt;
    act_used = 0;
    ev_chr_update(dt);
    ev_misc_update(dt);
    event_stats.prompt = -1;
    if (act_press)
        use_action();
    /* EV_BUDGET_US per frame: a map's constructors (300+ instances at a block load, 12-20 ms) spread over a few
     * frames, round robin from where the last frame stopped (TECH_DEBT event-14); the instances an InitializeEvent
     * adds behind the cursor run in the same pass */
    static int rr;
    int n0 = ninst, done = 0;
    for (int n = 0; n < ninst; n++) {
        int i = n < n0 ? (rr + n) % n0 : n;
        Inst *in = &inst[i];
        if (in->state != I_FREE && in->state != I_DONE && in->state != I_STOPPED)
            run(in);
        if (n + 1 < ninst && ticks() - t0 > EV_BUDGET_US * 295u) {
            rr = (i + 1) % (n0 ? n0 : 1);
            event_stats.deferred++;
            break;
        }
        done = n + 1;
    }
    if (done == ninst)
        rr = 0;
    int running = 0, waiting = 0, act = 0;
    for (int i = 0; i < ninst; i++) {
        Inst *in = &inst[i];
        act += in->state != I_FREE;
        running += in->state == I_RUN;
        waiting += in->state == I_MAIN || in->state == I_WAIT_T || in->state == I_WAIT_F;
    }
    for (int k = 0; k < EV_SETS; k++)  /* ObjAct "used this frame" lasts one update */
        for (int i = 0; sets[k].blob && i < NSEC(&sets[k], S_OBJACTS); i++)
            sets[k].oa_state[i] &= ~2;
    objact_flags();
    act_press = 0;
    event_stats.running = act, event_stats.waiting = waiting;
    if (act > event_stats.peak)
        event_stats.peak = act;
    unsigned us = (ticks() - t0) / 295;  /* EE clock 294.912 MHz */
    event_stats.us = (event_stats.us * 7 + us) / 8;
    if (us > 4000)
        printf("EVT update %u us, %d instances\n", us, act);
    (void)running;
}

int event_push_player(float pos[3], float r, float h)
{
    int touched = 0;
    /* the fog-wall walk (and any event anim) passes the object colliders, as DSR's 7410 walks through the fog
     * (ASSUMED: all colliders, not only the fog's; TECH_DEBT event-18) */
    if (pl && player_event_anim_playing(pl))
        return 0;
    for (int k = 1; k < EV_SETS; k++) {
        Set *s = &sets[k];
        if (!s->blob)
            continue;
        const EvCol *c = SEC(s, S_COLS, EvCol);
        for (int i = 0; i < NSEC(s, S_COLS); i++) {
            float dx0 = pos[0] - c[i].c[0], dz0 = pos[2] - c[i].c[2], reach = c[i].half[0] + c[i].half[2] + r;
            if (dx0 * dx0 + dz0 * dz0 > reach * reach)  /* every object has colliders now: cheap reject first */
                continue;
            const EvEnt *e = c[i].entity < 0 ? NULL : find_ent(s, c[i].entity);  /* -1: no entity id, always solid */
            if ((c[i].entity >= 0 && (!e || !ent_on(s, e))) || pos[1] + h < c[i].c[1] - c[i].half[1] ||
                pos[1] > c[i].c[1] + c[i].half[1] || c[i].c[1] + c[i].half[1] < pos[1] + 0.4f)
                continue;  /* ASSUMED: tops within 0.4 m above the feet are walked over (objects.c STEP_OVER) */
            float co = cosf(c[i].yaw), sn = sinf(c[i].yaw);
            float dx = pos[0] - c[i].c[0], dz = pos[2] - c[i].c[2];
            float lx = dx * co - dz * sn, lz = dx * sn + dz * co;  /* inverse of the exporter's object -> world */
            float px = c[i].half[0] + r - fabsf(lx), pz = c[i].half[2] + r - fabsf(lz);
            if (px <= 0 || pz <= 0)
                continue;
            if (px < pz)
                lx += lx < 0 ? -px : px;
            else
                lz += lz < 0 ? -pz : pz;
            pos[0] = c[i].c[0] + lx * co + lz * sn;
            pos[2] = c[i].c[2] - lx * sn + lz * co;
            touched = 1;
        }
    }
    return touched;
}

void event_rest(void)
{
    ev_temp_flags_reset(0);  /* FUN_1407cb660 (the event manager's rest pass) clears them first: FUN_1404f20b0 */
    for (int i = 0; i < ninst; i++) {
        Inst *in = &inst[i];
        if (in->state == I_FREE)
            continue;
        int rest = sets[in->set].ev[in->ev].rest;
        if (rest == 1)
            restart(in);
        else if (rest == 2)
            free_inst(in);
    }
}

static int find_state(const char *what, s32 id, int *found)
{
    *found = 0;
    for (int k = 0; k < EV_SETS; k++) {
        Set *s = &sets[k];
        if (!s->blob)
            continue;
        if (!strcmp(what, "treasure")) {
            const EvTreasure *t = SEC(s, S_TREASURES, EvTreasure);
            for (int i = 0; i < NSEC(s, S_TREASURES); i++)
                if (t[i].entity == id)
                    return *found = 1, s->tr_state[i];
            continue;
        }
        const EvEnt *e = find_ent(s, id);
        if (e)
            return *found = 1, !s->ent_off[e - s->ent];
    }
    return 0;
}

void event_test_cmd(const char *cmd)
{
    int want = 0;
    char a[16] = "", b[16] = "", c[8] = "";
    float v = 0, fx = 0, fz = 0;
    int id = 0, val = 0;
    if (sscanf(cmd, "flag %d %d", &id, &val) == 2) {
        flag_set(id, val);
        printf("TEST ev flag %d = %d\n", id, flag_get(id));
    } else if (!strcmp(cmd, "action")) {
        event_action();
    } else if (sscanf(cmd, "trace %d", &val) == 1) {
        trace = val;
    } else if (sscanf(cmd, "warp %d", &id) == 1 && pl) {
        warp_to(1, id);
    } else if (!strcmp(cmd, "waits")) {  /* what the waiting instances wait on: op of each registered condition */
        int h[OP_COUNT] = {0}, fo = 0, nm = 0;
        for (int i = 0; i < ninst; i++)
            if (inst[i].state == I_MAIN) {
                nm++, fo += inst[i].flag_only;
                for (int r = 0; r < inst[i].nreg; r++)
                    h[instr_at(&sets[inst[i].set], sets[inst[i].set].ev[inst[i].ev].ioff + inst[i].reg[r].off)->op]++;
            }
        printf("TEST ev waits: %d on MAIN (%d flag-only):", nm, fo);
        for (int o = 0; o < OP_COUNT; o++)
            if (h[o])
                printf(" %s %d", op_names[o], h[o]);
        printf("\n");
    } else if (!strcmp(cmd, "summary")) {
        printf("TEST ev summary pass %d fail %d; instances %d (peak %d), fired %d, ended %d, stubs %d, unsupported %d, "
               "%u B, %u us, %d deferred frames\n", test_pass, test_fail, event_stats.running, event_stats.peak,
               event_stats.fired, event_stats.ended, event_stats.stubs, event_stats.unsupported, event_stats.bytes,
               event_stats.us, event_stats.deferred);
    } else if (sscanf(cmd, "expect nofloor %f %f %f", &fx, &fz, &v) == 3) {
        ColHit h;  /* no solid floor within v + 1 .. v - 3 (a hole opened by an event) */
        int hit = col_floor(fx, fz, v + 1, v - 3, COL_SOLID, &h);
        test_pass += !hit, test_fail += hit;
        printf("TEST ev expect nofloor %.2f %.2f %.2f: %.2f %s\n", fx, fz, v, hit ? h.y : -999.0f, hit ? "FAIL" : "PASS");
    } else if (sscanf(cmd, "expect floor %f %f %f", &fx, &fz, &v) == 3) {
        ColHit h;  /* highest solid floor from v + 1 m down 3 m: hit parts switched by events show here */
        int hit = col_floor(fx, fz, v + 1, v - 3, COL_SOLID, &h), ok = hit && fabsf(h.y - v) < 0.15f;
        test_pass += ok, test_fail += !ok;
        printf("TEST ev expect floor %.2f %.2f %.2f: %.2f %s\n", fx, fz, v, hit ? h.y : -999.0f, ok ? "PASS" : "FAIL");
    } else if (sscanf(cmd, "expect %15s %15s %f", a, b, &v) == 3 && (!strcmp(a, "x") || !strcmp(a, "y") ||
                                                                       !strcmp(a, "z"))) {
        float got = !pl ? 0 : a[0] == 'x' ? pl->pos.x : a[0] == 'y' ? pl->pos.y : pl->pos.z;
        int ok = b[0] == '<' ? got < v : got > v;
        test_pass += ok, test_fail += !ok;
        printf("TEST ev expect %s %s %.2f: %.2f %s\n", a, b, v, got, ok ? "PASS" : "FAIL");
    } else if (sscanf(cmd, "lot %d", &id) == 1) {  /* AwardItemLot (2003[36]) of an exported lot */
        award_lot(id);
    } else if (sscanf(cmd, "expect inv %i %d", &id, &val) == 2) {  /* item (ITEM_* | row, e.g. 0x40000128) count */
        int got = inv_count((u32)id), ok = got == val;
        test_pass += ok, test_fail += !ok;
        printf("TEST ev expect inv %08x %d: %d %s\n", (unsigned)id, val, got, ok ? "PASS" : "FAIL");
    } else if (!strcmp(cmd, "rest")) {  /* what a bonfire rest does to the events and the temporary flags */
        event_rest();
    } else if (sscanf(cmd, "expect respawn %d", &val) == 1) {
        int ok = (s32)prog.respawn == val;
        test_pass += ok, test_fail += !ok;
        printf("TEST ev expect respawn %d: %d %s\n", val, (int)prog.respawn, ok ? "PASS" : "FAIL");
    } else if (sscanf(cmd, "expect value %d %d %d", &id, &val, &want) == 3) {  /* EventValue first, bits */
        int got = (int)flag_value_get(id, val), ok = got == want;
        test_pass += ok, test_fail += !ok;
        printf("TEST ev expect value %d %d %d: %d %s\n", id, val, want, got, ok ? "PASS" : "FAIL");
    } else if (sscanf(cmd, "expect bossbar %d %d", &id, &val) == 2) {  /* slot, entity shown (-1 none) */
        int got = hud_boss_bar_entity(id), ok = got == val;
        test_pass += ok, test_fail += !ok;
        printf("TEST ev expect bossbar %d %d: %d %s\n", id, val, got, ok ? "PASS" : "FAIL");
    } else if (sscanf(cmd, "expect defeats %d", &val) == 1) {
        int got = ev_boss_defeats(), ok = got == val;
        test_pass += ok, test_fail += !ok;
        printf("TEST ev expect defeats %d: %d %s\n", val, got, ok ? "PASS" : "FAIL");
    } else if (sscanf(cmd, "expect eventanim %d", &val) == 1) {  /* an event anim plays on the player */
        int got = pl && player_event_anim_playing(pl), ok = got == val;
        test_pass += ok, test_fail += !ok;
        printf("TEST ev expect eventanim %d: %d %s\n", val, got, ok ? "PASS" : "FAIL");
    } else if (sscanf(cmd, "expect mapevents %d %d %d", &id, &val, &want) == 3) {
        /* map mAA_BB: its events run (instances in a live set) in world block `want` (-1: none run) */
        int got = -1, n = 0;
        for (int k = 1; k < EV_SETS; k++)
            if (sets[k].blob && sets[k].live && sets[k].h->map[0] == id && sets[k].h->map[1] == val) {
                got = sets[k].block;
                for (int i = 0; i < ninst; i++)
                    n += inst[i].state != I_FREE && inst[i].set == k;
            }
        if (!n)
            got = -1;
        int ok = want == -2 ? got >= 0 : got == want;  /* -2: in any block */
        test_pass += ok, test_fail += !ok;
        printf("TEST ev expect mapevents m%02d_%02d in block %d: block %d, %d instances %s\n", id, val, want, got, n,
               ok ? "PASS" : "FAIL");
    } else if (!strcmp(cmd, "expect settled")) {  /* nothing left to run behind the last warp's load (event-14) */
        int n = 0;
        for (int i = 0; i < ninst; i++)
            n += inst[i].state == I_RUN;
        test_pass += !n, test_fail += !!n;
        printf("TEST ev expect settled: %d instances still to run %s\n", n, n ? "FAIL" : "PASS");
    } else if (sscanf(cmd, "expect bonfires %d", &val) == 1) {
        int got = ev_bonfire_count(), ok = got == val;
        test_pass += ok, test_fail += !ok;
        printf("TEST ev expect bonfires %d: %d %s\n", val, got, ok ? "PASS" : "FAIL");
    } else if (sscanf(cmd, "expect region %d %f %f %f %d", &id, &fx, &v, &fz, &val) == 5) {
        /* region-local point (x along the width, y up from the origin, z along the depth) turned into the world by the
         * region's yaw: inside 0 / 1 (in_region, the point as feet) */
        Set *s = NULL;
        const EvEnt *e = ent_any(1, id, &s);
        int got = -1;
        if (e && e->kind == ENT_REGION) {
            float c = cosf(e->yaw), sn = sinf(e->yaw);  /* inverse of in_region's world -> local */
            float p[3] = {e->pos[0] + fx * c + fz * sn, e->pos[1] + v, e->pos[2] - fx * sn + fz * c};
            got = in_region(e, p);
        }
        int ok = got == val;
        test_pass += ok, test_fail += !ok;
        printf("TEST ev expect region %d %.2f %.2f %.2f %d: %d %s\n", id, fx, v, fz, val, got, ok ? "PASS" : "FAIL");
    } else if (ev_chr_test(cmd, &test_pass, &test_fail)) {  /* chr ... / expect chr ... (ev_chr.c) */
    } else if (bonfire_test_cmd(cmd, &test_pass, &test_fail)) {  /* hurt / expect estus, stain ... (bonfire.c) */
    } else if (ev_obj_test(cmd, &test_pass, &test_fail)) {  /* obj ... / expect objanim / objbroken (ev_obj.c) */
    } else if (sscanf(cmd, "expect %15s %d %d", a, &id, &val) == 3) {
        int found = 1, got;
        if (!strcmp(a, "flag"))
            got = flag_get(id);
        else if (!strcmp(a, "drawn"))  /* map chunks of a switched piece / object: 1 drawn (drawgrp.c) */
            found = (got = dg_entity_drawn(id)) >= 0;
        else if (!strcmp(a, "sfx"))  /* event-owned map SFX: 1 = particles alive (fx.c) */
            found = (got = fx_entity_particles(id, 0)) >= 0, got = got > 0;
        else
            got = find_state(a, id, &found);
        int ok = found && got == val;
        test_pass += ok, test_fail += !ok;
        printf("TEST ev expect %s %d %d: %d%s %s\n", a, id, val, got, found ? "" : " (not found)", ok ? "PASS" : "FAIL");
    } else {
        printf("TEST ev error: '%s'\n", cmd);
    }
    (void)c;
}

void event_status(char *buf, int n)
{
    snprintf(buf, n, "EVT I%d W%d F%d L%u S%d U%d %uK %dUS%s%s", event_stats.running, event_stats.waiting,
             event_stats.fired, event_stats.last_id, event_stats.stubs, event_stats.unsupported,
             event_stats.bytes >> 10, (int)event_stats.us, event_stats.prompt >= 0 ? " ACT" : "", ev_message_hud());
}
