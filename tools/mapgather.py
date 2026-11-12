"""The map exporter's gather: MSB parts and objects to meshes (welding, wedge light, sky, navmesh) (2026-11-12, code moved as it was; tools/export_map.py runs it)."""
import sys
import collections.abc, json, math, os, struct, time
import souls
import havok_anim as ha
import texmap
import resample
import export_normal
import dsr_light
import area as area_mod
import pose_obj
import drawgroups
import emevd
import mapx_native
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapcfg
import mapchunks
import maplight
import mapsimp


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
    ln = np.sqrt(mapchunks._py_sum3(N[:, 0] * N[:, 0], N[:, 1] * N[:, 1], N[:, 2] * N[:, 2]))
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
        k = np.array([x ** 0.5 or 1.0 for x in mapchunks._py_sum3(v[:, 0] * v[:, 0], v[:, 1] * v[:, 1], v[:, 2] * v[:, 2]).tolist()])
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
    if np.sqrt((gap ** 2).sum(1)).min() > mapcfg.SKY_DIST:
        return True
    MX = mapx_native.lib()
    if MX is not None:  # the same squared distances, stopping at the first one in range (tools/mapx/rows.c)
        Pc, Sc = np.ascontiguousarray(P, np.float64), np.ascontiguousarray(nav_tree, np.float64)
        return not MX.mx_any_within(len(Pc), Pc.ctypes.data, len(Sc), Sc.ctypes.data, mapcfg.SKY_DIST ** 2)
    for i in range(0, len(P), 2048):
        d = ((P[i:i + 2048, None, :] - nav_tree[None]) ** 2).sum(2).min()
        if d <= mapcfg.SKY_DIST ** 2:
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
    if model + "_1.hkx" in files and model not in mapcfg.STATIC_BREAKABLES and not static:
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
    ids = maplight.part_draw_ids(msb)
    light_rows, fog_rows, tone_rows = maplight.draw_params(game, area)
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
    # dg.bin: DSR compares a piece's draw groups with those of the collision part under the player in whichever map
    # that part belongs to (one bit space for the maps an area load list keeps resident together: m10_00 / _01 / _02;
    # Burg collisions show 41-63 of Firelink's 67 pieces near the aqueduct, none deep inside). Pieces are merged into
    # sets over every collision of those maps, so a set decides alike from another block's floor too (DG_GROUPS=0: the
    # region's own collisions only, the old per-block sets)
    mapchunks.DG.collisions(drawgroups.group_collisions(game, map_id, M, reg_coll))
    mapchunks.DG.area = drawgroups.area_group(game, map_id)
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
                return base - max(ys) > mapchunks.CANOPY_H
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
                                    {p["name"]: maplight.probe_id(p.get("env_event")) for p in M["parts"] if p["type"] == 5})
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
    mapcfg.SCATTER[0] = dsr_light.Scatter(banks["LightScattering"][sc_id], area, sc_id) if sc_id is not None else None
    print(f"spawn collision {spawn_part}: tone map {main_tone}, scattering {sc_id}")
    near = mapcfg.SCATTER[0].near_factor() if mapcfg.SCATTER[0] else [1.0, 1.0, 1.0]
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
    switched = emevd.switched_entities(game, map_id) if mapchunks.EVENT_PARTS else set()
    for pi, (part, (lid, fid)) in enumerate(zip(M["parts"], ids)):
        for k in expire.get(pi - 1, ()):
            evict(k)
        if part["type"] in (0, 1) and not area_mod.drawn(vis, part["draw_groups"]):
            continue  # never drawn from the area's region
        far = part["type"] == 1 and not keep(*part["pos"])
        if part["type"] == 1 and not far and part["name"] in mapcfg.ANIM_PARTS:
            continue
        # objects beyond the region but inside map_radius are scenery like the map triangles around them: drawn as
        # static objects, breakables intact (the cropped collision ends at the region, nothing can reach them)
        if far and not (mapcfg.FAR_OBJ and map_r and in_map(*part["pos"])):
            continue
        if part["type"] == 1 and nav_tree is not None and \
                np.min(np.sum((nav_tree - np.array(part["pos"])) ** 2, axis=1)) > mapcfg.PARK_DIST ** 2:
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
        if part["model"] in mapcfg.KEEP_MODELS:
            mapcfg.KEEP_POS.append(part["pos"])
        pxf, pxn = place(part)
        bind = bind_scaled(F)
        for m in F["meshes"]:
            if part["type"] == 1 and m["dynamic"]:
                # skinned objects (corpses, o1950 chains): baked in the pose of their MSB init_anim (tools/pose_obj.py);
                # in bind pose they would T-pose
                pk = (part["model"], part.get("init_anim"))
                if pk not in poses:
                    poses[pk] = pose_obj.anim_bones(game, *pk) if mapcfg.POSE_OBJ else None
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
            sky_slot = 0
            if sky and fid in fog_rows:
                sky_fog = fog_rows[fid]
                if fid not in mapcfg.SKY_FOG_ROWS and len(mapcfg.SKY_FOG_ROWS) < 15:
                    mapcfg.SKY_FOG_ROWS.append(fid)
                sky_slot = mapcfg.SKY_FOG_ROWS.index(fid) + 1 if fid in mapcfg.SKY_FOG_ROWS else 0
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
            flags = (mapcfg.CHUNK_BLEND if blend else 0) | (mapcfg.CHUNK_ADD if "_add" in m["mtd"].lower() else 0) | \
                (mapcfg.CHUNK_SKY if sky else 0) | sky_slot << mapcfg.CHUNK_SKY_ROW
            if not m["dynamic"] and 0 <= m["default_bone"] < len(bind):
                xf, xn = bone_xf(pxf, pxn, *bind[m["default_bone"]])
            else:
                xf, xn = pxf, pxn
            grass = mapchunks.name_is_grass(m["diffuse"])
            lods = {f[0] & 0x0F000000: f for f in m["facesets"] if not f[0] & 0x80000000}
            if not lods:
                continue
            # objects beyond the region (70 m+ from the spawn in Burg) take the game's lowest LOD, as DSR draws them
            # at that range (o1154 barrel + ropes: 5.9k -> 1.2k tris before QEM)
            low = mapcfg.GAME_LOD or far
            if mapcfg.FAR_LOD and not low and part["type"] == 0 and len(lods) > 1 and nav_tree is not None:
                # map pieces whose LOD0 lies wholly beyond STREAM_FAR of the walkable space (whole-MSB areas) take the
                # game's lowest LOD before QEM, as DSR draws them from there (render-distance option (a), map-36)
                Vp = m["verts"]["pos"]
                wp = xf.many(_vert_np(Vp)[np.unique(faceset_tris(lods[min(lods)]))])
                lo, hi = wp.min(0), wp.max(0)
                d = np.maximum(0, np.maximum(lo[None] - nav_tree, nav_tree - hi[None]))
                low = len(wp) and np.min((d * d).sum(1)) > mapcfg.STREAM_FAR ** 2
                mapcfg.FAR_LOD_STATS[bool(low)] += 1
            fs = lods[max(lods)] if low else lods[min(lods)]
            dk = (id(fs), id(m["verts"]["pos"]))  # a model placed many times: its face set is deduplicated once
            if dk not in _DEDUP or _DEDUP[dk][0] is not fs or _DEDUP[dk][1] is not m["verts"]["pos"]:
                _DEDUP[dk] = (fs, m["verts"]["pos"], dedup_tris(faceset_tris(fs), m["verts"]))
            tl = list(_DEDUP[dk][2])
            if (map_r or mapsimp.PANO_R) and not sky:  # each used vertex placed once (was 9x per triangle: 11.7M in Burg)
                ui = list({i for t in tl for i in t})
                Wp = dict(zip(ui, map(tuple, xf.many(_vert_np(m["verts"]["pos"])[ui]).tolist())))
            if map_r and not sky:  # area map radius (tools/area.py): triangles centred beyond it are dropped
                if mapcfg.WS_SHARE_TRIS:  # the whole MSB's source triangles per 32 m cell, before the cut (ws_shares)
                    G = mapcfg.CHUNK_GRID
                    for t in tl:
                        ck = tuple(math.floor(sum(Wp[i][k] for i in t) / 3 / G) for k in range(3))
                        mapcfg.WS_SRC_TRIS[ck] = mapcfg.WS_SRC_TRIS.get(ck, 0) + 1
                tl = [t for t in tl if in_map(*(sum(Wp[i][k] for i in t) / 3 for k in range(3)))]
            if mapsimp.PANO_R and not sky:  # far panorama (tools/pano.py): beyond PANO_R of PANO_C it replaces the map
                pc = mapsimp.PANO_C or sp
                far_t = lambda t: sum((sum(Wp[i][k] for i in t) / 3 - pc[k]) ** 2 for k in range(3)) > mapsimp.PANO_R ** 2
                tl = [t for t in tl if far_t(t) == mapsimp.PANO_ONLY]
            fol, gpos = None, None
            if grass and MX is not None and m["verts"]["pos"] and type(m["verts"]["pos"][0][0]) is float:
                tl, fol, gpos = mapchunks.thin_foliage_c(MX, tl, m["verts"]["pos"], f"{part['name']}/{len(meshes)}",
                                               "ivy" in m["mtd"].lower(), xf.many(_vert_np(m["verts"]["pos"])), mx_grid)
            elif grass:
                tl, fol, gpos = mapchunks.thin_foliage(tl, m["verts"]["pos"], f"{part['name']}/{len(meshes)}",
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
                    dec = souls.dds_decode_rgba(dds[name], mapcfg.TEX_MAX)
                    if dec and export_normal.BAKE and not blend and m.get("bump") in dds and export_normal.bumped(m["mtd"]):
                        baked = export_normal.bake_rgba(*dec, dds[m["bump"]])  # the relief in the texels (BUMP_BAKE)
                        dec = (dec[0], dec[1], baked) if baked is not None else dec
                    if dec:
                        tex_index[name, blend] = len(textures)
                        textures.append(texmap.encode(*dec, soft=blend, name=name))
                        if not blend:
                            mapchunks.near_level(textures[-1], dds[name], name)
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
            spill = max(1.0, max(tint) / 2) if lit is None and flags & mapcfg.CHUNK_ADD else 1.0
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
                               keep=grass or blend or sky or part["model"] in mapcfg.KEEP_MODELS, name=f"{part['name']}/{name}",
                               dg=mapchunks.DG.add(part["draw_groups"]),
                               ent=part["entity_id"] if part["entity_id"] in switched else 0,
                               # PVS unit and occluder class: only static, opaque map pieces hide what is behind them
                               # (objects move or break, cut-outs and blends let light through)
                               piece=part["name"],
                               # cut-out cards (ivy): each leaf is a piece of its own for mapsimp.loose_pieces
                               cutout="_alp" in m["mtd"].lower() or "_edge" in m["mtd"].lower(),
                               occ=int(part["type"] == 0 and not blend and not grass and not sky and not water and
                                       "_alp" not in m["mtd"].lower() and "_edge" not in m["mtd"].lower() and
                                       part["entity_id"] not in switched)))

    if mx_grid is not None:
        MX.mx_navgrid_free(mx_grid)
    souls.VERT_NP = None
    souls.VERT_ROWS = False
    _VERT_NP.clear()  # per-FLVER arrays and deduplicated face sets: only for placing meshes
    _DEDUP.clear()
    mapchunks.rss(f"gather meshes ({len(meshes)})")
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
    if sh and mapcfg.SHADOWS:
        t0 = time.time()
        occ = [_mesh_np(m)[0][_mesh_np(m)[1][_np_list(m, "tris", m["tris"], np.intp, 3)]]
               for m in meshes if not m["flags"] & (mapcfg.CHUNK_BLEND | mapcfg.CHUNK_SKY) and not m["grass"] and m["tris"]]
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
            h["H"] = h["E"] * (1 - mapcfg.SHADOW_AMB * (1 - k))[:, None] + h["S"] * k[:, None]
        nv = sum(len(h["E"]) for _, h, *_ in pending)
        mapcfg.SUN_FLOOR[0] = floor[0] / max(1, floor[1])
        print(f"floors in sun: {100 * mapcfg.SUN_FLOOR[0]:.0f}%")
        print(f"sun shadow map {smap.W}x{smap.H} ({len(occ)} tris, {time.time() - t0:.1f} s): {100 * lit_n / max(1, nv):.0f}% "
              f"of {nv} vertices lit")
    else:
        for _, h, *_ in pending:
            h["H"] = h["E"] + h["S"]
    mapchunks.rss("shadows")
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
    mapcfg.TONES.update(tones)  # fog colours and the character light use the same tone maps
    mapcfg.MAIN_TONE[0] = tones[main_tone]

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
    print(f"foliage: {mapchunks.CANOPY_STATS[0]} canopy / wall tris (CANOPY_H {mapchunks.CANOPY_H} m) kept whole, never thinned")
    if mapcfg.FAR_LOD_STATS[1]:
        print(f"far LOD: {mapcfg.FAR_LOD_STATS[1]} of {sum(mapcfg.FAR_LOD_STATS)} map meshes with game LODs take the lowest "
              f"(beyond {mapcfg.STREAM_FAR:.0f} m of the navmesh)")
    return meshes, textures, nav_v, nav_t, spawn, (main_fog, fog_rows[main_fog]), sky_fog
