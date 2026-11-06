"""Export a DS1 map (default m10_02_00_00, Firelink Shrine) for the PS2 prototype.

Geometry: every MSB map piece (part type 0) and static object (type 1, obj/oXXXX.objbnd) at its placement
(position, XZY Euler degrees, scale), each FLVER mesh's LOD0 face set, simplified to a triangle budget (see BUDGET
below; foliage cards thinned instead). Static vertex light: the part's LightBank row (param/DrawParam/aXX, MSB part
LightID) baked per vertex x FLVER vertex colour. Effect materials (light shafts '_Add', mist '[Dn]_Alp') and the sky
(meshes farther than SKY_DIST from the spawn: sky dome, alpha cloud layer, distant ground ring) become blended / sky
chunks with vertex alpha and alpha in their CLUT; they are not simplified. Fog: the FogBank row most map pieces use,
in the header, plus the sky part's own row as a constant fog for the sky. Textures: each mesh's
diffuse from the area's TPF binders (map/mXX/mXX_000N.tpfbhd), largest mip <= TEX_MAX, 255 colours + one fully
transparent index (alpha < 50%) for cut-out foliage, CSM1-ordered CLUT (blended materials: 256 colours with alpha).
Walkable floor: the MSB navmesh parts.

Geometry is cut into VU1-sized chunks (<= CHUNK_TRIS triangles, grouped by CHUNK_GRID cell) of unindexed vertex
streams that the PS2 DMAs straight into VU1 memory.

Output map.bin (little-endian; streams 16-byte aligned, texture data 128-byte aligned):
  header  u32 'DSM4', chunks, textures, nav_verts, nav_tris; f32 spawn x, y, z, yaw;
          u32 offsets: chunks, textures, nav_verts, nav_tris; f32 fog begin, fog end (view z, metres), fog max
          density (may exceed 1: density = min(1, max x linear ramp)); u8 fog r, g, b, 0x80 (all zero: no fog);
          u8 sky fog r, g, b, density x 255; u32 fog section offset; pad to 80 bytes
  chunks  {s32 texture; u32 flags, tri count, pos offset, st offset, colour offset; f32 min[3], max[3];
          f32 origin[3], position step}  (64 B)
          flags bit 0: cull back faces (FLVER face set flag; front faces are clockwise on screen); bit 1 blended
          (alpha blend, no Z write); bit 2 additive (with bit 1); bit 3 sky (sky fog, no distance cull, drawn first);
          bit 4 foliage: bits 8-15 triangles per tuft, bits 16-23 / 24-31 lowest / highest tuft rank x 255 (tufts
          sorted by rank; map.c draws the tufts whose rank is below its density for the chunk's distance);
          bit 5: texture coordinates in 1/1024 units, else 1/4096
  streams pos s16[3] (world = origin + n x step; step a power of two, origin whole metres), st u16[2] normalized x
          4096 or 1024 (per-triangle shifted >= 0), colour u8[4] light (0x80 = 1.0) and alpha (vertex alpha for
          blended chunks, else 0x80); each stream zero-padded to 16 bytes (build_chunks), with a VIF code qword in
          front (chunk_streams: the three streams form one VIF packet; DSM5 cell table flag CELLS_VIFPKT)
  textures 32-byte entries + CLUTs + mip 0 / 1 pixels, see tools/texmap.py (PSMT4 / PSMT8, CT16 / CT32 CLUTs)
          nav_verts f32[3]  nav_tris u16[3]
  fog     u32 rows, collision tris, 0, 0; rows (48 B, fog_section docstring: FogBank row, character light, player
          light, specular terms, far depth of field; row 0 = the header's); u8 row per collision.bin triangle (the draw-param set of its
          MSB collision part)
DSM5 (STREAM=1, two levels; render-distance): as DSM4, plus header u32 cell table offset (after the fog offset);
          the chunk table holds the resident chunks grouped by cell (sky chunks last); flags bit 6 = coarse level
          (drawn only while the cell's near blob is not loaded). Cell table and cells.bin (written next to out.bin,
          packed raw so it can be read in place): see write_cells. Without STREAM, cells.bin is empty.
Usage: export_map.py <game dir> <out.bin> [map id] [preview prefix]
       export_map.py --render <map.bin> <preview prefix>   (offline renders of an existing map.bin, see VIEWS)"""
import collections.abc, functools, gc, json, math, os, struct, sys, time

# Python's cyclic GC: with the default thresholds the exporter's millions of long-lived tuples / lists were scanned by
# 45 full collections, 47 s of Firelink's 259 s (profiled 2026-11-02; also most of the simplify cache's load time). The
# exporter makes almost no reference cycles, so young collections run 100x less often and full ones only every ~500M
# allocations; gather()'s result is frozen out of later scans (main). EXPORT_GC=default keeps Python's thresholds (A/B).
if os.environ.get("EXPORT_GC") != "default":
    gc.set_threshold(70000, 50, 100)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import havok_anim as ha
import texmap
import resample  # Image.resize with a fixed pass order (TECH_DEBT build-7)
import export_normal
import dsr_light
import area as area_mod
import pose_obj
import drawgroups
import emevd
import mapx_native
import numpy as np

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
# a cell draws its near level). ASSUMED sizes (KB): arena ~10.8 MB less ~3.5 MB of neighbour lite sets + relief at a
# seam, shared by up to three blocks; per area through MAPENV_<area>. WS_BUDGET=0: no solve (A/B).
WS_BUDGET = os.environ.get("WS_BUDGET", "1") == "1"
WS_B0 = float(os.environ.get("WS_B0", 1536)) * 1024
WS_B1 = float(os.environ.get("WS_B1", 2048)) * 1024
WS_R0, WS_R1, WS_R1_IN = 30.0, 96.0, 16.0
# error caps of the solve (px at 640x448): ASSUMED twice the Burg's near / coarse thresholds (6 / 12 px)
WS_PX0 = float(os.environ.get("WS_PX0", 12.0))
WS_PX1 = float(os.environ.get("WS_PX1", 24.0))
# bytes per triangle of a streamed chunk (positions, st, colours, padding, record: 44-45 measured over 4 blocks as lists;
# indexed chunks (INDEXED, below): 31.2 measured on the Undead Burg west block's near level, 33 on its coarse level)
WS_BPT = 31 if os.environ.get("INDEXED", "1") == "1" else 45
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


CHUNK_TRIS = 54  # per VU1 batch: 7 header qwords + 3 qwords per vertex = 493 qw, the VU buffer (OFFSET 493): the limit
CHUNK_GRID = float(os.environ.get("CHUNK_GRID", 32.0))  # metres: triangles are grouped by the grid cell of their centroid
# (map.c culls by 32 m cells too), then cut into full, compact batches (split_batch)


def chunk_streams(ntri, pos, st, col, nv=0, idx=None):
    """A chunk's vertex streams as one VIF packet (render rebuild PR 1): a code qword in front of each stream
    {NOP, NOP, STCYCL 3,1, UNPACK V3-16 n -> TOPS+7} / {NOP x 3, UNPACK V2-16 unsigned n -> TOPS+8} / {NOP x 3,
    UNPACK V4-8 unsigned n -> TOPS+9}, then the stream zero-padded to 16 bytes (zeros = VIF NOPs). vu1.c sends the whole
    packet with one DMA REF (VU1_PKT); the stream offsets point past each code qword, so batches that send part of a
    chunk (foliage prefixes, clip splits) still REF the streams one by one. +48 bytes per chunk.
    Indexed chunks (nv vertices, idx: index_chunk): n = nv, the streams go to TOPS+8 / 9 / 10 (vu1mapi.vsm's header is
    8 qwords), then a fourth code qword {NOP x 2, STCYCL 1,1, UNPACK V4-8 unsigned ntri -> TOPS+8+3 nv} and the
    triangle stream (u8 x4 per triangle). +64 bytes per chunk.
    Returns (bytes, [pos, st, col offsets in it])."""
    n, a = (nv, MAPI_HDR) if nv else (3 * ntri, 7)
    assert 0 < n < 256, n
    codes = ((0, 0, 0x01000103, 0x69008000 | a | n << 16), (0, 0, 0, 0x6500C000 | (a + 1) | n << 16),
             (0, 0, 0, 0x6E00C000 | (a + 2) | n << 16))
    b, offs = bytearray(), []
    for arr, cw in zip((pos, st, col), codes):
        b += struct.pack("<4I", *cw)
        offs.append(len(b))
        b += arr
        b.extend(b"\0" * (-len(b) % 16))
    if nv:
        assert MAPI_HDR + 3 * nv + ntri <= MAPI_IN_QW
        b += struct.pack("<4I", 0, 0, 0x01000101, 0x6E00C000 | (MAPI_HDR + 3 * nv) | ntri << 16)
        b += idx
        b.extend(b"\0" * (-len(b) % 16))
    return b, offs


# Indexed chunks (vu1mapi.vsm, vu1.c vu1_add_idx): every chunk but foliage stores each distinct corner (position, st,
# colour) once plus a triangle stream of u8 x4 {3 v0, 3 v1, 3 v2, 0}, like the shared vertices of Sony's HiG and
# other indexed PS2 renderers. Undead Burg west: 1.61 distinct corners per triangle, 45 -> ~28 bytes per triangle.
# INDEXED=0: lists as before (A/B)
INDEXED = os.environ.get("INDEXED", "1") == "1"
MAPI_HDR, MAPI_IN_QW = 8, 493  # tools/vu1/gen.py MAPI_HDR, MAP_IN_QW


