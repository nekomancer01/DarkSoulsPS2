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
  fog     u32 rows, collision tris, sky rows, 0; rows (48 B, fog_section docstring: FogBank row, character light, player
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
import mapcells
import mapcfg
import mapchunks
import mapgather
import maplight
import mapsimp
import mapview
# the modules' names, read through export_map as before (tools/mapx/test_mapx.py, export_model.quantize_rgba, ...);
# settings are set on mapcfg (mapcfg.X = v), which every module reads at call time
for _m in (mapcells, mapcfg, mapchunks, mapgather, maplight, mapsimp, mapview):
    globals().update({k: v for k, v in vars(_m).items() if not k.startswith('__')})


def main():
    game, out = sys.argv[1], sys.argv[2]
    map_id = sys.argv[3] if len(sys.argv) > 3 else "m10_02_00_00"
    pass  # (moved to mapcfg)
    block = os.environ.get("AREA_BLOCK") or map_id
    b_room, b_near, b_row = residency.block_budget(block)
    if mapcfg.WS_UNION and mapcfg.STREAM and os.environ.get("AREA_BLOCK") and not b_row and not os.environ.get("WS_ROOM"):
        raise SystemExit(f"export_map: world block {block} has no RES_BLOCK row in ps2/proto/src/residency.h")
    mapcfg.WS_ROOM = mapcfg.WS_ROOM or float(b_room)
    mapcfg.WS_NEAR_KB = mapcfg.WS_NEAR_KB or float(b_near)
    print(f"budget: {block}: room {mapcfg.WS_ROOM:.0f} KB, near {mapcfg.WS_NEAR_KB:.0f} KB "
          f"({'environment' if os.environ.get('WS_ROOM') else 'residency.h ' + (block if b_row else 'default')})")
    preview = sys.argv[4] if len(sys.argv) > 4 else None
    t_start = time.time()
    meshes_in, textures, nav_v, nav_t, spawn, (fog_id, fog), sky_fog = mapgather.gather(game, map_id)
    import havok_collision
    havok_collision.forget()  # the whole MSB's collision (gather's probes / spawn part); area_collision keeps its own
    if os.environ.get("EXPORT_GC") != "default":
        gc.collect()
        gc.freeze()  # gather's meshes stay alive to the end: no later collection rescans them
    centre = spawn["pos"]
    mapchunks.rss("gather")
    t0 = time.time()
    if mapcfg.GAME_LOD:
        for m in meshes_in:
            m["tris"] = [tuple((m["pos"][m["wpos"][w]], w) for w in t) for t in m["tris"]]
    elif mapcfg.STREAM:  # coarse level first (it must not see the near level's corners), both cut by cell
        mapsimp.prune_foliage(meshes_in, nav_v, nav_t)
        if mapsimp.CUT_CELLS:
            mapsimp.cut_cells(meshes_in)
            mapchunks.rss("cut cells")
        for m in meshes_in:
            m["tris_in"] = m["tris"]
        far = mapsimp.far_cells(meshes_in, nav_v)
        # player positions for the spot budgets (world streaming v2): navmesh vertices, at most 4000
        ws_s = nav_v[::max(1, len(nav_v) // 4000)] if mapcfg.WS_BUDGET else None
        if ws_s and mapcfg.WS_UNION:  # the shared visual room (WS_UNION): per spot, this block's share
            ws_s = list(ws_s) + mapsimp.ws_foreign_spots(game, map_id, nav_v)  # and the other MSBs' spots this block reaches
            Ps = np.asarray(ws_s, np.float64).reshape(-1, 3)
            ws_bc = (mapcfg.WS_NEAR_R, mapcfg.WS_PIN_R, mapsimp.ws_shares(Ps, mapcfg.WS_PIN_R, game, map_id, nav_v) * (mapcfg.WS_ROOM - mapcfg.WS_NEAR_KB - mapcfg.WS_FAR_KB) * 1024,
                     "coarse level", mapcfg.WS_PX1)
            ws_bn = (0.0, mapcfg.WS_NEAR_R, mapsimp.ws_shares(Ps, mapcfg.WS_NEAR_R, game, map_id, nav_v) * mapcfg.WS_NEAR_KB * 1024, "near level", mapcfg.WS_PX0)
        elif ws_s:
            ws_bc = (mapcfg.WS_R1_IN, mapcfg.WS_R1, mapcfg.WS_B1, "coarse level", mapcfg.WS_PX1)
            ws_bn = (0.0, mapcfg.WS_R0, mapcfg.WS_B0, "near level", mapcfg.WS_PX0)
        mapsimp.simplify_meshes(meshes_in, spawn["pos"], nav_v, nav_t, src="tris_in", out="tris_far", eps_fixed=mapcfg.EPS_FAR, far=far,
                        floor=mapcfg.STREAM_D, cells=True, eps_far=min(mapcfg.EPS_FAR, mapcfg.BACKDROP_EPS),
                        budget=ws_bc if ws_s else None, nav_s=ws_s, env_pad=mapcfg.SIMPLIFY_ENV_FAR)
        mapchunks.rss("simplify coarse")
        # near level: no area term (its few px within NEAR_DRAW_R keep thin parts anyway; with it Burg's near level
        # grew 465k -> 650k tris and its largest cell 2.1 -> 3.2 MB)
        mapsimp.simplify_meshes(meshes_in, spawn["pos"], nav_v, nav_t, src="tris_in", out="tris", cells=True, far=far, area_k=0.0,
                        budget=ws_bn if ws_s else None, nav_s=ws_s)
    else:
        mapsimp.prune_foliage(meshes_in, nav_v, nav_t)
        mapsimp.simplify_meshes(meshes_in, spawn["pos"], nav_v, nav_t)
    t1 = time.time()
    mapchunks.rss("simplify")
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
        if mapcfg.STREAM and not m["keep"] and not mapcfg.GAME_LOD:
            # far cells: the near run's triangles are dropped, the coarse ones are always drawn (tcell None)
            tl, cl = m["tris"], m.get("tris_cell", [])
            nt = [i for i, c in zip(range(len(tl)), cl) if c != "far"]
            entries.append(dict(m, tris=mapsimp._take(tl, nt), tcell=[(1, cl[i]) for i in nt], wrgb=m.get("tris_wrgb", m["wrgb"])))
            tl, cl = m.get("tris_far", []), m.get("tris_far_cell", [])
            ct = [i for i, c in zip(range(len(tl)), cl) if c != "far"]
            wf = m.get("tris_far_wrgb", m["wrgb"])  # the coarse level's refit colours (SIMPLIFY_REFIT)
            entries.append(dict(m, tris=mapsimp._take(tl, ct), tcell=[(0, cl[i]) for i in ct], soft=True, wrgb=wf))
            far_t = mapsimp._take(tl, [i for i, c in zip(range(len(tl)), cl) if c == "far"])
            bd = mapsimp.backdrop_visible(m, far_t, nav_s) if mapsimp.BACKDROP else far_t
            if bd:  # far-only cells: one resident level; BACKDROP=1: the backdrop, drawn at any distance (map.c)
                entries.append(dict(m, tris=bd, tcell=[None] * len(bd), soft=True, wrgb=wf,
                                    flags=m["flags"] | (mapcfg.CHUNK_BACKDROP if mapsimp.BACKDROP else 0)))
        else:
            entries.append(dict(m, tcell=None))
    mapchunks.rss("entries")
    mapsimp.soften_thin([e for e in entries if e.get("soft") and e["tris"]], nav_s, mapcfg.STREAM_D)
    if mapsimp.SOFT_STATS[0]:
        print(f"soften: {mapsimp.SOFT_STATS[1]} of {mapsimp.SOFT_STATS[0]} coarse / backdrop tris under {mapcfg.SOFT_PX} px (colour pulled "
              f"toward their surroundings)")
    if mapsimp.BACKDROP_STATS[0]:
        print(f"backdrop: {mapsimp.BACKDROP_STATS[1]} of {mapsimp.BACKDROP_STATS[0]} far-cell tris kept (back faces from the navmesh "
              f"dropped: BACKDROP_SIDE {mapsimp.BACKDROP_SIDE})")
    MXL = mapx_native.lib()
    flat = None if MXL is not None else mapchunks.flatten(entries, textures)

    # MSB yaw (degrees about +Y, model forward -Z) -> game yaw (forward (sin, cos) on x/z): rotated by 180 degrees
    yaw = math.radians(spawn["rot"][1]) + math.pi

    mapchunks.rss("flatten")
    src = export_normal.SRC
    bkey = export_normal.assign(entries, src.get("dds", {}), src.get("smap"), mapcfg.MAIN_TONE[0], src.get("near", [1] * 3),
                                mapcfg.SHADOW_AMB, src.get("probes", {})) if export_normal.BUMP else None
    mapchunks.rss("bump")
    res = mapchunks.build_chunks_c(MXL, entries, textures, centre, bkey, mapcfg.STREAM, mapcfg.DRAW_GROUPS) if flat is None else None
    if res is None:
        meshes, verts, tris, uvs, fol, tcells, dgs, ents = flat or mapchunks.flatten(entries, textures)
        chunks = mapchunks.build_chunks(meshes, verts, tris, uvs, textures, centre, fol, bkey, tcells if mapcfg.STREAM else None,
                              dgs if mapcfg.DRAW_GROUPS else None, ents)
        res = chunks, len(meshes), len(tris)
    chunks, n_meshes, n_tris = res
    mapchunks.rss("chunks")
    cells = mapcells.stream_cells(chunks) if mapcfg.STREAM else None
    if mapcfg.STREAM:  # resident chunks grouped by cell, then the near chunks; bump.bin numbers them in this order
        res_order = [i for c in cells for i in c[1]] + [i for i, ch in enumerate(chunks) if ch[1] & mapcfg.CHUNK_SKY]
        order = res_order + [i for c in cells for i in c[2]]
        export_normal.CHUNK_BUMP[:] = [export_normal.CHUNK_BUMP[i] for i in order]
        mapchunks.CHUNK_DG[:] = [mapchunks.CHUNK_DG[i] for i in order]
        mapchunks.CHUNK_ENT[:] = [mapchunks.CHUNK_ENT[i] for i in order]
        if mapcfg.PVS and mapchunks.PVS_TRIS[0] is not None and len(mapchunks.CHUNK_PART) == len(order):
            pvs_cell = {i: (k, lv) for k, r, n in cells for lv, ids in ((0, r), (1, n)) for i in ids}
            mapchunks.PVS_ORDER[:] = [(i, pvs_cell.get(i)) for i in order]
        coarse = {i for i, t in enumerate(mapchunks.CHUNK_TAG) if t and t[0] == 0}
        all_chunks = [chunks[i] for i in order]
        chunks = [chunks[i][:1] + (chunks[i][1] | (mapcfg.CHUNK_COARSE if i in coarse else 0),) + chunks[i][2:]
                  for i in res_order]
    else:
        all_chunks = chunks
    blob = bytearray()

    def align(n):
        blob.extend(b"\0" * ((-(len(blob) + mapcfg.HDR)) % n))
    sec = {}
    # chunk vertex streams first (16-byte aligned for VIF UNPACK via DMA REF), then the chunk table
    recs = []
    mapcells.RES_SIZE.clear()
    for ti, fl, ntri, pos, st, col, lo, hi, org, step, sts in chunks:
        align(16)
        pk, offs, nw = mapchunks.chunk_packet(ti, fl, ntri, pos, st, col)
        mapcells.RES_SIZE.append(len(pk) + 64 + (-len(pk) % 16))  # packet + its chunk record (cells sidecar, world budget)
        offs = [len(blob) + mapcfg.HDR + o for o in offs]
        blob += pk
        recs.append(struct.pack("<iII3I6f4f", ti, fl | (mapcfg.CHUNK_ST1K if sts == 1024 else 0), nw, *offs, *lo, *hi,
                                *org, step))
    sec["chunks"] = len(blob) + mapcfg.HDR
    blob += b"".join(recs)
    align(16)
    sec["textures"] = texmap.write(blob, mapcfg.HDR, textures, all_chunks)  # texture table + CLUTs + mips (tools/texmap.py)
    align(4)
    sec["nav_verts"] = len(blob) + mapcfg.HDR
    for p in nav_v:
        blob += struct.pack("<3f", *p)
    sec["nav_tris"] = len(blob) + mapcfg.HDR
    for t in nav_t:
        blob += struct.pack("<3H", *t)
    align(4)
    sec["fog"] = len(blob) + mapcfg.HDR
    fog_blob, fog_info, gain = maplight.fog_section(game, map_id, fog_id)
    # sky rows after the per-triangle bytes: f32 begin, end, max density; u8 r, g, b, 0 (as the header's fog)
    sky_rows = b"".join(struct.pack("<3f4B", v["fogBeginZ"], v["fogEndZ"], v["degRotW"] / 100.0,
                                    *maplight.fog_rgb(v, mapcfg.MAIN_TONE[0]), 0)
                        for v in (maplight.draw_params(game, map_id[:3])[1][r] for r in mapcfg.SKY_FOG_ROWS))
    fog_blob = fog_blob[:8] + struct.pack("<I", len(mapcfg.SKY_FOG_ROWS)) + fog_blob[12:] + b"\0" * (-len(fog_blob) % 4) + \
        sky_rows
    blob += fog_blob
    cell_bin = b""
    if mapcfg.STREAM:
        align(16)
        sec["cells"] = len(blob) + mapcfg.HDR
        cell_tab, cell_bin = mapcells.write_cells(cells, chunks, all_chunks, len(chunks))
        blob += cell_tab
    header = struct.pack("<4s4I4f4I", b"DSM5" if mapcfg.STREAM else b"DSM4", len(chunks), len(textures), len(nav_v), len(nav_t),
                         *spawn["pos"], yaw, sec["chunks"], sec["textures"], sec["nav_verts"], sec["nav_tris"])
    # FogBank: fogBeginZ / fogEndZ metres, degRotW = maximum density % (may exceed 100), colour tone mapped (fog_rgb)
    header += struct.pack("<3f4B", fog["fogBeginZ"], fog["fogEndZ"], fog["degRotW"] / 100.0,
                          *maplight.fog_rgb(fog, mapcfg.MAIN_TONE[0]), 0x80)
    # the sky lies beyond its row's fogEndZ: constant fog at the row's maximum density
    header += struct.pack("<4B", *maplight.fog_rgb(sky_fog, mapcfg.MAIN_TONE[0]), min(255, int(sky_fog["degRotW"] * 255 / 100))) \
        if sky_fog else b"\0" * 4
    header += struct.pack("<I", sec["fog"])
    header += struct.pack("<I", sec.get("cells", 0))  # DSM5: cell table (write_cells)
    header += b"\0" * (mapcfg.HDR - len(header))
    assert len(header) == mapcfg.HDR
    with open(out, "wb") as f:
        f.write(header + blob)
    with open(os.path.join(os.path.dirname(os.path.abspath(out)), "cells.bin"), "wb") as f:
        f.write(cell_bin)  # empty without STREAM (the pack always has the entry)
    export_normal.write(os.path.join(os.path.dirname(os.path.abspath(out)), "bump.bin"), len(all_chunks))
    import export_collision
    dgn = drawgroups.write(os.path.join(os.path.dirname(os.path.abspath(out)), "dg.bin"), mapchunks.DG, mapchunks.CHUNK_DG,
                           souls.msb1(souls.read(f"{game}/map/MapStudio/{map_id}.msb")),
                           export_collision.area_collision(game, map_id)[0], mapchunks.CHUNK_ENT)
    print(f"dg.bin: {dgn[0]} piece / {dgn[1]} collision draw-group sets, {dgn[3]} switched entities in "
          f"{sum(1 for e in mapchunks.CHUNK_ENT if e)} chunks, {dgn[2]} bytes")
    dsr_light.write_tone(os.path.join(os.path.dirname(os.path.abspath(out)), "fx_tone.json"), mapcfg.MAIN_TONE[0])
    mapcells.write_pvsin(os.path.join(os.path.dirname(os.path.abspath(out)), "pvsin.bin"), all_chunks, nav_v, nav_t)
    if mapcfg.STREAM:
        nn = sum(len(c[2]) for c in cells)
        print(f"stream: {len(cells)} cells, resident {len(chunks)} chunks / {sum(c[2] for c in chunks)} tris "
              f"({sum(c[2] for c in chunks if c[1] & mapcfg.CHUNK_COARSE)} coarse), near {nn} chunks / "
              f"{sum(all_chunks[i][2] for i in range(len(chunks), len(all_chunks)))} tris, cells.bin {len(cell_bin)} "
              f"bytes, largest cell {max(c[3] for c in mapcells.CELL_SIZES) if mapcells.CELL_SIZES else 0} bytes")
        with open(out + ".budget.json", "w") as f:  # the spot budget solve's results
            json.dump(mapsimp.BUDGET_REPORT, f, indent=1)
        # the block's budget is a promise to the runtime (residency.h): a level whose own spots stay over it after the
        # solve fails the export
        bad = [f"{r['level']} {r['own_over_pct']} % of {r['own_spots']} spots over {r['budget_kb']} KB"
               for r in mapsimp.BUDGET_REPORT if mapcfg.WS_UNION and r.get("own_over_pct", 0) > mapcfg.WS_OVER_PCT]
        for r in mapsimp.BUDGET_REPORT:
            print(f"budget {r['level']}: {r.get('own_over_pct', 0)} % of the block's own spots over its budget "
                  f"(at most {mapcfg.WS_OVER_PCT:.0f} %), seam allowance {r['allow_pct']} %")
        if bad:
            raise SystemExit(f"export_map: over the block's budget after the solve: {'; '.join(bad)} "
                             f"(residency.h RES_BLOCK, or raise the caps WS_PX0 / WS_PX1)")
        # per cell bytes by level, the resident level's other items and the navmesh spots: the world budget check
        # across blocks (tools/world_budget.py)
        sky = sum(mapcells.RES_SIZE[i] for i in range(len(chunks)) if i < len(mapcells.RES_SIZE) and chunks[i][1] & mapcfg.CHUNK_SKY)
        sp = nav_v[::max(1, len(nav_v) // 4000)]
        with open(out + ".cells.json", "w") as f:
            json.dump(dict(cells=[[round(v, 2) if isinstance(v, float) else v for v in c] for c in mapcells.CELL_BUDGET],
                           sky=sky, spots=[[round(v, 2) for v in q] for q in sp]), f, separators=(",", ":"))
    print(f"{map_id}: {n_meshes} meshes -> {len(chunks)} chunks, {n_tris} tris, {texmap.stats(textures)}, "
          f"navmesh {len(nav_v)} verts / {len(nav_t)} tris, spawn {spawn['pos']}, "
          f"{len(header) + len(blob)} bytes, {time.time() - t_start:.1f} s (simplify {t1 - t0:.1f} s)")
    print("area rows (fog, light, tone, tone correct, point light, probe, dof): " + ", ".join(
        f"{k} (fog {v['fogBeginZ']}..{v['fogEndZ']} m {v['degRotW']}% rgb {maplight.fog_rgb(v, gain[k])}, char amb "
        f"{[round(128 * x) for x in ch[0]]} sun {[round(128 * x) for x in ch[1]]}, player light {ch[3][1]} "
        f"{ch[3][0] / 10}..{ch[3][2] / 10} m, env up {ch[4][0]} down {ch[4][1]} spec {ch[4][2]})" for k, v, ch in fog_info))
    if preview:
        mapview.render_preview(preview, out)


# Offline preview viewpoints: name -> (eye, look-at), both walkable spots; "spawn" is the game camera behind the
# player at the spawn (LockCamParam: 4 m back, pivot 1.42 m up, FOV 43 degrees).
VIEWS = {"court": ((-29.7, -53.2, 71.6), (-47.4, -58.8, 53.3)),
         "overlook": ((-24.7, -41.4, 22.4), (-47.4, -58.8, 53.3)),
         "bonfire": ((-49.6, -59.2, 54.2), (-51.8, -60.4, 55.9)),
         "far": ((-42.1, -52.9, 111.4), (-47.0, -63.0, 132.0))}


if __name__ == "__main__":
    if sys.argv[1] == "--render":  # export_map.py --render map.bin prefix: previews of an existing map.bin
        mapview.render_preview(sys.argv[3], sys.argv[2])
    else:
        main()
