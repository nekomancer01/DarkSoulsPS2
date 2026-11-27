"""DS1/DSR map collision (h*.hkx / l*.hkx in the map's .hkxbhd/.hkxbdt) -> world-space triangles.

Each file is a Havok 2015 tagfile: hkpPhysicsData -> hkpRigidBody -> hkpMoppBvTreeShape ->
CustomParamStorageExtendedMeshShape (hkpStorageExtendedMeshShape). Per subpart i:
  trianglesSubparts[i]  hkpExtendedMeshShape::TrianglesSubpart (numTriangleShapes, transform hkQsTransform)
  meshstorage[i]        MeshSubpartStorage: vertices hkVector4[], indices16 (4 u16 per triangle: a, b, c, 0)
  materialArray[i]      CustomMeshParameter: materialNameData = the subpart's hit material id (sound/footstep)
Vertices are in the model's own space and every placement applies its MSB part transform (_place). Most parts sit
at the origin (their hkx is authored in map space), but not all: Firelink h0017B2_0002 / h8000B2_0001, Undead Burg
h1120B1 (z -10, lands on its map pieces only with the offset: 2328 vs 1331 map vertices in its box) and
h1087B1_0000 (a second copy of h1087B1 128.8 m away). Layout after SoulsAssetPipeline / HKX2 (hkpStorageExtendedMeshShape).
A few files hold an hkpConvexVerticesShape instead (scan-8: m13_02 h/l9200B2, the NPC-only wall of the Great Hollow,
and m14_00 l8999B0; unplaced ones in m14_00 / m16_00): its hull is triangulated here (convex_hull_tris), so the
runtime sees plain triangles like a mesh subpart's."""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls

# MSB1 collision HitFilterID (soulstruct darksouls1ptde CollisionHitFilter, "courtesy of horkrux")
HIT_FILTERS = {0: "NoHiHitNoFeetIK", 8: "Normal", 9: "Water_A", 11: "NPCOnly_A", 13: "DeathCam", 14: "LethalFall",
               15: "KillPlane", 16: "Water_B", 17: "GroupSwitch", 19: "NPCOnly_B", 20: "LevelExit_A", 21: "Slide",
               22: "FallProtection", 23: "LevelExit_B"}


def msb_collisions(msb_bytes):
    """MSB1 collision parts (type 5) -> [dict(name, model, pos, rot, scale, hit_filter, draw_groups, entity, ...)],
    from souls.msb1's named fields (SoulsFormats MSB1 Part + Collision type data)."""
    out = []
    for p in souls.msb1(msb_bytes)["parts"]:
        if p["type"] != 5:
            continue
        out.append({"name": p["name"], "model": p["model"], "pos": p["pos"], "rot": p["rot"], "scale": p["scale"],
                    "draw_groups": p["draw_groups"], "entity": p["entity_id"], "hit_filter": p["hit_filter"],
                    "sound_space": p["sound_space"], "disable_start": p["disable_start"],
                    "play_region": p["play_region_id"], "lock_cam": tuple(p["lock_cam_ids"])})
    return out


def _qs_apply(qs, v):
    """hkQsTransform (t xyz_, q xyzw, s xyz_) applied to a point."""
    t, q, s = qs[0:3], qs[4:8], qs[8:11]
    r = souls._qrot(q, (v[0] * s[0], v[1] * s[1], v[2] * s[2]))
    return r[0] + t[0], r[1] + t[1], r[2] + t[2]


# hkpExtendedMeshShape::IndexStridingType (TrianglesSubpart.stridingType) -> MeshSubpartStorage array, struct code
_INDEX_ARRAYS = {1: ("indices8", "B"), 2: ("indices16", "H"), 3: ("indices32", "I")}


def _indices(h, ms, M, so_k, sub_t, ntri, stride):
    """(flat index tuple, indices per triangle) of one subpart. DSR's maps use indices16 with a stride of 8 bytes (3
    indices + 1 pad); the m99 test maps also have other strides (formats-6). The array is the one stridingType names;
    when that field is missing or names an empty array, the first non-empty one of indices16 / 32 / 8."""
    try:
        order = [_INDEX_ARRAYS[h.get(so_k, sub_t, "stridingType", "B")]]
    except KeyError:
        order = []
    order += [_INDEX_ARRAYS[t] for t in (2, 3, 1)]
    for field, code in order:
        try:
            _, io, ni = h.item(ms, M, field)
        except KeyError:
            continue  # no such array in this hkx version
        if ni or not ntri:
            break
    else:
        raise souls.UnknownVariant("havok collision index array", "none", f"subpart with {ntri} triangles")
    if not ntri:
        return (), 3  # empty model: the stride may be anything
    size = struct.calcsize(code)
    per = stride // size
    if stride % size or per < 3:
        raise ValueError(f"indexStriding {stride} does not fit {field} ({size} B per index)")
    need = per * (ntri - 1) + 3
    assert ni >= need, (field, ni, ntri, stride)
    return struct.unpack_from(f"<{need}{code}", h.data, io), per


