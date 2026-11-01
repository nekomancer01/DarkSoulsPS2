/* Enemy AI (ai.h): DSR's goal system around the original Lua scripts (lua0.c).
 *
 * Engine contract as the scripts use it (aiCommon top_goal / common_logic_func / approach_target / team_*_help ...):
 * - The top goal (GOAL_COMMON_TopGoal) holds the goals the logic adds with ai:AddTopGoal. The logic function runs
 *   when the top goal has no subgoals left, and after a perception state change (old top goals are dropped first).
 * - A goal is activated (Name_Activate(ai, goal)) when it first reaches the front of its parent's list. Each frame the
 *   front subgoal is updated first: success pops it, failure clears the list and fails the parent. Then the goal's
 *   own Name_Update(ai, goal, dT) runs (not for REGISTER_GOAL_NO_UPDATE goals; REGISTER_GOAL_UPDATE_TIME(min, max)
 *   spaces the calls); GOAL_RESULT_Continue from a goal whose subgoals have all succeeded means success, and a
 *   NO_UPDATE goal succeeds as soon as its list is empty.
 * - Interrupts (INTERUPT_*) raised this frame are offered to the active goals from the deepest up to the top goal
 *   (Name_Interupt, skipped for REGISTER_GOAL_NO_INTERUPT goals) and then to the logic's interrupt function, until
 *   one returns true; ai:IsInterupt(code) tells which one is being offered.
 * - Native goals (no Lua body in the game; common_func.lua only registers them, CommonAttack's Lua is a stub) drive
 *   the character: Wait, Stay, MoveToSomewhere, Guard, SidewayMove, KeepDist, BackToHome, SpinStep, LeaveTarget,
 *   ApproachStep, Parry, CommonAttack. Their parameter meanings come from how the scripts call them (TECH_DEBT
 *   enemy-1 lists what is assumed).
 * Perception (not in the scripts): sight = eye_dist and the eye_angX cone, hearing = a sound (ai_sound) within the
 * sound's radius and ear_dist; seen -> battle state, heard only -> caution; forgotten after Sight/SoundTargetForgetTime
 * (NpcThinkParam). The scripts' back-home rules (maxBackhomeDist / backhomeDist / backhomeBattleDist from home) end
 * a chase. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ai.h"
#include "lua0.h"

#define PI 3.14159265f
#define TAG_AI 1
#define TAG_GOAL 2
#define MAX_PARAMS 14
#define POOL 768
#define MAX_REGS 160
#define MAX_LOGICS 48
#define MAX_TEAM 32
#define MAX_SOUNDS 8
#define INF_LIFE 9999.0f  /* GetLife() of a goal added with life < 0 (never runs out) */

enum { R_FAILED = -1, R_CONTINUE = 0, R_SUCCESS = 1 };

/* ai_define.lua constants (fixed in DS1's scripts) */
enum { T_NONE = -2, T_SELF = -1, T_ENE0 = 0, T_FRI0 = 10, T_EVENT = 20, T_LOCALPLAYER = 21, P_INITIAL = 100,
       P_MOVE_POINT = 103, P_AI_FIXED = 107, P_INIT_POSE = 111, P_CURR_REQ = 114, P_LAST_SIGHT = 118 };
enum { DIR_CENTER, DIR_F, DIR_B, DIR_L, DIR_R };
enum { I_FindEnemy = 0, I_FindAttack = 1, I_Damaged = 2, I_Inside_ObserveArea = 16, I_ForgetTarget = 18,
       I_LoseSightTarget = 31 };
enum { X_maxBackhomeDist = 1, X_backhomeDist, X_backhomeBattleDist, X_nonBattleActLife, X_BattleStartDist,
       X_bMoveOnHearSound, X_CannotMoveAction, X_battleGoalID, X_callHelp_IsCall = 12, X_callHelp_IsReply,
       X_callHelp_MyPeerId, X_callHelp_CallPeerId, X_callHelp_DelayTime, X_callHelp_CallActionId };

enum { N_LUA, N_WAIT, N_STAY, N_MOVE, N_GUARD, N_SIDEWAY, N_KEEPDIST, N_BACKHOME, N_SPINSTEP, N_LEAVE, N_APPSTEP,
       N_PARRY, N_ATTACK, N_UNKNOWN };
static const char *const NATIVE[] = {"", "Wait", "Stay", "MoveToSomewhere", "Guard", "SidewayMove", "KeepDist",
                                     "BackToHome", "SpinStep", "LeaveTarget", "ApproachStep", "Parry", "CommonAttack"};

typedef struct {
    int id, native;
    char name[40];
    LVal act, upd, term, intr;
    unsigned char resolved, no_update, no_interupt, combo_cancel;
    float ut_min, ut_max;
} GoalReg;

struct Goal {
    Ai *ai;
    GoalReg *reg;
    int id, np, in_use;
    float life, age, update_wait;
    LVal p[MAX_PARAMS];
    Goal *sub, *next, *parent;
    unsigned char activated, had_sub, terminated;
    int last_sub_result;
    float timer[4], number[4];
    int team_rec;            /* AddGoalScopedTeamRecord type while this goal lives, -1 none */
    /* native state */
    int st, serial;
    float t, acc;
};

typedef struct { int id; char fn[48], intr[48]; LVal f, i; int resolved; } LogicReg;

static GoalReg regs[MAX_REGS];
static int nregs;
static LogicReg logics[MAX_LOGICS];
static int nlogics;
static Goal pool[POOL];
static Goal *free_list;
static int scripts_ok;
static Ai *team[MAX_TEAM];
static V3 team_pos[MAX_TEAM];
static int nteam;
static struct { V3 pos; float r; } sounds[MAX_SOUNDS];
static int nsounds;
static Ai *cur;              /* the AI being updated */
static int fallback_seen[64];
unsigned ai_lua_heap, ai_lua_instr;
int ai_lua_errors;
char ai_last_error[224];

/* ---- helpers ---- */

static float wrap(float a)
{
    while (a > PI)
        a -= 2 * PI;
    while (a < -PI)
        a += 2 * PI;
    return a;
}

static unsigned rnd_u(Ai *a)
{
    a->rng = a->rng * 1103515245u + 12345u;
    return a->rng >> 8;
}

static float yaw_to(V3 from, V3 to) { return atan2f(to.x - from.x, to.z - from.z); }

static float dist_xz(V3 a, V3 b)
{
    float dx = a.x - b.x, dz = a.z - b.z;
    return sqrtf(dx * dx + dz * dz);
}

