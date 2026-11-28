#!/usr/bin/env python3
"""tools/lz4items.py write mode (tools/ci/offline.sh; MAP_LZ4): LZ4 frames for the streamed items of a world block.

Runs on the synthetic block tools/ci/fixtures/lz4items/make_block.py builds in memory (no game data, no .bin files):
  - mapres.lz.bin / cells.lz.bin / lzidx.bin decode back to mapres.bin and every cell table entry's near part
  - frames tile mapres.bin (ascending, <= 64 KB, back to back with exact packed sizes), a compressed frame saves a byte
    and fits one page in place (raw + LZ_SLACK), frames / parts above LZ_GRP_CAP and incompressible ones are stored raw
  - every near part sits 64-aligned on the drive's 32 KB blocks as discalign.place puts it, the lzidx.bin header matches
  - LZ4_STORE=1 stores everything raw; a block without cells.bin writes nothing
  - discalign.check_pack reads CELI of a pack that holds lzidx.bin
  test_lz4items.py [--gen <dir>]: also every block under <dir> (a build's gen/) that has maptab.disc.bin and cells.disc.bin
Exit 0 = ok, 1 = a check failed."""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FIX = os.path.join(HERE, "fixtures", "lz4items")
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, FIX)
import chunkfile  # noqa: E402
import discalign  # noqa: E402
import lz4items as lz  # noqa: E402
import make_block  # noqa: E402
import pack  # noqa: E402

ERR = []


def check(ok, what):
    if not ok:
        ERR.append(what)
        print(f"FAIL: {what}")


