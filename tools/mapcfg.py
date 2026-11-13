"""The map exporter's settings (environment knobs, constants, shared state), split from export_map.py (2026-11-12, code moved as it was; tools/export_map.py runs it)."""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


TEX_MAX = 128
# breakable types tools/export_objects.py left out for its RAM budget (its <objects.bin>.static list, Makefile passes
# STATIC_OBJ=<file>): drawn as static map objects (intact model) instead of not at all
STATIC_BREAKABLES = set(open(os.environ["STATIC_OBJ"]).read().split()) if os.environ.get("STATIC_OBJ") else set()
# animated objects (doors, levers, gates, chests: export_objects.py's <objects.bin>.anim part names, Makefile passes
# ANIM_OBJ=<file>): src/objects.c draws them with their anims, so they are not baked
ANIM_PARTS = set(open(os.environ["ANIM_OBJ"]).read().split()) if os.environ.get("ANIM_OBJ") else set()
# PS2 triangle budget: most Firelink pieces only have LOD0 (and the game's LOD1/2, where present, are worse than
# QEM at the same count). Foliage is thinned by whole card / tuft (keep 1 in GRASS_KEEP, each with a rank that map.c
# compares with a density falling off with distance, see thin_foliage); everything else goes through quadric
# edge collapse (tools/simplify.py) with the error judged at each vertex's view distance max(NEAR, distance to the
# navmesh, SPAWN_K x distance to the spawn), and one error threshold for the whole map searched to fit BUDGET
# triangles, foliage not included (EPS fixes the threshold instead). GAME_LOD=1: the game's lowest LOD face sets and no
# simplification (for comparison).
GRASS_KEEP = int(os.environ.get("GRASS_KEEP", 16))
FOL_XZ = float(os.environ.get("FOL_XZ", 1.0))  # kept foliage tufts widened (see thin_foliage)
FOL_Y = float(os.environ.get("FOL_Y", 1.0))
FOL_IVY = float(os.environ.get("FOL_IVY", 1.0))
BUDGET = int(os.environ.get("BUDGET", 90000))
EPS = float(os.environ["EPS"]) if os.environ.get("EPS") else None
# largest simplification error when BUDGET cannot be met (angular error^2; sqrt x 568 = px at 640x448): 24 px
EPS_MAX = float(os.environ.get("EPS_MAX", (24 / 568) ** 2))
NEAR = float(os.environ.get("NEAR", 4.0))
# Two-level map with streamed near cells (render-distance; STREAM=1, map.bin 'DSM5' + cells.bin): every simplified mesh
# is cut by CHUNK_GRID cells and simplified twice with the vertices on cell cuts locked, so the two levels of any two
# cells meet without cracks. Near level: error EPS (or the BUDGET search) judged at max(NEAR, distance to navmesh), in
# cells.bin, one blob per cell, streamed by src/mapstream.c; coarse level: error EPS_FAR judged at max(STREAM_D, ...),
# resident in map.bin and drawn wherever a cell's near blob is not loaded (CHUNK_COARSE).
STREAM = os.environ.get("STREAM") == "1"
EPS_FAR = float(os.environ.get("EPS_FAR", (6 / 568) ** 2))
STREAM_D = float(os.environ.get("STREAM_D", 32.0))
# far-only cells (STREAM_FAR, the backdrop): their own coarse error. QEM's point-to-plane error lets vertices slide
# along a face's plane, so silhouettes move far more than EPS_FAR says: Firelink's ring wall (render-12) rose ~20 px
# with merlons / pillar caps collapsed and crack pinholes at 8 px, and matches DSR at 3 px. ASSUMED 3 px (finer: 1 px
# costs Firelink's whole near arena when applied to every coarse cell; backdrop-only it is a few k tris).
BACKDROP_EPS = float(os.environ.get("BACKDROP_EPS", (3 / 568) ** 2))
# cells whose centre is farther than this from every navmesh vertex never come within mapstream.c's 40 m load radius
# (+ half a cell diagonal + margin): one resident level, simplified without cell cuts (only their border with streamed
# cells stays locked), no near blob
STREAM_FAR = float(os.environ.get("STREAM_FAR", 78.0))
# thin faces of the coarse / backdrop levels (render-12, soften_thin): narrower than SOFT_PX pixels at their nearest
# view distance, their colour goes toward their surroundings. ASSUMED 2 px (measured on Firelink's ring wall pillars);
# SOFT_FOCAL = the runtime's horizontal focal length in px (render.c FOCAL_X at 512 px); SOFT_PX=0 turns it off (A/B)
SOFT_PX = float(os.environ.get("SOFT_PX", 2.0))
SOFT_FOCAL = 609.0 * 512 / 640
SOFT_REACH = 40.0  # m: viewpoints for the projected width: navmesh samples within this of the nearest (ASSUMED)
SOFT_R_PX = float(os.environ.get("SOFT_R_PX", 4.0))  # footprint radius of the colour average, px (ASSUMED)
CHUNK_COARSE = 64
# Spot budgets (world streaming v2, src/mapstream.c): the bytes of a level's streamed cells around any navmesh point.
# Measured 2026-11-06: near cells within 40 m of the Burg entry were 10.2 MB and coarse within 260 m 13.4 MB against a
# 10.8 MB arena, so the streamer could never hold what it wanted and dropped coarse cells for near ones (holes).
# solve_budget raises the thresholds of the least simplified cells around a spot that is over budget until every
# spot fits: detail drops only where the data would not fit, evenly over the spot. Near: cells within WS_R0 (mapstream
# MS_R0_WANT). Coarse: cells within WS_R1_IN..WS_R1 (mapstream pins the coarse level within MS_R1_PIN; inside WS_R1_IN
# a cell draws its near level). Sizes (KB) below; per area through MAPENV_<area>. WS_BUDGET=0: no solve (A/B).
WS_BUDGET = os.environ.get("WS_BUDGET", "1") == "1"
# Since 2026-11-08 the sizes are the project's DVD-9 budget (PS2 hardware budgets from the SDK, "Disc and read-ahead":
# 1.5 MB/s sustained, near 600 KB/s / coarse 400 KB/s of it at a 6 m/s sprint): near 2.5 MB within 30 m, coarse 3 MB
# within 16-96 m. The coarse level stays at 2 MB: the 10 MB arena, not the disc, limits it. At 3 MB no Burg coarse cell
# was simplified (0 of 60) and the pinned coarse level left no room for the near cells at the Burg bonfire (its statue
# drawn coarse) or the Taurus tower (ISO 2026-11-08, tools/world_budget.py). A CD build would take WS_B0=1536
# (500 KB/s). What all blocks ask of the arena together is checked by tools/world_budget.py.
WS_B0 = float(os.environ.get("WS_B0", 2560)) * 1024
WS_B1 = float(os.environ.get("WS_B1", 2048)) * 1024
WS_R0, WS_R1, WS_R1_IN = 30.0, 96.0, 16.0
# WS_EXTENTS (default on; 0 = cubes as before): spot distances to a cell are measured as mapstream.c / map.c measure
# them: the coarse level to the box of its triangles, the near level to that box clipped to its CHUNK_GRID cube
# (NEAR_CUBE; without it the whole box as well) (solve_budget, cell_extents)
WS_EXTENTS = os.environ.get("WS_EXTENTS", "1") == "1"
# error caps of the solve (px at 640x448): ASSUMED twice the Burg's near / coarse thresholds (6 / 12 px)
WS_PX0 = float(os.environ.get("WS_PX0", 12.0))
WS_PX1 = float(os.environ.get("WS_PX1", 24.0))
# ST_ALIGN (default on; 0 = every triangle keeps its own whole-repeat shift, the old export): triangles of a batch that
# share a corner up to whole texture repeats get the same shift, so the indexed chunk stores the corner once
# (tools/mapx/chunks.c st_batch)
ST_ALIGN = os.environ.get("ST_ALIGN", "1") == "1"
# bytes per triangle of a streamed chunk (positions, st, colours, padding, record: 44-45 measured over 4 blocks as lists;
# indexed chunks (INDEXED, below): 31.2 measured on the Undead Burg west block's near level, 33 on its coarse level;
# with ST_ALIGN 29.3 over both levels of that block, 31.8 without)
WS_BPT = (29 if ST_ALIGN else 31) if os.environ.get("INDEXED", "1") == "1" else 45
# WS_UNION (Burg plan B3, default on; WS_UNION=0 = the per-block budgets above): the spot budgets are what the arena's
# visual room holds at a spot, shared by every block loaded there. The runtime measures a spot as tools/world_budget.py
# does: near parts within WS_NEAR_R (mapstream.c MS_R0_DRAW) and the pinned coarse level from there to WS_PIN_R
# (MS_R1_PIN), plus kept meshes and the backdrop. The room (WS_ROOM KB: world.c WORLD_ARENA_KB less the structural and
# misc pools, x 92 %, as world_budget.py prints it) is split into WS_NEAR_KB for the near level and the rest for the
# coarse level less WS_FAR_KB for kept meshes, backdrop and sky (ASSUMED: their p90 at the Burg spots); a block of a
# world group with neighbours of its own MSB (tools/area.py BLOCKS cells) gets the share of each budget that its cells
# cover of the spot's disc (WS_NEAR_R for the near level, WS_PIN_R for the coarse level), the neighbours the rest.
# Other MSBs (Firelink, the Asylum) are not shared out: tools/world_budget.py checks those seams.
WS_UNION = os.environ.get("WS_UNION", "1") == "1"
# The hard tier (2026-11-08, the plan's review): near within 20 m, coarse and kept meshes within 40 m (map.c HZN_R 40:
# the horizon stands in beyond), against the room world_budget.py prints for world.c's WORLD_ARENA_KB 9152 (3618 KB with the Burg's lite sets, 2026-11-08)
# 2026-11-10 refit: collision tiles (#157), one events.bin / texture set per map (#159-#163) shrank the structural pool
# to 3376 KB and world_budget.py prints a 4450 KB room on main 4fa8d0d; WS_ROOM is that less 3 % (ASSUMED margin: more
# chunks grow the structural pool, +41 KB with the structure weight, TECH_DEBT map-41). The near level gets 60 % of it
# (was ~50 %): with the Burg's caps back at 12 / 24 px the strict gate is at 0.00 % (main 0.49 %), near level tris
# +11..+29 % per block, hard tier max 4290 KB. The standalone Burg ELF (MAP=m10_01_00_00, no world) keeps the old
# numbers (Makefile MAPENV_m10_01_00_00): its own arena crashed at these (grass_tris, 2.15 MB free).
# Since 2026-11-12 (TECH_DEBT world-14) the room and its near share are the block's RES_BLOCK row of
# ps2/proto/src/residency.h (tools/residency.py; set in main() once the block is known): 4175 / 2505 KB, the 4239 KB
# visual room the ISO's arena has (9088 KB, pools in whole pages) less a page; 4317 / 2590 before, against a room the
# runtime never had. WS_ROOM / WS_NEAR_KB in the environment (MAPENV) still win (A/B, the standalone Burg ELF).
# (3618 room: near 1792; 2048: 4 more seam spots over; 1536: the Parish near level 19 % on the allowance)
WS_ROOM = float(os.environ.get("WS_ROOM", 0))
WS_NEAR_KB = float(os.environ.get("WS_NEAR_KB", 0))
WS_FAR_KB = float(os.environ.get("WS_FAR_KB", 128))  # kept meshes (ASSUMED: ~p90 of the Burg's within 40 m)
# a spot still over at the caps may take its cells past the cap up to WS_ALLOW_PX (the seam allowance: two blocks'
# cells at one spot); world_budget.py fails the build when more than 1 % of the spots need it
import residency  # noqa: E402 (ps2/proto/src/residency.h: radii and per-block budgets shared with the runtime)
WS_NEAR_R = float(os.environ.get("WS_NEAR_R", residency.RES["RES_NEAR_R"]))
WS_PIN_R = float(os.environ.get("WS_PIN_R", residency.RES["RES_PIN_R"]))
# the export stops when, after the solve, more than this % of the block's own spots are still over its budget in a
# level (the error caps and the seam allowance spent): the block's data cannot fit the room it was given
WS_OVER_PCT = float(os.environ.get("WS_OVER_PCT", residency.RES["RES_ALLOW_BLOCK_PCT"]))
WS_ALLOW_PX = float(os.environ.get("WS_ALLOW_PX", 72.0))
# a block's share of a seam spot's room, x WS_SHARE_K (capped at the whole room): the neighbour's cells seldom fill
# their part of the disc as densely as the street they meet at (ASSUMED, sized so tools/world_budget.py's hard tier
# over the union of blocks stays at 0 %)
WS_SHARE_K = float(os.environ.get("WS_SHARE_K", 1.0))
CHUNK_BACKDROP = 128  # far background (far-only cells beyond STREAM_FAR of the navmesh): never distance / subpixel culled
SPAWN_K = float(os.environ.get("SPAWN_K", 0.6))
GAME_LOD = os.environ.get("GAME_LOD") == "1"
# objects drawn at full detail (no simplification): the bonfire (o0200, area.py's spawn rule) sits next to every
# spawn and is the close-up focal point; one global error threshold (up to 10 px at NEAR) shredded its sword and
# bones. Small (a few k tris) and placed only where DSR has a bonfire. Extend with KEEP_MODELS=o0200,oXXXX.
KEEP_MODELS = set(os.environ.get("KEEP_MODELS", "o0200").split(","))
# ...and the ground around them: within KEEP_R m (xz) of a placed KEEP_MODELS object, map vertices are judged at
# KEEP_D m (about 9 mm error instead of up to 7 cm), so simplified ground cannot rise over the bonfire's low sticks,
# bones (0.35 m) and ash ring (4 cm). Positions are collected by gather().
KEEP_R = float(os.environ.get("KEEP_R", 2.0))
KEEP_D = float(os.environ.get("KEEP_D", 0.5))
KEEP_POS = []
# area-burg-coverage (TECH_DEBT map-4, area-1): POSE_OBJ=0 skips skinned objects again, FAR_OBJ=0 keeps objects to the
# area region instead of map_radius (A/B)
POSE_OBJ = os.environ.get("POSE_OBJ", "1") == "1"
FAR_OBJ = os.environ.get("FAR_OBJ", "1") == "1"
FAR_LOD = os.environ.get("FAR_LOD", "1") == "1"  # map pieces beyond STREAM_FAR of the navmesh: lowest game LOD (A/B: 0)
FAR_LOD_STATS = [0, 0]  # pieces with several LODs: [kept LOD0, took the lowest]
DRAW_GROUPS = os.environ.get("DRAW_GROUPS", "1") == "1"
# Occlusion (tools/pvs_bake.c, src/pvs.c, 2026-11-06): every chunk holds triangles of one MSB part only (the PVS unit
# is a part within one CHUNK_GRID cell) and of one occluder class; pvsin.bin hands the chunks' triangles to the bake.
# PVS=0: chunks mix parts as before and no pvsin.bin is written (pvs.bin then shows everything)
PVS = os.environ.get("PVS", "1") == "1"  # 0: chunks ignore draw groups, dg.bin shows everything (A/B)
PARK_DIST = 150.0  # metres: objects farther than this from every walkable navmesh vertex are parked, not drawn
HDR = 80
CHUNK_CULL, CHUNK_BLEND, CHUNK_ADD, CHUNK_SKY = 1, 2, 4, 8
CHUNK_ST1K = 32  # texture coordinates in 1/1024 units (some >= 16) instead of 1/4096
# Sky chunks: flags bits 12-15 = 1 + index of the chunk's FogBank row in the fog section's sky rows (0: none). DSR fogs
# every map piece by the row of its own MSB FogID (exe FUN_140565600 reads the row by id; a RenderDoc frame of Firelink
# holds rows 20, 22 and 23 at once, TECH_DEBT map-15), the sky and far background included: Undead Burg's far pieces use
# three rows (15, 16, 19) that end 1.6-2 km out, so their density still ramps where the pieces stand.
CHUNK_SKY_ROW = 12
SKY_FOG_ROWS = []  # gather: FogBank row ids of the sky meshes, in slot order
# Light, fog colour and tone map follow DSR's own math (tools/dsr_light.py), from the area's own banks and MSB.
# CHAR_ALB: sRGB albedo the character light is fitted for.
SHADOWS = os.environ.get("SHADOWS", "1") == "1"  # bake sun shadows (dsr_light.ShadowMap)
# Shadow strength on probe + hemisphere light: gFC_ShadowColor 0.8 (MEASURED, RenderDoc at Firelink) x 0.75, the
# share of DSR's camera-distance fade (full to 3 m, gone at 10 m) averaged over a 0-20 m view (ASSUMED)
SHADOW_AMB = float(os.environ.get("SHADOW_AMB", 0.8 * 0.75))
# exposure = grayKey / clamp(geometric-mean scene luminance) (MEASURED: inverting the tone map on refcap's DXVK
# captures gives an exposed geo-mean of 0.189 at Firelink spot A = grayKey, i.e. unclamped, and 0.066 at the Burg
# spawn, i.e. the minAdaptedLum clamp; the old "sky keeps the view at maxAdapredLum" guess made Burg 3.6x too dark)
SUN_FLOOR = [1.0]  # share of the main area's floor vertices in sun (gather's shadow bake), for the character light
CHAR_ALB = 0.35
MAIN_TONE = [None]  # the lit map's tone map (gather), for the header fog colours
SCATTER = [None]  # the main collision row's dsr_light.Scatter (gather): bake, fog colours, character light
TONES = {}  # (ToneMapID, ToneCorrectID) -> dsr_light.Tone, exposure adapted by gather()
SKY_DIST = 500.0  # metres: meshes entirely this far from every walkable navmesh vertex are sky / background


