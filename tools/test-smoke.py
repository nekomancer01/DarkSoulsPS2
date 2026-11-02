#!/usr/bin/env python3
"""Autotest smoke test: FromSoft's own test points (map/autotest/<area>_pNN.pointlist) for every exported area.

  test-smoke.py <outdir> [--areas m10_02_00_00,...] [--max 30] [--fps-fail 20] [--holes-fail 0.002] [--gsdump] [--jobs 2]

For each area with a build (build/data/<area>.pak + build/proto[_<area>].sym.elf, `make [MAP=..] && make sym`): the
area's pointlist points inside its exported region (tools/area.py) are de-duplicated (< 4 m apart) and thinned to --max,
then one script (src/testscript.c) teleports to each, faces the next point, settles 1.5 s and takes a shot with the
magenta hole clear on ("option holes"). tools/test-run.py runs it headless. A point fails on: crash / hang / error in the
EE log, more than --holes-fail of the frame uncovered, segment fps below --fps-fail. Points without collision floor
under them (pointlist routes run on into neighbouring maps) are skipped by the ELF ("option skipmiss") and counted. Output: <outdir>/<area>/ (shots, frames.csv,
result.json) and <outdir>/smoke.json; exit code 1 on any failure."""
import argparse, glob, json, math, os, subprocess, sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
DEFAULT_MAP = "m10_02_00_00"


def game_dir():
    if os.environ.get("DSR_GAME"):
        return os.environ["DSR_GAME"]
    return subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "gamepath.py"), "--quiet"], capture_output=True,
                          text=True).stdout.strip()


def build_dir():
    return os.environ.get("BUILD", os.path.join(ROOT, "build"))


def area_elf(area):
    b = build_dir()
    return os.path.join(b, "proto.sym.elf" if area == DEFAULT_MAP else f"proto_{area}.sym.elf")


def exported_areas():
    b = build_dir()
    out = []
    for pak in sorted(glob.glob(os.path.join(b, "data", "m??_??_??_??.pak"))):
        area = os.path.basename(pak)[:-4]
        if os.path.exists(area_elf(area)):
            out.append(area)
    return out


def read_points(path):
    pts = []
    for line in open(path, encoding="ascii", errors="replace"):
        parts = line.strip().split(",")
        if len(parts) == 3:
            try:
                pts.append(tuple(float(v) for v in parts))
            except ValueError:
                pass
    return pts


def area_points(game, area, max_points):
    """[(list name, index, (x, y, z), yaw to the next point)] inside the area's region, de-duplicated and thinned."""
    import area as area_mod, souls
    M = souls.msb1(souls.read(f"{game}/map/MapStudio/{area}.msb"))
    keep = area_mod.region(M, area)
    out = []
    for f in sorted(glob.glob(f"{game}/map/autotest/{area}_p*.pointlist")):
        lst = os.path.basename(f)[len(area) + 1:-len(".pointlist")]
        pts = read_points(f)
        for i, p in enumerate(pts):
            nxt = pts[i + 1] if i + 1 < len(pts) else pts[i - 1] if i else p
            yaw = math.atan2(nxt[0] - p[0], nxt[2] - p[2]) if nxt != p else 0.0
            if i + 1 == len(pts) and i:  # last point: look back along the route
                yaw += math.pi
            if keep(*p) and all(math.dist(p, q[2]) > 4 for q in out):
                out.append((lst, i, p, yaw))
    if len(out) > max_points:
        step = len(out) / max_points
        out = [out[int(k * step)] for k in range(max_points)]
    return out