def verify(name, tab, res, cells, r):
    """the invariants of one block's write"""
    ix = lz.read_index(r["lzidx"])
    rows, _ = lz.cell_table(tab)
    check(ix["cells"] == len(rows), f"{name}: LZHD cells {ix['cells']} != {len(rows)}")
    check(ix["res_raw"] == len(res) and ix["res_lz"] == len(r["res_lz"]) and ix["cells_lz"] == len(r["cells_lz"]),
          f"{name}: LZHD sizes")
    # decode round trip
    res2, parts = lz.unpack(tab, r["lzidx"], r["res_lz"], r["cells_lz"])
    check(res2 == res, f"{name}: mapres.bin does not round trip")
    bad = [i for i, (row, p) in enumerate(zip(rows, parts)) if p != cells[row[0]:row[0] + row[1]]]
    check(not bad, f"{name}: near parts differ in entries {bad[:5]}")
    # res frames
    f = ix["resf"]
    check(f and f[0][0] == 0 and f[0][1] == 0, f"{name}: frame 0 is not at 0")
    for i, (rel, d, st) in enumerate(f):
        rend = f[i + 1][0] if i + 1 < len(f) else len(res)
        dend = f[i + 1][1] if i + 1 < len(f) else len(r["res_lz"])
        n, pk = rend - rel, dend - d
        check(0 < n <= lz.FRAME, f"{name}: frame {i} raw {n} B")
        if st:
            check(pk == n, f"{name}: raw frame {i}: packed {pk} != raw {n}")
        else:
            check(pk < n and n <= lz.LZ_GRP_CAP and n + lz.LZ_SLACK(pk) <= lz.LZ_PAGE,
                  f"{name}: compressed frame {i} ({n} -> {pk}) does not save a byte / fit its page")
        check(n <= lz.LZ_GRP_CAP or st, f"{name}: frame {i} of {n} B is compressed")
    check(all(a[0] < b[0] and a[1] < b[1] for a, b in zip(f, f[1:])), f"{name}: frame offsets not ascending")
    # near parts
    base = lz.cells_base()
    for i, (row, (d, pk, st)) in enumerate(zip(rows, ix["celi"])):
        if not row[1]:
            check((d, pk) == (0, 0), f"{name}: empty entry {i} has a place")
            continue
        check(d % 64 == 0, f"{name}: part {i} at {d} is not 64-aligned")
        check(discalign.blocks(base + d, pk) <= -(-pk // discalign.ECC),
              f"{name}: part {i} ({pk} B at {d}) touches more 32 KB blocks than it needs")
        check(st == (pk == row[1]) or not st, f"{name}: part {i} raw flag / sizes")
        if not st:
            check(pk < row[1] and row[1] <= lz.LZ_GRP_CAP and row[1] + lz.LZ_SLACK(pk) <= lz.LZ_PAGE,
                  f"{name}: compressed part {i} ({row[1]} -> {pk}) does not fit its page")
        else:
            check(pk == row[1], f"{name}: raw part {i} packed {pk} != raw {row[1]}")
    twin = {}
    for i, row in enumerate(rows):
        if row[1] and (row[0], row[1]) in twin:
            check(ix["celi"][i] == ix["celi"][twin[(row[0], row[1])]], f"{name}: entries naming one blob differ")
        twin.setdefault((row[0], row[1]), i)


def test_fixture():
    tab, res, cells = make_block.build()  # (generated each run: no .bin files in the tree, tools/ci/hygiene.py)
    r = lz.build(tab, res, cells, store=False)
    check(r is not None, "fixture: no write")
    verify("fixture", tab, res, cells, r)
    ix = lz.read_index(r["lzidx"])
    rows, h = lz.cell_table(tab)
    # what the fixture is for: raw and compressed frames / parts, a frame cut at 64 KB, the page-cap band
    check(any(s for _, _, s in ix["resf"]) and any(not s for _, _, s in ix["resf"]), "fixture: res frames all one kind")
    check(any(s for _, _, s in ix["celi"] if _) and any(not s for _, _, s in ix["celi"]), "fixture: parts all one kind")
    band = [n for _, _, n in lz.res_items(tab, h) if lz.LZ_GRP_CAP < n <= lz.FRAME]
    check(band, "fixture: no res item in the page-cap band")
    check(any(n > lz.FRAME for _, _, n in lz.res_items(tab, h)), "fixture: no res item over 64 KB")
    check(r["stats"]["res_lz"] < r["stats"]["res_raw"] and r["stats"]["cells_lz"] < r["stats"]["cells_raw"] + 32768,
          "fixture: nothing saved")
    # LZ4_STORE: everything raw, sizes = raw
    s = lz.build(tab, res, cells, store=True)
    check(all(st for _, _, st in lz.read_index(s["lzidx"])["resf"]) and len(s["res_lz"]) == len(res),
          "store: res frames not all raw")
    check(all(st for _, pk, st in lz.read_index(s["lzidx"])["celi"] if pk), "store: parts not all raw")
    res2, parts = lz.unpack(tab, s["lzidx"], s["res_lz"], s["cells_lz"])
    check(res2 == res, "store: mapres.bin does not round trip")
    # no cells.bin: nothing written
    check(lz.build(tab, res, b"") is None, "no cells.bin: wrote something")
    return tab, res, cells, r


def test_cli(tab, res, cells, r):
    with tempfile.TemporaryDirectory(prefix="lz4items") as d:
        o = [os.path.join(d, n) for n in ("mapres.lz.bin", "cells.lz.bin", "lzidx.bin")]
        src = [os.path.join(d, n) for n in ("maptab.disc.bin", "mapres.bin", "cells.disc.bin")]
        for fn, b in zip(src, (tab, res, cells)):
            with open(fn, "wb") as f:
                f.write(b)
        p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "lz4items.py")] + src + o,
                           capture_output=True, text=True)
        check(p.returncode == 0, f"cli: exit {p.returncode}: {p.stderr[-200:]}")
        check([open(x, "rb").read() for x in o] == [r["res_lz"], r["cells_lz"], r["lzidx"]], "cli: output differs from build()")
        # a block without cells.bin removes stale outputs
        p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "lz4items.py"), src[0], src[1],
                            os.path.join(d, "none.bin")] + o, capture_output=True, text=True)
        check(p.returncode == 0 and not any(os.path.exists(x) for x in o), "cli: stale outputs kept without cells.bin")
        # LZ4_OPT: the optimal parse is no larger and decodes the same
        env = dict(os.environ, LZ4_OPT="1")
        p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "lz4items.py")] + src + o, env=env,
                           capture_output=True, text=True)
        check(p.returncode == 0, f"cli LZ4_OPT: exit {p.returncode}: {p.stderr[-200:]}")
        if p.returncode == 0:
            ro = [open(x, "rb").read() for x in o]
            check(len(ro[0]) <= len(r["res_lz"]), "LZ4_OPT: mapres.lz.bin larger than lz4pack's")
            res2, parts = lz.unpack(tab, ro[2], ro[0], ro[1])
            check(res2 == res, "LZ4_OPT: mapres.bin does not round trip")


