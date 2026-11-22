/* World streaming (TECH_DEBT build-5): blocks of the game world load and unload in the background by player position,
 * so walking from one area into the next needs no reboot or loading screen. A block is one exported part of an MSB
 * map (tools/area.py BLOCKS, its own pack data/<block>.pak); the maps of a world group share one frame. The block
 * table comes from data/world.pak (tools/export_world.py). Block i = collision block i = map instance i; block 0 is
 * the boot area. A loaded block holds its "lite" set (collision + map tables, events, breakable / effect textures until
 * they upload); its map's resident level and near cells stream per cell around the player (src/mapstream.c), the
 * same for every block, so nothing swaps when the player crosses into another block (TECH_DEBT world-2 / world-5). */
#ifndef WORLD_H
#define WORLD_H

/* 1 (default): the boot block's relief streams like every block's (world.c), 0: resident from boot (main.c) */
#ifndef WORLD_BOOT_LEAN
#define WORLD_BOOT_LEAN 1  /* the boot block's map tables are let go away from it (world.c); 0: resident for good */
#endif
#ifndef WORLD_BOOT_RELIEF
#define WORLD_BOOT_RELIEF 1
#endif

#include <gsKit.h>

#define WORLD_MAX 32  /* blocks in world.bin (u32 load / keep masks, world.bin LINK / REAC v2) */

/* Before main.c loads the boot area: reads world.pak and opens every block's pack.
 * 0 = no world table, or one whose block 0 is not boot_area (single-area builds): main.c loads map.bin as before and
 * world_update does nothing. */
int world_init(const char *boot_area);
/* World builds, instead of map_init: the boot block's split map (maptab.bin; mapres.bin streams per cell) as map
 * instance 0; map_upload follows as usual. Then world_stream_open instead of map_stream_open. */
int world_boot_map(void);
void world_stream_open(void);
/* MS_POOLS: the structural pool's size from the world table (world_init computes it; tools/world_budget.py the same) */
unsigned world_struct_bytes(void);
/* main.c after enemy_init / objects_init / fx_init: other blocks' enemy, breakable and effect types (boot time,
 * before the near-cell arena) */
void world_systems(GSGLOBAL *gs);
/* Background read of bytes at rel in block's objects.bin into dst (64-byte aligned): a pack_req_read ticket (pack.h),
 * 0 when the block has no objects.bin or every request slot is taken (objects.c OBJ_STREAM) */
int world_obj_read(int block, unsigned rel, void *dst, unsigned bytes);
/* Once a frame with the player's feet (x, y, z) and the frame time: picks the block under the player, continues one
 * background read (a wanted block's lite set first, then the relief of the block underfoot), turns a fully read block on, unloads
 * the blocks the triangle under the player no longer keeps. Never waits for the IOP, except on a warp (> WORLD_WARP m in one frame: test spawns,
 * the first frame), which loads and swaps synchronously as DSR's loading screen does. */
void world_update(const float *feet, float dt);
/* player.c, when the feet at (x, y, z) have no floor: 1 if they lie in a block whose collision is still loading (and
 * no loaded floor is a short drop below), so the player stays where it is instead of stepping or falling in. The
 * block is then wanted; after WORLD_HOLD_S of holding the frame waits for it (loading screen). 0 without a world. */
int world_hold(const float *feet);
/* 1 when nothing the feet want is still loading (no lite set reading, every wanted block on): what a warp's settle
 * waits for. Read only; the Lab (lab/lab_main.c) steps world_update until it holds, so its reads never lag. */
int world_settled(const float *feet);
/* Block whose collision the player stood on last (0 = boot block), -1 without a world */
int world_current(void);
/* Streaming sources (Polaris 4.3): every place the world streams for, each with a position and a velocity (from
 * its last positions), all through one planner (wplan.c for the blocks, msplan.c for the cells). The player is one
 * (world_update's feet), planned by the walking rule; the others are targets, planned by the rule a warp's spot has
 * behind the loading screen (the hard tier it waits for, no read-ahead): a death puts one on the respawn point while
 * the death plays, so its blocks and cells are in or on their way when the respawn's loading screen opens
 * (player.c); a warp target and a cutscene camera are kinds for the code that knows them ahead. A warp (the player
 * moved > WORLD_WARP m in a frame) is the player's source turned target until world_settled. WORLD_SOURCES 0
 * (runtime switch): no source but the player. */
enum { WSRC_PLAYER, WSRC_WARP, WSRC_RESPAWN, WSRC_CAMERA, WSRC_N };
void world_source(int kind, const float pos[3]);
void world_source_off(int kind);
/* What of block k is resident now (Polaris 4.1, for the level chain in map.c): WB_ON its lite set is on (collision,
 * events, objects); WB_TABLES its map tables are in (chunk table, textures, cell table), so its levels may stream and
 * draw; WB_HORIZON its cell table and horizon are in (also while it is only kept and has let go of the rest:
 * WORLD_LITE_DIET); WB_FAR the far panorama of the block underfoot is drawn, which stands in for the blocks of its area
 * that are off. 0 without a world. */
enum { WB_ON = 1, WB_TABLES = 2, WB_HORIZON = 4, WB_FAR = 8 };
unsigned world_block_res(int k);
/* MSB map id (mAA_BB_CC_DD) of a block, NULL if none (sound.c: area banks and ambience per map) */
const char *world_map(int block);

typedef struct {
    int blocks, loaded, reading;     /* table size, blocks on (incl. boot), block being read (-1 none) */
    int current, full;               /* block under the player, block whose relief (bump.bin) is loaded (-1 none) */
    unsigned want;                   /* blocks the collision triangle under the player loads (world.bin links) */
    unsigned pool_kb, pool_used_kb;  /* 0 (no fixed pool since per-cell resident levels), lite sets + relief in use */
    unsigned load_ms, read_kb;       /* last lite load: wall time from first read to on, bytes */
    unsigned full_ms;                /* last relief load: ms from its first read to drawing it */
    unsigned on_us_max;              /* longest single-frame step (turning a block on / off), microseconds */
    unsigned leans, relites;         /* WORLD_LITE_DIET: kept-only blocks that let go of their map tables / read them back */
    unsigned pool_spills;            /* MS_POOLS: lite-set entries the structural pool had no room for (item pages) */
    int loads, unloads, fails;
    unsigned hold_ms;                /* time the player was held at a block still loading (world_hold) */
    int hold_settles;                /* holds that ran out (WORLD_HOLD_S) and waited behind the loading screen */
    unsigned holds;                  /* world_hold calls that held the player (frame.c's trace: the frame's held flag) */
} WorldStats;
extern WorldStats world_stats;

#endif