static float dist3(V3 a, V3 b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static Ai *friend_of(Ai *a)
{
    if (a->reply_to >= 0 && a->reply_to < nteam)
        return team[a->reply_to];
    Ai *best = NULL;
    float bd = 1e9f;
    for (int i = 0; i < nteam; i++)
        if (team[i] != a && team[i]->f) {
            float d = dist_xz(team_pos[i], a->f->pos);
            if (d < bd)
                bd = d, best = team[i];
        }
    return best;
}

/* Position (and facing) of a TARGET_* / POINT_* id */
static V3 target_pos(Ai *a, int t, float *yaw)
{
    const AiFrame *f = a->f;
    float y = f->yaw;
    V3 p = f->pos;
    switch (t) {
    case T_ENE0:
    case T_LOCALPLAYER: p = f->target, y = f->target_yaw; break;
    case T_FRI0: {
        Ai *fr = friend_of(a);
        if (fr && fr->f)
            p = fr->f->pos, y = fr->f->yaw;
        break;
    }
    case P_INITIAL:
    case P_MOVE_POINT:
    case P_AI_FIXED:
    case P_INIT_POSE: p = f->home, y = f->home_yaw; break;
    case P_LAST_SIGHT: p = a->last_sight; break;
    default: break;
    }
    if (yaw)
        *yaw = y;
    return p;
}

/* Angle of a target relative to the self's facing (radians, + = left as game yaw grows) */
static float rel_angle(Ai *a, V3 p) { return wrap(yaw_to(a->f->pos, p) - a->f->yaw); }

static int lint(LVal v) { return v.t == LT_NUM ? (int)v.u.n : v.t == LT_BOOL ? v.u.b : 0; }
static float lflt(LVal v) { return v.t == LT_NUM ? v.u.n : v.t == LT_BOOL ? (float)v.u.b : 0; }
#define ARGN(i) (n > (i) ? lflt(a[i]) : 0.0f)
#define ARGI(i) (n > (i) ? lint(a[i]) : 0)
#define ARGB(i) (n > (i) && ltruthy(a[i]))
#define SELF_AI (a[0].tag == TAG_GOAL ? ((Goal *)a[0].u.p)->ai : (Ai *)a[0].u.p)
#define SELF_GOAL (a[0].tag == TAG_GOAL ? (Goal *)a[0].u.p : NULL)
#define RET_NUM(x) return (o[0] = lnum((float)(x)), 1)
#define RET_BOOL(x) return (o[0] = lbool(x), 1)

/* ---- goal registry ---- */

static GoalReg *reg_find(int id)
{
    for (int i = 0; i < nregs; i++)
        if (regs[i].id == id)
            return &regs[i];
    return NULL;
}

static GoalReg *reg_get(int id)
{
    GoalReg *r = reg_find(id);
    if (r || nregs >= MAX_REGS)
        return r;
    r = &regs[nregs++];
    memset(r, 0, sizeof *r);
    r->id = id;
    r->native = N_UNKNOWN;
    return r;
}

static LVal fn_of(const char *name, const char *suffix)
{
    char b[64];
    snprintf(b, sizeof b, "%s_%s", name, suffix);
    return lua0_getglobal(b);
}

static void reg_resolve(GoalReg *r)
{
    if (r->resolved)
        return;
    r->resolved = 1;
    if (r->native != N_LUA)
        return;
    r->act = fn_of(r->name, "Activate"), r->upd = fn_of(r->name, "Update");
    r->term = fn_of(r->name, "Terminate"), r->intr = fn_of(r->name, "Interupt");
    if (r->act.t == LT_NIL && r->upd.t == LT_NIL)
        r->native = N_UNKNOWN;  /* registered but no body anywhere (e.g. a goal of another map's luabnd) */
}

/* ---- goal pool ---- */

static Goal *goal_new(Ai *ai, Goal *parent, int id, float life, const LVal *params, int np)
{
    Goal *g = free_list;
    if (!g)
        lua0_error("goal pool exhausted");
    free_list = g->next;
    memset(g, 0, sizeof *g);
    g->in_use = 1;
    g->ai = ai, g->parent = parent, g->id = id;
    g->reg = reg_get(id);
    if (g->reg)
        reg_resolve(g->reg);
    g->life = life;
    g->team_rec = -1;
    g->np = np > MAX_PARAMS ? MAX_PARAMS : np;
    for (int i = 0; i < g->np; i++)
        g->p[i] = params[i];
    for (int i = 0; i < 4; i++)
        g->timer[i] = -1;
    return g;
}

static int call_lua(Ai *a, LVal f, Goal *g, int with_dt, LVal *ret)
{
    if (f.t == LT_NIL)
        return 0;
    LVal args[3] = {lud(a, TAG_AI), lud(g, TAG_GOAL), lnum(a->f ? a->f->dt : 0)};
    LVal r = LNIL;
    void *prev = a->cur_goal;
    a->cur_goal = g;
    int n = lua0_call(f, args, with_dt ? 3 : 2, &r, 1);
    a->cur_goal = prev;
    if (n < 0) {
        a->errors++, ai_lua_errors++;
        snprintf(ai_last_error, sizeof ai_last_error, "%s: %s", g && g->reg ? g->reg->name : "logic", lua0_errmsg);
        if (ai_lua_errors < 20)
            printf("ai: %s\n", ai_last_error);
        return -1;
    }
    if (ret)
        *ret = r;
    return 1;
}

static void goal_free(Goal *g);

static void goal_clear_subs(Goal *g)
{
    Goal *s = g->sub;
    g->sub = NULL;
    while (s) {
        Goal *nx = s->next;
        goal_free(s);
        s = nx;
    }
}

/* Terminate (Lua) and release a goal and its subgoals */
static void goal_free(Goal *g)
{
    goal_clear_subs(g);
    if (g->activated && !g->terminated && g->reg && g->reg->native == N_LUA) {
        g->terminated = 1;
        call_lua(g->ai, g->reg->term, g, 0, NULL);
    }
    if (g->ai->ez_goal == g)
        g->ai->ez_goal = NULL, g->ai->ez_anim = -1;
    g->in_use = 0;
    g->next = free_list;
    free_list = g;
}

static Goal *goal_add(Goal *parent, int id, float life, const LVal *params, int np, int front)
{
    Goal *g = goal_new(parent->ai, parent, id, life, params, np);
    parent->had_sub = 1;
    if (front || !parent->sub) {
        g->next = parent->sub;
        parent->sub = g;
    } else {
        Goal *s = parent->sub;
        while (s->next)
            s = s->next;
        s->next = g;
    }
    return g;
}

/* ---- native goals ---- */

static void face_point(Ai *a, V3 p)
{
    a->out.face = 1;
    a->out.face_yaw = yaw_to(a->f->pos, p);
}

static void face_target(Ai *a, int t)
{
    if (t == T_SELF || t == T_NONE)
        return;
    face_point(a, target_pos(a, t, NULL));
}

static void move_dir(Ai *a, float yaw, int walk, int turn_target)
{
    a->out.move = 1;
    a->out.run = !walk || a->dash;
    a->out.move_yaw = yaw;
    if (turn_target == T_SELF || turn_target == T_NONE) {
        a->out.face = 1;
        a->out.face_yaw = yaw;
    } else
        face_target(a, turn_target);
}

static int life_over(const Goal *g) { return g->life >= 0 && g->life <= 0; }

static int native_update(Ai *a, Goal *g)
{
    const AiFrame *f = a->f;
    LVal *P = g->p;
    int np = g->np;
#define PI_(i) (np > (i) ? lint(P[i]) : 0)
#define PF_(i) (np > (i) ? lflt(P[i]) : 0.0f)
#define PB_(i) (np > (i) && ltruthy(P[i]))
    switch (g->reg ? g->reg->native : N_UNKNOWN) {
    case N_WAIT:   /* Wait(life, turn target): stand still */
        if (np > 0 && PI_(0) != T_SELF)
            face_target(a, PI_(0));
        return life_over(g) ? R_SUCCESS : R_CONTINUE;
    case N_STAY:   /* Stay(life, ?, turn target) */
        face_target(a, PI_(1));
        return life_over(g) ? R_SUCCESS : R_CONTINUE;
    case N_MOVE: { /* MoveToSomewhere(life, target, dir type, arrival dist, turn target, walk) */
        V3 p = target_pos(a, PI_(0), NULL);
        float arrive = PF_(2) > 0.3f ? PF_(2) : 0.3f;
        if (dist_xz(f->pos, p) <= arrive)
            return R_SUCCESS;
        if (life_over(g))
            return R_SUCCESS;
        move_dir(a, yaw_to(f->pos, p), PB_(4), np > 3 ? PI_(3) : T_SELF);
        return R_CONTINUE;
    }
    case N_GUARD:  /* Guard(life, guard anim, target, ...) */
        a->out.guard = PI_(0) > 0 ? PI_(0) : 9910;
        face_target(a, np > 1 ? PI_(1) : T_ENE0);
        return life_over(g) ? R_SUCCESS : R_CONTINUE;
    case N_SIDEWAY: {  /* SidewayMove(life, target, right?, angle, face target?, walk?, guard anim) */
        V3 p = target_pos(a, PI_(0), NULL);
        float to = yaw_to(f->pos, p), side = PB_(1) ? -PI / 2 : PI / 2;
        if (g->st == 0)
            g->st = 1, g->acc = 0, g->t = to;
        g->acc += fabsf(wrap(to - g->t));
        g->t = to;
        if (life_over(g) || (PF_(2) > 0 && g->acc >= PF_(2) * (PI / 180)))
            return R_SUCCESS;
        move_dir(a, wrap(to + side), np > 4 ? PB_(4) : 1, PB_(3) || np <= 3 ? PI_(0) : T_SELF);
        if (np > 5 && PI_(5) > 0)
            a->out.guard = PI_(5);
        return R_CONTINUE;
    }
    case N_KEEPDIST: { /* KeepDist(life, target, min, max, turn target, walk) */
        V3 p = target_pos(a, PI_(0), NULL);
        float d = dist_xz(f->pos, p), to = yaw_to(f->pos, p);
        if ((d >= PF_(1) && d <= PF_(2)) || life_over(g))
            return R_SUCCESS;
        move_dir(a, d < PF_(1) ? wrap(to + PI) : to, PB_(4), np > 3 ? PI_(3) : PI_(0));
        return R_CONTINUE;
    }
    case N_BACKHOME: { /* BackToHome(life, ...): walk to the MSB position, then face its direction */
        float d = dist_xz(f->pos, f->home);
        if (life_over(g))
            return R_SUCCESS;
        if (d > 0.5f) {
            move_dir(a, yaw_to(f->pos, f->home), 1, T_SELF);
            return R_CONTINUE;
        }
        a->out.face = 1, a->out.face_yaw = f->home_yaw;
        return fabsf(wrap(f->yaw - f->home_yaw)) < 0.2f ? R_SUCCESS : R_CONTINUE;
    }
    case N_SPINSTEP:   /* SpinStep(life, step anim, ...) */
    case N_APPSTEP:    /* ApproachStep(life, target?, anim?): the anim is the first param >= 700 */
    case N_PARRY:      /* Parry(life, anim?) */
    case N_ATTACK: {   /* CommonAttack(life, anim, target, dist type, success angle, turn time, turn face angle,
                        * combo?, move cancel?, guard break?, turn?, angle up, angle down) */
        int native = g->reg->native, anim = PI_(0), tgt = native == N_ATTACK ? PI_(1) : T_ENE0;
        if (native == N_APPSTEP)
            for (int i = 0; i < np; i++)
                if (lint(P[i]) >= 700) {
                    anim = lint(P[i]);
                    break;
                }
        if (anim <= 0)
            return life_over(g) ? R_SUCCESS : R_CONTINUE;
        int combo = native == N_ATTACK && PB_(6);
        if (g->st == 0) {  /* turn first (turn time / face angle), unless continuing a combo */
            if (native == N_ATTACK && tgt != T_NONE && !(combo && f->anim >= 3000)) {
                float rel = fabsf(rel_angle(a, target_pos(a, tgt, NULL))), face = np > 5 ? PF_(5) : 20;
                face_target(a, tgt);
                if (rel > face * (PI / 180) && g->t < (np > 4 ? PF_(4) : 1.5f)) {
                    g->t += f->dt;
                    return R_CONTINUE;
                }
            }
            g->st = 1, g->t = 0, g->serial = -1;
        }
        if (g->st == 1) {  /* request until the ESD starts the anim (a new action anim with this id) */
            a->out.req = anim;
            if (tgt != T_NONE && native == N_ATTACK)
                face_target(a, tgt);
            if (f->anim == anim && f->anim_t < 0.1f + f->dt) {
                g->st = 2;
                return R_CONTINUE;
            }
            g->t += f->dt;
            /* not taken (no such anim / the ESD is busy with a hit reaction) */
            return g->t > 1.0f || (life_over(g) && g->t > 0.3f) ? R_FAILED : R_CONTINUE;
        }
        /* playing: done when it ends, or for a combo link once the next attack can be requested */
        if (f->anim != anim || f->anim_t >= f->anim_len)
            return R_SUCCESS;
        if (combo && f->combo_window && g->next && g->next->reg && g->next->reg->combo_cancel)
            return R_SUCCESS;
        if (combo && f->combo_window && g->parent && g->parent->next && g->parent->next->reg &&
            g->parent->next->reg->combo_cancel)
            return R_SUCCESS;
        if (tgt != T_NONE && native == N_ATTACK)
            face_target(a, tgt);
        return R_CONTINUE;
    }
    case N_LEAVE: {    /* LeaveTarget(life, target, dist, turn target, walk, guard anim) */
        V3 p = target_pos(a, PI_(0), NULL);
        float d = dist_xz(f->pos, p);
        if (d >= PF_(1) || life_over(g))
            return R_SUCCESS;
        move_dir(a, wrap(yaw_to(f->pos, p) + PI), PB_(3), np > 2 ? PI_(2) : PI_(0));
        if (np > 4 && PI_(4) > 0)
            a->out.guard = PI_(4);
        return R_CONTINUE;
    }
    default:           /* a goal id with no body (e.g. Frampt's 533000_battle, not in this map's luabnd) */
        if (g->life < 0)
            g->life = 1.0f;
        return life_over(g) ? R_SUCCESS : R_CONTINUE;
    }
#undef PI_
#undef PF_
#undef PB_
}

/* ---- goal update ---- */

static int goal_update(Ai *a, Goal *g)
{
    const AiFrame *f = a->f;
    GoalReg *r = g->reg;
    int native = r ? r->native : N_UNKNOWN;
    if (!g->activated) {
        g->activated = 1;
        if (native == N_LUA && call_lua(a, r->act, g, 0, NULL) < 0)
            return R_FAILED;
        if (r && r->ut_max > 0)
            g->update_wait = 0;
    }
    g->age += f->dt;
    if (g->life >= 0)
        g->life = g->life > f->dt ? g->life - f->dt : 0;
    for (int i = 0; i < 4; i++)
        if (g->timer[i] > 0)
            g->timer[i] = g->timer[i] > f->dt ? g->timer[i] - f->dt : 0;
    if (native != N_LUA)
        return native_update(a, g);
    int res = R_CONTINUE;
    if (g->sub) {
        Goal *s = g->sub;
        int sr = goal_update(a, s);
        if (sr != R_CONTINUE)
            g->last_sub_result = sr;
        if (sr == R_SUCCESS && g->sub == s) {
            g->sub = s->next;
            goal_free(s);
        } else if (sr == R_FAILED) {
            goal_clear_subs(g);
            res = R_FAILED;
        }
    }
    if (res == R_FAILED)
        return res;
    if (!r->no_update && r->upd.t != LT_NIL) {
        int call = 1;
        if (r->ut_max > 0) {
            g->update_wait -= f->dt;
            call = g->update_wait <= 0;
            if (call)
                g->update_wait = r->ut_min + (r->ut_max - r->ut_min) * (rnd_u(a) % 1000) / 1000.0f;
        }
        if (call) {
            LVal rv = LNIL;
            if (call_lua(a, r->upd, g, 1, &rv) < 0)
                return R_FAILED;
            int v = lint(rv);
            if (v == R_SUCCESS || v == R_FAILED)
                return v;
        }
        return g->had_sub && !g->sub ? R_SUCCESS : R_CONTINUE;
    }
    return g->sub ? R_CONTINUE : R_SUCCESS;
}

static Goal *deepest(Goal *g)
{
    while (g->sub)
        g = g->sub;
    return g;
}

static void dispatch_interupt(Ai *a, int code)
{
    a->interupt = code;
    for (Goal *g = deepest(a->root); g; g = g->parent) {
        GoalReg *r = g->reg;
        if (!r || r->native != N_LUA || r->no_interupt || r->intr.t == LT_NIL || !g->activated)
            continue;
        LVal rv = LNIL;
        if (call_lua(a, r->intr, g, 0, &rv) > 0 && ltruthy(rv)) {
            a->interupt = -1;
            return;
        }
    }
    for (int i = 0; i < nlogics; i++)
        if (logics[i].id == a->th->logic && logics[i].i.t != LT_NIL) {
            call_lua(a, logics[i].i, a->root, 0, NULL);
            break;
        }
    a->interupt = -1;
}

static LogicReg *logic_of(int id)
{
    for (int i = 0; i < nlogics; i++)
        if (logics[i].id == id) {
            LogicReg *l = &logics[i];
            if (!l->resolved)
                l->resolved = 1, l->f = lua0_getglobal(l->fn), l->i = lua0_getglobal(l->intr);
            return l;
        }
    return NULL;
}

/* ---- perception ---- */

static void perceive(Ai *a)
{
    const AiFrame *f = a->f;
    const AiThink *th = a->th;
    float dt = f->dt;
    a->seen_t += dt, a->heard_t += dt;
    float d = dist3(f->pos, f->target);
    int seen = 0, heard = 0;
    if (f->target_alive) {
        float rel = fabsf(rel_angle(a, f->target));
        seen = d <= th->eye_dist && rel <= th->eye_ang_x * (PI / 360);
        for (int i = 0; i < nsounds; i++) {
            float sd = dist3(f->pos, sounds[i].pos);
            if (sd <= sounds[i].r && sd <= th->ear_dist)
                heard = 1, a->sound_pos = sounds[i].pos;
        }
        if (f->damaged)
            seen = 1;
    }
    int was_seen = a->seen_t < 0.2f;
    if (seen)
        a->seen_t = 0, a->last_sight = f->target;
    if (heard)
        a->heard_t = 0;
    if (was_seen && !seen && a->state == AIS_BATTLE)
        a->raised |= 1u << I_LoseSightTarget;
    int st;
    if (!f->target_alive)
        st = AIS_NORMAL, a->seen_t = a->heard_t = 1e6f;
    else if (a->seen_t <= th->sight_forget)
        st = AIS_BATTLE;
    else if (a->heard_t <= th->sound_forget)
        st = AIS_CAUTION;
    else
        st = AIS_NORMAL;
    a->change = st != a->state;
    if (a->change) {
        if (st == AIS_BATTLE)
            a->raised |= 1u << I_FindEnemy;
        if (a->state == AIS_BATTLE && st == AIS_NORMAL)
            a->raised |= 1u << I_ForgetTarget;
        a->prev_state = a->state;
        a->state = st;
    }
    if (f->damaged)
        a->raised |= 1u << I_Damaged;
    /* the target starting an attack nearby (dodges / guards in the battle goals' interrupts) */
    if (f->target_attack >= 0 && f->target_attack != a->prev_target_attack && d < 8 && a->state == AIS_BATTLE)
        a->raised |= 1u << I_FindAttack;
    a->prev_target_attack = f->target_attack;
    for (int i = 0; i < 4; i++) {
        if (!a->observe[i].on)
            continue;
        V3 p = target_pos(a, a->observe[i].target, NULL);
        float rel = rel_angle(a, p), dir = a->observe[i].dir == DIR_B ? PI : a->observe[i].dir == DIR_L ? PI / 2
                                                                        : a->observe[i].dir == DIR_R ? -PI / 2 : 0;
        int in = dist_xz(f->pos, p) <= a->observe[i].dist &&
                 fabsf(wrap(rel - dir)) <= a->observe[i].angle * (PI / 360);
        if (in && !a->observe[i].inside)
            a->raised |= 1u << I_Inside_ObserveArea;
        a->observe[i].inside = in;
    }
}

/* ---- the frame ---- */

void ai_init(Ai *a, const AiThink *th, int index, unsigned seed)
{
    memset(a, 0, sizeof *a);
    a->th = th, a->index = index, a->reply_to = -1;
    a->rng = seed * 2654435761u + 1;
    a->seen_t = a->heard_t = 1e6f;
    a->interupt = -1;
    a->prev_target_attack = -1;
    a->ez_anim = -1;
    a->out.req = a->out.guard = -1;
    for (int i = 0; i < 8; i++)
        a->timer[i] = -1;
}

void ai_reset(Ai *a)
{
    if (a->root) {
        goal_free(a->root);
        a->root = NULL;
    }
    a->out = (AiOut){.req = -1, .guard = -1};
}

int ai_goals_in_use(void)
{
    int n = 0;
    for (int i = 0; i < POOL; i++)
        n += pool[i].in_use;
    return n;
}

void ai_update(Ai *a, const AiFrame *f)
{
    a->f = f;
    a->out.req = -1, a->out.move = a->out.run = a->out.face = 0;
    a->out.guard = -1;
    if (!scripts_ok)
        return;
    cur = a;
    for (int i = 0; i < 8; i++)
        if (a->timer[i] > 0)
            a->timer[i] = a->timer[i] > f->dt ? a->timer[i] - f->dt : 0;
    if (!a->root) {
        GoalReg *top = NULL;
        for (int i = 0; i < nregs; i++)
            if (!strcmp(regs[i].name, "TopGoal"))
                top = &regs[i];
        a->root = goal_new(a, NULL, top ? top->id : -1, -1, NULL, 0);
        a->root->activated = 1;
    }
    a->raised = 0;
    perceive(a);
    LogicReg *lg = logic_of(a->th->logic);
    if (a->change && a->root->sub)
        goal_clear_subs(a->root);  /* replan on a state change */
    for (int code = 0; code < 32; code++)
        if (a->raised & (1u << code))
            dispatch_interupt(a, code);
    if (!a->root->sub && lg && lg->f.t != LT_NIL) {
        LVal arg = lud(a, TAG_AI);
        if (lua0_call(lg->f, &arg, 1, NULL, 0) < 0) {
            a->errors++, ai_lua_errors++;
            snprintf(ai_last_error, sizeof ai_last_error, "logic %s: %s", lg->fn, lua0_errmsg);
            if (ai_lua_errors < 20)
                printf("ai: %s\n", ai_last_error);
        }
    }
    if (a->ez_goal && a->ez_life > 0) {  /* DoEzAction guard held for its life while the calling goal lives */
        a->out.guard = a->ez_anim;
        a->ez_life -= f->dt;
    }
    if (a->root->sub) {
        Goal *s = a->root->sub;
        int r = goal_update(a, s);
        if (r != R_CONTINUE && a->root->sub == s) {
            a->root->sub = s->next;
            goal_free(s);
        }
    }
    a->change = 0;
    /* IsStartAttack / IsFinishAttack bookkeeping for SetAttackRequest (TeamCallHelp) */
    if (a->attack_req > 0 && f->anim == a->attack_req && f->anim_t < 0.1f + f->dt)
        a->attack_started = 1;
    if (a->attack_started && (f->anim != a->attack_req || f->anim_t >= f->anim_len))
        a->attack_finished = 1;
    cur = NULL;
}

const char *ai_goal_name(const Ai *a)
{
    static const char *const ST[] = {"NORMAL", "CAUTION", "FIND", "BATTLE"};
    if (!a->root || !a->root->sub)
        return ST[a->state & 3];
    const Goal *g = deepest(a->root);
    return g->reg && g->reg->name[0] ? g->reg->name : "?";
}

void ai_sound(V3 pos, float radius)
{
    if (nsounds < MAX_SOUNDS)
        sounds[nsounds].pos = pos, sounds[nsounds].r = radius, nsounds++;
}

void ai_team(Ai **all, const V3 *pos, int n)
{
    nteam = n > MAX_TEAM ? MAX_TEAM : n;
    for (int i = 0; i < nteam; i++)
        team[i] = all[i], team_pos[i] = pos[i];
}

static void mark_goals(void)
{
    for (int i = 0; i < POOL; i++)
        if (pool[i].in_use)
            for (int k = 0; k < pool[i].np; k++)
                lua0_mark(pool[i].p[k]);
    for (int i = 0; i < nregs; i++)
        lua0_mark(regs[i].act), lua0_mark(regs[i].upd), lua0_mark(regs[i].term), lua0_mark(regs[i].intr);
    for (int i = 0; i < nlogics; i++)
        lua0_mark(logics[i].f), lua0_mark(logics[i].i);
}

void ai_end_frame(void)
{
    nsounds = 0;
    if (scripts_ok && lua0_since_gc > 192 * 1024)
        lua0_gc(mark_goals);
    ai_lua_heap = lua0_heap, ai_lua_instr = lua0_instructions;
}

/* ---- engine globals (REGISTER_*, loadstring) ---- */

static int f_register_goal(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    GoalReg *r = reg_get(ARGI(0));
    const char *name = n > 1 ? lua0_cstr(a[1]) : NULL;
    if (!r || !name)
        return 0;
    snprintf(r->name, sizeof r->name, "%s", name);
    r->native = N_LUA;
    for (int i = 1; i < N_UNKNOWN; i++)
        if (!strcmp(name, NATIVE[i]))
            r->native = i;
    return 0;
}

static int f_no_update(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    GoalReg *r = reg_get(ARGI(0));
    if (r)
        r->no_update = ARGB(1);
    return 0;
}

static int f_no_interupt(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    GoalReg *r = reg_get(ARGI(0));
    if (r)
        r->no_interupt = ARGB(1);
    return 0;
}

static int f_update_time(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    GoalReg *r = reg_get(ARGI(0));
    if (r)
        r->ut_min = ARGN(1), r->ut_max = ARGN(2);
    return 0;
}

static int f_combo_cancel(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    GoalReg *r = reg_get(ARGI(0));
    if (r)
        r->combo_cancel = 1;
    return 0;
}

static int f_register_logic(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    if (nlogics >= MAX_LOGICS || n < 2)
        return 0;
    LogicReg *l = &logics[nlogics++];
    memset(l, 0, sizeof *l);
    l->id = ARGI(0);
    snprintf(l->fn, sizeof l->fn, "%s", lua0_cstr(a[1]) ? lua0_cstr(a[1]) : "");
    snprintf(l->intr, sizeof l->intr, "%s", n > 2 && lua0_cstr(a[2]) ? lua0_cstr(a[2]) : "");
    return 0;
}

static int f_nop(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)a, (void)n, (void)o, (void)up;
    return 0;
}

