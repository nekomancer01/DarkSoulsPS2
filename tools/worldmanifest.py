"""Whole-game world manifest (Polaris 4.8): every map of Lordran cut into world blocks on the 32 m cells, with each
block's collision and enemy counts, seams, lead and budget row, from the game's own data.

  worldmanifest.py <game dir> <out.json> [--md out.md] [--check] [--cache DIR]
  worldmanifest.py <game dir> --propose <map> [--cut N]   print tools/area.py BLOCKS rows for a map's automatic cut
                                                          (pins of that map ignored; N: collision per block, default COL_CUT)

Sources:
- tools/worldgraph.py: the maps (WorldMsbList / area load lists / MapViewList), their MapOffset events and the
  ConnectCollision parts between maps (DSR loads map B while the player stands on such a part of map A).
- tools/havok_collision.py: every map's h collision, counted per 32 m xz cell (area.CELL) by the cell rule export_world /
  export_collision use: a collision triangle belongs to every block one of its corners lies in. Cached per map in --cache
  (keyed by the MSB's and the hkx binder's size + mtime, and this tool's version).
- the MSB parts: player starts (type 4), bonfires (o0200), enemies (type 2, not c0000).
- tools/area.py BLOCKS / WORLDS: the hand-set cuts (pinned). A map with no pinned block is cut automatically (below);
  `--propose <map>` prints that cut as BLOCKS rows to pin it when the area is added (so a later change of the cutter
  never moves an exported block).
- ps2/proto/src/residency.h (tools/residency.py): the block's RES_BLOCK row, or the default row (flagged).

The cell rules a block must meet (the same ones the hand-set cuts were made by, tools/area.py):
- a block is a union of xz rectangles of whole 32 m cells; the blocks of one MSB tile its cells with no gap or overlap;
- its collision fits collision.bin's u16 triangle indices (export_collision.py stops otherwise). The counts here are
  before the exporters' filters, which only drop triangles (Asylum 65734 here, 65404 exported), so a built block over
  65535 is a warning (the export is the check) and the automatic cut aims at COL_CUT, under it with a margin.
Reported, not cut by: the enemy roster against area.MAX_ENEMIES (enemy_parts keeps the nearest MAX_ENEMIES of a cropped
block to its spawn and drops the rest; cutting for it would make one-cell blocks where enemies crowd).
Automatic cut: the occupied cells' bounding rectangle is split in two along its longer axis (the other one when the
longer has no cut) at the cell line that balances the two halves' collision best, recursively, until every part is
within COL_CUT and at most MAX_SPAN cells across (collision.bin's s16 positions); empty parts are dropped. No cut leaves a part under MIN_CELLS cells wide where the rectangle has room
for two such parts: every spot of a strip that narrow is a seam spot, and export_map.py's spot solve gives a block a
share of each seam spot's room (Blighttown's two-cell entry strip: 28 % of its spots over its share at 36 px).
Seams: two blocks of one MSB whose cells touch (edge or corner), and the ConnectCollision links (the block of map A
holding the part's collision triangles -> the block of map B holding most of them, as export_world.py links them).
Lead: export_world.py's LOAD_CELLS (one cell, 32 m) ahead of a seam, in seconds at SPRINT_MS (ASSUMED dash speed).

--check fails (exit 1) when a block of a world build (area.WORLDS) has no RES_BLOCK row of its own or no spawn, two
pinned blocks of a map overlap, or a world build has more than WORLD_MAX blocks; cells of a built map that no pinned block covers are listed (not a failure:
they are the areas still to add)."""
import glob, json, math, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import area, gamepath, worldgraph

VERSION = 1
CELL = area.CELL
COL_CAP = 65535  # collision.bin u16 triangle indices
COL_CUT = 60000  # the automatic cut's target (a margin under COL_CAP)
MAX_SPAN = 14  # cells across at most: collision.bin's s16 positions at 1/128 m reach +-256 m from its bbox midpoint, and a
               # block's triangles reach past its cells (a corner inside is enough; m15_01 has triangles 130-160 m past their
               # cells): 448 m leaves ~32 m a side, a likely bound, not a proof (export_collision.py asserts; y is not checked)
MIN_CELLS = 3  # no cut leaves a block narrower than this many cells (when the rectangle has room for two)
_HERE = os.path.dirname(os.path.abspath(__file__))


