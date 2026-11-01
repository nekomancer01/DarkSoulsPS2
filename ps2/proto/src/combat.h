/* HP and damage core: hurtboxes of characters (player, enemies, the debug dummy), attack capsule sweeps during TAE
 * attack windows, the physical damage formula, poise / super armour and hit reactions. Values from the exe and the
 * params where decoded (notes/PORTING.md "HP and damage"); PROVISIONAL ones are marked in combat.c. */
#ifndef COMBAT_H
#define COMBAT_H

#define COMBAT_MAX_CHRS 32

enum { TEAM_PLAYER, TEAM_ENEMY, TEAM_DUMMY };

/* One damageable character. The owner fills it, keeps it alive while registered and updates pos/yaw every frame. */
typedef struct CombatChr {
    /* owner-set */
    float pos[3];          /* feet, map space */
    float yaw;             /* facing; forward = (sin yaw, 0, cos yaw) */
    float radius, height;  /* hurtbox: vertical capsule (NpcParam hitRadius / hitHeight) */
    int team;              /* attacks never hit their own team */
    int hp, hp_max;
    float def_phys;        /* physical defence (flat, the character's total) */
    float absorb_phys;     /* physical absorption 0..1 (armour cut), applied after defence */
    float poise_max;       /* 0 = always staggered */
    int invincible;        /* i-frames (TAE ChrActionFlag 8) this frame */
    int super_armor;       /* TAE flag 24 this frame: damage taken, no hit reaction */
    /* combat-set (read by the owner / ESD env functions) */
    float poise;           /* current poise; regenerates in combat_update */
    int hit_count;         /* +1 per hit taken (owners compare with their last seen value) */
    int last_damage;       /* damage of the latest hit */
    int damage_level;      /* latest hit reaction level (ESD f236): 0 none (poise held), 1 small, 2 medium, ... */
    float hit_dir;         /* direction the latest hit came from, radians relative to the character's facing */
    int dead;              /* hp reached 0 (hp itself goes below 0 for the ESD's f1000() < 0 tests) */
    int new_hit;           /* a hit landed since the owner last cleared it (the player's: cleared in
                            * combat_player_frame, after its ESD saw it as f236) */
} CombatChr;

/* An attack: what the attacker's TAE AttackBehavior window + BehaviorParam/AtkParam rows resolve to */
typedef struct CombatAttack {
    int team;              /* attacker's team */
    int atk_param;         /* AtkParam row (for the HUD / debugging) */
    float phys;            /* attack rating, physical (weapon base + stat scaling) */
    float atk_phys_rate;   /* AtkParam atkPhysCorrection / 100 (motion value) */
    float poise_dmg;       /* AtkParam atkStam / poise damage */
    int damage_level;      /* AtkParam damageLevel */
    float src_pos[3];      /* attacker position, for the hit direction */
    int window;            /* attack window id: one target is hit at most once per window */
} CombatAttack;

int combat_register(CombatChr *c);    /* returns a slot (>= 0) or -1 when full */
void combat_unregister(CombatChr *c);
/* Capsule from a to b with radius r (the attack's dummy-poly segment this frame); hits every registered hurtbox
 * of another team that it touches and that was not already hit in this window. Returns the number of hits. */
int combat_sweep(const float a[3], const float b[3], float r, const CombatAttack *atk);
/* Called by every combat_sweep with the same capsule (breakable objects: main.c sets objects_sweep_hook), so any
 * attacker's weapon breaks them. NULL = none. */
extern int (*combat_sweep_hook)(const float a[3], const float b[3], float r, const CombatAttack *atk);
/* Apply one hit directly (falls, scripted damage, tests); returns the damage dealt (0 if invincible) */
int combat_apply(CombatChr *target, const CombatAttack *atk);
/* Raw damage, no hurtbox state: the physical damage formula */
int combat_damage(float atk_phys, float def_phys, float absorb);
/* Per frame: poise regeneration */
void combat_update(float dt);

/* Debug dummy target (COMBAT_DUMMY builds / HUD): NpcParam 206000's hurtbox and defence */
extern CombatChr combat_dummy;
void combat_dummy_place(const float pos[3], float yaw);

/* Latest hit anywhere, for the HUD / debugging */
typedef struct { int damage, atk_param; float atk, def; int hp_after, level; } CombatLast;
extern CombatLast combat_last;

/* Formulas (exe; see combat.c): CalcCorrectGraph row lookup, player max HP (VIT), Longsword attack rating (STR/DEX),
 * defence-reduced damage, player fall damage as a fraction of max HP (0 under 5 m, 1 = lethal from 20 m) */
float combat_graph(const float *row15, float x);
int combat_player_hp_max(void);
float combat_player_ar(void);
float combat_damage_f(float atk, float def);
float combat_fall_rate(float h);

/* Player glue, once per frame after player_update: syncs the player's hurtbox (hp both ways, i-frames) and sweeps the
 * weapon's AtkParam hit capsules (dummy polys 100-110-120 on R_Weapon) during the TAE attack windows of anim_id at
 * time t. Returns the number of hits this frame. */
struct Player;
int combat_player_frame(struct Player *p, int anim_id, float t, float dt);
CombatChr *combat_player(void);
extern float combat_hit_a[3], combat_hit_b[3];  /* the blade capsule this frame (debug drawing) */
extern int combat_hit_active;

#endif
