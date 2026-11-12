#!/usr/bin/env python3
"""Offline shots of exported map blocks: what the port would draw at a spot, without booting PCSX2.

  mapshot.py <out dir> <gen dir>... --script ps2/proto/tests/iso_burg_outline.txt [--ref <dir>] [--vs <gen dir>...]
  mapshot.py <out dir> <gen dir>... --at X Y Z YAW [PITCH] [--name spot]

Each <gen dir> holds one block's export (map.bin, cells.bin, hzn.bin: build/gen for the default map, build/gen/<block>
for a world block); give the blocks that are on at the spots (the Burg blocks for a Burg view: Firelink's export
carries the game's own low-detail Burg, which the runtime hides while the Burg blocks are on). A map or simplifier change is then judged in
seconds per view after the block's export (about a minute), not after the ELF, the ISO and a PCSX2 run. PCSX2 stays the
check for anything the runtime decides: what is loaded in time, memory, speed, the GS.

The picture: tools/mapview.py's software renderer (textures x vertex light, fog, alpha test, 640x448, the port's
43 degree view) from the port's camera (feet + 1.42 m, 4 m back along yaw / pitch; pitch > 0 looks down), with the
levels the runtime draws once everything wanted is in:
  near level     cells whose box is within RES_NEAR_R of the player (cells.bin), in place of their coarse level
  coarse level   the other cells within HZN_R of the camera (map.bin, CHUNK_COARSE)
  horizon        cells beyond HZN_R (hzn.bin, untextured) in place of their coarse level and opaque kept meshes
  kept meshes: always; backdrop and sky: those of the block the player stands in
--levels near|coarse|hzn draws that level everywhere instead (near: every cell's near level), to look at one level
alone. Under each picture: the memory the spot takes as tools/world_budget.py counts it (near level within RES_NEAR_R,
far level and kept meshes within RES_PIN_R, all blocks given) against the block's room (residency.h RES_BLOCK), red
when over; plus the pinned backdrop and sky and the blocks' horizons, which the room does not count.
Not drawn: characters, objects, effects, the relief pass, draw-group and PVS culling, late or refused cells.

--script: the `spawn x y z yaw`, `cam yaw pitch` and `shot name` lines of a test script (ps2/proto/src/testscript.c),
one PNG per shot. --ref DIR: reference shots <name>.png (tools/outline-dsr-shots.sh; cropped to 4:3) in a first
column of <out dir>/sheet.jpg. --vs GEN...: the same views of another export (a build before the change) next to
them. The shots are game-derived: keep the out dir outside the repository (AGENTS.md section 1)."""
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
import mapchunks  # noqa: E402
import mapview  # noqa: E402
import residency  # noqa: E402
import texmap  # noqa: E402

NEAR_R = float(residency.RES["RES_NEAR_R"])
PIN_R = float(residency.RES["RES_PIN_R"])
CELL_PART = 0x80000000
CHUNK_HZN = 256


def _src(path, name):
    """a value of a C `#define name value` (the runtime's own number), or None"""
    import re
    try:
        m = re.search(rf"#define\s+{name}\s+([0-9.]+)", open(path).read())
        return float(m.group(1)) if m else None
    except OSError:
        return None


