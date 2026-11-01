"""Export an area's breakable MSB objects (pots, ...) for the PS2 prototype's destruction sim (src/objects.c).

Breakable = an MSB object part (type 1) whose obj/<model>.objbnd carries a broken state: <model>_1.flver (the
pre-fractured mesh, one bone per piece, rigid weights) and <model>_1.hkx (one hkpRigidBody per piece:
hkpConvexTranslateShape around an hkpBoxShape, mass, inverse inertia, friction, restitution). The intact state is
<model>.flver + <model>.hkx (one fixed hkpRigidBody, an hkpCylinderShape for the pots). ObjectParam row <model id>
gives hp / defense (pots: hp 1, defense 0, isBreakByPlayerCollide 0).

One model.bin per type (export_model.py layout, shared MeshBuilder / write_model): bone 0 = the intact object (its
meshes carry group bit BITS_INTACT), bones 1..n = the pieces (bit BITS_BROKEN). All bones are roots; bone bind poses
are the FLVER's, which equal the Havok bodies' rest transforms (checked here).

objects.bin (little-endian, models 128-byte aligned from the blob start; pack_load gives 128-aligned blobs):
  header  "DSRB", u32 version 1, u32 types, u32 parts, u32 type_off, u32 part_off, u32 0, u32 0           (32 B)
  type    i32 model id (1497 = o1497), u32 model_off, u32 model_size, u32 pieces, u32 piece_off, i32 hp,
          i32 defense, f32 cylinder radius, y0, y1 (intact shape, model space, convex radius included),
          f32 friction, restitution, u32 intact bits, u32 broken bits, u32 0, u32 0                       (64 B)
  piece   f32 pos[3], q[4] (x y z w: rest transform, model space), half[3] (box half extents + convex radius),
          f32 centre[3] (box centre in the piece frame), inv_mass, inv_inertia[3] (piece frame), 3 x f32 0 (80 B)
  part    u16 type, u16 0, f32 pos[3], q[4] (MSB placement; scale is 1 for every breakable at m10_02)    (32 B)
Usage: export_objects.py <game dir> <out objects.bin> <map id e.g. m10_02_00_00>"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import area as area_mod
import havok_anim as ha
from export_model import MeshBuilder, bind_world, write_model

BITS_INTACT, BITS_BROKEN = 8, 16
TEX = 64    # EE RAM / VRAM budget (< 0.3 MB for all breakables with the VU1 skin streams)
# FLVER face set (LOD) per state: LOD2 for both keeps all breakables under 0.3 MB of EE RAM (intact pots 96-124 tris;
# LOD1 = 292-380 tris would cost 0.384 MB); pieces 176-226 tris for the whole pot
LOD_INTACT, LOD_BROKEN = 2, 2
OBJ_BUDGET = 0.4  # MB of EE RAM for breakables (HANDOFF RAM table); per area: tools/area.py obj_budget
SKIN_BATCH_TRIS, SKIN_BATCH_BYTES = 31, 96 * 32  # model.c: VU1_SKIN_TRIS, SK_V x 4 streams x 8 B


def objbnd(game, model):
    return {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(os.path.join(game, "obj", model + ".objbnd.dcx")))}


def is_breakable(files, model):
    return model + "_1.hkx" in files and model + "_1.flver" in files


def bodies(hkx):
    """[(name, motion type, rotation 3x3 rows, pos, inertia/mass inv xyzw, friction, restitution, shape)] of every
    hkpRigidBody; shape = ("box", half, centre, radius) or ("cyl", radius, a, b, convex radius)"""
    h = souls.HkTag(hkx)
    D = h.data
    rb = h.type("hkpRigidBody")
    co, _ = rb.field("collidable")
    mo, _ = rb.field("motion")
    mat, _ = rb.field("material")
    out = []
    for off in h.objects("hkpRigidBody"):
        t, so, _ = h.item(off, rb, "name")
        name = souls.cstr(D, so) if t else ""
        st, soff, _ = h.items[struct.unpack_from("<I", D, off + co)[0]]
        m = off + mo
        xf = struct.unpack_from("<16f", D, m + 32)  # hkMotionState.transform: rotation columns, then translation
        rot = [[xf[c * 4 + r] for c in range(3)] for r in range(3)]
        imi = struct.unpack_from("<4f", D, m + 208)
        fr, rest = struct.unpack_from("<ff", D, off + mat + 4)
        rad = struct.unpack_from("<f", D, soff + 32)[0]
        if st.name == "hkpConvexTranslateShape":
            centre = struct.unpack_from("<3f", D, soff + 64)
            ct, coff, _ = h.items[struct.unpack_from("<I", D, soff + 48)[0]]
            assert ct.name == "hkpBoxShape", ct.name
            shape = ("box", struct.unpack_from("<3f", D, coff + 48), centre, rad)
        elif st.name == "hkpBoxShape":
            shape = ("box", struct.unpack_from("<3f", D, soff + 48), (0, 0, 0), rad)
        elif st.name == "hkpCylinderShape":
            shape = ("cyl", struct.unpack_from("<f", D, soff + 40)[0], struct.unpack_from("<3f", D, soff + 48),
                     struct.unpack_from("<3f", D, soff + 64), rad)
        else:  # hkpConvexVerticesShape, ... (Undead Burg crates): bounds taken from the FLVER mesh (type_model)
            shape = ("other", st.name)
        out.append((name, D[m + 16], rot, xf[12:15], imi, fr, rest, shape))
    return out


def mesh_bounds(F):
    """{FLVER bone index (None = all): (lo, hi)} of the mesh vertices, bone-local (static object meshes are rigid)"""
    out = {}
    for m in F["meshes"]:
        V = m["verts"]
        for i, p in enumerate(V["pos"]):
            fb = m["bones"][V["bone_idx"][i][0]] if V["bone_idx"] and m["bones"] else m["default_bone"]
            for key in (fb, None):
                lo, hi = out.setdefault(key, ([1e9] * 3, [-1e9] * 3))
                for k in range(3):
                    lo[k], hi[k] = min(lo[k], p[k]), max(hi[k], p[k])
    return out


def souls_rot(q):
    """quaternion (x, y, z, w) -> 3x3 rotation rows"""
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def mat_to_quat(m):
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        s = (tr + 1) ** 0.5 * 2
        return ((m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, s / 4)
    i = max(range(3), key=lambda k: m[k][k])
    j, k = (i + 1) % 3, (i + 2) % 3
    s = (1 + m[i][i] - m[j][j] - m[k][k]) ** 0.5 * 2
    q = [0, 0, 0, (m[k][j] - m[j][k]) / s]
    q[i], q[j], q[k] = s / 4, (m[j][i] + m[i][j]) / s, (m[k][i] + m[i][k]) / s
    return tuple(q)


def type_model(game, model, files, dds):
    """-> (model.bin bytes, pieces [(pos, q, half, centre, inv_mass, inv_inertia)], cylinder (r, y0, y1), fr, rest)"""
    intact = souls.flver(files[model + ".flver"])
    broken = souls.flver(files[model + "_1.flver"])
    pieces_hk = {b[0]: b for b in bodies(files[model + "_1.hkx"])}
    intact_body = bodies(files[model + ".hkx"])[0]
    _, _, rot0, pos0, _, fr, rest, shape = intact_body
    if shape[0] == "other":  # no box / cylinder: the intact FLVER's bounding box stands in for the Havok shape
        lo, hi = mesh_bounds(intact)[None]
        cylinder = (min(hi[0] - lo[0], hi[2] - lo[2]) / 2, lo[1], hi[1])
    elif shape[0] == "cyl":  # pots: an upright hkpCylinderShape
        _, cr, va, vb, crad = shape
        cylinder = (cr + crad, min(va[1], vb[1]) - crad, max(va[1], vb[1]) + crad)
    else:  # crates / barrels (Undead Burg): a box; objects.c only pushes out of upright cylinders, so use the box's
        # inscribed vertical cylinder (radius = smaller x/z half extent: corners stay slightly walk-in, TECH_DEBT
        # physics-1), box rotation applied to the half extents for the axis-aligned footprint
        _, half, centre, crad = shape
        ext = [sum(abs(rot0[r][c]) * (half[c] + crad) for c in range(3)) for r in range(3)]
        cen = [pos0[r] + sum(rot0[r][c] * centre[c] for c in range(3)) for r in range(3)]
        cylinder = (min(ext[0], ext[2]), cen[1] - ext[1], cen[1] + ext[1])

    names = [model] + [b[0] for b in broken["bones"]]
    assert all(b[1] < 0 for b in broken["bones"]), "broken FLVER bones must be roots"
    sk = {n: i for i, n in enumerate(names)}
    mb = MeshBuilder(sk)
    pieces = []
    bw_b = bind_world(broken["bones"])
    pb = mesh_bounds(broken)
    for i, (n, _, t, q, _) in enumerate(broken["bones"]):
        if n not in pieces_hk:  # a piece without its own rigid body (Undead Burg o1120): mesh bounds + the mean
            # mass / inertia of the type's other bodies (ASSUMED, TECH_DEBT physics-1)
            hb = list(pieces_hk.values())
            imi = tuple(sum(b[4][k] for b in hb) / len(hb) for k in range(4))
            pieces_hk[n] = (n, 0, souls_rot(q), t, imi, fr, rest, ("other", "none"))
        hk = pieces_hk[n]
        hq = mat_to_quat(hk[2])
        dot = abs(sum(a * b for a, b in zip(hq, q)))
        dpos = max(abs(a - b) for a, b in zip(hk[3], t))
        _, _, _, _, imi, _, _, shp = hk
        if shp[0] != "box":  # convex hull piece: its mesh's bone-local bounding box (TECH_DEBT physics-1)
            lo, hi = pb.get(sk[n] - 1, ([-0.05] * 3, [0.05] * 3))  # no vertices on this bone: 10 cm stand-in
            shp = ("box", [(b - a) / 2 for a, b in zip(lo, hi)], [(a + b) / 2 for a, b in zip(lo, hi)], 0.0)
        _, half, centre, rad = shp
        half = tuple(h + rad for h in half)
        if not (dot > 0.999 and dpos < 1e-3):
            # Body frame != bone frame (Undead Burg o1000 barrel staves, up to ~20 deg): objects.c keeps one frame
            # per piece (the bone, which the mesh needs), so the box and the inverse inertia are moved into it: the
            # box becomes its axis-aligned bound in the bone frame (a bit larger), the inertia keeps its diagonal
            # (TECH_DEBT physics-1)
            Rb = souls_rot(q)
            Rh = hk[2]
            M = [[sum(Rb[k][r] * Rh[k][c] for k in range(3)) for c in range(3)] for r in range(3)]  # body -> bone
            wc = [hk[3][r] + sum(Rh[r][c] * centre[c] for c in range(3)) - t[r] for r in range(3)]
            centre = tuple(sum(Rb[k][r] * wc[k] for k in range(3)) for r in range(3))
            half = tuple(sum(abs(M[r][c]) * half[c] for c in range(3)) for r in range(3))
            imi = tuple(sum(M[r][c] ** 2 * imi[c] for c in range(3)) for r in range(3)) + (imi[3],)
        pieces.append((t, q, half, centre, imi[3], imi[:3]))
    ref = [((0, 0, 0), (0, 0, 0, 1))] + [(t, q) for t, q in bw_b]

    # Static object meshes are rigid and bone-local (no weights; bone_idx[0] -> mesh bone list -> FLVER bone): split
    # each mesh's chosen LOD face set by its vertices' bone and add the pieces through MeshBuilder's rigid path
    for F, bits in ((intact, BITS_INTACT), (broken, BITS_BROKEN)):
        for m in F["meshes"]:
            tex = os.path.splitext(os.path.basename(m["diffuse"]))[0] if m["diffuse"] else ""
            ti = mb.texture(tex, dds.get(tex) or dds.get(tex.lower())) if tex else -1
            V = m["verts"]
            fs = m["facesets"][min(LOD_INTACT if F is intact else LOD_BROKEN, len(m["facesets"]) - 1)]
            per_bone = {}
            for tri in souls.triangles(fs):
                fb = m["bones"][V["bone_idx"][tri[0]][0]] if V["bone_idx"] and m["bones"] else m["default_bone"]
                per_bone.setdefault(fb, []).extend(tri)
            for fb, idx in per_bone.items():
                skb = 0 if F is intact else sk[F["bones"][fb][0]]
                mb.add(F, None, m, (fs[0], False, idx, fs[3]), ti, (128, 128, 128), rigid=(skb, False), bits=bits)
    out = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"export_objects_{os.getpid()}_{model}.bin")
    write_model(out, names, [-1] * len(names), ref, mb, [])
    with open(out, "rb") as f:
        blob = f.read()
    os.unlink(out)
    # runtime heap of mdl_load's skin streams: one batch per <= 31 triangles of each (texture, flags) group
    groups = {}
    for key in mb.tri_tex:
        groups[key] = groups.get(key, 0) + 1
    heap = sum((n + SKIN_BATCH_TRIS - 1) // SKIN_BATCH_TRIS for n in groups.values()) * SKIN_BATCH_BYTES
    return blob, pieces, cylinder, fr, rest, heap


def main():
    game, out_path, map_id = sys.argv[1], sys.argv[2], sys.argv[3]
    import export_model
    export_model.TEX_MAX = TEX
    area = map_id[:3]
    mdir = os.path.join(game, "map")
    msb = souls.msb1(souls.read(os.path.join(mdir, "MapStudio", map_id + ".msb")))
    params = souls.game_params(game)["ObjectParam"]
    dds = {}
    for f in sorted(os.listdir(f"{mdir}/{area}")):
        if f.endswith(".tpfbhd") and f.startswith(area + "_"):
            for _, _, d in souls.bxf3(f"{mdir}/{area}/{f}", f"{mdir}/{area}/{f[:-3]}bdt"):
                dds.update(souls.tpf(d))
    common = f"{mdir}/{area}/{area}_9999.tpf.dcx"  # area-shared textures (the pots' m10_obj_pot3 diffuse)
    if os.path.exists(common):
        dds.update(souls.tpf(souls.read(common)))

    types, parts, heaps = [], [], []
    keep = area_mod.region(msb, map_id)
    for part in msb["parts"]:
        if part["type"] != 1 or not keep(*part["pos"]):
            continue
        model = part["model"]
        if model not in [t[0] for t in types]:
            files = objbnd(game, model)
            if not is_breakable(files, model):
                continue
            dds_obj = dict(dds)
            for n, d in files.items():
                if n.endswith(".tpf"):
                    dds_obj.update(souls.tpf(d))
            types.append((model, files) + type_model(game, model, files, dds_obj))
            heaps.append(types[-1][-1])
        ti = [t[0] for t in types].index(model)
        assert all(abs(s - 1) < 1e-3 for s in part["scale"]), f"{part['name']}: scaled"
        parts.append((ti, part["pos"], souls._euler_deg_xzy(*part["rot"])))

    # RAM budget (HANDOFF: objects runtime <= 0.4 MB): keep the types with the most placements per byte first; the
    # dropped types are listed in <out>.static and export_map.py draws them as static map objects (intact, no break)
    budget = area_mod.config(map_id).get("obj_budget", OBJ_BUDGET) * 1048576 - 24 * 1024
    cost = [len(t[2]) + t[-1] + 128 for t in types]
    count = [sum(1 for p in parts if p[0] == k) for k in range(len(types))]
    order = sorted(range(len(types)), key=lambda k: -count[k] / cost[k])
    kept, used = set(), 0
    for k in order:
        if used + cost[k] <= budget:
            kept.add(k)
            used += cost[k]
    dropped = sorted(types[k][0] for k in range(len(types)) if k not in kept)
    remap = {k: i for i, k in enumerate(sorted(kept))}
    types = [types[k] for k in sorted(kept)]
    heaps = [t[-1] for t in types]
    parts = [(remap[ti], pos, q) for ti, pos, q in parts if ti in remap]
    with open(out_path + ".static", "w") as f:
        f.write("".join(m + "\n" for m in dropped))
    if dropped:
        print(f"over the {budget / 1048576:.2f} MB budget, drawn as static map objects instead: {' '.join(dropped)}")

    TYPE, PIECE, PART = 64, 80, 32
    type_off = 32
    piece_off = type_off + TYPE * len(types)
    part_off = piece_off + PIECE * sum(len(t[3]) for t in types)
    blob = bytearray(part_off + PART * len(parts))
    struct.pack_into("<4s7I", blob, 0, b"DSRB", 1, len(types), len(parts), type_off, part_off, 0, 0)
    pi = 0
    for k, (model, files, mbin, pieces, cyl, fr, rest, heap) in enumerate(types):
        blob.extend(b"\0" * (-len(blob) % 128))
        moff = len(blob)
        blob += mbin
        mid = int(model[1:])
        row = params.get(mid, (None, {}))[1]
        struct.pack_into("<i4I2i5f4I", blob, type_off + TYPE * k, mid, moff, len(mbin), len(pieces),
                         piece_off + PIECE * pi, row.get("hp", 1), row.get("defense", 0), *cyl, fr, rest,
                         BITS_INTACT, BITS_BROKEN, 0, 0)
        for j, (t, q, half, centre, inv_mass, inv_inertia) in enumerate(pieces):
            struct.pack_into("<20f", blob, piece_off + PIECE * (pi + j), *t, *q, *half, *centre, inv_mass,
                             *inv_inertia, 0, 0, 0)
        pi += len(pieces)
        print(f"{model}: {len(pieces)} pieces, cylinder r {cyl[0]:.3f} y {cyl[1]:.3f}..{cyl[2]:.3f}, hp {row.get('hp')}, "
              f"model {len(mbin)} B + skin streams {heap} B, {sum(1 for p in parts if p[0] == k)} parts")
    for k, (ti, pos, q) in enumerate(parts):
        struct.pack_into("<HH3f4f", blob, part_off + PART * k, ti, 0, *pos, *q)
    with open(out_path, "wb") as f:
        f.write(blob)
    print(f"{len(types)} breakable types, {len(parts)} parts, {len(blob)} bytes -> {out_path}; EE RAM with the skin "
          f"streams and objects.c / physics.c pools (~24 KB): {(len(blob) + sum(heaps) + 24 * 1024) / 1048576:.3f} MB")


if __name__ == "__main__":
    main()
