/* Breakable MSB objects (pots o1495-o1497 at Firelink; tools/export_objects.py -> objects.bin in the area pack):
 * intact instances with cylinder collision, broken by weapon hits (combat sweeps), rolls / dashes into them and
 * landings on them into their pre-fractured pieces, which physics.c simulates until they settle and sink away.
 * Animated objects (objects.bin v3: doors, levers, gates, chests) play their Havok anims for events / ObjActs; their
 * hkx boxes ride their bones and push the player (objects_push_capsule). */
#ifndef OBJECTS_H
#define OBJECTS_H

#include "render.h"

struct Player;

/* objects.bin from the area pack (kept while the area is loaded); uploads the models' textures. Returns the number
 * of breakable instances (0 on a bad blob). */
int objects_init(GSGLOBAL *gs, const void *blob);
/* World blocks (src/world.c): at boot, block b's objects.bin: its types are loaded (compacted: the blob can be freed)
 * and its placements kept; objects_block_add places them when the block turns on and uploads the textures of its
 * breakable types no other block on holds (blob: its objects.bin again, read with the block), objects_block_remove
 * takes them away (broken pieces included) when it turns off and gives those textures back. Animated types keep
 * theirs from boot (each pack's shared texture model). */
#define OBJ_BLOCKS 5
int objects_block_types(GSGLOBAL *gs, int block, const void *blob);
void objects_block_add(int block, const void *blob);
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
/* EMEVD hooks (src/event.c): breakables of every loaded block by MSB entity id (objects.bin version 2). objects_entity:
 * the instance (position, broken state; pointers may be NULL) or -1; _break breaks it as a hit would, _restore puts it
 * back intact, _enable switches it on / off (off: not drawn, no collision). Each returns the instances changed. */
int objects_entity(int entity, float *pos, int *broken);
int objects_entity_break(int entity);
int objects_entity_restore(int entity);
int objects_entity_enable(int entity, int on);
/* Animated objects (objects.bin v3: doors, levers, gates, chests; src/ev_obj.c drives them) by MSB entity id.
 * _play starts Havok anim `anim` (a00_NNNN id) from its first frame and returns its length in s, -1 if the entity is
 * no animated object or has no such anim; _end puts it at the anim's last frame (EMEVD 2005[07], open doors after a
 * load); _state: the anim id last set (-1 reference pose), its time, 1 while it plays. _enable above hides them too. */
float objects_anim_play(int entity, int anim, int loop);
int objects_anim_end(int entity, int anim);
int objects_anim_state(int entity, int *anim, float *t, int *playing);

/* HUD: instances, broken ones, live / awake pieces, map queries last frame */
typedef struct { int instances, broken, pieces, awake, queries, us; } ObjectsStats;  /* us: objects_update time */
extern ObjectsStats objects_stats;

#endif
