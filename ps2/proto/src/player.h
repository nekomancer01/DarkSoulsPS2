#ifndef PLAYER_H
#define PLAYER_H

#include "render.h"

/* One frame of controller input, already mapped from the pad (or the demo script) */
typedef struct Input {
    float mx, my;      /* left stick, -1..1; my > 0 = forward (away from camera) */
    float cx, cy;      /* right stick */
    int r1;            /* R1 pressed this frame */
    int circle;        /* Circle held */
    /* held this frame (presses are found against the previous frame in player.c): R1, R2, L1, L2 */
    int r1_held, r2_held, l1_held, l2_held;
} Input;

/* ST_JUMP / ST_LAND play one animation like ST_ATTACK / ST_ROLL; ST_FALL = airborne loop; ST_DEAD = fall death */
typedef enum { ST_IDLE, ST_MOVE, ST_ATTACK, ST_ROLL, ST_JUMP, ST_FALL, ST_LAND, ST_DEAD } PlayerState;

/* ESD request slots (PadManipulator byte +0x84 + slot; f100 / f102 / f108 argument) */
#define REQ_SLOTS 53
#define REQ_R1 0
#define REQ_BACKSTEP 10
#define REQ_ROLL_F 15
#define REQ_JUMP 42
/* PadManipulator FUN_140396860 writes: R1 -> slots 0 and 7, R2 -> 5, L1 -> 1 and 21 (21 = the guard hold). L2 ->
 * 20: c0000 group 1 state 0 enters the shield parry (86) on f239 = f100(20), the left-hand weapon attack (55) on
 * f238 = f100(19); ASSUMED that L2 with a shield is slot 20 (the manipulator's write of +0x98 was not found) */
