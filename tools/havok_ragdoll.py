"""Character ragdoll from chr/c####.chrbnd c####.hkx (Havok 2015 tagfile): hkaRagdollInstance -> hkpRigidBody list
(one hkpCapsuleShape each, in the body frame) + hkpRagdollConstraintData joints, and the hkaSkeletonMapper that ties
each ragdoll bone to a bone of the animation skeleton. DSR uses these bodies twice: as the character's hit capsules
(damage is taken where a weapon meets a body) and as the death ragdoll.

ragdoll(hkx) -> dict(
  bodies=[dict(name, bone (animation skeleton bone name), world=(t, q) bind frame of the body, a, b (capsule ends in
               the body frame), radius, mass, friction)],
  joints=[dict(child, parent (body indices), pivot_child, pivot_parent (body frames), axis_child, axis_parent (the
               twist axis in each body's frame), cone (rad), twist_min, twist_max, plane_min, plane_max)])
None when the file has no ragdoll (c1000, objects)."""
import math, struct

import souls


def _mat_to_quat(c0, c1, c2):
    """hkRotation (three column vectors) -> unit quaternion (x, y, z, w)"""
    m00, m10, m20 = c0
    m01, m11, m21 = c1
    m02, m12, m22 = c2
    tr = m00 + m11 + m22
    if tr > 0:
        s = math.sqrt(tr + 1) * 2
        q = ((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, s / 4)
    elif m00 > m11 and m00 > m22:
        s = math.sqrt(1 + m00 - m11 - m22) * 2
        q = (s / 4, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s)
    elif m11 > m22:
        s = math.sqrt(1 + m11 - m00 - m22) * 2
        q = ((m01 + m10) / s, s / 4, (m12 + m21) / s, (m02 - m20) / s)
    else:
        s = math.sqrt(1 + m22 - m00 - m11) * 2
        q = ((m02 + m20) / s, (m12 + m21) / s, s / 4, (m10 - m01) / s)
    n = math.sqrt(sum(x * x for x in q)) or 1
    return tuple(x / n for x in q)


def ragdoll(hkx):
    h = souls.HkTag(hkx)
    if not h.objects("hkaRagdollInstance"):
        return None
    D = h.data
    ptr = lambda o: struct.unpack_from("<I", D, o)[0]

    def string(o):
        i = ptr(o)
        if not i:
            return ""
        _, so, sc = h.items[i]
        return D[so:so + sc].split(b"\0")[0].decode("cp932", "replace")

    RT, ST, BT = h.type("hkaRagdollInstance"), h.type("hkaSkeleton"), h.type("hkaBone")
    ri = h.objects("hkaRagdollInstance")[0]
    _, bo, bc = h.item(ri, RT, "rigidBodies")
    body_items = [ptr(bo + 8 * k) for k in range(bc)]

    def skel_names(so):
        _, o, c = h.item(so, ST, "bones")
        return [string(o + k * BT.size + BT.field("name")[0]) for k in range(c)]

    rag_skel = h.items[ptr(ri + RT.field("skeleton")[0])][1]
    rag_names = skel_names(rag_skel)
    _, mo, mc = h.item(ri, RT, "boneToRigidBodyMap")
    bone_to_body = list(struct.unpack_from(f"<{mc}i", D, mo))
    body_bone = {b: i for i, b in enumerate(bone_to_body) if b >= 0}  # body -> ragdoll bone

    # ragdoll bone -> animation bone (the mapper whose skeleton A is the ragdoll's)
    rag_to_anim = {}
    MT = h.type("hkaSkeletonMapper")
    mdo = MT.field("mapping")[0]
    MDT = h.type("hkaSkeletonMapperData")
    for m in h.objects("hkaSkeletonMapper"):
        md = m + mdo
        sa, sb = h.items[ptr(md + MDT.field("skeletonA")[0])][1], h.items[ptr(md + MDT.field("skeletonB")[0])][1]
        if sa != rag_skel:
            continue
        anim_names = skel_names(sb)
        _, so_, sc_ = h.items[ptr(md + MDT.field("simpleMappings")[0])]
        for k in range(sc_):
            a, b = struct.unpack_from("<2h", D, so_ + 64 * k)
            rag_to_anim.setdefault(a, anim_names[b])  # first mapping of a ragdoll bone is its own bone

    EO = h.type("hkpEntity")
    coll, motion, mat = (EO.field(n)[0] for n in ("collidable", "motion", "material"))
    MO = h.type("hkpMotion")
    ms, inv = MO.field("motionState")[0], MO.field("inertiaAndMassInv")[0]
    bodies = []
    for k, bi in enumerate(body_items):
        o = h.items[bi][1]
        st, so, _ = h.items[ptr(o + coll)]
        if st.name == "hkpCapsuleShape":
            r = struct.unpack_from("<f", D, so + 32)[0]
            a = struct.unpack_from("<3f", D, so + 48)
            b = struct.unpack_from("<3f", D, so + 64)
        elif st.name == "hkpSphereShape":
            r, a, b = struct.unpack_from("<f", D, so + 32)[0], (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)
        else:  # ASSUMED: a 0.1 m sphere at the body origin (TECH_DEBT enemy-17)
            print(f"  ragdoll body {k}: {st.name} read as a 0.1 m sphere")
            r, a, b = 0.1, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)
        tr = o + motion + ms
        cols = [struct.unpack_from("<3f", D, tr + 16 * j) for j in range(3)]
        t = struct.unpack_from("<3f", D, tr + 48)
        mi = struct.unpack_from("<f", D, o + motion + inv + 12)[0]
        rb = body_bone.get(k, -1)
        bodies.append(dict(name=string(o + h.type("hkpWorldObject").field("name")[0]),
                           bone=rag_to_anim.get(rb), world=(t, _mat_to_quat(*cols)), a=a, b=b, radius=r,
                           mass=1 / mi if mi else 0.0, friction=struct.unpack_from("<f", D, o + mat + 4)[0]))

    CI = h.type("hkpConstraintInstance")
    _, co, cc = h.item(ri, RT, "constraints")
    ent, dat = CI.field("entities")[0], CI.field("data")[0]
    AT = h.type("hkpRagdollConstraintData::Atoms")
    atoms = h.type("hkpRagdollConstraintData").field("atoms")[0]
    tf = AT.field("transforms")[0]
    ta, tb = (h.type("hkpSetLocalTransformsConstraintAtom").field(n)[0] for n in ("transformA", "transformB"))
    cone, twist, planes = (AT.field(n)[0] for n in ("coneLimit", "twistLimit", "planesLimit"))
    joints = []
    for k in range(cc):
        o = h.items[ptr(co + 8 * k)][1]
        dt, do, _ = h.items[ptr(o + dat)]
        if dt.name != "hkpRagdollConstraintData":
            continue
        ea, eb = ptr(o + ent), ptr(o + ent + 8)
        if ea not in body_items or eb not in body_items:
            continue
        at = do + atoms
        ax = D[at + cone + 3]  # twistAxisInA: column of each constraint frame that is the twist axis
        axis_c = struct.unpack_from("<3f", D, at + tf + ta + 16 * ax)
        axis_p = struct.unpack_from("<3f", D, at + tf + tb + 16 * ax)
        joints.append(dict(child=body_items.index(ea), parent=body_items.index(eb),
                           axis_child=axis_c, axis_parent=axis_p,
                           pivot_child=struct.unpack_from("<3f", D, at + tf + ta + 48),
                           pivot_parent=struct.unpack_from("<3f", D, at + tf + tb + 48),
                           cone=struct.unpack_from("<f", D, at + cone + 12)[0],
                           twist_min=struct.unpack_from("<f", D, at + twist + 8)[0],
                           twist_max=struct.unpack_from("<f", D, at + twist + 12)[0],
                           plane_min=struct.unpack_from("<f", D, at + planes + 8)[0],
                           plane_max=struct.unpack_from("<f", D, at + planes + 12)[0]))
    return dict(bodies=bodies, joints=joints)