# Shape envelope (tools/native/simplify.c g_env_pad; the C simplifier only): no collapse may move the surface further
# than this many metres from the source mesh, whatever error the budget solve asks of a cell, so a tower stays a tower
# when its cell is pushed to the caps. SIMPLIFY_ENV_NEAR: the near level (and areas without levels), SIMPLIFY_ENV_FAR:
# the coarse level and the backdrop. 0: off (the old export). ASSUMED values, see TECH_DEBT map-43
# SIMPLIFY_ENV_STRICT (default 1, the maintainer's choice 2026-11-10: shapes first): the envelope holds in every cell,
# also where the budget solve cannot pay for it (the world blocks grew 5-12 %, 0.31 % of the Burg's spots are over the
# room and near cells were refused on the iso_ame_report3 route). 0: cells in reach of a spot the solve leaves over
# budget take the run without the envelope (simplify_meshes_c), so no spot holds more than with the envelope off
SIMPLIFY_ENV_STRICT = os.environ.get("SIMPLIFY_ENV_STRICT", "1") == "1"
SIMPLIFY_ENV_NEAR = float(os.environ.get("SIMPLIFY_ENV_NEAR", 0.25))
SIMPLIFY_ENV_FAR = float(os.environ.get("SIMPLIFY_ENV_FAR", 1.0))
# Triangle guard (the maintainer's rule 2026-11-10: no exploded, stretched or sliver polygons, whatever it costs in
# triangles or memory; tools/native/simplify.c g_qmin / g_flip_cos / g_shape_tol). No collapse may leave a triangle
# turned more than SIMPLIFY_FLIP_DEG from its source triangle over all steps; with (almost) no area left; thinner than
# SIMPLIFY_QMIN (shape quality 4 sqrt(3) area / sum of squared edges: 1 equilateral, 0.1 about 1 : 17, 0.05 about
# 1 : 35 height to length) unless it got no thinner (0 by default: at 0.1 the world budget gate failed at 9.8 % of the
# spots, at 0.05 at 4.1 %, against 0.71 % without it, limit 1 %); or with a corner, edge middle or centre further than the level's envelope
# (SIMPLIFY_ENV_*) from every source triangle of its own mesh (a material stretched over another one's ground, the
# board sliver of the 2026-11-10 play report). Then SIMPLIFY_VALIDATE measures every replayed triangle against its
# source again, independently (simpmesh.c mx_simp_check), and steps each job with a failing triangle back to before
# the collapse that last changed it until none fails (0 steps is the source: always passes); the export stops if one
# still fails. SIMPLIFY_GUARD=0: neither (the old export). ASSUMED thresholds, see TECH_DEBT map-44
SIMPLIFY_GUARD = os.environ.get("SIMPLIFY_GUARD", "1") == "1"
SIMPLIFY_QMIN = float(os.environ.get("SIMPLIFY_QMIN", 0.0))
SIMPLIFY_FLIP_DEG = float(os.environ.get("SIMPLIFY_FLIP_DEG", 90.0))
SIMPLIFY_VALIDATE = SIMPLIFY_GUARD and os.environ.get("SIMPLIFY_VALIDATE", "1") == "1"
SIMPLIFY_VALIDATE_TOL = float(os.environ.get("SIMPLIFY_VALIDATE_TOL", 0))  # metres; 0: the level's envelope (else 0.25)
# SIMPLIFY_REFIT (default on; 0 = each surviving vertex keeps its source colour, the old export): after the simplifier,
# a level's surviving wedges take the area-weighted mean colour of the source wedges that collapsed into them
# (tools/mapx/simpmesh.c mx_simp_refit), per level, so the near and the coarse level each get their own colours
SIMPLIFY_REFIT = os.environ.get("SIMPLIFY_REFIT", "1") == "1"
CHUNK_TRIS = 54  # per VU1 batch: 7 header qwords + 3 qwords per vertex = 493 qw, the VU buffer (OFFSET 493): the limit
CHUNK_GRID = float(os.environ.get("CHUNK_GRID", 32.0))  # metres: triangles are grouped by the grid cell of their centroid
