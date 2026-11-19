/* HP and damage core (combat.h). Formulas from DarkSoulsRemastered.exe, constants at the addresses given:
 * - CalcCorrectGraph lookup FUN_1402dfb70 (max HP = row 100 at VIT, FUN_1402df9a0; weapon stat scaling)
 * - weapon stat correction FUN_1402dacd0, combined for STR + DEX in FUN_1402d9cb0
 * - physical defence FUN_1402d9890 (0x141A2B5F0..60C)
 * - player fall damage rate PlayerIns vtable+0x2A8 FUN_14035f750 -> FUN_1402df3a0 / FUN_1402df580 (0x141A2B61C..668) */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "combat.h"
#include "combat_data.h"
#include "trail.h"
#include "player.h"
#include "model.h"
#include "vmath.h"
#include "testscript.h"
#include "chrstat.h"

CombatChr combat_dummy;
CombatLast combat_last;

static CombatChr *chrs[COMBAT_MAX_CHRS];
static int hit_window[COMBAT_MAX_CHRS];  /* last attack window that hit this slot */
static float since_hit[COMBAT_MAX_CHRS];

/* PROVISIONAL: poise returns to full after this long without a hit (the exe's poise recovery is not decoded) */
#define POISE_RESET 4.0f

/* ---- formulas ---- */

/* FUN_1402dfb70: CalcCorrectGraph row (stageMaxVal[5], stageMaxGrowVal[5], adjPt_maxGrowVal[5]) at x. Within a
 * stage the growth is t^e for e >= 0, 1 - (1 - t)^-e for e < 0 (0x141615C88 = -1), clamped to the stage. */
float combat_graph(const float *g, float x)
{
    const float *X = g, *Y = g + 5, *E = g + 10;
    if (x > X[4])
        x = X[4];
    if (x <= 0)
        return Y[0];
    int i = 0;
    while (i < 3 && x > X[i + 1])
        i++;
    float dx = X[i + 1] - X[i], t = dx != 0 ? (x - X[i]) / dx : 1, f;
    f = E[i] >= 0 ? powf(t, E[i]) : 1 - powf(1 - t, -E[i]);
    float y = Y[i] + (Y[i + 1] - Y[i]) * f, lo = Y[i] < Y[i + 1] ? Y[i] : Y[i + 1], hi = Y[i] < Y[i + 1] ? Y[i + 1] : Y[i];
    return y < lo ? lo : y > hi ? hi : y;
}

/* From the saved stats and equipment (chrstat.c) once known; the class start values before that */
int combat_player_hp_max(void)
{
    return chr_stats.valid ? chr_stats.hp_max : (int)combat_graph(graph_100, PL_VIT);  /* Warrior VIT 11 -> 594 */
}

/* FUN_1402dacd0: stat at or above the requirement -> 1 + graph(stat) / 100 * correct / 100 (the under-requirement
 * penalty branch is not needed for the Warrior) */
static float stat_factor(int stat, int req, float correct)
{
    if (stat < req || correct <= 0)
        return 1;
    return combat_graph(graph_weapon, stat) / 100.0f * (correct / 100.0f) + 1;
}

/* FUN_1402d9cb0: STR and DEX factors combine as (a - 1) + (b - 1) + 1 */
float combat_player_ar(void)
{
    if (chr_stats.valid)
        return chr_stats.ar[HAND_R][EL_PHYS];
    float a = stat_factor(PL_STR, WEP_REQ_STR, WEP_CORRECT_STR), b = stat_factor(PL_DEX, WEP_REQ_DEX, WEP_CORRECT_DEX);
    return WEP_ATK_PHYS * ((a - 1) + (b - 1) + 1);
}

/* FUN_1402d9890: cut % by atk / def (def 0 -> ratio 8), piecewise quadratic through (0.12, 90) (1, 60) (2.5, 30)
 * (8, 10); damage = atk * (1 - cut / 100) (the second term's weight DAT_141C74E28 is 0) */
