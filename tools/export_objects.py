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
  header  "DSRB", u32 version 2, u32 types, u32 parts, u32 type_off, u32 part_off, u32 0, u32 0           (32 B)
  type    i32 model id (1497 = o1497), u32 model_off, u32 model_size, u32 pieces, u32 piece_off, i32 hp,
          i32 defense, f32 cylinder radius, y0, y1 (intact shape, model space, convex radius included),
          f32 friction, restitution, u32 intact bits, u32 broken bits, u32 0, u32 0                       (64 B)
  piece   f32 pos[3], q[4] (x y z w: rest transform, model space), half[3] (box half extents + convex radius),
          f32 centre[3] (box centre in the piece frame), inv_mass, inv_inertia[3] (piece frame), 3 x f32 0 (80 B)
  part    u16 type, u16 0, f32 pos[3], q[4] (MSB placement; scale is 1 for every breakable at m10_02), i32 MSB
          entity id (-1 none: EMEVD addresses a breakable by it), u32 0 x 3                                (48 B)
          (version 1: 32 B parts, no entity id; version 2: 32 B header)
Version 3 adds animated objects (doors, levers, gates, chests: src/objects.c, events play their anims) and grows the
header to 48 B:
  header  "DSRB", u32 3, u32 types, u32 parts, u32 type_off, u32 part_off, u32 anim types, u32 anim parts,
          u32 anim type_off, u32 anim part_off, u32 body_off, u32 texture model off (0 none), u32 static anims,
          u32 static anim off, u32 0 x 2                                                                    (64 B)
  The animated types' models carry no textures: their groups index the texture model's (one bone, no triangles),
  whose VRAM textures every type shares (mdl_share_textures).
  atype   i32 model id, u32 model_off, u32 model_size (compact anims last: mdl_compact frees the blob), u32 bodies,
          u32 first body, u32 EE RAM after mdl_compact (B, estimate), u32 root bones (bit per bone: objects.c
          puts the placement rotation into the roots' pose; types with more than 32 bones stay static), u32 0  (32 B)
  body    i32 bone, f32 centre[3], q[4] (x y z w), half[3] (box in the bone's frame, convex radius included), u32 0
          (48 B): every hkpRigidBody of <model>.hkx (keyframed, named after its bone); cylinders / convex shapes as
          their bounding box (convex: the bone's mesh bounds, TECH_DEBT event-11)
  apart   u16 atype, u16 0, f32 pos[3], q[4] (MSB placement), i32 MSB entity id, i32 MSB init anim (-1 none),
          u32 0 x 2 (48 B); scaled placements stay static
  sanim   i32 entity, u32 anims, 8 x (i32 anim id, f32 length s) (72 B): objects the events animate that stay static
          map objects (elevators, ...), so 2003[18] "wait for completion" waits as long as in DSR
Animated = an MSB object whose objbnd has an anibnd (skeleton + a00_NNNN.hkx anims) and that the map's EMEVD animates
(2005[07] End Of Animation / 2003[18] Force Animation on its entity id) or an ObjAct uses; not breakables, not the
models in ANIM_SKIP (elevators: their moving floor is A9's). Their part names go to <out>.anim and export_map.py leaves
them out of the static map (ANIM_OBJ=<file>), export_emevd.py out of its static colliders.
Usage: export_objects.py <game dir> <out objects.bin> <map id e.g. m10_02_00_00>"""
import math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import area as area_mod
import srcsum
import havok_anim as ha
from export_model import MeshBuilder, bind_world, write_model

BITS_INTACT, BITS_BROKEN = 8, 16
TEX = 64    # EE RAM / VRAM budget (< 0.3 MB for all breakables with the VU1 skin streams)
# FLVER face set (LOD) per state: LOD2 for both keeps all breakables under 0.3 MB of EE RAM (intact pots 96-124 tris;
# LOD1 = 292-380 tris would cost 0.384 MB); pieces 176-226 tris for the whole pot
LOD_INTACT, LOD_BROKEN = 2, 2
MAX_PIECES = 32  # src/objects.c MAX_PIECES
OBJ_BUDGET = 0.4  # MB of EE RAM for breakables (HANDOFF RAM table); per area: tools/area.py obj_budget
SKIN_BATCH_TRIS, SKIN_BATCH_BYTES = 31, 96 * 32  # model.c: VU1_SKIN_TRIS, SK_V x 4 streams x 8 B
# Animated objects: resident from boot like the breakables (every block's types load at boot, src/world.c), so a budget
# per block; LOD face set ANIM_LOD (doors are seen up close: 0), textures at TEX like the pots (VRAM is tight)
ANIM_BUDGET = float(os.environ.get("ANIM_BUDGET", 0.25))  # MB per area / block
ANIM_LOD = int(os.environ.get("ANIM_LOD", 0))
ANIM_TEX_H = int(os.environ.get("ANIM_TEX_H", 16))  # texture height (width 128; VRAM: 2 KB each at 16)
ANIM_MAX_TRIS = 600  # a type over this takes the next LOD (skin streams ~100 B per triangle)
# Elevators (A9 moves the platform and the player on it) and objects whose bones leave model.bin's +-8 m range stay
# static map objects; their anims stay event stubs
ANIM_SKIP = set(os.environ.get("ANIM_SKIP", "o1460").split(","))


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

    # every broken bone becomes a root piece at its model-space bind. Most broken FLVERs have root bones only; the
    # Asylum's o8403 parents its 18 pieces to a root bone named like the model, which has a rigid body of its own in
    # o8403_1.hkx and so is a piece too (its name then maps to that piece; the intact mesh uses bone 0 explicitly)
    names = [model] + [b[0] for b in broken["bones"]]
    sk = {n: i for i, n in enumerate(names)}
    mb = MeshBuilder(sk)
    pieces = []
    bw_b = bind_world(broken["bones"])
    pb = mesh_bounds(broken)
    for i, (n, _, t, q, _) in enumerate(broken["bones"]):
        t, q = bw_b[i]
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


def anim_targets(game, map_id, msb):
    """Entity ids the map's events animate (2005[07] / 2003[18]: literal targets and every int event arg, as
    export_emevd.py's toggled set) and the part names of every ObjAct"""
    import emevd
    ids = set()
    for ev in emevd.load(map_id, game).events:
        for ins in ev.instrs:
            if (ins.bank, ins.id) in ((2005, 7), (2003, 18)) and ins.args:
                ids.add(ins.args[0])
            elif (ins.bank, ins.id) == (2000, 0) and ins.args:
                ids.update(v for v in ins.args[2:] + ins.extra if isinstance(v, int))
    objact = {e["obj_act_part"] for e in msb["events"] if e["type_name"] == "ObjAct"}
    return ids, objact


def anibnd(files, model):
    """{lower-case name: bytes} of the objbnd's anibnd, or None"""
    a = files.get(model + ".anibnd")
    return {souls.basename(n).lower(): d for _, n, d in souls.bnd3(a)} if a else None


def anim_type_model(game, model, files, dds, shared_tex):
    """-> (model.bin bytes with compact anims, bodies [(bone, centre, q, half)], EE RAM, tris) or None. tex = (list,
    {name: index}) shared by all animated types of the pack: their models carry no textures, only indices into the
    pack's texture model (anim_texture_model; objects.c shares its VRAM textures with every type)"""
    import export_chr
    from export_model import bake, to_local
    an = anibnd(files, model)
    sk_name = next((k for k in an if k.startswith("ske")), None) if an else None  # skeleton.hkx / skelton.hkx
    if not sk_name or model + ".flver" not in files:
        return None
    names, parents, ref = ha.skeleton(an[sk_name])
    if len(names) > 32:
        print(f"{model}: {len(names)} bones (objects.c: 32 at most): stays static")
        return None
    if any(abs(c) > 7.99 for r in ref for c in r[0]):
        print(f"{model}: bone translations beyond model.bin's 8 m: stays static")
        return None
    anims = []
    for k in sorted(an):
        if not (k.startswith("a00_") and k.endswith(".hkx")):
            continue
        aid = int(k[4:8])
        files_a = {f"a00_{aid:04d}.hkx": an[k]}
        nf, fps, baked = bake(files_a, "a00", aid, ref)
        if any(abs(c) > 7.99 for pose in baked for _, t in pose for c in t):
            print(f"{model}: anim {aid} moves bones beyond 8 m: stays static")
            return None
        anims.append((aid, baked))
    F = souls.flver(files[model + ".flver"])
    sk = {n: i for i, n in enumerate(names)}
    bw = bind_world(F["bones"])
    skw = ha.world(parents, ref)
    for fb, (n, *_r) in enumerate(F["bones"]):  # the FLVER bind and the Havok reference pose agree (checked)
        if n in sk and max(abs(a - b) for a, b in zip(bw[fb][0], skw[sk[n]][0])) > 0.01:
            print(f"{model}: FLVER bone {n} off the skeleton's reference pose")
    mb = MeshBuilder(sk)
    tris = 0
    for lod in range(ANIM_LOD, 4):
        mb = MeshBuilder(sk)
        mb.textures, mb.tex_index = shared_tex
        for m in F["meshes"]:
            tex = os.path.splitext(os.path.basename(m["diffuse"]))[0] if m["diffuse"] else ""
            ti = anim_texture(mb, tex, (dds.get(tex) or dds.get(tex.lower())) if tex else None)
            fs = m["facesets"][min(lod, len(m["facesets"]) - 1)]
            if m["dynamic"]:  # skinned (weights, model-space positions)
                mb.add(F, bw, m, fs, ti, (128, 128, 128))
                continue
            V = m["verts"]
            per_bone = {}
            for tri in souls.triangles(fs):
                fb = m["bones"][V["bone_idx"][tri[0]][0]] if V["bone_idx"] and m["bones"] else m["default_bone"]
                per_bone.setdefault(fb, []).extend(tri)
            for fb, idx in per_bone.items():
                # rigid, bone-local to FLVER bone fb: into the frame of the skeleton bone it maps to
                skb, sfb = mb.skeleton_bone(F, fb)
                if sfb != fb:
                    tb, qb = bw[fb]
                    pos = [to_local(bw[sfb], tuple(a + b for a, b in zip(ha.qrot(qb, p), tb))) for p in V["pos"]]
                    nrm = [ha.qrot(qconj_(bw[sfb][1]), ha.qrot(qb, n)) for n in V["normal"]] if V["normal"] else None
                    m2 = dict(m, verts=dict(V, pos=pos, normal=nrm))
                else:
                    m2 = m
                mb.add(F, None, m2, (fs[0], False, idx, fs[3]), ti, (128, 128, 128), rigid=(skb, False))
        tris = len(mb.tris)
        if tris <= ANIM_MAX_TRIS:
            break
    # bodies: keyframed rigid bodies named after their bone, rest transform in model space -> the bone's frame
    bodies_out = []
    bounds = mesh_bounds(F)
    moving = {i for _, baked in anims for i in range(len(names))
              if any(max(abs(a - c) for a, c in zip(p[i][0] + p[i][1], baked[0][i][0] + baked[0][i][1])) > 1e-3
                     for p in baked)}
    for name, _, rot, pos, _, _, _, shape in bodies(files[model + ".hkx"]) if model + ".hkx" in files else []:
        b = sk.get(name, 0)
        bt, bq = skw[b]
        if shape[0] == "box":
            _, half, centre, rad = shape
            half = tuple(h + rad for h in half)
        elif shape[0] == "cyl":  # bounding box of the capsule-ended cylinder segment
            _, cr, va, vb, crad = shape
            centre = tuple((a + c) / 2 for a, c in zip(va, vb))
            half = tuple(abs(a - c) / 2 + cr + crad for a, c in zip(va, vb))
        else:  # convex vertices: the mesh bounds of the FLVER bone of that name (bone-local), for bones the anims
            # move (lever o1200's arms); a static bone's convex body is a frame's hull (door o1465): its box would fill
            # the doorway, so it is left out (TECH_DEBT event-11)
            fb = next((i for i, bb in enumerate(F["bones"]) if bb[0] == name), None)
            if fb is None or fb not in bounds or b not in moving:
                continue
            lo, hi = bounds[fb]
            centre, half = tuple((a + c) / 2 for a, c in zip(lo, hi)), tuple((c - a) / 2 for a, c in zip(lo, hi))
            bodies_out.append((b, centre, (0, 0, 0, 1), half))
            continue
        wc = [pos[r] + sum(rot[r][c] * centre[c] for c in range(3)) for r in range(3)]
        lc = to_local((bt, bq), wc)
        lq = ha.qmul(qconj_(bq), mat_to_quat(rot))
        bodies_out.append((b, tuple(lc), tuple(lq), tuple(half)))
    out = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"export_objects_{os.getpid()}_{model}_anim.bin")
    mb.textures = []  # in the pack's texture model
    write_model(out, names, parents, [(r[0], r[1]) for r in ref], mb, [])
    n0 = os.path.getsize(out)
    export_chr.append_compact_anims(out, len(names), anims)
    with open(out, "rb") as f:
        blob = f.read()
    anim_bytes = len(blob) - n0
    os.unlink(out)
    groups = {}
    for key in mb.tri_tex:
        groups[key] = groups.get(key, 0) + 1
    heap = sum((n + SKIN_BATCH_TRIS - 1) // SKIN_BATCH_TRIS for n in groups.values()) * SKIN_BATCH_BYTES
    print(f"{model}: animated, {len(names)} bones, {tris} tris, anims {[a for a, _ in anims]}, {len(bodies_out)} "
          f"bodies, model {len(blob)} B + skin streams {heap} B")
    # EE RAM after mdl_compact: skin streams + header / parents / ref / groups (~1 KB) + the anims
    return blob, bodies_out, heap + anim_bytes + 1024, tris, sum(1 << i for i, p in enumerate(parents) if p < 0)


def anim_objects(game, map_id, msb, keep, dds):
    """-> (types [(model, model.bin, bodies, heap)], parts [(name, type, pos, q, entity, init anim)]) within ANIM_BUDGET
    MB, the types with the most parts per byte first (the rest stay static map objects)"""
    ids, objact = anim_targets(game, map_id, msb)
    cand, models, still = [], {}, []
    for part in msb["parts"]:
        if part["type"] != 1 or not keep(*part["pos"]):
            continue
        if part.get("entity_id", -1) not in ids and part["name"] not in objact:
            continue
        still.append(part)
        if part["model"] in ANIM_SKIP:
            continue
        model = part["model"]
        if model not in models:
            files = objbnd(game, model)
            models[model] = None
            if is_breakable(files, model) or not anibnd(files, model):
                continue
            dds_obj = dict(dds)
            for n, d in files.items():
                if n.endswith(".tpf"):
                    dds_obj.update(souls.tpf(d))
            models[model] = anim_type_model(game, model, files, dds_obj, ([], {}))
            if models[model]:
                models[model] = models[model] + (dds_obj,)
        if models[model] is None:
            continue
        if any(abs(s - 1) > 1e-3 for s in part["scale"]):
            print(f"{part['name']}: scaled placement, stays static")
            continue
        cand.append(part)
    budget, used, kept = ANIM_BUDGET * 1048576, 0, []
    count = {m: sum(1 for p in cand if p["model"] == m) for m in models}
    for m in sorted((m for m in models if models[m] and count[m]), key=lambda m: -count[m] / len(models[m][0])):
        cost = models[m][2]
        if used + cost > budget:
            print(f"{m}: over the animated-object budget, stays static")
            continue
        kept.append(m)
        used += cost
    kept.sort()
    # again with one texture table for the kept types (VRAM: each texture uploaded once, model.c MAX_TEX 12)
    tex = ([], {})
    for m in list(kept):
        n = len(tex[0])
        r = anim_type_model(game, m, objbnd(game, m), models[m][5], tex)
        if len(tex[0]) > 12:
            print(f"{m}: over the 12 shared textures, stays static")
            del tex[0][n:]
            for k in [k for k, v in tex[1].items() if v >= n]:
                del tex[1][k]
            kept.remove(m)
            continue
        models[m] = r + (models[m][5],)
    types = [(m, models[m][0], models[m][1], models[m][2], models[m][4]) for m in kept]
    parts = [(p["name"], kept.index(p["model"]), p["pos"], souls._euler_deg_xzy(*p["rot"]), p.get("entity_id", -1),
              p.get("init_anim", -1) if p.get("init_anim") is not None else -1) for p in cand if p["model"] in kept]
    if types:
        print(f"animated objects: {len(tex[0])} shared textures {[(t[0], t[1]) for t in tex[0]]}")
    # event-animated objects left static (elevators, over budget / range): their anim lengths only, so the events
    # that wait for an anim (2003[18] wait for completion) take DSR's time
    statics = []
    for p in still:
        if p["model"] in kept or p.get("entity_id", -1) in (None, -1) or p.get("entity_id") not in ids:
            continue
        an = anibnd(objbnd(game, p["model"]), p["model"])
        lens = []
        for k in sorted(an or {}):
            if k.startswith("a00_") and k.endswith(".hkx") and len(lens) < 8:
                nf, fps, _, _ = ha.animation(an[k])
                lens.append((int(k[4:8]), (nf - 1) / fps if fps > 0 else 0.0))
        if lens:
            statics.append((p["entity_id"], lens))
            print(f"{p['name']} ({p['entity_id']}): static, event anims {[(a, round(l, 2)) for a, l in lens]}")
    return types, parts, anim_texture_model(tex) if types else b"", statics


def anim_texture(mb, name, d):
    """Shared texture index of DDS d (DXT, or BC7 as the chest o0110's) at 128 x ANIM_TEX_H texels at most (PSMT8 wants
    128 wide; VRAM: 2 KB each at 16, the default: animated textures stay resident from boot and
    the Burg block needs > 74 KB free at its load with enemies on; 32 and 64 broke world_cross), -1 without one"""
    if not d:
        return -1
    if name not in mb.tex_index:
        from PIL import Image
        from export_model import quantize
        dec = souls.dds_decode(d, 256)
        if dec:
            img = Image.frombytes("RGB", dec[:2], dec[2])
        else:
            dec = souls.dds_decode_rgba(d, 256)
            if not dec:
                return -1
            img = Image.frombytes("RGBA", dec[:2], dec[2]).convert("RGB")
        w, h = img.size
        h = max(8, min(ANIM_TEX_H, h * 128 // w))
        img = img.resize((128, h), Image.BILINEAR)
        mb.tex_index[name] = len(mb.textures)
        mb.textures.append(quantize(128, h, img.tobytes()))
    return mb.tex_index[name]


def anim_texture_model(tex):
    """model.bin with the pack's shared animated-object textures only (one bone, no triangles)"""
    mb = MeshBuilder({"t": 0})
    mb.textures = tex[0]
    out = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"export_objects_{os.getpid()}_tex.bin")
    write_model(out, ["t"], [-1], [((0, 0, 0), (0, 0, 0, 1))], mb, [])
    with open(out, "rb") as f:
        blob = f.read()
    os.unlink(out)
    return blob


def qconj_(q):
    return (-q[0], -q[1], -q[2], q[3])


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
        parts.append((ti, part["pos"], souls._euler_deg_xzy(*part["rot"]), part["entity_id"]))

    # RAM budget (HANDOFF: objects runtime <= 0.4 MB): keep the types with the most placements per byte first; the
    # dropped types are listed in <out>.static and export_map.py draws them as static map objects (intact, no break)
    budget = area_mod.config(map_id).get("obj_budget", OBJ_BUDGET) * 1048576 - 24 * 1024
    cost = [len(t[2]) + t[-1] + 128 for t in types]
    count = [sum(1 for p in parts if p[0] == k) for k in range(len(types))]
    order = sorted(range(len(types)), key=lambda k: -count[k] / cost[k])
    kept, used = set(), 0
    for k in order:
        # objects.c holds MAX_PIECES pieces per type: a type with more (the Asylum's o8610, 51) never loads there
        if used + cost[k] <= budget and len(types[k][3]) <= MAX_PIECES:
            kept.add(k)
            used += cost[k]
    dropped = sorted(types[k][0] for k in range(len(types)) if k not in kept)
    remap = {k: i for i, k in enumerate(sorted(kept))}
    types = [types[k] for k in sorted(kept)]
    heaps = [t[-1] for t in types]
    parts = [(remap[ti], pos, q, ent) for ti, pos, q, ent in parts if ti in remap]
    # .static / .anim are rewritten only when they change: map.bin depends on them, not on objects.bin
    srcsum.write_if_changed(out_path + ".static", "".join(m + "\n" for m in dropped))
    if dropped:
        print(f"over the {budget / 1048576:.2f} MB budget or {MAX_PIECES} pieces, drawn as static map objects instead: "
              f"{' '.join(dropped)}")

    atypes, aparts, atex, astatic = anim_objects(game, map_id, msb, keep, dds)
    srcsum.write_if_changed(out_path + ".anim", "".join(p[0] + "\n" for p in aparts))

    TYPE, PIECE, PART = 64, 80, 48
    type_off = 64
    piece_off = type_off + TYPE * len(types)
    part_off = piece_off + PIECE * sum(len(t[3]) for t in types)
    blob = bytearray(part_off + PART * len(parts))
    struct.pack_into("<4s5I", blob, 0, b"DSRB", 3, len(types), len(parts), type_off, part_off)
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
    for k, (ti, pos, q, ent) in enumerate(parts):
        struct.pack_into("<HH3f4fi3I", blob, part_off + PART * k, ti, 0, *pos, *q, ent, 0, 0, 0)
    # animated objects (version 3): types, bodies, parts after the breakables
    blob.extend(b"\0" * (-len(blob) % 16))
    atype_off = len(blob)
    blob += b"\0" * (32 * len(atypes))
    body_off = len(blob)
    nb = 0
    for k, (model, mbin, bods, heap, roots) in enumerate(atypes):
        struct.pack_into("<i5I2I", blob, atype_off + 32 * k, int(model[1:]), 0, len(mbin), len(bods), nb, heap, roots, 0)
        for bone, c, q, half in bods:
            blob += struct.pack("<i3f4f3fI", bone, *c, *q, *half, 0)
        nb += len(bods)
    apart_off = len(blob)
    for name, ti, pos, q, ent, init in aparts:
        blob += struct.pack("<HH3f4fii2I", ti, 0, *pos, *q, ent, init, 0, 0)
    for k, (model, mbin, bods, heap, roots) in enumerate(atypes):
        blob.extend(b"\0" * (-len(blob) % 128))
        struct.pack_into("<I", blob, atype_off + 32 * k + 4, len(blob))
        blob += mbin
    tex_off = 0
    if atex:
        blob.extend(b"\0" * (-len(blob) % 128))
        tex_off = len(blob)
        blob += atex
    static_off = len(blob)
    for ent, lens in astatic:
        lens = lens + [(-1, 0.0)] * (8 - len(lens))
        blob += struct.pack("<iI", ent, sum(1 for a, _ in lens if a >= 0)) + b"".join(struct.pack("<if", a, l) for a, l in lens)
    struct.pack_into("<10I", blob, 24, len(atypes), len(aparts), atype_off, apart_off, body_off, tex_off, len(astatic),
                     static_off, 0, 0)
    anim_ram = sum(t[3] for t in atypes)
    with open(out_path, "wb") as f:
        f.write(blob)
    print(f"{len(atypes)} animated types ({' '.join(t[0] for t in atypes)}), {len(aparts)} parts, models + skin streams "
          f"{anim_ram / 1048576:.3f} MB (budget {ANIM_BUDGET} MB)")
    print(f"{len(types)} breakable types, {len(parts)} parts, {len(blob)} bytes -> {out_path}; EE RAM with the skin "
          f"streams and objects.c / physics.c pools (~24 KB): {(len(blob) + sum(heaps) + 24 * 1024) / 1048576:.3f} MB")


if __name__ == "__main__":
    main()
