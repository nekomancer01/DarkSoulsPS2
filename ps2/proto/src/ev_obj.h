/* EMEVD object ops (bank 2005 and the object targets of 2003[18]) for src/event.c, on src/objects.c: animated objects
 * (doors, levers, gates, chests: objects.bin v3) play and end their Havok anims, ObjActs play their ObjActParam
 * objAnimId when used, breakables break / restore, objects switch on / off. Elevators (o1460 at Firelink) stay static
 * map objects (the moving platform is A9's): their anims take their DSR length, so the call / arrive events wait and
 * set their flags as in DSR. */
#ifndef EV_OBJ_H
#define EV_OBJ_H

/* 2003[18] Force Animation Playback on an object: 1 when entity is an animated object with that anim (*len = its
 * length in s, for "wait for completion"), 0 otherwise (a character or an object the port does not animate) */
int ev_obj_anim(int entity, int anim, int loop, float *len);
/* 2005[07] End Of Animation: the object holds the anim's last frame; 0 if it is no animated object */
int ev_obj_end_anim(int entity, int anim);
/* 2005[03] (De)activate Object: breakables and animated objects (the event colliders are event.c's) */
int ev_obj_enable(int entity, int on);
/* 2005[01] / [08] destroy, [02] restore: breakables; 0 = no breakable of that id (a stub for the caller) */
int ev_obj_destroy(int entity);
int ev_obj_restore(int entity);
/* An ObjAct was used (src/event.c use_action) or its state flag was on at a block load (after_load): its object plays
 * the ObjActParam objAnimId, or holds the anim's end (an opened door after a load) */
void ev_obj_objact(int part, int anim, int after_load);
/* Test commands (event_test_cmd): obj destroy|restore|enable|disable <entity>, obj anim|end <entity> <anim>,
 * expect objbroken <entity> <0|1>, expect objanim <entity> <anim> <playing 0|1>. 1 if cmd was one of them. */
int ev_obj_test(const char *cmd, int *pass, int *fail);

#endif
