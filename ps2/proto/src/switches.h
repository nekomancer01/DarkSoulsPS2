/* switches.h: every compile-time switch the C sources read, in one list. Nothing includes this file; the defaults and
 * the reasons for them stay next to the code that uses them (the "where" column). tools/ci/switches.py (CI job
 * "hygiene") fails when a #if / #ifdef / #ifndef in src/ tests a name that has no row here, or a row names something
 * src/ no longer tests; tools/ci/switches.sh (CI job "switches") compiles and links one ELF per entry of the "CI"
 * column, so a fallback path that stops building fails the PR instead of the next A/B test.
 *
 * Set a switch with make DEFS='-DNAME=value' (or EE_DBGINFOFLAGS=, which also lands in the objects' .cflags stamp).
 * Defaults are the values a plain `make` builds (some come from the Makefile: "make X=" in the what column).
 *
 * Columns: name | default | kind | CI | where | what
 *   kind  fallback  the other value builds the old path (project rule: new paths are on by default, the old one stays
 *                   reachable through a switch). CI builds it.
 *         debug     test / measurement / capture builds (scripted runs, logs, overlays). CI builds it.
 *         tune      a number (size, distance, time). CI builds only the values that name an old path.
 *         make      set by the Makefile from a make variable or the area being built.
 *         host      host test harnesses only (not an EE build); CI skips it.
 *   CI    the builds tools/ci/switches.sh makes, separated by ';', each one the extra compiler arguments; - = none.
 *
 * name                | default  | kind     | CI                                            | where         | what
 * ------------------- | -------- | -------- | --------------------------------------------- | ------------- | ----
 * AREA                | "m10_02_00_00" | make | -                                          | main.c        | the area pack (make MAP=)
 * CHR_DIR             | ""       | make     | -                                             | pack.h        | the area's enemy / common pack folder (make MAP=)
 * MDL_MAX_BONES       | 255      | make     | -DMDL_MAX_BONES=64                            | model.h       | largest skeleton (make CHR_BONES=61: one VU1 palette window, the old path)
 * PROFILE             | off      | make     | -DPROFILE                                     | prof.h        | per-pass EE cycles and cache misses (make PROFILE=1)
 * GPROF               | off      | make     | -DPROFILE -DGPROF                             | prof.c        | gprof sampling build (make GPROF=1, tools/ps2-gprof.sh)
 * PROF_LOG_FRAMES     | 300      | tune     | -                                             | prof.c        | PROFILE: frames between log lines
 * PROF_GPROF_START    | 120      | tune     | -                                             | prof.c        | GPROF: frames before sampling starts
 * PROF_GPROF_FRAMES   | 1800     | tune     | -                                             | prof.c        | GPROF: frames sampled
 * NO_PVS              | off      | fallback | -DNO_PVS                                      | main.c map.c  | no occlusion (make PVS=0)
 * MEDIA_CD            | off      | fallback | -DMEDIA_CD                                    | mapstream.c   | CD streaming budgets on any disc (make MEDIA=cd)
 * BUMP_BAKED          | 1        | fallback | -DBUMP_BAKED=0                                | bump.c        | relief baked at export; 0 = the runtime CLUT relief pass (make BUMP_BAKE=0)
 * COL_TILES           | 1        | fallback | -DCOL_TILES=0                                 | coltile.h     | collision index and 16 m tiles streamed; 0 = collision.bin whole (make COL_TILES=0)
 * WORLD_LITE_DIET     | 1        | fallback | -DWORLD_LITE_DIET=0                           | world.c       | kept-only blocks hold collision and events only (make WORLD_LITE_DIET=0)
 *
 * map streaming (mapstream.c, mapstream.h, world.c, world.h)
 * MS_STEADY           | 1        | fallback | -DMS_STEADY=0                                 | mapstream.c   | class hysteresis and held textures; 0 = the old plan
 * MS_NEAR_FIRST       | 1        | fallback | -DMS_NEAR_FIRST=0                             | mapstream.c   | near parts planned before coarse ones
 * MS_NEAR_OVER        | 1        | fallback | -DMS_NEAR_OVER=0                              | mapstream.c   | near parts may take coarse room in view
 * MS_CANCEL           | 1        | fallback | -DMS_CANCEL=0                                 | mapstream.c   | stale reads cancelled after a warp or settle
 * MS_SETTLE_NEAR      | 1        | fallback | -DMS_SETTLE_NEAR=0                            | mapstream.c   | a settle waits for the near parts at the feet
 * MS_SETTLE_HZN       | 1        | fallback | -DMS_SETTLE_HZN=0                             | mapstream.c   | a settle skips coarse cells the horizon covers
 * MS_STABLE           | 1        | fallback | -DMS_STABLE=0                                 | mapstream.c   | distance-only ranks with a bonus for what is in
 * MS_WHOLE_CELLS      | 1        | fallback | -DMS_WHOLE_CELLS=0                            | mapstream.c   | a cell's near parts planned together
 * MS_POOLS            | 1        | fallback | -DMS_POOLS=0                                  | mapstream.c   | structural and item pools
 * MS_LEND             | 1        | fallback | -DMS_LEND=0                                   | mapstream.c   | idle structural pages lent to items
 * MS_LEND_ALL         | 0        | fallback | -DMS_LEND_ALL=1                               | mapstream.c   | 1 = lend to every class (the earlier setting)
 * MS_LEND_NEAR        | 1        | fallback | -DMS_LEND_NEAR=0                              | mapstream.c   | near parts may use lent pages
 * MS_LEND_NEAR_KB     | 900      | tune     | -                                             | mapstream.c   | lent room near parts may hold
 * MS_PREP_NOLEND      | 0        | fallback | -DMS_PREP_NOLEND=60                           | mapstream.c   | frames a pool prepare keeps new items out (60 = the old hold)
 * MS_ITEMS_TOP        | 1        | fallback | -DMS_ITEMS_TOP=0                              | mapstream.c   | item pages taken from the top of the arena
 * MS_BORROW           | 1        | fallback | -DMS_BORROW=0                                 | mapstream.c   | free heap pages borrowed for map data; 0 = the fixed arena
 * MS_BORROW_FLOOR     | 2816 KB  | tune     | -                                             | mapstream.c   | heap kept free when borrowing
 * MS_DEFRAG           | 1        | fallback | -DMS_DEFRAG=0                                 | mapstream.c   | pages moved to close holes
 * MS_ONE_LOD          | 1        | fallback | -DMS_ONE_LOD=0                                | mapstream.c   | a cell's coarse item dropped once its near level is in
 * MS_MIP1_CLS         | C_PREF   | fallback | -DMS_MIP1_CLS=C_VIEW2                         | mapstream.c   | class of a streamed mip 1 (C_VIEW2 = the old one)
 * MS_POOL             | 8 MB     | tune     | -                                             | mapstream.c   | arena size
 * MS_NEAR_CUBE        | 1        | fallback | -DMS_NEAR_CUBE=0                              | mapstream.h   | near parts cut on the cube grid
 * MS_DG               | 1        | fallback | -DMS_DG=0                                     | map.c         | draw groups count for unseen items; 0 = pvs only
 * MS_BEHIND_KEEP      | 1        | fallback | -DMS_BEHIND_KEEP=0                            | mapstream.c   | horizon-covered coarse items out of view past MS_BEHIND_R kept, never read
 * MS_BEHIND_R         | 32 m     | tune     | -                                             | mapstream.c   | MS_BEHIND_KEEP distance
 * MS_R_HZN            | 40 m     | make     | -DMS_R_HZN=48.0f                              | mapstream.c   | coarse cells with a horizon wanted within (make BURG_HZN_NEAR=1: 40 with map.c HZN_R 32; 0 = residency.h RES_HZN_R 48)
 * MS_SCAN             | 1        | fallback | -DMS_SCAN=0                                   | mapstream.c   | read-ahead issued in disc order (circular scan); 0 = class and distance order
 * MS_RA_INFLIGHT      | 2        | fallback | -DMS_RA_INFLIGHT=8                            | mapstream.c   | read-ahead reads in flight at most (8 = MS_INFLIGHT, the old cap)
 * WORLD_BOOT_RELIEF   | 1        | fallback | -DWORLD_BOOT_RELIEF=0                         | world.h       | the boot block's relief streams; 0 = resident from boot
 * WORLD_RELIEF_SOFT   | 1        | fallback | -DWORLD_RELIEF_SOFT=0                         | world.c       | relief never drops wanted cells
 * WORLD_STRUCT_WHOLE  | 2        | fallback | -DWORLD_STRUCT_WHOLE=3                        | world.c       | wanted blocks counted whole per link (3 = the earlier worst case)
 * WORLD_KEPT_HZN      | 1        | fallback | -DWORLD_KEPT_HZN=0                            | world.c       | kept-only blocks keep their horizon
 * WORLD_TAB_SPLIT     | 1        | fallback | -DWORLD_TAB_SPLIT=0                           | world.c       | the map table read in pieces
 * WORLD_EV_SHARED     | 1        | fallback | -DWORLD_EV_SHARED=0                           | world.c       | a map's shared event data read once
 * OBJ_STREAM          | 1        | fallback | -DOBJ_STREAM=0                                | objects.c     | object types streamed by distance; 0 = all at load
 * PACK_JOB_ABANDON    | 1        | fallback | -DPACK_JOB_ABANDON=0                          | pack.c        | a dropped load cancels instead of waiting
 * PACK_CDROM          | probe    | debug    | -DPACK_CDROM=1                                | pack.c        | force disc (1) or host (0) paths
 *
 * map drawing (map.c, render.c, drawgrp.c, bump.c, main.c)
 * MAP_TILES           | 1        | fallback | -DMAP_TILES=0                                 | map.c         | cells tested by tile first
 * MAP_NEAR_SKIP       | 1        | fallback | -DMAP_NEAR_SKIP=0                             | map.c         | near-pass skips cells with no near level
 * MAP_PASS_MS         | 1        | fallback | -DMAP_PASS_MS=0                               | map.c         | "map: pass ms" timing log
 * SKY_ROWS            | 1        | fallback | -DSKY_ROWS=0                                  | map.c         | sky chunks under their own fog row
 * ZSPLIT              | 50 m     | fallback | -DZSPLIT=0                                    | map.c         | two Z ranges; 0 = one range
 * HZN_R               | 32 m     | make     | -DHZN_R=40.0f                                 | map.c         | horizon radius (make BURG_HZN_NEAR=1; 0 = 40 m, residency.h RES_PIN_R)
 * HZN_FADE            | 8 m      | fallback | -DHZN_FADE=0                                  | map.c         | horizon fade-in; 0 = hard switch
 * HZN_ZBIAS           | 0.03     | fallback | -DHZN_ZBIAS=0                                 | map.c         | fade depth bias; 0 = plain depth test
 * HZN_FADE_DEBUG      | 0        | debug    | -DHZN_FADE_DEBUG=1                            | map.c         | fade pass drawn solid magenta
 * HZN_OFF             | off      | fallback | -DHZN_OFF                                     | map.c         | horizon section ignored at runtime
 * NEAR_FADE           | 10       | fallback | -DNEAR_FADE=0                                 | map.c         | near detail dissolves in; 0 = hard swap
 * NEAR_DRAW_R         | 18 m     | tune     | -                                             | map.c         | near level draw radius
 * FAR                 | 250 m    | tune     | -                                             | map.c         | edge fog distance
 * CULL_FAR            | FAR      | tune     | -DCULL_FAR=1000.0f                            | map.c         | opaque cull distance (A/B 1000 m)
 * SMALL_Z             | 40 m     | tune     | -                                             | map.c         | small-box cull depth
 * SMALL_PX            | 3 px     | tune     | -                                             | map.c         | small-box cull radius
 * TEX_POOL_KB         | 896      | tune     | -                                             | map.c         | VRAM map texture pool
 * LPOOL_N             | 12288    | tune     | -                                             | map.c         | draw list pool entries
 * MAP_DBG             | off      | debug    | -DMAP_DBG=60; -DMAP_DBG=60 -DMAP_DBG_ALL -DMAP_DBG_FLIP | map.c | chunks / tris dropped per step every N frames
 * MAP_DBG_ALL         | off      | debug    | (with MAP_DBG)                                | map.c         | MAP_DBG: no frustum skip
 * MAP_DBG_FLIP        | off      | debug    | (with MAP_DBG)                                | map.c         | MAP_DBG: chunks drawn last frame, not now
 * MAP_TWO_PASS_CHECK  | off      | debug    | -DMAP_TWO_PASS_CHECK                          | map.c         | two-pass lists compared with one pass
 * MAP_PROF            | off      | debug    | -DMAP_PROF                                    | map.c         | visibility / EE clip time on the HUD
 * MAP_STATS           | off      | debug    | -DMAP_STATS                                   | map.c         | batches, pool traffic, streamed cells every 60 frames
 * MAP_PANO_R          | off      | debug    | -DMAP_PANO_R=60.0f -DPANO_ZOOM=2.0f           | main.c map.c  | panorama capture build (tools/pano.py)
 * PANO_ZOOM           | 1.0      | debug    | (with MAP_PANO_R)                             | render.c      | panorama capture focal length
 * DG_SHARED           | 1        | fallback | -DDG_SHARED=0                                 | drawgrp.c     | draw groups shared across blocks
 * BUMP_OFF            | off      | fallback | -DBUMP_OFF -DBUMP_BAKED=0                     | bump.c        | runtime relief: no relief pass (A/B)
 * BUMP_COVER          | off      | debug    | -DBUMP_COVER -DBUMP_BAKED=0                   | bump.c        | runtime relief: covered pixels black
 * BUMP_CAP            | 1536     | tune     | -                                             | bump.c        | relief batches per frame
 * BUMP_POOL_KB        | 384      | tune     | -                                             | bump.c        | normal map VRAM pool
 * DOF_BANDS           | 3        | tune     | -                                             | main.c        | depth of field bands
 * FPS_CAP_FIELDS      | 2        | fallback | -DFPS_CAP_FIELDS=1                            | main.c        | 30 fps cap; 1 = uncapped
 * V1B_FIELD           | off      | debug    | -DV1B_FIELD                                   | main.c        | point-sampled field output
 * R2_OFF              | off      | fallback | -DR2_OFF                                      | main.c        | no VRAM layout pass after the model upload (A/B)
 * V6_OFF              | off      | fallback | -DV6_OFF                                      | render.c      | gsKit's VRAM allocator
 *
 * textures and VU1 (texcache.c, vu1.c, vu1.h, model.c)
 * TC_RLIST            | 1        | fallback | -DTC_RLIST=0                                  | texcache.c    | resident-level list; 0 = walk every level
 * TC_PRESS            | 1        | fallback | -DTC_PRESS=0                                  | texcache.c    | mip 1 under pool pressure
 * TC_UP_OLD           | off      | fallback | -DTC_UP_OLD                                   | texcache.c    | the old 96 + 16 KB upload caps
 * TC_NO_NEAR          | off      | fallback | -DTC_NO_NEAR                                  | texcache.c    | near texture levels never drawn (A/B)
 * TC_HUD              | off      | debug    | -DTC_HUD                                      | texcache.c    | upload numbers on screen
 * TC_NEAR_KB          | 256      | tune     | -                                             | texcache.c    | near level pool
 * MIP_BIAS            | 1.0      | tune     | -                                             | texcache.c    | LOD bias in levels
 * MIP0_AHEAD          | 1.3      | fallback | -DMIP0_AHEAD=0                                | texcache.c    | mip 0 asked ahead; 0 = where drawn
 * MIP_TRILINEAR       | 1        | fallback | -DMIP_TRILINEAR=0                             | vu1.c         | trilinear; 0 = LINEAR_MIPMAP_NEAREST
 * VU1_TEX_PATH2       | off      | fallback | -DVU1_TEX_PATH2                               | vu1.h         | map texture uploads on PATH2
 * VU1_NO_IMT          | off      | fallback | -DVU1_NO_IMT                                  | vu1.c         | PATH3 uploads without IMT
 * VU1_NO_PKT          | off      | fallback | -DVU1_NO_PKT                                  | vu1.c         | map batches as stream-by-stream REFs
 * VU1_NO_SKINPKT      | off      | fallback | -DVU1_NO_SKINPKT                              | vu1.c         | skin batches as stream-by-stream REFs
 * VU1_MAP_OLD         | off      | fallback | -DVU1_MAP_OLD                                 | vu1.c         | the old map batch path
 * VU1_IDX_EE_CLIP     | off      | fallback | -DVU1_IDX_EE_CLIP                             | vu1.c         | indexed clip batches split on the EE
 * VU1_IDX_LISTS       | off      | debug    | -DVU1_IDX_LISTS                               | vu1.c         | indexed batches expanded to lists
 * VU1_IDX_NO_CLIP     | off      | debug    | -DVU1_IDX_NO_CLIP                             | vu1.c         | indexed clip batches not drawn
 * VU1_NO_CLIP         | off      | debug    | -DVU1_NO_CLIP                                 | vu1.c         | clip batches through the plain program
 * VU1_CHECK           | off      | debug    | -DVU1_CHECK                                   | vu1.c model.c | VIF chain checks in the EE log
 * CHAIN_MAP_QW        | 19000    | tune     | -                                             | vu1.c         | map chain buffer
 * CHAIN_CHR_QW        | 19100    | tune     | -                                             | vu1.c         | character chain buffer
 * CHAIN_FX_QW         | 4096     | tune     | -                                             | vu1.c         | particle chain buffer
 * TEXQ_MAP_QW         | 4096     | tune     | -                                             | vu1.c         | map texture queue
 * TEXQ_CHR_QW         | 256      | tune     | -                                             | vu1.c         | character texture queue
 * TEXQ_SLICE          | 512      | tune     | -                                             | vu1.c         | IMAGE qwords per GIF packet
 * SKIN_CLIP_MARGIN    | 1.5 m    | tune     | -                                             | vu1.c         | bone origin to farthest vertex
 * FX_VU1              | 1        | fallback | -DFX_VU1=0                                    | fx.c          | particles on VU1; 0 = EE billboards
 * V5_OFF              | off      | fallback | -DV5_OFF                                      | model.c       | character CLUTs as exported (CT32)
 * MDL_SKIN_PAGE       | 64 KB    | fallback | -DMDL_SKIN_PAGE=0                             | model.c       | skin packets in pages; 0 = one block
 * MDL_ANIM_PAGE       | 64 KB    | fallback | -DMDL_ANIM_PAGE=0                             | model.c       | anim section in pages; 0 = one copy
 * SPEC_TEST           | off      | debug    | -DSPEC_TEST=0.85,0.8                          | model.c       | metal / specular on every vertex
 * NO_VU0              | off      | fallback | -DNO_VU0                                      | vmath.c       | skin matrix product in C
 *
 * characters, logic, collision (enemy.c, ai.c, lua0.c, lpass.h, collision.c, nav.c)
 * LOGIC_SPREAD        | 1        | fallback | -DLOGIC_SPREAD=0                              | lpass.h       | logic work spread across frames
 * LOGIC_PASS_MS       | 1        | fallback | -DLOGIC_PASS_MS=0                             | lpass.h       | logic pass timing log
 * ACTIVE_MAX          | 16       | tune     | -                                             | enemy.c       | characters updated a frame
 * DRAW_MAX            | 16       | tune     | -                                             | enemy.c       | characters drawn a frame
 * ENEMY_LAZY_LOAD     | 1        | fallback | -DENEMY_LAZY_LOAD=0                           | enemy.c       | types loaded when near; 0 = every placement in LOAD_DIST
 * ENEMY_KEEP_DEAD     | 1        | fallback | -DENEMY_KEEP_DEAD=0                           | enemy.c       | killed enemies stay dead until a rest
 * ENEMY_WALLS         | 1        | fallback | -DENEMY_WALLS=0                               | enemy.c       | moves against walls and ledges; 0 = the old move
 * ENEMY_WALL_CHECK    | 1        | fallback | -DENEMY_WALL_CHECK=0                          | enemy.c       | wall-hit log by move kind
 * SEP_CARRY           | 1        | fallback | -DSEP_CARRY=0                                 | enemy.c       | separation rides the next move
 * MOVE_PROF           | 1        | fallback | -DMOVE_PROF=0                                 | enemy.c       | "enemy: moves" timing log
 * NO_AI_SCRIPTS       | off      | debug    | -DNO_AI_SCRIPTS                               | enemy.c       | no ESD / Lua
 * ENEMY_ONLY          | off      | debug    | -DENEMY_ONLY=c2900_c2540                      | enemy.c       | only these types
 * ENEMY_HIT_TEST      | off      | debug    | -DENEMY_HIT_TEST                              | enemy.c       | a hit on the nearest enemy every 2 s
 * LUA_GC_STEP_US      | 1000     | fallback | -DLUA_GC_STEP_US=0                            | ai.c          | incremental Lua GC; 0 = stop the world
 * LUA_POOL_GROW       | 1        | fallback | -DLUA_POOL_GROW=0                             | lua0.c        | Lua small objects in their own chunks
 * COL_SCAN            | 1        | fallback | -DCOL_SCAN=0                                  | collision.c   | box test before the lookups
 * PUSH2_CELLS         | 1        | fallback | -DPUSH2_CELLS=0                               | collision.c   | push-out v2 by cells
 * NAV_QREJ            | 1        | fallback | -DNAV_QREJ=0                                  | nav.c         | integer box reject before barycentrics
 * ESD_TRACE           | off      | host     | -                                             | esd.c         | ESD trace hook (native test harness)
 * OBJ_HOST            | off      | host     | -                                             | objects.c     | piece bodies for the host test harness
 *
 * scripted test builds (testscript.c, testscript.h, debug_hud.c, hud.c, loading.c, main.c)
 * TEST_SPAWN          | off      | debug    | -DTEST_SPAWN=0,0,0,0; -DTEST_SPAWN=0,0,0,0 -DCOMBAT_DUMMY; -DTEST_SPAWN=0,0,0,0 -DENEMY_VIEW=0,0,3 | testscript.c | start position x,y,z,angle
 * COMBAT_DUMMY        | off      | debug    | (with TEST_SPAWN)                             | testscript.c  | dummy target in front of the spawn
 * ENEMY_VIEW          | off      | debug    | (with TEST_SPAWN)                             | testscript.c  | camera orbits an MSB placement
 * COL_TEST            | off      | debug    | -DCOL_TEST=1; -DCOL_TEST=7                    | testscript.c  | collision / movement runs from the spawn
 * ESD_TEST            | off      | debug    | -DESD_TEST                                    | testscript.c  | backstep, dash attacks, running jump
 * CHAR_CLOSEUP        | off      | debug    | -DCHAR_CLOSEUP=0.6,1.6,0.1                    | testscript.c  | camera close to the player
 * HUD_DEBUG           | off      | debug    | -DHUD_DEBUG                                   | debug_hud.c   | debug overlay on from boot
 * HELM_OFF            | off      | debug    | -DHELM_OFF                                    | main.c        | start with the helm off
 * SLOW_FRAME_LOG      | off      | debug    | -DSLOW_FRAME_LOG                              | main.c pack.c | frames over 100 ms in the EE log
 * NO_HUD              | off      | debug    | -DNO_HUD                                      | hud.c         | HUD loaded, not drawn
 * DIED_TEST           | off      | debug    | -DDIED_TEST                                   | hud.c         | YOU DIED banner after 3 s
 * LOADING_OFF         | off      | fallback | -DLOADING_OFF                                 | loading.c     | no loading screen
 */
#ifndef SWITCHES_H
#define SWITCHES_H
#endif