def convex_shape(h, so):
    """hkpConvexVerticesShape at DATA offset so -> dict(vertices [(x, y, z)], planes [(nx, ny, nz, d)], radius).
    rotatedVertices is hkFourTransposedPoints[] (48 bytes: x0-x3, y0-y3, z0-z3; the last block padded), of which
    numVertices are the hull's; planeEquations: n . p + d <= 0 inside, n the outward normal (Havok hkpConvexVerticesShape)."""
    T = h.type("hkpConvexVerticesShape")
    nv = h.get(so, T, "numVertices", "i")
    _, vo, nblk = h.item(so, T, "rotatedVertices")
    _, po, npl = h.item(so, T, "planeEquations")
    if nv < 3 or nv > 4 * nblk or npl < 2:  # the least: a flat triangle (m14_00 l8999B0, 3 vertices, 5 planes)
        raise souls.UnknownVariant("havok convex shape", f"{nv} vertices in {nblk} blocks, {npl} planes",
                                   "hkpConvexVerticesShape without a readable hull")
    verts = []
    for j in range(nv):
        f = struct.unpack_from("<12f", h.data, vo + 48 * (j // 4))
        verts.append((f[j % 4], f[4 + j % 4], f[8 + j % 4]))
    planes = [struct.unpack_from("<4f", h.data, po + 16 * k) for k in range(npl)]
    return {"vertices": verts, "planes": planes, "radius": h.get(so, T, "radius", "f")}


def convex_hull_tris(shape):
    """A convex shape (convex_shape) -> [(p0, p1, p2)] triangles of its hull. Per plane, the hull vertices that lie
    furthest out along its normal (within 1e-4 of the hull's size of the furthest: the plane equations may sit a few
    cm off the vertices, m16_00 h7100B0), in order around the normal, as a fan; a plane with fewer than 3 such vertices
    (an edge) gives none, and zero-area fan triangles are dropped. Wound as DS collision is: the cross product
    (p1 - p0) x (p2 - p0) points into the hull (a floor faces up with a negative cross, tools/worldreach.py). A flat
    hull (every vertex on one plane: m13_02 h9200B2's quad, m14_00 l8999B0's triangle, m16_00 h7100B0's 16-gon, which
    has no plane for its faces) is its polygon, both sides. A rounded hull (convex radius > 0) is not a plain
    polyhedron: UnknownVariant."""
    if shape["radius"]:
        raise souls.UnknownVariant("havok convex radius", f"{shape['radius']:g}", "hkpConvexVerticesShape with a radius")
    verts = list(dict.fromkeys(tuple(v) for v in shape["vertices"]))  # padding / repeated points once
    lo = [min(v[i] for v in verts) for i in range(3)]
    hi = [max(v[i] for v in verts) for i in range(3)]
    tol = 1e-4 * max(1e-3, max(b - a for a, b in zip(lo, hi)))
    dot = lambda a, b: a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    sub = lambda a, b: (a[0] - b[0], a[1] - b[1], a[2] - b[2])
    cross = lambda a, b: (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    import math

    def fan(face, n, out):
        """face's vertices as triangles wound inward for the outward normal n"""
        c = tuple(sum(v[i] for v in face) / len(face) for i in range(3))
        u = sub(max(face, key=lambda v: dot(sub(v, c), sub(v, c))), c)
        w = cross(n, u)
        face = sorted(face, key=lambda v: math.atan2(dot(sub(v, c), w), dot(sub(v, c), u)))  # counter-clockwise about n
        for i in range(1, len(face) - 1):
            p0, p1, p2 = face[0], face[i + 1], face[i]  # clockwise about n: the cross points inward
            cr = cross(sub(p1, p0), sub(p2, p0))
            if dot(cr, cr) > (tol * tol) ** 2:
                out.append((p0, p1, p2))

    out = []
    p0 = verts[0]
    p1 = max(verts, key=lambda v: dot(sub(v, p0), sub(v, p0)))
    n = max((cross(sub(p1, p0), sub(v, p0)) for v in verts), key=lambda c: dot(c, c))
    ln = dot(n, n) ** 0.5
    if ln <= tol * tol:
        return out  # a point or a line: no surface
    n = (n[0] / ln, n[1] / ln, n[2] / ln)
    if all(abs(dot(n, sub(v, p0))) <= tol for v in verts):
        fan(verts, n, out)
        fan(verts, (-n[0], -n[1], -n[2]), out)
        return out
    for nx, ny, nz, _ in shape["planes"]:
        n = (nx, ny, nz)
        top = max(dot(n, v) for v in verts)
        face = [v for v in verts if dot(n, v) >= top - tol]
        if len(face) >= 3:
            fan(face, n, out)
    return out


def _convex_meshes(h, arrays):
    """every hkpConvexVerticesShape of a file as hkx_meshes entries (no hit material: -1)"""
    out = []
    for so in h.objects("hkpConvexVerticesShape"):
        tris = convex_hull_tris(convex_shape(h, so))
        index = {}
        for t in tris:
            for p in t:
                index.setdefault(p, len(index))
        verts = list(index)
        if arrays:
            import numpy as np
            verts = np.array(verts, np.float64).reshape(len(verts), 3)
        out.append((verts, [tuple(index[p] for p in t) for t in tris], -1))
    return out


def hkx_meshes(hkx_bytes, arrays=False):
    """Collision hkx -> [(vertices [(x, y, z)], triangles [(a, b, c)], material id)] one per subpart, then one per
    convex shape (convex_hull_tris). arrays: the vertices as an (n, 3) float64 array (numpy), the same values."""
    h = souls.HkTag(hkx_bytes)
    d = h.data
    out = []
    for tname in ("CustomParamStorageExtendedMeshShape", "hkpStorageExtendedMeshShape"):
        shapes = h.objects(tname)
        if shapes:
            break
    else:
        convex = _convex_meshes(h, arrays)
        if convex:
            return convex
        names = {t.name for t, _, _ in h.items if t}
        raise souls.UnknownVariant("havok collision shape", ",".join(sorted(n for n in names if "Shape" in n)) or "none",
                                   "no hkpStorageExtendedMeshShape or hkpConvexVerticesShape")
    for so in shapes:
        S = h.type(tname)
        sub_t, sub_o, nsub = h.item(so, S, "trianglesSubparts")
        _, ms_o, _ = h.item(so, S, "meshstorage")
        mats = []
        if tname.startswith("Custom"):
            _, ma_o, nma = h.item(so, S, "materialArray")
            mats = [struct.unpack_from("<I", d, ma_o + 8 * k)[0] for k in range(nma)]  # T* -> item index
        M = h.type("hkpStorageExtendedMeshShape::MeshSubpartStorage")
        for k in range(nsub):
            so_k = sub_o + sub_t.size * k
            ntri = h.get(so_k, sub_t, "numTriangleShapes", "i")
            stride = h.get(so_k, sub_t, "indexStriding", "H")  # bytes from one triangle's indices to the next
            toff, _ = sub_t.field("transform")
            qs = struct.unpack_from("<12f", d, so_k + toff)
            ms = h.items[struct.unpack_from("<I", d, ms_o + 8 * k)[0]][1]
            _, vo, nv = h.item(ms, M, "vertices")
            if arrays:
                verts = _verts_np(d, vo, nv, qs)
            else:
                verts = [struct.unpack_from("<3f", d, vo + 16 * i) for i in range(nv)]
                if qs[:3] != (0, 0, 0) or qs[4:8] != (0, 0, 0, 1) or qs[8:11] != (1, 1, 1):
                    verts = [_qs_apply(qs, v) for v in verts]
            idx, per = _indices(h, ms, M, so_k, sub_t, ntri, stride)
            tris = [idx[per * i:per * i + 3] for i in range(ntri)]
            mat = -1
            if k < len(mats):
                C = h.type("CustomMeshParameter")
                mat = h.get(h.items[mats[k]][1], C, "materialNameData", "I")
            out.append((verts, tris, mat))
    return out


def _qrot_np(q, V):
    """souls._qrot on the rows of an (n, 3) float64 array: the same operations per element, in the same order"""
    x, y, z, w = q
    vx, vy, vz = V[:, 0], V[:, 1], V[:, 2]
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    import numpy as np
    return np.stack([vx + w * tx + y * tz - z * ty, vy + w * ty + z * tx - x * tz, vz + w * tz + x * ty - y * tx], 1)


def _verts_np(d, vo, nv, qs):
    """a subpart's hkVector4 vertices (xyz of 16 bytes) as float64, the subpart transform applied as _qs_apply"""
    import numpy as np
    V = np.frombuffer(d, "<f4", 4 * nv, vo).reshape(nv, 4)[:, :3].astype(np.float64)
    if qs[:3] != (0, 0, 0) or qs[4:8] != (0, 0, 0, 1) or qs[8:11] != (1, 1, 1):
        t, s = qs[0:3], qs[8:11]
        V = _qrot_np(qs[4:8], np.stack([V[:, 0] * s[0], V[:, 1] * s[1], V[:, 2] * s[2]], 1))
        V = np.stack([V[:, 0] + t[0], V[:, 1] + t[1], V[:, 2] + t[2]], 1)
    return V


def _place_np(part, V):
    """_place on the rows of an (n, 3) array"""
    import numpy as np
    sx, sy, sz = part["scale"]
    q = souls._euler_deg_xzy(*part["rot"])
    R = _qrot_np(q, np.stack([V[:, 0] * sx, V[:, 1] * sy, V[:, 2] * sz], 1))
    p = part["pos"]
    return np.stack([R[:, 0] + p[0], R[:, 1] + p[1], R[:, 2] + p[2]], 1)


def _place(part, v):
    sx, sy, sz = part["scale"]
    q = souls._euler_deg_xzy(*part["rot"])
    r = souls._qrot(q, (v[0] * sx, v[1] * sy, v[2] * sz))
    p = part["pos"]
    return r[0] + p[0], r[1] + p[1], r[2] + p[2]


NP = os.environ.get("HAVOK_NP", "1") == "1"  # numpy for the vertices (the same values); 0: the per-vertex code
_MEMO = {}  # the last map_collision result: export_map asks for it several times per export (13.6 s of Burg's)


def map_collision(game, area="m10_02_00_00", detail="h"):
    """All MSB-placed collision of a map -> list of dict(part, model, hit_filter, material, tris=[(p0, p1, p2)])
    with world-space points. Models without a file in the binder are reported in 'missing'. The last result is kept
    (callers must not modify it; forget() drops it)."""
    key = (game, area, detail)
    if key not in _MEMO:
        _MEMO.clear()
        _MEMO[key] = _map_collision(game, area, detail)
    return _MEMO[key]


def forget():
    _MEMO.clear()


def _map_collision(game, area, detail):
    mdir = f"{game}/map/{area}/{detail}{area[1:]}"
    files = {souls.basename(n).split(".")[0][:7]: b for _, n, b in souls.bxf3(mdir + ".hkxbhd", mdir + ".hkxbdt")}
    parts = msb_collisions(souls.read(f"{game}/map/MapStudio/{area}.msb"))
    meshes, out, missing = {}, [], []
    for p in parts:
        model = detail + p["model"][1:]
        if model not in files:
            missing.append(p["name"])
            continue
        if model not in meshes:
            meshes[model] = hkx_meshes(files[model], arrays=NP)
        for verts, tris, mat in meshes[model]:
            wv = list(map(tuple, _place_np(p, verts).tolist())) if NP else [_place(p, v) for v in verts]
            out.append({"part": p["name"], "model": model, "hit_filter": p["hit_filter"], "material": mat,
                        "msb": p, "tris": [(wv[a], wv[b], wv[c]) for a, b, c in tris]})
    unused = sorted(set(files) - {detail + p["model"][1:] for p in parts})
    return out, missing, unused


if __name__ == "__main__":
    import sys, collections
    import gamepath
    game = gamepath.game_dir(sys.argv[1] if len(sys.argv) > 1 else None)
    for detail in "hl":
        subs, missing, unused = map_collision(game, detail=detail)
        by = collections.Counter()
        for s in subs:
            by[s["hit_filter"]] += len(s["tris"])
        print(f"{detail}: {len(subs)} subparts, {sum(by.values())} tris; by hit filter:",
              {f"{k} {HIT_FILTERS.get(k, '?')}": v for k, v in sorted(by.items())},
              "missing:", missing, "unplaced files:", unused)
