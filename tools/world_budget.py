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
room is the arena's build-constant size (world.c WORLD_ARENA_KB) less the structural pool (world.c world_struct_bytes, computed here the
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

NEAR_R = 20.0   # mapstream.c MS_R0_DRAW
PIN_R = 64.0    # the soft tier's outer edge (coarse HARD_R..PIN_R); the runtime pins the coarse level to MS_R1_PIN 40 m
                # (= HARD_R) and wants the ring beyond at a lower class out to MS_R_HZN 48 m; 64 m keeps the soft-tier log
                # comparable with the numbers before the Burg plan's B3
WS_R0, WS_R1_IN, WS_R1 = 30.0, 16.0, 96.0  # export_map.py
ROOM_KB = 10240 * 92 // 100 - 2064  # the budget's 10 MB arena (without world.bin)
def _world_c(name, default):
    """a #define of src/world.c (the build constants the runtime sizes its arena with)"""
    import re
    try:
        src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ps2", "proto", "src", "world.c")).read()
        m = re.search(rf"^#define {name} \(?(\d+)", src, re.M)
        return int(m[1]) if m else default
    except OSError:
        return default


ARENA_KB = _world_c("WORLD_ARENA_KB", 8640)  # world.c: the arena's build-constant size
FILL_PCT = 92        # mapstream.c MS_FILL_PCT
WORLD_SLOTS = 3      # world.c
MISC_EXTRA_KB = 640  # world.c WORLD_MISC_EXTRA
KEPT_HZN = os.environ.get("WORLD_KEPT_HZN", "1") != "0"  # world.c WORLD_KEPT_HZN
STRUCT_WHOLE = int(os.environ.get("WORLD_STRUCT_WHOLE", 2))  # world.c WORLD_STRUCT_WHOLE
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
    if KEPT_HZN:  # world.c WORLD_KEPT_HZN: a lean block keeps the cell table + horizon piece
        tabs -= r128(full - c0)
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
    misc = max((e[3] for e in ent.values()), default=0) + (MISC_EXTRA_KB << 10)
    return dict(structural_kb=best >> 10, misc_kb=misc >> 10, worst_link=dict(block=at[0], tri=at[1], load=at[2], keep=at[3]) if at else None,
                lite_kb={names[i]: dict(whole=e[0] >> 10, lean=(e[0] - e[1]) >> 10) for i, e in ent.items()})
NEAR_CUBE = os.environ.get("NEAR_CUBE", "1") == "1"  # export_map.py NEAR_CUBE (runtime MS_NEAR_CUBE)
# LINKS (Burg plan B3, default on; WORLD_BUDGET_LINKS=0 = every block whose cells are in range, as before): at a spot of
# block i, another block j of the same MSB counts only while the spot's links load it: tools/export_world.py loads j
# from a triangle within LOAD_CELLS cells (xz) of j's cells (tools/area.py BLOCKS), so a block behind a cliff or past
# the next street is not in the arena there. Blocks of other MSBs (Firelink, the Asylum) count by range as before.
LINKS = os.environ.get("WORLD_BUDGET_LINKS", "1") == "1"
# Tiers (Burg plan B3, 2026-11-08): the hard tier is what the horizon cannot stand in for (map.c HZN_R 40): near parts
# within HARD_NEAR_R, the coarse level and kept meshes within HARD_R (coarse of cells without a near level from 0 m);
# --strict fails on any spot of a STRICT_BLOCKS block over the room, or on a block whose solve took the seam allowance at
# more than ALLOW_PCT % of its spots (export_map.py WS_ALLOW_PX, map.bin.budget.json). The soft tier (coarse HARD_R to
# PIN_R, the spot's backdrop and sky) is logged. The Asylum and Firelink are reported, not gated (TECH_DEBT world-10).
HARD_NEAR_R, HARD_R = 20.0, 40.0
ALLOW_PCT = 1.0        # over + allowance spots of all gated blocks together, % of their spots
ALLOW_BLOCK_PCT = 5.0  # ... of one block's own spots: above it the build fails whatever the world total
STRICT_BLOCKS = os.environ.get("WORLD_BUDGET_STRICT", "m10_01_00_00_").split(",")
LOAD_CELLS = 1.0  # export_world.py


