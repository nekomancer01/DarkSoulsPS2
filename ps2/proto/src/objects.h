/* Breakable MSB objects (pots o1495-o1497 at Firelink; tools/export_objects.py -> objects.bin in the area pack):
 * intact instances with cylinder collision, broken by weapon hits (combat sweeps), rolls / dashes into them and
 * landings on them into their pre-fractured pieces, which physics.c simulates until they settle and sink away. */
#ifndef OBJECTS_H
#define OBJECTS_H

#include "render.h"

struct Player;

/* objects.bin from the area pack (kept while the area is loaded); uploads the models' textures. Returns the number
 * of breakable instances (0 on a bad blob). */
int objects_init(GSGLOBAL *gs, const void *blob);
/* World blocks (src/world.c): at boot, block b's objects.bin: its types are loaded (VRAM, compacted: the blob can be
 * freed) and its placements kept; objects_block_add places them when the block turns on, objects_block_remove takes
 * them away (broken pieces included) when it turns off. */
#define OBJ_BLOCKS 4
int objects_block_types(GSGLOBAL *gs, int block, const void *blob);
void objects_block_add(int block);
void objects_block_remove(int block);
/* Per frame after player_update: player push-out from intact objects, roll / dash / landing breaks, piece sim */
void objects_update(struct Player *p, float dt);
/* Draw intact objects and live pieces near the camera (inside the VU1 skin chain, like the characters) */
void objects_draw(void);
/* A weapon capsule a-b of radius r swept this frame (combat.c's sweep hook): breaks every intact object it touches
 * whose ObjectParam hp the damage reaches. src = attacker position (sets the pieces' push). Returns breaks. */
int objects_sweep(const float a[3], const float b[3], float r, float damage, const float src[3]);
struct CombatAttack;
/* Adapter for combat.h's combat_sweep_hook: damage = the attack's physical rating x motion value */
int objects_sweep_hook(const float a[3], const float b[3], float r, const struct CombatAttack *atk);
/* Break instance i (debug / scripts); dir = horizontal push direction (may be NULL) */
void objects_break(int i, const float *dir);
/* Nearest intact instance to p (its position in out, may be NULL), -1 if none: test scripts */
int objects_nearest(const float p[3], float *out);
/* Push a vertical capsule (feet pos, radius r, height h) out of intact objects; returns 1 if it touched one */
int objects_push_capsule(float pos[3], float r, float h);

/* HUD: instances, broken ones, live / awake pieces, map queries last frame */
typedef struct { int instances, broken, pieces, awake, queries, us; } ObjectsStats;  /* us: objects_update time */
extern ObjectsStats objects_stats;

#endif
