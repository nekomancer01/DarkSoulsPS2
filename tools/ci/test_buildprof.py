#!/usr/bin/env python3
"""tools/buildprof.py and tools/stepcache.py's timing rows without game data (tools/ci/offline.sh step 8).

  1 a synthetic stepcache.tsv (two blocks, a game-wide step, a character): report totals, the critical chain, replay
    with the world phase barriers on 1 and 2 slots, the replay with a block order file, and order
  2 sha: two copies of a build dir are equal; one byte of one pack makes them differ; a volatile file alone does not,
    except with --strict; a `hash` file compares like the dir it was made from
  3 stepcache.py's log_row: one row with every column; STEPCACHE_LOG=0 writes none
Exit code = number of failed checks."""
import io
import os
import shutil
import subprocess
import sys
import tempfile
from contextlib import redirect_stdout

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import buildprof  # noqa: E402

FAILS = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        FAILS.append(what)


def run(*argv):
    out = io.StringIO()
    with redirect_stdout(out):
        rc = buildprof.main(list(argv))
    return rc, out.getvalue()


T = 1000.0
# (start, end, result, name, outputs, prereqs); blocks a and b, b's steps longer
STEPS = [
    (0, 10, "miss", "objects", "<B>/gen/a/objects.bin", ""),
    (0, 30, "miss", "objects", "<B>/gen/b/objects.bin", ""),
    (10, 40, "miss", "map", "<B>/gen/a/map.bin", "<B>/gen/a/objects.bin;<B>/stamp/export_map.py.sum"),
    (30, 90, "miss", "map", "<B>/gen/b/map.bin", "<B>/gen/b/objects.bin"),
    (0, 5, "hit", "menu", "<B>/data/menu.bin", ""),
    (5, 25, "miss", "c1000", "<B>/gen/a/c1000/chr.bin", ""),
    (90, 100, "miss", "horizon", "<B>/gen/a/hzn.bin", "<B>/gen/a/map.bin"),
    (90, 130, "miss", "horizon", "<B>/gen/b/hzn.bin", "<B>/gen/b/map.bin"),
    (130, 150, "miss", "visibility", "<B>/gen/a/pvs.bin", "<B>/gen/a/map.bin;<B>/gen/a/hzn.bin"),
    (130, 170, "miss", "far panorama", "<B>/gen/b/far.bin", "<B>/gen/b/hzn.bin;<B>/gen/a/hzn.bin"),
]


def write_tsv(build, steps, t=T):
    with open(os.path.join(build, "stepcache.tsv"), "a") as f:
        for s0, s1, res, name, outs, pre in steps:
            f.write("\t".join([f"{t + s0:.3f}", f"{t + s1:.3f}", res, name, "0123456789ab", f"{s1 - s0:.3f}",
                               f"{(s1 - s0) * 2:.3f}", outs, pre]) + "\n")