/* loadstring: the only use (if.lua) builds "return function (arg) NAME(arg.ai, arg.goal, arg.codeNo) end".
 * Returns a chunk whose call gives a function calling the global NAME with those three fields. */
static int f_if_call(LVal *a, int n, LVal *o, const LVal *up)
{
    if (n < 1 || a[0].t != LT_TABLE)
        return 0;
    LVal args[3] = {lua0_get(a[0], lua0_str("ai")), lua0_get(a[0], lua0_str("goal")), lua0_get(a[0], lua0_str("codeNo"))};
    LVal fn = lua0_getglobal(lua0_cstr(*up));
    int r = lua0_call(fn, args, 3, o, 8);
    if (r < 0)
        lua0_error("%s", lua0_errmsg);
    return r;
}

static int f_chunk(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)a, (void)n;
    o[0] = lua0_cfunc(f_if_call, *up);
    return 1;
}

static int f_loadstring(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    const char *s = n > 0 ? lua0_cstr(a[0]) : NULL, *k = s ? strstr(s, "(arg) ") : NULL;
    if (!k) {
        o[0] = LNIL;
        return 1;
    }
    k += 6;
    char name[64];
    int i = 0;
    while (k[i] && k[i] != '(' && i < 63)
        name[i] = k[i], i++;
    name[i] = 0;
    o[0] = lua0_cfunc(f_chunk, lua0_str(name));
    return 1;
}

