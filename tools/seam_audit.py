#!/usr/bin/env python3
"""Offline hole audit at every world seam (map streaming research idea 9): where the port would show the sky (or,
with map.c VOID_RING, the fog-coloured void ring) while DSR, with every map resident, shows geometry.

  seam_audit.py <out dir> <gen dir> [--spots N] [--yaws N] [--size W H] [--fail PCT] [--only BLOCK,...]

<gen dir>: the build's export directory (build/gen: world.bin, the boot block's export; build/gen/<block>: the other
blocks', as ps2/proto/Makefile lays them out). Seams come from world.bin's links (tools/export_world.py): a collision
triangle of block i whose load mask names another block j stands at the seam i -> j (DSR's ConnectCollision parts
between maps, the cut cells inside one MSB). For each seam up to --spots spots on its walkable triangles (farthest
apart), and from each --yaws camera yaws starting towards block j (the port's camera: feet + 1.42 m, 4 m back,
pitch 0.1, pulled in front of the resident blocks' collision as src/cam_input.c does), three depth-only renders
with tools/mapview.py's renderer (opaque pieces only, no sky):

  ref      the blocks DSR has resident (the home block's area group and what its seams load; ref_blocks) at full
           detail (near level everywhere), no draw groups: what is there to see (--ref-dg:
           under the draw groups of the floor's part, DSR's rule; a hole the default ref finds and this one does not
           is one a draw-group mask explains, which DSR may show the same way)
  steady   the runtime once streaming caught up: i and the blocks its triangle loads on at their levels (mapshot.py's
           near / coarse / horizon picks), the others by i's far panorama (mapblock.c mp_draw_far, seam sets
           included), draw groups of the floor under the player plus DG_SEAM's floors 4-16 m ahead along the view
           (map.c map_set_player)
  loading  the same with j still off: drawn by i's far panorama only (the frames while j's lite set is read)

The ref leaves out another map's stand-ins for the home block's map (tools/farpano.py's rule, FAR_STANDIN_MAP;
--standins: kept), and both renders alpha-test the textured map pieces (--no-alpha: every triangle solid).

A hole is a pixel the ref covers and the runtime leaves empty; "sky" holes are those above the horizon, which the
void ring does not cover. The audit fails (exit 1) when a view's steady holes exceed --fail % of the frame (default
0.5); loading holes are reported, not failed (they last the read). Out: <out>/audit.json (every view: seam, spot,
yaw, hole shares), <out>/sheet.jpg (the worst views: steady | loading, holes magenta, sky holes red), one line per
seam on stdout. The pictures are game-derived: keep <out> outside the repository (AGENTS.md section 1).

Not modelled: PVS culling (tools/pvs_bake.c validates it), late or refused cells, characters and objects; the
runtime's draw-group rule as mapshot.py models it (one mask over the area group)."""
import argparse
import json
import math
import os
import struct
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapcfg  # noqa: E402
import mapshot  # noqa: E402
import mapview  # noqa: E402

DG_SEAM_R = 16.0  # map.c DG_SEAM_R
SPOT_APART = 8.0  # metres between two spots of one seam at least
UP = 0.7          # walkable: the triangle's normal y at least (ASSUMED: about 45 degrees, the port's slide limit)


