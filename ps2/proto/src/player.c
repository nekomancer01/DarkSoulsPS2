/* Player controller for the white-plane prototype.
 *
 * Timing, cancel windows, hit windows, root motion, loop speeds and attack stamina come from the game's own files via
 * player_data.h (tools/export_player.py). Which action runs when (R1 chain, roll, backstep, jump, rolling / running
 * attacks) is decided by c0000.esd through the ESD runtime (esd.c): this file turns the pad into the request slots the
 * ESD reads (f100 / f102 / f108), implements the character's env functions and commands (player_esd_env / _cmd), and
 * plays what the ESD asks for. Locomotion (walk / run / dash) is the engine's WalkAnim in DSR, so it stays here and
 * runs while the ESD enables it (c1_101). Stamina, turn and dash rules are the exe's (addresses below;
 * notes/EXE_FINDINGS.md). Values marked PROVISIONAL are not decoded yet; see notes/PORTING.md. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "player.h"
#include "esd.h"
#include "pack.h"
#include "player_data.h"
#include "model.h"
#include "map.h"
#include "collision.h"
#include "combat.h"
#include "save.h"
#include "bonfire.h"
#include "loading.h"
#include "sound.h"

/* the player anims (src/player_data.h, loaded from c0000.pak); after the includes: model.h has an `anims` field */
#define anims (player_data->anims)
#define PI 3.14159265f

/* Stamina is an int (chr+0x3F8); the setter FUN_140322cf0 clamps it to [-50, max] */
#define STAMINA_FLOOR (-50)
/* Player regen: PlayerIns vtable+0x1F0 FUN_14035ed00 = 45.0 (0x141A2BE40) + SpEffect staminaRecoverChangeSpeed
 * (none here), times the TAE 225 percent (ctx+0x9C), applied in FUN_14031fca0 */
#define STAMINA_REGEN 45.0f
/* Action costs come from the ESD itself: c1_1001(n) = AddStamina (FUN_14038dae0 case 0x3E9), rolls -30, backstep
 * -20, jump -30; its roll/backstep transitions require f1001() > 10 */
#define STEP_RELEASE_MIN 9     /* FUN_140398920 registers the roll on release only with stamina > 9 */
/* Dash (FUN_14031fca0 @0x14031FD40): -1 stamina per 0.1 s (0x141A2BC28) and no regen; a tick that leaves stamina
 * <= 0 blocks dashing for 3.0 s (0x141A2BC2C, checked in FUN_140378d00 with stamina >= 1) */
#define DASH_TICK 0.1f
#define DASH_LOCKOUT 3.0f
#define DASH_RUN_TIME 0.3f     /* WalkAnim_Twist FUN_1403a5030: run gait for > 0.3 s (0x1412E9AAC) before dashing */
#define ROLL_TAP 0.40f         /* FUN_140398920: Circle held > 0.4 s (0x141A2C0A0) = dash request, shorter = roll */
/* Turn rate: PlayerCtrl vtable+0xC8 FUN_140381fe0 sets TurnAnim+0x3C to 180 deg/s in the walk gait, else 720; the
 * TurnAnim step FUN_14039fcc0 limits the yaw change to rate * dt. Player TAEs have no SetTurnSpeed (224) override. */
#define TURN_WALK (180.0f * PI / 180.0f)
#define TURN_OTHER (720.0f * PI / 180.0f)
/* Walk/run: run above 0.97 + 0.01, back to walk below 0.97 - 0.01 (WalkAnim_Twist +0x21C / +0x220). ASSUMED pad
 * mapping: main.c gives a per-axis 0.18 dead zone with full tilt = 1; worn DualShock sticks often stop short of the
 * rim, so a radial tilt of STICK_FULL counts as 1.0 (PC magnitudes are already normalized by the input layer). */
#define RUN_ON 0.98f
#define RUN_OFF 0.96f
#define STICK_FULL 0.92f

static int in_window(int anim, int type, float t)
{
    const AnimData *a = &anims[anim];
    for (int i = 0; i < a->nwin; i++)
        if (a->win[i].type == type && t >= a->win[i].start && t < a->win[i].end)
            return 1;
    return 0;
}

/* SetSPRegenRatePercent (TAE 225, written to ctx+0x9C by FUN_14033fd90) windows are exported as type -2 - percent;
 * 100% when none is active (assumed: the per-frame reset was not located) or for anim -1 (idle, not exported) */
static float regen_rate(int anim, float t)
{
    if (anim < 0)
        return 1.0f;
    const AnimData *a = &anims[anim];
    for (int i = 0; i < a->nwin; i++)
        if (a->win[i].type <= -2 && a->win[i].type > EZ_MESSAGE && t >= a->win[i].start && t < a->win[i].end)
            return (-2 - a->win[i].type) / 100.0f;
    return 1.0f;
}

/* Root-motion forward/side offset at time t (30 fps samples, linear between them); anim forward is -z */
static void root_at(int anim, float t, float *fwd, float *side)
{
    const AnimData *a = &anims[anim];
    float f = t * 30.0f;
    int i = (int)f;
    if (i >= a->nroot - 1) {
        *fwd = -a->root[a->nroot - 1].z;
        *side = a->root[a->nroot - 1].x;
        return;
    }
    float k = f - i;
    *fwd = -(a->root[i].z + (a->root[i + 1].z - a->root[i].z) * k);
    *side = a->root[i].x + (a->root[i + 1].x - a->root[i].x) * k;
}

/* Duration of a looping animation (its 30 fps root-motion samples; the TAE length is not the loop length) */
static float loop_len(int anim) { return (anims[anim].nroot - 1) / 30.0f; }