float combat_damage_f(float atk, float def)
{
    const float X0 = 0.12f, X1 = 1, X2 = 2.5f, X3 = 8, Y0 = 90, Y1 = 60, Y2 = 30, Y3 = 10;
    float r = def > 0 ? atk / def : X3, cut;
    if (r < X1)
        cut = r <= X0 ? Y0 : (Y1 - Y0) / ((X1 - X0) * (X1 - X0)) * (r - X0) * (r - X0) + Y0;
    else if (r <= X2)
        cut = (Y1 - Y2) / ((X1 - X2) * (X1 - X2)) * (r - X2) * (r - X2) + Y2;
    else
        cut = r < X3 ? (Y2 - Y3) / ((X2 - X3) * (X2 - X3)) * (r - X3) * (r - X3) + Y3 : Y3;
    return (1 - cut / 100) * atk;
}

int combat_damage(float atk, float def, float absorb)
{
    return (int)(combat_damage_f(atk, def) * (1 - absorb));
}

/* Player fall damage as a fraction of max HP (FUN_1402df3a0): 0 under 5 m (0x141A2B630), 1 (lethal) from 20 m
 * (0x141A2B61C), else (h / 20)^2 (0x141A2B620 / 0x141A2B628) x the equip-weight factor FUN_1402df580: v = (1 - w /
 * 100) * 100 clamped to [1, 100], linear through (1, 1) (10, 1) (30, 0.95) (50, 0.9) (100, 0.7) */
float combat_fall_rate(float h)
{
    if (h < 5)
        return 0;
    if (h >= 20)
        return 1;
    static const float X[5] = {1, 10, 30, 50, 100}, Y[5] = {1, 1, 0.95f, 0.9f, 0.7f};
    float w = chr_stats.valid ? chr_stats.equip_weight : PL_EQUIP_WEIGHT, v = 1;
    if (w <= 100) {
        v = (1 - w / 100) * 100;
        v = v < 1 ? 1 : v > 100 ? 100 : v;
    }
    float k = Y[4];
    for (int i = 0; i < 4; i++)
        if (X[i] <= v && v <= X[i + 1]) {
            k = Y[i] + (Y[i + 1] - Y[i]) * (v - X[i]) / (X[i + 1] - X[i]);
            break;
        }
    return (h / 20) * (h / 20) * k;
}

/* ---- registry, sweeps, hits ---- */

int combat_register(CombatChr *c)
{
    for (int i = 0; i < COMBAT_MAX_CHRS; i++)
        if (chrs[i] == c)
            return i;
    for (int i = 0; i < COMBAT_MAX_CHRS; i++)
        if (!chrs[i]) {
            chrs[i] = c, hit_window[i] = -1, since_hit[i] = 99;
            if (c->poise_max > 0 && c->poise <= 0)
                c->poise = c->poise_max;
            return i;
        }
    return -1;
}

void combat_unregister(CombatChr *c)
{
    for (int i = 0; i < COMBAT_MAX_CHRS; i++)
        if (chrs[i] == c)
            chrs[i] = 0;
}

/* FUN_1402d9660: per element the defence curve FUN_1402d9890 of (attack, defence) times the element's rates, floored
 * at 0, summed. Physical defence is the typed one for a slash / blow / thrust attack (FUN_1402dbda0 param 1-3). The
 * function's extra factors (x1.4 by a flag, x1.125 / x0.8 by a sign test at 0x141A2B5B4..BC) come from callers in
 * the obfuscated .text and are not applied (TECH_DEBT combat-2). Absorption stays the port's physical cut. */
float combat_damage_on(const CombatChr *t, const CombatAttack *atk)
{
    float def = t->def_phys;
    if (t->has_typed && atk->attr >= ATK_SLASH && atk->attr <= ATK_THRUST)
        def = t->def_typed[atk->attr - 1];
    float sum = combat_damage_f(atk->phys * atk->atk_phys_rate, def) * (1 - t->absorb_phys);
    for (int e = 0; e < 3; e++) {
        float a = atk->el[e] * atk->el_rate[e];
        if (a > 0)
            sum += combat_damage_f(a, t->def_el[e]);
    }
    return sum > 0 ? sum : 0;
}

/* Lock-on pick: living enemies within range, scored by distance plus 4 m per radian off the view (ASSUMED; DSR's
 * pick rule is in the camera code, not decoded) */
