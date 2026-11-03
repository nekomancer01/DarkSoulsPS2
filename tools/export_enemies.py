"""Enemy placements of a map (MSB enemy parts, type 2) for the PS2 prototype: enemies.bin in the area pack.

Layout (little-endian):
  header  "DSRN", u32 count
  entry   char model[8] ("c2900"), i32 npc param, i32 think param, i32 entity id, f32 x, y, z, yaw     (36 B)
  navmesh (after the entries; older runtimes stop at count): the map's MSB navmesh parts (type 8) placing the
          map/<map>/<map>.nvmbnd NVM models, triangles within NAV_REACH of a non-c0000 placement and inside the area's
          region, vertices welded (NAV_WELD) so the parts connect, for enemy pathfinding (ps2/proto/src/nav.c)
          "NAV1", u32 verts, tris, grid w, grid h, f32 origin x, y, z, f32 grid x0, z0 (low corner), f32 grid cell
          (m), u32 list entries                                                                              (48 B)
          vert   i16 x, y, z: (position - origin) * NAV_Q                                                  (6 B)
          tri    u16 v[3], u16 nbr[3] (triangle across edge v[i] -> v[i+1], 0xFFFF none)                  (12 B)
          grid   u16 start[w * h + 1] into list, u16 list[] (the triangles whose x/z box meets the cell), padded
                 to 4 B
          (the NVM triangle flags are not kept: their meaning is not decoded, TECH_DEBT enemy-2)
yaw: radians in the game's convention (forward = (sin yaw, 0, cos yaw)) = MSB rot.y + pi, as the map's player spawn.
MSB enemy fields (think_id, npc_id, entity_id) come from souls.msb1. c0000 parts (human NPCs and phantoms) come after
the others, inside the area's region and not counted against the roster limit: the runtime places them as stand-ins
(ps2/proto/src/npc.c, TECH_DEBT enemy-14); older runtimes skip them (no c0000 chr.bin).
Usage: export_enemies.py <game dir> <out.bin> <map id>      (only the parts inside the area's region, tools/area.py)
       export_enemies.py --models <game dir> <map id>      (prints the area's enemy types)"""
import math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import area
import havok_anim as ha

# enemy_parts / npc_parts / MAX_ENEMIES live in area.py (per-area selection), so export_chr.py and lua50.py do not
# depend on this file
from area import MAX_ENEMIES, enemy_parts, npc_parts  # noqa: F401


# m from a placement; ASSUMED: past the largest maxBackhomeDist of the test areas' scripts (TECH_DEBT enemy-2)
NAV_REACH = 60.0
NAV_WELD = 0.02   # m: NVM parts share their border vertices
NAV_Q = 16        # 1/16 m vertex grid (3 cm error)
NAV_CELL = 8.0    # m


def navmesh(game, map_id, M, keep, anchors):
    """(verts [(x, y, z)], tris [(a, b, c, flags)]) of the area's navmesh near the anchors (see the layout above)"""
    path = f"{game}/map/{map_id}/{map_id}.nvmbnd.dcx"
    if not os.path.exists(path) or not anchors:
        return [], []
    models = {souls.basename(n)[:-4]: d for _, n, d in souls.bnd3(souls.read(path))}
    near = lambda p: keep(*p) and any((p[0] - a[0]) ** 2 + (p[2] - a[2]) ** 2 < NAV_REACH ** 2 and
                                      abs(p[1] - a[1]) < NAV_REACH for a in anchors)
    weld, verts, tris = {}, [], []
    for part in M["parts"]:
        key = f"{part['model']}A{map_id[1:3]}" if part["type"] == 8 else None
        if key not in models:
            continue
        q, sc, t = souls._euler_deg_xzy(*part["rot"]), part["scale"], part["pos"]
        nv, nt = souls.nvm(models[key])
        world = []
        for p in nv:
            r = ha.qrot(q, (p[0] * sc[0], p[1] * sc[1], p[2] * sc[2]))
            world.append((r[0] + t[0], r[1] + t[1], r[2] + t[2]))
        for a, b, c, flags in nt:
            pts = [world[a], world[b], world[c]]
            if not any(near(p) for p in pts):
                continue
            idx = []
            for p in pts:
                k = tuple(round(x / NAV_WELD) for x in p)
                if k not in weld:
                    weld[k] = len(verts)
                    verts.append(p)
                idx.append(weld[k])
            if len(set(idx)) == 3:
                tris.append((*idx, flags))
    return verts, tris


