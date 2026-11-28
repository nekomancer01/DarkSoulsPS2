#!/usr/bin/env python3
"""The coverage scan's reader errors and the block report gate without game data (tools/ci/offline.sh step 7).

  1 every reader raises souls.UnknownVariant (an AssertionError and a ValueError, as before) on a file it does not
    know, and souls.skipped() reaches the scan's hook, or raises under SOULS_STRICT
  1b havok_collision.convex_hull_tris triangulates a synthetic convex hull (scan-8)
  2 tools/scan/support.py reads the runtime's handler tables (lua0.c opcodes, ESD handlers, export_emevd classes)
  3 tools/blockreport.py --strict on a synthetic block passes, and fails when the block's effect textures exceed
    gs_budget.h's FX row or its Lua pack holds an opcode lua0.c lacks; it reads the Polaris 3.3 chunked objects.bin
Exit code = number of failed checks."""
import json
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools", "scan"))
import souls  # noqa: E402

FAILS = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        FAILS.append(what)


def raises_unknown(fn, *a):
    try:
        fn(*a)
    except souls.UnknownVariant as e:
        return isinstance(e, AssertionError) and isinstance(e, ValueError) and e.kind
    except Exception as e:  # noqa: BLE001
        return f"raised {type(e).__name__}"
    return None


junk = b"JUNK" + bytes(252)
import emevd, esd, ffx, lua50  # noqa: E402,E401
for name, fn in (("souls.flver", souls.flver), ("souls.tae", souls.tae), ("souls.bnd3", souls.bnd3),
                 ("souls.HkTag", souls.HkTag), ("ffx.parse", ffx.parse), ("esd.ESD", esd.ESD),
                 ("emevd.parse", emevd.parse), ("emevd.edf", emevd.edf), ("lua50.load", lua50.load)):
    k = raises_unknown(fn, junk)
    check(isinstance(k, str) and not k.startswith("raised"), f"{name} on an unknown file: UnknownVariant ({k})")
check(raises_unknown(souls.undcx, b"DCX\0" + bytes(36) + b"DCP\0KRAK" + bytes(16)) == "dcx compression",
      "souls.undcx on a non-DFLT DCX: UnknownVariant")

seen = []
souls.SEEN = lambda k, v: seen.append((k, v))
souls.skipped("flver vertex member", "x")
souls.SEEN = None
check(seen == [("flver vertex member", "x")], "souls.skipped reaches the scan's hook")
souls.STRICT = True
check(raises_unknown(souls.skipped, "k", 1) == "k", "souls.skipped raises under SOULS_STRICT")
souls.STRICT = False
check(souls.flver_member_read(0x02, 0, 0) and not souls.flver_member_read(0x16, 5, 1),
      "flver_member_read: position read, a second UV set not")

import support  # noqa: E402
check(len(support.lua_ops()) >= 30, f"lua0.c opcodes parsed ({len(support.lua_ops())})")
for kind, (env, cmd) in support.esd_handled().items():
    check(len(env) > 5 and len(cmd) > 0, f"ESD {kind} handlers parsed (env {len(env)}, commands {len(cmd)})")
check(len(support.emevd_class()) > 100, "export_emevd classes read")
import kinds  # noqa: E402
r = kinds.Recorder()
r("k", 1, "ok")
r("k", 1, "unknown")
r("k", 1, "ok", 2)
check(r.rows()[0][2:4] == ["unknown", 4], "scan recorder: the worst status wins, counts add")

import havok_collision  # noqa: E402
# scan-8: a synthetic hkpConvexVerticesShape hull (a 2 x 1 x 3 box off the origin, a repeated padding point) -> 12
# triangles, every one wound as DS collision (cross product into the hull); a flat quad (two opposite face planes, as
# m13_02's NPC wall) -> both sides; a rounded hull -> UnknownVariant
box = [(x, y, z) for x in (10, 12) for y in (-1, 0) for z in (5, 8)]
planes = [(1, 0, 0, -12), (-1, 0, 0, 10), (0, 1, 0, 0), (0, -1, 0, -1), (0, 0, 1, -8), (0, 0, -1, 5)]
tris = havok_collision.convex_hull_tris(dict(vertices=box + box[:2], planes=planes, radius=0.0))
ctr = (11, -0.5, 6.5)


def tcross(t):
    u, v = [t[1][i] - t[0][i] for i in range(3)], [t[2][i] - t[0][i] for i in range(3)]
    return u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]


