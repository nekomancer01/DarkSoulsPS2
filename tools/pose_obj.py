"""Skinned MSB objects (corpses o0500 / o0502, hanging chains o1950, ...) as static map geometry: each FLVER mesh
skinned once into the pose DSR shows at load, so tools/export_map.py can bake it like a map piece (TECH_DEBT map-4).

DSR plays an object's MSB init_anim (a00_<id>.hkx in its objbnd's anibnd) from load; corpses hold a single still pose
(their anims are 1-2 frames), so frame 0 is what the player sees. init_anim -1: the anibnd's first a00 animation
(o1950's only one is a00_0000; ASSUMED for the others, EMEVD may pick the pose). Linear blend skinning with the Havok
skeleton's reference pose as bind (same as tools/pose_check.py, verified on c0000)."""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import havok_anim as ha


def anim_bones(game, model, init_anim):
    """-> {bone name: (bind (t, q), pose (t, q))} in model space, or None (no anibnd / skeleton / animation)."""
    path = os.path.join(game, "obj", model + ".objbnd.dcx")
    files = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(path))}
    if model + ".anibnd" not in files:
        return None
    ani = {souls.basename(n).lower(): d for _, n, d in souls.bnd3(files[model + ".anibnd"])}
    anims = sorted(k for k in ani if k.startswith("a00_") and k.endswith(".hkx"))
    if "skeleton.hkx" not in ani or not anims:
        return None
    want = f"a00_{init_anim:04d}.hkx" if init_anim is not None and init_anim >= 0 else None
    names, parents, ref = ha.skeleton(ani["skeleton.hkx"])
    nf, _, t2b, frames = ha.animation(ani[want if want in ani else anims[0]])
    local = list(ref)
    for tr, b in enumerate(t2b):
        if 0 <= b < len(local):
            local[b] = frames[0][tr]
    bind, pose = ha.world(parents, ref), ha.world(parents, local)
    return {n: (bind[i], pose[i]) for i, n in enumerate(names)}


def skin_mesh(F, m, bones):
    """Posed (positions, normals) of a dynamic mesh's vertices, model space; vertices of bones the skeleton lacks
    keep their bind position."""
    V = m["verts"]
    out_p, out_n = [], []
    fb = F["bones"]
    for vi, p in enumerate(V["pos"]):
        n = V["normal"][vi] if V["normal"] else (0.0, 1.0, 0.0)
        infl = []
        if V["bone_idx"]:
            for b, w in zip(V["bone_idx"][vi], V["bone_w"][vi] if V["bone_w"] else (1.0, 0, 0, 0)):
                if w > 0 and b < len(m["bones"]) and fb[m["bones"][b]][0] in bones:
                    infl.append((bones[fb[m["bones"][b]][0]], w))
        if not infl:
            out_p.append(tuple(p))
            out_n.append(tuple(n))
            continue
        tw = sum(w for _, w in infl)
        acc, nacc = [0.0] * 3, [0.0] * 3
        for ((bt, bq), (t, q)), w in infl:
            iq = (-bq[0], -bq[1], -bq[2], bq[3])
            r = ha.qrot(q, ha.qrot(iq, (p[0] - bt[0], p[1] - bt[1], p[2] - bt[2])))
            rn = ha.qrot(q, ha.qrot(iq, n))
            for k in range(3):
                acc[k] += w / tw * (t[k] + r[k])
                nacc[k] += w / tw * rn[k]
        out_p.append(tuple(acc))
        out_n.append(tuple(nacc))
    return out_p, out_n
