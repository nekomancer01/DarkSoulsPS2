#!/usr/bin/env python3
"""EE heap drift from a PCSX2 EE log (TECH_DEBT world-5): GC sawtooth or a real leak?

Usage: heapdrift.py <emulog.txt> [--png out.png] [--csv out.csv] [--skip S] [--max-slope KB_PER_MIN]

Reads two kinds of lines the ELF prints:
  lua0: gc <before>K -> <after>K (+<since>K since) in <us> us, pool <used>K used <free>K free, malloc <m>K, heap free <f>K in-use <u>K
      one per Lua collection (lua0.c lua0_gc). Right after a sweep the VM holds only live objects, so <after> (Lua
      live bytes) and <u> (malloc in-use bytes of the whole EE heap) are the samples without the sawtooth.
  enemy: block <n> off: ... RAM free <k>K
      world.c turning a block off: the same moment of every soak round (enemy.c enemy_block_remove).
Prints the least-squares slope of each series in KB per minute over the samples after --skip seconds (boot and the
first round settle), and exits 1 when free RAM at block off falls faster than --max-slope (default 1 KB/min: 30 KB
over a 30-minute soak; the gc samples are taken wherever the player is, so their in-use bytes follow which enemy
types are loaded there, and they are the fallback only without block-off lines). --png draws the series (Pillow).
"""
import argparse
import re
import sys

GC = re.compile(r"^\[\s*([\d.]+)\] lua0: gc (\d+)K -> (\d+)K \(\+(\d+)K since\)(?: in \d+ us)?, pool (\d+)K used (\d+)K free, "
                r"malloc (\d+)K, heap free (\d+)K in-use (\d+)K")
OFF = re.compile(r"^\[\s*([\d.]+)\] enemy: block (\d+) off: .*RAM free (\d+)K")


def slope(pts):
    """least-squares KB per minute of [(t seconds, KB)]"""
    n = len(pts)
    if n < 2:
        return 0.0
    mt = sum(t for t, _ in pts) / n
    mv = sum(v for _, v in pts) / n
    d = sum((t - mt) ** 2 for t, _ in pts)
    return 60 * sum((t - mt) * (v - mv) for t, v in pts) / d if d else 0.0


def plot(path, series):
    from PIL import Image, ImageDraw
    W, H, M = 1000, 640, 50
    img = Image.new("RGB", (W, H * len(series)), "white")
    d = ImageDraw.Draw(img)
    t1 = max((p[-1][0] for _, p, _ in series if p), default=1) or 1
    for k, (name, pts, col) in enumerate(series):
        y0 = k * H
        if not pts:
            continue
        lo, hi = min(v for _, v in pts), max(v for _, v in pts)
        if hi - lo < 64:
            lo, hi = (lo + hi) / 2 - 32, (lo + hi) / 2 + 32
        X = lambda t: M + (W - 2 * M) * t / t1
        Y = lambda v: y0 + H - M - (H - 2 * M) * (v - lo) / (hi - lo)
        d.rectangle([M, y0 + M, W - M, y0 + H - M], outline="gray")
        d.line([(X(t), Y(v)) for t, v in pts], fill=col, width=2) if len(pts) > 1 else None
        d.text((M, y0 + 10), f"{name}: {len(pts)} samples, {slope(pts):+.2f} KB/min, range {lo:.0f}..{hi:.0f} KB",
               fill="black")
        d.text((4, y0 + M), f"{hi:.0f}", fill="black")
        d.text((4, y0 + H - M - 10), f"{lo:.0f}", fill="black")
        d.text((W - M - 40, y0 + H - M + 5), f"{t1 / 60:.0f} min", fill="black")
    img.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--png")
    ap.add_argument("--csv")
    ap.add_argument("--skip", type=float, default=60)
    ap.add_argument("--max-slope", type=float, default=1.0)
    a = ap.parse_args()
    gc, off = [], []
    for line in open(a.log, errors="replace"):
        m = GC.match(line)
        if m:
            gc.append(tuple(float(x) if i == 0 else int(x) for i, x in enumerate(m.groups())))
            continue
        m = OFF.match(line)
        if m:
            off.append((float(m[1]), int(m[2]), int(m[3])))
    if a.csv:
        with open(a.csv, "w") as f:
            f.write("t,lua_before_k,lua_after_k,since_k,pool_used_k,pool_free_k,lua_malloc_k,heap_free_k,in_use_k\n")
            for r in gc:
                f.write(",".join(str(x) for x in r) + "\n")
    g = [r for r in gc if r[0] >= a.skip]
    o = [r for r in off if r[0] >= a.skip]
    lua = [(r[0], r[2]) for r in g]
    use = [(r[0], r[8]) for r in g]
    lmal = [(r[0], r[6]) for r in g]
    offk = [(r[0], r[2]) for r in o]
    span = (g[-1][0] - g[0][0]) / 60 if len(g) > 1 else 0
    print(f"{len(gc)} gc lines ({len(g)} after {a.skip:.0f} s, {span:.1f} min), {len(off)} block-off lines")
    if gc:
        print(f"gc every {span * 60 / max(len(g) - 1, 1):.1f} s; Lua live {min(v for _, v in lua or [(0, 0)])}.."
              f"{max(v for _, v in lua or [(0, 0)])} K, pool {gc[-1][4]} K used / {gc[-1][5]} K free, "
              f"Lua malloc {gc[-1][6]} K")
    for name, pts in (("heap in-use after gc", use), ("Lua live after gc", lua), ("Lua malloc after gc", lmal),
                      ("RAM free at block off", offk)):
        if pts:
            print(f"{name:24s} first {pts[0][1]:6d} K  last {pts[-1][1]:6d} K  min {min(v for _, v in pts):6d}  "
                  f"max {max(v for _, v in pts):6d}  slope {slope(pts):+7.2f} KB/min")
    if a.png:
        plot(a.png, [("heap in-use after Lua gc (K)", use, "red"), ("Lua live after gc (K)", lua, "blue"),
                     ("RAM free at block off (K)", offk, "green")])
    if len(offk) >= 2:
        s = -slope(offk)
        what = "free RAM at block off falls"
    else:
        s = slope(use)
        what = "heap in-use after gc grows"
    if s > a.max_slope:
        print(f"DRIFT: {what} {s:.2f} KB/min (> {a.max_slope})")
        return 1
    print(f"no drift: {what} {s:+.2f} KB/min (limit {a.max_slope})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