/* ---- ai / goal methods ---- */

static int m_GetDist(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Ai *ai = SELF_AI;
    RET_NUM(dist3(ai->f->pos, target_pos(ai, ARGI(1), NULL)));
}

static int m_GetDistXZ(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Ai *ai = SELF_AI;
    RET_NUM(dist_xz(ai->f->pos, target_pos(ai, ARGI(1), NULL)));
}

static int m_GetDistY(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Ai *ai = SELF_AI;
    RET_NUM(fabsf(target_pos(ai, ARGI(1), NULL).y - ai->f->pos.y));
}

static int m_GetDistYSigned(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Ai *ai = SELF_AI;
    RET_NUM(target_pos(ai, ARGI(1), NULL).y - ai->f->pos.y);
}

static int m_GetRandam_Int(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Ai *ai = SELF_AI;
    int lo = ARGI(1), hi = ARGI(2);
    RET_NUM(hi > lo ? lo + (int)(rnd_u(ai) % (unsigned)(hi - lo + 1)) : lo);
}

static int m_GetRandam_Float(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Ai *ai = SELF_AI;
    float lo = ARGN(1), hi = ARGN(2);
    RET_NUM(lo + (hi - lo) * (rnd_u(ai) % 10001) / 10000.0f);
}

/* IsInsideTarget(target, dir, angle): target within the full angle around the self's front / back / left / right */
static int inside(Ai *ai, int t, int dir, float angle)
{
    float rel = rel_angle(ai, target_pos(ai, t, NULL));
    float c = dir == DIR_B ? PI : dir == DIR_L ? PI / 2 : dir == DIR_R ? -PI / 2 : 0;
    return fabsf(wrap(rel - c)) <= angle * (PI / 360);
}

