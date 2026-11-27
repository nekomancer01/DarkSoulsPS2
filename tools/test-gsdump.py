#!/usr/bin/env python3
"""GS dump regression: replay captured GS dumps headless, diff the images between two test runs, HTML report.

  test-gsdump.py replay <dump.gs.zst> <out.png> [--secs 6] [--slot N]
  test-gsdump.py compare <new run dir> <old run dir> <report.html> [--fail 0.01] [--no-replay]

Dumps come from tools/test-run.py --gsdump (single-frame dumps, control socket "gsdump", one per shot marker). A dump
holds the GS packet stream + registers + VRAM of one frame, so its replay does not depend on EE timing or game state:
the same dump always renders the same picture, and two builds' dumps of the same shot differ only where the GS input
differs. Replays run in pcsx2-dsrtest (tools/pcsx2-headless/, PCSX2 v2.8.2: the same GS code that captured them) with
the software renderer: `pcsx2-dsrtest -renderer sw -dumpdir <dir> -- <dump>` plays the dump once and writes each frame
at internal resolution as a PNG; the last one is the replay picture. --secs and --slot are kept for old command lines
and ignored (no window, no PINE slot).

compare: shots are matched by name (<area>/<shot>.png in both run dirs). For each it diffs the replayed dumps when
both runs have them (else the screenshots): RMSE and the fraction of pixels whose max channel difference > 24.
A shot over --fail changed pixels is a regression. The report shows new / old / amplified difference per shot."""
import argparse, glob, html, json, os, shutil, subprocess, sys, tempfile, time

TOOLS = os.path.dirname(os.path.abspath(__file__))


def runner():
    """the pcsx2-dsrtest binary (tools/pcsx2-run.sh's lookup: PCSX2_HEADLESS, this tree, the main checkout)"""
    sys.path.insert(0, TOOLS)
    import warmstate
    b = warmstate.runner_binary()
    if not b:
        raise RuntimeError("no pcsx2-dsrtest; run tools/pcsx2-headless/build.sh")
    return b