def write_script(path, area, pts):
    with open(path, "w") as f:
        f.write(f"# autotest smoke: {area}, {len(pts)} points from map/autotest (tools/test-smoke.py)\n")
        f.write("option holes\noption nodemo\noption hud\noption skipmiss\nwait 2\n")
        for lst, i, (x, y, z), yaw in pts:
            f.write(f"spawn {x:.3f} {y:.3f} {z:.3f} {yaw:.3f}\ncam {yaw:.3f} 0.25\nwait 1.5\nshot {lst}_{i:03d}\n")
        f.write("end\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("--areas", help="comma list (default: every exported area)")
    ap.add_argument("--max", type=int, default=120, help="candidate points per area (those off the collision are skipped)")
    ap.add_argument("--fps-fail", type=float, default=20)
    ap.add_argument("--holes-fail", type=float, default=0.002)
    ap.add_argument("--gsdump", action="store_true")
    ap.add_argument("--slot", type=int, help="PINE slot (default: test-run.py picks a free one; forces --jobs 1)")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("SMOKE_JOBS", 2)),
                    help="areas run at once, one headless PCSX2 each (~1.5 GB host RAM apiece; default 2)")
    a = ap.parse_args()
    game = game_dir()
    areas = a.areas.split(",") if a.areas else exported_areas()
    summary = {"areas": {}, "failures": 0, "warnings": 0}
    # areas run side by side (--jobs): tools/test-run.py gives each its own PINE slot, X display and data dir
    runs = []
    for area in areas:
        out = os.path.join(a.outdir, area)
        os.makedirs(out, exist_ok=True)
        elf = area_elf(area)
        ar = summary["areas"][area] = {"elf": elf, "points": [], "errors": [], "warnings": []}
        if not os.path.exists(elf):
            ar["errors"].append(f"no build: {elf}")
            summary["failures"] += 1
            continue
        pts = area_points(game, area, a.max)
        script = os.path.join(out, f"autotest_{area}.txt")
        write_script(script, area, pts)
        cmd = [sys.executable, os.path.join(TOOLS, "test-run.py"), elf, script, out, "--holes",
               *(["--slot", str(a.slot)] if a.slot else []), "--name", f"autotest_{area}", "--timeout", str(90 + 6 * len(pts))]
        if a.gsdump:
            cmd.append("--gsdump")
        print(f"[smoke] {area}: {len(pts)} points", flush=True)
        runs.append((area, out, pts, cmd))
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=1 if a.slot else max(1, a.jobs)) as ex:
        list(ex.map(lambda r: subprocess.run(r[3]), runs))
    for area, out, pts, cmd in runs:
        ar = summary["areas"][area]
        try:
            res = json.load(open(os.path.join(out, "result.json")))
        except (OSError, ValueError):
            ar["errors"].append("no result.json (runner crashed)")
            summary["failures"] += 1
            continue
        ar["errors"] += res["errors"]
        misses = [l for l in res["log"] if "floor MISS" in l]
        ar["skipped"] = len(misses)
        if misses and len(misses) * 2 > len(pts):
            ar["warnings"].append(f"{len(misses)} of {len(pts)} autotest points have no collision floor under them")
        if not res["shots"]:
            ar["errors"].append(f"no point of {len(pts)} has collision floor under it")
        for s in res["shots"]:
            st = s.get("stats", {})
            frac = s.get("holes", 0) / max(1, s.get("pixels", 1))
            pt = {"name": s["name"], "png": os.path.relpath(s["png"], a.outdir), "holes": round(frac, 5),
                  "fps": st.get("fps"), "ms_avg": st.get("ms_avg"), "ms_max": st.get("ms_max"), "map_max": st.get("map_max"),
                  "ram_min": st.get("ram_min"), "vram": st.get("vram"), "chain_map": st.get("chain_map"),
                  "chain_chr": st.get("chain_chr"), "tris": st.get("tris"), "pos": st.get("pos"), "fail": []}
            if s.get("gsdump"):
                pt["gsdump"] = os.path.relpath(s["gsdump"], a.outdir)
            if frac > a.holes_fail:
                pt["fail"].append(f"holes {frac * 100:.2f} %")
            if pt["fps"] is not None and pt["fps"] < a.fps_fail:
                pt["fail"].append(f"fps {pt['fps']:.1f} < {a.fps_fail}")
            ar["points"].append(pt)
        nfail = sum(1 for p in ar["points"] if p["fail"]) + len(ar["errors"])
        summary["failures"] += nfail
        summary["warnings"] += len(ar["warnings"])
        print(f"[smoke] {area}: {len(ar['points'])} shots, {nfail} failures, {len(ar['warnings'])} warnings", flush=True)
    json.dump(summary, open(os.path.join(a.outdir, "smoke.json"), "w"), indent=1)
    sys.exit(1 if summary["failures"] else 0)


if __name__ == "__main__":
    main()
