/* World streaming (TECH_DEBT build-5): blocks of the game world load and unload in the background by player position,
 * so walking from one area into the next needs no reboot or loading screen. A block is one exported part of an MSB
 * map (tools/area.py BLOCKS, its own pack data/<block>.pak); the maps of a world group share one frame. The block
 * table comes from data/world.pak (tools/export_world.py). Block i = collision block i = map instance i; block 0 is
 * the boot area. The block under the player is "full" (its map's resident level in the one full pool); the other
 * loaded blocks are "lite" (collision + map tables, their near cells streaming as usual). */
#ifndef WORLD_H
#define WORLD_H

#include <gsKit.h>

#define WORLD_MAX 8  /* blocks in world.bin */

/* Before main.c loads the boot area: reads world.pak, opens every block's pack and allocates the fixed pools.
 * 0 = no world table, or one whose block 0 is not boot_area (single-area builds): main.c loads map.bin as before and
 * world_update does nothing. */
int world_init(const char *boot_area);
/* World builds, instead of map_init: the boot block's split map (maptab.bin + mapres.bin) as map instance 0, full;
 * map_upload follows as usual. Then world_stream_open instead of map_stream_open. */
int world_boot_map(void);
void world_stream_open(void);
/* main.c after enemy_init / objects_init / fx_init: other blocks' enemy, breakable and effect types (boot time,
 * before the near-cell arena) */
void world_systems(GSGLOBAL *gs);
/* Once a frame with the player's feet (x, y, z) and the frame time: picks the block under the player, continues one
 * background read (a wanted block's lite set first, then the full pool's swap), turns a fully read block on, unloads
 * the blocks the triangle under the player no longer keeps. Never waits for the IOP, except on a warp (> WORLD_WARP m in one frame: test spawns,
 * the first frame), which loads and swaps synchronously as DSR's loading screen does. */
void world_update(const float *feet, float dt);
/* player.c, when the feet at (x, y, z) have no floor: 1 if they lie in a block whose collision is still loading (and
 * no loaded floor is a short drop below), so the player stays where it is instead of stepping or falling in. The
 * block is then wanted; after WORLD_HOLD_S of holding the frame waits for it (loading screen). 0 without a world. */
int world_hold(const float *feet);
/* Block whose collision the player stood on last (0 = boot block), -1 without a world */
int world_current(void);
/* MSB map id (mAA_BB_CC_DD) of a block, NULL if none (sound.c: area banks and ambience per map) */
const char *world_map(int block);

typedef struct {
    int blocks, loaded, reading;     /* table size, blocks on (incl. boot), block being read (-1 none) */
    int current, full;               /* block under the player, block whose streams are in the full pool (-1 none) */
    unsigned want;                   /* blocks the collision triangle under the player loads (world.bin links) */
    unsigned pool_kb, pool_used_kb;  /* fixed pools (full + slots), slots in use */
    unsigned load_ms, read_kb;       /* last lite load: wall time from first read to on, bytes */
    unsigned full_ms;                /* last full swap: time from going lite to drawing the new streams */
    unsigned on_us_max;              /* longest single-frame step (turning a block on / off / full), microseconds */
    int loads, unloads, fails;
    unsigned hold_ms;                /* time the player was held at a block still loading (world_hold) */
    int hold_settles;                /* holds that ran out (WORLD_HOLD_S) and waited behind the loading screen */
} WorldStats;
extern WorldStats world_stats;

#endif
