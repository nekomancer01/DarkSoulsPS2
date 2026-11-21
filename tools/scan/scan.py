#!/usr/bin/env python3
"""Coverage scan (Polaris 3.4): run every reader of the port over every file of the game once and list the variants
it meets, so whole-game export stops finding surprises area by area. What is scanned and how variants are judged:
tools/scan/kinds.py; what the port runs: tools/scan/support.py.

  scan.py [--game DIR] [--out DIR] [-j N] [--only SUBSTR] [--force]

Writes to --out (default build/scan, git-ignored: the full report names game files):
  cache/<hash>.json   one result per game file, reused while the file (size, mtime) and the scan + reader sources
                      are unchanged; --force ignores it
  scan.json           every (kind, value): status, count, files, an example file
  files.json          per game file its variants (kind, value, status, count)
  summary.md          ids and counts only (no file names, no game strings): the form that may leave the machine
Exit status 1 when a variant is unknown or a reader failed (status unknown / error) and is not listed in
tools/scan/known.tsv (a variant with a TECH_DEBT row), else 0.

Heavy: every FLVER, animation and effect of the game. Run it through the job queue: tools/q -m 10 -r 4 python3 ...
(-j 4 by default: about 3 GB with four workers)."""
import argparse
import collections
import hashlib
import json
import multiprocessing
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)

# the scan's result depends on these sources: a change to one re-scans every file
READERS = ["souls.py", "emevd.py", "esd.py", "ffx.py", "havok_anim.py", "havok_collision.py", "havok_ragdoll.py",
           "lua50.py", "fev.py", "fsb4.py", "material.py", "mapgather.py", "export_emevd.py", "export_effects.py"]
RUNTIME = ["lua0.c", "player.c", "enemy.c", "esd_env_move.c"]


def version():
    h = hashlib.sha1()
    for f in sorted(os.listdir(HERE)):
        if f.endswith(".py") and f != "scan.py":  # this driver does not change a file's result
            h.update(open(os.path.join(HERE, f), "rb").read())
    for f in READERS:
        h.update(open(os.path.join(TOOLS, f), "rb").read())
    for f in RUNTIME:
        h.update(open(os.path.join(ROOT, "ps2", "proto", "src", f), "rb").read())
    return h.hexdigest()[:16]


def game_files(game, only=None):
    out = []
    for d, _, fs in os.walk(game):
        for f in fs:
            rel = os.path.relpath(os.path.join(d, f), game)
            if only and only not in rel:
                continue
            out.append(rel)
    # largest first: the long files start early and the pool finishes together
    return sorted(out, key=lambda r: -os.path.getsize(os.path.join(game, r)))


_CTX = None


def _work(a):
    game, rel, cache, ver, force = a
    global _CTX
    import kinds
    st = os.stat(os.path.join(game, rel))
    key = hashlib.sha1(rel.encode()).hexdigest()[:20]
    cf = os.path.join(cache, key + ".json")
    stamp = [rel, st.st_size, st.st_mtime_ns, ver]
    if not force and os.path.exists(cf):
        try:
            with open(cf) as f:
                c = json.load(f)
            if c["stamp"] == stamp:
                return rel, c["rows"], c["secs"], True
        except (ValueError, KeyError):
            pass
    if _CTX is None:
        _CTX = kinds.Ctx(game)
    t = time.time()
    rows = kinds.scan_file(game, rel, _CTX).rows()
    secs = round(time.time() - t, 2)
    tmp = cf + f".{os.getpid()}"
    with open(tmp, "w") as f:
        json.dump({"stamp": stamp, "rows": rows, "secs": secs}, f)
    os.replace(tmp, cf)
    return rel, rows, secs, False


def load_known():
    """tools/scan/known.tsv: kind <TAB> value (* = every value of the kind) <TAB> TECH_DEBT id <TAB> note. A listed
    unknown does not fail the scan."""
    out = {}
    p = os.path.join(HERE, "known.tsv")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            if line.strip() and not line.startswith("#"):
                k, v, tid, *_ = line.rstrip("\n").split("\t") + [""]
                out[(k, v)] = tid
    return out


def known_id(known, kind, value):
    return known.get((kind, value)) or known.get((kind, "*")) or ""


def summarize(results):
    """{(kind, value): dict(status, count, files, example)}"""
    from kinds import SEVERITY
    agg = {}
    for rel, rows in results.items():
        for kind, value, status, n, ex in rows:
            e = agg.setdefault((kind, value), {"status": status, "count": 0, "files": 0, "example": ex})
            if SEVERITY[status] > SEVERITY[e["status"]]:
                e["status"], e["example"] = status, ex
            e["count"] += n
            e["files"] += 1
    return agg


