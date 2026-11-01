/* DSR event scripts (EMEVD) at runtime: the bytecode of tools/export_emevd.py (events.bin per map / block, evcommon.bin
 * once) run as DSR runs them: event instances started by InitializeEvent with their own args (slot + arg bytes,
 * substituted into instructions as they run), condition groups (MAIN = 0, AND 1..7, OR -1..-7) evaluated every frame
 * while MAIN waits, compiled / uncompiled group tests, skip / end / restart, waits, rest behaviour, the event's own
 * flag (event id + slot) set when it ends. Wired to: flags.c (event flags, event values), MSB regions / entities of
 * the map, enemy.c (enable / kill / dead / HP), collision.c (hit parts on / off), object colliders (fog walls,
 * gates: their hkx box while the object is on), ObjActs (chests, levers: action button -> flag), treasures (item
 * lots -> getItemFlagId + inventory), player warps. Everything else is a logged stub (TECH_DEBT event-*). */
#ifndef EVENT_H
#define EVENT_H

#include "player.h"

#define EV_SETS 5       /* set 0 = common.emevd, 1.. = loaded maps (one per world block) */
#define EV_MAX_INST 768 /* event instances alive at once, all sets (common + 2 maps start ~610 at most) */

/* Once at boot, before the first map: common events (evcommon.bin, kept by the caller); starts common event 0 / 50 */
int event_init(const void *common_blob);
/* A map's events.bin (kept by the caller until event_block_off) for world block `block` (0 = the boot area from
 * main.c, others from world.c turn_on; its collision block for "Activate Hit"). Resets the map's x5xxx flags and
 * starts its events 50 and 0. NULL (a pack without events.bin) = no events. event_block_off of a block that was never
 * turned on does nothing. */
void event_block_on(int block, const void *blob);
void event_block_off(int block);
/* Once a frame after the player / enemies / objects moved: action-button uses (ObjAct, treasure), then every running
 * instance until it waits. */
void event_update(Player *pl, float dt);
/* The action button went down this frame (pad Cross / test "ev action") */
void event_action(void);
/* Push a vertical capsule (feet pos, radius r, height h) out of the enabled object colliders; 1 if it touched one */
int event_push_player(float pos[3], float r, float h);
/* Bonfire rest (A4): Restart-on-rest events start over, End-on-rest ones stop */
void event_rest(void);
/* Test scripts ("ev <cmd>", testscript.c): flag ID 0|1, expect flag|obj|hit|treasure ID V, expect x|y|z <|> V,
 * action, warp ENTITY, trace 0|1|2, summary. Prints "TEST ev ..." lines (PASS / FAIL). */
void event_test_cmd(const char *cmd);
/* HUD line: instances running / waiting, conditions fired, last fired event, stubs, action prompt, update time */
void event_status(char *buf, int n);

typedef struct {
    int running, waiting, fired, stubs, unsupported, ended, peak;
    int deferred;  /* frames that hit EV_BUDGET_US and left instances for the next frame */
    unsigned last_id, us, bytes;
    int prompt;  /* help message id of the action in reach (-1 none) */
} EventStats;
extern EventStats event_stats;

#endif