CombatChr *combat_lock_pick(const float pos[3], float yaw, float range)
{
    CombatChr *best = 0;
    float bs = 1e9f;
    for (int i = 0; i < COMBAT_MAX_CHRS; i++) {
        CombatChr *c = chrs[i];
        if (!c || c->team != TEAM_ENEMY || c->dead)
            continue;
        float dx = c->pos[0] - pos[0], dz = c->pos[2] - pos[2], d = sqrtf(dx * dx + dz * dz);
        float a = atan2f(dx, dz) - yaw;
        while (a > 3.14159265f) a -= 6.2831853f;
        while (a < -3.14159265f) a += 6.2831853f;
        if (d > range || fabsf(a) > 1.5707963f || fabsf(c->pos[1] - pos[1]) > 4)
            continue;
        float sc = d + 4 * fabsf(a);
        if (sc < bs)
            bs = sc, best = c;
    }
    return best;
}

int combat_lock_alive(const CombatChr *c)
{
    for (int i = 0; i < COMBAT_MAX_CHRS; i++)
        if (chrs[i] == c)
            return !c->dead;
    return 0;
}

/* distance covered t s into a knockback: v0 for cont s, then slowing linearly to 0 over dec s */
static float knock_dist(const CombatChr *c, float t)
{
    float s = c->knock_v * (t < c->knock_cont ? t : c->knock_cont), u = t - c->knock_cont;
    if (u > 0) {
        u = u < c->knock_dec0 ? u : c->knock_dec0;
        s += c->knock_v * (u - (c->knock_dec0 > 0 ? u * u / (2 * c->knock_dec0) : 0));
    }
    return s;
}

void combat_knock_step(CombatChr *c, float dt, float out[2])
{
    out[0] = out[1] = 0;
    if (c->knock_v <= 0 || c->knock_dec > c->knock_cont + c->knock_dec0)
        return;
    float d = knock_dist(c, c->knock_dec + dt) - knock_dist(c, c->knock_dec);  /* knock_dec: time since the hit */
    c->knock_dec += dt;
    out[0] = c->knock_dir[0] * d, out[1] = c->knock_dir[1] * d;
}

void (*combat_status_hook)(CombatChr *t, const CombatAttack *atk);

int (*combat_throw_hook)(const float pos[3], float yaw, ThrowStart *out);