def _const(path, pat, conv):
    """a constant read from another source file, so the two cannot drift apart"""
    with open(os.path.join(_HERE, path)) as f:
        m = re.search(pat, f.read(), re.M)
    if not m:
        raise SystemExit(f"worldmanifest: cannot read {pat!r} from {path}")
    return conv(m.group(1))


WORLD_MAX = _const("../ps2/proto/src/world.h", r"^#define WORLD_MAX (\d+)", int)
ENEMY_CAP = area.MAX_ENEMIES
SPRINT_MS = 6.0  # ASSUMED dash speed (m/s) for lead seconds (TECH_DEBT world-10's 6 m/s sprint)
LOAD_CELLS = _const("export_world.py", r"^LOAD_CELLS, KEEP_CELLS = ([\d.]+),", float)
# DSR's progression order (the main route of a first playthrough: Asylum, Firelink, Burg and Parish, Depths, Darkroot,
# Blighttown, Quelaag's domain / Demon Ruins (one MSB with Lost Izalith), Sen's Fortress, Anor Londo, the Painted
# World, New Londo, the Duke's Archives, the Catacombs, the Tomb of the Giants, the Great Hollow / Ash Lake, the Kiln;
# Oolacile (the DLC, reached from Darkroot Basin) last). ASSUMED order of the optional and late areas.
PROGRESSION = ["m18_01_00_00", "m10_02_00_00", "m10_01_00_00", "m10_00_00_00", "m12_00_00_00", "m14_00_00_00",
               "m14_01_00_00", "m15_00_00_00", "m15_01_00_00", "m11_00_00_00", "m16_00_00_00", "m17_00_00_00",
               "m13_00_00_00", "m13_01_00_00", "m13_02_00_00", "m18_00_00_00", "m12_01_00_00"]
# maps not in Lordran's manifest: the test / debug maps (m99) and the DLC copy of Darkroot (m12_00_00_01: no event
# file of its own, tools/area.py uses m12_00_00_00)
SKIP = {"m12_00_00_01": "DLC copy of m12_00_00_00 (only m12_00_00_00.emevd exists)"}


def _cell(x, z):
    return math.floor(x / CELL), math.floor(z / CELL)


def map_cells(game, mid, cache=None):
    """per map: collision triangles by the set of cells their corners lie in ({"x,z;x,z": n}), triangles, a sample
    floor point per cell (the upward triangle centroid nearest the cell's centre) and the read error, if any"""
    mdir = f"{game}/map"
    files = [f"{mdir}/MapStudio/{mid}.msb", f"{mdir}/{mid}/h{mid[1:]}.hkxbhd", f"{mdir}/{mid}/h{mid[1:]}.hkxbdt"]
    key = [VERSION] + [[os.path.getsize(f), int(os.path.getmtime(f))] if os.path.exists(f) else None for f in files]
    cpath = cache and os.path.join(cache, f"{mid}.json")
    if cpath and os.path.exists(cpath):
        with open(cpath) as f:
            c = json.load(f)
        if c.get("key") == key:
            return c
    import havok_collision
    out = {"key": key, "sets": {}, "tris": 0, "floor": {}, "error": None}
    try:
        subs, missing, _ = havok_collision.map_collision(game, mid, "h")
        out["missing_models"] = missing
    except Exception as e:  # a shape the reader does not know (m13_02: hkpConvexVerticesShape)
        out["error"] = f"{type(e).__name__}: {e}"
        subs = []
    best = {}
    for s in subs:
        for t in s["tris"]:
            cs = sorted({_cell(p[0], p[2]) for p in t})
            k = ";".join(f"{a},{b}" for a, b in cs)
            out["sets"][k] = out["sets"].get(k, 0) + 1
            out["tris"] += 1
            # floor sample: an upward-facing triangle (normal y > 0.7) nearest its cell's centre
            ux, uy, uz = (t[1][i] - t[0][i] for i in range(3))
            vx, vy, vz = (t[2][i] - t[0][i] for i in range(3))
            nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
            ln = math.sqrt(nx * nx + ny * ny + nz * nz)
            if ln <= 0 or abs(ny) / ln < 0.7:
                continue
            cx, cy, cz = (sum(p[i] for p in t) / 3 for i in range(3))
            c = _cell(cx, cz)
            d = (cx - (c[0] + 0.5) * CELL) ** 2 + (cz - (c[1] + 0.5) * CELL) ** 2
            kc = f"{c[0]},{c[1]}"
            if kc not in best or d < best[kc][0]:
                best[kc] = (d, [round(cx, 1), round(cy, 1), round(cz, 1)])
    out["floor"] = {k: v[1] for k, v in best.items()}
    # connect collision parts: their triangles' centroid cells (to place a link in a block)
    out["part_cells"] = {}
    for s in subs:
        if s["part"]:
            pc = out["part_cells"].setdefault(s["part"], {})
            for t in s["tris"]:
                c = _cell(sum(p[0] for p in t) / 3, sum(p[2] for p in t) / 3)
                kc = f"{c[0]},{c[1]}"
                pc[kc] = pc.get(kc, 0) + 1
    havok_collision.forget()
    if cpath and not out["error"]:  # a read error is not cached: a reader fix or a transient failure re-reads
        os.makedirs(cache, exist_ok=True)
        with open(cpath + ".tmp", "w") as f:
            json.dump(out, f)
        os.replace(cpath + ".tmp", cpath)
    return out


