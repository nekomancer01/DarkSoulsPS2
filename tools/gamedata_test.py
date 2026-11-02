"""Offline test of tools/gamedata.py (tools/ci/offline.sh; no game): pack a made-up record of each exporter's layout and
read it back the way src/gamedata.c does (header, relocation, pointers into the blob)."""
import os, struct, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedata
from gamedata import Ptr, Scalar, Struct


def fake(t, k):
    """a value of type t; k makes values differ"""
    if isinstance(t, Scalar):
        return {"f": k * 0.5, "b": -(k % 100), "B": k % 200, "h": -k, "i": k * 1000}[t.fmt] if t.c != "char" else 65
    if isinstance(t, Ptr):
        return "NAME%d" % k if t.target is gamedata.CHAR else [fake(t.target, k + i) for i in range(3)]
    v = {}
    for i, (name, ft, dims, count, _) in enumerate(t.fields):
        if dims:
            flat = [fake(ft, k + i + j) for j in range(count)]
            v[name] = flat if len(dims) == 1 else [flat[r * dims[1]:(r + 1) * dims[1]] for r in range(dims[0])]
        else:
            v[name] = fake(ft, k + i)
    return v


def read(t, b, o, base):
    """value of type t at blob offset o, following pointers (stored as blob offsets after relocation by base)"""
    if isinstance(t, Scalar):
        return struct.unpack_from("<" + t.fmt, b, o)[0]
    if isinstance(t, Ptr):
        return struct.unpack_from("<I", b, o)[0] - base
    return {name: [read(ft, b, o + fo + i * ft.size, base) for i in range(count)] if dims else
            read(ft, b, o + fo, base) for name, ft, dims, count, fo in t.fields}


def main():
    base = 0x00400000
    for name in ("player", "combat", "progress"):
        mod = gamedata._module(name)
        _, lid = mod.HEADER()
        value = fake(mod.RECORD, 1)
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, name + ".gd")
            gamedata.write(out, name, mod.RECORD, value)
            b = bytearray(open(out, "rb").read())
        magic, layout, size, nrel, rel = struct.unpack_from("<4s4I", b, 0)
        assert magic == b"GDAT" and layout == lid and size == len(b), (name, magic, layout, size)
        for i in range(nrel):  # what gamedata.c does
            o = struct.unpack_from("<I", b, rel + 4 * i)[0]
            assert o % 4 == 0 and o < rel
            struct.pack_into("<I", b, o, struct.unpack_from("<I", b, o)[0] + base)
        got = read(mod.RECORD, b, gamedata.HDR, base)
        for fname, ft, dims, count, _ in mod.RECORD.fields:  # scalars and arrays come back as written
            if isinstance(ft, Scalar) and not isinstance(value[fname], str):
                want = gamedata._flatten(value[fname], len(dims)) if dims else value[fname]
                assert got[fname] == want, (name, fname, got[fname], want)
            elif isinstance(ft, Ptr):
                o = got[fname]
                assert gamedata.HDR + mod.RECORD.size <= o < rel, (name, fname, o)
        print(f"gamedata {name}: {len(b)} B, {nrel} pointers, layout {lid:08x}: ok")


if __name__ == "__main__":
    main()