inward = lambda t: sum(tcross(t)[i] * (ctr[i] - t[0][i]) for i in range(3)) > 0
area = sum(sum(x * x for x in tcross(t)) ** 0.5 / 2 for t in tris)
check(len(tris) == 12 and all(inward(t) for t in tris) and abs(area - 2 * (2 * 1 + 2 * 3 + 1 * 3)) < 1e-9
      and {p for t in tris for p in t} == set(box),
      f"convex_hull_tris: a box hull -> 12 inward-wound triangles covering its surface ({len(tris)})")
quad = [(0, 0, 0), (2, 0, 0), (2, 3, 0), (0, 3, 0)]
qp = [(0, 0, 1, 0), (0, 0, -1, 0), (1, 0, 0, -2), (-1, 0, 0, 0), (0, 1, 0, -3), (0, -1, 0, 0)]
qt = havok_collision.convex_hull_tris(dict(vertices=quad, planes=qp, radius=0.0))
nz = sorted(tcross(t)[2] for t in qt)
check(len(qt) == 4 and nz[0] < 0 < nz[-1] and nz[1] < 0 < nz[2], f"convex_hull_tris: a flat hull -> both sides ({len(qt)})")
check(raises_unknown(havok_collision.convex_hull_tris, dict(vertices=box, planes=planes, radius=0.05))
      == "havok convex radius", "convex_hull_tris on a rounded hull: UnknownVariant")


def block(tmp, fx_tex, lua_op):
    """a gen dir with one cell around a spot, an fx.bin of fx_tex (w, h) textures and an ai_map.lua0 of one op"""
    d = os.path.join(tmp, "m10_01_00_00_t")
    os.makedirs(d, exist_ok=True)
    cells = [[0, 0, 0, 2, -1, -1, 8, 1, 1, 1000 * 1024, 0, 0, 0]]
    json.dump(dict(cells=cells, sky=0, spots=[[0, 0, 0]]), open(os.path.join(d, "map.bin.cells.json"), "w"))
    off_tex = 64
    head = struct.pack("<4s7I6I2I", b"DSFX", 1, len(fx_tex), 0, 0, 0, 0, 0, off_tex, 0, 0, 0, 0, 0, 0, 0)
    tab = b"".join(struct.pack("<4H2I", w, h, 1, 1, 0, 0) for w, h in fx_tex)
    open(os.path.join(d, "fx.bin"), "wb").write(head + tab)
    proto = struct.pack("<5I", 0, 0, 1, 0, 0) + struct.pack("<I", lua_op)
    open(os.path.join(d, "ai_map.lua0"), "wb").write(b"DSRL" + struct.pack("<I", 1) + struct.pack("<I", 4) + b"main"
                                                      + proto)
    return d


def run(tmp, fx_tex, lua_op=0):
    d = block(tmp, fx_tex, lua_op)
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "blockreport.py"),
                        os.path.join(tmp, "br.json"), d, "--strict"], capture_output=True, text=True,
                       env=dict(os.environ, MAP="default"))
    return p.returncode, p.stdout + p.stderr


with tempfile.TemporaryDirectory() as tmp:
    import gs_budget
    row = gs_budget.GSB["fx"]
    rc, out = run(tmp, [(128, 128)] * 4)
    check(rc == 0, "blockreport --strict passes a block inside its budgets" + ("" if rc == 0 else "\n" + out))
    n = row // 17 + 1  # 128 x 128 PSMT8 + CLUT = 17 KB each
    rc, out = run(tmp, [(128, 128)] * n)
    check(rc == 1 and "VRAM fx" in out, f"blockreport --strict fails {n * 17} KB of effect textures over the {row} KB FX row"
          + ("" if rc == 1 else "\n" + out))
    left = [f for f in ("br.json", "br.world_budget.json", os.path.join("blockreport", "m10_01_00_00_t.json"))
            if os.path.exists(os.path.join(tmp, f))]
    check(not left and os.path.exists(os.path.join(tmp, "br.json.failed")),
          f"a failed gate leaves its outputs as .failed only, so a second make runs it again (left: {left})")
    rc, out = run(tmp, [(128, 128)], lua50.OPS.index("TFORLOOP") if "TFORLOOP" not in support.lua_ops() else 63)
    check(rc == 1 and "opcodes lua0.c lacks" in out, "blockreport --strict fails a Lua pack with an opcode lua0.c lacks"
          + ("" if rc == 1 else "\n" + out))
    rc, out = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "blockreport.py"),
                              os.path.join(tmp, "br.json"), block(tmp, [(128, 128)] * n, 0), "--strict"],
                             capture_output=True, text=True, env=dict(os.environ, MAP="default",
                                                                      BLOCKREPORT_STRICT="0")).returncode, ""
    check(rc == 0, "BLOCKREPORT_STRICT=0 reports a miss without failing")
