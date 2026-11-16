/* Player status effects (status.h). Data: each status SpEffectParam row carries its buildup (poizonAttackPower,
 * registIllness, registBlood, registCurse) and its own effect (changeHpPoint every motionInterval for
 * effectEndurance s, or changeHpRate % of max HP at once), exported per enemy attack window (export_chr.py
 * status_of). Resistances from FUN_1402dddd0 through chrstat.c. ASSUMED (TECH_DEBT status-1): the meter fills by
 * the SpEffect's power per hit and drains by STATUS_DECAY a second; a full meter starts the effect and empties;
 * curse kills (DS1's curse death; the halved max HP that follows is not modelled). */
#include <stdio.h>
#include <string.h>
#include "status.h"
#include "chrstat.h"
#include "combat.h"
#include "player.h"

#define STATUS_DECAY 1.0f  /* meter points a second (ASSUMED) */

StatusState status_pl;
static const char *const names[STATUS_COUNT] = {"poison", "toxic", "bleed", "curse"};
static int pending_curse;
static float pending_bleed;  /* hp % of max to take on the next update */

static void add(int k, int power, float hp, float tick, float time, float rate)
{
    StatusState *s = &status_pl;
    float res = chr_stats.valid ? chr_stats.resist[k] : 0;
    s->meter[k] += power;
    if (s->meter[k] < res)
        return;
    s->meter[k] = 0, s->procs[k]++;
    printf("status: %s full (resist %.0f): %s\n", names[k], res,
           k == STATUS_BLEED ? "bleed" : k == STATUS_CURSE ? "cursed" : "effect starts");
    if (k == STATUS_BLEED) {
        pending_bleed += rate;
    } else if (k == STATUS_CURSE) {
        pending_curse = 1;
    } else {
        s->active[k] = 1, s->left[k] = time, s->hp[k] = hp, s->tick[k] = tick > 0 ? tick : 1, s->tick_t[k] = s->tick[k];
    }
}

static void on_hit(CombatChr *t, const CombatAttack *atk)
{
    if (t != combat_player() || atk->st.type < 1 || atk->st.type > STATUS_COUNT)
        return;
    add(atk->st.type - 1, atk->st.build, atk->st.hp, atk->st.tick, atk->st.time, atk->st.rate);
}

void status_init(void)
{
    memset(&status_pl, 0, sizeof status_pl);
    combat_status_hook = on_hit;
}

void status_clear(void)
{
    int procs[STATUS_COUNT];
    memcpy(procs, status_pl.procs, sizeof procs);
    memset(&status_pl, 0, sizeof status_pl);
    memcpy(status_pl.procs, procs, sizeof procs);
    pending_curse = 0, pending_bleed = 0;
}

void status_test_add(int type, int power, float hp, float tick, float time, float rate)
{
    if (type >= 0 && type < STATUS_COUNT)
        add(type, power, hp, tick, time, rate);
}

void status_update(Player *p, float dt)
{
    StatusState *s = &status_pl;
    if (p->hp < 0) {  /* dead: the respawn starts clean (as a rest) */
        status_clear();
        return;
    }
    if (pending_bleed > 0) {
        p->hp -= (int)(p->hp_max * pending_bleed / 100);
        pending_bleed = 0;
    }
    if (pending_curse)
        p->hp = -1, pending_curse = 0;
    for (int k = 0; k < STATUS_COUNT; k++) {
        if (s->meter[k] > 0)
            s->meter[k] = s->meter[k] > STATUS_DECAY * dt ? s->meter[k] - STATUS_DECAY * dt : 0;
        if (!s->active[k] || k == STATUS_BLEED || k == STATUS_CURSE)
            continue;
        s->left[k] -= dt;
        if ((s->tick_t[k] -= dt) <= 0) {
            s->tick_t[k] += s->tick[k];
            p->hp -= (int)s->hp[k];
        }
        if (s->left[k] <= 0)
            s->active[k] = 0;
    }
    if (p->hp == 0)
        p->hp = -1;  /* the ESD death branches test f1000() < 0 */
}
