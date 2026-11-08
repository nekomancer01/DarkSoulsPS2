#!/usr/bin/env python3
"""HTML summary of one tools/test-all.sh run: smoke shots per area with their numbers, render-regression and perf results,
and the frame / disc budget (tools/budget.py) of every tools/test-run.py result under the run dir.

  test-report.py <run dir>     reads smoke/smoke.json, regress*.json, perf.json, steps.txt, **/result.json; writes
                               <run dir>/report.html. Exit code 1 when a result is over budget (budget.py's gates), so a
                               folder of ISO runs (world_route, iso_burg_taurus, rd_firelink, smoke) passes or fails too."""
import html, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def load(path):
    try:
        return json.load(open(path))
    except (OSError, ValueError):
        return None


def main():
    run = sys.argv[1]
    smoke = load(os.path.join(run, "smoke", "smoke.json"))
    perf = load(os.path.join(run, "perf.json"))
    regs = {n: load(os.path.join(run, n + ".json")) for n in ("regress-last", "regress-baseline")}
    stamp = load(os.path.join(run, "stamp.json"))
    steps = open(os.path.join(run, "steps.txt")).read() if os.path.exists(os.path.join(run, "steps.txt")) else ""
    e = html.escape
    o = ["<!doctype html><meta charset=utf-8><title>DSR PS2 tests</title><style>body{font:13px sans-serif;"
         "background:#16181c;color:#ddd;margin:16px}a{color:#8cf}.bad{color:#ff6b6b}.warn{color:#fc6}.ok{color:#7d7}"
         "table{border-collapse:collapse}td,th{padding:2px 8px;border-bottom:1px solid #333;text-align:right}"
         "td:first-child,th:first-child{text-align:left}.grid{display:flex;flex-wrap:wrap;gap:8px}"
         ".grid div{width:260px}.grid img{width:260px}pre{background:#0d0e10;padding:8px}</style>",
         f"<h1>DSR PS2 test run {e(os.path.basename(os.path.abspath(run)))}</h1><h2>Steps</h2><pre>{e(steps)}</pre>"]
    if stamp:  # tools/runstamp.py: the tree and ELFs this run tested
        import runstamp
        o.append(f"<p>Tested: {e(runstamp.line(stamp))}</p>")
    if perf and "areas" in perf:
        o.append(f"<h2>Perf (build {e(perf['build'])} vs {e(str(perf.get('previous')))})</h2><table><tr><th>area</th>"
                 "<th>spots</th><th>fps mean</th><th>worst frame ms</th><th>RAM min KB</th><th>VRAM min KB</th></tr>")
        for a, s in perf["areas"].items():
            o.append(f"<tr><td>{e(a)}</td><td>{s['spots']}</td><td>{s['fps_mean']}</td><td>{s['ms_max']:.1f}</td>"
                     f"<td>{s['ram_min_kb']}</td><td>{s['vram_min_kb']}</td></tr>")
        o.append("</table>")
        if perf["regressions"]:
            o.append("<p class=bad>Regressions:</p><ul>" + "".join(
                f"<li>{e(g['area'])}/{e(g['spot'])} {g['field']}: {g['old']:g} &rarr; {g['new']:g}</li>"
                for g in perf["regressions"]) + "</ul>")
        else:
            o.append(f"<p class=ok>No perf regressions ({perf['compared']} spots compared).</p>")
    import budget
    checked = [budget.check_result(p) for p in budget.results([run])]
    if checked:
        nf = sum(1 for r in checked if r["fails"])
        o.append(f"<h2>Budget (budget doc, final 2026-11-08)</h2><p class={'bad' if nf else 'ok'}>{len(checked)} runs, "
                 f"{nf} over budget</p>")
        for r in checked:
            o.append(f"<pre class={'bad' if r['fails'] else 'ok'}>{e(budget.table(r))}</pre>")
    for n, r in regs.items():
        if r:
            cls = "bad" if r["regressions"] else "ok"
            o.append(f"<h2>Render {n[8:]}</h2><p class={cls}>{r['compared']} shots compared, {r['regressions']} "
                     f"regressions &mdash; <a href='{n}.html'>report</a></p>")
    if smoke:
        o.append(f"<h2>Smoke (autotest points)</h2><p class={'bad' if smoke['failures'] else 'ok'}>"
                 f"{smoke['failures']} failures, {smoke['warnings']} warnings</p>")
        for area, ar in smoke["areas"].items():
            o.append(f"<h3>{e(area)}</h3>")
            for x in ar["errors"]:
                o.append(f"<div class=bad>{e(x)}</div>")
            for x in ar["warnings"]:
                o.append(f"<div class=warn>{e(x)}</div>")
            o.append("<div class=grid>")
            for p in ar["points"]:
                cls = "bad" if p["fail"] else ""
                img = os.path.join("smoke", p["png"])
                fps = f"{p['fps']:.0f}" if p.get("fps") is not None else "?"
                o.append(f"<div class={cls}><a href='{e(img)}'><img src='{e(img)}'></a><br>{e(p['name'])}: {fps} fps, "
                         f"max {p.get('ms_max') or 0:.0f} ms, map {p.get('map_max') or 0:.1f} ms, holes "
                         f"{p['holes'] * 100:.2f} %, RAM {int((p.get('ram_min') or 0) / 1024)} K"
                         + (f"<br>{e('; '.join(p['fail']))}" if p["fail"] else "") + "</div>")
            o.append("</div>")
    open(os.path.join(run, "report.html"), "w").write("\n".join(o))
    print(os.path.join(run, "report.html"))
    return 1 if any(r["fails"] for r in checked) else 0


if __name__ == "__main__":
    sys.exit(main())