int combat_apply(CombatChr *t, const CombatAttack *atk)
{
    if (t->invincible && !atk->is_throw)
        return 0;
    float dx = atk->src_pos[0] - t->pos[0], dz = atk->src_pos[2] - t->pos[2], a = atan2f(dx, dz) - t->yaw;
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    /* Guarding (player.c guard states; enemies' guard layer): a hit from the front half (ASSUMED: EquipParamWeapon
     * guardAngle is 0 on the starting shields, the exe's angle test was not found) is cut per element by the
     * shield's phys / mag / fire / thunGuardCutRate and costs stamina: AtkParam atkStam x (100 - staminaGuardDef)
     * / 100 (ASSUMED, the community's DS1 rule; the exe's guard code is in the obfuscated .text, TECH_DEBT combat-9).
     * Stamina below 0 breaks the guard. */
    /* Parry: DS1's parry is an AttackBehavior of AttackType 64 that meets an attack whose anim is in a "Parry
     * Possible" window (TAE flag 5). ASSUMED geometry (TECH_DEBT combat-10): an attack that reaches the parrying
     * character from its front half while the window is open counts as met by the shield */
    if (!atk->is_throw && t->parrying && atk->parryable && a > -1.5707963f && a < 1.5707963f) {
        if (atk->src)
            atk->src->parried = 1;
        t->new_parry = 1;
        printf("combat: %s parried a hit (atk %.0f)\n", t->team == TEAM_PLAYER ? "player" : "enemy",
               atk->phys * atk->atk_phys_rate);
        return 0;
    }
    t->guard_hit = t->guard_broken = 0;
    float cut[4] = {1, 1, 1, 1};
    if (!atk->is_throw && t->guarding && a > -1.5707963f && a < 1.5707963f && !atk->unblockable) {
        for (int e = 0; e < 4; e++)
            cut[e] = 1 - t->guard_cut[e] / 100;
        float st = atk->stam_dmg * (100 - t->guard_stab) / 100;
        t->guard_stamina_dmg = st > 0 ? st : 0;
        t->guard_hit = 1;
        if (t->stamina) {  /* below 0 breaks the guard (ASSUMED, TECH_DEBT combat-9); floor -50 as FUN_140322cf0 */
            *t->stamina -= (int)(t->guard_stamina_dmg + 0.5f);
            t->guard_broken = *t->stamina < 0;
            if (*t->stamina < -50)
                *t->stamina = -50;
        }
    }
    CombatAttack ga = *atk;
    ga.atk_phys_rate *= cut[0];
    for (int e = 0; e < 3; e++)
        ga.el_rate[e] *= cut[1 + e];
    int dmg = (int)combat_damage_on(t, &ga);
    t->hp -= dmg;
    t->hit_count++;
    t->new_hit = 1;
    t->last_damage = dmg;
    /* poise: AtkParam damage level unless the hit is absorbed by poise / super armour; a guarded hit reacts through
     * the guard row of the reaction table instead (f237, esd_env_move.c) */
    int level = atk->damage_level;
    if (t->guard_hit) {
        /* level kept for the guard reaction class */
    } else if (t->super_armor) {
        level = 0;
    } else if (t->poise_max > 0) {
        t->poise -= atk->poise_dmg;
        if (t->poise > 0)
            level = 0;
        else
            t->poise = t->poise_max;
    }
    t->damage_level = level;
    t->hit_dir = a;
    /* knockback: AtkParam knockbackDist along the hit (attacker -> target), covered at constant speed for the
     * reaction size's KnockBackParam ContTime then slowing to 0 over DecTime: v0 = d / (cont + dec / 2) (ASSUMED
     * profile and dmgLevel -> size order, TECH_DEBT combat-12); none while guarding, super armour or i-frames */
    if (atk->knock > 0 && !atk->is_throw && !t->super_armor) {
        const KnockRow *kr = &knock_rows[t->knock_row == 1];
        int k = atk->damage_level >= 0 && atk->damage_level < 8 ? atk->damage_level : 2;
        float cont = kr->cont[k], dec = kr->dec[k], d = atk->knock * (t->guard_hit ? 0.5f : 1);
        float len = sqrtf(dx * dx + dz * dz);
        if (cont + dec * 0.5f > 0 && len > 1e-3f) {
            t->knock_dir[0] = -dx / len, t->knock_dir[1] = -dz / len;
            t->knock_v = d / (cont + dec * 0.5f), t->knock_cont = cont, t->knock_dec0 = dec, t->knock_dec = 0;
        }
    }
    if (atk->st.type > 0 && combat_status_hook && !t->guard_hit)  /* a guarded hit builds no status (ASSUMED) */
        combat_status_hook(t, atk);
    if (t->hp <= 0) {
        t->hp = -1;  /* the ESD death branches test f1000() < 0 */
        t->dead = 1;
    }
    for (int i = 0; i < COMBAT_MAX_CHRS; i++)
        if (chrs[i] == t)
            since_hit[i] = 0;
    printf("combat: %s hit by %s, atk %.0f def %.0f dmg %d hp %d level %d dir %.0f%s%s\n", t->team == TEAM_PLAYER ? "player" :
           t->team == TEAM_ENEMY ? "enemy" : "dummy", atk->team == TEAM_PLAYER ? "player" : "enemy",
           atk->phys * atk->atk_phys_rate, t->def_phys, dmg, t->hp, level, a * 57.29578f,
           t->guarding && !t->guard_hit ? " guard up" : "", t->guard_hit ? (t->guard_broken ? " guarded BROKEN" : " guarded") : "");
    combat_last = (CombatLast){dmg, atk->atk_param, atk->phys * atk->atk_phys_rate, t->def_phys, t->hp, level};
    return dmg;
}

int (*combat_sweep_hook)(const float a[3], const float b[3], float r, const CombatAttack *atk);

int combat_sweep(const float a[3], const float b[3], float r, const CombatAttack *atk)
{
    int hits = 0;
    if (combat_sweep_hook)
        combat_sweep_hook(a, b, r, atk);
    for (int i = 0; i < COMBAT_MAX_CHRS; i++) {
        CombatChr *c = chrs[i];
        if (!c || c->team == atk->team || c->dead || hit_window[i] == atk->window)
            continue;
        float rr = c->radius, h = c->height;
        if (c->hit_test)  /* broad phase: the bounds of its own capsules */
            rr = c->reach > rr ? c->reach : rr, h = c->top > h ? c->top : h;
        float lo = c->pos[1] + rr, hi = c->pos[1] + h - rr;
        if (hi < lo)
            lo = hi = c->pos[1] + h * 0.5f;
        float p[3] = {c->pos[0], lo, c->pos[2]}, q[3] = {c->pos[0], hi, c->pos[2]};
        float reach = r + rr;
        if (seg_seg_d2(a, b, p, q) > reach * reach || (c->hit_test && !c->hit_test(c, a, b, r)))
            continue;
        hit_window[i] = atk->window;
        combat_apply(c, atk);
        hits++;
    }
    return hits;
}

