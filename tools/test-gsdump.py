#!/usr/bin/env python3
"""GS dump regression: replay captured GS dumps headless, diff the images between two test runs, HTML report.

  test-gsdump.py replay <dump.gs.zst> <out.png> [--secs 6] [--slot N]
  test-gsdump.py compare <new run dir> <old run dir> <report.html> [--fail 0.01] [--no-replay]

Dumps come from tools/test-run.py --gsdump (single-frame dumps, F8 hotkey, one per shot marker). A dump holds the GS
packet stream + registers + VRAM of one frame, so its replay does not depend on EE timing or game state: the same dump
always renders the same picture, and two builds' dumps of the same shot differ only where the GS input differs.
Replays run in the flatpak PCSX2 (v2.8.2, the same GS code that captured them; software renderer from the user's ini),
which boots .gs.zst files directly (GSDumpReplayer). pcsx2-gsrunner is the upstream batch tool for this; building it
from PCSX2 source is not done yet (TECH_DEBT test-2).

compare: shots are matched by name (<area>/<shot>.png in both run dirs). For each it diffs the replayed dumps when
both runs have them (else the screenshots): RMSE and the fraction of pixels whose max channel difference > 24.
A shot over --fail changed pixels is a regression. The report shows new / old / amplified difference per shot."""
import argparse, glob, html, json, os, shutil, subprocess, sys, time

TOOLS = os.path.dirname(os.path.abspath(__file__))


_tr = _slot = None


def test_run():
    """tools/test-run.py as a module (slot locks, window capture)"""
    global _tr
    if not _tr:
        import importlib.util
        spec = importlib.util.spec_from_file_location("test_run", os.path.join(TOOLS, "test-run.py"))
        _tr = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_tr)
    return _tr


def replay(dump, out, secs=6.0, slot=None):
    """Boot the dump in a headless PCSX2 (own data dir, muted), screenshot the window after secs, kill it."""
    global _slot
    tr = test_run()
    dump = os.path.realpath(dump)
    slot = _slot = _slot or tr.acquire_slot(slot)  # free slot -> own Xvfb display + data dir, kept for every replay
    DISPLAY = tr.DISPLAY = tr.display_for(slot)  # own Xvfb per slot, as tools/pcsx2-run.sh
    hh = tr.host_home()
    data = f"{hh}/.var/app/net.pcsx2.PCSX2/test/{slot}"
    subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "test-pcsx2-ini.py"),
                    f"{hh}/.var/app/net.pcsx2.PCSX2/config/PCSX2", data, str(slot)], check=True)
    if not subprocess.run(["xdpyinfo"], env=dict(os.environ, DISPLAY=DISPLAY), capture_output=True).returncode == 0:
        subprocess.Popen(["setsid", "Xvfb", DISPLAY, "-screen", "0", "1280x960x24", "-nolisten", "tcp"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, pass_fds=())
        time.sleep(1)
    os.makedirs("/tmp/pcsx2-headless", exist_ok=True)
    idf = f"/tmp/pcsx2-headless/id.gs.{os.getpid()}"
    # muted like every headless run (null sink dsr_mute, tools/pcsx2-run.sh)
    sh = (f"pactl list short sinks | grep -q dsr_mute || pactl load-module module-null-sink sink_name=dsr_mute "
          f"sink_properties=device.description=DSR-test-mute >/dev/null; "
          f"DISPLAY={DISPLAY} XAUTHORITY= WAYLAND_DISPLAY= setsid flatpak run --instance-id-fd=3 "
          f"--env=PULSE_SINK=dsr_mute --socket=x11 --nosocket=wayland --env=QT_QPA_PLATFORM=xcb "
          f"'--filesystem={os.path.dirname(dump)}:ro' net.pcsx2.PCSX2 -datapath '{data}' -batch -- '{dump}' "
          f"> /dev/null 2>&1 < /dev/null 3> '{idf}' & for _ in $(seq 50); do [ -s '{idf}' ] && break; sleep 0.1; done")
    subprocess.run(["distrobox-host-exec", "bash", "-c", sh])
    inst = open(idf).read().strip()
    os.remove(idf)
    open(f"/tmp/pcsx2-headless/{inst}", "w").close()
    try:
        time.sleep(secs)
        if not tr.capture(out):
            raise RuntimeError(f"replay screenshot failed: {dump}")
    finally:
        subprocess.run(["distrobox-host-exec", "flatpak", "kill", inst], capture_output=True)
        os.remove(f"/tmp/pcsx2-headless/{inst}")
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