def world(gen):
    """world.bin -> [(pack, map, gen dir, [(first tri, tris, load mask, keep mask)])]"""
    import chunkfile as chunk
    w = chunk.read(os.path.join(gen, "world.bin"), b"WRLD")  # tools/export_world.py
    bl, lr, lk = w.get(b"BLKS", 1), w.get(b"LRNG", 1), w.get(b"LINK", 1)
    blocks = []
    for i in range(len(bl) // 64):
        pack, mp = (x.split(b"\0")[0].decode() for x in struct.unpack_from("<16s16s", bl, 64 * i))
        blocks.append([pack, mp, gen if i == 0 else os.path.join(gen, pack), []])
    ranges = [struct.unpack_from("<2I", lr, 8 * i) for i in range(len(blocks))]
    links = [struct.unpack_from("<I3H", lk, 12 * i) for i in range(len(lk) // 12)]
    for i, (first, cnt) in enumerate(ranges):
        blocks[i][3] = links[first:first + cnt]
    return blocks


def seam_spots(col, links, i, j, n):
    """up to n feet positions on block i's walkable triangles that load block j, farthest apart"""
    tri, ids = col
    on = np.zeros(len(tri), bool)
    lut = {}
    for first, cnt, load, _keep in links:
        if load >> j & 1:
            lut[(first, first + cnt)] = 1
    if not lut:
        return []
    for (a, b) in lut:
        on |= (ids >= a) & (ids < b)
    T = tri[on]
    if not len(T):
        return []
    nrm = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
    ny = np.abs(nrm[:, 1]) / np.maximum(np.linalg.norm(nrm, axis=1), 1e-9)
    c = T[ny >= UP].mean(1)
    if not len(c):
        return []
    out = [c[np.argmin(((c - c.mean(0)) ** 2).sum(1))]]
    d = ((c - out[0]) ** 2).sum(1)
    while len(out) < n:
        k = int(np.argmax(d))
        if d[k] < SPOT_APART ** 2:
            break
        out.append(c[k])
        d = np.minimum(d, ((c - c[k]) ** 2).sum(1))
    return out


def floor_seam(blocks, wb, home, feet, fwd, floor):
    """map.c DG_SEAM / DG_SEAM_VIEW: the floor of another map of the area group 4-16 m ahead along the view adds its
    part's groups to the shown mask"""
    if floor is None:
        return None
    floor = list(floor)
    for r in np.arange(4.0, DG_SEAM_R + 0.1, 4.0):
        p = feet + r * fwd
        for k, b in enumerate(blocks):
            if k == home or wb[k][1] == wb[home][1] or b.col is None:
                continue
            for y in (p[1] + 3.0, p[1]):  # (col_floor_block from 3 m above down to 6 m below)
                m = b.floor_mask(np.array([p[0], y, p[2]]))
                if m is not None:
                    floor = [a | x for a, x in zip(floor, m)]
                    return floor
    return floor


def scene(blocks, wb, home, feet, eye, fwd, on, mode):
    """the opaque, non-sky triangles drawn: mode 'ref' (every block, near level, no draw groups) or 'run' (blocks in
    on at their levels, the others by the home block's far panorama) -> (P, ST, TI, textures). ALPHA keeps the
    textures, so cut-out texels test through as on the GS (Darkroot's leaf cards are 60 % transparent: as solid walls
    the ref counted the sky between the leaves as holes, 2026-11-21); TI -1: untextured (horizon, panorama)"""
    skip = mapcfg.CHUNK_SKY | mapcfg.CHUNK_BLEND
    if mode == "ref":
        # (on: the blocks DSR has resident here, ref_blocks)
        floor = blocks[home].floor_mask(feet) if REF_DG else None
        picks = [(k, c) for k, b in enumerate(blocks) if k in on for c in b.pick(feet, eye, "near", floor)]
        if CUT_STANDINS:
            picks = [(k, c) for k, c in (standin_cut(wb, home, k, c) for k, c in picks) if c is not None]
    else:
        floor = floor_seam(blocks, wb, home, feet, fwd, blocks[home].floor_mask(feet))
        picks = [(k, c) for k, b in enumerate(blocks)
                 for c in (b.pick(feet, eye, None, floor) if k in on else blocks[home].far_pick(k, floor, eye))]
    picks = [(k, c) for k, c in picks if not c[4] & skip and len(c[0])]
    if not picks:
        return np.zeros((0, 3, 3), np.float32), np.zeros((0, 3, 2)), np.zeros(0, np.int64), []
    base, tex = {}, []
    if ALPHA:
        for k in sorted({k for k, _c in picks}):
            base[k] = len(tex)
            tex += blocks[k].tex
    tx = lambda k, c: ALPHA and c[3] >= 0 and c[1] is not None and len(c[1]) == len(c[0])
    P = np.concatenate([c[0] for _k, c in picks])
    ST = np.concatenate([c[1] if tx(k, c) else np.zeros((len(c[0]), 3, 2)) for k, c in picks])
    TI = np.concatenate([np.full(len(c[0]), base[k] + c[3] if tx(k, c) else -1, np.int64) for k, c in picks])
    return P, ST, TI, tex


def depth(S, eye, tgt, W, H, tmp):
    """opaque depth (H, W), inf where nothing is drawn (S: scene()'s)"""
    P, ST, TI, tex = S
    n = len(P)
    if not n:
        return np.full((H, W), np.inf)
    geo = (P.astype(np.float32), ST, np.full((n, 3, 4), 128.0), TI, np.zeros(n, np.int64),
           np.zeros(n), np.concatenate([P.min(1), P.max(1)], 1))
    z = []
    mapview.render_view(tmp, geo, tex, tuple(eye), tuple(tgt), W=W, H=H, zout=z, alpha_pal=True)
    return z[0]


def horizon_row(eye, tgt, H):
    """screen row of the horizon (mapview's 43 degree view, pitch from eye -> tgt)"""
    f = H / 2 / math.tan(math.radians(43.0) / 2)
    d = np.array(tgt) - np.array(eye)
    p = math.asin(d[1] / np.linalg.norm(d))
    return H / 2 - f * math.tan(-p)


def area_group(gen):
    """dg.bin's area group (tools/drawgroups.py: the AA of mAA when the map is in an area load list), 0 without one"""
    import drawgroups
    try:
        d = drawgroups.read(os.path.join(gen, "dg.bin"))
    except ValueError:
        return 0
    return d.area if d else 0


def standin_cut(wb, home, k, c):
    """(k, chunk) without the triangles that are another map's stand-ins for the home block's map, by
    tools/farpano.py's rule (inside that map's cells and within FAR_STANDIN_MAP of its surfaces: Firelink's low-detail
    Burg and its backdrop's view of it). DSR draws both, the real map in front; the port draws the real map and leaves
    the copy out, so the copy is no hole (2026-11-21: Firelink's far backdrop 70-140 m away and a Firelink card 1 m
    from the eye on a Burg roof were most of the Burg -> Burg failures). (k, None): nothing left"""
    own = wb[home][1]
    if wb[k][1] == own or not len(c[0]):
        return k, c
    key = (own, k, id(c[0]))
    if key not in _SI:
        import farpano
        import horizon
        if own not in _SI_TRIS:
            T = [h[0] for kk, b in enumerate(_BLOCKS) if wb[kk][1] == own for hz in b.hzn for h in hz if len(h[0])]
            _SI_TRIS[own] = np.concatenate(T) if T else np.zeros((0, 3, 3))
        cen = c[0].mean(1).astype(np.float64)
        cut = farpano.in_map(cen, own)
        # (FAR_STANDIN_MAP 0, or no native distance library / no horizon: farpano.py's xz-cell rule alone)
        if cut.any() and STANDIN_MAP > 0 and len(_SI_TRIS[own]) and horizon._err_lib() is not None:
            cut[cut] = horizon._dist(_SI_TRIS[own], cen[cut], np.full(int(cut.sum()), STANDIN_MAP)) < STANDIN_MAP
        _SI[key] = cut
    cut = _SI[key]
    if not cut.any():
        return k, c
    if cut.all():
        return k, None
    keep = ~cut
    return k, (c[0][keep], c[1][keep] if c[1] is not None and len(c[1]) == len(keep) else c[1], c[2][keep]
               if c[2] is not None and len(c[2]) == len(keep) else c[2], c[3], c[4], c[5])


def ref_blocks(wb, i, on):
    """the blocks DSR has resident at a spot of block i: its area group's (one area load list) and those i's seams load
    (world.bin links). Every block of the world was the first run's ref: from Darkroot, the Asylum's and Firelink's
    far planes covered the sky and read as 40-98 % holes (2026-11-18)"""
    a = _AREA[i]
    return set(on) | {k for k in range(len(wb)) if (a and _AREA[k] == a) or wb[k][1] == wb[i][1]}


CAM_D, CAM_HEAD, CAM_WALL, CAM_MIN = 4.0, 1.42, 0.55, 0.2  # src/cam_input.c cam_follow (CAM_WALL: R_NEAR + 0.05)


def cam_pull(feet, yaw, pitch, cols):
    """the port's camera (src/cam_input.c cam_follow): mapshot.camera's eye pulled in front of the first collision
    triangle on the ray from the head to it (cols: the collision of the blocks resident there, every triangle taken as
    solid). Without it a spot by a wall put the eye inside the rock of the next map, and the audit counted that rock's
    back faces as holes (2026-11-21: most of the Depths -> Burg failures)"""
    eye, tgt = mapshot.camera(*feet, yaw, pitch)
    head = np.array([feet[0], feet[1] + CAM_HEAD, feet[2]])
    d = eye - head
    lo, hi = np.minimum(head, eye) - 0.01, np.maximum(head, eye) + 0.01
    t_hit = 1.0
    for T in cols:
        m = (T.max(1) >= lo).all(1) & (T.min(1) <= hi).all(1)
        if not m.any():
            continue
        A, e1, e2 = T[m, 0], T[m, 1] - T[m, 0], T[m, 2] - T[m, 0]
        pv = np.cross(d, e2)
        det = (e1 * pv).sum(1)
        with np.errstate(all="ignore"):
            inv = 1.0 / det
            s = head - A
            u = (s * pv).sum(1) * inv
            q = np.cross(s, e1)
            v = (q * d).sum(1) * inv
            t = (e2 * q).sum(1) * inv
        ok = (np.abs(det) > 1e-12) & (u >= 0) & (v >= 0) & (u + v <= 1) & (t >= 0) & (t <= t_hit)
        if ok.any():
            t_hit = float(t[ok].min())
    if t_hit < 1.0:
        want = max(t_hit * CAM_D - CAM_WALL, CAM_MIN)
        eye = head + d * (want / CAM_D)
        tgt = tgt + (eye - mapshot.camera(*feet, yaw, pitch)[0])
    return eye, tgt


def view(job):
    """one camera: -> dict with the hole shares, and the masks for the sheet"""
    wb, i, j, feet, yaw, W, H, tmp = job
    blocks = _BLOCKS
    load = 1 << i
    for first, cnt, ld, _k in wb[i][3]:
        load |= ld
    on = {k for k in range(len(blocks)) if load >> k & 1}
    if CAM_COL:
        eye, tgt = cam_pull(feet, yaw, 0.1, [_COLS[k] for k in ref_blocks(wb, i, on) if _COLS[k] is not None])
    else:
        eye, tgt = mapshot.camera(*feet, yaw, 0.1)
    fwd = np.array([math.sin(yaw), 0.0, math.cos(yaw)])
    zr = depth(scene(blocks, wb, i, feet, eye, fwd, ref_blocks(wb, i, on), "ref"), eye, tgt, W, H, tmp)
    out = {"seam": f"{wb[i][0]}->{wb[j][0]}", "i": i, "j": j, "feet": [round(float(v), 2) for v in feet],
           "yaw": round(yaw, 3)}
    hz = int(np.clip(horizon_row(eye, tgt, H), 0, H))
    masks = {}
    for name, blk_on in (("steady", on), ("loading", on - {j})):
        zz = depth(scene(blocks, wb, i, feet, eye, fwd, blk_on, "run"), eye, tgt, W, H, tmp)
        hole = np.isfinite(zr) & ~np.isfinite(zz)
        out[name] = round(100.0 * hole.mean(), 3)
        out[name + "_sky"] = round(100.0 * hole[:hz].sum() / hole.size, 3)
        masks[name] = (hole, zz)
    return out, masks, zr, hz


_BLOCKS = None
_AREA = []
_COLS = []      # per block its collision triangles (cam_pull)
_SI, _SI_TRIS = {}, {}  # standin_cut's masks per (home map, block, chunk) and each map's horizon triangles
STANDIN_MAP = float(os.environ.get("FAR_STANDIN_MAP", 8.0))  # tools/farpano.py's (0: its xz-cell rule)
CUT_STANDINS = True  # --standins: kept in the ref (counted as holes)
ALPHA = True    # --no-alpha: every map triangle solid (the audit before 2026-11-21)
CAM_COL = True  # --no-cam-col: the eye 4 m back whatever is in the way (the audit before 2026-11-21)
REF_DG = False  # --ref-dg: the ref under DSR's draw-group rule (the floor's part alone), not everything


def main():
    global _BLOCKS
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("gen")
    ap.add_argument("--spots", type=int, default=3)
    ap.add_argument("--yaws", type=int, default=4)
    ap.add_argument("--size", type=int, nargs=2, default=(256, 180))
    ap.add_argument("--fail", type=float, default=0.5, help="steady hole share of a view that fails, percent")
    ap.add_argument("--only", help="comma-separated block packs: seams from these blocks only")
    ap.add_argument("--ref-dg", action="store_true", help="ref under DSR's draw groups (fewer holes a mask explains)")
    ap.add_argument("--no-cam-col", action="store_true", help="no camera collision (the eye always 4 m back)")
    ap.add_argument("--no-alpha", action="store_true", help="no alpha test (cut-out map triangles count as solid)")
    ap.add_argument("--standins", action="store_true", help="count another map's stand-ins for this map as holes")
    a = ap.parse_args()
    global REF_DG, CAM_COL, ALPHA, _COLS, CUT_STANDINS
    CUT_STANDINS = not a.standins
    REF_DG = a.ref_dg
    CAM_COL = not a.no_cam_col
    ALPHA = not a.no_alpha
    os.makedirs(a.out, exist_ok=True)
    t0 = time.time()
    wb = world(a.gen)
    _BLOCKS = [mapshot.Block(g) for _p, _m, g, _l in wb]
    global _AREA
    _AREA = [area_group(g) for _p, _m, g, _l in wb]
    _COLS = [(b.col or mapshot.collision(os.path.join(b.gen, "collision.bin")) or (None,))[0] for b in _BLOCKS]
    print("seam_audit: area groups " + " ".join(f"{p}:{a}" for (p, *_), a in zip(wb, _AREA)), flush=True)
    print(f"seam_audit: {len(wb)} blocks loaded in {time.time() - t0:.1f} s", flush=True)
    W, H = a.size
    tmp = os.path.join(a.out, ".v.png")
    rows, worst = [], []
    only = set(a.only.split(",")) if a.only else None
    for i, (pack, _m, _g, links) in enumerate(wb):
        if only and pack not in only:
            continue
        col = _BLOCKS[i].col or mapshot.collision(os.path.join(_BLOCKS[i].gen, "collision.bin"))
        if col is None:
            continue
        loads = 0
        for _f, _c, ld, _k in links:
            loads |= ld
        for j in range(len(wb)):
            if j == i or not loads >> j & 1:
                continue
            spots = seam_spots(col, links, i, j, a.spots)
            # towards block j: from the spot to the middle of j's cells (its collision's mean)
            cj = _BLOCKS[j].col[0].mean((0, 1)) if _BLOCKS[j].col is not None else None
            seam_rows = []
            for s, feet in enumerate(spots):
                y0 = math.atan2(cj[0] - feet[0], cj[2] - feet[2]) if cj is not None else 0.0
                for k in range(a.yaws):
                    yaw = y0 + 2 * math.pi * k / a.yaws
                    r, masks, zr, hz = view((wb, i, j, np.array(feet, np.float64), yaw, W, H, tmp))
                    r["spot"] = s
                    seam_rows.append(r)
                    worst.append((r["steady"] + 0.25 * r["loading"], r, masks, hz))
                    worst.sort(key=lambda w: -w[0])
                    del worst[12:]
            if not seam_rows:
                continue
            rows += seam_rows
            bad = [r for r in seam_rows if r["steady"] > a.fail]
            print(f"seam_audit: {pack} -> {wb[j][0]}: {len(spots)} spots x {a.yaws} yaws, holes steady max "
                  f"{max(r['steady'] for r in seam_rows):.2f} % (sky {max(r['steady_sky'] for r in seam_rows):.2f}), "
                  f"loading max {max(r['loading'] for r in seam_rows):.2f} % (sky "
                  f"{max(r['loading_sky'] for r in seam_rows):.2f}){', FAIL ' + str(len(bad)) + ' views' if bad else ''}",
                  flush=True)
    if os.path.exists(tmp):
        os.remove(tmp)
    seams = sorted({r["seam"] for r in rows})
    fails = [r for r in rows if r["steady"] > a.fail]
    json.dump({"fail_pct": a.fail, "seams": seams, "views": rows, "fails": len(fails)},
              open(os.path.join(a.out, "audit.json"), "w"), indent=1)
    if worst:
        from PIL import Image, ImageDraw
        sh = Image.new("RGB", (2 * W, len(worst) * (H + 12)))
        for r_, (_s, r, masks, hz) in enumerate(worst):
            for c, name in enumerate(("steady", "loading")):
                hole, zz = masks[name]
                img = np.zeros((H, W, 3), np.uint8)
                fin = np.isfinite(zz)
                # near light, far dark on a log scale (a linear one left everything within 30 m one grey)
                img[fin] = np.clip(240 - 45 * np.log2(1 + zz[fin][:, None] / 2), 40, 240).astype(np.uint8)
                img[hole] = (255, 0, 255)
                img[:hz][hole[:hz]] = (255, 0, 0)
                sh.paste(Image.fromarray(img), (c * W, r_ * (H + 12) + 12))
                seam = r["seam"].replace("_00_00", "")  # (m10_01_00_00_w -> m10_01_w: the label fits the panel)
                ImageDraw.Draw(sh).text((c * W + 2, r_ * (H + 12)), f"{seam} s{r['spot']} y{r['yaw']:.1f} {name[0]} "
                                        f"{r[name]:.2f}%", fill=(255, 255, 0))
        sh.save(os.path.join(a.out, "sheet.jpg"), quality=88)
    print(f"seam_audit: {len(seams)} seams, {len(rows)} views, {len(fails)} over {a.fail} % steady holes; "
          f"{os.path.join(a.out, 'audit.json')} ({time.time() - t0:.0f} s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
