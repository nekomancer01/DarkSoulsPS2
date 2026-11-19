/* switches.h: every compile-time switch the C sources read, in one list. Nothing includes this file; the defaults and
 * the reasons for them stay next to the code that uses them (the "where" column). tools/ci/switches.py (CI job
 * "hygiene") fails when a #if / #ifdef / #ifndef in src/ tests a name that has no row here, or a row names something
 * src/ no longer tests; tools/ci/switches.sh (CI job "switches") compiles and links one ELF per entry of the "CI"
 * column, so a fallback path that stops building fails the PR instead of the next A/B test.
 *
 * Set a switch with make DEFS='-DNAME=value' (or EE_DBGINFOFLAGS=, which also lands in the objects' .cflags stamp).
 * Defaults are the values a plain `make` builds (some come from the Makefile: "make X=" in the what column).
 * Rows marked "runtime" are also runtime switches (src/cvar.h): a test script's "option set NAME VALUE" (or
 * tools/test-run.py --set NAME=VALUE) flips them at boot in the same ELF / ISO, for one-build A/B runs.
 *
 * Columns: name | default | kind | CI | owner | pr | where | what
 *   kind  fallback  the other value builds the old path (project rule: new paths are on by default, the old one stays
 *                   reachable through a switch). CI builds it.
 *         debug     test / measurement / capture builds (scripted runs, logs, overlays). CI builds it.
 *         tune      a number (size, distance, time). CI builds only the values that name an old path.
 *         make      set by the Makefile from a make variable or the area being built.
 *         host      host test harnesses only (not an EE build); CI skips it.
 *   CI    the builds tools/ci/switches.sh makes, separated by ';', each one the extra compiler arguments; - = none.
 *         For a fallback this is the off path: the build that selects the old code.
 *   owner the Polaris module the switch belongs to (hal core io gfx world scene game ui snd fx cine main tools; see
 *         tools/ci/switches.py OWNERS), from the file in the where column.
 *   pr    the PR that added it: #N, init (the first public release) or c:<commit> (a direct commit to main);
 *         "new" in the PR that adds the switch, until its number is known.
 * Build-time switches (Makefile variables, the exporters' environment) are registered in tools/switches.tsv;
 * python3 -I tools/ci/switches.py --tsv prints both tables as one.
 *
 * name                | default  | kind     | CI                                            | owner  | pr    | where         | what
 * ------------------- | -------- | -------- | --------------------------------------------- | ------ | ----- | ------------- | ----
 * AI_PERCEPT          | 1        | fallback | -DAI_PERCEPT=0                                | game   | #234  | ai.c          | DSR sight (FUN_140618680: eye cones as half-angles, sight ray every 0.25 s) and hearing (FUN_140623d10); 0 = flat eye_angX cone, no sight line, hearing capped at ear_dist
 * AREA                | "m10_02_00_00" | make | -                                          | main   | init  | main.c        | the area pack (make MAP=)
 * CHR_DIR             | ""       | make     | -                                             | io     | init  | pack.h        | the area's enemy / common pack folder (make MAP=)
 * MDL_MAX_BONES       | 255      | make     | -DMDL_MAX_BONES=64                            | scene  | #107  | model.h       | largest skeleton (make CHR_BONES=61: one VU1 palette window, the old path)
 * PROFILE             | off      | make     | -DPROFILE                                     | core   | init  | prof.h        | per-pass EE cycles and cache misses (make PROFILE=1)
 * GPROF               | off      | make     | -DPROFILE -DGPROF                             | core   | init  | prof.c        | gprof sampling build (make GPROF=1, tools/ps2-gprof.sh)
 * HEAPMAP             | off      | make     | -DHEAPMAP                                     | core   | #203  | hitch.c       | heap census per caller in the EE log (make HEAPMAP=1, tools/heapmap.py)
 * MEM_STRICT          | off      | debug    | -DMEM_STRICT                                  | core   | new   | mem.c         | TEST error on a heap allocation in play outside a legacy tag (mem.h MEM_TAGS) or off the main thread
 * FRAME_STRICT        | off      | debug    | -DFRAME_STRICT                                | main   | new   | frame.c vu1.c | TEST error on a draw call in a simulate phase or a simulate call in an emit phase (frame.h), every vu1_add* checked
 * FRAME_SKIP_MAP      | 0        | debug    | -DFRAME_SKIP_MAP=1                            | main   | new   | frame.c       | runtime: the map pass writes no packets (the check that SIM_B reads nothing EMIT_MAP produced, frame.h)
 * OT_HASH             | off      | debug    | -DOT_HASH                                     | gfx    | new   | vu1.c render.c frame.c | "othash:" lines: the VIF1 stream per kick and gsKit's queue hashed on frozen test frames (the order table's byte check, tests/ot_check.txt)
 * PROF_LOG_FRAMES     | 300      | tune     | -                                             | core   | init  | prof.c        | PROFILE: frames between log lines
 * PROF_GPROF_START    | 120      | tune     | -                                             | core   | init  | prof.c        | GPROF: frames before sampling starts
 * PROF_GPROF_FRAMES   | 1800     | tune     | -                                             | core   | init  | prof.c        | GPROF: frames sampled
 * MAP_PANO_NEAR       | 1        | fallback | -DMAP_PANO_NEAR=0                             | world  | #249  | map.c         | the boot map's picture layers only while the player is in the boot block (0: from its whole draw-group area)
 * MAP_PLIGHT          | 1        | fallback | -DMAP_PLIGHT=0                                | world  | #243  | map.c         | the player's point light (PointLightBank) on the map around them (0: characters only, the old path)
 * MAP_PEER_ZBIAS      | 0.02     | tune     | -DMAP_PEER_ZBIAS=0                            | world  | #257  | map.c         | other maps' blocks drawn this share further away (their copies of shared pieces lose to the player's map); 0 = no bias
 * PLIGHT_BLEND        | 0.5f     | tune     | -DPLIGHT_BLEND=0.0f                           | world  | #249  | map.c         | seconds a new player-light row blends in (DSR 0.5 s, FUN_1402d3970; 0: at once, the old path)
 * PLIGHT_RATIO        | 60       | tune     | -                                             | world  | #243  | map.c         | MAP_PLIGHT: % of the area's ambient the player's light must reach for the pass to run
 * NO_PVS              | off      | fallback | -DNO_PVS                                      | main   | #116  | main.c mapblock.c mapstream.c  | no occlusion (make PVS=0)
 * MEDIA_CD            | off      | fallback | -DMEDIA_CD                                    | world  | #144  | mapstream.c mapstream_i.h   | CD streaming budgets on any disc (make MEDIA=cd)
 * BUMP_BAKED          | 1        | fallback | -DBUMP_BAKED=0                                | gfx    | #165  | bump.c        | relief baked at export; 0 = the runtime CLUT relief pass (make BUMP_BAKE=0)
 * COL_TILES           | 1        | fallback | -DCOL_TILES=0                                 | world  | #157  | coltile.h     | collision index and 16 m tiles streamed; 0 = collision.bin whole (make COL_TILES=0)
 * WORLD_LITE_DIET     | 1        | fallback | -DWORLD_LITE_DIET=0                           | world  | #147  | world.c       | kept-only blocks hold collision and events only (make WORLD_LITE_DIET=0)
 * WORLD_DG_LITE       | 1        | fallback | -DWORLD_DG_LITE=0                             | world  | #209  | world.c       | a block's dg.bin / pvs.bin come with its lite set (pvs.bin let go while lean); 0 = read for every block at boot
 * RES_BLOCK           | undefined | make    | -                                             | world  | #198  | residency.h   | X-macro the includer defines to expand the per-block budget table (not a build switch)
 * PICKUP              | 1        | fallback | -DPICKUP=0                                    | game   | #229  | pickup.h event.c | treasure shine, pickup anim, item get dialog, enemy drops; 0 = treasures picked up with none of them, no drops
 *
 * map streaming (mapstream.c, mapstream.h, world.c, world.h)
 * MS_LEND_NEAR_KB     | 900      | tune     | -                                             | world  | #147  | mapstream_i.h   | lent room near parts may hold
 * MS_LEND_NEAR_ALL    | 1        | fallback | -DMS_LEND_NEAR_ALL=0                          | world  | #245  | mapstream_i.h   | near parts may take all the pools' idle room; 0 = at most MS_LEND_NEAR_KB
 * MS_DEFRAG_LEND      | 1        | fallback | -DMS_DEFRAG_LEND=0                            | world  | #245  | mapstream_i.h   | MS_DEFRAG moves groups into the pools' idle room too; 0 = item pages only
 * MS_LEND_REHOME      | 1        | fallback | -DMS_LEND_REHOME=0                            | world  | #257  | msarena.c       | a lent near group a lite set needs moves into the item pages instead of being dropped; 0 = dropped
 * MS_BORROW_FLOOR     | 2816 KB  | tune     | -                                             | world  | #138  | mapstream_i.h   | heap kept free when borrowing
 * MS_HOLD_INFLIGHT    | 0        | tune     | -DMS_HOLD_INFLIGHT=3                          | world  | #201  | mapstream.c     | map reads in flight while a block's lite set loads; 0 with WORLD_SEAM_BUNDLE (its lite set is one disc run), 3 without
 * MS_MIP0             | 0        | fallback | -DMS_MIP0=1                                   | world  | #201  | mapstream.c     | 1 = full-size (mip 0) map textures streamed within 5-10 m (more reads, less room for geometry)
 * MS_READ_BY_CLASS    | 0        | fallback | -DMS_READ_BY_CLASS=1                          | world  | #201  | mapstream.c     | 1 = hard-tier reads go out by class, not nearest first
 * MS_ORPHAN           | 1        | fallback | -DMS_ORPHAN=0                                 | world  | #210  | mapstream.c   | a closing block's reads are cancelled without waiting; 0 = mapstream_close waits for them
 * MS_LEND_ALL_DROP    | 0        | fallback | -DMS_LEND_ALL_DROP=1                          | world  | #201  | msarena.c       | 1 = a pool allocation drops every lent item, not only those in its way
 * MS_MIP1_CLS         | C_PREF   | fallback | -DMS_MIP1_CLS=C_VIEW2                         | world  | #147  | mapstream_i.h   | class of a streamed mip 1 (C_VIEW2 = the old one)
 * MS_POOL             | 8 MB     | tune     | -                                             | world  | init  | mapstream_i.h   | arena size
 * MS_DG               | 1        | fallback | -DMS_DG=0                                     | world  | #131  | map.c         | draw groups count for unseen items; 0 = pvs only
 * MS_SCAN             | 1        | fallback | -DMS_SCAN=0                                   | world  | #79   | mapstream.c   | read-ahead issued in disc order (circular scan); 0 = class and distance order
 * MS_RA_INFLIGHT      | 2        | fallback | -DMS_RA_INFLIGHT=8                            | world  | #166  | mapstream.c   | read-ahead reads in flight at most (8 = MS_INFLIGHT, the old cap)
 * MS_DEADLINE         | 1        | fallback | -DMS_DEADLINE=0                               | world  | #254  | mapstream.c   | deadline-aware hard-tier read order (late items first, rest by disc order); 0 = nearest first
 * MS_DL_FIRST         | 1        | fallback | -DMS_DL_FIRST=0                               | world  | #254  | mapstream.c   | a late item jumps the disc server queue (with MS_DEADLINE); 0 = queued by LSN like the rest
 * MS_GAP_KB           | 96       | tune     | -DMS_GAP_KB=0                                 | world  | #254  | mapstream.c   | a queued item within this many KB after the last read in the same pack goes next; 0 = off
 * WORLD_BOOT_RELIEF   | 1        | fallback | -DWORLD_BOOT_RELIEF=0                         | world  | #125  | world.h       | the boot block's relief streams; 0 = resident from boot
 * WORLD_BOOT_LEAN     | 1        | fallback | -DWORLD_BOOT_LEAN=0                           | world  | #206  | world.h       | the boot block's map tables are let go away from it (the far panorama stands in); 0 = resident for good
 * WORLD_STRUCT_WHOLE  | 2        | fallback | -DWORLD_STRUCT_WHOLE=3                        | world  | #147  | world.c       | wanted blocks counted whole per link (3 = the earlier worst case)
 * WORLD_SEAM_BUNDLE   | 1        | make     | -DWORLD_SEAM_BUNDLE=0                         | world  | #255  | mapstream.c   | block packs lite set first in read order (make WORLD_SEAM_BUNDLE=), map reads held while a lite set loads; 0 = old order, MS_HOLD_INFLIGHT 3
 * WORLD_SLOT_NEAREST  | 1        | fallback | -DWORLD_SLOT_NEAREST=0                        | world  | #236  | world.c       | with every slot taken by wanted blocks, the farthest gives its slot to a nearer wanted one; 0 = only unwanted blocks go
 * WORLD_FAR_RESERVE   | 1        | fallback | -DWORLD_FAR_RESERVE=0                         | world  | #250  | world.c       | a buffer of the largest far panorama reserved at boot for when the heap has no room; 0 = heap only
 * WORLD_WANT_3D       | 1        | fallback | -DWORLD_WANT_3D=0                             | world  | #250  | world.c       | wanted blocks read nearest first counting the height range; 0 = x / z only
 * WORLD_REACH         | 1        | fallback | -DWORLD_REACH=0                               | world  | #253  | world.c       | neighbours wanted by walking time to their seam (world.bin v5 reach table) against the measured turn-on time; 0 = the links' load masks alone (runtime: option set WORLD_REACH 0)
 * OBJ_STREAM          | 1        | fallback | -DOBJ_STREAM=0                                | game   | #188  | objects.c     | object types streamed by distance; 0 = all at load
 * PACK_JOB_ABANDON    | 1        | fallback | -DPACK_JOB_ABANDON=0                          | io     | #181  | pack.c        | a dropped load cancels instead of waiting (runtime: option set PACK_JOB_ABANDON 0)
 * PACK_CDROM          | probe    | debug    | -DPACK_CDROM=1                                | io     | init  | pack.c        | force disc (1) or host (0) paths
 *
 * map drawing (map.c, render.c, drawgrp.c, bump.c, main.c)
 * MAP_TILES           | 1        | fallback | -DMAP_TILES=0                                 | world  | #143  | map_i.h         | cells tested by tile first
 * MAP_NEAR_SKIP       | 1        | fallback | -DMAP_NEAR_SKIP=0                             | world  | #143  | map_i.h         | near-pass skips cells with no near level
 * MAP_TWO_PASS        | 1        | fallback | -DMAP_TWO_PASS=0                              | world  | #148  | map_i.h         | per-cell pre-pass and cached draw list, then the emit pass (off under MAP_DBG / MAP_PANO_R); 0 = the per-chunk loop
 * MAP_PASS_MS         | 1        | fallback | -DMAP_PASS_MS=0                               | world  | #143  | map.c         | "map: pass ms" timing log
 * SKY_ROWS            | 1        | fallback | -DSKY_ROWS=0                                  | world  | #183  | map_i.h         | sky chunks under their own fog row
 * ZSPLIT              | 50 m     | fallback | -DZSPLIT=0                                    | world  | #190  | map.c         | two Z ranges; 0 = one range
 * HZN_R               | 40 m     | tune     | -                                             | world  | #122  | map_i.h         | horizon radius
 * HZN_FADE            | 8 m      | fallback | -DHZN_FADE=0                                  | world  | #135  | map_i.h         | horizon fade-in; 0 = hard switch
 * HZN_ZBIAS           | 0.03     | fallback | -DHZN_ZBIAS=0                                 | world  | #135  | map_i.h         | fade depth bias; 0 = plain depth test
 * HZN_FADE_DEBUG      | 0        | debug    | -DHZN_FADE_DEBUG=1                            | world  | #135  | map_i.h         | fade pass drawn solid magenta
 * HZN_OFF             | off      | fallback | -DHZN_OFF                                     | world  | #122  | map.c         | horizon section ignored at runtime
 * NEAR_FADE           | 10       | fallback | -DNEAR_FADE=0                                 | world  | #156  | map_i.h         | near detail dissolves in; 0 = hard swap
 * NEAR_DRAW_R         | 18 m     | tune     | -                                             | world  | init  | map_i.h         | near level draw radius
 * FAR                 | 250 m    | tune     | -                                             | world  | init  | map_i.h         | edge fog distance
 * CULL_FAR            | FAR      | tune     | -DCULL_FAR=1000.0f                            | world  | init  | map_i.h         | opaque cull distance (A/B 1000 m)
 * SMALL_Z             | 40 m     | tune     | -                                             | world  | init  | map_i.h         | small-box cull depth
 * SMALL_PX            | 3 px     | tune     | -                                             | world  | init  | map_i.h         | small-box cull radius
 * TEX_POOL_KB         | 896      | tune     | -                                             | world  | init  | map_i.h         | VRAM map texture pool
 * LPOOL_N             | 12288    | tune     | -                                             | world  | #148  | map_i.h         | draw list pool entries
 * MAP_DBG             | off      | debug    | -DMAP_DBG=60; -DMAP_DBG=60 -DMAP_DBG_ALL -DMAP_DBG_FLIP | world  | init  | map.c map_i.h | chunks / tris dropped per step every N frames
 * MAP_DBG_ALL         | off      | debug    | (with MAP_DBG)                                | world  | init  | map.c         | MAP_DBG: no frustum skip
 * MAP_DBG_FLIP        | off      | debug    | (with MAP_DBG)                                | world  | init  | map.c         | MAP_DBG: chunks drawn last frame, not now
 * MAP_TWO_PASS_CHECK  | off      | debug    | -DMAP_TWO_PASS_CHECK                          | world  | #148  | map.c mapblock.c         | two-pass lists compared with one pass
 * MAP_PROF            | off      | debug    | -DMAP_PROF                                    | world  | init  | map.c         | visibility / EE clip time on the HUD
 * MAP_STATS           | off      | debug    | -DMAP_STATS                                   | world  | init  | map.c         | batches, pool traffic, streamed cells every 60 frames
 * MAP_POP_LOG         | 1        | debug    | -DMAP_POP_LOG=0                               | world  | #244  | map.c         | "map: pop cell" line per pop-in in test runs (in-view wait, why, draws after a settle)
 * MAP_PANO_R          | off      | debug    | -DMAP_PANO_R=60.0f -DPANO_ZOOM=2.0f           | main   | init  | main.c map.c map_i.h  | panorama capture build (tools/pano.py)
 * PANO_ZOOM           | 1.0      | debug    | (with MAP_PANO_R)                             | gfx    | init  | render.c      | panorama capture focal length
 * DG_SHARED           | 1        | fallback | -DDG_SHARED=0                                 | world  | #131  | drawgrp.c     | draw groups shared across blocks
 * DG_SEAM             | 1        | fallback | -DDG_SEAM=0                                   | world  | #251  | map.c         | the floor of another map of the area group within 16 m ahead adds its draw groups; 0 = the floor's alone
 * DG_SEAM_HOLD        | 30       | tune     | -DDG_SEAM_HOLD=0                              | world  | #252  | map.c         | frames a seam floor found ahead keeps adding its draw groups after the probe last hit it (0: none, the old path)
 * DG_SEAM_VIEW        | 1        | fallback | -DDG_SEAM_VIEW=0                              | world  | #252  | map.c         | DG_SEAM probes the camera's view direction as well as the heading (border draw groups); 0 = the heading alone
 * HZ_ONLY_OWN         | 1        | fallback | -DHZ_ONLY_OWN=0                               | world  | #252  | mapblock.c    | a kept-only block's horizon cells inside an on block of another map are left out; 0 = all drawn
 * MAP_FAR_SEAM        | 1        | fallback | -DMAP_FAR_SEAM=0                              | world  | #252  | mapblock.c    | far panorama seam sets (farpano.py FAR_SEAM): the one nearest the camera within 48 m stands in for its block's main sets; 0 = main sets only
 * VOID_RING           | 1        | fallback | -DVOID_RING=0                                 | world  | #252  | map.c         | last-resort backdrop: a band in the sky's fog colour from the horizon down over the sky dome, so voids read as fog, not sky (runtime: option set VOID_RING 0)
 * VOID_RING_SKY       | 1        | fallback | -DVOID_RING_SKY=0                             | world  | #252  | map.c         | VOID_RING in the sky's fog colour (header sky row); 0 = the map's current fog colour
 * VOID_RING_TOP       | 0.0f     | tune     | -                                             | world  | #252  | map.c         | VOID_RING: degrees above the horizon where the band is still opaque
 * VOID_RING_FADE      | 6.0f     | tune     | -                                             | world  | #252  | map.c         | VOID_RING: degrees over which it fades out above that
 * BUMP_OFF            | off      | fallback | -DBUMP_OFF -DBUMP_BAKED=0                     | gfx    | init  | bump.c        | runtime relief: no relief pass (A/B)
 * BUMP_COVER          | off      | debug    | -DBUMP_COVER -DBUMP_BAKED=0                   | gfx    | init  | bump.c        | runtime relief: covered pixels black
 * BUMP_CAP            | 1536     | tune     | -                                             | gfx    | init  | bump.c        | relief batches per frame
 * BUMP_POOL_KB        | 384      | tune     | -                                             | gfx    | init  | bump.c        | normal map VRAM pool
 * DOF_BANDS           | 3        | tune     | -                                             | main   | #34   | main.c        | depth of field bands
 * FPS_CAP_FIELDS      | 2        | fallback | -DFPS_CAP_FIELDS=1                            | main   | init  | main.c        | 30 fps cap; 1 = uncapped
 * V1B_FIELD           | off      | debug    | -DV1B_FIELD                                   | main   | init  | main.c        | point-sampled field output
 * R2_OFF              | off      | fallback | -DR2_OFF                                      | main   | init  | main.c        | no VRAM layout pass after the model upload (A/B)
 *
 * textures and VU1 (texcache.c, vu1.c, vu1.h, model.c)
 * TC_RLIST            | 1        | fallback | -DTC_RLIST=0                                  | gfx    | #143  | texcache.c    | resident-level list; 0 = walk every level
 * TC_PRESS            | 1        | fallback | -DTC_PRESS=0                                  | gfx    | #163  | texcache.c    | mip 1 under pool pressure (runtime: option set TC_PRESS 0)
 * TC_PLAN             | 1        | fallback | -DTC_PLAN=0                                   | gfx    | #234  | texcache.c    | texture levels planned to the pool budget per texture (runtime: option set TC_PLAN 0)
 * TC_NO_NEAR          | off      | fallback | -DTC_NO_NEAR                                  | gfx    | init  | texcache.c    | near texture levels never drawn (A/B)
 * TC_HUD              | off      | debug    | -DTC_HUD                                      | gfx    | init  | texcache.c    | upload numbers on screen
 * TC_NEAR_KB          | 256      | tune     | -                                             | gfx    | init  | texcache.c    | near level pool
 * MIP_BIAS            | 1.0      | tune     | -                                             | gfx    | #33   | texcache.c    | LOD bias in levels
 * MIP0_AHEAD          | 1.3      | fallback | -DMIP0_AHEAD=0                                | gfx    | #156  | texcache.c    | mip 0 asked ahead; 0 = where drawn
 * MIP_TRILINEAR       | 1        | fallback | -DMIP_TRILINEAR=0                             | gfx    | #184  | vu1.c         | trilinear; 0 = LINEAR_MIPMAP_NEAREST (runtime: option set MIP_TRILINEAR 0)
 * VU1_TEX_PATH2       | off      | fallback | -DVU1_TEX_PATH2                               | gfx    | #104  | vu1.h         | map texture uploads on PATH2
 * VU1_MAP_OLD         | off      | fallback | -DVU1_MAP_OLD                                 | gfx    | init  | vu1.c         | the old map batch path
 * VU1_IDX_LISTS       | off      | debug    | -DVU1_IDX_LISTS                               | gfx    | #124  | vu1.c         | indexed batches expanded to lists
 * VU1_IDX_NO_CLIP     | off      | debug    | -DVU1_IDX_NO_CLIP                             | gfx    | #124  | vu1.c         | indexed clip batches not drawn
 * VU1_NO_CLIP         | off      | debug    | -DVU1_NO_CLIP                                 | gfx    | init  | vu1.c         | clip batches through the plain program
 * VU1_CHECK           | off      | debug    | -DVU1_CHECK                                   | gfx    | #118  | vu1.c model.c | VIF chain checks in the EE log
 * CHAIN_MAP_QW        | 19000    | tune     | -                                             | gfx    | init  | vu1.c         | map chain buffer
 * CHAIN_CHR_QW        | 19100    | tune     | -                                             | gfx    | init  | vu1.c         | character chain buffer
 * CHAIN_FX_QW         | 4096     | tune     | -                                             | gfx    | #106  | vu1.c         | particle chain buffer
 * TEXQ_MAP_QW         | 4096     | tune     | -                                             | gfx    | #104  | vu1.c         | map texture queue
 * TEXQ_CHR_QW         | 256      | tune     | -                                             | gfx    | #104  | vu1.c         | character texture queue
 * TEXQ_SLICE          | 512      | tune     | -                                             | gfx    | #104  | vu1.c         | IMAGE qwords per GIF packet
 * SKIN_CLIP_MARGIN    | 1.5 m    | tune     | -                                             | gfx    | init  | vu1.c         | bone origin to farthest vertex
 * FX_VU1              | 1        | fallback | -DFX_VU1=0                                    | fx     | #106  | fx.c          | particles on VU1; 0 = EE billboards
 * V5_OFF              | off      | fallback | -DV5_OFF                                      | scene  | init  | model.c       | character CLUTs as exported (CT32)
 * MDL_SKIN_PAGE       | 64 KB    | fallback | -DMDL_SKIN_PAGE=0                             | scene  | #138  | model.c       | skin packets in pages; 0 = one block
 * MDL_ANIM_PAGE       | 64 KB    | fallback | -DMDL_ANIM_PAGE=0                             | scene  | #140  | model.c       | anim section in pages; 0 = one copy
 * SPEC_TEST           | off      | debug    | -DSPEC_TEST=0.85,0.8                          | scene  | init  | model.c       | metal / specular on every vertex
 * NO_VU0              | off      | fallback | -DNO_VU0                                      | core   | #94   | vmath.c       | skin matrix product in C
 *
 * characters, logic, collision (enemy.c, ai.c, lua0.c, lpass.h, collision.c, nav.c)
 * LOGIC_SPREAD        | 1        | fallback | -DLOGIC_SPREAD=0                              | gfx    | #156  | lpass.h       | logic work spread across frames
 * LOGIC_PASS_MS       | 1        | fallback | -DLOGIC_PASS_MS=0                             | gfx    | #156  | lpass.h       | logic pass timing log
 * ACTIVE_MAX          | 16       | tune     | -                                             | game   | init  | enemy.c       | characters updated a frame
 * CHR_OMIT            | 1        | fallback | -DCHR_OMIT=0                                  | game   | #234  | omit.h enemy.c | update omission levels (DSR FUN_14036e8d0); 0 = every active character every frame (runtime: option set CHR_OMIT 0)
 * OMIT_FULL_N         | 6        | tune     | -                                             | game   | #234  | omit.h        | CHR_OMIT: in-view characters at level 0
 * OMIT_MID_N          | 10       | tune     | -                                             | game   | #234  | omit.h        | CHR_OMIT: in-view characters at level 2 or better
 * OMIT_NEAR_M         | 12 m     | tune     | -                                             | game   | #234  | omit.h        | CHR_OMIT: out of view within this = level 5, not 30
 * OMIT_DT_MAX         | 0.25 s   | tune     | -                                             | game   | #234  | omit.h        | CHR_OMIT: longest step one update takes
 * DRAW_MAX            | 16       | tune     | -                                             | game   | init  | enemy.c       | characters drawn a frame
 * ENEMY_LAZY_LOAD     | 1        | fallback | -DENEMY_LAZY_LOAD=0                           | game   | #138  | enemy.c       | types loaded when near; 0 = every placement in LOAD_DIST
 * ENEMY_KEEP_DEAD     | 1        | fallback | -DENEMY_KEEP_DEAD=0                           | game   | #140  | enemy.c       | killed enemies stay dead until a rest
 * ENEMY_TMP_GENTLE    | 1        | fallback | -DENEMY_TMP_GENTLE=0                          | game   | #257  | enemy.c       | model temporaries never clear drawn cells while the heap has room; 0 = the arena at any cost first
 * DPAD_STICK          | 0        | fallback | -DDPAD_STICK=1                                | main   | #228  | main.c        | the d-pad moves like the left stick (old path); 0 = DSR's weapon / item d-pad
 * ENEMY_NEAR_PLACE    | 1        | fallback | -DENEMY_NEAR_PLACE=0                          | game   | #210  | enemy.c       | characters placed within 70 m, a few a frame; 0 = every placement of a block at once
 * POSE_SLOTS          | 40       | tune     | -                                             | game   | #212  | enemy.c       | shared pose pool slots (MAX_ENEMIES = one each, the old layout)
 * ENEMY_WALLS         | 1        | fallback | -DENEMY_WALLS=0                               | game   | #156  | enemy.c       | moves against walls and ledges; 0 = the old move (runtime: option set ENEMY_WALLS 0)
 * ENEMY_WALL_CHECK    | 1        | fallback | -DENEMY_WALL_CHECK=0                          | game   | #156  | enemy.c       | wall-hit log by move kind
 * ENEMY_ROOT_Y        | 1        | fallback | -DENEMY_ROOT_Y=0                              | game   | #248  | enemy.c       | root-motion height inside TAE IGNORE_GRAVITY windows (Taurus Demon's 9061 jump down); 0 = flat root motion
 * SEP_CARRY           | 1        | fallback | -DSEP_CARRY=0                                 | game   | #156  | enemy.c       | separation rides the next move (runtime: option set SEP_CARRY 0)
 * MOVE_PROF           | 1        | fallback | -DMOVE_PROF=0                                 | game   | #156  | enemy.c       | "enemy: moves" timing log
 * NO_AI_SCRIPTS       | off      | debug    | -DNO_AI_SCRIPTS                               | game   | init  | enemy.c       | no ESD / Lua
 * ENEMY_ONLY          | off      | debug    | -DENEMY_ONLY=c2900_c2540                      | game   | init  | enemy.c       | only these types
 * ENEMY_HIT_TEST      | off      | debug    | -DENEMY_HIT_TEST                              | game   | init  | enemy.c       | a hit on the nearest enemy every 2 s
 * LUA_GC_STEP_US      | 1000     | fallback | -DLUA_GC_STEP_US=0                            | game   | #95   | ai.c          | incremental Lua GC; 0 = stop the world
 * LUA_POOL_GROW       | 1        | fallback | -DLUA_POOL_GROW=0                             | game   | #140  | lua0.c        | Lua small objects in their own chunks (runtime: option set LUA_POOL_GROW 0)
 * COL_SCAN            | 1        | fallback | -DCOL_SCAN=0                                  | world  | #164  | collision.c   | box test before the lookups
 * PUSH2_CELLS         | 1        | fallback | -DPUSH2_CELLS=0                               | world  | #138  | collision.c   | push-out v2 by cells (runtime: option set PUSH2_CELLS 0)
 * NAV_QREJ            | 1        | fallback | -DNAV_QREJ=0                                  | world  | #164  | nav.c         | integer box reject before barycentrics
 * EV_ENT_KEEP         | 1        | fallback | -DEV_ENT_KEEP=0                               | game   | #213  | event.c       | entity switches kept by id and replayed when a block's events turn on; 0 = only the sets on at the time
 * PLAYER_RIM          | 1        | fallback | -DPLAYER_RIM=0                                | game   | #213  | player.c      | wall faces are no floor; the capsule's rim carries the player over small gaps; 0 = the centre point alone
 * PLAYER_PROXY        | 1        | fallback | -DPLAYER_PROXY=0                              | game   | #214  | player.c      | DSR's ChrProxy step (lift 0.31 m, edges ridden within 60 deg), step down 0.4 m, maxSlope 60 deg, slide over 35 deg; 0 = step 0.6 / 1.2, walls below 45 deg, no slide
 * SLIDE_SUPPORT       | 1        | fallback | -DSLIDE_SUPPORT=0                             | game   | #257  | player.c      | the slide reads the mean walkable normal under the capsule (Havok support), not one triangle; 0 = the triangle under the centre
 * CHR_PUSH_WEIGHT     | 1        | fallback | -DCHR_PUSH_WEIGHT=0                           | game   | #257  | enemy.c       | player / enemy capsule push shared by NpcParam weight (chr.bin v13); 0 = the enemy takes it all
 * RAG_CONTACTS        | 1        | fallback | -DRAG_CONTACTS=0                              | scene  | #241  | ragdoll.c     | ragdoll bodies collide with the map's walls and with each other; 0 = floor only
 * RAG_DEBUG           | 0        | debug    | -DRAG_DEBUG=1                                 | scene  | #241  | ragdoll.c     | EE log twice a second per awake ragdoll: fastest body, bodies at rest, body pushes
 * ESD_TRACE           | off      | host     | -                                             | game   | init  | esd.c         | ESD trace hook (native test harness)
 * OBJ_HOST            | off      | host     | -                                             | game   | init  | objects.c     | piece bodies for the host test harness
 * MEM_HOST_TEST       | off      | host     | -                                             | core   | new   | mem.c         | the pages / bump / pool parts only, for tools/ci/test_mem.sh on the host
 *
 * scripted test builds (testscript.c, testscript.h, debug_hud.c, hud.c, loading.c, main.c)
 * TEST_SPAWN          | off      | debug    | -DTEST_SPAWN=0,0,0,0; -DTEST_SPAWN=0,0,0,0 -DCOMBAT_DUMMY; -DTEST_SPAWN=0,0,0,0 -DENEMY_VIEW=0,0,3 | core   | init  | testscript.c | start position x,y,z,angle
 * COMBAT_DUMMY        | off      | debug    | (with TEST_SPAWN)                             | core   | init  | testscript.c  | dummy target in front of the spawn
 * ENEMY_VIEW          | off      | debug    | (with TEST_SPAWN)                             | core   | init  | testscript.c  | camera orbits an MSB placement
 * COL_TEST            | off      | debug    | -DCOL_TEST=1; -DCOL_TEST=7                    | core   | init  | testscript.c  | collision / movement runs from the spawn
 * ESD_TEST            | off      | debug    | -DESD_TEST                                    | core   | init  | testscript.c  | backstep, dash attacks, running jump
 * CHAR_CLOSEUP        | off      | debug    | -DCHAR_CLOSEUP=0.6,1.6,0.1                    | core   | init  | testscript.c  | camera close to the player
 * HUD_DEBUG           | off      | debug    | -DHUD_DEBUG                                   | core   | init  | debug_hud.c   | debug overlay on from boot
 * HELM_OFF            | off      | debug    | -DHELM_OFF                                    | main   | init  | main.c        | start with the helm off
 * SLOW_FRAME_LOG      | off      | debug    | -DSLOW_FRAME_LOG                              | main   | #75   | main.c pack.c | frames over 100 ms in the EE log
 * NO_HUD              | off      | debug    | -DNO_HUD                                      | ui     | init  | hud.c         | HUD loaded, not drawn
 * DIED_TEST           | off      | debug    | -DDIED_TEST                                   | ui     | init  | hud.c         | YOU DIED banner after 3 s
 * LOADING_OFF         | off      | fallback | -DLOADING_OFF                                 | ui     | #67   | loading.c     | no loading screen
 */
#ifndef SWITCHES_H
#define SWITCHES_H
#endif