def nav_blob(verts, tris):
    if not tris or len(verts) > 0xFFFF or len(tris) >= 0xFFFF:
        return b""
    lo = [min(v[i] for v in verts) for i in range(3)]
    hi = [max(v[i] for v in verts) for i in range(3)]
    org = [(lo[i] + hi[i]) / 2 for i in range(3)]
    qv = [tuple(int(round((v[i] - org[i]) * NAV_Q)) for i in range(3)) for v in verts]
    assert all(-32768 <= c <= 32767 for v in qv for c in v), "navmesh wider than the i16 grid"
    edge = {}
    for t, (a, b, c, _) in enumerate(tris):
        for u, v in ((a, b), (b, c), (c, a)):
            edge.setdefault((min(u, v), max(u, v)), []).append(t)
    out = b""
    for t, (a, b, c, flags) in enumerate(tris):
        nbr = []
        for u, v in ((a, b), (b, c), (c, a)):
            other = [o for o in edge[(min(u, v), max(u, v))] if o != t]
            nbr.append(other[0] if other else 0xFFFF)
        out += struct.pack("<6H", a, b, c, *nbr)
    gw = int((hi[0] - lo[0]) // NAV_CELL) + 1
    gh = int((hi[2] - lo[2]) // NAV_CELL) + 1
    cells = [[] for _ in range(gw * gh)]
    for t, (a, b, c, _) in enumerate(tris):
        xs, zs = [verts[i][0] for i in (a, b, c)], [verts[i][2] for i in (a, b, c)]
        for gz in range(int((min(zs) - lo[2]) // NAV_CELL), int((max(zs) - lo[2]) // NAV_CELL) + 1):
            for gx in range(int((min(xs) - lo[0]) // NAV_CELL), int((max(xs) - lo[0]) // NAV_CELL) + 1):
                cells[gz * gw + gx].append(t)
    starts, lst = [0], []
    for c in cells:
        lst += c
        starts.append(len(lst))
    head = struct.pack("<4s4I6fI", b"NAV1", len(verts), len(tris), gw, gh, *org, lo[0], lo[2], NAV_CELL, len(lst))
    body = b"".join(struct.pack("<3h", *v) for v in qv)
    if len(lst) > 0xFFFF:
        return b""
    body += b"\0" * (-len(body) % 4) + out + struct.pack(f"<{len(starts)}H", *starts)
    body += struct.pack(f"<{len(lst)}H", *lst)
    return head + body + b"\0" * (-len(body) % 4)


def main():
    if sys.argv[1] == "--models":  # export_enemies.py --models <game dir> <map id>: the area's enemy types (Makefile)
        b = souls.read(f"{sys.argv[2]}/map/MapStudio/{sys.argv[3]}.msb")
        print(" ".join(sorted({r[0] for r in enemy_parts(b, sys.argv[3]) if r[0] != "c0000"})))
        return
    game, out, map_id = sys.argv[1], sys.argv[2], sys.argv[3]
    b = souls.read(f"{game}/map/MapStudio/{map_id}.msb")
    rows = [r for r in enemy_parts(b, map_id) if r[0] != "c0000"] + npc_parts(b, map_id)
    M = souls.msb1(b)
    verts, tris = navmesh(game, map_id, M, area.region(M, map_id), [r[4] for r in rows if r[0] != "c0000"])
    nav = nav_blob(verts, tris)
    with open(out, "wb") as f:
        f.write(struct.pack("<4sI", b"DSRN", len(rows)))
        for model, npc, think, ent, pos, rot in rows:
            f.write(struct.pack("<8s3i4f", model.encode(), npc, think, ent, *pos, math.radians(rot[1]) + math.pi))
        f.write(nav)
    models = sorted({r[0] for r in rows if r[0] != "c0000"})
    nnpc = sum(r[0] == "c0000" for r in rows)
    print(f"{map_id}: {len(rows) - nnpc} enemies + {nnpc} c0000 NPCs, models {models}; navmesh {len(verts)} verts "
          f"{len(tris)} tris, {len(nav)} B")


if __name__ == "__main__":
    main()