def index_chunk(ntri, pos, st, col):
    """Unindexed corner streams -> (nv, pos, st, col, idx) with the distinct corners in first-use order (the count padded
    to a multiple of 3 with copies of the last one: vu1mapi transforms three per loop pass), or None when the chunk
    does not fit the VU1 buffer that way or is no smaller."""
    n = 3 * ntri
    P = np.frombuffer(bytes(pos[:6 * n]), np.uint8).reshape(n, 6)
    S = np.frombuffer(bytes(st[:4 * n]), np.uint8).reshape(n, 4)
    C = np.frombuffer(bytes(col[:4 * n]), np.uint8).reshape(n, 4)
    K = np.ascontiguousarray(np.concatenate([P, S, C], 1)).view("V14").ravel()
    _, first, inv = np.unique(K, return_index=True, return_inverse=True)
    order = np.argsort(first, kind="stable")  # distinct corners in first-use order
    rank = np.empty(len(order), np.int64)
    rank[order] = np.arange(len(order))
    src = first[order]
    nv = -(-len(src) // 3) * 3
    if nv > 255 or MAPI_HDR + 3 * nv + ntri > MAPI_IN_QW:
        return None
    src = np.concatenate([src, np.repeat(src[-1:], nv - len(src))])
    tri = rank[inv.ravel()].reshape(ntri, 3) * 3
    idx = np.zeros((ntri, 4), np.uint8)
    idx[:, :3] = tri
    pad = lambda k: -(-k // 16) * 16
    if pad(6 * nv) + 2 * pad(4 * nv) + pad(4 * ntri) + 16 >= pad(6 * n) + 2 * pad(4 * n):
        return None
    return nv, P[src].tobytes(), S[src].tobytes(), C[src].tobytes(), idx.tobytes()


def chunk_packet(ti, fl, ntri, pos, st, col):
    """chunk_streams of one chunk tuple, indexed when it pays (index_chunk; never foliage) ->
    (bytes, offsets, ntri word for the chunk record: ntri | nv << 16)"""
    ix = index_chunk(ntri, pos, st, col) if INDEXED and not fl & CHUNK_GRASS else None
    if ix is None:
        pk, offs = chunk_streams(ntri, pos, st, col)
        return pk, offs, ntri
    nv, p2, s2, c2, idx = ix
    pk, offs = chunk_streams(ntri, p2, s2, c2, nv, idx)
    return pk, offs, ntri | nv << 16


POS_MAX = 32000  # s16 position range used before snapping (leaves room for split_batch / snapping)
GRASS_D0 = float(os.environ.get("GRASS_D0", 10.0))
# STREAM: ground foliage tufts ranked above the density map.c draws at GRASS_NEAR_D (map.c NEAR_DRAW_R - NEAR_HYST, where
# a cell's near blob is drawn at the latest) go into their cell's near blob, the rest (and canopy, rank 0) stay resident
GRASS_NEAR_D = float(os.environ.get("GRASS_NEAR_D", 22.0))  # metres: full exported foliage density up to here, then (D0 / d)^2 (map.c)
CAM_REACH = 6.0  # metres: how far the camera gets from the walkable navmesh (4 m arm, pivot 1.4 m up, falls)
GRASS_GRID = 2.0  # metres: foliage cell (by tuft centre); map.c picks the density per chunk, so cells stay small
CHUNK_GRASS = 16  # foliage chunk: flags bits 8-15 triangles per tuft, 16-23 / 24-31 lowest / highest rank x 255


def rss(stage):
    """Peak / current RSS at an export stage (the 24 GB Undead Burg run, 2026-10-31: find what grows)"""
    if sys.platform == "emscripten":  # in-browser converter: the wasm heap (it never shrinks, 4 GB at most)
        import js
        if hasattr(js, "heapMB"):
            print(f"mem {stage}: wasm heap {js.heapMB():.0f} MB", flush=True)
            return
    st = dict(l.split(":", 1) for l in open("/proc/self/status") if l.startswith(("VmRSS", "VmHWM")))
    print(f"mem {stage}: rss {int(st['VmRSS'].split()[0]) >> 10} MB, peak {int(st['VmHWM'].split()[0]) >> 10} MB",
          flush=True)


# near texture level (texmap.add_near, texcache level 2): twice mip 0's size for the textures named in TEX_NEAR
# (comma list, "all" = every opaque texture), drawn on near chunks; TECH_DEBT map-2 / texstream-5
TEX_NEAR = set(os.environ.get("TEX_NEAR", "").split(",")) - {""}


def near_level(t, dds, name):
    """Add the near level to texture record t from its DDS when TEX_NEAR asks for it."""
    if "all" in TEX_NEAR or name in TEX_NEAR:
        dec = souls.dds_decode_rgba(dds, 2 * TEX_MAX)
        if dec and dec[0] > t["w"]:  # only when the source has more detail than mip 0
            texmap.add_near(t, *dec)


def split_batch(ks, cents):
    """Triangles of one (texture, flags, cell) group -> ceil(n / CHUNK_TRIS) batches of nearly equal size, by recursive
    median cuts along the longest axis of the centroids, so every batch is full and its box compact (F2).
    ks: triangle numbers (int array), cents: centroid per triangle number (array (n, 3))."""
    k = -(-len(ks) // CHUNK_TRIS)
    if k <= 1:
        return [ks]
    c = cents[ks]
    ext = c.max(0) - c.min(0)
    ax = int(np.argmax(ext))  # first longest axis
    ks = ks[np.argsort(c[:, ax], kind="stable")]
    k1 = k // 2
    cut = max(len(ks) - (k - k1) * CHUNK_TRIS, min(k1 * CHUNK_TRIS, round(len(ks) * k1 / k)))
    return split_batch(ks[:cut], cents) + split_batch(ks[cut:], cents)


def pos_scale(lo, hi, org):
    """Smallest power-of-two step (<= 1 m) that puts every offset from the chunk origin in s16 range."""
    m = max(max(abs(hi[a] - org[a]), abs(lo[a] - org[a])) for a in range(3))
    e = -16
    while m / 2.0 ** e > POS_MAX and e < 0:
        e += 1
    return e


CHUNK_TAG = []  # build_chunks: (level, cell) per chunk (STREAM), else None
CHUNK_DG = []  # build_chunks: draw-group set index per chunk (tools/drawgroups.py, dg.bin)
CHUNK_PART = []  # build_chunks_c (PVS): (MSB part name, occluder) per chunk
CHUNK_SRC = []  # build_chunks_c (PVS): per chunk the indices of its triangles in PVS_TRIS
PVS_TRIS = [None]  # build_chunks_c (PVS): float32 (triangles, 3, 3) world positions of every input triangle
PVS_ORDER = []  # STREAM + PVS: per chunk in map.bin / dg.bin order (index in build order, (cell key, 0 resident / 1 near) or None)
CHUNK_ENT = []  # build_chunks: MSB entity id per chunk (0 = none): the part an event switches on / off (dg.bin DGM3)
# TECH_DEBT event-5: pieces / objects whose entity id the map's events switch ((De)activate Object, Activate Map Part,
# tools/emevd.switched_entities) never share a chunk with anything else, so src/drawgrp.c can hide exactly them
EVENT_PARTS = os.environ.get("EVENT_PARTS", "1") == "1"
DG = drawgroups.Table()  # the area's piece draw-group sets (gather: one per mesh)


_py_sum3 = dsr_light.sum3  # Python's sum() of three floats, per element


def _first_ext(v, X, cs):
    """min / max per batch from ufunc.reduceat (v) with the sign of a zero result taken from the first zero in the
    batch, as Python's min() / max() keep the first of equal values"""
    z = np.nonzero(v == 0)
    if len(z[0]):
        v = v.copy()
        for b, a in zip(*(i.tolist() for i in z)):
            s = cs[b]
            e = cs[b + 1] if b + 1 < len(cs) else len(X)
            col = X[s:e, a]
            v[b, a] = col[np.argmax(col == 0)]
    return v


def build_chunks(meshes, verts, tris, uvs, textures, centre, fol, bkey=None, tcell=None, dgk=None, entk=None):
    """Triangles -> VU1 batches: (texture, flags, tri count, pos bytes, st bytes, colour bytes, min, max, origin, pos
    step, st scale), grouped across meshes by (texture, cull flag, CHUNK_GRID cell of the centroid) and cut into full,
    compact batches (split_batch). Foliage (fol[k] = (rank, tuft, tuft size), else None): whole tufts grouped by
    (texture, flags, GRASS_GRID cell of the tuft centre, tuft size), sorted by rank, so a chunk is one density layer of
    its cell and map.c draws a prefix.
    Unindexed quantised streams (R3): positions s16 x3 (V3-16) in steps of 2^e m from an integer-metre chunk origin;
    a vertex shared by several chunks is first snapped to the coarsest of their steps, so every chunk decodes it to the
    same point (no cracks); texture coords u16 x2 (V2-16) in units of 1 / st scale (4096, or 1024 when a chunk has
    coordinates >= 16); colour u8 x4 (V4-8). bkey: {triangle: normal-map key} (export_normal.assign): triangles
    with different keys never share a chunk; export_normal.CHUNK_BUMP gets each chunk's key. tcell (STREAM): per
    triangle (level, cell) or None: such triangles are grouped by that CHUNK_GRID cell (no coarser far cells) and
    level; CHUNK_TAG gets each chunk's (level, cell) or None. dgk / entk: per triangle draw-group set / switched
    entity id (CHUNK_DG / CHUNK_ENT per chunk); triangles with different ones never share a chunk."""
    # vectorised (was 26 s of pure Python per corner at Firelink): every value is computed with the same float
    # operations in the same order as the per-corner code was, so map.bin is unchanged (see _py_sum3, _first_ext)
    nt = len(tris)
    T = np.asarray(tris, np.int64).reshape(-1, 3)
    fvt = np.zeros(nt, np.int64)
    for ti, fv, vc, ft, tc, fl, lo, hi in meshes:
        fvt[ft:ft + tc] = fv
    G = T + fvt[:, None]  # global vertex per corner
    P = np.array([v[0] for v in verts], np.float64).reshape(-1, 3)
    exact = all(type(x) is float for v in verts for x in v[0])
    if exact:
        X = P[G]
        C = _py_sum3(X[:, 0], X[:, 1], X[:, 2]) / 3
    else:  # positions that are not plain floats: Python's own sum (other summation rules)
        C = np.array([[sum(verts[fvt[k] + i][0][a] for i in tris[k]) / 3 for a in range(3)] for k in range(nt)],
                     np.float64).reshape(-1, 3)
    cl = C.tolist()
    # distance-scaled CHUNK_GRID cell of each centroid: floor(log2(max(1, dist / 4 grid))) from the float's exponent;
    # values within 1e-9 of a power of two are redone with math.log2 as before (its rounding decides there)
    D = C - np.asarray(centre, np.float64)
    r = np.sqrt(_py_sum3(D[:, 0] ** 2, D[:, 1] ** 2, D[:, 2] ** 2)) / (4 * CHUNK_GRID)
    mant, ex = np.frexp(r)
    lvl = np.where(r >= 1.0, ex - 1, 0)
    for k in np.nonzero((r >= 1.0) & ((np.abs(mant - 0.5) < 1e-9) | (np.abs(mant - 1.0) < 1e-9)))[0].tolist():
        c = cl[k]
        dist = math.sqrt(sum((c[a] - centre[a]) ** 2 for a in range(3)))
        lvl[k] = max(0, int(math.log2(max(1.0, dist / (4 * CHUNK_GRID)))))
    gq = (CHUNK_GRID * np.ldexp(1.0, lvl)).tolist()
    fq = np.floor(C / np.asarray(gq)[:, None]).astype(np.int64).tolist()
    groups, tufts = {}, {}
    for ti, fv, vc, ft, tc, fl, lo, hi in meshes:
        for k in range(ft, ft + tc):
            c = cl[k]
            if fol[k]:
                t = tufts.setdefault(fol[k][1], [ti, fl, fol[k][0], fol[k][2], [0.0] * 3, [], dgk[k] if dgk else 0,
                                                 entk[k] if entk else 0])
                t[4] = [t[4][a] + c[a] for a in range(3)]
                t[5].append((fv, k))
                continue
            if fl & CHUNK_SKY:  # sky: 8 azimuth sectors x above / below the horizon, so batches stay full
                cell = ("sky", int((math.atan2(c[0] - centre[0], c[2] - centre[2]) + math.pi) / (math.pi / 4)) % 8,
                        c[1] > centre[1])
            elif tcell and tcell[k] is not None:
                cell = ("s", tcell[k][0]) + tuple(tcell[k][1])
            else:  # coarser far away
                cell = (gq[k],) + tuple(fq[k])
            # dgk: pieces with different draw groups never share a chunk (src/drawgrp.c hides chunks per group)
            cell += (dgk[k] if dgk else 0, entk[k] if entk else 0)
            # the coarse level (STREAM level 0) is drawn beyond the relief range: no normal-map split, fuller batches
            bk = bkey.get(k) if bkey and not (tcell and tcell[k] is not None and tcell[k][0] != 1) else None
            groups.setdefault((ti, fl, cell, bk), []).append((fv, k))
    groups = export_normal.merge_small(groups)
    batches = []
    export_normal.CHUNK_BUMP.clear()
    CHUNK_TAG.clear()
    CHUNK_DG.clear()
    CHUNK_ENT.clear()
    for (ti, fl, cell, bk), ks in groups.items():
        for part in split_batch(np.array([k for _, k in ks], np.int64), C):
            part = part.tolist()
            batches.append((ti, fl, part))
            export_normal.CHUNK_BUMP.append((bk, part) if bk else None)
            CHUNK_TAG.append((cell[1], cell[2:-2]) if cell[0] == "s" else None)
            CHUNK_DG.append(cell[-2])
            CHUNK_ENT.append(cell[-1])
    fgroups = {}
    near_r = min(1.0, (GRASS_D0 / GRASS_NEAR_D) ** 2)
    for ti, fl, r, n, c, ks, dg, ent in tufts.values():
        cell = tuple(int(math.floor(c[a] / len(ks) / GRASS_GRID)) for a in range(3)) + (dg, ent)
        # STREAM: (CHUNK_GRID cell of the tuft centre) for tufts only drawn nearer than GRASS_NEAR_D, else resident
        sc = tuple(int(math.floor(c[a] / len(ks) / CHUNK_GRID)) for a in range(3)) if tcell and r >= near_r else None
        fgroups.setdefault((ti, fl, n, cell, sc), []).append((r, ks))
    for (ti, fl, n, cell, sc), ts in fgroups.items():
        ts.sort(key=lambda t: t[0])
        per = max(1, CHUNK_TRIS // n)
        for i in range(0, len(ts), per):
            part = ts[i:i + per]
            lo, hi = int(part[0][0] * 255), min(255, math.ceil(part[-1][0] * 255))
            batches.append((ti, fl | CHUNK_GRASS | min(n, 255) << 8 | lo << 16 | hi << 24,
                            [k for _, ks in part for _, k in ks]))
            export_normal.CHUNK_BUMP.append(None)
            CHUNK_TAG.append((1, sc) if sc else None)
            CHUNK_DG.append(cell[-2])
            CHUNK_ENT.append(cell[-1])
    # every corner of every batch, in batch order: triangle, global vertex, position
    nb = len(batches)
    tb = [len(part) for _, _, part in batches]
    K = np.fromiter((k for _, _, part in batches for k in part), np.int64, sum(tb))
    # np.intp: repeat / reduceat take counts and indices only as the platform's index type (int32 in the in-browser
    # converter's 32-bit numpy, which refuses int64 there; int64 natively, so the same arrays)
    cb = np.repeat(np.arange(nb), np.asarray(tb, np.intp) * 3)  # batch of each corner
    cs = np.concatenate([[0], np.cumsum(tb)[:-1]]).astype(np.intp) * 3  # first corner of each batch
    CG = G[K].reshape(-1)
    PC = P[CG]
    # chunk boxes (the first of equal values wins, as min() / max() did: the sign of a zero), origins and steps
    MN = _first_ext(np.minimum.reduceat(PC, cs, 0), PC, cs)
    MX = _first_ext(np.maximum.reduceat(PC, cs, 0), PC, cs)
    ORG = np.rint((MN + MX) / 2) + 0.0  # float(round(x)): no negative zero
    mnl, mxl, orgl = MN.tolist(), MX.tolist(), ORG.tolist()
    E = np.array([pos_scale(mnl[b], mxl[b], orgl[b]) for b in range(nb)], np.int64)
    # then the step of each position = the coarsest among the chunks using it (positions keyed by value, as before)
    pk = {}
    pid = np.fromiter((pk.setdefault(v[0], len(pk)) for v in verts), np.int64, len(verts))
    cp = pid[CG]
    while True:
        S = np.full(len(pk), -99, np.int64)
        np.maximum.at(S, cp, E[cb])
        sv = np.ldexp(1.0, S[cp])[:, None]
        Q = np.rint(PC / sv) * sv
        m = np.maximum.reduceat(np.abs(Q - ORG[cb]).max(1), cs) if nb else np.zeros(0)
        grow = m / np.ldexp(1.0, E) > 32767  # snapping moved a vertex out of range: coarser step for this chunk
        if not grow.any():
            break
        E[grow] += 1
    sc = np.ldexp(1.0, E)[cb][:, None]
    NQ = np.rint((Q - ORG[cb]) / sc)
    err = float(np.abs(ORG[cb] + NQ * sc - PC).max()) if len(PC) else 0.0
    assert len(NQ) == 0 or (NQ.min() >= -32768 and NQ.max() <= 32767), "chunk position out of s16 range"
    pos16 = NQ.astype("<i2")
    # texture coordinates: 1/4096 units, or 1/1024 when a chunk has coordinates >= 65535 / 4096
    tws = np.array([(textures[ti][0], textures[ti][1]) if ti >= 0 else (1, 1) for ti, _, _ in batches],
                   np.float64).reshape(-1, 2)
    UV = np.array(uvs, np.float64).reshape(-1, 3, 2)[K].reshape(-1, 2) / tws[cb]
    smax = np.maximum.reduceat(UV.max(1), cs) if nb else np.zeros(0)
    sts = np.where(smax < 65535 / 4096, 4096, 1024)
    ST = np.minimum(65535, np.rint(UV * sts[cb][:, None]))
    assert len(ST) == 0 or ST.min() >= 0, "negative texture coordinate"
    st16 = ST.astype("<u2")
    rgb = np.array([v[1] for v in verts], np.int64).reshape(-1, 3)
    al = np.fromiter((v[2] for v in verts), np.int64, len(verts))
    col = np.concatenate([rgb, al[:, None]], 1)[CG]
    assert len(col) == 0 or (col.min() >= 0 and col.max() <= 255), "vertex colour out of byte range"
    col8 = col.astype(np.uint8)
    out = []
    pad = lambda b: b + b"\0" * (-len(b) % 16)
    ce = np.cumsum(np.asarray(tb, np.int64) * 3).tolist()
    for b, (ti, fl, part) in enumerate(batches):
        s, e = cs[b], ce[b]
        out.append((ti, fl, len(part), pad(pos16[s:e].tobytes()), pad(st16[s:e].tobytes()), pad(col8[s:e].tobytes()),
                    mnl[b], mxl[b], orgl[b], 2.0 ** int(E[b]), int(sts[b])))
    print(f"chunks: {len(out)}, {sum(o[2] for o in out) / max(1, len(out)):.1f} tris / chunk, largest position "
          f"error {err * 1000:.2f} mm")
    return out


def flatten(entries, textures):
    """the drawn entries -> build_chunks' inputs: per entry (texture, first vertex, vertices, first triangle, triangles,
    flags, box), vertices (position, rgb, alpha) of its distinct corners (position, wedge) in sorted order, triangles
    in those, texture coordinates per corner (in texels, from the integer below the triangle's smallest), and per
    triangle foliage, stream cell, draw group, entity"""
    meshes, verts, tris, uvs, fol, tcells, dgs, ents = [], [], [], [], [], [], [], []
    for m in entries:
        tl = m["tris"]
        if not tl:
            continue
        tl = list(tl)  # a mapx_native.Tris: its corner triples
        ti = m["ti"]
        used = sorted({c for t in tl for c in t})  # corners (position, wedge)
        remap = {c: k for k, c in enumerate(used)}
        base = len(verts)
        lo, hi = [1e9] * 3, [-1e9] * 3
        wrgb = m["wrgb"].tolist() if isinstance(m["wrgb"], np.ndarray) else m["wrgb"]  # soften_thin_c's array
        for p, a in used:
            verts.append((p, wrgb[a], m["wa"][a]))
            for j in range(3):
                lo[j], hi[j] = min(lo[j], p[j]), max(hi[j], p[j])
        first_tri = len(tris)
        fol += m["fol"] if m["fol"] else [None] * len(tl)
        tcells += m["tcell"] if m["tcell"] else [None] * len(tl)
        dgs += [m["dg"]] * len(tl)
        ents += [m["ent"]] * len(tl)
        for t in tl:
            tris.append(tuple(remap[c] for c in t))
            if ti >= 0:
                tw, th = textures[ti][0], textures[ti][1]
                uv = [m["wuv"][a] for _, a in t]
                du, dv = math.floor(min(u for u, _ in uv)), math.floor(min(v for _, v in uv))
                uvs.append([((u - du) * tw, (v - dv) * th) for u, v in uv])
            else:
                uvs.append([(0, 0)] * 3)
        meshes.append((ti, base, len(used), first_tri, len(tl), m["flags"], lo, hi))
    return meshes, verts, tris, uvs, fol, tcells, dgs, ents


def build_chunks_c(MX, entries, textures, centre, bkey, stream, draw_groups):
    """flatten + build_chunks in C (tools/mapx/chunks.c): the same chunks, CHUNK_BUMP / TAG / DG / ENT; also returns
    the number of drawn entries and triangles; None when a corner position is not a plain float (build_chunks then
    sums them with Python's rules)"""
    import ctypes
    es = [m for m in entries if m["tris"]]
    i32, i64, f64 = np.int32, np.int64, np.float64
    ne = len(es)
    nts = [len(m["tris"]) for m in es]
    toff = np.concatenate([[0], np.cumsum(nts)]).astype(i64) if ne else np.zeros(1, i64)
    nt = int(toff[-1])
    cps, cws = [np.zeros(0)], [np.zeros(0, i32)]
    for m in es:
        tl = m["tris"]
        if isinstance(tl, mapx_native.Tris):  # float64 from simplify_meshes_c
            cps.append(tl.P.reshape(-1))
            cws.append(tl.W.reshape(-1))
            continue
        cp = [x for t in tl for c in t for x in c[0]]
        if any(t is not float for t in set(map(type, cp))):
            return None
        cps.append(np.array(cp, f64))
        cws.append(np.array([c[1] for t in tl for c in t], i32))
    cpos, cw = np.concatenate(cps), np.concatenate(cws).astype(i32)
    wt, woff, wrgb, wa, wuv, w0 = {}, [], [], [], [], 0
    for m in es:  # wedge tables, once per mesh (its entries share them)
        k = (id(m["wrgb"]), id(m["wa"]), id(m["wuv"]))
        if k not in wt:
            wt[k] = w0
            wrgb.append(_np_list(m, "wrgb", m["wrgb"], i64, 3))
            wa.append(_np_list(m, "wa", m["wa"], i64, 1))
            wuv.append(_np_list(m, "wuv", m["wuv"], f64, 2))
            w0 += len(m["wa"])
        woff.append(wt[k])
    woff = np.array(woff + [w0], i64)
    cat = lambda xs, dt, w: np.ascontiguousarray(np.concatenate(xs).astype(dt)) if xs else np.zeros((0, w), dt)
    wrgb, wa, wuv = cat(wrgb, i32, 3), cat(wa, i32, 1), cat(wuv, f64, 2)
    if (wrgb.size and (wrgb.min() < 0 or wrgb.max() > 255)) or (wa.size and (wa.min() < 0 or wa.max() > 255)):
        raise AssertionError("vertex colour out of byte range")
    tex = np.array([(t[0], t[1]) for t in textures] or [(1, 1)], i32)
    tuft, frank, fsize = np.full(nt, -1, i64), np.zeros(nt), np.zeros(nt, i32)
    tlev, tkey = np.full(nt, -1, i32), np.zeros((nt, 3), i64)
    dgv, entv, bkv = np.zeros(nt, i32), np.zeros(nt, i32), np.full(nt, -1, i32)
    tids, dids, eids, bids = {}, {}, {}, {}
    tl_ = toff.tolist()
    for e, m in enumerate(es):  # per triangle values set a mesh entry at a time
        a, b = tl_[e], tl_[e + 1]
        if m["fol"]:
            fs = [(k, f) for k, f in zip(range(a, b), m["fol"]) if f]
            if fs:
                sel = np.array([k for k, _ in fs], np.intp)
                tuft[sel] = [tids.setdefault(f[1], len(tids)) for _, f in fs]
                frank[sel] = [f[0] for _, f in fs]
                fsize[sel] = [f[2] for _, f in fs]
        if stream and m["tcell"]:
            cs = [(k, c) for k, c in zip(range(a, b), m["tcell"]) if c is not None]
            if cs:
                sel = np.array([k for k, _ in cs], np.intp)
                tlev[sel] = [c[0] for _, c in cs]
                tkey[sel] = [c[1] for _, c in cs]
        dgv[a:b] = dids.setdefault((m["dg"] if draw_groups else 0, m.get("piece", "") if PVS else "",
                                    m.get("occ", 0) if PVS else 0), len(dids))
        entv[a:b] = eids.setdefault(m["ent"], len(eids))
    if isinstance(bkey, export_normal.BumpKeys) and bkey.unique():  # the same numbering from the arrays
        sel = bkey.G < nt
        if sel.any():
            ids, first = _first_rows(np.stack([bkey.TI[sel], bkey.LAB[sel]], 1))
            bkv[bkey.G[sel]] = ids
            bids = dict.fromkeys(zip(bkey.TI[sel][first].tolist(), bkey.LAB[sel][first].tolist()))
    elif bkey:
        bs = [(k, bids.setdefault(v, len(bids))) for k, v in bkey.items() if k < nt]
        if bs:
            bkv[np.array([k for k, _ in bs], np.intp)] = [i for _, i in bs]
    ti = np.array([m["ti"] for m in es] or [0], i32)
    fl = np.array([m["flags"] for m in es] or [0], i64)
    ce = np.ascontiguousarray(np.array(centre, f64).reshape(3))
    arrs = [ti, fl, toff, woff, cpos, cw, wrgb, wa, wuv, tex, tuft, frank, fsize, tlev, tkey, dgv, entv, bkv, ce]
    arrs = [np.ascontiguousarray(x) for x in arrs]
    h = MX.mx_chunks_new(ne, *[x.ctypes.data for x in arrs[:15]], arrs[15].ctypes.data, arrs[16].ctypes.data,
                         arrs[17].ctypes.data, 1 if stream else 0, CHUNK_GRID, arrs[18].ctypes.data, GRASS_GRID,
                         min(1.0, (GRASS_D0 / GRASS_NEAR_D) ** 2), CHUNK_TRIS, export_normal.BUMP_MIN_GROUP, POS_MAX,
                         CHUNK_SKY, CHUNK_GRASS)
    if not h:
        raise MemoryError("mx_chunks_new")
    try:
        cnt = np.zeros(4, i64)
        nb = MX.mx_chunks_count(h, cnt.ctypes.data)
        if nb < 0:
            return None  # a value out of range: the Python code asserts
        iv, flv, tag = np.zeros((max(nb, 1), 9), i32), np.zeros(max(nb, 1), i64), np.zeros((max(nb, 1), 4), i64)
        box, offs, bt = np.zeros((max(nb, 1), 9)), np.zeros((nb + 1, 3), i64), np.zeros(max(nt, 1), i64)
        pos, st, col = (np.zeros(max(int(n), 1), np.uint8) for n in cnt[1:])
        err = ctypes.c_double()
        MX.mx_chunks_fetch(h, iv.ctypes.data, flv.ctypes.data, tag.ctypes.data, box.ctypes.data, offs.ctypes.data,
                           bt.ctypes.data, pos.ctypes.data, st.ctypes.data, col.ctypes.data, ctypes.byref(err))
    finally:
        MX.mx_chunks_free(h)
    dgl, entl, bkl = list(dids), list(eids), list(bids)
    pos, st, col = pos.tobytes(), st.tobytes(), col.tobytes()
    out = []
    export_normal.CHUNK_BUMP.clear()
    CHUNK_TAG.clear()
    CHUNK_DG.clear()
    CHUNK_ENT.clear()
    CHUNK_PART.clear()
    CHUNK_SRC.clear()
    PVS_TRIS[0] = cpos.astype(np.float32).reshape(-1, 3, 3) if PVS and len(cpos) == 9 * nt else None
    btl = bt.tolist()
    for b, (v, f, tg, bx, o0, o1) in enumerate(zip(iv.tolist(), flv.tolist(), tag.tolist(), box.tolist(),
                                                    offs[:-1].tolist(), offs[1:].tolist())):
        t_i, ntri, kind, dg, ent, bk, e, sts, t0 = v
        out.append((t_i, f, ntri, pos[o0[0]:o1[0]], st[o0[1]:o1[1]], col[o0[2]:o1[2]], bx[0:3], bx[3:6], bx[6:9],
                    2.0 ** e, sts))
        export_normal.CHUNK_BUMP.append((bkl[bk], btl[t0:t0 + ntri]) if bk >= 0 else None)
        CHUNK_TAG.append((tg[0], tuple(tg[1:])) if kind in (1, 2) else None)
        CHUNK_DG.append(dgl[dg][0])
        CHUNK_ENT.append(entl[ent])
        CHUNK_PART.append(dgl[dg][1:])
        CHUNK_SRC.append(bt[t0:t0 + ntri])
    print(f"chunks: {len(out)}, {sum(o[2] for o in out) / max(1, len(out)):.1f} tris / chunk, largest position "
          f"error {err.value * 1000:.2f} mm")
    return out, ne, nt


def name_is_grass(name):
    # m10_01_grass_ground_* is solid ground (Burg floors, ~1k tris): thinned as tufts it left holes under the player
    # once the draw-group filter hid the stand-in floor beneath (m2250B1 over m2420B1, autotest point 25)
    return bool(name) and ("grass" in name or "fern" in name) and "ground" not in name


CANOPY_H = float(os.environ.get("CANOPY_H", 2.5))  # metres above the walkable ground: foliage up there is canopy / ivy


def thin_foliage_c(L, tl, pos, seed, ivy, world, grid):
    """thin_foliage in C (tools/mapx/foliage.c, libmapx): the same triangles, ranks, tuft names and positions. The
    canopy test is gather's is_canopy on world (the mesh's vertices placed with xf.many, the same values as xf) against
    the navmesh grid (mx_navgrid; None when the area has no navmesh points: always canopy)."""
    import ctypes
    T = np.ascontiguousarray(np.asarray(tl, np.int32).reshape(-1, 3))
    P = np.array(_vert_np(pos), np.float64).reshape(-1, 3)  # a copy: moved positions are written into it
    Wc = np.ascontiguousarray(world, np.float64)
    nt, nv = len(T), len(P)
    moved = np.zeros(nv, np.uint8)
    out_t, root, size = (np.empty(max(1, nt), np.int32) for _ in range(3))
    rank = np.empty(max(1, nt))
    cn = ctypes.c_int()
    n = L.mx_thin_foliage(nt, T.ctypes.data, nv, P.ctypes.data, moved.ctypes.data, Wc.ctypes.data, grid, 1, CANOPY_H,
                          seed.encode(), int(bool(ivy)), GRASS_KEEP, FOL_XZ, FOL_Y, FOL_IVY, out_t.ctypes.data,
                          rank.ctypes.data, root.ctypes.data, size.ctypes.data, ctypes.byref(cn))
    CANOPY_STATS[0] += cn.value
    out = [tl[k] for k in out_t[:n].tolist()]
    fol = [(r, f"{seed}/{c}", z) for r, c, z in zip(rank[:n].tolist(), root[:n].tolist(), size[:n].tolist())]
    pos = list(pos)
    for i in np.nonzero(moved)[0].tolist():
        pos[i] = tuple(P[i].tolist())
    return out, fol, pos


def thin_foliage(tl, pos, seed, ivy, canopy=None):
    """Foliage cards / tufts (triangles connected by shared positions; 2-32 triangles, often back-to-back pairs) ->
    (kept triangles, [(rank, component, size)] per kept triangle, positions). Each component gets a stable random u in
    [0, 1); it is kept if u < 1 / GRASS_KEEP and its rank is u * GRASS_KEEP (uniform in [0, 1)). The PS2 draws the
    components whose rank is below the density for their distance (map.c), so thinning never splits a tuft or a
    two-sided card. Kept tufts are widened to make up for the missing ones (FOL_XZ sideways and FOL_Y up about the
    tuft's base), so the ground stays about as covered as in DSR. Ivy leaves (single quads on walls) are thinned half
    as much (keep 1 in GRASS_KEEP / 2) and scaled FOL_IVY about their centre, as bigger leaves stand out.
    canopy(vertex indices) -> True for tree canopy / wall foliage (above the walkable ground, render-distance): every
    such component is kept, unscaled, with rank 0, so map.c never thins it with distance (a thinned canopy showed two
    leaf clusters on Firelink's big tree)."""
    import hashlib
    key = [(round(p[0] * 1e4), round(p[1] * 1e4), round(p[2] * 1e4)) for p in pos]
    par = list(range(len(tl)))

    def root(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x
    first = {}
    for k, t in enumerate(tl):
        for i in t:
            if key[i] in first:
                par[root(k)] = root(first[key[i]])
            else:
                first[key[i]] = k
    comps = {}
    for k in range(len(tl)):
        comps.setdefault(root(k), []).append(k)
    out, fol, pos = [], [], list(pos)
    keep = max(1, (GRASS_KEEP + 1) // 2) if ivy else GRASS_KEEP
    sxz, sy = (FOL_IVY, FOL_IVY) if ivy else (FOL_XZ, FOL_Y)
    for c, ks in comps.items():
        u = int.from_bytes(hashlib.md5(f"{seed}/{c}".encode()).digest()[:4], "little") / 2 ** 32
        vs = sorted({i for k in ks for i in tl[k]})
        if canopy and canopy(vs):
            CANOPY_STATS[0] += len(ks)
            out.extend(tl[k] for k in ks)
            fol.extend((0.0, f"{seed}/{c}", len(ks)) for _ in ks)
            continue
        if u * keep >= 1:
            continue
        o = [sum(pos[i][a] for i in vs) / len(vs) for a in range(3)]
        if not ivy:
            o[1] = min(pos[i][1] for i in vs)
        for i in vs:
            p = pos[i]
            pos[i] = (o[0] + (p[0] - o[0]) * sxz, o[1] + (p[1] - o[1]) * sy, o[2] + (p[2] - o[2]) * sxz)
        for k in ks:
            out.append(tl[k])
            fol.append((u * keep, f"{seed}/{c}", len(ks)))
    return out, fol, pos


CANOPY_STATS = [0]


def draw_params(game, area):
    """param/DrawParam/<area>_DrawParam.parambnd -> ({row: LightBank fields}, {row: FogBank fields}, {row: ToneMapBank
    fields}) of the area."""
    defs = {}
    for _, n, d in souls.bnd3(souls.read(os.path.join(game, "paramdef", "paramdef.paramdefbnd.dcx"))):
        t, f = souls.paramdef(d)
        defs[t] = f
    banks = {souls.basename(n): d for _, n, d in
             souls.bnd3(souls.read(os.path.join(game, "param", "DrawParam", f"a{area[1:3]}_DrawParam.parambnd.dcx")))}
    light = {r: v for r, (_, v) in souls.param(banks[f"{area}_LightBank.param"], defs["LIGHT_BANK"]).items()}
    fog = {r: v for r, (_, v) in souls.param(banks[f"{area}_FogBank.param"], defs["FOG_BANK"]).items()}
    tone = {r: v for r, (_, v) in souls.param(banks[f"{area}_ToneMapBank.param"], defs["TONE_MAP_BANK"]).items()}
    return light, fog, tone


def part_draw_ids(msb, n=2):
    """MSB1 parts in file order -> [(LightID, FogID, ...)] from each part's entity data: the first n of the s8 fields
    after the entity id (LightID, FogID, ScatterID, LensFlareID, ShadowID, DofID, ToneMapID, ToneCorrectID)."""
    def table(pos):
        _, _, count = struct.unpack_from("<iii", msb, pos)
        offs = struct.unpack_from(f"<{count - 1}i", msb, pos + 12)
        return offs, struct.unpack_from("<i", msb, pos + 12 + 4 * (count - 1))[0]
    _, nxt = table(0)
    for _ in range(2):  # events, regions
        _, nxt = table(nxt)
    out = []
    for o in table(nxt)[0]:
        ent = struct.unpack_from("<i", msb, o + 0x58)[0]
        out.append(struct.unpack_from(f"<{n}b", msb, o + ent + 4))
    return out


def fog_rgb(row, tone):
    """FogBank colour as the PS2 shows it. DSR's map shaders (FRPG_Phn_*.fpo, decompiled from a RenderDoc capture
    at Firelink) take the lit HDR colour to gamma space (pow 1/2.2), lerp it towards gFC_FogCol = pow(col / 255 x
    colA / 100, 2.2) (exe FUN_1401c8b00; measured 0.170 for row 20) by saturate(max x saturate((z - begin) / (end -
    begin))), apply the light scattering (dsr_light.Scatter), go back to linear (pow 2.2) and only then tone map. The
    GS mixes after the vertex-baked tone map, so its fog colour is the constant scattered at half the fog's end
    distance (ASSUMED reference distance, view across the sun), taken back to linear and tone mapped."""
    F = [dsr_light.lin(row[f"col{c}"], row["colA"]) for c in "RGB"]
    if SCATTER[0]:
        F = SCATTER[0].gamma(F, max(0.0, row["fogEndZ"] / 2), 0.0)
    return [round(255 * v) for v in tone([max(0.0, x) ** 2.2 for x in F])]


def area_tone(tone_rows, banks, t, c):
    """The baked map's tone map for (ToneMapID, ToneCorrectID) (auto exposure from gather), else a new one."""
    if (t, c) not in TONES:
        TONES[t, c] = dsr_light.Tone(tone_rows.get(t, tone_rows[min(tone_rows)]), banks["ToneCorrect"].get(c))
    return TONES[t, c]


def adapted(tone, light_row, faces):
    """A copy of tone with its exposure adapted to an area lit by light_row (no baked scene there: the luminance of
    a 0.3 albedo averaged over all normals stands in for DSR's screen average)."""
    f = dsr_light.light_fn(light_row)
    ns = [(math.sqrt(1 - (1 - 2 * (i + 0.5) / 16) ** 2) * math.cos(i * 2.4), 1 - 2 * (i + 0.5) / 16,
           math.sqrt(1 - (1 - 2 * (i + 0.5) / 16) ** 2) * math.sin(i * 2.4)) for i in range(16)]
    e = [f(n, faces) for n in ns]
    lum = sum(dsr_light.LUMA[c] * dsr_light.srgb_lin(0.3) * x[c] for x in e for c in range(3)) / len(e)
    t = dsr_light.Tone.__new__(dsr_light.Tone)
    t.__dict__.update(tone.__dict__)
    t.adapt(lum)
    return t


def char_light(row, faces, tone, sun=None, shadow=False):
    """Skinned characters on the PS2 (vu1skin.vsm): colour = amb + sun x max(n . L, 0) in 0x80 units for texture
    modulation. Fitted to DSR's light (LightBank row, light probe, tone map) for a mid-grey albedo CHAR_ALB: amb =
    the mean output over normals facing away from L, amb + sun = the output facing L. Returns (amb, sun, L)."""
    f0 = dsr_light.light_fn(row)
    k = SUN_FLOOR[0] if shadow else 1.0  # characters stand in sun this often: the bake's shadowing on average
    near = SCATTER[0].near_factor() if SCATTER[0] else [1.0, 1.0, 1.0]  # scattering at d = 0 (dsr_light.Scatter)
    f = lambda n, faces: [(a * (1 - SHADOW_AMB * (1 - k)) + b * k) * m for a, b, m in zip(*f0(n, faces, split=True), near)]
    L = sun or max(((dsr_light.light_dir(row[f"degRotX_{i}"], row[f"degRotY_{i}"]), dsr_light.lin(255, row[f"colA_{i}"]))
                    for i in "012"), key=lambda d: d[1])[0]
    a = dsr_light.srgb_lin(CHAR_ALB)
    out = lambda n: tone([a * e for e in f(n, faces)])
    away = []
    for i in range(32):  # Fibonacci sphere, the half facing away from L
        y = 1 - 2 * (i + 0.5) / 32
        r, ph = math.sqrt(1 - y * y), i * 2.39996
        n = (r * math.cos(ph), y, r * math.sin(ph))
        if n[0] * L[0] + n[1] * L[1] + n[2] * L[2] <= 0:
            away.append(out(n))
    amb = [sum(o[c] for o in away) / len(away) / CHAR_ALB for c in range(3)]
    full = [v / CHAR_ALB for v in out(L)]
    return amb, [max(0.0, full[c] - amb[c]) for c in range(3)], L


def probe_id(event):
    """Collision part env_event (MSB Environment event: 'GIマップスポット29', 'GIEL h1000') -> light probe number
    (GI_EnvDif_<map>_0029 / _1000): the event name's trailing number."""
    import re
    m = re.findall(r"\d+", event or "")
    return int(m[-1]) if m else -1


def fog_section(game, map_id, main_fog):
    """Area draw-param rows + the row of every collision triangle, for map.c's area switch: DSR takes the draw
    params of the collision part under the player (MSB fog / light / tone map / tone correct / point light
    (lantern) ids and its GI probe, env_event). One row per distinct set; row 0 = the most common set with the main
    FogID. Collision triangles are numbered like export_collision.area_collision (collision.bin's set), so
    col_floor's hit index is the index here. Each row: the FogBank row, the character light (char_light: the
    collision's LightID row, probe, tone map) and the player-attached point light (PointLightBank[lantern_id], e.g.
    Firelink 20 'B2拠点', Burg 10/11/12 player outdoors / indoors / shade), which lights characters by their
    distance to the player. Returns (section bytes, [(row key, fog fields, char light)], tone map per row key).
    Section: u32 rows, collision tris, 0, 0; rows (48 B) {f32 begin, end, max density; u8 fog r, g, b, 0;
    u8 ambient r, g, b, 0; u8 sun r, g, b, 0; s8 sun direction x, y, z (x 127), u8 point light begin (0.1 m);
    u8 point light r, g, b (added to the ambient at full strength), u8 point light end (0.1 m); 48 B: u8 env
    reflection up r, g, b, down r, g, b, specular light r, g, b (0x80 = 1.0); s8 specular light direction x, y, z
    (x 127); u8 far depth of field begin, end (m), strength (%), 0}; u8 row per triangle. The DoF bytes: DofBank
    row of the collision part's DofID (farDofBegin / farDofEnd / farDofMul; zero: none), for main.c's far blur."""
    import export_collision
    msb = souls.read(f"{game}/map/MapStudio/{map_id}.msb")
    M = souls.msb1(msb)
    light_rows, fog_rows, tone_rows = draw_params(game, map_id[:3])
    banks = dsr_light.draw_banks(game, map_id[:3])
    probes = dsr_light.load_probes(game, map_id)
    spec_probes = dsr_light.load_spec_probes(game, map_id)
    coll = {p["name"]: p for p in M["parts"] if p["type"] == 5}
    key_of = lambda p: (p["fog_id"] if p["fog_id"] in fog_rows else main_fog, p["light_id"], p["tone_map_id"],
                        p["tone_correct_id"], p["lantern_id"], probe_id(p.get("env_event")), p["dof_id"])
    subs = export_collision.area_collision(game, map_id)[0]  # collision.bin's triangles (area region, tools/area.py)
    # every corner quantised as collision.bin does (round((p - origin) / SCALE); numpy's rint rounds half to even
    # like round()), the triangles that collapse there skipped
    P = np.array([p for s in subs for t in s["tris"] for p in t], np.float64).reshape(-1, 3, 3)
    lo, hi = P.reshape(-1, 3).min(0).tolist(), P.reshape(-1, 3).max(0).tolist()
    origin = [round((lo[k] + hi[k]) / 2) for k in range(3)]
    Q = np.rint((P - np.array(origin, np.float64)) / export_collision.SCALE)
    ok = (Q[:, 0] != Q[:, 1]).any(1) & (Q[:, 1] != Q[:, 2]).any(1) & (Q[:, 0] != Q[:, 2]).any(1)
    nok = np.concatenate([[0], np.cumsum(ok)]).tolist()
    tri_key, count, i = [], {}, 0
    for s in subs:
        k = key_of(coll[s["part"]]) if s["part"] in coll else None
        n = nok[i + len(s["tris"])] - nok[i]
        i += len(s["tris"])
        if n:
            tri_key += [k] * n
            count[k] = count.get(k, 0) + n
    keys = [k for k in count if k is not None]
    if not keys:
        keys = [(main_fog, min(light_rows), min(tone_rows), min(tone_rows), -1, -1, -1)]
    main = max(keys, key=lambda k: (k[0] == main_fog, count.get(k, 0)))
    order = [main] + sorted((k for k in keys if k != main), key=lambda k: -count.get(k, 0))
    assert len(order) <= 255, "too many draw-param sets"
    tone, chars, pls, specs = {}, {}, {}, {}
    a = dsr_light.srgb_lin(CHAR_ALB)
    for k in order:
        f, l, t, c, lant, probe, _ = k
        row = light_rows.get(l, light_rows[min(light_rows)])
        tone[k] = area_tone(tone_rows, banks, t, c)
        if k != main:  # areas the map bake did not adapt to (cell, elevator): their own exposure
            tone[k] = adapted(tone[k], row, probes.get(probe))
        chars[k] = char_light(row, probes.get(probe), tone[k], shadow=k == main)
        pl = banks["PointLight"].get(lant)
        if pl and pl["colA"] > 0:  # the point light's share on top of the area light, for albedo CHAR_ALB, at full strength
            e0 = dsr_light.light_fn(row)((0, 1, 0), probes.get(probe))
            add = [dsr_light.lin(pl[f"col{ch}"], pl["colA"]) for ch in "RGB"]
            t0 = tone[k]
            kk = [(o1 - o0) / CHAR_ALB for o1, o0 in zip(t0([a * (e + x) for e, x in zip(e0, add)]), t0([a * e for e in e0]))]
            pls[k] = (min(255, round(10 * pl["dwindleBegin"])), [min(255, round(0x80 * x)) for x in kk],
                      min(255, round(10 * pl["dwindleEnd"])))
        else:
            pls[k] = (0, [0, 0, 0], 0)
        # specular for vu1skin (TECH_DEBT map-29): env reflection up / down = envSpc x the probe's GI_EnvSpc cube
        # (rough mip), the LightBank specular light *_s and its direction; tone mapped for albedo CHAR_ALB, 0x80 units
        env = [dsr_light.lin(row[f"envSpc_col{ch}"], row["envSpc_colA"]) for ch in "RGB"]
        up, down = dsr_light.env_hemispheres(spec_probes[probe]) if probe in spec_probes else ([0] * 3, [0] * 3)
        u8 = lambda hdr: [min(255, round(0x80 * o / CHAR_ALB)) for o in tone[k]([a * x for x in hdr])]
        specs[k] = (u8([e * x for e, x in zip(env, up)]), u8([e * x for e, x in zip(env, down)]),
                    u8([dsr_light.lin(row[f"col{ch}_s"], row["colA_s"]) for ch in "RGB"]),
                    [max(-127, min(127, round(127 * x))) for x in dsr_light.light_dir(row["degRotX_s"], row["degRotY_s"])])
    idx = {k: i for i, k in enumerate(order)}
    tri_row = bytes(idx.get(k, 0) for k in tri_key)
    out = struct.pack("<4I", len(order), len(tri_row), 0, 0)
    for k in order:
        v = fog_rows[k[0]]
        amb, sun, L = chars[k]
        pb, prgb, pe = pls[k]
        eu, ed, si, ls = specs[k]
        dof = banks["Dof"].get(k[6])
        dof = [min(255, max(0, round(x))) for x in (dof["farDofBegin"], dof["farDofEnd"], dof["farDofMul"])] if dof \
            else [0, 0, 0]
        out += struct.pack("<3f4B4B4B3bB4B3B3B3B3b3Bx", v["fogBeginZ"], v["fogEndZ"], v["degRotW"] / 100.0, *fog_rgb(v, tone[k]), 0,
                           *[min(255, round(0x80 * x)) for x in amb], 0, *[min(255, round(0x80 * x)) for x in sun], 0,
                           *[max(-127, min(127, round(127 * x))) for x in L], pb, *prgb, pe, *eu, *ed, *si, *ls, *dof)
    return out + tri_row, [(k, fog_rows[k[0]], chars[k] + (pls[k], specs[k])) for k in order], tone


def _qrot_np(q, V):
    """havok_anim.qrot / souls._qrot on (n, 3) rows, the same operations in the same order"""
    x, y, z, w = q
    vx, vy, vz = V[:, 0], V[:, 1], V[:, 2]
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    return np.stack([vx + w * tx + y * tz - z * ty, vy + w * ty + z * tx - x * tz, vz + w * tz + x * ty - y * tx], 1)


_VERT_NP = {}  # id(vertex attribute list) -> (the list, float64 array): FLVER meshes are placed many times


def _vert_np(lst):
    if isinstance(lst, souls.Rows):  # gather's FLVER attributes: the array itself
        return lst.a if lst.a.dtype == np.float64 else lst.a.astype(np.float64)
    e = _VERT_NP.get(id(lst))
    if e is None or e[0] is not lst or len(e[1]) != len(lst):  # (souls.flver fills it; the lists only grow)
        e = _VERT_NP[id(lst)] = (lst, np.array(lst, np.float64))
    return e[1]


def _first_rows(K):
    """rows of an int array -> (id per row numbered by first appearance, row index of each id's first appearance)"""
    MX = mapx_native.lib()
    if MX is not None and K.ndim == 2:  # a hash instead of np.unique's sort (tools/mapx/rows.c)
        Kc = np.ascontiguousarray(K, np.int64)
        ids, first = np.empty(len(Kc), np.int64), np.empty(len(Kc), np.int64)
        u = MX.mx_first_rows(len(Kc), Kc.shape[1], Kc.ctypes.data, ids.ctypes.data, first.ctypes.data)
        return ids.astype(np.intp), first[:u].astype(np.intp)
    _, first, inv = np.unique(K, axis=0, return_index=True, return_inverse=True)
    inv = inv.reshape(-1)
    order = np.argsort(first, kind="stable")
    rank = np.empty(len(order), np.intp)  # intp: np.bincount takes no int64 under 32-bit numpy (the browser)
    rank[order] = np.arange(len(order))
    return rank[inv], first[order]


def _weld(V, vis, xf, xn):
    """gather's weld of one mesh's used vertices vis (ascending): positions by world position (0.1 mm), wedges by
    (position, UV, colour) with the normals of merged wedges summed. -> (pos [tuples], wpos, wuv, wcol, wn [lists],
    fvw {vertex: wedge}, npd {name: (list, its array)} for pos, wpos, wuv, wcol (wn when normals are summed): what
    np.array gives from those lists, for _np_list), each in order of first appearance. Vectorised (was ~1 M Python iterations per Firelink
    export); every value is the per-vertex code's: Python's round() and numpy's rint both round half to even, normals
    are summed in vertex order (np.add.at), the first normal of a wedge is kept as is."""
    idx = np.asarray(vis, np.int64)
    n = len(idx)
    if not n:
        return [], [], [], [], [], {}, {}
    Pw = xf.many(_vert_np(V["pos"])[idx])
    MX = mapx_native.lib()
    WN = None
    if MX is not None:  # both first-appearance numberings and the normal sums in C (tools/mapx/dedup.c)
        Pw = np.ascontiguousarray(Pw, np.float64)
        UV = np.ascontiguousarray(_vert_np(V["uv"])[idx]) if V["uv"] else None
        C = np.ascontiguousarray(_vert_np(V["color"])[idx]) if V["color"] else None
        N = np.ascontiguousarray(xn.many(_vert_np(V["normal"])[idx]), np.float64) if V["normal"] else None
        pid, pfirst, wid, wfirst = (np.empty(n, np.int64) for _ in range(4))
        WN, cnt = np.empty((n, 3)) if N is not None else None, np.zeros(2, np.int64)
        p_ = lambda a: None if a is None else a.ctypes.data
        MX.mx_weld(n, Pw.ctypes.data, p_(UV), p_(C), p_(N), pid.ctypes.data, pfirst.ctypes.data, wid.ctypes.data,
                   wfirst.ctypes.data, p_(WN), cnt.ctypes.data)
        pid, wid = pid.astype(np.intp), wid.astype(np.intp)
        pfirst, wfirst = pfirst[:cnt[0]].astype(np.intp), wfirst[:cnt[1]].astype(np.intp)
        if WN is not None:
            WN = WN[:cnt[1]]
        UV = UV if UV is not None else np.zeros((n, 2))
        C = C if C is not None else np.ones((n, 4))
    else:
        pid, pfirst = _first_rows(np.rint(Pw * 1e4).astype(np.int64))
        UV = _vert_np(V["uv"])[idx] if V["uv"] else np.zeros((n, 2))
        C = _vert_np(V["color"])[idx] if V["color"] else np.ones((n, 4))
        wk = np.concatenate([pid[:, None], np.rint(UV * 4096).astype(np.int64), np.rint(C * 255).astype(np.int64)], 1)
        wid, wfirst = _first_rows(wk)
    Pp = np.ascontiguousarray(Pw[pfirst], np.float64)
    pos = souls.Rows(Pp)  # reads as the list of position tuples
    vl = idx.tolist()
    Wp = pid[wfirst]
    wpos = Wp.tolist()
    wfl = wfirst.tolist()
    if isinstance(V["uv"], souls.Rows):  # the same tuples, read from the array
        wuv = souls.Rows(np.ascontiguousarray(V["uv"].a[idx[wfirst]], np.float64))
    else:
        wuv = [V["uv"][vl[i]] for i in wfl] if V["uv"] else [(0.0, 0.0)] * len(wfirst)
    if isinstance(V["color"], souls.Rows):
        wcol = souls.Rows(np.ascontiguousarray(V["color"].a[idx[wfirst]], np.float64))
    else:
        wcol = [V["color"][vl[i]] for i in wfl] if V["color"] else [(1, 1, 1, 1)] * len(wfirst)
    npd = dict(pos=(pos, Pp), wpos=(wpos, Wp.astype(np.int32)), wuv=(wuv, np.ascontiguousarray(UV[wfirst])),
               wcol=(wcol, np.ascontiguousarray(C[wfirst])))
    if V["normal"] and WN is not None:
        wn = WN.tolist()
        npd["wn"] = (wn, WN)
    elif V["normal"]:
        N = xn.many(_vert_np(V["normal"])[idx])
        WN = N[wfirst].copy()
        rest = np.ones(n, bool)
        rest[wfirst] = False
        np.add.at(WN, wid[rest], N[rest])
        wn = WN.tolist()
        npd["wn"] = (wn, WN)
    else:  # (0, 1, 0) per vertex, summed as Python ints
        wn = [[0, int(c), 0] for c in np.bincount(wid, minlength=len(wfirst)).tolist()]
    return pos, wpos, wuv, wcol, wn, VertWedge(idx, wid), npd


class VertWedge(collections.abc.Mapping):
    """_weld's {vertex: wedge} as two arrays (vs ascending, ws): reads as that dict"""
    __slots__ = ("vs", "ws", "_d")

    def __init__(self, vs, ws):
        self.vs, self.ws, self._d = vs, ws, None

    def _dict(self):
        if self._d is None:
            self._d = dict(zip(self.vs.tolist(), self.ws.tolist()))
        return self._d

    def __getitem__(self, k):
        return self._dict()[k]

    def __iter__(self):
        return iter(self.vs.tolist())

    def __len__(self):
        return len(self.vs)

    def __repr__(self):
        return repr(self._dict())


def _np_list(m, key, lst, dt, w):
    """lst (a list of mesh m, or an array) as a C-contiguous (n, w) array of dtype dt (w 1: (n,)): the array gather
    kept for that very list in m["_np"][key] (see _weld), else np.array of it"""
    c = m.get("_np", {}).get(key)
    a = c[1] if c is not None and c[0] is lst else lst if len(lst) else np.zeros((0, w), dt)
    a = np.asarray(a, dt)
    return np.ascontiguousarray(a.reshape(-1) if w == 1 else a.reshape(-1, w))


def _mesh_np(m):
    """mesh m's positions (n, 3) float64 and wedge -> position (int32) as arrays (np.array of its pos / wpos lists)"""
    return _np_list(m, "pos", m["pos"], np.float64, 3), _np_list(m, "wpos", m["wpos"], np.int32, 1)


def _wedge_light(lit, wn, pos, wpos, probes, probe_at, plights, npd={}):
    """gather's per-wedge light (dsr_light.light_fn split=True + the static point lights) for one lit mesh ->
    dict(E (hemisphere + probe + point lights), S (directional), P (positions), N (unit normals)), (n, 3) arrays,
    each element computed as the per-wedge code did (normal length by Python's sum(), probe lookups in wedge order:
    ProbeIndex caches per cell)."""
    md = {"_np": npd}
    MX = mapx_native.lib()
    if MX is not None and hasattr(lit, "params") and all(hasattr(f, "params") for f in plights) and len(wpos):
        R = _wedge_light_c(MX, lit, md, wn, pos, wpos, probes, probe_at, plights)
        if R is not None:
            return R
    N = _np_list(md, "wn", wn, np.float64, 3)
    ln = np.sqrt(_py_sum3(N[:, 0] * N[:, 0], N[:, 1] * N[:, 1], N[:, 2] * N[:, 2]))
    ln[ln == 0] = 1.0
    N = N / ln[:, None]
    P = _np_list(md, "pos", pos, np.float64, 3)[_np_list(md, "wpos", wpos, np.intp, 1)] if len(wpos) else np.zeros((0, 3))
    faces = [probes.get(pr) for pr in probe_at.many(P)]
    E, S = lit.many(N, faces)
    for f in plights:
        E = E + f.many(P, N)
    return dict(E=E, S=S, P=P, N=N)


def _wedge_light_c(MX, lit, md, wn, pos, wpos, probes, probe_at, plights):
    """_wedge_light in C (tools/mapx/wlight.c); None when the probe index has no C handle or a probe cube is not
    (6, R, R, 3)"""
    Wn = _np_list(md, "wn", wn, np.float64, 3)
    Pa, Wa = _np_list(md, "pos", pos, np.float64, 3), _np_list(md, "wpos", wpos, np.int64, 1)
    slot = probe_at.slots(Pa[Wa])  # the probe queries in wedge order, as many() (its cache keeps the first answer)
    if slot is None:
        return None
    tab = getattr(probe_at, "_mx_faces", None)
    if tab is None:
        R = dsr_light.PROBE_RES
        F = [np.asarray(probes[v], np.float64) for v in probe_at.vals]
        if any(f.shape != (6, R, R, 3) for f in F):
            return None
        tab = probe_at._mx_faces = np.ascontiguousarray(np.stack(F) if F else np.zeros((1, 6, R, R, 3)))
    dirs, up, down, env = lit.params
    lp = np.zeros(28)
    lp[0] = len(dirs)
    for j, (d, col) in enumerate(dirs):
        lp[1 + 3 * j:4 + 3 * j], lp[10 + 3 * j:13 + 3 * j] = d, col
    lp[19:22], lp[22:25], lp[25:28] = up, down, env
    pl = np.ascontiguousarray([[*f.params[0], f.params[1], f.params[2], *f.params[3]] for f in plights] or
                              np.zeros((0, 8)), np.float64)
    n = len(Wa)
    E, S, P, N = (np.empty((n, 3)) for _ in range(4))
    sl = np.ascontiguousarray(slot, np.int32)
    MX.mx_wedge_light(n, Wn.ctypes.data, Pa.ctypes.data, Wa.ctypes.data, sl.ctypes.data, lp.ctypes.data,
                      tab.ctypes.data, dsr_light.PROBE_RES, len(pl), pl.ctypes.data, E.ctypes.data, S.ctypes.data,
                      P.ctypes.data, N.ctypes.data)
    return dict(E=E, S=S, P=P, N=N)


def bind_scaled(F):
    """World (t, q, s) of every FLVER bone in bind pose, with the bones' scale (souls.flver_bind_world drops it).
    Objects carry scaled root bones: the bonfire o0200's ash / bones / sticks hang off 1.2x bones and its sword off a
    (0.91, 1.2, 0.91) one; without the scale its sticks and bones sat 17 % small and 0.3 m off the sword."""
    out = []
    for name, parent, t, q, sc in F["bones"]:
        if parent < 0:
            out.append((t, q, sc))
        else:
            pt, pq, ps = out[parent]
            r = souls._qrot(pq, tuple(a * b for a, b in zip(ps, t)))
            out.append((tuple(a + b for a, b in zip(pt, r)), souls._qmul(pq, q), tuple(a * b for a, b in zip(ps, sc))))
    return out


def bone_xf(pxf, pxn, bt, bq, bs):
    """Bone-relative vertex / normal -> world (scale, rotate, translate, then the part placement)."""
    def xf(p):
        return pxf(tuple(c + d for c, d in zip(souls._qrot(bq, tuple(a * b for a, b in zip(bs, p))), bt)))

    def xn(n):
        v = souls._qrot(bq, tuple(a / b for a, b in zip(n, bs)))
        k = sum(c * c for c in v) ** 0.5 or 1.0
        return pxn(tuple(c / k for c in v))

    def xn_many(N):
        v = _qrot_np(bq, N / np.asarray(bs, np.float64))
        k = np.array([x ** 0.5 or 1.0 for x in _py_sum3(v[:, 0] * v[:, 0], v[:, 1] * v[:, 1], v[:, 2] * v[:, 2]).tolist()])
        return pxn.many(v / k[:, None])
    xf.many = lambda P: pxf.many(_qrot_np(bq, np.asarray(bs, np.float64) * P) + np.asarray(bt, np.float64))
    xn.many = xn_many
    return xf, xn


def xf_part(part, F, m):
    """Mesh vertex -> world: placement x the mesh's default bone (non-skinned FLVER meshes are bone-relative)."""
    pxf, pxn = place(part)
    bind = bind_scaled(F)
    if not m["dynamic"] and 0 <= m["default_bone"] < len(bind):
        return bone_xf(pxf, pxn, *bind[m["default_bone"]])[0]
    return pxf


def mtd_params(game):
    """mtd/Mtd.mtdbnd -> {material file name: {g_ parameter: tuple of values}}. Each parameter: its name, its type
    name (int / float1-4 / bool), a value marker byte (0xC0 bool, 0xC5 int, 0xCA float), padding to 4, a u32 count
    and the values."""
    import re
    out = {}
    for _, n, d in souls.bnd3(souls.read(os.path.join(game, "mtd", "Mtd.mtdbnd.dcx"))):
        ps = {}
        for m in re.finditer(rb"(g_[A-Za-z0-9_]+)", d):
            tm = re.compile(rb"(int|float[234]?|bool)").search(d, m.end(), m.end() + 20)
            mk = tm and re.compile(rb"[\xc0\xc5\xca]").search(d, tm.end() + 4, tm.end() + 40)
            if not mk:
                continue
            p = (mk.end() + 3) & ~3
            t = tm.group().decode()
            fmt = "f" if t.startswith("float") else "i" if t == "int" else "B"
            ps[m.group(1).decode()] = struct.unpack_from(f"<{struct.unpack_from('<I', d, p)[0]}{fmt}", d, p + 4)
        out[souls.basename(n)] = ps
    return out


def water_rgba(mp):
    """[We] water materials have no diffuse map: DSR's water shader mixes the refraction (tinted by g_WaterColor,
    alpha = its opacity) with a Fresnel reflection (g_FresnelColor; F = bias + scale x (1 - cos)^pow). Stand-in: one
    RGBA colour at a 60 degree view angle, linear -> sRGB. Firelink pool (A10_Water_lake): pale green, ~40 % opaque;
    aqueduct (A10_Watercourse): dark, ~90 % opaque."""
    wc = mp.get("g_WaterColor", (0.1, 0.1, 0.1, 0.5))
    fc = mp.get("g_FresnelColor", (1.0, 1.0, 1.0))
    f = min(1.0, mp.get("g_FresnelBias", (0.1,))[0] + mp.get("g_FresnelScale", (1.0,))[0] *
            0.5 ** mp.get("g_FresnelPow", (2.0,))[0])
    rgb = [wc[i] * (1 - f) + fc[i] * f for i in range(3)]
    a = wc[3] + (1 - wc[3]) * f
    return bytes([round(255 * min(1.0, x) ** (1 / 2.2)) for x in rgb] + [round(255 * a)])


def is_effect(mtd):
    """Translucent effect materials (light shafts '_Add', mist/cloud '[Dn]_Alp'): drawn opaque they become solid slabs.
    Skipped until the renderer has alpha blending."""
    m = mtd.lower()
    return "_add" in m or ("[dn]" in m and "_alp" in m)


def is_sky(mtd, pts, nav_tree):
    """Sky / background layer: a sky or cloud material (A10_Sky[Dn]_LS, A10_02_m9000_Sky, A10_cloud[DB]_Alp, ...), or
    a mesh entirely farther than SKY_DIST from the area's walkable navmesh (Firelink's m9000 ground ring, Undead
    Burg's m8000B1). Was: > SKY_DIST from the spawn, which only works when the spawn is central (TECH_DEBT build-6)."""
    low = mtd.lower()
    if "sky" in low or "cloud" in low:
        return True
    if nav_tree is None or not len(pts):
        return False
    P = np.array(pts)
    lo, hi = P.min(0), P.max(0)
    gap = np.maximum(0, np.maximum(lo - nav_tree, nav_tree - hi))  # mesh box to each walkable point
    if np.sqrt((gap ** 2).sum(1)).min() > SKY_DIST:
        return True
    MX = mapx_native.lib()
    if MX is not None:  # the same squared distances, stopping at the first one in range (tools/mapx/rows.c)
        Pc, Sc = np.ascontiguousarray(P, np.float64), np.ascontiguousarray(nav_tree, np.float64)
        return not MX.mx_any_within(len(Pc), Pc.ctypes.data, len(Sc), Sc.ctypes.data, SKY_DIST ** 2)
    for i in range(0, len(P), 2048):
        d = ((P[i:i + 2048, None, :] - nav_tree[None]) ** 2).sum(2).min()
        if d <= SKY_DIST ** 2:
            return False
    return True


def navmesh_points(game, map_id, M, keep):
    """World positions of the area's navmesh vertices inside the region (MSB navmesh parts, type 8)."""
    mdir = os.path.join(game, "map")
    models = {souls.basename(n)[:-4]: d for _, n, d in souls.bnd3(souls.read(f"{mdir}/{map_id}/{map_id}.nvmbnd.dcx"))}
    out = []
    for part in M["parts"]:
        key = f"{part['model']}A{map_id[1:3]}" if part["type"] == 8 else None
        if key in models:
            xf, _ = place(part)
            out += [q for q in (xf(p) for p in souls.nvm(models[key])[0]) if keep(*q)]
    return out


def load_object(game, model, dds, static=False):
    """obj/<model>.objbnd: the FLVER named after the model (not the _1 broken/alternate state); its TPF textures are
    added to dds. None if the archive or FLVER is missing, or the object is breakable (<model>_1.hkx in the objbnd)
    and not drawn static (STATIC_BREAKABLES, or static=True: placed beyond the area's collision)."""
    path = os.path.join(game, "obj", model + ".objbnd.dcx")
    if not os.path.exists(path):
        return None
    files = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(path))}
    if model + "_1.hkx" in files and model not in STATIC_BREAKABLES and not static:
        return None  # breakable (has a broken state): tools/export_objects.py exports it, src/objects.c draws it
    if model + ".tpf" in files:
        dds.update(souls.tpf(files[model + ".tpf"]))
    return souls.flver(files[model + ".flver"]) if model + ".flver" in files else None


_DEDUP = {}  # gather: (id(face set), id(positions)) -> (face set, positions, dedup_tris result)


def faceset_tris(fs):
    """souls.triangles(fs) as an (n, 3) int64 array (libmapx mx_faceset_tris; without it from souls.triangles)"""
    MX = mapx_native.lib()
    if MX is None:
        return np.array(souls.triangles(fs), np.int64).reshape(-1, 3)
    idx = np.ascontiguousarray(fs[2], np.int64)
    out = np.empty((max(len(idx) - 2, 1), 3), np.int64)
    n = MX.mx_faceset_tris(len(idx), idx.ctypes.data, 1 if fs[1] else 0, out.ctypes.data)
    return out[:n]


def dedup_tris(tl, V):
    """Drop repeated triangles inside one mesh: same positions (0.1 mm), winding and UVs as an earlier one. Some FLVER
    face sets store faces two or three times (Burg barrel o1152: 1488 tris = 496 x 3, map piece m2011B1: 4.6k of 9.5k);
    the copies draw the same pixels, and their non-manifold edges stop the QEM simplifier (both kept 97-100 %).
    tl: index triples (a list, or faceset_tris' array: the kept ones come back as tuples of ints either way)."""
    if not len(tl):
        return []
    MX = mapx_native.lib()
    if MX is not None:  # the same keys, rotation and first-appearance test in C (tools/mapx/dedup.c)
        T = np.ascontiguousarray(np.asarray(tl, np.int64).reshape(-1, 3))
        P = np.ascontiguousarray(_vert_np(V["pos"]), np.float64)
        U = np.ascontiguousarray(_vert_np(V["uv"]), np.float64) if V["uv"] else None
        keep = np.empty(len(T), np.int64)
        nk = MX.mx_dedup_tris(len(P), P.ctypes.data, None if U is None else U.ctypes.data, len(T), T.ctypes.data,
                              keep.ctypes.data)
        keep = keep[:nk]
        if isinstance(tl, np.ndarray):
            return list(map(tuple, tl[keep].tolist()))
        return [tl[i] for i in keep.tolist()]
    # per corner (x, y, z, u, v) rounded, rotated to start at the lexicographically smallest corner (the first of
    # equal ones, as c.index(min(c))); the first triangle of each key is kept, in order
    K = np.rint(_vert_np(V["pos"]) * 1e4).astype(np.int64)
    K = np.concatenate([K, np.rint(_vert_np(V["uv"]) * 4096).astype(np.int64) if V["uv"] else
                        np.zeros((len(K), 2), np.int64)], 1)
    C = K[np.asarray(tl, np.int64)]  # (n, 3 corners, 5)

    def less(a, b):
        lt = np.zeros(len(a), bool)
        for j in range(a.shape[1] - 1, -1, -1):
            lt = (a[:, j] < b[:, j]) | ((a[:, j] == b[:, j]) & lt)
        return lt
    r = np.zeros(len(C), np.int64)
    rows = np.arange(len(C))
    for i in (1, 2):
        r = np.where(less(C[:, i], C[rows, r]), i, r)
    rot = C[rows[:, None], (r[:, None] + np.arange(3)) % 3].reshape(len(C), -1)  # cyclic rotation keeps the winding
    keep = _first_rows(rot)[1]
    if isinstance(tl, np.ndarray):
        return list(map(tuple, tl[keep].tolist()))
    return [tl[i] for i in keep.tolist()]


def place(part):
    q = souls._euler_deg_xzy(*part["rot"])
    s, t = part["scale"], part["pos"]

    def xf(p):
        r = ha.qrot(q, (p[0] * s[0], p[1] * s[1], p[2] * s[2]))
        return (r[0] + t[0], r[1] + t[1], r[2] + t[2])

    def xn(n):
        return ha.qrot(q, n)
    # (n, 3) arrays at once, the same operations per element
    xf.many = lambda P: _qrot_np(q, P * np.asarray(s, np.float64)) + np.asarray(t, np.float64)
    xn.many = lambda N: _qrot_np(q, N)
    return xf, xn


def quantize_rgba(w, h, rgba, soft=False):
    """-> (w, h, CSM1 CLUT bytes, pixel bytes). Index 255 is transparent (alpha 0); PSMT8 wants width >= 128.
    soft: 256 RGBA colours, CLUT alpha 0..0x80 (blended effects)."""
    from PIL import Image
    img = Image.frombytes("RGBA", (w, h), rgba)
    if w < 128:
        img = resample.resize(img, (128, h * 128 // w), Image.BILINEAR)
    if img.size[1] > 128:  # VRAM budget: 128x128 at most (16 KB each with objects' textures added)
        img = resample.resize(img, (img.size[0], 128), Image.BILINEAR)
    w, h = img.size
    if soft:
        import octree  # Pillow's FASTOCTREE, the same on every platform (TECH_DEBT build-7)
        q = octree.quantize(img, 256)
        pal = list(q.getpalette("RGBA") or [])[:1024]
        pal += [0] * (1024 - len(pal))
        clut = bytearray(1024)
        for i in range(256):
            j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
            r, g, b, a = pal[4 * i:4 * i + 4]
            clut[4 * j:4 * j + 4] = bytes([r, g, b, (a * 0x80 + 127) // 255])
        return w, h, bytes(clut), q.tobytes()
    alpha = img.getchannel("A").point(lambda a: 255 if a >= 128 else 0)
    q = img.convert("RGB").quantize(255, method=Image.Quantize.MEDIANCUT)
    pal = (q.getpalette() or [])[:765]
    pal += [0] * (765 - len(pal))
    idx = bytearray(q.tobytes())
    am = alpha.tobytes()
    for i, a in enumerate(am):
        if not a:
            idx[i] = 255
    # the transparent index's colour: the mean of the opaque texels next to transparent ones. The GS filters CLUT
    # texels bilinearly, so cut-out edges (grass blades, leaves) blend towards this colour, not black.
    edge = [0, 0, 0, 0]
    for i, a in enumerate(am):
        if a and any(0 <= j < len(am) and not am[j] for j in (i - 1, i + 1, i - w, i + w)):
            r, g, b = pal[3 * idx[i]:3 * idx[i] + 3]
            edge = [edge[0] + r, edge[1] + g, edge[2] + b, edge[3] + 1]
    clear = [c // edge[3] for c in edge[:3]] if edge[3] else [0, 0, 0]
    clut = bytearray(1024)
    for i in range(256):
        j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)
        rgb = pal[3 * i:3 * i + 3] if i < 255 else clear
        clut[4 * j:4 * j + 4] = bytes(rgb + [0x80 if i < 255 else 0])
    return w, h, bytes(clut), bytes(idx)


def gather(game, map_id):
    """MSB map pieces + objects -> (meshes, textures, navmesh, spawn part, fog row). Each mesh: dict(pos=[welded
    world positions], wpos=[wedge -> position vertex], wuv, wrgb (static light), wa (alpha), tris=[wedge triples], ti,
    flags (CHUNK_*), grass, keep (not simplified))."""
    mdir = os.path.join(game, "map")
    area = map_id[:3]
    msb = souls.read(f"{mdir}/MapStudio/{map_id}.msb")
    M = souls.msb1(msb)
    ids = part_draw_ids(msb)
    light_rows, fog_rows, tone_rows = draw_params(game, area)
    pieces = [i for i, p in zip(ids, M["parts"]) if p["type"] == 0]
    main_light = max({l for l, _ in pieces if l in light_rows}, key=lambda r: sum(l == r for l, _ in pieces))
    main_fog = max({f for _, f in pieces if f in fog_rows}, key=lambda r: sum(f == r for _, f in pieces))
    lights = {r: dsr_light.light_fn(v) for r, v in light_rows.items()}
    mtds = mtd_params(game)
    sky_fog = None
    spawn = area_mod.spawn_part(M, map_id)
    sp = spawn["pos"]
    # area region (tools/area.py): objects and navmesh inside it; map pieces when their draw groups are shown from the
    # region's collision parts (DSR's own visibility); the whole map when the area has no radius
    import export_collision
    keep = area_mod.region(M, map_id)
    vis = area_mod.visible_groups(msb, M, map_id, {s["part"] for s in export_collision.area_collision(game, map_id)[0]})
    reg_coll = {s["part"] for s in export_collision.area_collision(game, map_id)[0]}
    DG.collisions([p["draw_groups"] for p in M["parts"] if p["type"] == 5 and p["name"] in reg_coll])  # dg.bin
    in_map = area_mod.map_region(M, map_id)  # map_radius around the spawn, or a world block's cells (tools/area.py)
    map_r = in_map is not None
    nav_pts = navmesh_points(game, map_id, M, keep)
    souls.VERT_NP = _VERT_NP  # filled as souls.flver parses (the values np.array gives from the lists), until the end
    souls.VERT_ROWS = souls.FLVER_NP and os.environ.get("VERT_ROWS", "1") == "1"  # FLVER attributes as souls.Rows
    MX = mapx_native.lib()  # tools/mapx (C): thin_foliage + is_canopy; MAPX=0 runs the Python code below
    mx_grid = None
    if MX is not None and nav_pts:
        _np_pts = np.ascontiguousarray(nav_pts, np.float64)
        mx_grid = MX.mx_navgrid_new(len(_np_pts), _np_pts.ctypes.data)
    nav_grid = {}  # 2 m xz cells -> navmesh heights, for is_canopy
    for p in nav_pts:
        nav_grid.setdefault((int(p[0] // 2), int(p[2] // 2)), []).append(p[1])

    def is_canopy(ps):
        """foliage component (world positions) whose base lies CANOPY_H above the walkable ground within 4 m (xz),
        or with no walkable ground within 12 m: tree canopy / wall foliage, not ground grass"""
        base = min(p[1] for p in ps)
        cx, cz = sum(p[0] for p in ps) / len(ps), sum(p[2] for p in ps) / len(ps)
        for r in (2, 6):  # 4 m, then 12 m
            ys = [y for i in range(int(cx // 2) - r, int(cx // 2) + r + 1) for j in range(int(cz // 2) - r, int(cz // 2) + r + 1)
                  for y in nav_grid.get((i, j), ()) if y <= base + 0.5]
            if ys:
                return base - max(ys) > CANOPY_H
        return True
    # walkable space for the sky test: navmesh vertices on a 10 m grid (sky threshold 500 m, so the error is moot)
    nav_tree = np.unique(np.round(np.array(nav_pts) / 10), axis=0) * 10 if nav_pts else None
    tone = tone_rows.get(main_light, tone_rows[min(tone_rows)])  # rows are per block like the light rows
    fx_gain = tone["grayKeyValue"] / tone["maxAdapredLum"]
    # DSR's light model (tools/dsr_light.py): light probes by collision part, tone map + tone correction per part's
    # ToneMapID / ToneCorrectID (auto exposure from the baked scene, below), the bonfire's point light
    banks = dsr_light.draw_banks(game, area)
    probes = dsr_light.load_probes(game, map_id)
    import havok_collision
    probe_at = dsr_light.ProbeIndex(havok_collision.map_collision(game, map_id)[0], probes,
                                    {p["name"]: probe_id(p.get("env_event")) for p in M["parts"] if p["type"] == 5})
    tones = {}
    # the tone map and scattering are global in DSR: the collision part under the player picks the rows (map pieces
    # have none; RenderDoc at the Burg spawn: tone map / ToneCorrect row 11 of the spawn's collision, not the most
    # common row 10). The lit map is baked with the rows of the collision part under the spawn.
    spawn_part, best = None, 1e18
    for sub in havok_collision.map_collision(game, map_id)[0]:
        for t in sub["tris"]:
            c = [sum(v[k] for v in t) / 3 for k in range(3)]
            dy = sp[1] - c[1]
            dd = (c[0] - sp[0]) ** 2 + (c[2] - sp[2]) ** 2 + (4 * dy if dy < -0.5 else dy) ** 2  # prefer below
            if dd < best:
                best, spawn_part = dd, sub["part"]
    coll = [p for p in M["parts"] if p["type"] == 5 and p["tone_map_id"] in tone_rows]
    under = next((p for p in coll if p["name"] == spawn_part), None)
    if under is None and coll:  # fallback: the most common collision row
        under = max(coll, key=lambda p: sum((q["tone_map_id"], q["tone_correct_id"]) ==
                                             (p["tone_map_id"], p["tone_correct_id"]) for q in coll))
    main_tone = (under["tone_map_id"], under["tone_correct_id"]) if under else (main_light, main_light)
    tone_of = {i: main_tone for i in range(len(M["parts"]))}
    sc_id = under["scatter_id"] if under and under["scatter_id"] in banks["LightScattering"] else None
    SCATTER[0] = dsr_light.Scatter(banks["LightScattering"][sc_id], area, sc_id) if sc_id is not None else None
    print(f"spawn collision {spawn_part}: tone map {main_tone}, scattering {sc_id}")
    near = SCATTER[0].near_factor() if SCATTER[0] else [1.0, 1.0, 1.0]
    # static point lights (visuals-effects, TECH_DEBT map-28): bonfire effect lights + MSB Light events; the
    # PointLightBank light of the collision part follows the player (fog_section). FX_LIGHTS=0 for A/B.
    # static point lights (export_effects.static_lights): the Makefile passes them as a file (FX_LIGHTS_FILE, written
    # by export_effects.py --static-lights) so map.bin depends on the lights, not on export_effects.py
    plights = []
    if os.environ.get("FX_LIGHTS", "1") == "1":
        if os.environ.get("FX_LIGHTS_FILE"):
            plights = [dsr_light.cubic_light(*a) for a in json.load(open(os.environ["FX_LIGHTS_FILE"]))]
        else:
            import export_effects
            plights = [dsr_light.cubic_light(*a) for a in export_effects.static_lights(game, map_id)]
    albedo_cache, pending = {}, []

    def albedo(name):
        """Mean sRGB colour of a texture's opaque texels: the tone map is applied per vertex for this colour."""
        if name not in albedo_cache:
            dec = souls.dds_decode_rgba(dds[name], 16) if name in dds else None
            a = (0.5, 0.5, 0.5)
            if dec:
                px = [dec[2][i:i + 4] for i in range(0, len(dec[2]), 4)]
                px = [p for p in px if p[3] >= 128] or px
                a = tuple(max(0.02, sum(p[k] for p in px) / len(px) / 255) for k in range(3))
            albedo_cache[name] = a
        return albedo_cache[name]

    # texture name -> DDS, from every TPF binder of the area
    dds = {}
    for f in sorted(os.listdir(f"{mdir}/{area}")):
        if f.endswith(".tpfbhd") and f.startswith(area + "_"):
            for _, _, d in souls.bxf3(f"{mdir}/{area}/{f}", f"{mdir}/{area}/{f[:-3]}bdt"):
                dds.update(souls.tpf(d))

    textures, tex_index, meshes = [], {}, []
    flvers, poses = {}, {}
    # each FLVER is dropped after the last part that places it (with its gather caches): parsed FLVERs held to the
    # end of the loop were 1.6 GB of Burg's 2.3 GB, past what a browser tab's converter can grow to (tools/pages)
    last_use = {}
    for pi, part in enumerate(M["parts"]):
        if part["type"] in (0, 1):
            last_use[part["model"] if part["type"] == 0 else (part["model"], not keep(*part["pos"]))] = pi
    expire = {}
    for k, pi in last_use.items():
        expire.setdefault(pi, []).append(k)

    def evict(k):
        F = flvers.pop(k, None)
        if not F:
            return
        lists = {id(m["verts"][a]) for m in F["meshes"] for a in souls.VERT_NP_ATTRS}
        for dk in [dk for dk, e in _DEDUP.items() if id(e[1]) in lists]:
            del _DEDUP[dk]
        for i in lists:
            _VERT_NP.pop(i, None)
    export_normal.SRC.update(dds=dds, probes=probes, near=near)
    switched = emevd.switched_entities(game, map_id) if EVENT_PARTS else set()
    for pi, (part, (lid, fid)) in enumerate(zip(M["parts"], ids)):
        for k in expire.get(pi - 1, ()):
            evict(k)
        if part["type"] in (0, 1) and not area_mod.drawn(vis, part["draw_groups"]):
            continue  # never drawn from the area's region
        far = part["type"] == 1 and not keep(*part["pos"])
        if part["type"] == 1 and not far and part["name"] in ANIM_PARTS:
            continue
        # objects beyond the region but inside map_radius are scenery like the map triangles around them: drawn as
        # static objects, breakables intact (the cropped collision ends at the region, nothing can reach them)
        if far and not (FAR_OBJ and map_r and in_map(*part["pos"])):
            continue
        if part["type"] == 1 and nav_tree is not None and \
                np.min(np.sum((nav_tree - np.array(part["pos"])) ** 2, axis=1)) > PARK_DIST ** 2:
            continue  # parked out of sight (Burg o1950 chains at y -839, 850 m under the walkable area)
        if part["type"] == 0:  # map piece
            if part["model"] not in flvers:
                path = f"{mdir}/{map_id}/{part['model']}A{map_id[1:3]}.flver.dcx"
                flvers[part["model"]] = souls.flver(souls.read(path))
        elif part["type"] == 1:  # object (bonfire, pots, ...): obj/oXXXX.objbnd, its main FLVER and its own TPF
            if (part["model"], far) not in flvers:
                flvers[part["model"], far] = load_object(game, part["model"], dds, far)
        else:
            continue
        F = flvers[part["model"]] if part["type"] == 0 else flvers[part["model"], far]
        if F is None:
            continue
        if part["model"] in KEEP_MODELS:
            KEEP_POS.append(part["pos"])
        pxf, pxn = place(part)
        bind = bind_scaled(F)
        for m in F["meshes"]:
            if part["type"] == 1 and m["dynamic"]:
                # skinned objects (corpses, o1950 chains): baked in the pose of their MSB init_anim (tools/pose_obj.py);
                # in bind pose they would T-pose
                pk = (part["model"], part.get("init_anim"))
                if pk not in poses:
                    poses[pk] = pose_obj.anim_bones(game, *pk) if POSE_OBJ else None
                if poses[pk] is None:
                    continue
                sk = pose_obj.skin_mesh_np(F, m, poses[pk])  # in C; the lists from Python without it
                if sk is not None:
                    P, N = souls.Rows(sk[0]), souls.Rows(sk[1])
                else:
                    P, N = pose_obj.skin_mesh(F, m, poses[pk])
                m = dict(m, verts=dict(m["verts"], pos=P, normal=N), dynamic=0, default_bone=-1)
            if part["type"] == 0 and m["verts"]["pos"]:
                xfm = xf_part(part, F, m)  # once per mesh (was once per vertex: 1.9M placements in Burg)
                sky = is_sky(m["mtd"], xfm.many(_vert_np(m["verts"]["pos"])), nav_tree)
            else:
                sky = False
            if sky and fid in fog_rows:
                sky_fog = fog_rows[fid]
            water = "water" in m["mtd"].lower()
            # "_Alp" MTDs are DSR's alpha-blended materials ("_Edge" = alpha-tested cut-outs). Floor decals / dirt /
            # ground overlays (blood, soot, stains: M[D][L]_Alp) exported as cut-outs drew every texel above 50 % alpha at
            # full strength and the rest in the clear colour (dark patches on Burg's floors), so they take texmap's soft
            # path (CT32 alpha, blended). Only those: blended meshes are kept unsimplified (keep=), and all _Alp meshes
            # together cost +43k tris / 36 fps at Burg spot B (measured 2026-10-31); wall / cloth _Alp stay cut-outs
            # (ASSUMED, TECH_DEBT map-30)
            decal = "_alp" in m["mtd"].lower() and any(k in (m["diffuse"] or "").lower()
                                                       for k in ("decal", "dirt", "ground", "stain"))
            blend = is_effect(m["mtd"]) or decal or water
            flags = (CHUNK_BLEND if blend else 0) | (CHUNK_ADD if "_add" in m["mtd"].lower() else 0) | \
                (CHUNK_SKY if sky else 0)
            if not m["dynamic"] and 0 <= m["default_bone"] < len(bind):
                xf, xn = bone_xf(pxf, pxn, *bind[m["default_bone"]])
            else:
                xf, xn = pxf, pxn
            grass = name_is_grass(m["diffuse"])
            lods = {f[0] & 0x0F000000: f for f in m["facesets"] if not f[0] & 0x80000000}
            if not lods:
                continue
            # objects beyond the region (70 m+ from the spawn in Burg) take the game's lowest LOD, as DSR draws them
            # at that range (o1154 barrel + ropes: 5.9k -> 1.2k tris before QEM)
            low = GAME_LOD or far
            if FAR_LOD and not low and part["type"] == 0 and len(lods) > 1 and nav_tree is not None:
                # map pieces whose LOD0 lies wholly beyond STREAM_FAR of the walkable space (whole-MSB areas) take the
                # game's lowest LOD before QEM, as DSR draws them from there (render-distance option (a), map-36)
                Vp = m["verts"]["pos"]
                wp = xf.many(_vert_np(Vp)[np.unique(faceset_tris(lods[min(lods)]))])
                lo, hi = wp.min(0), wp.max(0)
                d = np.maximum(0, np.maximum(lo[None] - nav_tree, nav_tree - hi[None]))
                low = len(wp) and np.min((d * d).sum(1)) > STREAM_FAR ** 2
                FAR_LOD_STATS[bool(low)] += 1
            fs = lods[max(lods)] if low else lods[min(lods)]
            dk = (id(fs), id(m["verts"]["pos"]))  # a model placed many times: its face set is deduplicated once
            if dk not in _DEDUP or _DEDUP[dk][0] is not fs or _DEDUP[dk][1] is not m["verts"]["pos"]:
                _DEDUP[dk] = (fs, m["verts"]["pos"], dedup_tris(faceset_tris(fs), m["verts"]))
            tl = list(_DEDUP[dk][2])
            if (map_r or PANO_R) and not sky:  # each used vertex placed once (was 9x per triangle: 11.7M in Burg)
                ui = list({i for t in tl for i in t})
                Wp = dict(zip(ui, map(tuple, xf.many(_vert_np(m["verts"]["pos"])[ui]).tolist())))
            if map_r and not sky:  # area map radius (tools/area.py): triangles centred beyond it are dropped
                tl = [t for t in tl if in_map(*(sum(Wp[i][k] for i in t) / 3 for k in range(3)))]
            if PANO_R and not sky:  # far panorama (tools/pano.py): beyond PANO_R of PANO_C it replaces the map
                pc = PANO_C or sp
                far_t = lambda t: sum((sum(Wp[i][k] for i in t) / 3 - pc[k]) ** 2 for k in range(3)) > PANO_R ** 2
                tl = [t for t in tl if far_t(t) == PANO_ONLY]
            fol, gpos = None, None
            if grass and MX is not None and m["verts"]["pos"] and type(m["verts"]["pos"][0][0]) is float:
                tl, fol, gpos = thin_foliage_c(MX, tl, m["verts"]["pos"], f"{part['name']}/{len(meshes)}",
                                               "ivy" in m["mtd"].lower(), xf.many(_vert_np(m["verts"]["pos"])), mx_grid)
            elif grass:
                tl, fol, gpos = thin_foliage(tl, m["verts"]["pos"], f"{part['name']}/{len(meshes)}",
                                             "ivy" in m["mtd"].lower(),
                                             canopy=lambda vs, Vp=m["verts"]["pos"], xf=xf: is_canopy([xf(Vp[i]) for i in vs]))
            if not tl:
                continue
            ti = -1
            name = m["diffuse"]
            if water:  # no diffuse map: a solid texture in the water's colour (water_rgba)
                name = "water:" + m["mtd"]
                if (name, blend) not in tex_index:
                    tex_index[name, blend] = len(textures)
                    textures.append(texmap.encode(128, 16, water_rgba(mtds.get(m["mtd"], {})) * (128 * 16), soft=True))
                ti = tex_index[name, blend]
            elif name and name in dds:
                if (name, blend) not in tex_index:
                    dec = souls.dds_decode_rgba(dds[name], TEX_MAX)
                    if dec:
                        tex_index[name, blend] = len(textures)
                        textures.append(texmap.encode(*dec, soft=blend, name=name))
                        if not blend:
                            near_level(textures[-1], dds[name], name)
                ti = tex_index.get((name, blend), -1)
            V = dict(m["verts"], pos=gpos) if gpos else m["verts"]
            # weld by world position (0.1 mm), wedges = (position, UV, colour); normals of merged wedges summed
            Tl = np.array(tl, np.int64).reshape(-1, 3)
            used = np.flatnonzero(np.bincount(Tl.reshape(-1).astype(np.intp)))  # sorted, as np.unique
            pos, wpos, wuv, wcol, wn, fvw, npd = _weld(V, used, xf, xn)
            # effects and sky: unlit, material colour x g_DiffuseMapColorPower x the same exposure as the lit map;
            # additive light beyond the 2x vertex colour range moves into alpha. Everything else gets its part's
            # LightBank row.
            mp = mtds.get(m["mtd"], {})
            lit = None if blend or sky else lights.get(lid, lights[main_light])
            if lit is None:
                gain = fx_gain * mp.get("g_DiffuseMapColorPower", (1.0,))[0]
                tint = [gain * x for x in mp.get("g_DiffuseMapColor", (1.0, 1.0, 1.0))]
            spill = max(1.0, max(tint) / 2) if lit is None and flags & CHUNK_ADD else 1.0
            wrgb, wa = [], []
            alb = albedo(name)
            if lit and not water:  # linear HDR irradiance now, tone mapped once the exposure is known (below)
                # (hemisphere + probe + point lights, directional, position, unit normal) per wedge: shadowed below
                hdr = _wedge_light(lit, wn, pos, wpos, probes, probe_at, plights, npd)
                # [M] materials blend a second diffuse by vertex alpha; the PS2 draws the first only, so the vertex
                # colour carries the blend: DSR's output for the blended mean colour / the first texture's mean
                alb2 = albedo(m["diffuse2"]) if "[M]" in m["mtd"] and m.get("diffuse2") in dds else None
                pending.append((wrgb, hdr, wcol, alb, alb2, tone_of[pi], npd))
            if lit and not water:  # tone mapped below; alpha 0x80
                wa = [0x80] * len(wn)
                npd["wa"] = (wa, np.full(len(wa), 0x80, np.int64))
            else:
                for n, c in zip(wn, wcol):
                    k = [x / spill for x in tint] if not lit else (1.0, 1.0, 1.0)
                    if sky:  # emissive HDR: texture x MTD colour x power, through the tone map like the lit map
                        t = tones.get(tone_of[pi]) or tones.setdefault(tone_of[pi], dsr_light.Tone(
                            tone_rows[tone_of[pi][0]], banks["ToneCorrect"].get(tone_of[pi][1])))
                        g = mp.get("g_DiffuseMapColorPower", (1.0,))[0]
                        out = t([dsr_light.srgb_lin(alb[j]) * g * x for j, x in
                                 enumerate(mp.get("g_DiffuseMapColor", (1.0, 1.0, 1.0)))])
                        k = [out[j] / alb[j] for j in range(3)]
                    if water:  # colour and opacity are in the texture (vertex colours hold the shader's flow data)
                        k, c = (1.0, 1.0, 1.0), (1.0, 1.0, 1.0, 1.0)
                    wrgb.append([min(255, int(0x80 * k[j] * c[j])) for j in range(3)])
                    wa.append(min(0x80, int(0x80 * c[3] * spill + 0.5)) if blend else 0x80)
            # normal-map relief (tools/export_normal.py): per-wedge tangent frames of lit, opaque bumped meshes
            bump = export_normal.register(m, V, fvw, wn, xn, light_rows.get(lid, light_rows[main_light]), alb,
                                          probe_at, npd.get("wn")) if lit and not water and not grass and V["uv"] else None
            # triangles in wedges (fvw[vertex]), as an array too (simplify_meshes_c, far_cells_c, the shadow map)
            lut = np.zeros(int(fvw.vs.max()) + 1, np.int32)
            lut[fvw.vs] = fvw.ws
            Tw = np.ascontiguousarray(lut[Tl])
            mtris = souls.Rows(Tw)  # reads as the list of wedge triples
            npd["tris"] = (mtris, Tw)
            npd.pop("wn", None)  # used up (light, relief)
            if not (lit and not water):
                npd.pop("wcol", None)  # the tone map below takes the lit meshes' wedge colours
            meshes.append(dict(pos=pos, wpos=wpos, _np=npd, wuv=wuv, wrgb=wrgb, wa=wa, tris=mtris,
                               bump=bump,
                               ti=ti if V["uv"] else -1, flags=(1 if fs[3] else 0) | flags, grass=grass, fol=fol,
                               keep=grass or blend or sky or part["model"] in KEEP_MODELS, name=f"{part['name']}/{name}",
                               dg=DG.add(part["draw_groups"]),
                               ent=part["entity_id"] if part["entity_id"] in switched else 0,
                               # PVS unit and occluder class: only static, opaque map pieces hide what is behind them
                               # (objects move or break, cut-outs and blends let light through)
                               piece=part["name"],
                               occ=int(part["type"] == 0 and not blend and not grass and not sky and not water and
                                       "_alp" not in m["mtd"].lower() and "_edge" not in m["mtd"].lower() and
                                       part["entity_id"] not in switched)))

    if mx_grid is not None:
        MX.mx_navgrid_free(mx_grid)
    souls.VERT_NP = None
    souls.VERT_ROWS = False
    _VERT_NP.clear()  # per-FLVER arrays and deduplicated face sets: only for placing meshes
    _DEDUP.clear()
    rss(f"gather meshes ({len(meshes)})")
    # lit map: auto exposure per tone map row (geometric mean, see the constants), then vertex colour = tone mapped colour / texture mean, so
    # texture x vertex colour gives DSR's output for the texture's mean colour
    # sun shadows: depth map of the opaque map (no foliage, effects or sky) along the main ShadowBank row's light
    # direction. DSR's deferred pass multiplies probe + hemisphere light by the shadow rate too (LightProbe.fpo:
    # Radiosity = min(shadow, Radiosity)), but fades the shadow out with camera distance (ShadowBank row 20: full to
    # 3 m, gone at 10 m), which a static bake cannot do: directional light is dropped in shadow, the rest x
    # (1 - SHADOW_AMB), SHADOW_AMB the shadow's average strength over a 0-20 m view
    sh = banks["Shadow"].get(main_tone[0]) or banks["Shadow"].get(main_light)
    # the player's reach: navmesh vertices (shadows only matter where characters and the camera go)
    nav_focus = navmesh_points(game, map_id, M, keep)
    if sh and SHADOWS:
        t0 = time.time()
        occ = [_mesh_np(m)[0][_mesh_np(m)[1][_np_list(m, "tris", m["tris"], np.intp, 3)]]
               for m in meshes if not m["flags"] & (CHUNK_BLEND | CHUNK_SKY) and not m["grass"] and m["tris"]]
        occ = np.concatenate(occ) if occ else np.zeros((0, 3, 3))
        smap = dsr_light.ShadowMap(occ, dsr_light.light_dir(sh["lightDegRotX"], sh["lightDegRotY"]),
                                   focus=nav_focus or [p for m in meshes for p in m["pos"][::50]])
        export_normal.SRC["smap"] = smap
        lit_n, floor = 0, [0.0, 0]
        ks = smap.lit_many(np.concatenate([h["P"] for _, h, *_ in pending] or [np.zeros((0, 3))]),
                           np.concatenate([h["N"] for _, h, *_ in pending] or [np.zeros((0, 3))]))
        j = 0
        for _, h, *_ in pending:
            k = ks[j:j + len(h["E"])]
            j += len(h["E"])
            for x in k.tolist():  # running sums in wedge order, as before
                lit_n += x
            for x in k[h["N"][:, 1] > 0.7].tolist():  # floors: how much of the ground characters stand on is in sun
                floor[0] += x
                floor[1] += 1
            h["H"] = h["E"] * (1 - SHADOW_AMB * (1 - k))[:, None] + h["S"] * k[:, None]
        nv = sum(len(h["E"]) for _, h, *_ in pending)
        SUN_FLOOR[0] = floor[0] / max(1, floor[1])
        print(f"floors in sun: {100 * SUN_FLOOR[0]:.0f}%")
        print(f"sun shadow map {smap.W}x{smap.H} ({len(occ)} tris, {time.time() - t0:.1f} s): {100 * lit_n / max(1, nv):.0f}% "
              f"of {nv} vertices lit")
    else:
        for _, h, *_ in pending:
            h["H"] = h["E"] + h["S"]
    rss("shadows")
    # tone map, vectorised per mesh with the per-wedge operations (dsr_light.sum3 / srgb_lin_many / Tone.many)
    lum = {}
    for _, h, wcol, alb, alb2, tk, npd in pending:
        C = _np_list({"_np": npd}, "wcol", wcol, np.float64, 4)
        A = np.stack([alb[j] + (alb2[j] - alb[j]) * C[:, 3] for j in range(3)], 1) if alb2 else \
            np.broadcast_to(np.asarray(alb, np.float64), (len(C), 3))
        h["L"] = dsr_light.srgb_lin_many(A * C[:, :3])  # srgb_lin(a[j] * c[j])
        H, L = h["H"], h["L"]
        y = dsr_light.sum3(*(dsr_light.LUMA[j] * L[:, j] * H[:, j] for j in range(3)))
        s = lum.setdefault(tk, [0.0, 0])
        for x in y.tolist():
            s[0] += math.log(x if x > 1e-4 else 1e-4)
            s[1] += 1
    for tk, (s, n) in lum.items():
        t = tones.setdefault(tk, dsr_light.Tone(tone_rows[tk[0]], banks["ToneCorrect"].get(tk[1])))
        t.adapt(math.exp(s / max(1, n)))  # geometric mean over the bake's vertices stands in for DSR's screen average
        print(f"tone row {tk[0]} / correct {tk[1]}: scene luminance {math.exp(s / max(1, n)):.3f} -> exposure {t.exposure:.2f}")
    for wrgb, h, wcol, alb, alb2, tk, npd in pending:
        out = tones[tk].many(h["L"] * h["H"] * np.asarray(near, np.float64))  # + scattering at d = 0
        q = np.trunc(0x80 * out / np.asarray(alb, np.float64) + 0.5)
        q = np.ascontiguousarray(np.minimum(255, q).astype(np.int64))
        wrgb.extend(q.tolist())
        npd["wrgb"] = (wrgb, q)
        npd.pop("wcol", None)
        h.clear()
    TONES.update(tones)  # fog colours and the character light use the same tone maps
    MAIN_TONE[0] = tones[main_tone]

    # navmesh: MSB navmesh parts (type 8) placing the NVM models
    nav_models = {souls.basename(n)[:-4]: d for _, n, d in
                  souls.bnd3(souls.read(f"{mdir}/{map_id}/{map_id}.nvmbnd.dcx"))}
    nav_v, nav_t = [], []
    for part in M["parts"]:
        if part["type"] != 8:
            continue
        key = f"{part['model']}A{map_id[1:3]}"
        if key not in nav_models:
            continue
        xf, _ = place(part)
        nv, nt = souls.nvm(nav_models[key])
        base = len(nav_v)
        nav_v += [xf(p) for p in nv]
        nav_t += [(a + base, b + base, c + base) for a, b, c, _ in nt if any(keep(*nav_v[base + i]) for i in (a, b, c))]
    print(f"foliage: {CANOPY_STATS[0]} canopy / wall tris (CANOPY_H {CANOPY_H} m) kept whole, never thinned")
    if FAR_LOD_STATS[1]:
        print(f"far LOD: {FAR_LOD_STATS[1]} of {sum(FAR_LOD_STATS)} map meshes with game LODs take the lowest "
              f"(beyond {STREAM_FAR:.0f} m of the navmesh)")
    return meshes, textures, nav_v, nav_t, spawn, (main_fog, fog_rows[main_fog]), sky_fog


def view_distance(points, spawn, nav_v, nav_t, raw=False, floor=None):
    """Per point (lists of positions): the distance it is judged at, max(NEAR, distance to the walkable navmesh,
    SPAWN_K x distance to the spawn). Near anything walkable it stays detailed, background gets coarse. raw: the
    distance to the navmesh alone."""
    import numpy as np
    nv = np.array(nav_v, dtype=np.float64)
    nt = np.array(nav_t, dtype=np.int64)
    a, b, c = nv[nt[:, 0]], nv[nt[:, 1]], nv[nt[:, 2]]
    samples = [nv, (a + b + c) / 3]
    for s in (1, 2, 3):  # points inside the navmesh triangles, so large polygons count too
        for u, v in ((s / 4, 0), (0, s / 4), (s / 4, (4 - s) / 4 * 0.5)):
            samples.append(a + u * (b - a) + v * (c - a))
    samples.append((a + b) / 2)
    samples.append((b + c) / 2)
    samples.append((a + c) / 2)
    S = np.unique(np.round(np.concatenate(samples) * 2) / 2, axis=0)  # 0.5 m
    allp = np.array([p for ps in points for p in ps], dtype=np.float64)
    cells, inv = np.unique(np.round(allp / 2) * 2, axis=0, return_inverse=True)  # 2 m: only needed coarsely
    dn = np.empty(len(cells))
    lib = dsr_light.geom_lib()
    if lib is not None and len(S):  # k-d tree (tools/native/geom.c): Burg / Firelink 103 s of brute force -> < 1 s
        # cells (2 m) and samples (0.5 m) lie on lattices, so both paths' squared distances are exact: same result
        Sc, Qc, d2 = np.array(S, np.float64, order="C"), np.ascontiguousarray(cells, np.float64), np.empty(len(cells))
        if lib.nearest_d2(len(Qc), Qc.ctypes.data, len(Sc), Sc.ctypes.data, d2.ctypes.data):
            raise MemoryError("nearest_d2")
        dn[:] = np.sqrt(np.maximum(d2, 0))
    else:
        s2 = (S ** 2).sum(1)
        for i in range(0, len(cells), 4096):
            c = cells[i:i + 4096]
            d2 = (c ** 2).sum(1)[:, None] + s2[None, :] - 2 * c @ S.T
            dn[i:i + 4096] = np.sqrt(np.maximum(d2.min(1), 0))
    dn = dn[inv.ravel()]
    if raw:
        return [dn[k:k + len(ps)].tolist() for k, ps in zip(np.cumsum([0] + [len(ps) for ps in points]), points)]
    ds = np.sqrt(((allp - np.array(spawn)) ** 2).sum(1))
    d = np.maximum(floor or NEAR, np.maximum(dn, SPAWN_K * ds))
    for kp in KEEP_POS:  # ground under kept objects (KEEP_R)
        near_k = (allp[:, 0] - kp[0]) ** 2 + (allp[:, 2] - kp[2]) ** 2 < KEEP_R ** 2
        d[near_k] = np.minimum(d[near_k], KEEP_D)
    out, k = [], 0
    for ps in points:
        out.append(d[k:k + len(ps)].tolist())
        k += len(ps)
    return out


def prune_foliage(meshes, nav_v, nav_t):
    """Drop the kept foliage tufts that map.c could never draw: those whose rank stays above the density at the
    nearest the camera gets (distance to the navmesh - CAM_REACH; the chunk box can be a GRASS_GRID cell closer)."""
    ms = [m for m in meshes if m["grass"] and m["tris"]]
    pts = [[m["pos"][m["wpos"][t[0]]] for t in m["tris"]] for m in ms]
    before = after = 0
    for m, d in zip(ms, view_distance(pts, None, nav_v, nav_t, raw=True)):
        near = {}
        for (r, tuft, _), x in zip(m["fol"], d):
            near[tuft] = min(near.get(tuft, x), x)
        keep = [r < (GRASS_D0 / max(GRASS_D0, near[tuft] - CAM_REACH - 2 * GRASS_GRID)) ** 2 for r, tuft, _ in m["fol"]]
        before += len(keep)
        m["tris"] = [t for t, k in zip(m["tris"], keep) if k]
        m["fol"] = [f for f, k in zip(m["fol"], keep) if k]
        after += len(m["tris"])
    print(f"foliage: {before} tris kept by GRASS_KEEP, {after} within the camera's reach")


BACKDROP_STATS = [0, 0]


def _take(tl, idx):
    """the triangles idx (ascending) of tl (a list or mapx_native.Tris)"""
    if isinstance(tl, mapx_native.Tris):
        return tl[np.asarray(idx, np.intp)] if idx else []
    return [tl[i] for i in idx]


def backdrop_visible(m, tl, nav_s):
    """Backdrop triangles of mesh m minus the one-sided ones (CHUNK_CULL) that face away from every navmesh sample point:
    never seen from anywhere the player can stand."""
    BACKDROP_STATS[0] += len(tl)
    if not BACKDROP_SIDE or not m["flags"] & 1 or not nav_s:
        BACKDROP_STATS[1] += len(tl)
        return tl
    import numpy as np
    if not tl:
        return tl
    P = mapx_native.tri_arrays(tl)[0]  # (n, 3 corners, xyz)
    n = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
    S = np.array(nav_s, dtype=np.float64)
    # facing some sample point: n . (s - p0) > 0 (the winding / cull sense as map.c draws it is checked by the A/B)
    vis = ((S[None, :, :] - P[:, None, 0, :]) * n[:, None, :]).sum(2)
    keep = (vis * BACKDROP_SIDE > 0).any(1)
    out = _take(tl, np.flatnonzero(keep).tolist())
    BACKDROP_STATS[1] += len(out)
    return out


SOFT_STATS = [0, 0]  # coarse / backdrop triangles seen, softened


def soften_thin(entries, view_s, floor):
    """Thin faces of the coarse / backdrop levels (render-12): a face drawn narrower than SOFT_PX pixels covers a pixel
    centre in one frame and misses it in the next, so the narrow lit pillar fronts of Firelink's ring wall (0.3 m, ~2 px
    from the graveyard) flicker as the camera moves (512 px frame, no anti-aliasing; DSR draws them 4-6 px wide). The
    width is the triangle's narrowest extent projected toward the viewpoints view_s (navmesh samples), the median over
    those within SOFT_REACH metres of the nearest one, at their distance - CAM_REACH (at least floor). Its corners'
    colours are pulled by 1 - width / SOFT_PX toward the mean colour of every coarse / backdrop triangle within SOFT_R_PX
    pixels of it at that distance (weighted by area seen from the viewpoints, any mesh): a pre-filter like a texture
    mip, for geometry. entries: the level entries (dicts with tris, wrgb); each softened one gets its own wrgb copy."""
    import numpy as np
    if not SOFT_PX or not entries or not len(view_s):
        return
    L = mapx_native.lib()
    if L is not None and dsr_light.geom_lib() is not None:
        return soften_thin_c(L, entries, view_s, floor)
    S = np.asarray(view_s, dtype=np.float64)
    rows = []  # per entry: corners P (n, 3, xyz), wedges W, colour per triangle, area seen, px, footprint (m / px)
    for e in entries:
        tl = e["tris"]
        SOFT_STATS[0] += len(tl)
        P = np.array([[c[0] for c in t] for t in tl], dtype=np.float64)
        W = np.array([[c[1] for c in t] for t in tl], dtype=np.int64)
        E = np.stack([P[:, 1] - P[:, 0], P[:, 2] - P[:, 1], P[:, 0] - P[:, 2]], 1)
        N = np.cross(E[:, 0], -E[:, 2])
        area2 = np.linalg.norm(N, axis=1)
        N /= np.maximum(area2, 1e-12)[:, None]
        height = area2 / np.maximum(np.linalg.norm(E, axis=2).max(1), 1e-6)  # narrowest extent
        px, mpp, seen = np.empty(len(tl)), np.empty(len(tl)), np.empty(len(tl))
        for i in range(0, len(tl), 2048):
            V = S[None, :, :] - P[i:i + 2048].mean(1)[:, None, :]
            d = np.linalg.norm(V, axis=2)
            cos = np.abs((V * N[i:i + 2048, None, :]).sum(2)) / np.maximum(d, 1e-6)
            dd = np.maximum(d - CAM_REACH, floor)
            far = d > d.min(1, keepdims=True) + SOFT_REACH
            w = height[i:i + 2048, None] * cos * SOFT_FOCAL / dd
            w[far], dd[far], cos[far] = np.nan, np.nan, np.nan
            px[i:i + 2048] = np.nanmedian(w, 1)
            mpp[i:i + 2048] = np.nanmedian(dd, 1) / SOFT_FOCAL
            seen[i:i + 2048] = np.nanmean(cos, 1)
        C = np.array(e["wrgb"], dtype=np.float64)[W].mean(1) if len(tl) else np.zeros((0, 3))
        rows.append((P, W, C, area2 * 0.5 * seen, px, mpp))
    cen = np.concatenate([r[0].mean(1) for r in rows if len(r[0])])
    col = np.concatenate([r[2] for r in rows if len(r[0])])
    wt = np.concatenate([r[3] for r in rows if len(r[0])])
    G = 2.0  # m: grid of triangle centroids for the footprint search
    gk = np.floor(cen / G).astype(np.int64)
    lib = dsr_light.geom_lib()
    if lib is None:  # the numpy path's cell lists (a Python dict over every centroid: only built when it is used)
        grid = {}
        for i, k in enumerate(map(tuple, gk)):
            grid.setdefault(k, []).append(i)
        grid = {k: np.array(v) for k, v in grid.items()}
    else:  # the cells sorted by key, members ascending (as the grid lists), for geom.c soft_targets
        order = np.lexsort((gk[:, 2], gk[:, 1], gk[:, 0]))
        sk = gk[order]
        new = np.ones(len(sk), bool)
        new[1:] = (sk[1:] != sk[:-1]).any(1)
        ckey = np.ascontiguousarray(sk[new])
        cstart = np.append(np.nonzero(new)[0], len(sk)).astype(np.int64)
        cidx = np.ascontiguousarray(order, np.int64)
        cen_c, col_c, wt_c = (np.ascontiguousarray(a, np.float64) for a in (cen, col, wt))
    for e, (P, W, C, _, px, mpp) in zip(entries, rows):
        k = np.clip(1 - px / SOFT_PX, 0, 1) if len(P) else px
        if not len(P) or not k.any():
            continue
        tgt = C.copy()
        if lib is not None:
            q = np.nonzero(k > 0)[0]
            Cq = np.ascontiguousarray(((P[q, 0] + P[q, 1]) + P[q, 2]) / 3)  # P[i].mean(0)
            Rq = np.ascontiguousarray(SOFT_R_PX * mpp[q])
            out, ok = np.empty((len(q), 3)), np.zeros(len(q), np.uint8)
            if lib.soft_targets(len(q), Cq.ctypes.data, Rq.ctypes.data, len(cen_c), cen_c.ctypes.data,
                                col_c.ctypes.data, wt_c.ctypes.data, len(ckey), ckey.ctypes.data, cstart.ctypes.data,
                                cidx.ctypes.data, G, out.ctypes.data, ok.ctypes.data) < 0:
                raise MemoryError("soft_targets")
            tgt[q[ok == 1]] = out[ok == 1]
        else:  # numpy path (no C helpers)
            for i in np.nonzero(k > 0)[0]:
                c, r = P[i].mean(0), SOFT_R_PX * mpp[i]
                lo, hi = np.floor((c - r) / G).astype(int), np.floor((c + r) / G).astype(int)
                idx = [grid[q] for q in ((x, y, z) for x in range(lo[0], hi[0] + 1) for y in range(lo[1], hi[1] + 1)
                                         for z in range(lo[2], hi[2] + 1)) if q in grid]
                if not idx:
                    continue
                idx = np.concatenate(idx)
                idx = idx[((cen[idx] - c) ** 2).sum(1) <= r * r]
                if len(idx) and wt[idx].sum() > 0:
                    tgt[i] = (col[idx] * wt[idx, None]).sum(0) / wt[idx].sum()
        # per wedge: the area-weighted mean of its triangles' k and targets
        ta = np.linalg.norm(np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]), axis=1)
        nw = len(e["wrgb"])
        kw, ww, tw = np.zeros(nw), np.zeros(nw), np.zeros((nw, 3))
        for j in range(3):
            np.add.at(kw, W[:, j], k * ta)
            np.add.at(ww, W[:, j], ta)
            np.add.at(tw, W[:, j], tgt * ta[:, None])
        out = list(e["wrgb"])
        for w in np.nonzero((ww > 0) & (kw > 0))[0]:
            f, t = kw[w] / ww[w], tw[w] / ww[w]
            out[w] = [min(255, int(out[w][j] + f * (t[j] - out[w][j]) + 0.5)) for j in range(3)]
        e["wrgb"] = out
        SOFT_STATS[1] += int((k > 0).sum())


def soften_thin_c(L, entries, view_s, floor):
    """soften_thin in C (tools/mapx/soften.c, libmapx): the same widths, footprint colours and wedge colours as the
    numpy code with geom.c's soft_targets."""
    S = np.ascontiguousarray(view_s, dtype=np.float64)
    toff, woff, Ps, Ws, Cs = [0], [0], [], [], []
    for e in entries:
        tl = e["tris"]
        SOFT_STATS[0] += len(tl)
        P, W = mapx_native.tri_arrays(tl)
        Ps.append(P.reshape(-1, 9))
        Ws.append(W)
        Cs.append(np.asarray(e["wrgb"], dtype=np.int32).reshape(-1, 3))
        toff.append(toff[-1] + len(tl))
        woff.append(woff[-1] + len(Cs[-1]))
    P, W, C = (np.ascontiguousarray(np.concatenate(x)) for x in (Ps, Ws, Cs))
    toff, woff = np.array(toff, np.int64), np.array(woff, np.int64)
    out, changed = np.empty_like(C), np.zeros(len(entries), np.uint8)
    SOFT_STATS[1] += L.mx_soften_thin(len(entries), toff.ctypes.data, P.ctypes.data, W.ctypes.data, woff.ctypes.data,
                                      C.ctypes.data, len(S), S.ctypes.data, floor, SOFT_PX, SOFT_FOCAL, SOFT_REACH,
                                      SOFT_R_PX, CAM_REACH, out.ctypes.data, changed.ctypes.data)
    for i in np.nonzero(changed)[0].tolist():
        entries[i]["wrgb"] = out[woff[i]:woff[i + 1]]  # (n, 3) int32: build_chunks_c takes it as is


# far panorama (render-distance variant A): PANO_R metres from PANO_C ("x,y,z", default the spawn); PANO_ONLY=1 keeps
# only the triangles beyond it at full detail (the capture export for tools/pano.py), else they are dropped
PANO_R = float(os.environ.get("PANO_R", 0))
PANO_C = [float(v) for v in os.environ["PANO_C"].split(",")] if os.environ.get("PANO_C") else None
PANO_ONLY = os.environ.get("PANO_ONLY") == "1"
BACKDROP = os.environ.get("BACKDROP", "1") == "1"  # far-only cells as the backdrop (render-distance pick A2); BACKDROP=0: old cut
BACKDROP_SIDE = int(os.environ.get("BACKDROP_SIDE", 0))  # 0: no back-face pruning (A/B); 1 / -1: front = n.(s-p) > 0 / < 0


def far_cells(meshes, nav_v):
    """STREAM: the set of CHUNK_GRID cells (of triangle centroids) whose centre lies beyond STREAM_FAR of the navmesh."""
    import numpy as np
    L = mapx_native.lib()
    if L is not None:
        return far_cells_c(L, meshes, nav_v)
    keys = set()
    for m in meshes:
        if m["keep"]:
            continue
        for t in m.get("tris_in", m["tris"]):
            ps = [m["pos"][m["wpos"][w]] for w in t]
            keys.add(tuple(int(math.floor(sum(q[a] for q in ps) / 3 / CHUNK_GRID)) for a in range(3)))
    keys = sorted(keys)
    if not keys or not nav_v:
        return set()
    c = (np.array(keys, dtype=np.float64) + 0.5) * CHUNK_GRID
    nv = np.array(nav_v, dtype=np.float64)
    d2 = np.concatenate([((c[i:i + 256, None, :] - nv[None, :, :]) ** 2).sum(2).min(1) for i in range(0, len(c), 256)])
    far = {k for k, d in zip(keys, d2) if d > STREAM_FAR * STREAM_FAR}
    print(f"stream: {len(far)} of {len(keys)} cells beyond {STREAM_FAR:.0f} m of the navmesh (one resident level)")
    return far


def far_cells_c(L, meshes, nav_v):
    """far_cells in C (tools/mapx/cells.c, libmapx)"""
    import ctypes
    Ts = [np.zeros((0, 9))]
    for m in meshes:
        tl = m.get("tris_in", m["tris"])
        if m["keep"] or not tl:
            continue
        P, Wp = _mesh_np(m)
        Ts.append(P[Wp[_np_list(m, "tris", tl, np.intp, 3)]].reshape(-1, 9))
    T = np.ascontiguousarray(np.concatenate(Ts))
    if not len(T) or not nav_v:
        return set()
    nv = np.ascontiguousarray(nav_v, np.float64)
    out, nc = np.empty((len(T), 3), np.int64), ctypes.c_longlong()
    nf = L.mx_far_cells(len(T), T.ctypes.data, len(nv), nv.ctypes.data, CHUNK_GRID, STREAM_FAR, out.ctypes.data,
                        ctypes.byref(nc))
    far = set(map(tuple, out[:nf].tolist()))
    print(f"stream: {len(far)} of {nc.value} cells beyond {STREAM_FAR:.0f} m of the navmesh (one resident level)")
    return far


def simplify_meshes(meshes, spawn, nav_v, nav_t, src="tris", out="tris", eps_fixed=None, floor=None, cells=False,
                    far=frozenset(), area_k=None, eps_far=None, budget=None, nav_s=None):
    """Quadric edge collapse (tools/simplify.py) of every non-grass mesh. Meshes that share vertex positions (2 mm)
    are welded into one group and simplified together, so seams between neighbouring meshes are ordinary edges
    whose vertices move for both sides at once (no cracks); every triangle keeps its own mesh's UVs / colours.
    One process per group. Cost = error^2 / view distance^2 (angular error^2); one global threshold is searched to
    fit BUDGET triangles (foliage not counted: map.c thins it by distance), or EPS (eps_fixed) if set. Each mesh's
    src triangles (wedge triples) become out = corners (position, wedge). cells (STREAM): each group is cut into
    CHUNK_GRID cells by triangle centroid, positions on the cuts are locked, and out + "_cell" gets each triangle's
    cell; floor: the view distance floor (NEAR); eps_far: the threshold of the far cells' jobs (else the same)."""
    import numpy as np
    import simplify
    import simplify_pool
    MX = mapx_native.lib()
    if MX is not None and os.environ.get("SIMPLIFY_NATIVE") != "0" and nav_t:
        return simplify_meshes_c(MX, meshes, spawn, nav_v, nav_t, src, out, eps_fixed, floor, cells, far, area_k, eps_far,
                                 budget, nav_s)
    if budget:
        print(f"simplify: no budget solve without the native simplifier ({budget[3]})")

    def key(p):
        return (round(p[0] * 500), round(p[1] * 500), round(p[2] * 500))  # 2 mm
    # key() of every position, per mesh at once (numpy rint rounds half to even like round())
    pkeys = {}

    def keys_of(mi):
        if mi not in pkeys:
            P = np.array(meshes[mi]["pos"], np.float64).reshape(-1, 3)
            pkeys[mi] = list(map(tuple, np.rint(P * 500).astype(np.int64).tolist()))
        return pkeys[mi]
    # identical copies (same texture, same triangles; Firelink has some far _L walls twice) only draw once
    seen = set()
    for mi, m in enumerate(meshes):
        if m["keep"]:
            continue
        kp, wp = keys_of(mi), m["wpos"]
        k = (m["ti"], frozenset(frozenset(kp[wp[w]] for w in t) for t in m[src]))
        if k in seen:
            m[src] = []
        seen.add(k)
    live = [mi for mi, m in enumerate(meshes) if not m["keep"] and m[src]]
    par = {mi: mi for mi in live}

    def root(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x
    first = {}
    for mi in live:
        for k in keys_of(mi):
            if k in first:
                par[root(mi)] = root(first[k])
            else:
                first[k] = mi
    groups = {}
    for mi in live:
        groups.setdefault(root(mi), []).append(mi)
    groups = list(groups.values())
    # per group: welded positions, attributes = (mesh, wedge), triangles in attribute indices
    jobs, attrs, jcell, jkeys = [], [], [], []
    for g in groups:
        percell = {}
        for mi in g:
            m = meshes[mi]
            if cells and m[src]:  # CHUNK_GRID cell of each triangle's centroid (sum() of the corners, as before)
                Pm = np.array(m["pos"], np.float64).reshape(-1, 3)[np.array(m["wpos"], np.int64)[np.array(m[src], np.int64)]]
                cc = np.floor(_py_sum3(Pm[:, 0], Pm[:, 1], Pm[:, 2]) / 3 / CHUNK_GRID).astype(np.int64).tolist()
            for ti_, t in enumerate(m[src]):
                c = None
                if cells:
                    c = tuple(cc[ti_])
                    c = "far" if c in far else c  # far cells: one uncut job per welded group
                percell.setdefault(c, []).append((mi, t))
        for c, items in percell.items():
            pid, aid, pos, wpos, attr, tris, jk = {}, {}, [], [], [], [], []
            for mi, t in items:
                m = meshes[mi]
                kp = keys_of(mi)
                tt = []
                for w in t:
                    if (mi, w) not in aid:
                        k = kp[m["wpos"][w]]
                        if k not in pid:
                            pid[k] = len(pos)
                            pos.append(m["pos"][m["wpos"][w]])
                            jk.append(k)
                        aid[(mi, w)] = len(wpos)
                        wpos.append(pid[k])
                        attr.append((mi, w))
                    tt.append(aid[(mi, w)])
                tris.append(tuple(tt))
            jobs.append([pos, wpos, tris, None, None])
            attrs.append(attr)
            jcell.append(c)
            jkeys.append(jk)
    njobs = {}  # positions on a cell cut (in several jobs of one welded group) never move: both levels keep the cut
    for jk in jkeys:
        for k in jk:
            njobs[k] = njobs.get(k, 0) + 1
    for j, jk in zip(jobs, jkeys):
        j[3] = [njobs[k] > 1 for k in jk]
    dist = view_distance([j[0] for j in jobs], spawn, nav_v, nav_t, floor=floor)
    for j, d in zip(jobs, dist):
        j[4] = [1.0 / (x * x) for x in d]
    # cached by job content, run on forkserver workers that are retried when one dies (tools/simplify_pool.py;
    # SIMPLIFY_PROCS / SIMPLIFY_CACHE / SIMPLIFY_NATIVE)
    logs = simplify_pool.run_jobs(jobs, area_k=area_k)
    grass = sum(len(m[src]) for m in meshes if m["keep"] and not m["grass"])
    full = sum(len(lg[0]) for lg in logs)
    # every step of every group as (cost, triangles removed): the count at a threshold is one masked sum
    cost = np.concatenate([lg[1] for lg in logs] + [np.zeros(0)])
    gone = np.concatenate([lg[2] for lg in logs] + [np.zeros(0, np.int32)]).astype(np.int64)

    def count(eps):
        return grass + full - int(gone[cost <= eps].sum())
    print("simplify: tris at an error of " + ", ".join(f"{px} px {count((px / 568) ** 2)}" for px in (1, 2, 3, 4, 6, 8, 12, 24)))
    if eps_fixed is not None or EPS is not None:
        eps = eps_fixed if eps_fixed is not None else EPS
    else:
        lo, hi = 1e-16, 1.0
        while count(hi) > BUDGET and hi < 1e6:
            hi *= 100
        for _ in range(40):
            mid = math.sqrt(lo * hi)
            lo, hi = (lo, mid) if count(mid) <= BUDGET else (mid, hi)
        eps = hi
        if eps > EPS_MAX:  # BUDGET unreachable (Undead Burg: lightmapped pieces full of attribute seams / non-manifold
            # edges QEM must keep): the search ran to its cap and would collapse everything collapsible into spikes
            print(f"simplify: BUDGET {BUDGET} not reachable ({count(eps)} tris at any error); error capped at "
                  f"{math.sqrt(EPS_MAX) * 568:.0f} px -> {count(EPS_MAX)} tris")
            eps = EPS_MAX
    for mi in live:
        meshes[mi][out] = []
        meshes[mi][out + "_cell"] = []
    for (pos, _, _, _, _), attr, lg, c in zip(jobs, attrs, logs, jcell):
        for p0, a0, p1, a1, p2, a2 in simplify.replay_log(lg, eps_far if c == "far" and eps_far is not None else eps).tolist():
            mi = attr[a0][0]
            meshes[mi][out].append(((pos[p0], attr[a0][1]), (pos[p1], attr[a1][1]), (pos[p2], attr[a2][1])))
            meshes[mi][out + "_cell"].append(c)
    for m in meshes:
        if m["keep"]:
            m[out] = [tuple((m["pos"][m["wpos"][w]], w) for w in t) for t in m[src]]
        elif not m[src]:
            m[out] = []
    print(f"simplify: {full} -> {count(eps)} tris ({grass} kept: effects, sky; foliage extra) in {len(groups)} welded "
          f"groups / {len(jobs)} jobs, eps {eps:.3g} "
          f"(error <= {math.sqrt(max(eps, 0)) * 568:.1f} px at 640x448)")
    return eps


def solve_budget(MX, h, jc, eps, eps_far, budget, samples):
    """World streaming v2: per-job thresholds so that at every sample point (navmesh) the bytes of the level's cells
    within [r_in, r) (r None: every cell) stay under nbytes. Each cell's threshold is eps x 1.5^k; while a point is over,
    the heaviest cell around it that can still be raised goes one step up. Jobs outside cells (no cell cut, far-only
    cells) keep eps / eps_far. budget = (r_in, r, nbytes, name). Returns the per-job thresholds (float64) or None."""
    import numpy as np
    r_in, r, nbytes, name, px_cap = budget
    njobs = len(jc)
    cells = sorted({c for c in jc if isinstance(c, tuple)})
    if not cells or not len(samples):
        return None
    cid = {c: i for i, c in enumerate(cells)}
    jcell = np.array([cid[c] if isinstance(c, tuple) else -1 for c in jc], np.int64)
    # at most px_cap px of error: past it the spot stays over budget (mapstream drops its farthest near cells first).
    # Without a cap the near level of dense Burg spots went to 130x the threshold (68 px) and still did not fit: the
    # simplifier keeps open borders, UV seams and small parts, so a spot's bytes stop falling (first export 2026-11-06)
    K = 0
    while K < 12 and math.sqrt(eps * 1.5 ** (K + 1)) * 568 <= px_cap + 1e-6:
        K += 1
    steps = [eps * 1.5 ** k for k in range(K + 1)]
    cnt = np.zeros(max(njobs, 1), np.int64)
    by = np.zeros((len(cells), K + 1))  # bytes of cell c at step k
    sel = jcell >= 0
    for k, e in enumerate(steps):
        MX.mx_simp_job_counts(h, e, cnt.ctypes.data)
        by[:, k] = np.bincount(jcell[sel], weights=cnt[:njobs][sel], minlength=len(cells)) * WS_BPT
    lo = np.array(cells, np.float64) * CHUNK_GRID
    hi = lo + CHUNK_GRID
    P = np.asarray(samples, np.float64).reshape(-1, 3)
    d = np.sqrt((np.maximum(0, np.maximum(lo[None] - P[:, None], P[:, None] - hi[None])) ** 2).sum(2))
    M = (d >= r_in) & (d < r) if r else np.ones(d.shape, bool)
    Mf = M.astype(np.float64)
    step = np.zeros(len(cells), np.int64)
    cur = by[np.arange(len(cells)), step]
    W = Mf @ cur
    w0, raised, stuck = W.max(), 0, False
    while W.max() > nbytes and raised < 100000:
        p = int(W.argmax())
        cand = np.flatnonzero(M[p] & (step < K) & (cur > 0))
        if not len(cand):
            stuck = True
            break
        # the least simplified cells of the spot go up together: the spot's error evens out (one cell taken to 130x
        # the threshold while its neighbours kept theirs looked broken next to them; first export 2026-11-06)
        cand = cand[step[cand] == step[cand].min()]
        for c in cand.tolist():
            step[c] += 1
            new = by[c, step[c]]
            W -= Mf[:, c] * (cur[c] - new)
            cur[c] = new
        raised += 1
    job_eps = np.array([steps[step[jcell[i]]] if jcell[i] >= 0 else (eps_far if jc[i] == "far" and eps_far is not None
                                                                       else eps) for i in range(njobs)], np.float64)
    nr = int((step > 0).sum())
    print(f"budget {name}: worst spot {w0 / 1024:.0f} -> {W.max() / 1024:.0f} KB (budget {nbytes / 1024:.0f} KB, "
          f"{len(P)} navmesh points); {nr} of {len(cells)} cells coarser (up to {1.5 ** step.max():.1f}x the threshold, "
          f"{math.sqrt(steps[step.max()]) * 568:.1f} px)" + (f"; over budget at the {px_cap:.0f} px cap" if stuck else ""))
    BUDGET_REPORT.append(dict(level=name, worst_kb=round(w0 / 1024), after_kb=round(W.max() / 1024),
                              budget_kb=round(nbytes / 1024), cells=len(cells), coarser=nr, max_x=1.5 ** int(step.max()),
                              met=not stuck))
    return job_eps


BUDGET_REPORT = []  # solve_budget results, written next to map.bin (map.bin.budget.json)


def simplify_meshes_c(MX, meshes, spawn, nav_v, nav_t, src, out, eps_fixed, floor, cells, far, area_k, eps_far,
                      budget=None, nav_s=None):
    """simplify_meshes in C (tools/mapx/simpmesh.c): the same groups, jobs, view distances, threshold and output, the
    jobs run on threads in process (no job cache: tools/native/simplify.c is the simplifier either way)"""
    import ctypes
    import numpy as np
    import simplify
    i32, i64, f64 = np.int32, np.int64, np.float64

    def flat(rows, dt, w):
        return np.ascontiguousarray(np.array(rows, dt).reshape(-1, w)) if rows else np.zeros((0, w), dt)
    nm = len(meshes)
    PW = [_mesh_np(m) for m in meshes]
    Ps, Ws = [p for p, _ in PW], [w for _, w in PW]
    Ts = [_np_list(m, "tris", m[src], i32, 3) for m in meshes]
    off = lambda xs: np.concatenate([[0], np.cumsum([len(x) for x in xs])]).astype(i64)
    cat = lambda xs, dt, w: np.ascontiguousarray(np.concatenate(xs).astype(dt)) if xs else np.zeros((0, w), dt)
    P, poff, W, woff, T, toff = cat(Ps, f64, 3), off(Ps), cat(Ws, i32, 1), off(Ws), cat(Ts, i32, 3), off(Ts)
    tids = {}
    ti = np.array([tids.setdefault(m["ti"], len(tids)) for m in meshes] or [0], i32)
    keep = np.array([1 if m["keep"] else 0 for m in meshes] or [0], np.uint8)
    F = flat(sorted(far), i64, 3)
    nv, nt = flat(nav_v, f64, 3), flat(nav_t, i32, 3)
    sp = np.ascontiguousarray(np.array(spawn, f64).reshape(3))
    kp = flat([tuple(k[:3]) for k in KEEP_POS], f64, 3)
    ak = simplify.AREA_K if area_k is None else area_k
    a = lambda x: x.ctypes.data
    if os.environ.get("SIMP_DUMP"):  # debugging: the simplifier's whole input, to rerun / analyse it without the game
        np.savez_compressed(f"{os.environ['SIMP_DUMP']}.{out}.npz", P=P, poff=poff, W=W, woff=woff, T=T, toff=toff,
                            keep=keep, ti=ti, F=F, nv=nv, nt=nt, sp=sp, kp=kp, cells=int(bool(cells)),
                            floor=floor or NEAR, ak=ak, eps=-1.0 if eps_fixed is None else eps_fixed,
                            eps_far=-1.0 if eps_far is None else eps_far, names=np.array([m["name"] for m in meshes]),
                            budget=np.array(budget[:3] if budget else (), f64),
                            nav_s=np.array(nav_s if nav_s is not None else np.zeros((0, 3)), f64))
    t0 = time.time()
    h = MX.mx_simp_new(nm, a(P), a(poff), a(W), a(woff), a(T), a(toff), a(keep), a(ti), 1 if cells else 0, CHUNK_GRID,
                       len(F), a(F), len(nv), a(nv), len(nt), a(nt), a(sp), floor or NEAR, SPAWN_K, len(kp), a(kp),
                       KEEP_R ** 2, KEEP_D, simplify.BORDER_W, simplify.SEAM_W, simplify.SEAM_K * simplify.SEAM_K,
                       ak ** 2, simplify.MIN_COS)
    if not h:
        raise MemoryError("mx_simp_new")
    try:
        st = np.zeros(3, i64)
        MX.mx_simp_stats(h, a(st))
        full, ngroups, njobs = (int(x) for x in st)
        print(f"simplify: {njobs} jobs, run in C in {time.time() - t0:.1f} s", flush=True)
        dup = np.zeros(max(nm, 1), np.uint8)
        MX.mx_simp_dup(h, a(dup))
        for mi in np.flatnonzero(dup[:nm]).tolist():
            meshes[mi][src] = []
        grass = sum(len(m[src]) for m in meshes if m["keep"] and not m["grass"])

        def count(eps):
            return grass + int(MX.mx_simp_count(h, eps))
        print("simplify: tris at an error of " + ", ".join(f"{px} px {count((px / 568) ** 2)}" for px in (1, 2, 3, 4, 6, 8, 12, 24)))
        if eps_fixed is not None or EPS is not None:
            eps = eps_fixed if eps_fixed is not None else EPS
        else:
            lo, hi = 1e-16, 1.0
            while count(hi) > BUDGET and hi < 1e6:
                hi *= 100
            for _ in range(40):
                mid = math.sqrt(lo * hi)
                lo, hi = (lo, mid) if count(mid) <= BUDGET else (mid, hi)
            eps = hi
            if eps > EPS_MAX:  # see simplify_meshes
                print(f"simplify: BUDGET {BUDGET} not reachable ({count(eps)} tris at any error); error capped at "
                      f"{math.sqrt(EPS_MAX) * 568:.0f} px -> {count(EPS_MAX)} tris")
                eps = EPS_MAX
        kind, ck = np.zeros(max(njobs, 1), i32), np.zeros((max(njobs, 1), 3), i64)
        MX.mx_simp_jobs(h, a(kind), a(ck))
        jc = [None if k == 0 else "far" if k == 1 else tuple(c) for k, c in zip(kind[:njobs].tolist(), ck.tolist())]
        job_eps = solve_budget(MX, h, jc, eps, eps_far, budget, nav_s) if budget and cells else None
        if job_eps is not None:
            n = int(MX.mx_simp_replay_jobs(h, job_eps.ctypes.data))
        else:
            n = int(MX.mx_simp_replay(h, eps, eps_far if eps_far is not None else 0.0, 1 if eps_far is not None else 0))
        tj, tm = np.zeros(max(n, 1), i32), np.zeros(max(n, 1), i32)
        cm, cp, cw = (np.zeros(max(3 * n, 1), i32) for _ in range(3))
        MX.mx_simp_fetch(h, a(tj), a(tm), a(cm), a(cp), a(cw))
        final = grass + n if job_eps is not None else count(eps)
    finally:
        MX.mx_simp_free(h)
    # the output as Tris (corner arrays), per mesh in replay order: positions from the group mesh cm, vertex cp
    CP = P[poff[cm[:3 * n]] + cp[:3 * n]].reshape(-1, 3, 3)
    CW = cw[:3 * n].reshape(-1, 3)
    tm, tj = tm[:n], tj[:n]
    order = np.argsort(tm, kind="stable")
    ends = np.cumsum(np.bincount(tm, minlength=nm)).tolist() if n else [0] * nm
    tjl = tj[order].tolist()
    for mi, m in enumerate(meshes):
        if m["keep"]:
            if m[src]:
                m[out] = mapx_native.Tris(np.ascontiguousarray(Ps[mi][Ws[mi][Ts[mi]]]), np.ascontiguousarray(Ts[mi]))
            else:
                m[out] = []
        elif not m[src]:
            m[out] = []
        else:
            a, b = ends[mi - 1] if mi else 0, ends[mi]
            sel = order[a:b]
            m[out] = mapx_native.Tris(np.ascontiguousarray(CP[sel]), np.ascontiguousarray(CW[sel])) if b > a else []
            m[out + "_cell"] = [jc[j] for j in tjl[a:b]]
    print(f"simplify: {full} -> {final} tris ({grass} kept: effects, sky; foliage extra) in {ngroups} welded "
          f"groups / {njobs} jobs, eps {eps:.3g} "
          f"(error <= {math.sqrt(max(eps, 0)) * 568:.1f} px at 640x448)")
    return eps


PVSIN_MAGIC = b"PVI1"


def write_pvsin(path, all_chunks, nav_v, nav_t):
    """Input of the occlusion bake (tools/pvs_bake.c): per chunk in map.bin / dg.bin order its unit, flags and box,
    the units' cell boxes, the navmesh, then the chunks' triangles (world positions, float32). A unit is one MSB part
    within one CHUNK_GRID cell (huge terrain pieces so come in cell-sized clusters); sky chunks are unit 0xFFFFFFFF
    (always drawn). An empty file (header only, 0 chunks) when there is no PVS input: the bake then writes a pvs.bin
    that shows everything.
      header (32 B): magic, chunks, units, tris, nav verts, nav tris, cell table entries, f32 cell size
      per chunk (44 B): u32 unit, flags (bit 0 occluder, bit 1 near level, bit 2 coarse level, neither: kept /
                        always drawn; bits 8-15 map.c chunk flags), first triangle, triangles, cell table entry
                        (0xFFFFFFFF none: sky); f32 box min[3], max[3]
      per unit (40 B): i32 cell key[3], u32 part; f32 box of its cell's chunks min[3], max[3]
      parts: u32 count, NUL-terminated names, padded to 4 bytes
      nav: f32 x3 per vertex, u32 x3 per triangle
      tris: f32 x9 per triangle, in chunk order"""
    T = PVS_TRIS[0]
    if not PVS or T is None or len(PVS_ORDER) != len(all_chunks):
        if PVS:
            print("pvsin.bin: no PVS input (Python chunk path or no streaming): pvs.bin will show everything")
        with open(path, "wb") as f:
            f.write(struct.pack("<4s7I", PVSIN_MAGIC, 0, 0, 0, 0, 0, 0, 0))
        return
    units, parts, crec, cbox = {}, {}, [], {}
    tri_chunks, first = [], 0
    for j, (i, ck) in enumerate(PVS_ORDER):
        ch = all_chunks[j]
        part, occ = CHUNK_PART[i]
        sky = ch[1] & CHUNK_SKY
        if sky or ck is None:
            u = 0xFFFFFFFF
        else:
            key = tuple(ck[0])
            u = units.setdefault((parts.setdefault(part, len(parts)), key), len(units))
            lo, hi = cbox.get(key, ((1e9,) * 3, (-1e9,) * 3))
            cbox[key] = (tuple(min(a, b) for a, b in zip(lo, ch[6])), tuple(max(a, b) for a, b in zip(hi, ch[7])))
        src = np.asarray(CHUNK_SRC[i], np.int64)
        near = ck is not None and ck[1] == 1
        coarse = not near and CHUNK_TAG[i] is not None and CHUNK_TAG[i][0] == 0
        fl = (1 if occ and not ch[1] & (CHUNK_BLEND | CHUNK_GRASS | CHUNK_SKY) else 0) | (2 if near else 0) | \
             (4 if coarse else 0) | (ch[1] & 0xFF) << 8
        ent = CHUNK_ENTRY[j] if j < len(CHUNK_ENTRY) and CHUNK_ENTRY[j] is not None else 0xFFFFFFFF
        crec.append(struct.pack("<5I6f", u, fl, first, len(src), ent, *ch[6], *ch[7]))
        tri_chunks.append(src)
        first += len(src)
    urec = []
    for (pi, key), u in sorted(units.items(), key=lambda kv: kv[1]):
        lo, hi = cbox[key]
        urec.append(struct.pack("<3iI6f", *key, pi, *lo, *hi))
    pn = b"".join(n.encode() + b"\0" for n in parts)
    idx = np.concatenate(tri_chunks) if tri_chunks else np.zeros(0, np.int64)
    with open(path, "wb") as f:
        f.write(struct.pack("<4s6If", PVSIN_MAGIC, len(crec), len(urec), len(idx), len(nav_v), len(nav_t),
                            len(CELL_SIZES), CHUNK_GRID))
        f.write(b"".join(crec))
        f.write(b"".join(urec))
        f.write(struct.pack("<I", len(parts)) + pn + b"\0" * (-(len(pn) + 4) % 4))
        f.write(np.asarray(nav_v, np.float32).reshape(-1, 3).tobytes())
        f.write(np.asarray(nav_t, np.uint32).reshape(-1, 3).tobytes())
        f.write(np.ascontiguousarray(T[idx]).tobytes())
    print(f"pvsin.bin: {len(crec)} chunks, {len(urec)} units of {len(parts)} parts, {len(idx)} tris")


def stream_cells(chunks):
    """STREAM: [(cell key, resident chunk indices (coarse level + kept meshes whose centre lies in the cell, by
    texture), near chunk indices (by texture))] in Z order of the key (cell_zorder); sky chunks belong to no cell."""
    cells = {}
    for i, ch in enumerate(chunks):
        if ch[1] & CHUNK_SKY:
            continue
        t = CHUNK_TAG[i]
        key = tuple(t[1]) if t else tuple(int(math.floor((ch[6][a] + ch[7][a]) / 2 / CHUNK_GRID)) for a in range(3))
        cells.setdefault(key, ([], []))[1 if t and t[0] == 1 else 0].append(i)
    def lev(i):  # kept chunks, then the coarse ones, each by texture: map.c cuts a cell's items by level (streaming v2)
        t = CHUNK_TAG[i]
        return (1 if t and t[0] == 0 else 0, chunks[i][0])
    return [(k, sorted(r, key=lev), sorted(n, key=lambda i: chunks[i][0]))
            for k, (r, n) in sorted(cells.items(), key=lambda kv: cell_zorder(kv[0]))]


def cell_zorder(key):
    """Z order (Morton) of a cell key: the bits of x, z and y interleaved, so cells next to each other on the ground
    sit next to each other in the cell table, mapres.bin and cells.bin (the order of the disc). mapstream reads
    neighbouring resident items in one request (MS_RUN) and a walk reads nearby sectors; sorted by (x, y, z) as before,
    the cells of one x column came first and a neighbour in x was a whole column away (plan step 5, 2026-11-06)."""
    k = 0
    for b in range(10):
        for a in (0, 2, 1):
            k |= ((key[a] + 512) >> b & 1) << (3 * b + (0, 2, 1).index(a))
    return k


CELL_SIZES = []
CHUNK_ENTRY = []  # write_cells: per chunk (map.bin / dg.bin order) its cell's first cell table entry, else None
CELLS_VIFPKT = 1  # cell table word 1, format flags: bit 0 = chunk streams are VIF packets (chunk_streams)
# Near cell blobs over NEAR_PART bytes are written as several parts (consecutive cell table entries, the later ones
# marked CELL_PART in their near count): each part is a mapstream item of its own, so no near cell needs one big hole
# in the arena (Burg near cells of 0.6-2.2 MB found none with 1.2-1.6 MB free: 115 refusals on the ISO route,
# TECH_DEBT world-9). map.c draws a cell's near level once every part is in. Plan step 5, 2026-11-06.
# One part is at most one page of mapstream's arena (MS_PAGE, plan step 6): 256 KB parts (step 5) still found no hole
# of their size with ~450 KB free in the arena; a part of a page fits any free page. A single chunk over a page makes
# a part of its own (it starts on a page).
NEAR_PART = 64 * 1024
NEAR_PARTS_MAX = 64  # map.c NEAR_PARTS
CELL_PART = 0x80000000


def write_cells(cells, res, all_chunks, nres):
    """DSM5 cell table + cells.bin. Table: u32 cells, flags (CELLS_VIFPKT), 0, 0; per cell (48 B) f32 min[3], max[3] (resident + near
    chunks), u32 blob offset, blob bytes (in cells.bin), near chunks, first resident chunk, resident chunks, number in
    bump.bin of the cell's first near chunk.
    cells.bin: per cell one blob (64-byte aligned): near chunk records (as map.bin's, stream offsets relative to the
    blob, flags bits 8-31 = the chunk's number in bump.bin; foliage (CHUNK_GRASS) keeps its tuft bits there, its
    number is the table's first near number + its index in the blob), then the streams (16-byte aligned).
    A cell whose blob would pass NEAR_PART bytes is split into parts of whole chunks (in their order): the first part
    in the cell's own entry, the others in the entries after it with near | CELL_PART, 0 resident chunks and the same
    box; each part is a blob of its own."""
    tab, cbin = bytearray(struct.pack("<4I", len(cells), CELLS_VIFPKT, 0, 0)), bytearray()
    CELL_SIZES.clear()
    CHUNK_ENTRY[:] = [None] * len(all_chunks)
    first, gid = 0, nres
    for key, r, n in cells:
        boxes = [all_chunks[i] for i in range(first, first + len(r))] + \
                [all_chunks[gid + j] for j in range(len(n))]
        mn = [min(b[6][a] for b in boxes) for a in range(3)]
        mx = [max(b[7][a] for b in boxes) for a in range(3)]
        pkts = [chunk_packet(*all_chunks[gid + j][:6]) for j in range(len(n))]

        def size(j):  # bytes chunk gid + j adds to a blob: its record + its packet (VIF code qwords, streams)
            return 64 + len(pkts[j][0])
        parts, cur, cb = [], [], 0
        for j in range(len(n)):
            if cur and cb + size(j) + 63 > NEAR_PART:
                parts.append(cur)
                cur, cb = [], 0
            cur.append(j)
            cb += size(j)
        parts.append(cur)
        assert len(parts) <= NEAR_PARTS_MAX, f"cell {key}: {len(parts)} near parts"
        for i in list(range(first, first + len(r))) + [gid + j for j in range(len(n))]:
            CHUNK_ENTRY[i] = len(CELL_SIZES)  # the cell's own (first) entry, also for its later near parts
        for p, part in enumerate(parts):
            b = bytearray(b"\0" * (64 * len(part)))
            for i, j in enumerate(part):
                ti, fl, ntri, pos, st, col, lo, hi, org, step, sts = all_chunks[gid + j]
                pk, offs, nw = pkts[j]
                offs = [len(b) + o for o in offs]
                b += pk
                assert gid + j < 1 << 24
                b[64 * i:64 * i + 64] = struct.pack("<iII3I6f4f", ti, fl | (CHUNK_ST1K if sts == 1024 else 0) |
                                                    (0 if fl & CHUNK_GRASS else (gid + j) << 8), nw, *offs, *lo, *hi,
                                                    *org, step)
            b.extend(b"\0" * (-len(b) % 64))
            off = len(cbin) if part else 0
            cbin += b if part else b""
            CELL_SIZES.append((key, len(r) if not p else 0, len(part), len(b) if part else 0))
            tab += struct.pack("<6f6I", *mn, *mx, off, len(b) if part else 0, len(part) | (CELL_PART if p else 0),
                               first if not p else first + len(r), len(r) if not p else 0, gid + (part[0] if part else 0))
        first += len(r)
        gid += len(n)
    tab[0:4] = struct.pack("<I", len(CELL_SIZES))
    return bytes(tab), bytes(cbin)


def main():
    game, out = sys.argv[1], sys.argv[2]
    map_id = sys.argv[3] if len(sys.argv) > 3 else "m10_02_00_00"
    preview = sys.argv[4] if len(sys.argv) > 4 else None
    t_start = time.time()
    meshes_in, textures, nav_v, nav_t, spawn, (fog_id, fog), sky_fog = gather(game, map_id)
    import havok_collision
    havok_collision.forget()  # the whole MSB's collision (gather's probes / spawn part); area_collision keeps its own
    if os.environ.get("EXPORT_GC") != "default":
        gc.collect()
        gc.freeze()  # gather's meshes stay alive to the end: no later collection rescans them
    centre = spawn["pos"]
    rss("gather")
    t0 = time.time()
    if GAME_LOD:
        for m in meshes_in:
            m["tris"] = [tuple((m["pos"][m["wpos"][w]], w) for w in t) for t in m["tris"]]
    elif STREAM:  # coarse level first (it must not see the near level's corners), both cut by cell
        prune_foliage(meshes_in, nav_v, nav_t)
        for m in meshes_in:
            m["tris_in"] = m["tris"]
        far = far_cells(meshes_in, nav_v)
        # player positions for the spot budgets (world streaming v2): navmesh vertices, at most 4000
        ws_s = nav_v[::max(1, len(nav_v) // 4000)] if WS_BUDGET else None
        simplify_meshes(meshes_in, spawn["pos"], nav_v, nav_t, src="tris_in", out="tris_far", eps_fixed=EPS_FAR, far=far,
                        floor=STREAM_D, cells=True, eps_far=min(EPS_FAR, BACKDROP_EPS),
                        budget=(WS_R1_IN, WS_R1, WS_B1, "coarse level", WS_PX1) if ws_s else None, nav_s=ws_s)
        rss("simplify coarse")
        # near level: no area term (its few px within NEAR_DRAW_R keep thin parts anyway; with it Burg's near level
        # grew 465k -> 650k tris and its largest cell 2.1 -> 3.2 MB)
        simplify_meshes(meshes_in, spawn["pos"], nav_v, nav_t, src="tris_in", out="tris", cells=True, far=far, area_k=0.0,
                        budget=(0.0, WS_R0, WS_B0, "near level", WS_PX0) if ws_s else None, nav_s=ws_s)
    else:
        prune_foliage(meshes_in, nav_v, nav_t)
        simplify_meshes(meshes_in, spawn["pos"], nav_v, nav_t)
    t1 = time.time()
    rss("simplify")
    if os.environ.get("LOD_REPORT"):  # per mesh: source / near / coarse triangles, size, navmesh distance (debugging)
        nv = np.array(nav_v, dtype=np.float64) if nav_v else np.zeros((1, 3))
        with open(os.environ["LOD_REPORT"], "w") as f:
            f.write("name\tkeep\tgrass\tsrc\tnear\tcoarse\tsize\tnav_d\tcx\tcy\tcz\n")
            for m in meshes_in:
                P = np.array(m["pos"], dtype=np.float64)
                if not len(P):
                    continue
                lo, hi = P.min(0), P.max(0)
                d = np.maximum(0, np.maximum(lo[None] - nv, nv - hi[None]))
                src = len(m.get("tris_in", m["tris"]))
                f.write(f"{m['name']}\t{int(m['keep'])}\t{int(bool(m['grass']))}\t{src}\t{len(m['tris'])}\t"
                        f"{len(m.get('tris_far', m['tris']))}\t{np.linalg.norm(hi - lo):.1f}\t"
                        f"{np.sqrt((d * d).sum(1).min()):.1f}\t" + "\t".join(f"{v:.1f}" for v in (lo + hi) / 2) + "\n")
    # one entry per drawn level of a mesh: STREAM gives simplified meshes a near (level 1) and a coarse (level 0) entry
    # whose triangles carry (level, cell); kept meshes (effects, sky, foliage, KEEP_MODELS) one resident entry
    entries = []
    nav_s = nav_v[::max(1, len(nav_v) // 256)]  # navmesh sample points for the backdrop's back-face test
    for m in meshes_in:
        if STREAM and not m["keep"] and not GAME_LOD:
            # far cells: the near run's triangles are dropped, the coarse ones are always drawn (tcell None)
            tl, cl = m["tris"], m.get("tris_cell", [])
            nt = [i for i, c in zip(range(len(tl)), cl) if c != "far"]
            entries.append(dict(m, tris=_take(tl, nt), tcell=[(1, cl[i]) for i in nt]))
            tl, cl = m.get("tris_far", []), m.get("tris_far_cell", [])
            ct = [i for i, c in zip(range(len(tl)), cl) if c != "far"]
            entries.append(dict(m, tris=_take(tl, ct), tcell=[(0, cl[i]) for i in ct], soft=True))
            far_t = _take(tl, [i for i, c in zip(range(len(tl)), cl) if c == "far"])
            bd = backdrop_visible(m, far_t, nav_s) if BACKDROP else far_t
            if bd:  # far-only cells: one resident level; BACKDROP=1: the backdrop, drawn at any distance (map.c)
                entries.append(dict(m, tris=bd, tcell=[None] * len(bd), soft=True,
                                    flags=m["flags"] | (CHUNK_BACKDROP if BACKDROP else 0)))
        else:
            entries.append(dict(m, tcell=None))
    rss("entries")
    soften_thin([e for e in entries if e.get("soft") and e["tris"]], nav_s, STREAM_D)
    if SOFT_STATS[0]:
        print(f"soften: {SOFT_STATS[1]} of {SOFT_STATS[0]} coarse / backdrop tris under {SOFT_PX} px (colour pulled "
              f"toward their surroundings)")
    if BACKDROP_STATS[0]:
        print(f"backdrop: {BACKDROP_STATS[1]} of {BACKDROP_STATS[0]} far-cell tris kept (back faces from the navmesh "
              f"dropped: BACKDROP_SIDE {BACKDROP_SIDE})")
    MXL = mapx_native.lib()
    flat = None if MXL is not None else flatten(entries, textures)

    # MSB yaw (degrees about +Y, model forward -Z) -> game yaw (forward (sin, cos) on x/z): rotated by 180 degrees
    yaw = math.radians(spawn["rot"][1]) + math.pi

    rss("flatten")
    src = export_normal.SRC
    bkey = export_normal.assign(entries, src.get("dds", {}), src.get("smap"), MAIN_TONE[0], src.get("near", [1] * 3),
                                SHADOW_AMB, src.get("probes", {})) if export_normal.BUMP else None
    rss("bump")
    res = build_chunks_c(MXL, entries, textures, centre, bkey, STREAM, DRAW_GROUPS) if flat is None else None
    if res is None:
        meshes, verts, tris, uvs, fol, tcells, dgs, ents = flat or flatten(entries, textures)
        chunks = build_chunks(meshes, verts, tris, uvs, textures, centre, fol, bkey, tcells if STREAM else None,
                              dgs if DRAW_GROUPS else None, ents)
        res = chunks, len(meshes), len(tris)
    chunks, n_meshes, n_tris = res
    rss("chunks")
    cells = stream_cells(chunks) if STREAM else None
    if STREAM:  # resident chunks grouped by cell, then the near chunks; bump.bin numbers them in this order
        res_order = [i for c in cells for i in c[1]] + [i for i, ch in enumerate(chunks) if ch[1] & CHUNK_SKY]
        order = res_order + [i for c in cells for i in c[2]]
        export_normal.CHUNK_BUMP[:] = [export_normal.CHUNK_BUMP[i] for i in order]
        CHUNK_DG[:] = [CHUNK_DG[i] for i in order]
        CHUNK_ENT[:] = [CHUNK_ENT[i] for i in order]
        if PVS and PVS_TRIS[0] is not None and len(CHUNK_PART) == len(order):
            pvs_cell = {i: (k, lv) for k, r, n in cells for lv, ids in ((0, r), (1, n)) for i in ids}
            PVS_ORDER[:] = [(i, pvs_cell.get(i)) for i in order]
        coarse = {i for i, t in enumerate(CHUNK_TAG) if t and t[0] == 0}
        all_chunks = [chunks[i] for i in order]
        chunks = [chunks[i][:1] + (chunks[i][1] | (CHUNK_COARSE if i in coarse else 0),) + chunks[i][2:]
                  for i in res_order]
    else:
        all_chunks = chunks
    blob = bytearray()

    def align(n):
        blob.extend(b"\0" * ((-(len(blob) + HDR)) % n))
    sec = {}
    # chunk vertex streams first (16-byte aligned for VIF UNPACK via DMA REF), then the chunk table
    recs = []
    for ti, fl, ntri, pos, st, col, lo, hi, org, step, sts in chunks:
        align(16)
        pk, offs, nw = chunk_packet(ti, fl, ntri, pos, st, col)
        offs = [len(blob) + HDR + o for o in offs]
        blob += pk
        recs.append(struct.pack("<iII3I6f4f", ti, fl | (CHUNK_ST1K if sts == 1024 else 0), nw, *offs, *lo, *hi,
                                *org, step))
    sec["chunks"] = len(blob) + HDR
    blob += b"".join(recs)
    align(16)
    sec["textures"] = texmap.write(blob, HDR, textures, all_chunks)  # texture table + CLUTs + mips (tools/texmap.py)
    align(4)
    sec["nav_verts"] = len(blob) + HDR
    for p in nav_v:
        blob += struct.pack("<3f", *p)
    sec["nav_tris"] = len(blob) + HDR
    for t in nav_t:
        blob += struct.pack("<3H", *t)
    align(4)
    sec["fog"] = len(blob) + HDR
    fog_blob, fog_info, gain = fog_section(game, map_id, fog_id)
    blob += fog_blob
    cell_bin = b""
    if STREAM:
        align(16)
        sec["cells"] = len(blob) + HDR
        cell_tab, cell_bin = write_cells(cells, chunks, all_chunks, len(chunks))
        blob += cell_tab
    header = struct.pack("<4s4I4f4I", b"DSM5" if STREAM else b"DSM4", len(chunks), len(textures), len(nav_v), len(nav_t),
                         *spawn["pos"], yaw, sec["chunks"], sec["textures"], sec["nav_verts"], sec["nav_tris"])
    # FogBank: fogBeginZ / fogEndZ metres, degRotW = maximum density % (may exceed 100), colour tone mapped (fog_rgb)
    header += struct.pack("<3f4B", fog["fogBeginZ"], fog["fogEndZ"], fog["degRotW"] / 100.0,
                          *fog_rgb(fog, MAIN_TONE[0]), 0x80)
    # the sky lies beyond its row's fogEndZ: constant fog at the row's maximum density
    header += struct.pack("<4B", *fog_rgb(sky_fog, MAIN_TONE[0]), min(255, int(sky_fog["degRotW"] * 255 / 100))) \
        if sky_fog else b"\0" * 4
    header += struct.pack("<I", sec["fog"])
    header += struct.pack("<I", sec.get("cells", 0))  # DSM5: cell table (write_cells)
    header += b"\0" * (HDR - len(header))
    assert len(header) == HDR
    with open(out, "wb") as f:
        f.write(header + blob)
    with open(os.path.join(os.path.dirname(os.path.abspath(out)), "cells.bin"), "wb") as f:
        f.write(cell_bin)  # empty without STREAM (the pack always has the entry)
    export_normal.write(os.path.join(os.path.dirname(os.path.abspath(out)), "bump.bin"), len(all_chunks))
    import export_collision
    dgn = drawgroups.write(os.path.join(os.path.dirname(os.path.abspath(out)), "dg.bin"), DG, CHUNK_DG,
                           souls.msb1(souls.read(f"{game}/map/MapStudio/{map_id}.msb")),
                           export_collision.area_collision(game, map_id)[0], CHUNK_ENT)
    print(f"dg.bin: {dgn[0]} piece / {dgn[1]} collision draw-group sets, {dgn[3]} switched entities in "
          f"{sum(1 for e in CHUNK_ENT if e)} chunks, {dgn[2]} bytes")
    dsr_light.write_tone(os.path.join(os.path.dirname(os.path.abspath(out)), "fx_tone.json"), MAIN_TONE[0])
    write_pvsin(os.path.join(os.path.dirname(os.path.abspath(out)), "pvsin.bin"), all_chunks, nav_v, nav_t)
    if STREAM:
        nn = sum(len(c[2]) for c in cells)
        print(f"stream: {len(cells)} cells, resident {len(chunks)} chunks / {sum(c[2] for c in chunks)} tris "
              f"({sum(c[2] for c in chunks if c[1] & CHUNK_COARSE)} coarse), near {nn} chunks / "
              f"{sum(all_chunks[i][2] for i in range(len(chunks), len(all_chunks)))} tris, cells.bin {len(cell_bin)} "
              f"bytes, largest cell {max(c[3] for c in CELL_SIZES) if CELL_SIZES else 0} bytes")
        with open(out + ".budget.json", "w") as f:  # tools/stream_budget.py, the PR's numbers
            json.dump(BUDGET_REPORT, f, indent=1)
    print(f"{map_id}: {n_meshes} meshes -> {len(chunks)} chunks, {n_tris} tris, {texmap.stats(textures)}, "
          f"navmesh {len(nav_v)} verts / {len(nav_t)} tris, spawn {spawn['pos']}, "
          f"{len(header) + len(blob)} bytes, {time.time() - t_start:.1f} s (simplify {t1 - t0:.1f} s)")
    print("area rows (fog, light, tone, tone correct, point light, probe, dof): " + ", ".join(
        f"{k} (fog {v['fogBeginZ']}..{v['fogEndZ']} m {v['degRotW']}% rgb {fog_rgb(v, gain[k])}, char amb "
        f"{[round(128 * x) for x in ch[0]]} sun {[round(128 * x) for x in ch[1]]}, player light {ch[3][1]} "
        f"{ch[3][0] / 10}..{ch[3][2] / 10} m, env up {ch[4][0]} down {ch[4][1]} spec {ch[4][2]})" for k, v, ch in fog_info))
    if preview:
        render_preview(preview, out)


# Offline preview viewpoints: name -> (eye, look-at), both walkable spots; "spawn" is the game camera behind the
# player at the spawn (LockCamParam: 4 m back, pivot 1.42 m up, FOV 43 degrees).
VIEWS = {"court": ((-29.7, -53.2, 71.6), (-47.4, -58.8, 53.3)),
         "overlook": ((-24.7, -41.4, 22.4), (-47.4, -58.8, 53.3)),
         "bonfire": ((-49.6, -59.2, 54.2), (-51.8, -60.4, 55.9)),
         "far": ((-42.1, -52.9, 111.4), (-47.0, -63.0, 132.0))}


def load_map_bin(path):
    """map.bin -> (triangles: pos (N,3,3), st (N,3,2), rgba (N,3,4), texture (N), flags (N), foliage rank (N, 0 for
    other triangles: the tuft's rank estimated from its place in the chunk), chunk box (N,6);
    textures [(w, h, palette (256,4) rgb + alpha 0..0x80, indices (h,w))]; spawn (x, y, z, yaw); fog (begin, end, max
    density, rgb, sky rgb, sky density 0..1))."""
    import numpy as np
    b = open(path, "rb").read()
    _, nch, ntex = struct.unpack_from("<4s2I", b, 0)
    spawn = struct.unpack_from("<4f", b, 20)
    och, otex = struct.unpack_from("<2I", b, 36)
    P, ST, C, TI, FL, R, BX = [], [], [], [], [], [], []
    for i in range(nch):
        ti, fl, n, po, so, co = struct.unpack_from("<iII3I", b, och + 64 * i)
        BX.append(np.tile(np.array(struct.unpack_from("<6f", b, och + 64 * i + 24)), (n, 1)))
        org, step = np.array(struct.unpack_from("<3f", b, och + 64 * i + 48)), struct.unpack_from("<f", b, och + 64 * i + 60)[0]
        if fl & CHUNK_GRASS:
            per = max(1, (fl >> 8) & 255)
            lo, hi = (fl >> 16 & 255) / 255, (fl >> 24) / 255
            R.append(lo + (hi - lo) * (np.arange(n) // per + 0.5) / max(1, n // per))
        else:
            R.append(np.zeros(n))
        P.append(org + step * np.frombuffer(b, np.int16, n * 9, po).reshape(n, 3, 3).astype(np.float32))
        ST.append(np.frombuffer(b, np.uint16, n * 6, so).reshape(n, 3, 2) / (1024.0 if fl & CHUNK_ST1K else 4096.0))
        C.append(np.frombuffer(b, np.uint8, n * 12, co).reshape(n, 3, 4))
        TI.append(np.full(n, ti))
        FL.append(np.full(n, fl))
    tex = texmap.load(b, ntex, otex)
    fb, fe, fm = struct.unpack_from("<3f", b, 52)
    fc, sc = struct.unpack_from("<4B", b, 64), struct.unpack_from("<4B", b, 68)
    fog = (fb, fe, fm, fc[:3], sc[:3], sc[3] / 255.0)
    return (np.concatenate(P), np.concatenate(ST), np.concatenate(C), np.concatenate(TI), np.concatenate(FL),
            np.concatenate(R), np.concatenate(BX)), \
        tex, spawn, fog


def render_view(path, geo, tex, eye, target, W=960, H=540, fov_y=43.0, fog=None, zout=None):
    """Z-buffered, perspective-correct textured software render like the PS2: CLUT textures x vertex light, alpha
    test on opaque chunks, back faces culled where the chunk flag says so, near plane 0.1 m; blended chunks after the
    opaque ones, far to near per pixel (alpha or additive, Z tested, not written); fog from load_map_bin's fog (sky
    chunks: the sky's constant fog)."""
    import numpy as np
    from PIL import Image
    P, ST, C, TI, FL, R, BX = geo
    eye = np.array(eye, np.float64)
    # foliage density by chunk distance, like map.c
    d = np.sqrt((np.maximum(0, np.maximum(BX[:, :3] - eye, eye - BX[:, 3:])) ** 2).sum(1))
    show = ~(FL & CHUNK_GRASS).astype(bool) | (R < np.minimum(1, (GRASS_D0 / np.maximum(d, 1e-3)) ** 2))
    P, ST, C, TI, FL = P[show], ST[show], C[show], TI[show], FL[show]
    fwd = np.array(target, np.float64) - eye
    fwd /= np.linalg.norm(fwd)
    right = np.array([fwd[2], 0.0, -fwd[0]])
    right /= np.linalg.norm(right)
    up = np.cross(fwd, right)
    f = H / 2 / math.tan(math.radians(fov_y) / 2)
    d = P.astype(np.float64) - eye
    V = np.stack([d @ right, d @ up, d @ fwd], -1)  # (N,3,3) camera space
    near = 0.1
    UV = ST.astype(np.float64)
    COL = C.astype(np.float64) / 128
    front = (V[..., 2] > near)
    allin = front.all(1)
    # triangles crossing the near plane: clip (Sutherland-Hodgman) and fan
    ext_v, ext_uv, ext_c, ext_i = [], [], [], []
    for i in np.where(front.any(1) & ~allin)[0]:
        poly = []
        for k in range(3):
            a, b = k, (k + 1) % 3
            za, zb = V[i, a, 2], V[i, b, 2]
            if za > near:
                poly.append((V[i, a], UV[i, a], COL[i, a]))
            if (za > near) != (zb > near):
                t = (near - za) / (zb - za)
                poly.append(tuple(x[a] + t * (x[b] - x[a]) for x in (V[i], UV[i], COL[i])))
        for k in range(1, len(poly) - 1):
            tri = (poly[0], poly[k], poly[k + 1])
            ext_v.append([x[0] for x in tri])
            ext_uv.append([x[1] for x in tri])
            ext_c.append([x[2] for x in tri])
            ext_i.append(i)
    idx = np.concatenate([np.where(allin)[0], np.array(ext_i, dtype=np.int64)])
    V = np.concatenate([V[allin], np.array(ext_v).reshape(-1, 3, 3)])
    UV = np.concatenate([UV[allin], np.array(ext_uv).reshape(-1, 3, 2)])
    COL = np.concatenate([COL[allin], np.array(ext_c).reshape(-1, 3, 4)])
    TI, FL = TI[idx], FL[idx]
    sx = W / 2 + f * V[..., 0] / V[..., 2]
    sy = H / 2 - f * V[..., 1] / V[..., 2]
    cr = (sx[:, 1] - sx[:, 0]) * (sy[:, 2] - sy[:, 0]) - (sy[:, 1] - sy[:, 0]) * (sx[:, 2] - sx[:, 0])
    keep = (np.abs(cr) > 1e-9) & ~((FL & 1).astype(bool) & (cr > 0))  # negative screen cross = front (DS winding)
    x0, x1 = np.floor(sx.min(1)), np.ceil(sx.max(1))
    y0, y1 = np.floor(sy.min(1)), np.ceil(sy.max(1))
    keep &= (x1 >= 0) & (x0 < W) & (y1 >= 0) & (y0 < H)
    x0, y0 = np.maximum(x0, 0), np.maximum(y0, 0)
    x1, y1 = np.minimum(x1, W), np.minimum(y1, H)
    size = np.maximum(x1 - x0, y1 - y0)
    # textures in one flat index array + palettes (rgb, alpha 0..0x80)
    offs, flat, pal = [], [], np.zeros((len(tex) + 1, 256, 4), np.float32)
    o = 0
    for k, (w, h, p, ix) in enumerate(tex):
        offs.append(o)
        flat.append(ix.ravel())
        o += w * h
        pal[k] = p
    flat = np.concatenate(flat) if flat else np.zeros(1, np.uint8)
    tw = np.array([t[0] for t in tex] + [1])
    th = np.array([t[1] for t in tex] + [1])
    offs = np.array(offs + [0])

    def fragments(mask):
        """(pixel, z, rgb, alpha 0..1, triangle flags) for the triangles in mask, in size buckets"""
        s = 1
        while True:
            sel = np.where(mask & (size <= s) & ((size > s // 2) if s > 1 else True))[0]
            if len(sel):
                oy, ox = np.mgrid[0:s, 0:s]
                ox, oy = ox.ravel(), oy.ravel()
                step = max(1, 2_000_000 // (s * s))
                for c0 in range(0, len(sel), step):
                    t = sel[c0:c0 + step]
                    px = (x0[t, None] + ox[None, :])
                    py = (y0[t, None] + oy[None, :])
                    cx, cy = px + 0.5, py + 0.5
                    ax, ay = sx[t], sy[t]
                    l0 = ((ax[:, 1, None] - cx) * (ay[:, 2, None] - cy) - (ay[:, 1, None] - cy) * (ax[:, 2, None] - cx)) / cr[t, None]
                    l1 = ((ax[:, 2, None] - cx) * (ay[:, 0, None] - cy) - (ay[:, 2, None] - cy) * (ax[:, 0, None] - cx)) / cr[t, None]
                    l2 = 1 - l0 - l1
                    m = (l0 >= 0) & (l1 >= 0) & (l2 >= 0) & (px < x1[t, None]) & (py < y1[t, None])
                    ti_, fi = np.nonzero(m)
                    if not len(ti_):
                        continue
                    tt = t[ti_]
                    b0, b1, b2 = l0[ti_, fi], l1[ti_, fi], l2[ti_, fi]
                    iz = V[tt, 0, 2] ** -1 * b0 + V[tt, 1, 2] ** -1 * b1 + V[tt, 2, 2] ** -1 * b2
                    w0, w1, w2 = b0 / V[tt, 0, 2] / iz, b1 / V[tt, 1, 2] / iz, b2 / V[tt, 2, 2] / iz
                    u = UV[tt, 0, 0] * w0 + UV[tt, 1, 0] * w1 + UV[tt, 2, 0] * w2
                    v = UV[tt, 0, 1] * w0 + UV[tt, 1, 1] * w1 + UV[tt, 2, 1] * w2
                    k = TI[tt]
                    k = np.where(k < 0, len(tex), k)
                    tx = np.floor(u * tw[k]).astype(np.int64) % tw[k]
                    ty = np.floor(v * th[k]).astype(np.int64) % th[k]
                    texel = np.where(k < len(tex), flat[np.minimum(offs[k] + ty * tw[k] + tx, len(flat) - 1)], 0)
                    fl = FL[tt]
                    textured = k < len(tex)
                    tc = np.where(textured[:, None], pal[k, texel], np.array([150, 150, 150, 128.0]))
                    vc = COL[tt, 0] * b0[:, None] + COL[tt, 1] * b1[:, None] + COL[tt, 2] * b2[:, None]
                    col = tc[:, :3] * vc[:, :3]
                    alpha = tc[:, 3] / 128 * vc[:, 3]
                    ok = (fl & 2).astype(bool) | (texel != 255) | ~textured  # alpha test on opaque cut-outs
                    z = 1 / iz
                    if fog:
                        fb, fe, fm, fc, sc, sd = fog
                        dens = np.where(fl & 8, sd, fm * np.clip((z - fb) / max(fe - fb, 1e-6), 0, 1))
                        fcol = np.where((fl & 8)[:, None].astype(bool), np.array(sc, float), np.array(fc, float))
                        col = col * (1 - dens[:, None]) + fcol * dens[:, None]
                    pix = (py[ti_, fi] * W + px[ti_, fi]).astype(np.int64)
                    yield pix[ok], z[ok], col[ok], alpha[ok], fl[ok]
            if s >= max(W, H):
                break
            s *= 2

    zbuf = np.full(W * H, np.inf)
    cbuf = np.zeros((W * H, 3), np.float32)
    blend = (FL & 2).astype(bool)
    for pix, z, col, _, _ in fragments(keep & ~blend):
        if not len(pix):
            continue
        o = np.lexsort((z, pix))
        pix, z, col = pix[o], z[o], col[o]
        first = np.r_[True, pix[1:] != pix[:-1]]
        pix, z, col = pix[first], z[first], col[first]
        upd = z < zbuf[pix]
        zbuf[pix[upd]] = z[upd]
        cbuf[pix[upd]] = col[upd]
    cbuf[np.isinf(zbuf)] = (190, 200, 210)
    if zout is not None:  # opaque depth per pixel (sky > SKY_DIST), for hole searches
        zout.append(zbuf.reshape(H, W).copy())
    # blended: Z tested against the opaque result, applied far to near per pixel in layers
    fr = [x for x in fragments(keep & blend)]
    if fr:
        pix, z, col, alpha, fl = (np.concatenate([x[i] for x in fr]) for i in range(5))
        vis = z < zbuf[pix]
        pix, z, col, alpha, fl = pix[vis], z[vis], col[vis], alpha[vis], fl[vis]
        o = np.lexsort((-z, pix))
        pix, col, alpha, fl = pix[o], col[o], alpha[o], fl[o]
        start = np.r_[True, pix[1:] != pix[:-1]]
        grp = np.cumsum(start) - 1
        rank = np.arange(len(pix)) - np.flatnonzero(start)[grp]
        for r in range(int(rank.max()) + 1 if len(rank) else 0):
            sel = rank == r
            p, c, a, add = pix[sel], col[sel], np.minimum(alpha[sel], 2)[:, None], (fl[sel] & 4).astype(bool)[:, None]
            cbuf[p] = np.where(add, cbuf[p] + c * a, cbuf[p] + (c - cbuf[p]) * a)
    Image.fromarray(np.clip(cbuf, 0, 255).astype(np.uint8).reshape(H, W, 3)).save(path)


def render_preview(prefix, bin_path):
    """<prefix>_spawn.png, <prefix>_court.png, <prefix>_overlook.png from a map.bin."""
    geo, tex, (x, y, z, yaw), fog = load_map_bin(bin_path)
    piv = (x, y + 1.42, z)
    eye = (x - math.sin(yaw) * 4.0, y + 1.42 + 4.0 * math.sin(0.25), z - math.cos(yaw) * 4.0)
    views = dict(spawn=(eye, (piv[0] + math.sin(yaw), piv[1], piv[2] + math.cos(yaw))), **VIEWS)
    for name, (e, t) in views.items():
        render_view(f"{prefix}_{name}.png", geo, tex, e, t, fog=fog)


if __name__ == "__main__":
    if sys.argv[1] == "--render":  # export_map.py --render map.bin prefix: previews of an existing map.bin
        render_preview(sys.argv[3], sys.argv[2])
    else:
        main()
