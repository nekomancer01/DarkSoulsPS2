#!/usr/bin/env python3
"""World budget check across blocks (world streaming v2, TECH_DEBT world-10).

export_map.py's spot budget solve (solve_budget) sizes each block on its own: near level within WS_R0 of a navmesh
point under WS_B0, coarse level within WS_R1_IN..WS_R1 under WS_B1. At runtime the arena holds every block's items at
once, so near a seam two or three blocks' levels add up, and nothing checked the sum. This tool reads each block's
map.bin.cells.json (export_map.py: per cell its box and its bytes by level, the navmesh spots) and measures, at every
spot of every block, what all blocks together ask of the arena:

  near    near parts of cells within NEAR_R (mapstream.c MS_R0_DRAW: map.c draws them there), measured to the cell's
          box clipped to its cube (NEAR_CUBE, mapstream.h cell_near_d2)
  coarse  coarse level of cells from NEAR_R to PIN_R (MS_R1_PIN, pinned), and of nearer cells without a near level
          (MS_ONE_LOD: a cell whose near parts are in leaves its coarse level to them)
  kept    kept meshes (resident, not coarse) of cells within PIN_R
  far     the backdrop and sky of the spot's own block (pinned)

The sum is checked against ROOM (KB): with the arena's pools (mapstream.c MS_POOLS, the Burg plan's step B2) the visual
room is the project budget's 10 MB map arena less the structural pool (world.c world_struct_bytes, computed here the
same way from world.bin's links and the blocks' lite-set entries: "structural_kb") and the misc pool (largest relief +
largest texture buffer + WORLD_MISC_EXTRA), x MS_FILL_PCT 92 %; without world.bin, the old ROOM_KB (one neighbour's
lite set of up to 2064 KB out). The per-block budgets the per-block budgets are summed across blocks (near within WS_R0 against WS_B0,
coarse within WS_R1_IN..WS_R1 against WS_B1) to show what the seams add. Writes <out>.json and prints one line each.

  world_budget.py <out.json> <gen dir> [<gen dir> ...] [--room KB] [--strict]

--strict: exit 1 when a spot is over ROOM (the build only warns: the solve's error caps leave dense spots over).
Numbers are box distances, as mapstream.c ranks items."""
import json
import os
import sys

import numpy as np

NEAR_R = 26.0   # mapstream.c MS_R0_DRAW
PIN_R = 64.0    # mapstream.c MS_R1_PIN (= MS_R_HZN: the horizon stands for the coarse level beyond)
WS_R0, WS_R1_IN, WS_R1 = 30.0, 16.0, 96.0  # export_map.py
ROOM_KB = 10240 * 92 // 100 - 2064  # the budget's 10 MB arena (without world.bin)
ARENA_KB = 10240
FILL_PCT = 92        # mapstream.c MS_FILL_PCT
WORLD_SLOTS = 3      # world.c
MISC_EXTRA_KB = 640  # world.c WORLD_MISC_EXTRA
STRUCT_WHOLE = int(os.environ.get("WORLD_STRUCT_WHOLE", WORLD_SLOTS))  # world.c WORLD_STRUCT_WHOLE
RELIEF_POOL = os.environ.get("WORLD_RELIEF_POOL", "0") == "1"  # world.c WORLD_RELIEF_POOL
LITE_DIET = os.environ.get("WORLD_LITE_DIET", "1") != "0"  # world.c WORLD_LITE_DIET


def r128(n):
    return (n + 127) & ~127


def lite_entries(d):
    """a block's gen dir -> (lite set bytes as world.c counts them, its map tables' bytes, relief, texture buffer)"""
    import struct
    sz = lambda f: os.path.getsize(os.path.join(d, f)) if os.path.exists(os.path.join(d, f)) else 0
    tab = open(os.path.join(d, "maptab.bin"), "rb").read()
    full = len(tab)
    chunk_off, tex_off = struct.unpack_from("<2I", tab, 36)
    cell_off = struct.unpack_from("<I", tab, 76)[0]
    tex, cell = tex_off - chunk_off + 80, cell_off - chunk_off + 80
    b0, c0 = tex & ~127, cell & ~127  # world.c tab_split
    tabs = r128(tex) + r128(cell - b0) + r128(full - c0)
    total = r128(sz("collision.bin")) + tabs + r128(sz("events.bin"))
    return total, tabs, r128(sz("bump.bin")), max(r128(sz("objects.bin")), r128(sz("fx.bin")))