def block_rects(name):
    """tools/area.py BLOCKS cells of a block as [(x0, z0, x1, z1)] in metres, and its MSB; ([], None) if none"""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import area
    except ImportError:
        return [], None
    b = area.BLOCKS.get(name)
    if not b or not b.get("cells"):
        return [], b.get("map") if b else None
    c = b["cells"]
    rs = [tuple(c)] if isinstance(c[0], (int, float)) else [tuple(x) for x in c]
    return [(x0 * area.CELL, z0 * area.CELL, (x1 + 1) * area.CELL, (z1 + 1) * area.CELL) for x0, z0, x1, z1 in rs], b["map"]


def rect_dist(rs, P):
    """xz distance of each spot to the nearest rectangle of rs"""
    d = np.full(len(P), np.inf)
    for x0, z0, x1, z1 in rs:
        dx = np.maximum(0, np.maximum(x0 - P[:, 0], P[:, 0] - x1))
        dz = np.maximum(0, np.maximum(z0 - P[:, 2], P[:, 2] - z1))
        d = np.minimum(d, np.hypot(dx, dz))
    return d


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
    tot = {k: np.zeros(len(P)) for k in ("near", "coarse", "kept", "far", "near_ws", "coarse_ws", "hard", "soft")}
    per_block = []
    rects = [block_rects(name) for name, *_ in blocks]
    for k, (name, c, sky, _) in enumerate(blocks):
        # LINKS: the spots whose links load this block (its own, and those of its MSB's blocks within LOAD_CELLS)
        on = np.ones(len(P), bool)
        if LINKS and rects[k][0]:
            near_k = rect_dist(rects[k][0], P) <= LOAD_CELLS * 32.0
            for j in range(len(blocks)):
                if j != k and rects[j][1] == rects[k][1] and rects[j][0]:
                    on[own == j] = near_k[own == j]
        if LINKS:  # another MSB's block (Firelink, the Asylum: ConnectCollision links): only at spots within
            # LOAD_CELLS cells of its own navmesh (its links sit on its walkable floors), never across a height range
            mine = P[own == k]
            for j in range(len(blocks)):
                if j == k or (rects[j][1] is not None and rects[j][1] == rects[k][1]) or not len(mine):
                    continue
                Q = P[own == j]
                dmin = np.full(len(Q), np.inf)
                for a in range(0, len(mine), 512):
                    dmin = np.minimum(dmin, np.sqrt(((Q[:, None, :] - mine[None, a:a + 512, :]) ** 2).sum(2)).min(1))
                on[own == j] = dmin <= LOAD_CELLS * 32.0
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
        n = ((dn < NEAR_R).astype(np.float64) @ near) * on
        co = ((((dn >= NEAR_R) | ~has_near[None]) & (d < PIN_R)).astype(np.float64) @ coarse) * on
        ke = ((d < PIN_R).astype(np.float64) @ kept) * on
        far = np.where(own == k, back.sum() + sky / 1024, 0.0)
        hard = (((dn < HARD_NEAR_R).astype(np.float64) @ near) +
                (((dn >= HARD_NEAR_R) | ~has_near[None]) & (d < HARD_R)).astype(np.float64) @ coarse +
                (d < HARD_R).astype(np.float64) @ kept) * on
        soft = (((d >= HARD_R) & (d < PIN_R)).astype(np.float64) @ coarse) * on + far
        nws = (dn < WS_R0).astype(np.float64) @ near
        cws = ((d >= WS_R1_IN) & (d < WS_R1)).astype(np.float64) @ coarse
        for key, v in (("near", n), ("coarse", co), ("kept", ke), ("far", far), ("near_ws", nws), ("coarse_ws", cws),
                       ("hard", hard), ("soft", soft)):
            tot[key] += v
        per_block.append(dict(block=name, cells=len(c), near_kb=round(float(near.sum())),
                              coarse_kb=round(float(coarse.sum())), kept_kb=round(float(kept.sum())),
                              backdrop_kb=round(float(back.sum() + sky / 1024)), spots=int((own == k).sum())))
    total = tot["near"] + tot["coarse"] + tot["kept"] + tot["far"]
    gated = np.isin(own, [k for k, (name, *_) in enumerate(blocks) if any(name.startswith(s) for s in STRICT_BLOCKS if s)])
    hard_over = (tot["hard"] > room) & gated
    allow, allow_spots = {}, []  # per block and level its own spots' allowance %; the spots (x, y, z, block, level, KB)
    for d in dirs:
        try:
            for r in json.load(open(os.path.join(d, "map.bin.budget.json"))):
                if r.get("allow_pct"):
                    allow[os.path.basename(os.path.normpath(d)) + " " + r["level"]] = r["allow_pct"]
                for s in r.get("allow_spots", []):
                    allow_spots.append(s[:3] + [os.path.basename(os.path.normpath(d)), r["level"], s[3]])
        except (OSError, ValueError, TypeError):
            pass
    w = int(total.argmax())
    over = float((total > room).mean() * 100)
    res = dict(room_kb=room, pools=pools, near_r=NEAR_R, pin_r=PIN_R, blocks=per_block, spots=len(P),
               total=pct(total), near=pct(tot["near"]), coarse=pct(tot["coarse"]), kept=pct(tot["kept"]),
               far=pct(tot["far"]), spots_over_room_pct=round(over, 1),
               hard=dict(gated_spots=int(gated.sum()), over_pct=round(float(hard_over.sum() / max(gated.sum(), 1) * 100), 2),
                         over_spots=[[round(float(v), 1) for v in P[i]] + [blocks[own[i]][0], round(float(tot["hard"][i]))]
                                     for i in np.flatnonzero(hard_over)[:200].tolist()], allowance_pct=allow,
                         **pct(tot["hard"][gated] if gated.any() else tot["hard"])),
               soft=pct(tot["soft"]),
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
    hd = res["hard"]
    # the gate (Burg plan B3): spots over the room or on the seam allowance, all gated blocks together, at most
    # ALLOW_PCT % of their spots; no block above ALLOW_BLOCK_PCT % of its own
    gnames = [b[0] for b in blocks if any(b[0].startswith(s) for s in STRICT_BLOCKS if s)]
    keys = {(round(float(P[i][0]), 1), round(float(P[i][1]), 1), round(float(P[i][2]), 1)) for i in np.flatnonzero(hard_over)}
    keys |= {tuple(s[:3]) for s in allow_spots if s[3] in gnames}
    uniq = len({tuple(np.round(P[i], 1)) for i in np.flatnonzero(gated)}) or 1
    world_pct = round(100.0 * len(keys) / uniq, 2)
    bad_allow = {k: v for k, v in allow.items() if v > ALLOW_BLOCK_PCT and any(k.startswith(s) for s in STRICT_BLOCKS if s)}
    res["hard"].update(world_over_or_allowance_pct=world_pct, allowance_spots=allow_spots[:500])
    json.dump(res, open(out, "w"), indent=1)
    print(f"world budget: hard tier (near < {HARD_NEAR_R:.0f} m, coarse + kept < {HARD_R:.0f} m) of {hd['gated_spots']} "
          f"gated spots ({','.join(STRICT_BLOCKS)}): p50 {hd['p50']} / p90 {hd['p90']} / max {hd['max']} KB, "
          f"{hd['over_pct']} % over {room:.0f} KB; seam allowance {allow or 'none'}; soft tier (coarse {HARD_R:.0f}-"
          f"{PIN_R:.0f} m, backdrop, sky) p90 {res['soft']['p90']} KB")
    seen = set()
    for s in hd["over_spots"]:
        if tuple(s[:3]) not in seen:
            seen.add(tuple(s[:3]))
            print(f"world budget: hard tier over at ({s[0]}, {s[1]}, {s[2]}) (spot of {s[3]}): {s[4]} KB")
    for s in allow_spots:
        print(f"world budget: seam allowance at ({s[0]}, {s[1]}, {s[2]}): {s[3]} {s[4]}, {s[5]} KB")
    print(f"world budget: hard tier gate: {len(keys)} of {uniq} spots over or on the allowance ({world_pct} %, at most "
          f"{ALLOW_PCT} %); per block and level " + (", ".join(f"{k} {v} %" for k, v in allow.items()) or "none")
          + f" (at most {ALLOW_BLOCK_PCT} % each)")
    fail = world_pct > ALLOW_PCT or bool(bad_allow)
    if fail and strict:
        print(f"world budget: FAIL (--strict): hard tier {world_pct} % of the spots over or on the allowance"
              + (f"; over {ALLOW_BLOCK_PCT} % in {bad_allow}" if bad_allow else ""))
    return 1 if strict and fail else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
