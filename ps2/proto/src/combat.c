/* HP and damage core (combat.h). Formulas from DarkSoulsRemastered.exe, constants at the addresses given:
 * - CalcCorrectGraph lookup FUN_1402dfb70 (max HP = row 100 at VIT, FUN_1402df9a0; weapon stat scaling)
 * - weapon stat correction FUN_1402dacd0, combined for STR + DEX in FUN_1402d9cb0
 * - physical defence FUN_1402d9890 (0x141A2B5F0..60C)
 * - player fall damage rate PlayerIns vtable+0x2A8 FUN_14035f750 -> FUN_1402df3a0 / FUN_1402df580 (0x141A2B61C..668) */
#include <math.h>
#include <string.h>
#include "combat.h"
#include "combat_data.h"
#include "player.h"
#include "model.h"
#include "vmath.h"

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

int combat_player_hp_max(void) { return (int)combat_graph(graph_100, PL_VIT); }  /* Warrior VIT 11 -> 594 */

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
    float w = PL_EQUIP_WEIGHT, v = 1;
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

int combat_apply(CombatChr *t, const CombatAttack *atk)
{
    if (t->invincible)
        return 0;
    int dmg = combat_damage(atk->phys * atk->atk_phys_rate, t->def_phys, t->absorb_phys);
    t->hp -= dmg;
    t->hit_count++;
    t->new_hit = 1;
    t->last_damage = dmg;
    /* poise: AtkParam damage level unless the hit is absorbed by poise / super armour */
    int level = atk->damage_level;
    if (t->super_armor) {
        level = 0;
    } else if (t->poise_max > 0) {
        t->poise -= atk->poise_dmg;
        if (t->poise > 0)
            level = 0;
        else
            t->poise = t->poise_max;
    }
    t->damage_level = level;
    float dx = atk->src_pos[0] - t->pos[0], dz = atk->src_pos[2] - t->pos[2], a = atan2f(dx, dz) - t->yaw;
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    t->hit_dir = a;
    if (t->hp <= 0) {
        t->hp = -1;  /* the ESD death branches test f1000() < 0 */
        t->dead = 1;
    }
    for (int i = 0; i < COMBAT_MAX_CHRS; i++)
        if (chrs[i] == t)
            since_hit[i] = 0;
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
    if (hp <= 0 && (p->hp > 0 || player_chr.hp != synced))
        hp = -1;  /* dead: the ESD death branches test f1000() < 0 */
    p->hp = player_chr.hp = synced = hp;
    player_chr.hp_max = p->hp_max;
    if (hp > 0)
        player_chr.dead = 0;  /* respawned */
    player_chr.invincible = p->invincible;
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
        /* the pose at t (no crossfade: attack windows start long after the 0.15 s blend), R_Weapon to world as
         * model_draw places the model: rotate by yaw + pi (model faces -Z), translate to the feet */
        static BoneLocal pose[MAX_BONES];
        static M34 world[SK_BONES_MAX];  /* gamedata_init refuses more bones */
        model_sample(model_anim(anim_id), t, 0, pose);
        float c = cosf(p->yaw + 3.14159265f), s = sinf(p->yaw + 3.14159265f);
        M34 place = {{c, 0, s, 0, 1, 0, -s, 0, c, p->pos.x, p->pos.y, p->pos.z}};
        for (int i = 0; i <= SK_R_WEAPON; i++) {
            M34 local = m34_from_qt(pose[i].q, pose[i].t);
            world[i] = sk_parent[i] < 0 ? m34_mul(&place, &local) : m34_mul(&world[(int)sk_parent[i]], &local);
        }
        CombatAttack atk = {TEAM_PLAYER, aw->atk_param, combat_player_ar(), aw->phys_corr / 100.0f,
                            WEP_POISE_DMG * aw->sa_corr / 100.0f, aw->dmg_level,
                            {p->pos.x, p->pos.y, p->pos.z}, window};
        for (int h = 0; h < aw->nhit; h++) {
            const float *d1 = dummy_pos(aw->hit[h].dp1), *d2 = dummy_pos(aw->hit[h].dp2);
            if (!d1 || !d2)
                continue;
            float a[3], b[3];
            m34_apply(&world[SK_R_WEAPON], d1, a);
            m34_apply(&world[SK_R_WEAPON], d2, b);
            if (h == 0)
                memcpy(combat_hit_a, a, 12), memcpy(combat_hit_b, b, 12), combat_hit_active = 1;
            hits += combat_sweep(a, b, aw->hit[h].r, &atk);
        }
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