def structural(gen):
    """world.bin (gen dir) -> dict(structural_kb, misc_kb, worst link) as world.c world_struct_bytes / world_init, or None"""
    import struct
    p = os.path.join(gen, "world.bin")
    if not os.path.exists(p):
        return None
    b = open(p, "rb").read()
    if b[:4] != b"DSW1":
        return None
    n = struct.unpack_from("<I", b, 8)[0]
    names = [b[16 + 64 * i:32 + 64 * i].split(b"\0")[0].decode() for i in range(n)]
    lr = struct.unpack_from(f"<{2 * n}I", b, 16 + 64 * n)
    lbase = 16 + 64 * n + 8 * n
    ent = {}
    for i, nm in enumerate(names[1:], 1):
        d = os.path.join(gen, nm)
        if os.path.exists(os.path.join(d, "maptab.bin")):
            ent[i] = lite_entries(d)
    best, at = 0, None
    for blk in range(n):
        for l in range(lr[2 * blk + 1]):
            first, cnt, load, keep = struct.unpack_from("<IHBB", b, lbase + 8 * (lr[2 * blk] + l))
            load |= (1 << blk) if blk else 0
            keep |= load
            on = [i for i in ent if keep >> i & 1]
            lean_of = {i: ent[i][0] - ent[i][1] if LITE_DIET else ent[i][0] for i in on}
            whole = sorted((i for i in on if (load >> i & 1) or not LITE_DIET), key=lambda i: -ent[i][0])
            v = sorted([ent[i][0] for i in whole[:STRUCT_WHOLE]] + [lean_of[i] for i in on if i not in whole[:STRUCT_WHOLE]],
                       reverse=True)[:WORLD_SLOTS]
            if sum(v) > best:
                best, at = sum(v), (names[blk], first, load, keep)
    misc = (max((e[2] for e in ent.values()), default=0) if RELIEF_POOL else 0) + max((e[3] for e in ent.values()), default=0) + (MISC_EXTRA_KB << 10)
    return dict(structural_kb=best >> 10, misc_kb=misc >> 10, worst_link=dict(block=at[0], tri=at[1], load=at[2], keep=at[3]) if at else None,
                lite_kb={names[i]: dict(whole=e[0] >> 10, lean=(e[0] - e[1]) >> 10) for i, e in ent.items()})
NEAR_CUBE = os.environ.get("NEAR_CUBE", "1") == "1"  # export_map.py NEAR_CUBE (runtime MS_NEAR_CUBE)


def load(d):
    """gen dir -> (name, cells array (n, 10), sky bytes, spots (m, 3)) or None without a cells sidecar"""
    p = os.path.join(d, "map.bin.cells.json")
    if not os.path.exists(p):
        return None
    j = json.load(open(p))
    c = np.asarray(j["cells"], np.float64).reshape(len(j["cells"]), -1)
    return os.path.basename(os.path.normpath(d)), c, float(j.get("sky", 0)), np.asarray(j["spots"], np.float64).reshape(-1, 3)


def box_d(lo, hi, P):
    """(m spots) x (n boxes) box distances"""
    return np.sqrt((np.maximum(0, np.maximum(lo[None] - P[:, None], P[:, None] - hi[None])) ** 2).sum(2))


def pct(x):
    return dict(p50=round(float(np.percentile(x, 50))), p90=round(float(np.percentile(x, 90))),
                p99=round(float(np.percentile(x, 99))), max=round(float(x.max())))