void combat_update(float dt)
{
    for (int i = 0; i < COMBAT_MAX_CHRS; i++) {
        if (!chrs[i])
            continue;
        since_hit[i] += dt;
        if (chrs[i]->poise_max > 0 && since_hit[i] > POISE_RESET)
            chrs[i]->poise = chrs[i]->poise_max;
    }
}

/* ---- the player: hurtbox + Longsword hit capsules from the current pose ---- */

static const float *dummy_pos(int id)
{
    for (int i = 0; i < WEP_DUMMIES; i++)
        if (wep_dummy[i].id == id)
            return wep_dummy[i].p;
    return 0;
}

/* the pose at t (no crossfade: attack windows start long after the 0.15 s blend), R_Weapon to world as model_draw
 * places the model: rotate by yaw + pi (model faces -Z), translate to the feet; world[SK_R_WEAPON] after the call */
static M34 world[SK_BONES_MAX];  /* gamedata_init refuses more bones */
static void weapon_world(const Player *p, int anim_id, float t)
{
    static BoneLocal pose[MAX_BONES];
    model_sample(model_anim(anim_id), t, 0, pose);
    float c = cosf(p->yaw + 3.14159265f), s = sinf(p->yaw + 3.14159265f);
    M34 place = {{c, 0, s, 0, 1, 0, -s, 0, c, p->pos.x, p->pos.y, p->pos.z}};
    for (int i = 0; i <= SK_R_WEAPON; i++) {
        M34 local = m34_from_qt(pose[i].q, pose[i].t);
        world[i] = sk_parent[i] < 0 ? m34_mul(&place, &local) : m34_mul(&world[(int)sk_parent[i]], &local);
    }
}

static CombatChr player_chr;
float combat_hit_a[3], combat_hit_b[3];  /* last blade capsule (debug drawing) */
int combat_hit_active;