# kinds whose values are names out of the game (MTD file names, Havok class names are SDK names, param type names are
# paramdef names): summary.md gives their counts per status only
NAMED_KINDS = ("mtd (map)", "mtd missing from Mtd.mtdbnd", "flver vertex layout")


def write_summary(path, agg, known, nfiles, secs):
    by_kind = collections.defaultdict(list)
    for (k, v), e in agg.items():
        by_kind[k].append((v, e))
    L = ["# Coverage scan summary", "", f"{nfiles} game files, {secs:.0f} s of reader time. Status per variant: ok, "
         "info (seen, nothing claimed), unported (the reader knows it, the port does not run it), stub, unknown (the "
         "reader does not know it), error (a reader failure). Counts are occurrences; files = game files they occur in.",
         ""]
    tot = collections.Counter()
    for k in sorted(by_kind):
        for v, e in by_kind[k]:
            tot[e["status"]] += 1
    L.append("Variants per status: " + ", ".join(f"{s} {n}" for s, n in sorted(tot.items())))
    L.append("")
    for k in sorted(by_kind):
        rows = sorted(by_kind[k], key=lambda ve: (-ve[1]["count"], ve[0]))
        L.append(f"## {k}")
        L.append("")
        if k in NAMED_KINDS:
            c = collections.Counter()
            for v, e in rows:
                c[e["status"]] += 1
            L.append("distinct values per status: " + ", ".join(f"{s} {n}" for s, n in sorted(c.items())))
            L.append("")
            continue
        L.append("| value | status | count | files | tech debt |")
        L.append("|---|---|---|---|---|")
        for v, e in rows:
            L.append(f"| {v} | {e['status']} | {e['count']} | {e['files']} | {known_id(known, k, v)} |")
        L.append("")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")


def main():
    import gamepath
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", default=None)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "scan"))
    ap.add_argument("-j", type=int, default=4)
    ap.add_argument("--only", default=None, help="scan only game files whose relative path contains this")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()
    game = a.game or gamepath.game_dir()
    cache = os.path.join(a.out, "cache")
    os.makedirs(cache, exist_ok=True)
    ver = version()
    files = game_files(game, a.only)
    t0 = time.time()
    results, secs, cached = {}, 0.0, 0
    jobs = [(game, rel, cache, ver, a.force) for rel in files]
    with multiprocessing.Pool(max(1, a.j), maxtasksperchild=50) as pool:
        for i, (rel, rows, s, hit) in enumerate(pool.imap_unordered(_work, jobs)):
            results[rel] = rows
            secs += s
            cached += hit
            if (i + 1) % 200 == 0 or i + 1 == len(jobs):
                print(f"scan: {i + 1}/{len(jobs)} files ({cached} cached), {time.time() - t0:.0f} s", flush=True)
    agg = summarize(results)
    known = load_known()
    # written as <name>.tmp, renamed when the scan passes, else <name>.failed (and no file under the real name): a
    # rerun of make after a failed scan scans again instead of finding a fresh files.json
    outs = [os.path.join(a.out, n) for n in ("scan.json", "files.json") + (() if a.only else ("summary.md",))]
    for p in outs:
        for old in (p, p + ".failed"):
            if os.path.exists(old):
                os.remove(old)
    with open(os.path.join(a.out, "scan.json.tmp"), "w") as f:
        json.dump({"version": ver, "files": len(files), "reader_secs": round(secs, 1),
                   "variants": [dict(kind=k, value=v, **e, tech_debt=known_id(known, k, v))
                                for (k, v), e in sorted(agg.items())]}, f, indent=1)
    with open(os.path.join(a.out, "files.json.tmp"), "w") as f:  # per game file (tools/blockreport.py: effect coverage)
        json.dump({rel: [r[:4] for r in rows] for rel, rows in sorted(results.items()) if rows}, f)
    if not a.only:
        write_summary(os.path.join(a.out, "summary.md.tmp"), agg, known, len(files), secs)
    bad = [(k, v, e) for (k, v), e in sorted(agg.items()) if e["status"] in ("unknown", "error") and not known_id(known, k, v)]
    for k, v, e in bad:
        print(f"scan: {e['status']} {k} {v}: {e['count']} x in {e['files']} files, e.g. {e['example']}")
    st = collections.Counter(e["status"] for e in agg.values())
    print(f"scan: {len(files)} files, {len(agg)} variants ({dict(st)}), {len(bad)} unknown / error not in known.tsv, "
          f"{time.time() - t0:.0f} s; {a.out}/scan.json")
    for p in outs:
        os.replace(p + ".tmp", p + ".failed" if bad else p)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
