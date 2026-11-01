"""Offline reference pose for comparing the PS2 character with the source data.

Poses one animation frame straight from the hkx (havok_anim decoder) and the original FLVER parts (all four bone
weights, float, no PS2 quantization), renders it with ps2/proto's game camera (third-person orbit at the map spawn:
yaw = spawn yaw, pitch 0.25, 4 m, target 1.42 m up; FOCAL_X 609 / FOCAL_Y 568.4 at 640x448), and optionally puts it
next to a PCSX2 capture of the same frame (proto built with a forced pose and the player pinned at the spawn).
Also prints the body's lean: head and neck relative to the feet/pelvis in the character's own frame.

Usage: pose_check.py <game dir> <anim id> <frame> <out.png> [--map map.bin] [--shot pcsx2.png] [--blend id] [--mirror]
  --blend   50/50 blend with another animation of the same archive (export_model.py TURN_PAIR, e.g. 535 + 536)
  --mirror  also render the model mirrored on X (left-handed data shown right-handed) for a side-by-side check"""
import math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls
import havok_anim as ha

PARTS = ["HD_M_9360", "BD_M_9360", "AM_M_9360", "LG_M_9360"]
LOD2 = 0x02000000
FOCAL_X, FOCAL_Y, W, H = 609.0, 568.4, 640, 448


def load_mesh(game, names):
    """-> list of (verts [(pos, normal, [(skel bone, w)])], tris) per mesh, positions in model space (bind pose)"""
    sk = {n: i for i, n in enumerate(names)}
    out = []
    for part in PARTS:
        files = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(f"{game}/parts/{part}.partsbnd.dcx"))}
        F = souls.flver(files[f"{part}.flver"])
        for m in F["meshes"]:
            fs = [f for f in m["facesets"] if f[0] == LOD2]
            if not fs:
                continue
            V = m["verts"]
            verts = []
            for vi in range(len(V["pos"])):
                infl = [(sk[F["bones"][m["bones"][b]][0]], w) for b, w in zip(V["bone_idx"][vi], V["bone_w"][vi]) if w > 0]
                verts.append((V["pos"][vi], V["normal"][vi] if V["normal"] else (0, 1, 0), infl))
            out.append((verts, list(souls.triangles(fs[0]))))
    return out


def skin(meshes, bind, pose):
    """Linear blend skinning: v' = sum w * pose_b * bind_b^-1 * v"""
    res = []
    for verts, tris in meshes:
        P, N = [], []
        for p, n, infl in verts:
            acc, nacc, tw = [0.0] * 3, [0.0] * 3, sum(w for _, w in infl) or 1.0
            for b, w in infl:
                bt, bq = bind[b]
                l = ha.qrot((-bq[0], -bq[1], -bq[2], bq[3]), (p[0] - bt[0], p[1] - bt[1], p[2] - bt[2]))
                ln = ha.qrot((-bq[0], -bq[1], -bq[2], bq[3]), n)
                t, q = pose[b]
                r, rn = ha.qrot(q, l), ha.qrot(q, ln)
                for k in range(3):
                    acc[k] += w / tw * (t[k] + r[k])
                    nacc[k] += w / tw * rn[k]
            P.append(acc)
            N.append(nacc)
        res.append((P, N, tris))
    return res


def camera(yaw, pitch=0.25, d=4.0):
    """main.c's orbit camera around a player at the origin facing yaw; returns (eye, right, up, fwd)"""
    target = (0.0, 1.42, 0.0)
    eye = (-math.sin(yaw) * math.cos(pitch) * d, 1.42 + math.sin(pitch) * d, -math.cos(yaw) * math.cos(pitch) * d)
    f = [target[k] - eye[k] for k in range(3)]
    l = math.sqrt(sum(c * c for c in f))
    f = [c / l for c in f]
    r = [f[2], 0.0, -f[0]]
    l = math.sqrt(sum(c * c for c in r))
    r = [c / l for c in r]
    u = [f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]]
    return eye, r, u, f


def place(yaw, p):
    """model.c: model -> world = rotate about Y by yaw + pi (model forward -Z = game forward)"""
    a = yaw + math.pi
    c, s = math.cos(a), math.sin(a)
    return (c * p[0] + s * p[2], p[1], -s * p[0] + c * p[2])


def render(skinned, yaw, mirror=False):
    from PIL import Image, ImageDraw
    eye, r, u, f = camera(yaw)
    L = (0.4, 0.8, -0.45)
    il = 1 / math.sqrt(sum(c * c for c in L))
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    dr = ImageDraw.Draw(img)
    polys = []
    for P, N, tris in skinned:
        S = []
        for p in P:
            q = place(yaw, (-p[0], p[1], p[2]) if mirror else p)
            d = [q[k] - eye[k] for k in range(3)]
            vx, vy, vz = (sum(d[k] * a[k] for k in range(3)) for a in (r, u, f))
            S.append((W / 2 + FOCAL_X * vx / vz, H / 2 - FOCAL_Y * vy / vz, vz))
        for t in tris:
            a, b, c = (S[i] for i in t)
            n = N[t[0]]
            n = place(yaw, (-n[0], n[1], n[2]) if mirror else n)
            k = 0.5 + 0.75 * max(0.0, sum(n[i] * L[i] for i in range(3)) * il)
            g = min(255, int(110 * k))
            polys.append((a[2] + b[2] + c[2], [a[:2], b[:2], c[:2]], (g, int(g * 0.8), int(g * 0.6), 255)))
    for _, pts, col in sorted(polys, key=lambda x: -x[0]):
        dr.polygon(pts, fill=col)
    return img