static int m_IsInsideTarget(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    RET_BOOL(inside(SELF_AI, ARGI(1), ARGI(2), ARGN(3)));
}

/* IsInsideTargetEx(target, self or target origin, dir, angle, dist) */
static int m_IsInsideTargetEx(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Ai *ai = SELF_AI;
    RET_BOOL(inside(ai, ARGI(1), ARGI(3), ARGN(4)) && dist_xz(ai->f->pos, target_pos(ai, ARGI(1), NULL)) <= ARGN(5));
}

static int m_TurnTo(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    Ai *ai = SELF_AI;
    face_target(ai, n > 1 ? ARGI(1) : T_ENE0);
    return 0;
}

/* IsLookToTarget() / (angle) / (target, angle): the self faces the target within the angle (default 10 deg) */
static int m_IsLookToTarget(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Ai *ai = SELF_AI;
    int t = T_ENE0;
    float ang = 10;
    if (n == 2)
        ang = ARGN(1);
    else if (n >= 3)
        t = ARGI(1), ang = ARGN(2);
    float rel = fabsf(rel_angle(ai, target_pos(ai, t, NULL)));
    RET_BOOL(rel <= ang * (PI / 180) + 0.02f);
}

static int m_GetHpRate(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Ai *ai = SELF_AI;
    int t = ARGI(1);
    if (t == T_ENE0 || t == T_LOCALPLAYER)
        RET_NUM(ai->f->target_hp_max > 0 ? (float)ai->f->target_hp / ai->f->target_hp_max : 0);
    RET_NUM(ai->f->hp_max > 0 ? (float)ai->f->hp / ai->f->hp_max : 0);
}

