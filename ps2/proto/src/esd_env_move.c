/* ESD env functions / commands of the movement side (character proxy: ground, airborne, falling, landing).
 * esd.c calls esd_env_move / esd_cmd_move first for every id; return 1 when handled.
 *
 * The physics itself (capsule vs map collision, gravity, airborne velocity, landing) runs every frame in player.c
 * constrain() and leaves its result in the Player (airborne, vel, fall_y0, hp). In DSR the normal fall loop
 * (a00_0760) and its landing (761 / 610) are engine-driven, not ESD states; the ESD only sees the proxy through the
 * env functions below and handles the fall *death* (group 0 states 57-59: 6065 / 6066 / 6067). */
#include "esd.h"
#include "player.h"
#include "combat.h"
#include "combat_data.h"

/* EzStateActionEnv FUN_14038b540 case ids */
#define F_ON_GROUND 201   /* FUN_1403868a0: character proxy (ChrCtrl+0x28) byte +0x32; the fall-death loop (state 58)
                           * waits for it before the impact anim 6067 */
#define F_CAN_FALL 200   /* FUN_140386850: proxy not on ground (+0x32) && (+0x6D == 0 || +0x1B4 >= 1) && ChrActionFlag 51
                          * Disable Fall Motion (ctx+0xA0 bit 13) clear -> group 0 state 14 (fall start 1550) */
#define F_FALL_HEIGHT 224 /* FUN_14037ac30: proxy +0x114 - +0x14 = height fallen (10000 with ctx+0xAC bit 1) */
#define F_EQUIP_LOAD 263  /* PlayerIns vt+0x558 FUN_140360780 -> FUN_1402df510: equip weight / max load x 100 (x factor
                           * FUN_1402df690, ASSUMED 1); picks the landing tier with f224 */
#define F_LAND_VARIANT 246 /* ctx+0x31C bit 1: landing variants 1765-1767 (not set by anything here: 0) */
#define F_FLAG_A4_10 271  /* ctx+0xA4 bit 10 (landing state 34 / 1790) */
#define F_DAMAGE_CLASS 237 /* FUN_1402ddda0(level ctx+0x1EC, guard ctx+0x1F0): int table 0x141A2B780[guard * 12 + level],
                            * 0 for level >= 12 or guard >= 5 (dumped by exe-decoding) */
#define F_DAMAGE_LEVEL 236  /* ctx+0x1EC (DmgLv): the reaction level of a hit landing this frame, 0 otherwise */
#define F_DAMAGE_DIR 222    /* ctx+0x1E8 (DamageDir), 0-3 */
#define F_FALL_DEATH 283  /* FUN_14033bd70: ctx+0x1E0 != -1 / ChrIns sub-state == 1; with f1000() < 0 it selects
                           * state 57 (fall death). The prototype has no other way to die, so HP <= 0 is enough. */

int esd_env_move(EsdCtx *c, int id, const float *args, int nargs, float *out)
{
    if (c->kind != ESD_PLAYER || !c->p)
        return 0;  /* enemies answer these through their own hooks */
    const Player *p = (const Player *)c->p;
    const CombatChr *me = combat_player();
    (void)args, (void)nargs;
    switch (id) {
    case F_DAMAGE_LEVEL:
        *out = me->new_hit ? me->damage_level : 0;
        return 1;
    case F_DAMAGE_CLASS: {
        static const unsigned char cls[5][12] = {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
                                                 {0, 1, 3, 3, 4, 1, 3, 3, 1, 3, 4, 3},
                                                 {0, 1, 1, 3, 3, 1, 3, 3, 1, 3, 4, 3},
                                                 {0, 1, 1, 3, 3, 1, 3, 3, 1, 3, 4, 3},
                                                 {0, 1, 1, 1, 3, 1, 3, 3, 1, 3, 4, 3}};
        int level = me->new_hit ? me->damage_level : 0, guard = 0;  /* no guarding yet: guard level 0 */
        *out = level >= 0 && level < 12 ? cls[guard][level] : 0;
        return 1;
    }
    case F_DAMAGE_DIR: {
        /* from the hit reactions' poses (a00 2004-2007 = 2004 + f222): 2007 leans back (hit from the front) = 3, 2006
         * leans forward (from behind) = 2, 2004 / 2005 lean sideways = 0 / 1 (which side is which: ASSUMED) */
        float a = me->hit_dir;
        *out = a > -0.785f && a < 0.785f ? 3 : a >= 2.356f || a <= -2.356f ? 2 : a > 0 ? 0 : 1;
        return 1;
    }
    case F_CAN_FALL:
        /* the proxy +0x6D / +0x1B4 term is not modelled */
        *out = p->airborne && !(p->ctx_a0 & 0x2000);
        return 1;
    case F_FALL_HEIGHT:
        *out = p->airborne ? p->fall_y0 - p->pos.y : p->land_h;
        return 1;
    case F_EQUIP_LOAD:
        /* max load ASSUMED 40 + END (FUN_1402e19b0 not decoded); Warrior 21.4 / 52 -> 41 */
        *out = (int)(PL_EQUIP_WEIGHT / (40.0f + PL_END) * 100);
        return 1;
    case F_LAND_VARIANT:
        *out = 0;
        return 1;
    case F_FLAG_A4_10:
        *out = (p->ctx_a4 >> 10) & 1;
        return 1;
    case F_ON_GROUND:
        *out = !p->airborne;
        return 1;
    case F_FALL_DEATH:
        *out = p->hp <= 0;
        return 1;
    }
    return 0;
}

int esd_cmd_move(EsdCtx *c, int bank, int id, const float *args, int nargs)
{
    (void)bank, (void)id, (void)args, (void)nargs;
    if (c->kind != ESD_PLAYER || !c->p)
        return 0;
    return 0;
}
