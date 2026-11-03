"""DS1/DSR map collision (h*.hkx / l*.hkx in the map's .hkxbhd/.hkxbdt) -> world-space triangles.

Each file is a Havok 2015 tagfile: hkpPhysicsData -> hkpRigidBody -> hkpMoppBvTreeShape ->
CustomParamStorageExtendedMeshShape (hkpStorageExtendedMeshShape). Per subpart i:
  trianglesSubparts[i]  hkpExtendedMeshShape::TrianglesSubpart (numTriangleShapes, transform hkQsTransform)
  meshstorage[i]        MeshSubpartStorage: vertices hkVector4[], indices16 (4 u16 per triangle: a, b, c, 0)
  materialArray[i]      CustomMeshParameter: materialNameData = the subpart's hit material id (sound/footstep)
Vertices are in the model's own space and every placement applies its MSB part transform (_place). Most parts sit
at the origin (their hkx is authored in map space), but not all: Firelink h0017B2_0002 / h8000B2_0001, Undead Burg
h1120B1 (z -10, lands on its map pieces only with the offset: 2328 vs 1331 map vertices in its box) and
h1087B1_0000 (a second copy of h1087B1 128.8 m away). Layout after SoulsAssetPipeline / HKX2 (hkpStorageExtendedMeshShape)."""
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
        raise ValueError(f"subpart with {ntri} triangles has no index array")
    if not ntri:
        return (), 3  # empty model: the stride may be anything
    size = struct.calcsize(code)
    per = stride // size
    if stride % size or per < 3:
        raise ValueError(f"indexStriding {stride} does not fit {field} ({size} B per index)")
    need = per * (ntri - 1) + 3
    assert ni >= need, (field, ni, ntri, stride)
    return struct.unpack_from(f"<{need}{code}", h.data, io), per


def hkx_meshes(hkx_bytes, arrays=False):
    """Collision hkx -> [(vertices [(x, y, z)], triangles [(a, b, c)], material id)] one per subpart. arrays: the
    vertices as an (n, 3) float64 array (numpy), the same values."""
    h = souls.HkTag(hkx_bytes)
    d = h.data
    out = []
    for tname in ("CustomParamStorageExtendedMeshShape", "hkpStorageExtendedMeshShape"):
        shapes = h.objects(tname)
        if shapes:
            break
    else:
        names = {t.name for t, _, _ in h.items if t}
        raise ValueError("no hkpStorageExtendedMeshShape; types: " + ", ".join(sorted(names)))
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