def test_check_pack(tab, res, cells, r):
    """discalign.check_pack on a pack with lzidx.bin: clean, then with a part moved over a block boundary"""
    with tempfile.TemporaryDirectory(prefix="lz4items") as d:
        files = {"maptab.bin": tab, "cells.bin": r["cells_lz"], "mapres.bin": r["res_lz"], "lzidx.bin": r["lzidx"]}
        args = [os.path.join(d, "t.pak")]
        for n, b in files.items():
            p = os.path.join(d, n)
            open(p, "wb").write(b)
            kind = {"maptab.bin": "maptab", "cells.bin": "cells"}.get(n)  # (cells.lz.bin as Task 3 wraps it: cellsz)
            args.append(f"{n}=raw:" + (f"chunk:{kind}:{p}" if kind else p))
        pack.main(args, log=lambda *_: None)
        bad = discalign.check_pack(args[0])
        check(not bad, f"check_pack on a clean LZ pack: {bad[:2]}")
        # move the last placed part so that it straddles a 32 KB block it need not touch
        ix = lz.read_index(r["lzidx"])
        celi = bytearray(chunkfile.File(r["lzidx"], "LZIX").get("CELI", 1))
        i = max(range(len(ix["celi"])), key=lambda k: ix["celi"][k][1] if ix["celi"][k][1] < discalign.ECC else 0)
        d0, pk, _ = ix["celi"][i]
        want = (d0 // discalign.ECC + 1) * discalign.ECC - pk // 2
        lz.struct.pack_into("<I", celi, 8 * i, want - lz.cells_base())
        new = bytearray(r["lzidx"])
        o = bytes(r["lzidx"]).find(bytes(chunkfile.File(r["lzidx"], "LZIX").get("CELI", 1)))
        new[o:o + len(celi)] = celi
        p = os.path.join(d, "lzidx.bin")
        open(p, "wb").write(bytes(new))
        args2 = [os.path.join(d, "u.pak")] + [a if not a.startswith("lzidx.bin=") else f"lzidx.bin=raw:{p}" for a in args[1:]]
        pack.main(args2, log=lambda *_: None)
        check(discalign.check_pack(args2[0]), "check_pack: a part over a block boundary passed")


def test_gen(gen):
    n = 0
    for d in sorted(os.listdir(gen)):
        b = os.path.join(gen, d)
        paths = [os.path.join(b, f) for f in ("maptab.disc.bin", "mapres.bin", "cells.disc.bin")]
        if not (os.path.exists(paths[0]) and os.path.exists(paths[2])):
            continue
        tab, res, cells = (open(p, "rb").read() if os.path.exists(p) else b"" for p in paths)
        r = lz.build(tab, res, cells, store=False)
        if r is None:
            continue
        verify(d, tab, res, cells, r)
        n += 1
        print(f"{d}: mapres {len(res) >> 10} -> {len(r['res_lz']) >> 10} KB, cells {r['stats']['cells_raw'] >> 10} -> "
              f"{len(r['cells_lz']) >> 10} KB")
    check(n, f"no block under {gen}")


def main():
    a = sys.argv[1:]
    fx = test_fixture()
    test_cli(*fx)
    test_check_pack(*fx)
    if "--gen" in a:
        test_gen(a[a.index("--gen") + 1])
    print(f"test_lz4items: {len(ERR)} failed")
    sys.exit(1 if ERR else 0)


if __name__ == "__main__":
    main()