def _sets(mc):
    return [([tuple(map(int, c.split(","))) for c in k.split(";")], n) for k, n in mc["sets"].items()]


def _in(rects, c):
    return any(r[0] <= c[0] <= r[2] and r[1] <= c[1] <= r[3] for r in rects)


def col_count(sets, rects):
    """collision triangles of a block (a triangle counts when one of its corners lies in the block's cells)"""
    return sum(n for cs, n in sets if any(_in(rects, c) for c in cs))


def map_parts(game, mid):
    import souls
    M = souls.msb1(souls.read(f"{game}/map/MapStudio/{mid}.msb"))
    starts = [(p["name"], p["pos"]) for p in M["parts"] if p["type"] == 4]
    fires = [(p["name"], p["pos"]) for p in M["parts"] if p["type"] == 1 and p["model"] == area.BONFIRE]
    enemies = [(p["name"], p["model"], p["pos"]) for p in M["parts"] if p["type"] == 2 and p["model"]
               and p["model"] != "c0000"]
    return starts, fires, enemies


def auto_cut(sets, cells):
    """[(x0, z0, x1, z1)] tiling the occupied cells, each within COL_CUT where a cut can do it (see the docstring)"""
    if not cells:
        return []
    xs, zs = [c[0] for c in cells], [c[1] for c in cells]
    out = []

    def load(r):
        return col_count(sets, [r])

    def occupied(r):
        return any(_in([r], c) for c in cells)

    def tight(r):
        cs = [c for c in cells if _in([r], c)]
        return (min(c[0] for c in cs), min(c[1] for c in cs), max(c[0] for c in cs), max(c[1] for c in cs))

    def split(r):
        r = tight(r)
        if (load(r) <= COL_CUT and max(r[2] - r[0], r[3] - r[1]) + 1 <= MAX_SPAN) or (r[0] == r[2] and r[1] == r[3]):
            out.append(r)
            return
        w, d = r[2] - r[0] + 1, r[3] - r[1] + 1
        best = None
        for axis in ((0, 1) if w >= d else (1, 0)):
            lo, hi = (r[0], r[2]) if axis == 0 else (r[1], r[3])
            for cut in range(lo, hi):
                a = (r[0], r[1], cut, r[3]) if axis == 0 else (r[0], r[1], r[2], cut)
                b = (cut + 1, r[1], r[2], r[3]) if axis == 0 else (r[0], cut + 1, r[2], r[3])
                if not occupied(a) or not occupied(b):
                    continue
                if min(cut - lo + 1, hi - cut) < MIN_CELLS and hi - lo + 1 >= 2 * MIN_CELLS:
                    continue  # no strip: a block under MIN_CELLS wide is all seam (export_map.py's spot shares)
                score = max(load(a), load(b))
                if best is None or score < best[0]:
                    best = (score, a, b)
            if best:
                break  # the longer axis first; the other only when it has no cut
        if not best:
            out.append(r)
            return
        split(best[1])
        split(best[2])

    split((min(xs), min(zs), max(xs), max(zs)))
    return sorted(out, key=lambda r: (r[1], r[0]))


def _touch(ra, rb):
    return any(a[0] - 1 <= b[2] and b[0] - 1 <= a[2] and a[1] - 1 <= b[3] and b[1] - 1 <= a[3] for a in ra for b in rb)


