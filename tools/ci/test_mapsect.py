#!/usr/bin/env python3
"""map.bin and the files split from it cut into sections (tools/ci/offline.sh; Polaris 4 finish).

tools/mapwrap.py sections(kind, payload) cuts a stage-1 payload by the header offsets (map_i.h Header). This checks that
the sections are contiguous, start at 0, join to the payload, use the known ids and keep their order, that the chunk
table holds the header's chunks, and that wrap -> unwrap keeps the payload. Prints per kind which sections it holds.

  test_mapsect.py                 the synthetic DSM5 fixture (tools/ci/fixtures/make_map_dsm5.py) through
                                  tools/mapsplit.py: map, maptab (with a horizon), mapres, maptex, cells
  test_mapsect.py --gen <dir>     also every block's map.bin / maptab.bin / mapres.bin / maptex.bin / cells.bin under <dir>
Exit 0 = ok, 1 = a file failed."""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(HERE, "fixtures"))
import mapwrap  # noqa: E402
import make_map_dsm5  # noqa: E402

FILES = {"map": "map.bin", "maptab": "maptab.bin", "mapres": "mapres.bin", "maptex": "maptex.bin", "cells": "cells.bin"}


def check(kind, payload, what):
    """-> list of error strings"""
    errs = []
    try:
        secs = mapwrap.sections(kind, payload)
    except AssertionError as e:
        return [f"{what}: {e}"]
    pos = 0
    for sid, off, size in secs:
        if sid not in mapwrap.SECTION_IDS:
            errs.append(f"{what}: unknown section id {sid!r}")
        if off != pos:
            errs.append(f"{what}: {sid.decode()} starts at {off}, expected {pos}")
        if size <= 0:
            errs.append(f"{what}: {sid.decode()} empty")
        pos = off + size
    if pos != len(payload):
        errs.append(f"{what}: sections end at {pos}, payload is {len(payload)} bytes")
    if b"".join(payload[o:o + s] for _, o, s in secs) != payload:
        errs.append(f"{what}: joined sections differ from the payload")
    ids = [s[0] for s in secs]
    if len(set(ids)) != len(ids):
        errs.append(f"{what}: a section id twice: {[i.decode() for i in ids]}")
    if kind in ("map", "maptab") and secs:
        d = {s[0]: s for s in secs}
        nch = struct.unpack_from("<I", payload, 4)[0]
        if d.get(b"CHKT", (0, 0, 0))[2] < nch * 64:
            errs.append(f"{what}: chunk table {d.get(b'CHKT', (0, 0, 0))[2]} bytes holds fewer than {nch} chunks")
        if secs[0][:2] != (b"MHDR", 0) or secs[0][2] != mapwrap.HDR:
            errs.append(f"{what}: header section is not the first {mapwrap.HDR} bytes")
    w = mapwrap.wrap(kind, payload)
    if mapwrap.unwrap(w, kind) != payload:
        errs.append(f"{what}: wrap / unwrap changed the payload")
    elif mapwrap.sections(kind, mapwrap.unwrap(w, kind)) != secs:
        errs.append(f"{what}: sections of the unwrapped payload differ")
    return errs


def fixture_files():
    """-> {kind: bytes}"""
    m, hzn = make_map_dsm5.build()
    with tempfile.TemporaryDirectory() as t:
        p = {k: os.path.join(t, v) for k, v in FILES.items()}
        open(p["map"], "wb").write(m)
        open(os.path.join(t, "hzn.bin"), "wb").write(hzn)
        r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "mapsplit.py"), p["map"], p["maptab"], p["mapres"],
                            os.path.join(t, "hzn.bin")], capture_output=True, text=True, env={**os.environ, "TEX_PEERS": ""})
        if r.returncode:
            sys.exit(f"tools/mapsplit.py on the fixture failed:\n{r.stdout}{r.stderr}")
        out = {k: open(p[k], "rb").read() for k in ("map", "maptab", "mapres")}
    out["maptex"] = b"T" * 256
    out["cells"] = b"C" * 4096
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen")
    a = ap.parse_args()
    errs = []
    seen = {}  # kind -> {id: [files, bytes]}
    f = fixture_files()
    for kind, data in f.items():
        e = check(kind, data, f"fixture {kind}")
        errs += e
        if not e and not a.gen:  # the per-kind listing (sections and byte counts); with --gen it is the build's
            for sid, _, size in mapwrap.sections(kind, data):
                c = seen.setdefault(kind, {}).setdefault(sid.decode(), [0, 0])
                c[0] += 1
                c[1] += size
    tab = {s[0]: s for s in mapwrap.sections("maptab", f["maptab"])}
    for need in (b"MHDR", b"CHKT", b"TEXS", b"FOGS", b"CELT", b"HZNB"):
        if need not in tab:
            errs.append(f"fixture maptab: no {need.decode()} section")
    if {s[0] for s in mapwrap.sections("mapres", f["mapres"])} != {b"MHDR", b"STRM", b"TEXS"}:
        errs.append("fixture mapres: expected MHDR STRM TEXS")
    if {s[0] for s in mapwrap.sections("map", f["map"])} != set(mapwrap.SECTION_IDS) - {b"HZNB"}:
        errs.append("fixture map: expected every section but HZNB")
    nfiles = 0
    if a.gen:
        for d in sorted(os.listdir(a.gen)):
            for kind, name in FILES.items():
                p = os.path.join(a.gen, d, name)
                if not os.path.isfile(p):
                    continue
                payload = mapwrap.unwrap(open(p, "rb").read(), kind)
                e = check(kind, payload, f"{d}/{name}")
                errs += e
                nfiles += 1
                if not e:
                    for sid, _, size in mapwrap.sections(kind, payload):
                        c = seen.setdefault(kind, {}).setdefault(sid.decode(), [0, 0])
                        c[0] += 1
                        c[1] += size
    for kind in FILES:
        if kind in seen:
            print(f"{kind:7s}" + "  ".join(f"{i} {c[1]}B x{c[0]}" for i, c in seen[kind].items()))
    for e in errs:
        print("FAIL:", e)
    print(f"test_mapsect: {'FAILED' if errs else 'ok'} (synthetic fixture{f', {nfiles} files under ' + a.gen if a.gen else ''})")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
