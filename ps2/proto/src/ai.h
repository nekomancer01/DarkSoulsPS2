/* Enemy AI: DSR's own Lua 5.0 AI scripts (script/aiCommon.luabnd + script/<map>.luabnd) run unchanged on the lua0
 * VM, with the engine side of DSR's goal system in C (ai.c). See notes/PORTING.md "Enemy AI: Lua 5.0 VM".
 *
 * Per NpcThinkParam the engine runs a logic function (logicId: REGISTER_LOGIC_FUNC, e.g. common10000_Logic ->
 * COMMON_EasySetup3) whenever the top goal has no subgoals or the perception state changed; the logic adds top goals
 * (battle goal battleGoalID, NonBattleAct, TeamCallHelp...). Goals are Lua (Name_Activate / _Update / _Terminate /
 * _Interupt, REGISTER_GOAL) or native C++ in the game (Wait, Stay, MoveToSomewhere, Guard, SidewayMove, KeepDist,
 * BackToHome, SpinStep, LeaveTarget, ApproachStep, Parry, CommonAttack), implemented here. The native goals drive the
 * character through AiOut: one action request for the ESD (the exe's ctx+0x528), locomotion, facing, guard pose. */
#ifndef AI_H
#define AI_H

#include "render.h"

/* What the AI asks of its character this frame (read by enemy.c and the ESD env functions) */
typedef struct {
    int req;           /* action request (attacks 3000.., steps 700.., call-help / reply actions), -1 none */
    int move;          /* walk / run along move_yaw */
    int run;
    float move_yaw;    /* game yaw: forward = (sin, 0, cos) */
    int face;          /* turn towards face_yaw */
    float face_yaw;
    int guard;         /* guard pose anim to hold while moving (DoEzAction / Guard goal: 9910), -1 none */
} AiOut;

/* NpcThinkParam / NpcParam values for one placement (chr.bin variant, tools/export_chr.py) */
#define AI_EXCEL_N 24  /* AI_EXCEL_THINK_PARAM_TYPE__* (ai_define.lua): GetExcelParam index -> value */
typedef struct {
    int npc, think, logic, battle;
    float eye_dist, eye_ang_x, eye_ang_y, ear_dist, ear_cut;
    float near_d, mid_d, far_d, out_d;
    float sight_forget, sound_forget;
    int goal_action_caution, goal_id_caution, goal_action_find, goal_id_find;
    float call_range, call_min_target_dist;
    float excel[AI_EXCEL_N];
} AiThink;

/* What the AI sees this frame (filled by enemy.c) */
typedef struct {
    V3 pos, home;
    float yaw, home_yaw, radius;
    int hp, hp_max;
    V3 target;               /* TARGET_ENE_0 = the player */
    float target_yaw;
    int target_hp, target_hp_max, target_alive;
    int target_attack;       /* the player's attack anim playing, -1 none */
    int anim;                /* action anim playing (attack / step / damage), -1 none */
    float anim_t, anim_len;
    int combo_window;        /* TAE flag 86 open: the next combo request is taken (f120) */
    int cancel_window;       /* the action can be cancelled into movement (TAE flag 23 "AI combo" window) */
    int damaged;             /* a hit landed this frame */
    float dt;
} AiFrame;

typedef struct Goal Goal;
typedef struct Ai {
    const AiThink *th;
    const AiFrame *f;
    AiOut out;
    Goal *root;
    int index;
    unsigned rng;
    int state, prev_state;   /* AIS_* perception state */
    int change;              /* state changed this frame (IsChangeState) */
    float seen_t, heard_t;   /* seconds since the target was last seen / heard (large = never) */
    V3 last_sight, sound_pos;
    int interupt;            /* interrupt being dispatched (IsInterupt), -1 none */
    unsigned raised;         /* interrupt bits raised this frame */
    float timer[8], number[16];
    int force_battle;
    int called;              /* TeamHelp: a call from a peer is waiting for a reply */
    int reply_to;            /* index of the caller (TARGET_FRI_0) */
    int calling;             /* TeamHelp_ReserveCall .. TeamHelp_Call */
    int call_done;
    struct { int on, target, dir; float angle, dist; int inside; } observe[4];
    int dash;
    int prev_target_attack, target_attack_seen;
    int prev_anim;
    float anim_started_t;    /* for IsStartAttack / IsFinishAttack */
    int attack_req, attack_started, attack_finished;
    int errors;
    void *cur_goal;          /* goal whose Lua function is running */
    int ez_anim;             /* DoEzAction guard pose (9910) held while ez_goal lives, for ez_life seconds */
    float ez_life;
    void *ez_goal;
} Ai;

enum { AIS_NORMAL, AIS_CAUTION, AIS_FIND, AIS_BATTLE };

/* Load the scripts (lua0 packs: aiCommon from common.pak, the map's from the area pack) and register the engine
 * functions; returns chunks loaded, -1 on failure */
int ai_scripts(const void *common, int common_size, const void *map, int map_size);
/* a further map's ai_map.lua0 (world blocks) into the same VM */
int ai_scripts_add(const void *map, int map_size);
void ai_init(Ai *a, const AiThink *th, int index, unsigned seed);
/* One think step + goal update; fills a->out */
void ai_update(Ai *a, const AiFrame *f);
/* Clear all goals (death) */
void ai_reset(Ai *a);
int ai_goals_in_use(void);  /* goal pool slots held (leak check at world block on / off) */
/* A noise this frame at pos audible within radius (player footsteps, rolls, attacks): "heard" (ear_dist) */
void ai_sound(V3 pos, float radius);
/* After all enemies updated: drop this frame's sounds, collect garbage */
void ai_end_frame(void);
/* Deepest active goal's name (HUD) */
const char *ai_goal_name(const Ai *a);
/* Allies for call-help / team records / TARGET_FRI_0 (enemy.c registers every Ai with its position) */
void ai_team(Ai **all, const V3 *pos, int n);
extern unsigned ai_lua_heap, ai_lua_instr;
extern int ai_lua_errors;
extern char ai_last_error[224];

#endif
