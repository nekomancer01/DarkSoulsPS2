#!/usr/bin/env python3
# Usage: build.py [TECH_DEBT.md] [out.html] — render notes/TECH_DEBT.md into the tech-debt web page (template.html).
import json, os, re, sys, datetime

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "notes/TECH_DEBT.md")
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "notes/techdebt-site.html")


def cells(line):
    parts = [p.strip().replace("\\|", "|") for p in re.split(r"(?<!\\)\|", line.strip())[1:-1]]
    return parts


open_rows, resolved, section = [], [], None
for line in open(src, encoding="utf-8"):
    if line.startswith("## "):
        section = line[3:].strip().lower()
        continue
    if not re.match(r"^\| [a-z]+-\d+ ", line):
        continue
    c = cells(line)
    if section == "open":
        if len(c) > 6:  # stray pipes inside a cell: keep ID..Sev, merge the rest into the last column
            c = c[:4] + [c[-2]] + [" | ".join(c[4:-2] + [c[-1]])] if c[-2] in "HML" else c[:5] + [" | ".join(c[5:])]
        c += [""] * (6 - len(c))
        open_rows.append(dict(id=c[0], area=c[1], dsr=c[2], port=c[3], sev=c[4].strip("* "), fix=c[5]))
    elif section == "resolved":
        c += [""] * (3 - len(c))
        resolved.append(dict(id=c[0], date=c[1], fix=" | ".join(c[2:])))

data = dict(open=open_rows, resolved=resolved,
            updated=datetime.datetime.fromtimestamp(os.path.getmtime(src)).strftime("%Y-%m-%d %H:%M"))
tpl = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "template.html"), encoding="utf-8").read()
blob = json.dumps(data, ensure_ascii=False).replace("</", "<\\/")
open(out, "w", encoding="utf-8").write(tpl.replace("/*DATA*/null", blob))
print(f"{out}: {len(open_rows)} open, {len(resolved)} resolved")