HZN_R = _src(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../ps2/proto/src/map_i.h"), "HZN_R") or 40.0


def chunks(b, off, n, base=0, st=True):
    """n chunk records (64 B, map.c Chunk) at off of b, stream offsets relative to base -> per chunk (pos (t,3,3),
    st (t,3,2), rgba (t,3,4), texture, flags, box (6)); st False: no st stream (horizon)"""
    out = []
    for i in range(n):
        ti, fl, nw, po, so, co = struct.unpack_from("<iII3I", b, off + 64 * i)
        t, nv = nw & 0xFFFF, nw >> 16
        box = np.array(struct.unpack_from("<6f", b, off + 64 * i + 24))
        org, step = np.array(struct.unpack_from("<3f", b, off + 64 * i + 48)), struct.unpack_from("<f", b, off + 64 * i + 60)[0]
        po, so, co = po + base, so + base, co + base
        if nv:
            ix = np.frombuffer(b, np.uint8, t * 4, co + -(-4 * nv // 16) * 16 + 16).reshape(t, 4)[:, :3]
            pv = np.frombuffer(b, np.int16, nv * 3, po).reshape(nv, 3)[ix]
            sv = np.frombuffer(b, np.uint16, nv * 2, so).reshape(nv, 2)[ix] if st else np.zeros((t, 3, 2), np.uint16)
            cv = np.frombuffer(b, np.uint8, nv * 4, co).reshape(nv, 4)[ix]
        else:
            pv = np.frombuffer(b, np.int16, t * 9, po).reshape(t, 3, 3)
            sv = np.frombuffer(b, np.uint16, t * 6, so).reshape(t, 3, 2) if st else np.zeros((t, 3, 2), np.uint16)
            cv = np.frombuffer(b, np.uint8, t * 12, co).reshape(t, 3, 4)
        out.append((org + step * pv.astype(np.float32), sv / (1024.0 if fl & mapcfg.CHUNK_ST1K else 4096.0), cv, ti, fl, box))
    return out


class Block:
    """one exported block: resident chunks (map.bin), cells with their near chunks (cells.bin), horizon (hzn.bin)"""

    def __init__(self, gen):
        self.gen = gen
        b = open(os.path.join(gen, "map.bin"), "rb").read()
        magic, nch, ntex = struct.unpack_from("<4s2I", b, 0)
        och, otex = struct.unpack_from("<2I", b, 36)
        self.tex = texmap.load(b, ntex, otex)
        fb, fe, fm = struct.unpack_from("<3f", b, 52)
        fc, sc = struct.unpack_from("<4B", b, 64), struct.unpack_from("<4B", b, 68)
        self.fog = (fb, fe, fm, fc[:3], sc[:3], sc[3] / 255.0)
        self.res = chunks(b, och, nch)
        self.cells = []  # (box (6), resident chunk ids, near chunks, cell table entry)
        ocell = struct.unpack_from("<I", b, 76)[0] if magic == b"DSM5" else 0
        cpath = os.path.join(gen, "cells.bin")
        cb = open(cpath, "rb").read() if os.path.exists(cpath) else b""
        if ocell:
            for i in range(struct.unpack_from("<I", b, ocell)[0]):
                box = np.array(struct.unpack_from("<6f", b, ocell + 16 + 48 * i))
                off, size, nn, first, nres, _ = struct.unpack_from("<6I", b, ocell + 16 + 48 * i + 24)
                near = chunks(cb, off, nn & ~CELL_PART, off) if size else []
                # (a near chunk's record keeps its number in bump.bin in the flags' bits 8+; foliage its tuft bits)
                near = [(p, s, c, ti, fl & 0xFF if not fl & mapchunks.CHUNK_GRASS else fl, bx) for p, s, c, ti, fl, bx in near]
                if nn & CELL_PART and self.cells:  # a later part of the cell before it
                    self.cells[-1][2].extend(near)
                else:
                    self.cells.append((box, list(range(first, first + nres)), near, i))
        # the export's bytes per cell (map.bin.cells.json, what tools/world_budget.py reads): key, box, near bytes,
        # coarse / kept / backdrop bytes of its resident items, near box
        jp = os.path.join(gen, "map.bin.cells.json")
        J = json.load(open(jp)) if os.path.exists(jp) else None
        self.mem = np.array(J["cells"], np.float64).reshape(-1, 19) if J and J["cells"] else None
        self.sky_kb = J["sky"] / 1024 if J else 0.0
        self.hzn_kb = (os.path.getsize(os.path.join(gen, "hzn.bin")) / 1024) if os.path.exists(os.path.join(gen, "hzn.bin")) else 0.0
        self.hzn = []  # per cell: horizon chunks
        hpath = os.path.join(gen, "hzn.bin")
        hb = open(hpath, "rb").read() if os.path.exists(hpath) else b""
        if hb[:4] == b"HZN1":
            _, hch, hcells, _ = struct.unpack_from("<4s3I", hb, 0)
            hc = chunks(hb, 16 + 32 * hcells, hch, 0, st=False)
            for i in range(hcells):
                first, cnt, _ = struct.unpack_from("<2HI", hb, 16 + 32 * i + 24)
                self.hzn.append(hc[first:first + cnt])

    def pick(self, feet, eye, levels):
        """the chunks drawn from here: [(chunk, texture offset applies)]"""
        out, coarse_off = [], set()
        covered = set()  # resident chunks a horizon stands for
        for box, res, near, ci in self.cells:
            d_p = math.sqrt((np.maximum(0, np.maximum(box[:3] - feet, feet - box[3:])) ** 2).sum())
            d_e = math.sqrt((np.maximum(0, np.maximum(box[:3] - eye, eye - box[3:])) ** 2).sum())
            hz = self.hzn[ci] if ci < len(self.hzn) else []
            lv = levels or ("near" if near and d_p < NEAR_R else "hzn" if hz and d_e >= HZN_R else "coarse")
            if lv == "near" and near:
                out += near
                coarse_off.update(i for i in res if self.res[i][4] & mapcfg.CHUNK_COARSE)
            elif lv == "hzn" and hz:
                out += [(p, s, c, -1, fl & ~CHUNK_HZN, bx) for p, s, c, _, fl, bx in hz]
                covered.update(i for i in res if not self.res[i][4] & (mapcfg.CHUNK_BLEND | mapcfg.CHUNK_SKY | mapchunks.CHUNK_GRASS))
        out += [c for i, c in enumerate(self.res) if i not in coarse_off and i not in covered]
        return out


def memory(blocks, feet, home):
    """What the streamer must hold with the player at feet, as tools/world_budget.py's hard tier counts it (KB):
    near level of cells within RES_NEAR_R (box clipped to the cell's cube), coarse level of the other cells within
    RES_PIN_R, kept meshes within RES_PIN_R, over all blocks given; pinned: the home block's backdrop and sky; room:
    the home block's RES_BLOCK budget (residency.h). None without the exports' map.bin.cells.json."""
    if any(b.mem is None for b in blocks):
        return None
    box_d = lambda lo, hi: np.sqrt((np.maximum(0, np.maximum(lo - feet, feet - hi)) ** 2).sum(1))
    m = dict(near=0.0, coarse=0.0, kept=0.0)
    for b in blocks:
        c = b.mem
        d = box_d(c[:, 3:6], c[:, 6:9])
        k0 = c[:, 0:3] * 32.0
        nlo = np.maximum(c[:, 3:6], k0)
        dn = box_d(nlo, np.maximum(np.minimum(c[:, 6:9], k0 + 32.0), nlo))
        m["near"] += c[dn < NEAR_R, 9].sum() / 1024
        m["coarse"] += c[((dn >= NEAR_R) | (c[:, 9] == 0)) & (d < PIN_R), 10].sum() / 1024
        m["kept"] += c[d < PIN_R, 11].sum() / 1024
    h = blocks[home]
    m["pinned"] = h.mem[:, 12].sum() / 1024 + h.sky_kb
    m["hard"] = m["near"] + m["coarse"] + m["kept"]
    m["room"] = float(residency.block_budget(os.path.basename(os.path.normpath(h.gen)))[0])
    m["horizon"] = sum(b.hzn_kb for b in blocks)
    return m


def mem_line(m):
    return (f"near {m['near']:.0f} + far {m['coarse']:.0f} + kept {m['kept']:.0f} = {m['hard']:.0f} of {m['room']:.0f} KB room"
            f"{' OVER' if m['hard'] > m['room'] else ''}; backdrop + sky {m['pinned']:.0f}, horizon {m['horizon']:.0f} KB")


def camera(x, y, z, yaw, pitch):
    """the port's eye and a point it looks at, for a player standing at (x, y, z)"""
    c = 4.0 * math.cos(pitch)
    eye = np.array([x - c * math.sin(yaw), y + 1.42 + 4.0 * math.sin(pitch), z - c * math.cos(yaw)])
    return eye, eye + np.array([math.sin(yaw) * math.cos(pitch), -math.sin(pitch), math.cos(yaw) * math.cos(pitch)])


def shot(path, blocks, feet, yaw, pitch, levels=None, size=(640, 448)):
    """one picture -> triangles drawn"""
    feet = np.array(feet, np.float64)
    eye, tgt = camera(*feet, yaw, pitch)
    P, ST, C, TI, FL, BX, tex = [], [], [], [], [], [], []
    home = min(range(len(blocks)), key=lambda i: min([float(((np.maximum(0, np.maximum(c[0][:3] - feet, feet - c[0][3:]))) ** 2).sum())
                                                         for c in blocks[i].cells] or [1e30]))
    for bi, b in enumerate(blocks):
        for p, s, c, ti, fl, bx in b.pick(feet, eye, levels):
            if fl & (mapcfg.CHUNK_SKY | mapcfg.CHUNK_BACKDROP) and bi != home:
                continue  # one sky and one backdrop: the player's block's (another block's backdrop is this block)
            n = len(p)
            P.append(p), ST.append(s), C.append(c), BX.append(np.tile(bx, (n, 1)))
            TI.append(np.full(n, ti + len(tex) if ti >= 0 else -1))
            FL.append(np.full(n, fl))
        tex += b.tex
    geo = (np.concatenate(P), np.concatenate(ST), np.concatenate(C), np.concatenate(TI), np.concatenate(FL),
           np.zeros(sum(len(p) for p in P)), np.concatenate(BX))
    # (foliage: every tuft; map.c thins it by distance)
    mapview.render_view(path, geo, tex, tuple(eye), tuple(tgt), W=size[0], H=size[1], fog=blocks[home].fog)
    m = memory(blocks, feet, home)
    if m:  # the memory the spot takes, under the picture
        from PIL import Image, ImageDraw
        im = Image.open(path).convert("RGB")
        out = Image.new("RGB", (im.size[0], im.size[1] + 16), (150, 0, 0) if m["hard"] > m["room"] else (0, 0, 0))
        out.paste(im, (0, 0))
        ImageDraw.Draw(out).text((4, im.size[1] + 2), mem_line(m), fill=(255, 255, 255))
        out.save(path)
    return len(geo[0]), m


def script_shots(path):
    """[(name, (x, y, z), yaw, pitch)] of a test script's shot lines"""
    out, pos, yaw, pitch = [], None, 0.0, 0.0
    for line in open(path):
        w = line.split("#")[0].split()
        if not w:
            continue
        if w[0] == "spawn" and len(w) >= 4:
            pos, yaw = tuple(float(v) for v in w[1:4]), float(w[4]) if len(w) > 4 else yaw
        elif w[0] == "cam" and len(w) >= 3:
            yaw, pitch = float(w[1]), float(w[2])
        elif w[0] == "pitch" and len(w) >= 2:
            pitch = float(w[1])
        elif w[0] == "shot" and len(w) >= 2 and pos:
            out.append((w[1], pos, yaw, pitch))
    return out


def sheet(path, cols, names, size=(480, 336)):
    """cols: [(label, dir)]; one row per name; a 16:9 reference is cropped to 4:3 (a shot keeps its memory line)"""
    from PIL import Image, ImageDraw
    W, H = size[0], size[1] + 12
    im = Image.new("RGB", (W * len(cols), H * len(names)))
    for r, n in enumerate(names):
        for c, (label, d) in enumerate(cols):
            f = os.path.join(d, n + ".png")
            if not os.path.exists(f):
                continue
            t = Image.open(f).convert("RGB")
            w, h = t.size
            if w * 3 > h * 4 + 200:
                x = int((w - h * 4 / 3) / 2)
                t = t.crop((x, 0, w - x, h))
            im.paste(t.resize((W, H), Image.LANCZOS), (c * W, r * H))
            ImageDraw.Draw(im).text((c * W + 6, r * H + 4), f"{label} {n}", fill=(255, 255, 0))
    im.save(path, quality=88)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("gen", nargs="+")
    ap.add_argument("--script")
    ap.add_argument("--at", nargs="+", type=float, metavar="V", help="X Y Z YAW [PITCH]")
    ap.add_argument("--name", default="spot")
    ap.add_argument("--only", help="comma-separated shot names of the script")
    ap.add_argument("--levels", choices=("near", "coarse", "hzn"))
    ap.add_argument("--ref")
    ap.add_argument("--vs", nargs="+")
    a = ap.parse_args()
    shots = script_shots(a.script) if a.script else []
    if a.at:
        shots.append((a.name, tuple(a.at[:3]), a.at[3], a.at[4] if len(a.at) > 4 else 0.1))
    if a.only:
        shots = [s for s in shots if s[0] in a.only.split(",")]
    if not shots:
        ap.error("no shots: --script or --at")
    cols = [("game", a.ref)] if a.ref else []
    for label, gens in (("before", a.vs), ("now", a.gen)):
        if not gens:
            continue
        d = os.path.join(a.out, label) if a.vs else a.out
        os.makedirs(d, exist_ok=True)
        t0 = time.time()
        blocks = [Block(g) for g in gens]
        print(f"mapshot: {label}: {len(blocks)} blocks loaded in {time.time() - t0:.1f} s", flush=True)
        for name, pos, yaw, pitch in shots:
            t0 = time.time()
            n, m = shot(os.path.join(d, name + ".png"), blocks, pos, yaw, pitch, a.levels)
            print(f"mapshot: {label} {name}: {n} tris, {time.time() - t0:.1f} s" + (f"; {mem_line(m)}" if m else ""), flush=True)
        cols.append((label, d))
    if len(cols) > 1:
        sheet(os.path.join(a.out, "sheet.jpg"), cols, [s[0] for s in shots])
        print(f"mapshot: {os.path.join(a.out, 'sheet.jpg')}")


if __name__ == "__main__":
    main()
