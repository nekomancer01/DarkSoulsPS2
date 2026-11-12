#!/usr/bin/env python3
"""LOD budget viewer: a local web page (three.js) that shows a build's streamed map blocks cell by cell and lets you pick
a level of detail per level (near / coarse / kept / backdrop) and per cell, with the world budget recomputed live.

Everything is read from a build's gen dir at run time and served on 127.0.0.1 only (game-derived data never leaves the
machine and never enters the repository):
  blocks   the blocks world.bin lists (tools/world_budget.py structural(): its lite sets), else every subdir with a
           map.bin.cells.json
  meshes   per cell and level the triangles of map.bin (resident chunks: coarse / kept / backdrop by chunk flags, the
           cell table's resident range) and cells.bin (the near blob, all parts), decoded as export_map.py load_map_bin
           does; shown with their baked vertex light x each texture's mean colour
  variants per cell and level, meshoptimizer simplifications (tools/meshopt, the binding the exporter uses) to RATIOS of
           the triangles, cell borders locked as export_map.py locks cell cuts. Bytes of a variant are estimated as the
           level's exported bytes x achieved / original triangles (streams are ~31-45 bytes per triangle either way)
  budget   tools/world_budget.py main() itself, with its cells sidecar loader fed the scaled bytes and its tier radii
           (NEAR_R, HARD_NEAR_R, HARD_R, PIN_R) set from the page: room, structural pool, arena want per spot, hard-tier
           gate. A near level picked "off" scales to 0 bytes, which world_budget counts as MS_ONE_LOD (coarse from 0 m).
The variants are a preview: the real export simplifies per mesh with export_map.py's error thresholds (EPS, EPS_FAR,
WS_PX0 / WS_PX1), so the page's "Copy choices" JSON is a record of what was picked, not an input the exporter reads yet.

  lodview.py [gen dir] [--port 8771] [--blocks a,b,...]
gen dir defaults to build/gen (BUILD=... /gen of another tree works the same). Open http://127.0.0.1:<port>/."""
import argparse
import contextlib
import http.server
import io
import json
import os
import struct
import sys
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "meshopt"))
import meshopt  # noqa: E402
import texmap  # noqa: E402
import world_budget as wb  # noqa: E402

CHUNK_BLEND, CHUNK_SKY, CHUNK_GRASS, CHUNK_COARSE, CHUNK_BACKDROP = 2, 8, 16, 64, 128  # export_map.py chunk flags
CELL_PART = 0x80000000  # export_map.py write_cells
LEVELS = ("near", "coarse", "kept", "backdrop")  # map.bin.cells.json columns 9-12
RATIOS = (1.0, 0.75, 0.5, 0.35, 0.25, 0.15)
# budgets the page shows for reference: not computed from the gen dir (latest measurements, notes / PRs named)
REFERENCE = [
    dict(name="Free VRAM", value="651-759 KB", src="ISO route after PR #165 (BUMP_BAKE)"),
    dict(name="HZN_R / MS_R_HZN", value="32 / 40 m", src="strip branch (main: map.c HZN_R 40)"),
    dict(name="Logic pass", value="<= 5 ms", src="frame-time plan"),
    dict(name="Frame gate", value="<= 34 ms", src="tools/test-run.py hitch gate"),
    dict(name="Disc seeks", value="<= 2 / s", src="DVD-9 budget (mapstream MS_SCAN)"),
]