def lean(names, parents, pose_local):
    """Head/neck over feet in the character frame (x = character left, z = forward), metres and degrees"""
    Wd = ha.world(parents, pose_local)
    ix = names.index
    h, n, pel = Wd[ix("Head")][0], Wd[ix("Neck")][0], Wd[ix("Pelvis")][0]
    lf, rf = Wd[ix("L_Foot")][0], Wd[ix("R_Foot")][0]
    feet = [(lf[k] + rf[k]) / 2 for k in range(3)]
    side, fwd = h[0] - feet[0], -(h[2] - feet[2])
    up = h[1] - feet[1]
    side_p = n[0] - pel[0]
    return dict(head_side_m=round(side, 3), head_fwd_m=round(fwd, 3),
                side_deg=round(math.degrees(math.atan2(side, up)), 1),
                fwd_deg=round(math.degrees(math.atan2(fwd, up)), 1),
                spine_side_deg=round(math.degrees(math.atan2(side_p, n[1] - pel[1])), 1))


def main():
    a = sys.argv[1:]
    game, aid, frame, out = a[0], int(a[1]), int(a[2]), a[3]
    opt = lambda k: a[a.index(k) + 1] if k in a else None
    chr_dir = os.path.join(game, "chr")
    anibnd = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(f"{chr_dir}/c0000.anibnd.dcx"))}
    names, parents, ref = ha.skeleton(anibnd["Skeleton.hkx"])
    archive = "c0000_a00_lo" if aid < 3000 else "c0000_a2x"
    files = {souls.basename(n): d for _, n, d in souls.bnd3(souls.read(f"{chr_dir}/{archive}.anibnd.dcx"))}
    nf, fps, t2b, frames = ha.animation(files[f"{'a00' if aid < 3000 else 'a23'}_{aid:04d}.hkx"])
    pose_local = list(ref)
    for tr, b in enumerate(t2b):
        pose_local[b] = frames[min(frame, nf - 1)][tr]
    if opt("--blend"):
        bid = int(opt("--blend"))
        nf2, _, t2b2, frames2 = ha.animation(files[f"{'a00' if bid < 3000 else 'a23'}_{bid:04d}.hkx"])
        other = list(ref)
        for tr, b in enumerate(t2b2):
            other[b] = frames2[min(frame, nf2 - 1)][tr]
        mixed = []
        for (ta, qa, sa), (tb, qb, _) in zip(pose_local, other):
            sg = 1 if sum(x * y for x, y in zip(qa, qb)) >= 0 else -1
            q = [(qa[c] + sg * qb[c]) / 2 for c in range(4)]
            l = math.sqrt(sum(c * c for c in q)) or 1.0
            mixed.append((tuple((ta[c] + tb[c]) / 2 for c in range(3)), tuple(c / l for c in q), sa))
        pose_local = mixed
    print("lean", lean(names, parents, pose_local))
    bind = ha.world(parents, ref)
    pose = ha.world(parents, pose_local)
    sk = skin(load_mesh(game, names), bind, pose)
    yaw = 0.0
    if opt("--map"):
        yaw = struct.unpack_from("<f", open(opt("--map"), "rb").read(), 32)[0]
    from PIL import Image, ImageDraw
    panels = [("offline (hkx + FLVER, 4 weights)", render(sk, yaw))]
    if "--mirror" in a:
        panels.append(("offline mirrored X", render(sk, yaw, True)))
    if opt("--shot"):
        shot = Image.open(opt("--shot")).convert("RGB")
        # game area: the 4:3 picture inside PCSX2's window (non-black columns of the middle row)
        row = [shot.getpixel((x, shot.height // 2)) for x in range(shot.width)]
        xs = [x for x, p in enumerate(row) if sum(p) > 30]
        x0, x1 = xs[0], xs[-1] + 1
        h = round((x1 - x0) * 3 / 4)
        y0 = 17 if shot.height > h + 17 else 0  # menu bar
        game_img = shot.crop((x0, y0, x1, y0 + h)).resize((W, H), Image.LANCZOS)
        over = game_img.copy()
        ref_img = panels[0][1]
        alpha = ref_img.split()[3]
        edge = Image.new("L", (W, H), 0)
        px, ed = alpha.load(), edge.load()
        for y in range(1, H - 1):
            for x in range(1, W - 1):
                if px[x, y] and not (px[x - 1, y] and px[x + 1, y] and px[x, y - 1] and px[x, y + 1]):
                    ed[x, y] = 255
        over.paste((0, 255, 255), (0, 0), edge)
        panels = [("PCSX2", game_img)] + panels + [("PCSX2 + offline outline", over)]
    crop = (170, 90, 470, 448)
    cw, ch = crop[2] - crop[0], crop[3] - crop[1]
    S = Image.new("RGB", (cw * 2 * len(panels), ch * 2 + 18), "white")
    d = ImageDraw.Draw(S)
    for i, (lab, im) in enumerate(panels):
        bg = Image.new("RGB", (W, H), (150, 160, 170))
        bg.paste(im, (0, 0), im if im.mode == "RGBA" else None)
        S.paste(bg.crop(crop).resize((cw * 2, ch * 2), Image.LANCZOS), (i * cw * 2, 18))
        tag = f"a{aid:04d}" + (f"+a{int(opt('--blend')):04d}" if opt("--blend") else "")
        d.text((i * cw * 2 + 4, 3), f"{lab}  {tag} f{frame}", fill=(0, 0, 0))
    S.save(out)
    print("wrote", out)


main()
