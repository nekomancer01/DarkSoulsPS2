#ifndef PLAYER_H
#define PLAYER_H

#include "render.h"

/* One frame of controller input, already mapped from the pad (or the demo script) */
typedef struct Input {
    float mx, my;      /* left stick, -1..1; my > 0 = forward (away from camera) */
    float cx, cy;      /* right stick */
    int r1;            /* R1 pressed this frame */
    int circle;        /* Circle held */
} Input;

/* ST_JUMP / ST_LAND play one animation like ST_ATTACK / ST_ROLL; ST_FALL = airborne loop; ST_DEAD = fall death */
typedef enum { ST_IDLE, ST_MOVE, ST_ATTACK, ST_ROLL, ST_JUMP, ST_FALL, ST_LAND, ST_DEAD } PlayerState;

/* ESD request slots (PadManipulator byte +0x84 + slot; f100 / f102 / f108 argument) */
#define REQ_SLOTS 53
#define REQ_R1 0
#define REQ_BACKSTEP 10
#define REQ_ROLL_F 15
#define REQ_JUMP 42

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

#endif
