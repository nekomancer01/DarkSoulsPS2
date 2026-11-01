"""DSR world graph for level streaming (build-5): which map loads with which, and which map each map connects to through
which collision part. Reads the game's own load lists and every MSB.

  python3 tools/worldgraph.py [--game DIR] [--json out.json]     # text summary to stdout

Inputs (all in map/, Shift-JIS text, CRLF, one `map:/MapStudio/<msb>[\\t#comment]` per line):
  WorldMsbList.worldloadlistlist  the maps of the world; blank lines separate areas (m10 | m11 | m12 ...)
  MapViewList.loadlistlist        map-view slots: line index = (AA - 10) * 10 + BB, blank lines = empty slots
  mAA/mAA.arealoadlist            maps of area AA that are resident together (m10: m10_00, m10_01, m10_02)
MSB facts used per map (souls.msb1): ConnectCollision parts (collision part + target map id, -1 = any block), the
MapOffset event (the map's offset in a shared world frame, if any) and the Collision parts' map_name_id.
Edges are directed (src map -> dst map); a ConnectCollision whose target has -1 fields matches every existing MSB
with that prefix (m10_01_-1_-1 -> m10_01_00_00)."""
import glob, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamepath, souls

LINE = re.compile(r"map:/MapStudio/(m\d\d_\d\d_\d\d_\d\d)\.msb(?:\s*#(.*))?$")


def loadlist(path):
    """Load-list text file -> [(line index, map id or None, comment)]."""
    with open(path, "rb") as f:
        text = f.read().decode("cp932")
    out = []
    for i, line in enumerate(text.split("\r\n")):
        m = LINE.match(line.strip())
        out.append((i, m.group(1), (m.group(2) or "").strip()) if m else (i, None, line.strip()))
    while out and out[-1][1] is None and not out[-1][2]:
        out.pop()
    return out


def groups(entries):
    """Blank-line separated groups of map ids."""
    out, cur = [], []
    for _, mid, _ in entries:
        if mid:
            cur.append(mid)
        elif cur:
            out.append(cur)
            cur = []
    if cur:
        out.append(cur)
    return out


def match_maps(map_id, maps):
    """ConnectCollision map_id (AA, BB, CC, DD; -1 = any) -> existing map ids."""
    return [m for m in maps if all(v < 0 or int(x) == v for v, x in zip(map_id, m[1:].split("_")))]


def world(game):
    mdir = f"{game}/map"
    maps = sorted(os.path.basename(p)[:-4] for p in glob.glob(f"{mdir}/MapStudio/*.msb"))
    wl = loadlist(f"{mdir}/WorldMsbList.worldloadlistlist")
    mv = loadlist(f"{mdir}/MapViewList.loadlistlist")
    area_lists = {os.path.basename(p).split(".")[0]: [m for _, m, _ in loadlist(p) if m]
                  for p in sorted(glob.glob(f"{mdir}/*/*.arealoadlist"))}
    out = {"maps": {}, "world_groups": groups(wl), "world_comments": {m: c for _, m, c in wl if m},
           "map_view_slots": {i: m for i, m, _ in mv if m}, "area_load": area_lists, "edges": []}
    for mid in maps:
        M = souls.msb1(souls.read(f"{mdir}/MapStudio/{mid}.msb"))  # one map at a time (memory watchdog)
        offs = [e for e in M["events"] if e["type"] == 9]
        info = {"parts": len(M["parts"]),
                "collisions": sum(1 for p in M["parts"] if p["type"] == 5),
                "map_offset": [list(e["offset_pos"]) + [e["offset_degree"]] for e in offs],
                "area_load": next((k for k, v in area_lists.items() if mid in v), None),
                "world_group": next((i for i, g in enumerate(out["world_groups"]) if mid in g), None),
                "map_view_slot": next((i for i, m in out["map_view_slots"].items() if m == mid), None)}
        out["maps"][mid] = info
        for p in M["parts"]:
            if p["type"] != 11:
                continue
            dst = match_maps(p["map_id"], maps)
            out["edges"].append({"src": mid, "part": p["name"], "collision": p["collision"],
                                 "map_id": list(p["map_id"]), "dst": dst, "pos": list(p["pos"]),
                                 "disp_groups": list(p["disp_groups"])})
        del M
    return out


def summary(W):
    lines = ["world groups (WorldMsbList): " + " | ".join(" ".join(g) for g in W["world_groups"]),
             "area load lists: " + "; ".join(f"{k}: {' '.join(v)}" for k, v in W["area_load"].items() if not k.startswith("m99"))]
    adj = {}
    for e in W["edges"]:
        for d in e["dst"] or ["(none)"]:
            adj.setdefault(e["src"], {}).setdefault(d, []).append(f"{e['part']}->{e['collision']}")
    for src in W["maps"]:
        info = W["maps"][src]
        off = f" offset {info['map_offset']}" if info["map_offset"] else ""
        lines.append(f"{src} (slot {info['map_view_slot']}, area list {info['area_load']}, {info['collisions']} collisions){off}")
        for dst, parts in sorted(adj.get(src, {}).items()):
            lines.append(f"    -> {dst}: {len(parts)} parts ({', '.join(parts[:4])}{' ...' if len(parts) > 4 else ''})")
    missing = [(e["src"], e["part"], e["map_id"]) for e in W["edges"] if not e["dst"]]
    if missing:
        lines.append(f"connect collisions with no existing target map: {missing}")
    return "\n".join(lines)


if __name__ == "__main__":
    args = sys.argv[1:]
    game = args[args.index("--game") + 1] if "--game" in args else None
    W = world(gamepath.game_dir(game))
    print(summary(W))
    if "--json" in args:
        with open(args[args.index("--json") + 1], "w") as f:
            json.dump(W, f, indent=1, ensure_ascii=False)
