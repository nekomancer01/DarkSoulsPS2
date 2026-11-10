/* Logic pass breakdown (Burg plan B6, default on; make DEFS=-DLOGIC_PASS_MS=0 = off): COP0 Count ticks per part of
 * the budget's logic pass (testscript.c pb_frame: hitch sections HS_PRE + HS_LOGIC) this frame, so a bonfire's 5 to
 * 15 ms can be pinned on a part before anything is changed. Every frame is timed (about 100 Count reads with 16
 * characters active, ~15 us in PCSX2). Logged every LPASS_LOG frames as "logic: pass ms" (the average per frame, then
 * the frame with the largest logic total, then the largest of each part on its own) and parsed by tools/budget.py.
 * Parts are exclusive: a part timed inside another (nav inside ai, the Lua GC step inside the enemy pass) is taken out
 * of the outer one; "other" is the rest of the two sections.
 *   player   player_update, combat_player_frame, event_push_player
 *   stream   enemy_stream: type loads, skin batch builds
 *   enemy    the rest of enemy_update: npc_update, ai_team, the active selection, deaths, animation clocks, TAE sounds
 *   ai       ai_update (goals, Lua AI scripts) without nav
 *   nav      nav_route (navmesh searches)
 *   esd      esd_update (enemyCommon ESD) and ev_apply
 *   move     root motion, locomotion move_by, fall_step, separate (capsules push apart)
 *   ragdoll  dead_ragdoll, rag_push
 *   pose     mdl_sample / mdl_blend of the active characters
 *   hits     attack_sweeps (enemy weapons on the player)
 *   gc       the Lua GC step (ai_end_frame)
 *   objects  objects_update
 *   events   event_update (EMEVD instances)
 *   bonfire  bonfire_update (rest, respawn, bloodstain) */
#ifndef LPASS_H
#define LPASS_H
#include "ticks.h"

/* The logic pass fixes the breakdown pointed at (default on; make DEFS=-DLOGIC_SPREAD=0 = the old order and code):
 * collision.c skips triangles outside the tested box on the fragments' integer grid, event.c lets an instance whose
 * MAIN group is held false by a flag condition sleep until the flags change, enemy.c spreads a rest's or death's
 * respawn and budgets navmesh searches by time */
#ifndef LOGIC_SPREAD
#define LOGIC_SPREAD 1
#endif

#ifndef LOGIC_PASS_MS
#define LOGIC_PASS_MS 1
#endif

enum { LG_PLAYER, LG_STREAM, LG_ENEMY, LG_AI, LG_NAV, LG_ESD, LG_MOVE, LG_RAGDOLL, LG_POSE, LG_HITS, LG_GC, LG_OBJECTS,
       LG_EVENTS, LG_BONFIRE, LG_N };

extern unsigned lpass_t[LG_N];  /* EE ticks this frame */
static inline unsigned lpass_now(void) { return ticks(); }
#if LOGIC_PASS_MS
#define LG_BEGIN(s) unsigned lg_t0_##s = lpass_now()
#define LG_END(s, part) (lpass_t[part] += lpass_now() - lg_t0_##s)
/* a part timed inside another: what the inner parts took in between comes off the outer one */
#define LG_BEGIN_X(s) unsigned lg_t0_##s = lpass_now(), lg_in_##s = lpass_sum()
#define LG_END_X(s, part) (lpass_t[part] += lpass_now() - lg_t0_##s - (lpass_sum() - lg_in_##s))
#else
#define LG_BEGIN(s) ((void)0)
#define LG_END(s, part) ((void)0)
#define LG_BEGIN_X(s) ((void)0)
#define LG_END_X(s, part) ((void)0)
#endif
static inline unsigned lpass_sum(void)
{
    unsigned s = 0;
    for (int i = 0; i < LG_N; i++)
        s += lpass_t[i];
    return s;
}

/* main.c, end of the logic section: total = the frame's HS_PRE + HS_LOGIC ticks. Accumulates, logs, clears */
void lpass_frame(unsigned total);

#endif