#define REQ_R1_STEP 7
#define REQ_L1 1
#define REQ_R2 5
#define REQ_L2 20
#define REQ_GUARD 21
typedef struct Player {
    V3 pos;
    float yaw;          /* facing; forward = (sin yaw, 0, cos yaw) */
    PlayerState state;
    int anim;           /* ANIM_* playing in ST_ATTACK / ST_ROLL */
    float t;            /* time in that animation */
    float speed;        /* current locomotion speed, m/s */
    int loco;           /* ANIM_WALK / ANIM_RUN / ANIM_DASH while moving */
    float loco_t;       /* time in the looping walk/run/dash animation, advanced by speed */
    float idle_t;
    int stamina, stamina_max;  /* int, as chr+0x3F8 / +0x3FC in the game */
    float regen_acc;    /* fractional regen carried between frames (chr+0x1E0) */
    float dash_acc;     /* dash time since the last -1 tick (chr+0x1F0) */
    float dash_lock;    /* no-dash timer after a dash emptied stamina (chr+0x1F4) */
    int gait;           /* -1 no stick, 0 walk, 1 run (WalkAnim_Twist +0x140) */
    float run_t;        /* time in the run gait (WalkAnim_Twist +0x1B8) */
    /* ESD request model, as the exe's ESD context (ctx = ChrCtrl+0x48) and PadManipulator keep it */
    unsigned char trig[REQ_SLOTS];        /* pad requests made this frame (manipulator +0x84[slot]; f100) */
    unsigned char latch[REQ_SLOTS];       /* buffered requests (ctx+0x468[slot]; f102 / f108) */
    unsigned char req_enable[REQ_SLOTS];  /* ctx+0xF2[slot]: "Anim Cancel" ChrActionFlags of the playing anim */
    unsigned char req_buffer[REQ_SLOTS];  /* ctx+0x1A0[slot]: "Key Input" ChrActionFlags: presses latch only here */
    /* hold-type requests (FUN_140388790): the pad's held state and hold time (manipulator +0xF0[slot]), hold windows
     * opened by ChrActionFlags (ctx+0x127 / +0x15C, FUN_140340580), the hold latch (ctx+0x3F4), the held result
     * read by f220 / f243 (ctx+0x2E1) and the slot-21 hold timer (ctx+0x320) */
    unsigned char pad_held[REQ_SLOTS], pad_prev[REQ_SLOTS];
    float hold_time[REQ_SLOTS];
    unsigned char hold_en[REQ_SLOTS], hold_acc[REQ_SLOTS], hold_latch[REQ_SLOTS], held[REQ_SLOTS];
    float hold_timer21;
    /* upper-body layer (ESD c1_4 / c1_13 on layer 4: the guard poses) over the locomotion */
    int ovl_anim;          /* ANIM_* or -1 */
    float ovl_t;
    int ovl_loop;
    int ovl_cmd;           /* ESD command that started it: 3 (c1_3, ends f0) or 4 / 13 (end f1) */
    int motion_mode;       /* c1_109 (chr+0x270): 11 in every guard state of group 1 */
    int guarding;          /* guard up: an upper-body guard anim in motion mode 11 */
    int guard_hit;         /* a guarded hit landed this frame (f237 picks the guard row) */
    int parrying;          /* the parry anim's Parry attack window (TAE AttackType 64) is open */
    float fwd_flick;       /* time since the stick went from the dead zone to forward (f212, kick / jump attack) */
    float prev_tilt;
    unsigned ctx_a0, ctx_a4;              /* ChrActionFlag bit sets (ctx+0xA0 / +0xA4) */
    unsigned ctx_1dc;                     /* ESD command flags (ctx+0x1DC; c1_107 ... c1_147) */
    int no_magic;                         /* ctx+0xF1 (flag 97): magic / item slots 2, 3, 14 disabled */
    int accept_all_state;                 /* group-1 state where c1_146 set ctx+0x1DC bit 19 */
    unsigned char ctx_1e0, ctx_454;       /* c1_133 / c1_140 values (read by f267 / f293) */
    int esd_layer, esd_group;  /* c1_3 layer (anim slot priority) and ESD group of the playing ESD anim */
    int esd_loco;       /* ESD c1_101: locomotion (WalkAnim) enabled */
    float circle_held;  /* seconds Circle has been down: tap = roll / backstep, hold = dash */
    int invincible, hitting;
    float frame_dt;     /* last update's dt, for animation blending in player_draw */
    int on_map;         /* walk on the map's navmesh instead of the infinite plane */
    int hp, hp_max;     /* max from VIT (combat_player_hp_max, 594 for the Warrior); < 0 when dead (ESD f1000() < 0) */
    int last_fall_damage;  /* HP lost on the latest landing (HUD) */
    int deaths;            /* deaths so far (HUD) */
    float land_h;          /* height of the latest fall at its landing (ESD f224 once on the ground) */
    int airborne;       /* character proxy in the air (no floor within the step limits) */
    V3 vel;             /* airborne velocity, m/s: x/z carried from take-off, y from gravity */
    float fall_y0;      /* height where the player left the ground (fall distance = fall_y0 - y) */
    float jump_window;  /* FUN_140398920 +0x340: 0.2 s after releasing Circle while dashing, a new press jumps */
} Player;

void player_init(Player *p);
/* cam_yaw: camera heading, so stick input is camera-relative */
void player_update(Player *p, const Input *in, float cam_yaw, float dt);
void player_draw(const Player *p);
/* Restart the ESD (both groups at their first state, requests cleared), e.g. after a respawn */
void player_esd_reset(Player *p);
/* Current ESD state id of a group (0 airborne / damage, 1 actions), -1 without an ESD (test scripts, HUD) */
int player_esd_state(int group);
/* Rolling / backstepping (ESD group 1 states 32-35, 37-40) or dashing: breaks objects it runs into (objects.c) */
int player_breaks_objects(const Player *p);
/* TAE id of the attack animation playing (combat.c weapon sweeps), or -1 */
int player_attack_anim(const Player *p);
const char *player_state_name(const Player *p);
/* EMEVD 2003[18] on the player: play c0000 a00 anim `id` (exported ones only, tools/export_player.py) with its root
 * motion, outside the ESD; its length in s, 0 if this build does not carry it */
float player_event_anim(Player *p, int id);
int player_event_anim_playing(const Player *p);
int player_event_anim_id(const Player *p);  /* the event anim playing (its a00 id), -1 none */
/* Time of TAE 231 EzState message `msg` (the event Lua's OnCheckEzStateMessage) in anim `id`, -1 if it has none */
float player_anim_message(int id, int msg);

#endif