def manifest(game, cache=None, log=print):
    import residency
    W = worldgraph.world(game)
    maps = [m for m in W["maps"] if not m.startswith("m99") and m not in SKIP]
    order = [m for m in PROGRESSION if m in maps] + sorted(m for m in maps if m not in PROGRESSION)
    built = {}
    for w, names in area.WORLDS.items():
        for b in names:
            built[b] = w
    blocks, out_maps, errors, warnings = [], {}, [], []
    for rank, mid in enumerate(order):
        mc = map_cells(game, mid, cache)
        sets = _sets(mc)
        cells = sorted({c for cs, _ in sets for c in cs})
        starts, fires, enemies = map_parts(game, mid)
        ecells = [_cell(p[0], p[2]) for _, _, p in enemies]
        pinned = [(b, c) for b, c in area.BLOCKS.items() if c["map"] == mid]
        whole = [b for b in built if b == mid]  # a plain area (no BLOCKS entry): the whole MSB
        if pinned:
            cut = [(b, area.cell_rects(c["cells"]) if c.get("cells") else None, c) for b, c in pinned]
            source = "pinned"
        elif whole:
            cut = [(mid, None, {})]
            source = "pinned"
        else:
            rs = auto_cut(sets, cells)
            cut = [(f"{mid}_{chr(ord('a') + i)}" if len(rs) > 1 else mid, [r], {}) for i, r in enumerate(rs)]
            source = "auto"
        covered = set()
        mblocks = []
        for name, rects, cfg in cut:
            bc = cells if rects is None else [c for c in cells if _in(rects, c)]
            if rects is not None and covered & set(bc):
                errors.append(f"{name}: cells {sorted(covered & set(bc))} also in another block of {mid}")
            covered |= set(bc)
            col = mc["tris"] if rects is None else col_count(sets, rects)
            ens = [e for e, c in zip(enemies, ecells) if rects is None or _in(rects, c)]
            inb = (lambda p: True) if rects is None else (lambda p: _in(rects, _cell(p[0], p[2])))
            bst = [n for n, p in starts if inb(p)]
            bfire = [n for n, p in fires if inb(p)]
            spawn = cfg.get("spawn") or cfg.get("spawn_at")
            if not spawn:
                if bst:
                    spawn = bst[0]
                else:  # a floor point in the block's middle cell
                    fl = [(k, v) for k, v in mc["floor"].items() if rects is None or _in(rects, tuple(map(int, k.split(","))))]
                    if fl:
                        mx = sum(v[0] for _, v in fl) / len(fl)
                        mz = sum(v[2] for _, v in fl) / len(fl)
                        spawn = min(fl, key=lambda kv: (kv[1][0] - mx) ** 2 + (kv[1][2] - mz) ** 2)[1]
            room, near, own = residency.block_budget(name)
            b = {"name": name, "map": mid, "rank": rank, "source": source,
                 "cells": [list(r) for r in rects] if rects is not None else None, "cell_count": len(bc),
                 "collision": col, "enemies": len(ens), "player_starts": bst, "bonfires": bfire, "spawn": spawn,
                 "world": built.get(name), "budget": {"room_kb": room, "near_kb": near, "own_row": own},
                 "issues": []}
            if rects is not None and max(max(r[2] - r[0], r[3] - r[1]) + 1 for r in rects) > MAX_SPAN:
                b["issues"].append(f"wider than {MAX_SPAN} cells (collision.bin s16 range: export_collision may assert)")
            if col > COL_CAP:
                b["issues"].append(f"collision {col} > {COL_CAP}")
            if rects is not None and len(ens) > ENEMY_CAP:  # a whole MSB keeps every placement
                b["issues"].append(f"enemies {len(ens)} > {ENEMY_CAP} (enemy_parts drops the farthest)")
            if b["world"] and not own:
                b["issues"].append("no RES_BLOCK row of its own")
            if not spawn:
                b["issues"].append("no spawn (no player start, no floor)")
            mblocks.append(b)
            if b["world"]:
                for i in b["issues"]:
                    (warnings if i.startswith(("enemies", "collision", "wider")) else errors).append(f"{name}: {i}")
        left = sorted(set(cells) - covered)
        left_col = col_count(sets, [(c[0], c[1], c[0], c[1]) for c in left]) if left else 0
        if left and any(b["world"] for b in mblocks):
            warnings.append(f"{mid}: {len(left)} cells ({left_col} collision triangles) in no block (areas to add)")
        info = W["maps"][mid]
        out_maps[mid] = {"rank": rank, "source": source, "collision": mc["tris"], "cells": len(cells),
                         "bounds": [min(c[0] for c in cells), min(c[1] for c in cells), max(c[0] for c in cells),
                                    max(c[1] for c in cells)] if cells else None,
                         "uncovered_cells": [list(c) for c in left], "uncovered_collision": left_col,
                         "enemies": len(enemies), "player_starts": len(starts), "bonfires": [n for n, _ in fires],
                         "map_offset": info["map_offset"], "area_load": info["area_load"],
                         "read_error": mc["error"], "blocks": [b["name"] for b in mblocks]}
        if mc["error"]:
            warnings.append(f"{mid}: collision unreadable ({mc['error']})")
        blocks += mblocks
        log(f"{mid}: {source}, {len(mblocks)} blocks, {mc['tris']} collision, {len(left)} cells left")
    names = {b["name"] for b in blocks}
    for b, w in sorted(built.items()):
        if b not in names:
            errors.append(f"{b}: in area.WORLDS[{w!r}] but no map or BLOCKS entry makes a block of that name")
    # seams
    byname = {b["name"]: b for b in blocks}
    seams = []
    for i, a in enumerate(blocks):
        for b in blocks[i + 1:]:
            if a["map"] == b["map"] and a["cells"] and b["cells"] and _touch(a["cells"], b["cells"]):
                seams.append({"a": a["name"], "b": b["name"], "kind": "cells"})
    link_seen = set()
    for e in W["edges"]:
        if e["src"] not in out_maps:
            continue
        pc = map_cells(game, e["src"], cache)["part_cells"].get(e["collision"], {})
        if not pc:
            continue
        cs = [(tuple(map(int, k.split(","))), n) for k, n in pc.items()]
        srcs = {b["name"] for b in blocks if b["map"] == e["src"]
                and any(b["cells"] is None or _in(b["cells"], c) for c, _ in cs)}
        for dm in e["dst"]:
            if dm not in out_maps:
                continue
            cand = [b for b in blocks if b["map"] == dm]
            if not cand:
                continue
            dst = max(cand, key=lambda b: sum(n for c, n in cs if b["cells"] is None or _in(b["cells"], c)))
            for s in sorted(srcs):
                k = (s, dst["name"])
                if s != dst["name"] and k not in link_seen:
                    link_seen.add(k)
                    seams.append({"a": s, "b": dst["name"], "kind": "link", "part": e["part"],
                                  "collision": e["collision"]})
    lead_m = LOAD_CELLS * CELL
    for s in seams:
        s["lead_m"] = lead_m
        s["lead_s"] = round(lead_m / SPRINT_MS, 2)
        s["built"] = bool(byname[s["a"]]["world"] and byname[s["b"]]["world"])
    for w, names in area.WORLDS.items():
        if len(names) > WORLD_MAX:
            errors.append(f"world {w}: {len(names)} blocks > WORLD_MAX {WORLD_MAX} (world.h)")
    return {"version": VERSION, "cell": CELL, "caps": {"collision": COL_CAP, "cut": COL_CUT, "enemies": ENEMY_CAP,
                                                    "world_max": WORLD_MAX},
            "progression": order, "skipped": SKIP, "maps": out_maps, "blocks": blocks, "seams": seams,
            "worlds": area.WORLDS, "errors": errors, "warnings": warnings}