int combat_player_frame(Player *p, int anim_id, float t, float dt)
{
    static int window = 0, cur_win = -1, last_anim = -1;
    static float last_t;
    /* hurtbox: Warrior capsule (COL_PLAYER_R / H; NpcParam row of the player not decoded) */
    player_chr.pos[0] = p->pos.x, player_chr.pos[1] = p->pos.y, player_chr.pos[2] = p->pos.z;
    player_chr.yaw = p->yaw;
    player_chr.radius = 0.4f, player_chr.height = 1.5f;
    player_chr.team = TEAM_PLAYER;
    /* HP: merge both sides since the last sync - the player side (fall damage, respawn, estus) changed p->hp, combat
     * (enemy sweeps, which run after this) changed player_chr.hp */
    static int synced;
    static int first = 1;
    if (first)
        synced = player_chr.hp = p->hp, first = 0;
    int hp = p->hp + (player_chr.hp - synced);
    if (test_god && hp < p->hp_max)
        hp = p->hp_max;  /* test script "option god": routes with enemies on that the walker cannot fight */
    if (hp <= 0 && (p->hp > 0 || player_chr.hp != synced))
        hp = -1;  /* dead: the ESD death branches test f1000() < 0 */
    p->hp = player_chr.hp = synced = hp;
    player_chr.hp_max = p->hp_max;
    if (hp > 0)
        player_chr.dead = 0;  /* respawned */
    player_chr.invincible = p->invincible;
    if (chr_stats.valid) {  /* armour + stats (chrstat.c); no absorption in DS1's armour */
        player_chr.def_phys = chr_stats.def[EL_PHYS];
        for (int e = 0; e < 3; e++)
            player_chr.def_el[e] = chr_stats.def[1 + e], player_chr.def_typed[e] = chr_stats.def_typed[1 + e];
        player_chr.has_typed = 1;
        player_chr.poise_max = chr_stats.poise;
        /* guard with the left-hand weapon (the guard states play the left hand's guardmotionCategory, f278(0)) */
        for (int e = 0; e < 4; e++)
            player_chr.guard_cut[e] = chr_stats.guard_cut[HAND_L][e];
        player_chr.guard_stab = chr_stats.guard_stab[HAND_L];
        player_chr.guard_level = chr_stats.guard_level[HAND_L];
    }
    player_chr.guarding = p->guarding;
    player_chr.parrying = p->parrying;
    player_chr.knock_row = 1;
    player_chr.stamina = &p->stamina;  /* a guarded hit takes its stamina cost at once (combat_apply) */
    player_chr.new_hit = 0;  /* the ESD (in player_update, before this) has seen last frame's hit */
    combat_register(&player_chr);
    combat_update(dt);

    int hits = 0;
    combat_hit_active = 0;
    if (anim_id != last_anim || t < last_t)
        cur_win = -1;
    last_anim = anim_id, last_t = t;
    for (int w = 0; w < ATK_WINDOWS; w++) {
        const AtkWindow *aw = &atk_windows[w];
        if (aw->anim != anim_id || t < aw->start || t >= aw->end)
            continue;
        if (cur_win != w)
            cur_win = w, window++;
        weapon_world(p, anim_id, t);
        CombatAttack atk = {TEAM_PLAYER, aw->atk_param, combat_player_ar(), aw->phys_corr / 100.0f,
                            (chr_stats.valid ? chr_stats.poise_dmg[HAND_R] : WEP_POISE_DMG) * aw->sa_corr / 100.0f,
                            aw->dmg_level, {p->pos.x, p->pos.y, p->pos.z}, window};
        atk.attr = aw->attr;
        atk.knock = aw->knock;
        if (chr_stats.valid)
            for (int e = 0; e < 3; e++)
                atk.el[e] = chr_stats.ar[HAND_R][1 + e];
        atk.el_rate[0] = aw->mag_corr / 100.0f, atk.el_rate[1] = aw->fire_corr / 100.0f;
        atk.el_rate[2] = aw->thun_corr / 100.0f;
        for (int h = 0; h < aw->nhit; h++) {
            const float *d1 = dummy_pos(aw->hit[h].dp1), *d2 = dummy_pos(aw->hit[h].dp2);
            if (!d1 || !d2)
                continue;
            float a[3], b[3];
            m34_apply(&world[SK_R_WEAPON], d1, a);
            m34_apply(&world[SK_R_WEAPON], d2, b);
            if (h == 0)
                memcpy(combat_hit_a, a, 12), memcpy(combat_hit_b, b, 12), combat_hit_active = 1;
            int n = combat_sweep(a, b, aw->hit[h].r, &atk);
            hits += n;
            if (n && aw->hit_stop > p->hitstop)  /* AtkParam hitStopTime: the swing stops for a moment on contact */
                p->hitstop = aw->hit_stop;
        }
    }
    trail_tick(dt);  /* the weapon trail (trail.c): base and tip each frame of a SpawnFFX_Blade window */
    for (int w = 0; w < NTRAILS; w++) {
        const TrailWindow *tw = &trails[w];
        const float *b = dummy_pos(tw->base), *e = dummy_pos(tw->tip);
        if (tw->anim != anim_id || t < tw->start || t >= tw->end || !b || !e)
            continue;
        float a[3], c[3];
        weapon_world(p, anim_id, t);
        m34_apply(&world[SK_R_WEAPON], b, a);
        m34_apply(&world[SK_R_WEAPON], e, c);
        trail_sample(a, c);
        break;
    }
    p->hp = player_chr.hp;  /* (the player cannot hit itself; kept for symmetry) */
    synced = p->hp;
    return hits;
}

CombatChr *combat_player(void) { return &player_chr; }

void combat_dummy_place(const float pos[3], float yaw)
{
    memset(&combat_dummy, 0, sizeof combat_dummy);
    memcpy(combat_dummy.pos, pos, 12);
    combat_dummy.yaw = yaw;
    combat_dummy.radius = DUMMY_RADIUS, combat_dummy.height = DUMMY_HEIGHT;
    combat_dummy.team = TEAM_DUMMY;
    combat_dummy.hp = combat_dummy.hp_max = DUMMY_HP;
    combat_dummy.def_phys = DUMMY_DEF_PHYS;
    combat_dummy.poise_max = DUMMY_POISE;
    combat_register(&combat_dummy);
}