def main(argv):
    room = ROOM_KB
    strict = "--strict" in argv
    args = [a for a in argv if a != "--strict"]
    if "--room" in args:
        i = args.index("--room")
        room = float(args[i + 1])
        del args[i:i + 2]
    out, dirs = args[0], args[1:]
    pools = structural(os.path.dirname(os.path.abspath(out)))
    if pools and "--room" not in argv:
        room = (ARENA_KB - pools["structural_kb"] - pools["misc_kb"]) * FILL_PCT // 100
    env = {}
    for d in dirs:
        try:
            env.update(dict(kv.split("=", 1) for kv in open(os.path.join(d, "mapenv")).read().split() if "=" in kv))
        except OSError:
            pass
    b0 = float(os.environ.get("WS_B0", env.get("WS_B0", 2560)))
    b1 = float(os.environ.get("WS_B1", env.get("WS_B1", 2048)))
    blocks = [b for b in (load(d) for d in dirs) if b]
    if not blocks:
        print("world budget: no map.bin.cells.json (no streamed block)")
        json.dump(dict(blocks=[]), open(out, "w"))
        return 0
    P = np.concatenate([s for _, _, _, s in blocks])
    own = np.concatenate([np.full(len(s), k) for k, (_, _, _, s) in enumerate(blocks)])
    tot = {k: np.zeros(len(P)) for k in ("near", "coarse", "kept", "far", "near_ws", "coarse_ws")}
    per_block = []
    for k, (name, c, sky, _) in enumerate(blocks):
        lo, hi = c[:, 3:6], c[:, 6:9]
        near, coarse, kept, back = (c[:, 9 + i] / 1024 for i in range(4))
        d = box_d(lo, hi, P)
        if NEAR_CUBE:  # the near level by the box clipped to the cell's cube (mapstream.h cell_near_d2)
            k0 = c[:, 0:3] * 32.0
            nlo = np.maximum(lo, k0)
            dn = box_d(nlo, np.maximum(np.minimum(hi, k0 + 32.0), nlo), P)
        else:
            dn = d
        has_near = near > 0
        n = (dn < NEAR_R).astype(np.float64) @ near
        co = (((dn >= NEAR_R) | ~has_near[None]) & (d < PIN_R)).astype(np.float64) @ coarse
        ke = (d < PIN_R).astype(np.float64) @ kept
        far = np.where(own == k, back.sum() + sky / 1024, 0.0)
        nws = (dn < WS_R0).astype(np.float64) @ near
        cws = ((d >= WS_R1_IN) & (d < WS_R1)).astype(np.float64) @ coarse
        for key, v in (("near", n), ("coarse", co), ("kept", ke), ("far", far), ("near_ws", nws), ("coarse_ws", cws)):
            tot[key] += v
        per_block.append(dict(block=name, cells=len(c), near_kb=round(float(near.sum())),
                              coarse_kb=round(float(coarse.sum())), kept_kb=round(float(kept.sum())),
                              backdrop_kb=round(float(back.sum() + sky / 1024)), spots=int((own == k).sum())))
    total = tot["near"] + tot["coarse"] + tot["kept"] + tot["far"]
    w = int(total.argmax())
    over = float((total > room).mean() * 100)
    res = dict(room_kb=room, pools=pools, near_r=NEAR_R, pin_r=PIN_R, blocks=per_block, spots=len(P),
               total=pct(total), near=pct(tot["near"]), coarse=pct(tot["coarse"]), kept=pct(tot["kept"]),
               far=pct(tot["far"]), spots_over_room_pct=round(over, 1),
               worst=dict(pos=[round(float(v), 1) for v in P[w]], block=blocks[own[w]][0],
                          **{k: round(float(v[w])) for k, v in tot.items()}),
               near_ws=dict(budget_kb=b0, over_pct=round(float((tot["near_ws"] > b0).mean() * 100), 1), **pct(tot["near_ws"])),
               coarse_ws=dict(budget_kb=b1, over_pct=round(float((tot["coarse_ws"] > b1).mean() * 100), 1),
                              **pct(tot["coarse_ws"])))
    with open(out, "w") as f:
        json.dump(res, f, indent=1)
    t = res["total"]
    if pools:
        print(f"world budget: structural pool {pools['structural_kb']} KB (link {pools['worst_link']}), misc "
              f"{pools['misc_kb']} KB: visual room {room} KB of the {ARENA_KB} KB arena at {FILL_PCT} %; lite sets "
              + ", ".join(f"{k} {v['whole']}/{v['lean']}" for k, v in pools["lite_kb"].items()) + " KB whole / lean")
    print(f"world budget: {len(blocks)} blocks, {len(P)} spots: arena wants p50 {t['p50']} / p90 {t['p90']} / max "
          f"{t['max']} KB (near within {NEAR_R:.0f} m p90 {res['near']['p90']}, coarse to {PIN_R:.0f} m p90 "
          f"{res['coarse']['p90']}, kept p90 {res['kept']['p90']}, backdrop + sky p90 {res['far']['p90']}) against "
          f"{room:.0f} KB room: {over:.1f} % of the spots over (worst {res['worst']['block']} {res['worst']['pos']})")
    print(f"world budget: blocks summed, near within {WS_R0:.0f} m p90 {res['near_ws']['p90']} / max "
          f"{res['near_ws']['max']} KB ({res['near_ws']['over_pct']} % over WS_B0 {b0:.0f}), coarse "
          f"{WS_R1_IN:.0f}-{WS_R1:.0f} m p90 {res['coarse_ws']['p90']} / max {res['coarse_ws']['max']} KB "
          f"({res['coarse_ws']['over_pct']} % over WS_B1 {b1:.0f})")
    return 1 if strict and over > 0 else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