static float wrap(float a)
{
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

static void turn_toward(Player *p, float target, float dt)
{
    float d = wrap(target - p->yaw), step = (p->gait == 0 ? TURN_WALK : TURN_OTHER) * dt;
    p->yaw = wrap(p->yaw + (d > step ? step : d < -step ? -step : d));
}

/* Max stamina from END: FUN_1402dfcd0 = clamp(int(80 + 80 * (END / 40)^1.4 + 0.5), 80, 160) (constants at
 * 0x141A2B58C..98); Warrior END 12 -> 95 */
static int stamina_max_for(int end)
{
    int m = (int)(80.0f + 80.0f * powf(end / 40.0f, 1.4f) + 0.5f);
    return m < 80 ? 80 : m > 160 ? 160 : m;
}

/* Stamina setter FUN_140322cf0 */
static void set_stamina(Player *p, int v)
{
    p->stamina = v < STAMINA_FLOOR ? STAMINA_FLOOR : v > p->stamina_max ? p->stamina_max : v;
}

int player_esd_env(EsdCtx *c, int id, const float *args, int nargs, float *out);
int player_esd_cmd(EsdCtx *c, int bank, int id, const float *args, int nargs);
static EsdProgram esd_prog;
static EsdCtx esd;
static int esd_ok;
static int esd_moving;       /* this frame's stick state, for actions the ESD starts */
static float esd_stick_yaw;

void player_init(Player *p)
{
    *p = (Player){0};
    p->stamina = p->stamina_max = stamina_max_for(START_END);
    p->hp = p->hp_max = combat_player_hp_max();  /* CalcCorrectGraph row 100 at VIT (FUN_1402df9a0): 594 */
    p->state = ST_IDLE;
    p->loco = ANIM_WALK;
    p->gait = -1;
    p->esd_loco = 1;
    /* c0000.esd, packed by tools/export_esd.py; the blob stays allocated for the ESD tables */
    static void *blob;
    if (!blob) {
        Pack pk;
        if (pack_open(&pk, PACK_ROOT "c0000.pak")) {
            blob = pack_load(&pk, "esd.bin", 0);
            pack_close(&pk);
        }
        esd_ok = esd_load(&esd_prog, blob);
    }
    esd_ctx_init(&esd, &esd_prog, ESD_PLAYER, p, player_esd_env, player_esd_cmd);
    esd_start(&esd);
}

static void start_anim(Player *p, PlayerState st, int anim, int stamina_cost)
{
    p->state = st;
    p->anim = anim;
    p->t = 0;
    p->speed = 0;
    set_stamina(p, p->stamina - stamina_cost);
}


/* Per-frame stamina, after FUN_14031fca0: dash ticks and the lockout timer, else regen scaled by the playing
 * animation's TAE 225 percent, integer part applied and the fraction carried */
static void stamina_tick(Player *p, int anim, float t, float dt)
{
    int dashing = p->state == ST_MOVE && p->loco == ANIM_DASH;
    if (dashing) {
        p->dash_acc += dt;
        if (p->dash_acc >= DASH_TICK) {
            set_stamina(p, p->stamina - 1);
            p->dash_acc = 0;
            if (p->stamina <= 0)
                p->dash_lock = DASH_LOCKOUT;
        }
    }
    if (p->dash_lock > 0)
        p->dash_lock -= dt;
    if (dashing || (p->ctx_1dc & 8))  /* ctx+0x1DC bit 3 (ESD c1_110) also stops regen */
        return;
    float acc = STAMINA_REGEN * regen_rate(anim, t) * dt + p->regen_acc;
    int n = (int)acc;
    p->regen_acc = acc - n;
    set_stamina(p, p->stamina + n);
}


/* Fallback without collision.bin: keep the player on the navmesh: follow its height; a move that leaves it slides
 * along the edge (x or z part alone), or is refused */
static void constrain_navmesh(Player *p, V3 old)
{
    float h;
    if (map_floor(p->pos.x, p->pos.z, old.y, &h)) {
        p->pos.y = h;
        return;
    }
    float dx = p->pos.x - old.x, dz = p->pos.z - old.z;
    int x_first = fabsf(dx) >= fabsf(dz);
    for (int k = 0; k < 2; k++) {
        int use_x = k == 0 ? x_first : !x_first;
        float nx = old.x + (use_x ? dx : 0), nz = old.z + (use_x ? 0 : dz);
        if (map_floor(nx, nz, old.y, &h)) {
            p->pos = v3(nx, h, nz);
            return;
        }
    }
    p->pos = old;
}

/* Airborne character proxy, FUN_1402bbe40: the velocity is clamped to |x|, |z| <= 6 (0x141A2AE54) and
 * -50 <= y <= 1 (0x141A2AE5C / 0x141A2AE58) m/s, then v += gravity * dt; gravity is the hkpWorldCinfo vector
 * (0, -9.8, 0) at 0x14130B1D0 (FUN_1402a36b0), its y scaled by the TAE gravity multiplier (proxy+0x1E4 <- ctx+0x448:
 * ChrActionFlag 27 Ignore Gravity, ChrActionGradient output 27, FUN_1403410f0) */
#define GRAVITY 9.8f
#define AIR_XZ_MAX 6.0f
#define AIR_UP_MAX 1.0f
#define AIR_DOWN_MAX 50.0f
/* Running jump: Circle released while dashing opens a 0.2 s window (FUN_140398920 +0x340 = 0x3E4CCCCD); pressing it
 * again requests slot 42 -> ESD group 1 state 36: f1001() > 10, c1_1001(-30), anim f110(5) = MoveParam dashEscapeF */
#define JUMP_WINDOW 0.2f
#define JUMP_COST 30
/* Root-motion reduction (TAE 236, FUN_140343660 / FUN_14037c250): delta *= 1 - clamp(v * (1 - mobility), 0, 1);
 * mobility = PlayerIns vtable+0x5A0 FUN_140360bf0 = FUN_1402e1c20(equip weight / max load): < 0.5 -> 0.9,
 * 0.5 -> 0.75, <= 1 -> 0.85, > 1 -> 0.6. Warrior: 21.4 / 52 (max load 40 + END: FUN_140362340 not decoded) */
#define MOBILITY 0.9f
/* Landing tiers come from the ESD (f224 fall height, f263 equip load); lethal from 20 m through the damage rate */
/* HitFilterID 14 lethal fall, 15 kill plane, 20/23 level exit (the planes under the cliffs: falling out of the map) */
#define COL_LETHAL ((1u << 14) | (1u << 15) | (1u << 20) | (1u << 23))
#define FALL_OUT_Y -250.0f /* below Firelink's lowest collision (-200): fell out of the map */
/* PROVISIONAL step easing (the proxy's step handling is not decoded): time constant and largest lag behind the floor */
#define STEP_EASE 0.04f
#define STEP_LAG 0.12f

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Value of a TAE ramp (Grad) of this type at t, or `none` outside every window */
static float grad_at(int anim, int type, float t, float none)
{
    const AnimData *a = &anims[anim];
    for (int i = 0; i < a->ngrad; i++) {
        const Grad *g = &a->grad[i];
        if (g->type == type && t >= g->start && t < g->end)
            return g->v0 + (g->v1 - g->v0) * (t - g->start) / (g->end - g->start);
    }
    return none;
}

static int playing_anim(const Player *p)
{
    return p->state == ST_ATTACK || p->state == ST_ROLL || p->state == ST_JUMP || p->state == ST_LAND ||
           p->state == ST_FALL || p->state == ST_DEAD;
}

static float gravity_scale(const Player *p)
{
    if (!playing_anim(p))
        return 1;
    if (in_window(p->anim, FLAG_IGNORE_GRAVITY, p->t))
        return 0;
    return grad_at(p->anim, 27, p->t, 1);
}

static void respawn(Player *p)
{
    int died = p->hp < 0;
    if (died) {
        loading_hold();    /* DSR reloads the map behind its loading screen (src/loading.c); a far respawn point's
                            * blocks load under it this frame (world.c settle) */
        save_request();    /* DSR autosaves on death; the next save_tick writes the respawn spot, full HP, deaths + 1 */
        bonfire_death(p);  /* bloodstain with the souls / humanity held */
    }
    p->deaths += died;  /* HUD: deaths so far (kill-plane respawns keep HP and do not count) */
    if (!died || !bonfire_spawn(&p->pos, &p->yaw))  /* a death: the respawn point (last bonfire) */
        map_spawn(&p->pos, &p->yaw);
    p->hp = p->hp_max;
    p->state = ST_IDLE;
    p->airborne = 0;
    p->vel = v3(0, 0, 0);
    player_esd_reset(p);  /* both ESD groups back to their start states, no pending requests */
    if (died)
        bonfire_revive(p);  /* Estus, enemies, events as a rest */
}

/* For combat.c: the TAE id of the attack animation playing, or -1 */
int player_attack_anim(const Player *p)
{
    return p->state == ST_ATTACK ? anims[p->anim].id : -1;
}

/* Touchdown after a fall of fall_y0 - y metres */
static void land(Player *p, float y)
{
    float h = p->fall_y0 - y;
    p->airborne = 0;
    p->vel = v3(0, 0, 0);
    p->land_h = h;  /* ESD f224 after the landing */
    if (p->state == ST_DEAD)
        return;  /* ESD group 0 state 58 sees f201 (on ground) and plays the impact 6067 (state 59) */
    /* fall damage = max HP x the exe's rate (combat_fall_rate: 0 under 5 m, (h / 20)^2 x equip-weight factor, 1 from
     * 20 m); ASSUMED: the rate scales max HP (the vtable+0x2A8 caller sits in the obfuscated .text) */
    int dmg = (int)(p->hp_max * combat_fall_rate(h));
    p->last_fall_damage = dmg;
    if (dmg > 0) {
        p->hp -= dmg;
        if (p->hp <= 0)
            p->hp = -1;  /* HP below 0 */
    }
    /* the landing animation is the ESD's (group 0 states 14 / 15 on f201): 31 / 32 / 33 = 1750 / 1755 / 1760 by f224
     * (fall height) and f263 (equip load), 97 = 6070 when f224 >= 20 or HP < 0, 99 = 6073 when HP < 0 under 10 m */
    if (p->state == ST_FALL || p->state == ST_LAND)
        p->state = ST_IDLE;
}

/* Resolve this frame's move against the map collision (collision.c), like the character proxy: on the ground the
 * capsule slides along walls and the feet follow the floor within the step limits; without a floor the player is
 * airborne: x/z keep the take-off velocity (a jump's root motion while it plays), y follows gravity, and the first
 * floor crossed is the landing. Crossing a lethal-fall / kill plane, or dropping out of the map, respawns. */
static void constrain(Player *p, V3 old)
{
    if (!col_tris()) {
        constrain_navmesh(p, old);
        return;
    }
    float pos[3] = {old.x, old.y, old.z}, dt = p->frame_dt;
    float dx = p->pos.x - old.x, dz = p->pos.z - old.z;
    ColHit hit;
    if (p->airborne && p->state != ST_JUMP)
        dx = p->vel.x * dt, dz = p->vel.z * dt;
    col_move(pos, dx, dz, COL_PLAYER_R, COL_PLAYER_H, COL_STEP_UP, COL_SOLID);
    if (!p->airborne) {
        if (col_floor(pos[0], pos[2], pos[1] + COL_STEP_UP, pos[1] - COL_STEP_DOWN, COL_SOLID, &hit)) {
            /* ease the feet onto each step instead of snapping (the Havok proxy moves over steps over several
             * frames); the lag stays under STEP_LAG so the step window above still finds the floor */
            float y = old.y + (hit.y - old.y) * (1 - expf(-dt / STEP_EASE));
            p->pos = v3(pos[0], clampf(y, hit.y - STEP_LAG, hit.y + STEP_LAG), pos[2]);
            return;
        }
        p->airborne = 1;
        p->fall_y0 = old.y;
        p->vel = v3(clampf(dx / dt, -AIR_XZ_MAX, AIR_XZ_MAX), 0, clampf(dz / dt, -AIR_XZ_MAX, AIR_XZ_MAX));
    } else if (p->state == ST_JUMP) {
        p->vel.x = clampf(dx / dt, -AIR_XZ_MAX, AIR_XZ_MAX);
        p->vel.z = clampf(dz / dt, -AIR_XZ_MAX, AIR_XZ_MAX);
    }
    p->vel.y = clampf(p->vel.y - GRAVITY * gravity_scale(p) * dt, -AIR_DOWN_MAX, AIR_UP_MAX);
    float y = pos[1] + p->vel.y * dt;
    if (col_floor(pos[0], pos[2], pos[1], y, COL_LETHAL, 0) || y < FALL_OUT_Y) {
        respawn(p);
        return;
    }
    /* landed: the highest floor between the last height (plus a step, for slopes) and the new one. The fall itself
     * (1550 / 1500) is the ESD's: group 0 enters state 14 on f200 (airborne without Disable Fall Motion); a fall of
     * 20 m or more lands as state 97 (lethal) through f224, and the damage applied here */
    if (col_floor(pos[0], pos[2], pos[1] + COL_STEP_UP, y, COL_SOLID, &hit)) {
        y = hit.y;
        land(p, y);
    }
    p->pos = v3(pos[0], y, pos[2]);
}

/* ---- ESD request model (exe: ESD ctx update FUN_140388020 -> FUN_140388790, TAE flags FUN_140340580) ---- */

static int esd_action(const Player *p) { return p->state == ST_ATTACK || p->state == ST_ROLL || p->state == ST_JUMP; }

/* Anim an event forced on the player (EMEVD 2003[18], player_event_anim), -1 none */
static int event_anim = -1;

float player_event_anim(Player *p, int id)
{
    for (int i = 0; i < ANIM_COUNT; i++)
        if (anims[i].key == id) {
            /* plays as an action outside both ESD groups: their c1_3 anims and stops are ignored until it ends
             * (ASSUMED: DSR's event anim slot outranks the ESD's, TECH_DEBT event-18) */
            start_anim(p, ST_ROLL, i, 0);
            p->esd_group = -1, p->esd_layer = 99;
            event_anim = i;
            return anims[i].length;
        }
    return 0;
}

int player_event_anim_playing(const Player *p) { return event_anim >= 0 && esd_action(p) && p->anim == event_anim; }

int player_event_anim_id(const Player *p) { return player_event_anim_playing(p) ? anims[p->anim].id : -1; }

float player_anim_message(int id, int msg)
{
    for (int i = 0; i < ANIM_COUNT; i++)
        if (anims[i].key == id)
            for (int w = 0; w < anims[i].nwin; w++)
                if (anims[i].win[w].type == EZ_MESSAGE - msg)
                    return anims[i].win[w].start;
    return -1;
}

/* The anim whose TAE drives the flags this frame: the playing one-shot, else the locomotion loop (idle a00_0000 is
 * not exported and has no ChrActionFlags) */
static int flag_anim(const Player *p)
{
    if (playing_anim(p))
        return p->anim;
    return p->state == ST_MOVE ? p->loco : -1;
}

static void set_slots(unsigned char *a, const unsigned char *slots, int n)
{
    for (int i = 0; i < n; i++)
        a[slots[i]] = 1;
}

/* One ChrActionFlag (TAE event 0) active this frame -> ctx fields, as FUN_140340580. "Key Input" flags open the input
 * buffer of their request slots (ctx+0x1A0), "Anim Cancel" flags enable them (ctx+0xF2); the rest are bits of
 * ctx+0xA0 / +0xA4. (The +0x127 / +0x15C slot arrays they also set feed the hold / R2 logic, not modelled.) */
static void apply_flag(Player *p, int f)
{
    static const unsigned char lh[] = {1, 19, 20, 51, 52}, magic[] = {2, 3},
        steps[] = {9, 10, 11, 12, 15, 16, 17, 18, 42},
        common[] = {0, 1, 2, 3, 9, 10, 11, 12, 15, 16, 17, 18, 19, 20, 42, 51, 52};
    static const struct { unsigned char flag; unsigned bit; } a0[] = {
        {7, 1}, {3, 2}, {5, 4}, {6, 8}, {8, 0x10}, {11, 0x20}, {78, 0x20}, {17, 0x40}, {18, 0x80}, {56, 0x100},
        {57, 0x200}, {19, 0x800}, {27, 0x1000}, {51, 0x2000}, {39, 0x4000}, {50, 0x8000}, {44, 0x10000},
        {28, 0x20000}, {43, 0x40000}, {20, 0x80000}, {32, 0x100000}, {49, 0x200000}, {55, 0x400000},
        {52, 0x800000}, {53, 0x1000000}, {54, 0x2000000}, {24, 0x4000000}, {45, 0x8000000}, {37, 0x10000000},
        {38, 0x20000000}, {40, 0x40000000}, {41, 0x80000000u}}, a4[] = {
        {42, 1}, {59, 2}, {60, 4}, {61, 0x20}, {63, 0x40}, {64, 0x80}, {83, 0x100}, {65, 0x200}, {66, 0x400},
        {84, 0x800}, {69, 0x1000}, {70, 0x2000}, {67, 0x4000}, {68, 0x8000}, {71, 0x10000}, {73, 0x20000},
        {74, 0x40000}, {75, 0x100000}, {80, 0x200000}, {81, 0x400000}, {76, 0x800000}, {72, 0x1000000},
        {82, 0x2000000}, {85, 0x4000000}, {88, 0x8000000}, {92, 0x10000000}, {94, 0x80000000u}};
    switch (f) {
    case 1: p->req_buffer[REQ_R1] = 1; return;                            /* Right Hand Attack Key Input */
    case 4: p->req_enable[REQ_R1] = 1; return;                            /* Right Hand Attack Anim Cancel */
    case 9: set_slots(p->req_buffer, lh, 5); return;                      /* Left Hand Attack Key Input */
    case 10: set_slots(p->req_buffer, magic, 2); return;                  /* Magic Key Input */
    case 16: set_slots(p->req_enable, lh, 5); return;                     /* Left Hand Attack Anim Cancel */
    case 25: set_slots(p->req_buffer, steps, 9); return;                  /* Step Movement Key Input */
    case 26: set_slots(p->req_enable, steps, 9); return;                  /* Step Movement Anim Cancel */
    case 29: set_slots(p->req_enable, magic, 2); return;                  /* Magic Anim Cancel */
    case 30: p->req_buffer[14] = 1; return;                               /* Item Key Input */
    case 31: p->req_enable[14] = 1; return;                               /* Item Anim Cancel */
    case 35: p->req_buffer[7] = 1; return;                                /* First Step RH Attack Key Input */
    case 36: p->req_enable[7] = 1; return;                                /* First Step RH Attack Anim Cancel */
    case 87: set_slots(p->req_buffer, common, sizeof common); return;     /* Common Key Input */
    case 97: p->no_magic = 1; return;                                     /* Disable Magic / Item Use Cancel */
    }
    for (unsigned i = 0; i < sizeof a0 / sizeof a0[0]; i++)
        if (a0[i].flag == f)
            p->ctx_a0 |= a0[i].bit;
    for (unsigned i = 0; i < sizeof a4 / sizeof a4[0]; i++)
        if (a4[i].flag == f)
            p->ctx_a4 |= a4[i].bit;
}

/* Before the ESD steps: this frame's flags from the playing anim's TAE windows, then the request latches */
static void update_requests(Player *p)
{
    memset(p->req_enable, 0, sizeof p->req_enable);
    memset(p->req_buffer, 0, sizeof p->req_buffer);
    p->ctx_a0 = p->ctx_a4 = 0;
    p->no_magic = 0;
    int anim = flag_anim(p);
    if (anim >= 0) {
        const AnimData *a = &anims[anim];
        float t = playing_anim(p) ? p->t : fmodf(p->loco_t, loop_len(anim));
        for (int i = 0; i < a->nwin; i++)
            if (a->win[i].type > 0 && t >= a->win[i].start && t < a->win[i].end)
                apply_flag(p, a->win[i].type);
    }
    /* FUN_140388020: no stamina, no buffered requests */
    if (p->stamina < 1)
        memset(p->latch, 0, sizeof p->latch);
    /* c1_146 (ctx+0x1DC bit 19) accepts every input while the state that set it lasts (FUN_140388790) */
    int all = (p->ctx_1dc >> 19) & 1;
    if (all && esd_state(&esd, 1) != p->accept_all_state) {
        memset(p->latch, 0, sizeof p->latch);
        p->ctx_1dc &= ~0x80000u;
        all = 0;
    }
    /* a press is buffered only inside its slot's Key Input window (FUN_140388790); requests cannot be made in the
     * air or while dead anyway */
    for (int i = 0; i < REQ_SLOTS; i++)
        if (all || p->req_buffer[i])
            p->latch[i] |= p->trig[i];
}

void player_update(Player *p, const Input *in, float cam_yaw, float dt)
{
    V3 old = p->pos;
    p->frame_dt = dt;
    float tilt = sqrtf(in->mx * in->mx + in->my * in->my);
    if (tilt > 1)
        tilt = 1;
    /* camera-relative stick heading */
    float stick_yaw = cam_yaw + atan2f(in->mx, in->my);
    int moving = tilt > 0.2f;

    /* gait with the exe's walk/run hysteresis; dashing needs the run gait for > DASH_RUN_TIME */
    float mag = tilt / STICK_FULL;
    p->gait = !moving ? -1 : (p->gait == 1 ? mag >= RUN_OFF : mag > RUN_ON);
    p->run_t = p->gait == 1 ? p->run_t + dt : 0;

    /* Circle: tap (release before ROLL_TAP) = roll / backstep, hold = dash, as in DS1; released while dashing and
     * pressed again within JUMP_WINDOW = running jump (FUN_140398920; the DSR L3-jump option is not modelled) */
    int roll_pressed = 0, jump_pressed = in->circle && p->circle_held == 0 && p->jump_window > 0;
    if (p->jump_window > 0)
        p->jump_window -= dt;
    if (in->circle)
        p->circle_held += dt;
    else {
        if (p->circle_held > 0 && p->circle_held < ROLL_TAP && p->stamina > STEP_RELEASE_MIN)
            roll_pressed = 1;
        if (p->circle_held >= ROLL_TAP && p->state == ST_MOVE && p->loco == ANIM_DASH && moving)
            p->jump_window = JUMP_WINDOW;
        p->circle_held = 0;
    }
    int dashing = in->circle && p->circle_held >= ROLL_TAP && p->gait == 1 && p->run_t > DASH_RUN_TIME &&
                  p->stamina >= 1 && p->dash_lock <= 0;

    /* Pad -> ESD request slots (PadManipulator FUN_140398920 / FUN_140396860: byte +0x84 + slot): R1 = 0, Circle tap
     * with the stick = roll 15 (forward; 16-18 are the lock-on directions), without = backstep 10, the running jump 42.
     * Nothing is requested in the air or while dead. */
    memset(p->trig, 0, sizeof p->trig);
    /* R1 also counts in the air: group 0's fall loop (state 15) turns it into the plunging attack (19 / 20) */
    if (p->state != ST_DEAD && in->r1)
        p->trig[REQ_R1] = 1;
    if (p->state != ST_FALL && p->state != ST_DEAD && !p->airborne) {
        if (jump_pressed && moving)
            p->trig[REQ_JUMP] = 1, p->jump_window = 0;
        else if (roll_pressed)
            p->trig[moving ? REQ_ROLL_F : REQ_BACKSTEP] = 1;
    }
    esd_moving = moving, esd_stick_yaw = stick_yaw;
    update_requests(p);

    int ticked = 0;
    if (p->state == ST_FALL || p->state == ST_DEAD) {
        /* airborne loop / fall death: no control; constrain() moves the body and detects the landing */
        p->t += dt;
        p->invincible = p->hitting = 0;
        if (p->state == ST_DEAD) {
            /* the ESD switches the death anims; the engine side respawns once the last one has played: 6067 (state 59,
             * death in the air), 6170 (98, lethal landing), 6173 (100, died on a short landing) */
            /* a death on the ground (enemy hit, hurt): the ESD's death anim, once it ends with the player on the floor
             * (ASSUMED: DSR fades to the load after YOU DIED, TECH_DEBT a4-4) */
            int ended = p->t >= anims[p->anim].length;
            if (ended && (p->anim == ANIM_FALL_DEATH_LAND || p->anim == ANIM_ESD_FALL_IMPACT_END ||
                          p->anim == ANIM_ESD_LAND_DEATH_END || (p->hp < 0 && !p->airborne && !anims[p->anim].loop))) {
                respawn(p);
                old = p->pos;  /* constrain() below resolves from the spawn, not the death site */
            }
        } else {
            stamina_tick(p, p->anim, p->t, dt);
        }
        ticked = 1;
    } else if (playing_anim(p)) {
        float f0, s0, f1, s1;
        root_at(p->anim, p->t, &f0, &s0);
        float keep = 1 - clampf(grad_at(p->anim, 236, p->t, 0) * (1 - MOBILITY), 0, 1);
        p->t += dt;
        root_at(p->anim, p->t, &f1, &s1);
        float df = (f1 - f0) * keep, ds = (s1 - s0) * keep, sy = sinf(p->yaw), cy = cosf(p->yaw);
        p->pos.x += sy * df + cy * ds;
        p->pos.z += cy * df - sy * ds;
        if (moving && !in_window(p->anim, FLAG_DISABLE_TURNING, p->t) && p->anim != event_anim)
            turn_toward(p, stick_yaw, dt);
        p->invincible = in_window(p->anim, FLAG_INVINCIBLE, p->t);
        p->hitting = in_window(p->anim, -1, p->t);
        stamina_tick(p, p->anim, p->t, dt);
        ticked = 1;
        /* landing is engine-driven (not an ESD state): it ends by itself or by a move cancel */
        if (p->state == ST_LAND && (p->t >= anims[p->anim].length || (moving && in_window(p->anim, FLAG_MOVE_CANCEL, p->t))))
            p->state = ST_IDLE;
    }

    /* an event anim has no ESD state to end it: it ends with its TAE (a loop plays until another one replaces it) */
    if (player_event_anim_playing(p) && p->t >= anims[p->anim].length && !anims[p->anim].loop)
        p->state = ST_IDLE;
    if (!player_event_anim_playing(p))
        event_anim = -1;
    /* the ESD: may start or end an action (player_esd_cmd) */
    esd.p = esd.chr = p, esd.in = in, esd.dt = dt;
    esd_update(&esd);

    if (!playing_anim(p) && p->state != ST_FALL && p->state != ST_DEAD) {
        p->invincible = p->hitting = 0;
        if (moving && p->esd_loco) {
            int loco = dashing ? ANIM_DASH : p->gait == 1 ? ANIM_RUN : ANIM_WALK;
            if (loco != p->loco || p->state != ST_MOVE) {
                /* keep the stride phase when switching walk/run/dash, as the loops are aligned at frame 0 */
                float ph = p->loco_t / loop_len(p->loco);
                p->loco = loco;
                p->loco_t = (ph - floorf(ph)) * loop_len(loco);
            }
            p->state = ST_MOVE;
            turn_toward(p, stick_yaw, dt);
            /* the loop's own root motion moves the character, as in the game (no constant speed) */
            float loop = loop_len(p->loco), t0 = fmodf(p->loco_t, loop), t1 = t0 + dt, f0, s0, f1, s1;
            root_at(p->loco, t0, &f0, &s0);
            if (t1 <= loop) {
                root_at(p->loco, t1, &f1, &s1);
            } else {  /* wrapped: rest of this cycle + start of the next */
                float fe, se, fw, sw;
                root_at(p->loco, loop, &fe, &se);
                root_at(p->loco, t1 - loop, &fw, &sw);
                f1 = fe + fw, s1 = se + sw;
            }
            p->speed = (f1 - f0) / dt;
            p->pos.x += sinf(p->yaw) * (f1 - f0);
            p->pos.z += cosf(p->yaw) * (f1 - f0);
            p->loco_t += dt;
            if (!ticked)
                stamina_tick(p, p->loco, t0, dt);
        } else {
            p->state = ST_IDLE;
            p->speed = 0;
            p->idle_t += dt;
            if (!ticked)
                stamina_tick(p, -1, 0, dt);
        }
    }
    if (p->on_map)
        constrain(p, old);
}

int player_esd_state(int group) { return esd_ok ? esd_state(&esd, group) : -1; }

int player_breaks_objects(const Player *p)
{
    int s = player_esd_state(1);
    return (p->state == ST_MOVE && p->loco == ANIM_DASH) ||
           (p->state == ST_ROLL && ((s >= 32 && s <= 35) || (s >= 37 && s <= 40)));
}

/* Back to the ESD's start states with no pending requests (respawn) */
void player_esd_reset(Player *p)
{
    memset(p->trig, 0, sizeof p->trig);
    memset(p->latch, 0, sizeof p->latch);
    p->ctx_1dc = 0;
    p->ctx_1e0 = p->ctx_454 = 0;
    p->esd_loco = 1;
    esd.p = esd.chr = p;
    esd_start(&esd);
}

/* ---- ESD env functions and commands of the character (EzStateActionEnv FUN_14038b540 / EzStateActionEvent
 * FUN_14038dae0; names in data/esd_names.json). Ids not handled here return 0 / do nothing (TECH_DEBT esd-1). ---- */

/* Steps (backstep, rolls, jump): the slots f108 accepts, mask 0x40000079e00 in FUN_140386af0 */
static int is_step(int slot) { return slot >= 0 && slot < 43 && ((0x40000079e00ULL >> slot) & 1); }

/* ctx+0xF2[slot] with ctx+0xF1 (flag 97) hiding the magic / item slots 2, 3, 14 */
static int enabled(const Player *p, int s)
{
    if (p->no_magic && (s == 14 || s == 2 || s == 3))
        return 0;
    return p->req_enable[s];
}

/* FUN_140323c60(chr, enabled): stamina >= 1, and the slot enabled or !FUN_14032c4f0(chr). FUN_14032c4f0 is 1 while
 * the chr has a partner link (handles chr+0x16C..+0x178 differ from the invalid handle 0x20FFFFFFFF at 0x141A2BC90:
 * throws); the port has none, so only stamina gates. (c0000 group 0 state 15 -> 19 tests f100(0) during a00_1500,
 * which opens no Anim Cancel window: the plunge needs this gate open.) */
static int gate(const Player *p, int s) { (void)s; return p->stamina >= 1; }

/* f102: FUN_140386910 (the per-slot timer ctx+0x320 belongs to hold-type inputs this build does not make) */
static int requested(const Player *p, int s) { return gate(p, s) && enabled(p, s) && p->latch[s]; }

/* Anim for c1_3(weaponSel, animId): FUN_140325f70 adds the hand's weapon motion (sel 1 right, 0 left, -1 the
 * two-handed one) as the aNN file; sel 2 / 3 are a00 ids as given */
static int anim_for(int sel, int id)
{
    int file = sel == 1 || sel == -1 ? WEAPON_MOTION_R : sel == 0 ? WEAPON_MOTION_L : 0;
    int key = file ? file * 10000 + id : id;
    for (int i = 0; i < ANIM_COUNT; i++)
        if (anims[i].key == key)
            return i;
    return -1;
}

int player_esd_env(EsdCtx *c, int id, const float *a, int n, float *out)
{
    Player *p = c->p;
    int s = n > 0 ? (int)a[0] : 0, slot = s >= 0 && s < REQ_SLOTS;
    float v = 0;
    switch (id) {
    case 0:     /* IsAnimEnd: no ESD anim (ctx+0x80 handle) or it ended, or a move cancel cut it (FUN_14037b3e0) */
    case 1:
        v = !(esd_action(p) || p->state == ST_DEAD) || p->t >= anims[p->anim].length ||
            (esd_moving && (p->ctx_a0 & 0x20));
        break;
    case 100: v = slot && gate(p, s) && p->trig[s]; break;                      /* IsButtonTriggered */
    case 101: v = 0; break;      /* IsButtonReleased: no hold-type pad inputs are mapped (only R1 / Circle taps) */
    case 102: v = slot && requested(p, s); break;                                 /* IsActionRequested */
    case 103: v = 0; break;      /* FUN_1403867b0: 1.0 while the slot's button is held (manipulator +0xF0[slot]) */
    case 104: v = 0; break;      /* weapon hold-style change request (no two-handing input) */
    case 105: v = 0; break;      /* ctx+0x42C[i]: event / gesture requests (none) */
    case 108: v = slot && is_step(s) && requested(p, s); break;                 /* IsStepRequested */
    case 109: v = 0; break;      /* PlayerIns vt+0x438: weapon / item switch request (none) */
    case 110: v = s >= 0 && s < 6 ? move_escape[s] : -1; break;                 /* GetRollAnimId */
    case 111:   /* FUN_140386bb0: FUN_140323c60(chr, ctx+0x49E any slot enabled) && manipulator +0x1C4 (some request
                 * made this frame) */
        for (int i = 0; i < REQ_SLOTS; i++)
            v = v || p->trig[i];
        v = v && gate(p, 0);
        break;
    case 112:   /* ctx+0x49D, set by FUN_140388790: some request is actionable now (f102 / f108 per slot) */
        for (int i = 0; i < REQ_SLOTS; i++)
            v = v || requested(p, i);
        break;
    case 207: v = 1; break;      /* GetWeaponHoldStyle: one-handed */
    case 211: v = p->ctx_1dc & 1; break;
    case 219: v = p->state != ST_MOVE ? 0 : p->loco == ANIM_DASH ? 3 : p->loco == ANIM_RUN ? 2 : 1; break;
    case 225:   /* GetWeaponMotionCategory(hand): FUN_1403261d0 returns the hold style for -1 when one-handed */
        v = s == 1 ? WEAPON_MOTION_R : s == 0 ? WEAPON_MOTION_L : s == -1 ? 1 : -1;
        break;
    case 226: v = (p->ctx_1dc >> 5) & 1; break;
    case 227: v = -1; break;     /* selected item motion type: no item selected */
    case 231: v = 255; break;    /* selected spell motion type: 0xFF = not castable (the Warrior has no spells) */
    case 238: case 239: case 240: v = gate(p, id - 219) && p->trig[id - 219]; break;  /* f100(19 / 20 / 21) */
    case 241: case 242: v = requested(p, id - 241 + 19); break;                 /* f102(19 / 20) */
    case 249: v = (p->ctx_1dc >> 9) & 1; break;
    case 253: v = (p->ctx_1dc >> 1) & 1; break;
    case 257: v = 0; break;      /* movement set (MoveParam row): row 0, as f110 */
    case 267: v = p->ctx_1e0; break;
    case 271: v = (p->ctx_a4 >> 10) & 1; break;    /* flag 66 Special Transition */
    case 278: v = s == 0 ? GUARD_MOTION_L : s == 1 ? GUARD_MOTION_R : 0; break;  /* guardmotionCategory */
    case 287: v = (p->ctx_1dc >> 15) & 1; break;
    case 293: v = p->ctx_454; break;
    case 298: v = (p->ctx_a4 >> 11) & 1; break;    /* flag 84 Special Transition 2 */
    case 299: v = (p->ctx_1dc >> 18) & 1; break;
    case 300: v = (p->ctx_a0 >> 20) & 1; break;    /* flag 32 Generic Anim Cancel */
    case 1000: v = p->hp; break;
    case 1001: v = p->stamina; break;
    case 1003: v = n >= 2 ? (float)((int)a[0] + rand() % ((int)a[1] - (int)a[0] + 1)) : 0; break;  /* RandInt */
    case 1004: v = n >= 2 ? a[0] + (a[1] - a[0]) * (rand() / (float)RAND_MAX) : 0; break;           /* RandFloat */
    default: return 0;
    }
    *out = v;
    return 1;
}

/* ctx+0x1DC bit commands (FUN_14038dae0): c1_<id> -> bit */
static int flag_cmd_bit(int id)
{
    switch (id) {
    case 107: return 2;
    case 110: return 3;   /* DisableStaminaRegen (FUN_14031fca0 skips regen) */
    case 111: return 4;
    case 113: return 5;
    case 116: return 6;
    case 117: return 7;
    case 118: return 8;
    case 119: case 130: return 10;
    case 122: return 11;
    case 138: return 14;
    }
    return -1;
}

int player_esd_cmd(EsdCtx *c, int bank, int id, const float *a, int n)
{
    Player *p = c->p;
    if (bank != 1)
        return 0;
    int b = flag_cmd_bit(id);
    if (b >= 0) {
        p->ctx_1dc = (p->ctx_1dc & ~(1u << b)) | ((n > 0 && a[0] != 0) ? 1u << b : 0);
        return 1;
    }
    switch (id) {
    case 3:     /* SetAnim(weaponSel, animId, layer, blend, loop) */
    case 12:    /* (weaponSel, animA, animB, layer, blend, loop, f272()): landings 1750 / 1755 (group 0 states 31 / 32);
                 * animA is played, the animB blend by f272 is not modelled (TECH_DEBT esd-6) */
    case 4: {   /* SetMoveSyncAnim: same here (the speed sync is not modelled) */
        /* HP < 0: only the death reactions run (group 0 states 57-59: 6065 / 6066 / 6067 for a fall death); they
         * play as ST_DEAD, which the engine side (map-collision) ends with the respawn. Otherwise the airborne loop
         * and death states belong to the engine and ESD anims are not started there. */
        int dead = p->hp < 0;
        if (n < 2 || (!dead && (p->state == ST_FALL || p->state == ST_DEAD)))
            return 1;
        int anim = (int)a[1] < 0 ? -1 : anim_for((int)a[0], (int)a[1]);
        /* Layers are anim-player slots (FUN_140385550: tables 0x14132D000 / 0x14132D030, layer 6 -> slots 7 / 23,
         * 7 -> 8 / 24, 9 -> 11 / 26). ASSUMED (TECH_DEBT esd-9): the higher slot wins, so a group cannot replace or
         * stop the other group's ESD anim on a higher or equal layer (group 0's fall 1550 / 1500 on 7 and plunge
         * 3800 on 6 against group 1's R1 3000 on 6); the other group then waits in its state until f0 */
        int layer = id == 12 ? (n > 3 ? (int)a[3] : 0) : (n > 2 ? (int)a[2] : 0);
        if (esd_action(p) && p->esd_group != c->group && layer <= p->esd_layer)
            return 1;
        if (anim < 0) {  /* -1 stops the action (or an anim this build does not carry: TECH_DEBT esd-2) */
            if (esd_action(p))
                p->state = ST_IDLE;
            return 1;
        }
        p->esd_layer = layer, p->esd_group = c->group;
        if (dead) {
            if (p->state != ST_DEAD || p->anim != anim)
                start_anim(p, ST_DEAD, anim, 0);
            return 1;
        }
        PlayerState st = anim == ANIM_JUMP ? ST_JUMP : anims[anim].key >= 10000 ? ST_ATTACK : ST_ROLL;
        if (esd_moving && anim != ANIM_BACKSTEP)
            p->yaw = esd_stick_yaw;
        /* attack stamina: BehaviorParam_PC of the anim's AttackBehavior, taken at the start (TECH_DEBT esd-3) */
        start_anim(p, st, anim, anims[anim].stamina);
        /* a new ESD anim clears the buffered requests unless ctx+0x1DC bit 19 is set (FUN_140388670) */
        if (!(p->ctx_1dc & 0x80000))
            memset(p->latch, 0, sizeof p->latch);
        return 1;
    }
    case 101:   /* SetLocomotionEnabled (ChrCtrl+0x1B0) */
        p->esd_loco = n > 0 && a[0] != 0;
        return 1;
    case 132: p->ctx_1dc |= 0x2000; return 1;
    case 133: p->ctx_1e0 = n > 0 ? (unsigned char)(int)a[0] : 0; return 1;
    case 140: p->ctx_454 = n > 0 ? (unsigned char)(int)a[0] : 0; return 1;
    case 146: p->ctx_1dc |= 0x80000, p->accept_all_state = esd_state(c, 1); return 1;  /* accept all input */
    case 147: p->ctx_1dc |= 0x100000; return 1;
    case 1001:  /* AddStamina */
        if (n > 0)
            set_stamina(p, p->stamina + (int)a[0]);
        return 1;
    case 1100: case 1101: return 1;  /* no effect in the exe's dispatcher either */
    }
    return 0;
}

const char *player_state_name(const Player *p)
{
    static char buf[48];
    if (esd_ok && (esd_action(p) || p->state == ST_IDLE || p->state == ST_MOVE)) {
        const char *n = p->state == ST_IDLE ? "IDLE" : p->state == ST_MOVE ? (p->loco == ANIM_DASH ? "DASH" :
                        p->loco == ANIM_RUN ? "RUN" : "WALK") : anims[p->anim].name;
        int id = p->state == ST_MOVE ? anims[p->loco].key : esd_action(p) ? anims[p->anim].key : 0;
        snprintf(buf, sizeof buf, "ESD %d/%d %s %d", esd_state(&esd, 0), esd_state(&esd, 1), n, id);
        return buf;
    }
    switch (p->state) {
    case ST_IDLE: return "IDLE";
    case ST_MOVE: return p->loco == ANIM_DASH ? "DASH A00_0535" : p->loco == ANIM_RUN ? "RUN A00_0500" : "WALK A00_0200";
    default:
        snprintf(buf, sizeof buf, "%s %d", anims[p->anim].name, anims[p->anim].key);
        return buf;
    }
}

/* Crossfade length when the playing animation changes; the ESD's c1_3 blend argument is 0.1-0.2 s */
#define BLEND_TIME 0.15f

void player_draw(const Player *p)
{
    static BoneLocal cur[MAX_BONES], from[MAX_BONES], target[MAX_BONES];
    static int last_anim = -2;
    static float blend = 1;
    int anim, loop;
    float t;
    switch (p->state) {
    case ST_MOVE: anim = model_anim(anims[p->loco].id), t = p->loco_t, loop = 1; break;
    case ST_ATTACK:
    case ST_ROLL:
    case ST_JUMP:
    case ST_LAND:
    case ST_FALL:
    case ST_DEAD: anim = model_anim(anims[p->anim].id), t = p->t, loop = anims[p->anim].loop; break;
    default: anim = model_anim(0), t = p->idle_t, loop = 1;
    }
    {  /* the playing anim's TAE sound events (footsteps, swings, armor, voice: src/sound.c) */
        int key = p->state == ST_MOVE ? anims[p->loco].key : p->state == ST_IDLE ? 0 : anims[p->anim].key;
        float len = anim >= 0 ? model_anim_length(anim) : 0, st = loop && len > 0 ? fmodf(t, len) : t;
        snd_anim(SND_OWNER_PLAYER, 0, key, st, (const float *)&p->pos);  /* V3 as float[3] */
    }
    if (anim != last_anim) {
        if (last_anim != -2)
            memcpy(from, cur, sizeof cur);
        blend = last_anim == -2 ? 1 : 0;
        last_anim = anim;
    }
    model_sample(anim, t, loop, target);
    blend += p->frame_dt / BLEND_TIME;
    if (blend < 1)
        model_blend(from, target, blend, cur);
    else
        memcpy(cur, target, sizeof cur);
    model_draw(cur, p->pos, p->yaw, p->invincible);
}