def markdown(M):
    L = ["# World manifest", "",
         f"{len(M['maps'])} maps, {len(M['blocks'])} blocks ({sum(1 for b in M['blocks'] if b['world'])} in a world "
         f"build), {len(M['seams'])} seams. Collision cap {M['caps']['collision']} (cut at {M['caps']['cut']}), enemy roster {M['caps']['enemies']} "
         f"per block.", "",
         f"All of Lordran as one world: {len(M['blocks'])} blocks against WORLD_MAX {M['caps']['world_max']} "
         f"(world.h, world.bin's u32 masks).", "",
         "| # | map | source | collision | cells | blocks | built | cells left (collision) | note |",
         "|---|---|---|---:|---:|---:|---:|---|---|"]
    for mid, m in M["maps"].items():
        nb = sum(1 for b in M["blocks"] if b["map"] == mid and b["world"])
        note = m["read_error"] or ""
        L.append(f"| {m['rank'] + 1} | {mid} | {m['source']} | {m['collision']} | {m['cells']} | {len(m['blocks'])} | "
                 f"{nb} | {len(m['uncovered_cells'])} ({m['uncovered_collision']}) | {note} |")
    L += ["", "| block | map | cells | collision | enemies | spawn | budget room / near KB | built | issues |",
          "|---|---|---|---:|---:|---|---|---|---|"]
    for b in M["blocks"]:
        bud = b["budget"]
        cells = "whole" if b["cells"] is None else " ".join("%d,%d..%d,%d" % tuple(r) for r in b["cells"])
        sp = b["spawn"] if isinstance(b["spawn"], str) or b["spawn"] is None else "(%.0f, %.0f, %.0f)" % tuple(b["spawn"])
        L.append(f"| {b['name']} | {b['map']} | {cells} | {b['collision']} | {b['enemies']} | {sp} | "
                 f"{bud['room_kb']} / {bud['near_kb']}{'' if bud['own_row'] else ' (default)'} | "
                 f"{'yes' if b['world'] else ''} | {'; '.join(b['issues'])} |")
    L += ["", "| seam | kind | lead | built |", "|---|---|---|---|"]
    for s in M["seams"]:
        L.append(f"| {s['a']} - {s['b']} | {s['kind']}{' ' + s['part'] if s.get('part') else ''} | "
                 f"{s['lead_m']:.0f} m / {s['lead_s']} s | {'yes' if s['built'] else ''} |")
    if M["errors"] or M["warnings"]:
        L += ["", "## Checks", ""] + [f"- ERROR {e}" for e in M["errors"]] + [f"- {w}" for w in M["warnings"]]
    return "\n".join(L) + "\n"