def decode_chunk(b, rec, base):
    """one 64-byte chunk record at rec (stream offsets relative to base) -> (texture, flags, pos (n,3,3), light (n,3,4));
    the same decoding as export_map.py load_map_bin"""
    ti, fl, n, po, so, co = struct.unpack_from("<iII3I", b, rec)
    n, nv = n & 0xFFFF, n >> 16
    org = np.array(struct.unpack_from("<3f", b, rec + 48), np.float32)
    step = struct.unpack_from("<f", b, rec + 60)[0]
    po, co = po + base, co + base
    if nv:
        ix = np.frombuffer(b, np.uint8, n * 4, co + -(-4 * nv // 16) * 16 + 16).reshape(n, 4)[:, :3]
        pv = np.frombuffer(b, np.int16, nv * 3, po).reshape(nv, 3)[ix]
        cv = np.frombuffer(b, np.uint8, nv * 4, co).reshape(nv, 4)[ix]
    else:
        pv = np.frombuffer(b, np.int16, n * 9, po).reshape(n, 3, 3)
        cv = np.frombuffer(b, np.uint8, n * 12, co).reshape(n, 3, 4)
    return ti, fl, org + step * pv.astype(np.float32), cv


def tex_means(mb):
    """map.bin -> (ntex, 3) mean colour of each texture (opaque texels), 0..1"""
    ntex, otex = struct.unpack_from("<I", mb, 8)[0], struct.unpack_from("<I", mb, 40)[0]
    out = []
    for w, h, pal, ix in texmap.load(mb, ntex, otex):
        px = pal[ix.reshape(-1)]
        op = px[px[:, 3] > 0]
        out.append((op if len(op) else px)[:, :3].mean(0) / 255.0)
    return np.array(out, np.float32).reshape(-1, 3)


def weld(pos, col):
    """triangle soup -> (vertices, colours u8 (n,4), indices u32): equal position + colour share a vertex"""
    P = pos.reshape(-1, 3)
    C = col.reshape(-1, 3)
    key = np.concatenate([np.round(P * 256).astype(np.int64), C.astype(np.int64)], 1)
    u, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
    idx = inv.reshape(-1).astype(np.uint32)
    t = idx.reshape(-1, 3)
    t = t[(t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])]
    c4 = np.concatenate([C[first], np.full((len(first), 1), 255, np.uint8)], 1)
    return P[first].astype(np.float32), c4.astype(np.uint8), t.reshape(-1)


class Block:
    def __init__(self, d):
        self.dir, self.name = d, os.path.basename(os.path.normpath(d))
        j = json.load(open(os.path.join(d, "map.bin.cells.json")))
        self.cells = np.asarray(j["cells"], np.float64).reshape(len(j["cells"]), -1)
        self.spots = np.asarray(j["spots"], np.float64).reshape(-1, 3)
        self.sky_b = float(j.get("sky", 0))
        mb = open(os.path.join(d, "map.bin"), "rb").read()
        cb = open(os.path.join(d, "cells.bin"), "rb").read()
        nch = struct.unpack_from("<I", mb, 4)[0]
        och = struct.unpack_from("<I", mb, 36)[0]
        cto = struct.unpack_from("<I", mb, 76)[0]
        tm = tex_means(mb)
        nent = struct.unpack_from("<I", mb, cto)[0]
        soup = {}  # (cell, level) -> [(pos, colour)]
        used = np.zeros(nch, bool)
        ci = -1

        def add(c, lev, ti, fl, pos, light):
            tc = tm[ti] if 0 <= ti < len(tm) else np.full(3, 0.6, np.float32)
            rgb = np.clip(light[..., :3].astype(np.float32) / 128.0 * tc * 255.0, 0, 255).astype(np.uint8)
            soup.setdefault((c, lev), []).append((pos, rgb))
        for e in range(nent):
            off, nb, nn, first, nres = struct.unpack_from("<5I", mb, cto + 16 + 48 * e + 24)
            if not nn & CELL_PART:
                ci += 1
            for k in range(nn & 0xFFFF):
                ti, fl, pos, light = decode_chunk(cb, off + 64 * k, off)
                add(ci, 0, ti, fl, pos, light)
            for i in range(first, first + nres):
                ti, fl, pos, light = decode_chunk(mb, och + 64 * i, 0)
                used[i] = True
                add(ci, 1 if fl & CHUNK_COARSE else 3 if fl & CHUNK_BACKDROP else 2, ti, fl, pos, light)
        assert ci + 1 == len(self.cells), f"{self.name}: {ci + 1} cell table cells, {len(self.cells)} in cells.json"
        sky = []
        for i in np.flatnonzero(~used):
            ti, fl, pos, light = decode_chunk(mb, och + 64 * i, 0)
            if fl & CHUNK_SKY:
                sky.append((pos, np.clip(light[..., :3].astype(np.float32) / 128.0 *
                                         (tm[ti] if 0 <= ti < len(tm) else 0.6) * 255, 0, 255).astype(np.uint8)))
        self.sky = weld(np.concatenate([p for p, _ in sky]), np.concatenate([c for _, c in sky])) if sky else None
        # variants[(cell, level)] = [(vertices, colours, indices) per RATIOS]
        self.variants, self.tris = {}, np.zeros((len(self.cells), 4), np.int64)
        self.achieved = np.ones((len(self.cells), 4, len(RATIOS)))
        for (c, lev), parts in soup.items():
            v, col, idx = weld(np.concatenate([p for p, _ in parts]), np.concatenate([q for _, q in parts]))
            n0 = len(idx) // 3
            self.tris[c, lev] = n0
            out = []
            for r, ratio in enumerate(RATIOS):
                if r == 0 or n0 < 8:
                    s = idx
                else:
                    s, _ = meshopt.simplify(v, idx, max(1, int(n0 * ratio)), error=1e9, lock_border=True)
                out.append(s)
                self.achieved[c, lev, r] = len(s) / 3 / max(n0, 1)
            self.variants[(c, lev)] = (v, col, out)

    def meta(self):
        c = self.cells
        return dict(name=self.name, spots=np.round(self.spots, 2).reshape(-1).tolist(),
                    cells=[dict(key=c[i, 0:3].astype(int).tolist(), box=np.round(c[i, 3:9], 2).tolist(),
                                near_box=np.round(c[i, 13:19], 2).tolist() if c.shape[1] >= 19 else np.round(c[i, 3:9], 2).tolist(),
                                kb=np.round(c[i, 9:13] / 1024, 1).tolist(), tris=self.tris[i].tolist(),
                                achieved=np.round(self.achieved[i], 3).tolist()) for i in range(len(c))])

    def mesh(self, lev, r, cells=None):
        """binary: u32 count; per cell u32 cell, nv, nidx; f32 pos[nv*3]; u8 rgba[nv*4]; u32 idx[nidx]"""
        out = io.BytesIO()
        items = [(c, self.variants[(c, lev)]) for c in range(len(self.cells)) if (c, lev) in self.variants
                 and (cells is None or c in cells)] if lev >= 0 else ([(-1, (self.sky[0], self.sky[1], [self.sky[2]]))]
                                                                     if self.sky is not None else [])
        out.write(struct.pack("<I", len(items)))
        for c, (v, col, var) in items:
            idx = var[min(r, len(var) - 1)]
            out.write(struct.pack("<iII", c, len(v), len(idx)))
            out.write(v.astype("<f4").tobytes())
            out.write(col.tobytes())
            out.write(idx.astype("<u4").tobytes())
        return out.getvalue()


class Model:
    def __init__(self, gen, only=None):
        self.gen = os.path.abspath(gen)
        self.pools = wb.structural(self.gen)
        names = list(self.pools["lite_kb"]) if self.pools else sorted(os.listdir(self.gen))
        if only:
            names = [n for n in names if n in only]
        dirs = [os.path.join(self.gen, n) for n in names if os.path.exists(os.path.join(self.gen, n, "map.bin.cells.json"))]
        self.blocks = []
        for d in dirs:
            t = time.time()
            self.blocks.append(Block(d))
            print(f"lodview: {os.path.basename(d)}: {len(self.blocks[-1].cells)} cells, "
                  f"{int(self.blocks[-1].tris.sum())} triangles, variants in {time.time() - t:.1f} s", flush=True)
        self.lock = threading.Lock()
        self._box, self._box_d = {}, wb.box_d
        self.defaults = dict(NEAR_R=wb.NEAR_R, HARD_NEAR_R=wb.HARD_NEAR_R, HARD_R=wb.HARD_R, PIN_R=wb.PIN_R)

    def box_d(self, lo, hi, P):
        """world_budget.box_d, memoized: the boxes and spots do not change with the picked levels (most of a run)"""
        key = hash((lo.tobytes(), hi.tobytes(), P.shape))
        if key not in self._box:
            self._box[key] = self._box_d(lo, hi, P)
        return self._box[key]

    def budget(self, factors, radii):
        """world_budget.main over the blocks with each cell's level bytes x factors[block][cell][level]"""
        scaled = {}
        for b in self.blocks:
            c = b.cells.copy()
            f = factors.get(b.name)
            if f is not None:
                c[:, 9:13] *= np.asarray(f, np.float64).reshape(len(c), 4)
            scaled[b.dir] = (b.name, c, b.sky_b, b.spots)
        with self.lock:
            saved = (wb.load, wb.structural, wb.box_d, {k: getattr(wb, k) for k in self.defaults})
            wb.load = lambda d: scaled.get(d)
            wb.box_d = self.box_d
            wb.structural = lambda gen: self.pools
            for k, v in self.defaults.items():
                setattr(wb, k, float(radii.get(k, v)))
            out = os.path.join(self.gen, ".lodview_budget.json")
            log = io.StringIO()
            try:
                with contextlib.redirect_stdout(log):
                    rc = wb.main([out] + [b.dir for b in self.blocks] + ["--strict"])
                res = json.load(open(out))
                L = wb.LAST
                res.update(log=log.getvalue().splitlines(), strict_fail=bool(rc),
                           spot_total=np.round(L["total"]).astype(int).tolist(),
                           spot_hard=np.round(L["tot"]["hard"]).astype(int).tolist(),
                           spot_near=np.round(L["tot"]["near"]).astype(int).tolist(),
                           spot_coarse=np.round(L["tot"]["coarse"]).astype(int).tolist(),
                           spot_kept=np.round(L["tot"]["kept"]).astype(int).tolist(),
                           spot_far=np.round(L["tot"]["far"]).astype(int).tolist(),
                           spot_gated=L["gated"].astype(int).tolist(), arena_kb=wb.ARENA_KB, fill_pct=wb.FILL_PCT)
            finally:
                wb.load, wb.structural, wb.box_d = saved[0], saved[1], saved[2]
                for k, v in saved[3].items():
                    setattr(wb, k, v)
                with contextlib.suppress(OSError):
                    os.remove(out)
        return res

    def meta(self):
        return dict(gen=os.path.relpath(self.gen, os.path.join(HERE, "..")) if self.gen.startswith(os.path.abspath(os.path.join(HERE, ".."))) else os.path.basename(self.gen),
                    ratios=RATIOS, levels=LEVELS, pools=self.pools, radii=self.defaults, reference=REFERENCE,
                    near_cube=wb.NEAR_CUBE, blocks=[b.meta() for b in self.blocks])


def handler(model, page_dir):
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, body, ctype, code=200):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            from urllib.parse import parse_qs, urlparse
            u = urlparse(self.path)
            q = {k: v[0] for k, v in parse_qs(u.query).items()}
            if u.path in ("/", "/index.html", "/lodview.js"):
                f = "index.html" if u.path != "/lodview.js" else "lodview.js"
                return self.send(open(os.path.join(page_dir, f), "rb").read(),
                                 "text/html; charset=utf-8" if f.endswith("html") else "text/javascript")
            if u.path == "/api/meta":
                return self.send(json.dumps(model.meta()).encode(), "application/json")
            if u.path == "/api/mesh":
                b = model.blocks[int(q["block"])]
                lev = -1 if q["level"] == "sky" else LEVELS.index(q["level"])
                return self.send(b.mesh(lev, int(q.get("r", 0))), "application/octet-stream")
            self.send(b"not found", "text/plain", 404)

        def do_POST(self):
            if self.path != "/api/budget":
                return self.send(b"not found", "text/plain", 404)
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])) or b"{}")
            t = time.time()
            res = model.budget(body.get("factors", {}), body.get("radii", {}))
            res["ms"] = round((time.time() - t) * 1000)
            self.send(json.dumps(res).encode(), "application/json")
    return H


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("gen", nargs="?", default=os.path.join(HERE, "..", "build", "gen"))
    ap.add_argument("--port", type=int, default=8771)
    ap.add_argument("--blocks", default="")
    a = ap.parse_args()
    model = Model(a.gen, set(a.blocks.split(",")) if a.blocks else None)
    if not model.blocks:
        sys.exit(f"lodview: no block with map.bin.cells.json under {a.gen} (STREAM=1 build)")
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), handler(model, os.path.join(HERE, "lodview")))
    print(f"lodview: http://127.0.0.1:{a.port}/ ({len(model.blocks)} blocks)", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