import blockreport  # noqa: E402
# a Polaris 3.3 chunked objects.bin (file id OBJS) whose BTEX texture model holds one 128 x 64 texture
dsrm = b"DSRM" + struct.pack("<6I", 0, 0, 0, 0, 1, 0) + struct.pack("<8I", 0, 0, 0, 0, 0, 64, 0, 0) + bytes(4)
dsrm += struct.pack("<4I", 128, 64, 0, 0)
body = struct.pack("<4sHHII", b"BTEX", 1, 0, len(dsrm), 0) + dsrm + bytes(-len(dsrm) % 16)
objs = struct.pack("<4s4sII", b"CHNK", b"OBJS", 32 + len(body), 1) + body + struct.pack("<4sIII", b"CEND", 0, 0, 0)
with tempfile.TemporaryDirectory() as tmp:
    open(os.path.join(tmp, "objects.bin"), "wb").write(objs)
    v = blockreport.vram(tmp).get("obj", {})
    check(v.get("kb") == 9.0 and v.get("textures") == 1, f"blockreport reads a chunked objects.bin's BTEX textures ({v})")
    open(os.path.join(tmp, "objects.bin"), "wb").write(b"NEW?" + bytes(60))
    check(blockreport.vram(tmp)["obj"].get("unread") == "NEW?", "blockreport flags an objects.bin format it cannot read")
# the sets of blocks on together at a seam spot (world.bin links) against the rows: two blocks whose FX sets fit alone
# but not together fail; fit together, pass; blocks never on together are not summed
import chunkfile  # noqa: E402
with tempfile.TemporaryDirectory() as tmp:
    boot = os.path.join(tmp, "gen")
    os.makedirs(boot)
    blks = b"".join(struct.pack("<16s16sII24x", nm, mp, 1 if i == 0 else 0, nb)
                    for i, (nm, mp, nb) in enumerate(((b"boot", b"m10_02_00_00", 0), (b"m10_01_00_00_b", b"m10_01_00_00", 1),
                                                      (b"m10_01_00_00_p", b"m10_01_00_00", 0))))
    nbrs = bytes([0]) + bytes(15) + bytes([1, 0]) + bytes(14) + bytes([0]) + bytes(15)
    lrnl = struct.pack("<6I", 0, 0, 0, 1, 1, 0)  # block 1: one link (loads its neighbour 0); block 2: none
    lnkl = struct.pack("<IHHHH", 0, 1, 1, 0, 0)
    chunkfile.write(os.path.join(boot, "world.bin"), b"WRLD", [(b"BLKS", 2, blks), (b"NBRS", 1, nbrs),
                                                              (b"LRNL", 1, lrnl), (b"LNKL", 1, lnkl)])
    fxrow = gs_budget.GSB["fx"]
    half = fxrow * 0.6
    vr = lambda kb: dict(fx=dict(kb=kb), obj=dict(kb=1.0), chr=dict(kb=1.0))  # noqa: E731
    spots = blockreport.spot_live_vram(boot, {"gen": vr(half), "m10_01_00_00_b": vr(half), "m10_01_00_00_p": vr(half)})
    check(half <= fxrow and len(spots) == 1 and spots[0][1] == "fx" and spots[0][0] == ["boot", "m10_01_00_00_b"]
          and spots[0][2] > fxrow, f"blockreport: two blocks on together whose FX sets fit alone but not together fail ({spots})")
    spots = blockreport.spot_live_vram(boot, {"gen": vr(fxrow * 0.4), "m10_01_00_00_b": vr(fxrow * 0.4),
                                              "m10_01_00_00_p": vr(fxrow * 0.9)})
    check(not spots, f"blockreport: two blocks whose FX sets fit together pass, a block never on with them is not summed ({spots})")
print(f"test_scan: {len(FAILS)} failed")
sys.exit(len(FAILS))