static int m_IsBattleState(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_BOOL(SELF_AI->state == AIS_BATTLE); }
static int m_IsCautionState(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_BOOL(SELF_AI->state == AIS_CAUTION); }
static int m_IsFindState(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_BOOL(SELF_AI->state == AIS_FIND); }
static int m_IsChangeState(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_BOOL(SELF_AI->change); }

static int m_IsSearchTarget(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)n, (void)up;
    RET_BOOL(SELF_AI->state != AIS_NORMAL);
}

static int m_IsInterupt(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    RET_BOOL(SELF_AI->interupt >= 0 && SELF_AI->interupt == ARGI(1));
}

static int m_GetExcelParam(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Ai *ai = SELF_AI;
    int i = ARGI(1);
    RET_NUM(i >= 0 && i < AI_EXCEL_N ? ai->th->excel[i] : 0);
}

static int m_GetNpcThinkParamID(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_NUM(SELF_AI->th->think); }
static int m_GetBattleGoalId(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_NUM(SELF_AI->th->battle); }

static int m_GetDistParam(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    const AiThink *th = SELF_AI->th;
    int d = ARGI(1);
    RET_NUM(d == -1 ? th->near_d : d == -2 ? th->mid_d : d == -3 ? th->far_d : d == -4 ? th->out_d : 0);
}

/* Replanning (GetReplanningGoalAction / ID): NpcThinkParam goalAction_ToCaution / ToFind and their goal ids */
static int m_GetReplanningGoalAction(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)n, (void)up;
    Ai *ai = SELF_AI;
    RET_NUM(ai->state == AIS_CAUTION ? ai->th->goal_action_caution : ai->state == AIS_FIND ? ai->th->goal_action_find : 0);
}

static int m_GetReplanningGoalID(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)n, (void)up;
    Ai *ai = SELF_AI;
    RET_NUM(ai->state == AIS_CAUTION ? ai->th->goal_id_caution : ai->state == AIS_FIND ? ai->th->goal_id_find : 0);
}

static int m_GetMovePointNumber(LVal *a, int n, LVal *o, const LVal *up) { (void)a, (void)n, (void)up; RET_NUM(-1); }

static int m_GetMovePointEffectRange(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)n, (void)up;
    Ai *ai = SELF_AI;
    RET_NUM(dist_xz(ai->f->pos, ai->f->home));
}

static int add_goal(LVal *a, int n, LVal *o, Goal *parent, int front)
{
    if (!parent)
        return 0;
    Goal *g = goal_add(parent, ARGI(1), n > 2 ? ARGN(2) : -1, a + 3, n > 3 ? n - 3 : 0, front);
    o[0] = lud(g, TAG_GOAL);
    return 1;
}

static int m_AddTopGoal(LVal *a, int n, LVal *o, const LVal *up) { (void)up; return add_goal(a, n, o, SELF_AI->root, 0); }
static int m_AddSubGoal(LVal *a, int n, LVal *o, const LVal *up) { (void)up; return add_goal(a, n, o, SELF_GOAL, 0); }
static int m_AddSubGoal_Front(LVal *a, int n, LVal *o, const LVal *up) { (void)up; return add_goal(a, n, o, SELF_GOAL, 1); }

static int m_ClearSubGoal(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)n, (void)o, (void)up;
    Goal *g = SELF_GOAL;
    if (!g && SELF_AI->root)
        g = SELF_AI->root;
    if (g)
        goal_clear_subs(g);
    return 0;
}

static int m_HasTopSubgoal(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_BOOL(SELF_AI->root && SELF_AI->root->sub); }

static int has_goal(const Goal *g, int id)
{
    for (; g; g = g->next)
        if (g->id == id || has_goal(g->sub, id))
            return 1;
    return 0;
}

static int m_HasGoal(LVal *a, int n, LVal *o, const LVal *up) { (void)up; RET_BOOL(SELF_AI->root && has_goal(SELF_AI->root->sub, ARGI(1))); }

static int m_GetLife(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)n, (void)up;
    Goal *g = SELF_GOAL;
    RET_NUM(!g ? 0 : g->life < 0 ? INF_LIFE : g->life);
}

static int m_GetParam(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Goal *g = SELF_GOAL;
    int i = ARGI(1);
    o[0] = g && i >= 0 && i < g->np ? g->p[i] : LNIL;
    return 1;
}

static int m_IsExistParam(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Goal *g = SELF_GOAL;
    int i = ARGI(1);
    RET_BOOL(g && i >= 0 && i < g->np && g->p[i].t != LT_NIL);
}

static int m_GetSubGoalNum(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)n, (void)up;
    int c = 0;
    for (Goal *s = SELF_GOAL ? SELF_GOAL->sub : NULL; s; s = s->next)
        c++;
    RET_NUM(c);
}

static int m_GetLastSubGoalResult(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_NUM(SELF_GOAL ? SELF_GOAL->last_sub_result : 0); }