def propose(M, mid):
    rows = []
    for b in M["blocks"]:
        if b["map"] != mid:
            continue
        sp = b["spawn"]
        if not sp:
            rows.append(f'    # {b["name"]}: no spawn (no player start, no floor): add spawn_at by hand')
            continue
        spawn = f'spawn="{sp}"' if isinstance(sp, str) else f"spawn_at=({sp[0]}, {sp[1]}, {sp[2]})"
        cells = ", ".join("(%d, %d, %d, %d)" % tuple(r) for r in b["cells"]) if b["cells"] else None
        rows.append(f'    "{b["name"]}": dict(map="{mid}", {spawn}' + (f", cells=({cells},)" if cells else "") +
                    f", obj_budget=BLOCK_OBJ_MB),  # collision {b['collision']}, enemies {b['enemies']}")
    return "\n".join(rows)


if __name__ == "__main__":
    a = sys.argv[1:]
    opt = lambda k: a[a.index(k) + 1] if k in a else None
    game = gamepath.game_dir(a[0] if a and not a[0].startswith("--") else None)
    cache = opt("--cache")
    if "--propose" in a:
        # the map's automatic cut even when area.py pins it (to re-cut an area), at --cut N collision triangles
        mid = opt("--propose")
        if opt("--cut"):
            COL_CUT = int(opt("--cut"))
        for b in [b for b, c in area.BLOCKS.items() if c["map"] == mid]:
            del area.BLOCKS[b]
        for w in area.WORLDS.values():
            w[:] = [b for b in w if not b.startswith(mid)]
        M = manifest(game, cache, log=lambda m: print(m, file=sys.stderr))
        print(propose(M, opt("--propose")))
        sys.exit(0)
    pos = [x for i, x in enumerate(a) if not x.startswith("--") and (i == 0 or a[i - 1] not in ("--cache", "--md"))]
    if len(pos) < 2:
        sys.exit("usage: worldmanifest.py <game> <out.json> [--md <out.md>] [--cache <dir>] [--check] | "
                 "<game> --propose <map>")
    out = pos[1]
    M = manifest(game, cache, log=lambda m: print("worldmanifest: " + m, file=sys.stderr))
    with open(out + ".tmp", "w") as f:
        json.dump(M, f, indent=1)
    os.replace(out + ".tmp", out)
    if opt("--md"):
        with open(opt("--md"), "w") as f:
            f.write(markdown(M))
    for w in M["warnings"]:
        print("worldmanifest: " + w, file=sys.stderr)
    for e in M["errors"]:
        print("worldmanifest: ERROR " + e, file=sys.stderr)
    print(f"worldmanifest: {len(M['maps'])} maps, {len(M['blocks'])} blocks, {len(M['seams'])} seams, "
          f"{len(M['errors'])} errors -> {out}", file=sys.stderr)
    if "--check" in a and M["errors"]:
        os.replace(out, out + ".failed")  # no target left behind: the next make re-runs and fails again
        print(f"worldmanifest: --check failed, manifest kept as {out}.failed", file=sys.stderr)
        sys.exit(1)