def replay(dump, out, secs=6.0, slot=None):
    """Replay the dump once in pcsx2-dsrtest (software renderer, no window, no sound); its last frame -> out."""
    dump = os.path.realpath(dump)
    tmp = tempfile.mkdtemp(prefix="gsreplay.", dir="/tmp")
    try:
        # -datapath: the replay's logs and caches stay in its scratch dir (not the default data dir in the home,
        # shared by parallel replays)
        r = subprocess.run([runner(), "-surfaceless", "-renderer", "sw", "-datapath", os.path.join(tmp, "data"),
                            "-dumpdir", tmp, "-timeout", "120", "--", dump],
                           capture_output=True, text=True, timeout=180)
        frames = sorted(glob.glob(os.path.join(tmp, "*_frame*.png")))
        if r.returncode != 0 or not frames:
            raise RuntimeError(f"replay failed (exit {r.returncode}, {len(frames)} frames): {dump}\n"
                               + "\n".join((r.stdout + r.stderr).splitlines()[-5:]))
        shutil.move(frames[-1], out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return out


def diff(a_png, b_png, out_png):
    from PIL import Image
    import numpy as np
    a = np.asarray(Image.open(a_png).convert("RGB")).astype(int)
    b = np.asarray(Image.open(b_png).convert("RGB")).astype(int)
    if a.shape != b.shape:
        h, w = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
        a, b = a[:h, :w], b[:h, :w]
    if a.shape[0] > 400:  # PCSX2 window chrome: menu bar (top 24 px) and status bar (FPS / speed, bottom 28 px)
        a, b = a[24:-28], b[24:-28]
    d = np.abs(a - b)
    changed = float((d.max(axis=2) > 24).mean())
    rmse = float(np.sqrt((d.astype(float) ** 2).mean()))
    Image.fromarray(np.clip(d * 4, 0, 255).astype("uint8")).save(out_png)
    return rmse, changed


def shots(run):
    """{'<area>/<shot>': png} of a test run dir (any depth)"""
    out = {}
    for p in glob.glob(os.path.join(run, "**", "*.png"), recursive=True):
        if "/_gs/" in p or p.endswith(("_diff.png", "_replay.png")):
            continue
        out[os.path.relpath(p, run)[:-4]] = p
    return out


def compare(new, old, report, fail=0.01, use_replay=True, slot=None):
    rdir = os.path.join(os.path.dirname(os.path.abspath(report)), "_gs")
    os.makedirs(rdir, exist_ok=True)
    a, b = shots(new), shots(old)
    rows, regress = [], 0
    for key in sorted(set(a) & set(b)):
        src = "screenshot"
        pa, pb = a[key], b[key]
        da, db = pa[:-4] + ".gs.zst", pb[:-4] + ".gs.zst"
        if use_replay and os.path.exists(da) and os.path.exists(db):
            safe = key.replace("/", "__")
            ra, rb = os.path.join(rdir, safe + "_new_replay.png"), os.path.join(rdir, safe + "_old_replay.png")
            cache_b = db[:-7] + "_replay.png"  # replays of the old run are kept next to its dumps
            if not os.path.exists(cache_b):
                replay(db, cache_b, slot=slot)
            shutil.copy(cache_b, rb)
            replay(da, pa[:-4] + "_replay.png", slot=slot)
            shutil.copy(pa[:-4] + "_replay.png", ra)
            pa, pb, src = ra, rb, "gs replay"
        dpng = os.path.join(rdir, key.replace("/", "__") + "_diff.png")
        rmse, changed = diff(pa, pb, dpng)
        bad = changed > fail
        regress += bad
        rows.append(dict(key=key, src=src, rmse=rmse, changed=changed, bad=bad, new=pa, old=pb, diff=dpng))
    only_new, only_old = sorted(set(a) - set(b)), sorted(set(b) - set(a))
    rel = lambda p: os.path.relpath(p, os.path.dirname(os.path.abspath(report)))
    with open(report, "w") as f:
        f.write("<!doctype html><meta charset=utf-8><title>Render regression</title><style>body{font:13px sans-serif;"
                "background:#16181c;color:#ddd;margin:16px}img{width:32%;margin-right:1%}.bad{color:#ff6b6b}"
                "h3{margin:18px 0 4px}td{padding:2px 10px}</style>")
        f.write(f"<h1>Render regression</h1><p>new: {html.escape(new)}<br>old: {html.escape(old)}<br>"
                f"{len(rows)} shots compared, <b class={'bad' if regress else ''}>{regress} over {fail * 100:.1f} % "
                f"changed pixels</b>; only in new: {len(only_new)}, only in old: {len(only_old)}</p><table>")
        for r in sorted(rows, key=lambda r: -r["changed"]):
            f.write(f"<tr class={'bad' if r['bad'] else ''}><td><a href='#{html.escape(r['key'])}'>"
                    f"{html.escape(r['key'])}</a></td><td>{r['changed'] * 100:.2f} %</td><td>RMSE {r['rmse']:.2f}</td>"
                    f"<td>{r['src']}</td></tr>")
        f.write("</table>")
        for r in sorted(rows, key=lambda r: -r["changed"]):
            f.write(f"<h3 id='{html.escape(r['key'])}' class={'bad' if r['bad'] else ''}>{html.escape(r['key'])}: "
                    f"{r['changed'] * 100:.2f} % changed ({r['src']})</h3><div>new / old / diff x4</div>"
                    f"<img src='{rel(r['new'])}'><img src='{rel(r['old'])}'><img src='{rel(r['diff'])}'>")
    res = {"compared": len(rows), "regressions": regress, "only_new": only_new, "only_old": only_old,
           "rows": [{k: v for k, v in r.items() if k in ("key", "src", "rmse", "changed", "bad")} for r in rows]}
    json.dump(res, open(os.path.splitext(report)[0] + ".json", "w"), indent=1)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("replay")
    r.add_argument("dump")
    r.add_argument("out")
    r.add_argument("--secs", type=float, default=6)
    r.add_argument("--slot", type=int)
    c = sub.add_parser("compare")
    c.add_argument("new")
    c.add_argument("old")
    c.add_argument("report")
    c.add_argument("--fail", type=float, default=0.01)
    c.add_argument("--no-replay", action="store_true")
    c.add_argument("--slot", type=int)
    a = ap.parse_args()
    if a.cmd == "replay":
        replay(a.dump, a.out, a.secs, a.slot)
        print(a.out)
    else:
        res = compare(a.new, a.old, a.report, a.fail, not a.no_replay, a.slot)
        print(f"{res['compared']} shots compared, {res['regressions']} regressions -> {a.report}")
        sys.exit(1 if res["regressions"] else 0)


if __name__ == "__main__":
    main()