static float *timer_slot(LVal *a, int i)
{
    if (a[0].tag == TAG_GOAL)
        return i >= 0 && i < 4 ? &((Goal *)a[0].u.p)->timer[i] : NULL;
    return i >= 0 && i < 8 ? &((Ai *)a[0].u.p)->timer[i] : NULL;
}

static int m_SetTimer(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    float *t = timer_slot(a, ARGI(1));
    if (t)
        *t = ARGN(2);
    return 0;
}

static int m_IsFinishTimer(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    float *t = timer_slot(a, ARGI(1));
    RET_BOOL(!t || *t <= 0);
}

static float *number_slot(LVal *a, int i)
{
    if (a[0].tag == TAG_GOAL)
        return i >= 0 && i < 4 ? &((Goal *)a[0].u.p)->number[i] : NULL;
    return i >= 0 && i < 16 ? &((Ai *)a[0].u.p)->number[i] : NULL;
}

static int m_SetNumber(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    float *v = number_slot(a, ARGI(1));
    if (v)
        *v = ARGN(2);
    return 0;
}

static int m_GetNumber(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    float *v = number_slot(a, ARGI(1));
    RET_NUM(v ? *v : 0);
}

/* SetAttackRequest(anim): request an action this frame (TeamCallHelp's call action); IsStartAttack / IsFinishAttack */
static int m_SetAttackRequest(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    Ai *ai = SELF_AI;
    int anim = ARGI(1);
    if (anim > 0) {
        if (anim != ai->attack_req)
            ai->attack_req = anim, ai->attack_started = ai->attack_finished = 0;
        ai->out.req = anim;
    }
    return 0;
}

static int m_IsStartAttack(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_BOOL(SELF_AI->attack_started); }
static int m_IsFinishAttack(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_BOOL(SELF_AI->attack_finished); }

/* DoEzAction(life, anim): guard poses (99xx) are held over the movement, anything else is requested */
static int m_DoEzAction(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    Ai *ai = SELF_AI;
    int anim = ARGI(2);
    if (anim >= 9900 && anim < 10000)
        ai->ez_anim = anim, ai->ez_life = ARGN(1) > 0 ? ARGN(1) : INF_LIFE, ai->ez_goal = ai->cur_goal;
    else if (anim > 0)
        ai->out.req = anim;
    return 0;
}

static int m_IsEnableComboAttack(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_BOOL(SELF_AI->f->combo_window); }
static int m_IsEnableCancelMove(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_BOOL(SELF_AI->f->cancel_window); }
static int m_StartDash(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)o, (void)up; SELF_AI->dash = 1; return 0; }
static int m_EndDash(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)o, (void)up; SELF_AI->dash = 0; return 0; }

static int m_IsForceBattleGoal(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_BOOL(SELF_AI->force_battle); }
static int m_ClearForceBattleGoal(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)o, (void)up; SELF_AI->force_battle = 0; return 0; }

/* AddObserveArea(slot, observer, target, dir, angle, dist): INTERUPT_Inside_ObserveArea when the target enters */
static int m_AddObserveArea(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    Ai *ai = SELF_AI;
    int s = ARGI(1);
    if (s >= 0 && s < 4) {
        ai->observe[s].on = 1, ai->observe[s].target = ARGI(3), ai->observe[s].dir = ARGI(4);
        ai->observe[s].angle = ARGN(5), ai->observe[s].dist = ARGN(6), ai->observe[s].inside = 0;
    }
    return 0;
}

static int m_IsInsideObserve(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Ai *ai = SELF_AI;
    int s = ARGI(1);
    RET_BOOL(s >= 0 && s < 4 && ai->observe[s].on && ai->observe[s].inside);
}

/* Team: call help (callHelp_MyPeerId / CallPeerId peers within callHelp_CallValidRange), goal-scoped records */
static int m_TeamHelp_IsValidCall(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)n, (void)up;
    Ai *ai = SELF_AI;
    RET_BOOL(!ai->call_done && ai->th->excel[X_callHelp_IsCall] != 0 && ai->state == AIS_BATTLE);
}

static int m_TeamHelp_ReserveCall(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)o, (void)up; SELF_AI->calling = 1; return 0; }

static int m_TeamHelp_Call(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)n, (void)o, (void)up;
    Ai *ai = SELF_AI;
    ai->call_done = 1, ai->calling = 0;
    int peer = (int)ai->th->excel[X_callHelp_CallPeerId];
    for (int i = 0; i < nteam; i++) {
        Ai *b = team[i];
        if (b == ai || !b->f || (int)b->th->excel[X_callHelp_MyPeerId] != peer || b->state == AIS_BATTLE)
            continue;
        if (dist_xz(team_pos[i], ai->f->pos) <= ai->th->call_range && b->th->excel[X_callHelp_IsReply] != 0)
            b->called = 1, b->reply_to = ai->index, b->change = 1;
    }
    return 0;
}

static int m_TeamHelp_IsValidReply(LVal *a, int n, LVal *o, const LVal *up) { (void)n, (void)up; RET_BOOL(SELF_AI->called); }

static int m_TeamHelp_Reply(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)n, (void)o, (void)up;
    Ai *ai = SELF_AI;
    ai->called = 0;
    ai->seen_t = 0;  /* the caller's target becomes known */
    if (ai->f)
        ai->last_sight = ai->f->target;
    return 0;
}

static int m_AddGoalScopedTeamRecord(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    Goal *g = SELF_GOAL;
    if (g)
        g->team_rec = ARGI(1);
    return 0;
}

static int has_rec(const Goal *g, int type)
{
    for (; g; g = g->next)
        if (g->team_rec == type || has_rec(g->sub, type))
            return 1;
    return 0;
}

/* GetTeamRecordCount(type, target, dist): allies within dist that hold such a record */
static int m_GetTeamRecordCount(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    Ai *ai = SELF_AI;
    int c = 0;
    float range = n > 3 ? ARGN(3) : 1e9f;
    for (int i = 0; i < nteam; i++)
        if (team[i] != ai && team[i]->root && has_rec(team[i]->root->sub, ARGI(1)) &&
            dist_xz(team_pos[i], ai->f->pos) <= range)
            c++;
    RET_NUM(c);
}

static int m_IsLocalPlayer(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    int t = ARGI(1);
    RET_BOOL(t == T_ENE0 || t == T_LOCALPLAYER);
}

static int m_PrintText(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)o, (void)up;
    if (n > 1 && lua0_cstr(a[1]))
        printf("ai[%d]: %s\n", SELF_AI->index, lua0_cstr(a[1]));
    return 0;
}

static int m_true(LVal *a, int n, LVal *o, const LVal *up) { (void)a, (void)n, (void)up; RET_BOOL(1); }

/* Methods with no game state behind them yet (ladders, objects, events, SpEffects, weapons, paths, odds params):
 * Is / Has / Can / Last... -> false, everything else -> 0 (logged once). */