def profile_checks(tmp):
    b = os.path.join(tmp, "prof")
    os.makedirs(b)
    write_tsv(b, [(0, 1, "miss", "old run", "<B>/data/x", "")], t=0.0)  # an earlier build run: left out
    write_tsv(b, STEPS)
    rows = buildprof.load(b)
    check(len(rows) == len(STEPS), f"load keeps the latest run only ({len(rows)} rows)")
    rc, out = run("report", b)
    check(rc == 0 and "10 steps, 1 hits, span 170.0 s" in out, "report totals")
    check(any(ln.split()[:3] == ["map", "2", "0"] and "90.0" in ln for ln in out.splitlines()), "report map row")
    check(any(ln.startswith("{CHR}") for ln in out.splitlines()), "characters fold into {CHR}")
    rc, out = run("critical", b)
    chain = [ln.split()[-2] + " " + ln.split()[-1] for ln in out.splitlines()[2:]]
    check(rc == 0 and chain == ["objects [b]", "map [b]", "horizon [b]", "panorama [b]"],
          f"critical chain objects -> map -> horizon -> far panorama of b ({chain})")
    rows = buildprof.load(b)
    t1, ph = buildprof.replay(rows, 1)
    check(abs(t1 - sum(s[1] - s[0] for s in STEPS)) < 1e-6, f"replay -j 1 = the wall sum ({t1})")
    t2, ph = buildprof.replay(rows, 2)
    # 2 slots, a before b: exports and characters 0-90 (objects a/b, map a, c1000, menu, map b runs 30-90),
    # horizons wait for every export: 90-130, bakes 130-170
    check(abs(t2 - 170) < 1e-6, f"replay -j 2 = 170 s ({t2})")
    check(ph[1][0] >= ph[0][1] and ph[2][0] >= ph[1][1], f"phase barriers hold ({ph})")
    t9, _ = buildprof.replay(rows, 2, scale={"map": 0.5})
    check(t9 < t2, f"--scale map=0.5 shortens the replay ({t9} < {t2})")
    rc, out = run("order", b)
    check(out.split() == ["b", "a"], f"order: the block with more phase work first ({out.split()})")
    of = os.path.join(tmp, "order.txt")
    open(of, "w").write("m:b\nm:a\n")
    rc, out = run("replay", b, "-j", "2", "--order", of)
    check(rc == 0 and out.startswith("replay -j 2: "), "replay with an order file")
    check(subprocess.run([sys.executable, os.path.join(ROOT, "tools", "buildprof.py"), "report",
                          os.path.join(tmp, "none")], capture_output=True).returncode == 2, "report without a TSV exits 2")


def sha_checks(tmp):
    a = os.path.join(tmp, "A")
    for rel, data in [("data/menu.pak", b"menu"), ("gen/a/map.bin", b"map"), ("gen/a/far.bin", b"far"),
                      ("data/snd.log", b"log")]:
        os.makedirs(os.path.dirname(os.path.join(a, rel)), exist_ok=True)
        open(os.path.join(a, rel), "wb").write(data)
    b = os.path.join(tmp, "B")
    shutil.copytree(a, b)
    check(run("sha", a, b)[0] == 0, "sha: copies are equal")
    open(os.path.join(b, "gen/a/far.bin"), "wb").write(b"fat")
    open(os.path.join(b, "data/snd.log"), "wb").write(b"other")
    check(run("sha", a, b)[0] == 0, "sha: volatile files alone do not differ")
    check(run("sha", a, b, "--strict")[0] == 1, "sha --strict: they do")
    h = os.path.join(tmp, "A.sha")
    rc, out = run("hash", a)
    open(h, "w").write(out)
    check(run("sha", h, b)[0] == 0, "sha: a hash file compares like its dir")
    open(os.path.join(b, "data/menu.pak"), "wb").write(b"menX")
    check(run("sha", a, b)[0] == 1 and run("sha", h, b)[0] == 1, "sha: one changed byte of a pack differs")


def stepcache_checks(tmp):
    import stepcache
    b = os.path.join(tmp, "sc")
    os.makedirs(b)
    out = os.path.join(b, "gen", "a", "map.bin")
    stepcache.log_row(b, 100.0, "miss", "map", "0123456789abcdef", 3.5, [out], [out + ".in"],
                      lambda s: s.replace(os.path.abspath(b), "<B>"))
    rows = open(os.path.join(b, "stepcache.tsv")).read().splitlines()
    f = rows[0].split("\t") if rows else []
    check(len(rows) == 1 and len(f) == 9 and f[2:5] == ["miss", "map", "0123456789ab"] and f[6] == "3.500"
          and f[7] == "<B>/gen/a/map.bin" and f[8] == "<B>/gen/a/map.bin.in", f"log_row writes one full row ({f})")
    os.environ["STEPCACHE_LOG"] = "0"
    try:
        stepcache.log_row(b, 100.0, "miss", "map", "", 0.0, [out], [])
    finally:
        del os.environ["STEPCACHE_LOG"]
    check(len(open(os.path.join(b, "stepcache.tsv")).read().splitlines()) == 1, "STEPCACHE_LOG=0 writes no row")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        profile_checks(tmp)
        sha_checks(tmp)
        stepcache_checks(tmp)
    print(f"test_buildprof: {len(FAILS)} failed")
    return len(FAILS)


if __name__ == "__main__":
    sys.exit(main())