static int m_stub(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)a, (void)n;
    const char *name = lua0_cstr(*up);
    int is = name && (!strncmp(name, "Is", 2) || !strncmp(name, "Has", 3) || !strncmp(name, "Can", 3) ||
                      !strncmp(name, "Last", 4) || !strncmp(name, "DbgAutoRemo_Is", 14));
    unsigned h = 0;
    for (const char *c = name ? name : ""; *c; c++)
        h = h * 31 + (unsigned char)*c;
    if (!fallback_seen[h % 64]) {
        fallback_seen[h % 64] = 1;
        printf("ai: method %s not implemented (returns %s)\n", name ? name : "?", is ? "false" : "0");
    }
    if (is)
        RET_BOOL(0);
    if (name && !strcmp(name, "GetEzStateAnimId"))
        RET_NUM(-1);
    RET_NUM(0);
}

static LVal method_fallback(int tag, LVal key)
{
    (void)tag;
    return lua0_cfunc(m_stub, key);
}

static const struct { const char *name; LCFunc f; } METHODS[] = {
    {"GetDist", m_GetDist}, {"GetDistXZ", m_GetDistXZ}, {"GetDistY", m_GetDistY}, {"GetDistYSigned", m_GetDistYSigned},
    {"GetRandam_Int", m_GetRandam_Int}, {"GetRandam_Float", m_GetRandam_Float},
    {"IsInsideTarget", m_IsInsideTarget}, {"IsInsideTargetEx", m_IsInsideTargetEx}, {"TurnTo", m_TurnTo},
    {"IsLookToTarget", m_IsLookToTarget}, {"GetHpRate", m_GetHpRate}, {"IsBattleState", m_IsBattleState},
    {"IsCautionState", m_IsCautionState}, {"IsFindState", m_IsFindState}, {"IsChangeState", m_IsChangeState},
    {"IsSearchTarget", m_IsSearchTarget}, {"IsInterupt", m_IsInterupt}, {"GetExcelParam", m_GetExcelParam},
    {"GetNpcThinkParamID", m_GetNpcThinkParamID}, {"GetBattleGoalId", m_GetBattleGoalId},
    {"GetDistParam", m_GetDistParam}, {"GetReplanningGoalAction", m_GetReplanningGoalAction},
    {"GetReplanningGoalID", m_GetReplanningGoalID}, {"GetMovePointNumber", m_GetMovePointNumber},
    {"GetMovePointEffectRange", m_GetMovePointEffectRange}, {"AddTopGoal", m_AddTopGoal},
    {"AddSubGoal", m_AddSubGoal}, {"AddSubGoal_Front", m_AddSubGoal_Front}, {"AddLifeParentSubGoal", m_AddSubGoal},
    {"ClearSubGoal", m_ClearSubGoal}, {"HasTopSubgoal", m_HasTopSubgoal}, {"HasGoal", m_HasGoal},
    {"GetLife", m_GetLife}, {"GetParam", m_GetParam}, {"IsExistParam", m_IsExistParam},
    {"GetSubGoalNum", m_GetSubGoalNum}, {"GetLastSubGoalResult", m_GetLastSubGoalResult},
    {"SetTimer", m_SetTimer}, {"IsFinishTimer", m_IsFinishTimer}, {"SetNumber", m_SetNumber},
    {"GetNumber", m_GetNumber}, {"SetAttackRequest", m_SetAttackRequest}, {"IsStartAttack", m_IsStartAttack},
    {"IsFinishAttack", m_IsFinishAttack}, {"DoEzAction", m_DoEzAction},
    {"IsEnableComboAttack", m_IsEnableComboAttack}, {"IsEnableCancelMove", m_IsEnableCancelMove},
    {"StartDash", m_StartDash}, {"EndDash", m_EndDash}, {"IsForceBattleGoal", m_IsForceBattleGoal},
    {"ClearForceBattleGoal", m_ClearForceBattleGoal}, {"AddObserveArea", m_AddObserveArea},
    {"IsInsideObserve", m_IsInsideObserve}, {"TeamHelp_IsValidCall", m_TeamHelp_IsValidCall},
    {"TeamHelp_ReserveCall", m_TeamHelp_ReserveCall}, {"TeamHelp_Call", m_TeamHelp_Call},
    {"TeamHelp_IsValidReply", m_TeamHelp_IsValidReply}, {"TeamHelp_Reply", m_TeamHelp_Reply},
    {"TeamHelp_ValidateCall", f_nop}, {"TeamHelp_ValidateReply", f_nop},
    {"AddGoalScopedTeamRecord", m_AddGoalScopedTeamRecord}, {"GetTeamRecordCount", m_GetTeamRecordCount},
    {"IsLocalPlayer", m_IsLocalPlayer}, {"PrintText", m_PrintText}, {"ReqPlatoonState", f_nop},
    {"IsOnNearMeshByPos", m_true}, {"SetMoveLROnly", f_nop}, {"ChangeEquipItem", f_nop},
    {"ChangeEquipMagic", f_nop}, {"AddObserveChrDmySphere", f_nop}, {"SetAIFixedMoveTarget", f_nop},
    {"SetEventMoveTarget", f_nop}, {"SetAllowTriggerNearObjAct", f_nop}, {"ClearFinishObjAct", f_nop},
    {"KickEvent", f_nop}, {"Replaning", f_nop}, {"DbgSetLastActIdx", f_nop},
};

int ai_scripts(const void *common, int common_size, const void *map, int map_size)
{
    for (int i = 0; i < POOL; i++)
        pool[i].next = i + 1 < POOL ? &pool[i + 1] : NULL;
    free_list = &pool[0];
    lua0_init();
    lua0_register("REGISTER_GOAL", f_register_goal);
    lua0_register("REGISTER_GOAL_NO_UPDATE", f_no_update);
    lua0_register("REGISTER_GOAL_NO_INTERUPT", f_no_interupt);
    lua0_register("REGISTER_GOAL_UPDATE_TIME", f_update_time);
    lua0_register("REGISTER_DBG_GOAL_PARAM", f_nop);
    lua0_register("REGISTER_LOGIC_FUNC", f_register_logic);
    lua0_register("ENABLE_COMBO_ATK_CANCEL", f_combo_cancel);
    lua0_register("loadstring", f_loadstring);
    LVal mt = lua0_newtable();
    for (unsigned i = 0; i < sizeof METHODS / sizeof METHODS[0]; i++)
        lua0_set(mt, lua0_str(METHODS[i].name), lua0_cfunc(METHODS[i].f, LNIL));
    lua0_methods(TAG_AI, mt, method_fallback);
    lua0_methods(TAG_GOAL, mt, method_fallback);
    int n = 0, k;
    if (common && (k = lua0_load(common, common_size)) > 0)
        n += k;
    if (map && (k = lua0_load(map, map_size)) > 0)
        n += k;
    for (int i = 0; i < nregs; i++)
        reg_resolve(&regs[i]);
    lua0_gc(mark_goals);
    scripts_ok = n > 0;
    ai_lua_heap = lua0_heap;
    printf("ai: %d chunks, %d goals, %d logics, Lua heap %u bytes\n", n, nregs, nlogics, lua0_heap);
    return scripts_ok ? n : -1;
}

int ai_scripts_add(const void *map, int map_size)
{
    int first = nregs, k = lua0_load(map, map_size);
    for (int i = first; i < nregs; i++)
        reg_resolve(&regs[i]);
    lua0_gc(mark_goals);
    ai_lua_heap = lua0_heap;
    printf("ai: +%d chunks, %d goals, %d logics, Lua heap %u bytes\n", k, nregs, nlogics, lua0_heap);
    return k;
}
