#!/usr/bin/env python3
"""Chunk version check (CI job "hygiene"): every chunk_get / chunk_opt call in ps2/proto/src names the version its
writer (tools/*.py through tools/chunkfile.py, tools/*.c through tools/native/chunkw.h) writes.

  chunkver.py [--root DIR]    DIR: a tree with ps2/proto/src and tools/ (default: this repo); exit 0 / 1

The runtime refuses any other version (tools/chunkfile.py docstring), so a version bumped on one side only is a
stale export at run time; this fails it before an ISO is built. One row per mismatch:
  chunkver: WRLD/NBRS reader v2 (ps2/proto/src/world.c:412) writer v1 (tools/export_world.py:88)
Reads whose chunk id or version is not a literal (or a #define of one), and chunks no writer file spells out, are listed
as "unresolved" and do not fail."""
import os
import re
import sys

ID = r"(?:CHUNK_)?ID\(\s*'(.)'\s*,\s*'(.)'\s*,\s*'(.)'\s*,\s*'(.)'\s*\)"
OPEN = re.compile(r"chunk_open(?:_part)?\s*\([^;]*?" + ID)
GET = re.compile(r"\bchunk_(get|opt)\s*\(\s*&?\w+\s*,\s*(?:" + ID + r"|(\w+))\s*,\s*(\w+)\s*,")
# writers: a pack()/write() call's file id, a chunk tuple (b"ID", version, ...), a chunkw_begin / chunkw_start call
W_FILE = re.compile(r"\b(?:pack|write)\s*\([^()]*?b\"([A-Z0-9 ]{4})\"|chunkw_begin\s*\(\s*&?\w+\s*,\s*\"([A-Z0-9 ]{4})\"")
W_TUPLE = re.compile(r"\(\s*b\"([A-Z0-9 ]{4})\"\s*,\s*(\w+)\s*,")
W_C = re.compile(r"chunkw_start\s*\(\s*&?\w+\s*,\s*\"([A-Z0-9 ]{4})\"\s*,\s*(\w+)\s*\)")


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def defines(root):
    d = {}
    src = os.path.join(root, "ps2", "proto", "src")
    for f in sorted(os.listdir(src)):
        if f.endswith(".h"):
            for m in re.finditer(r"^#define\s+(\w+)\s+(\d+)u?\s*$", open(os.path.join(src, f), errors="replace").read(), re.M):
                d[m.group(1)] = int(m.group(2))
    return d


def readers(root, defs):
    """[(file id, chunk id, version or None, 'optional' flag, 'path:line')]; chunk id None when not a literal"""
    out = []
    src = os.path.join(root, "ps2", "proto", "src")
    for f in sorted(os.listdir(src)):
        if not f.endswith(".c") or f == "chunk.c":
            continue
        text = open(os.path.join(src, f), errors="replace").read()
        text = re.sub(r"/\*.*?\*/|//[^\n]*", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
        ev = sorted([(m.start(), "open", "".join(m.groups())) for m in OPEN.finditer(text)] +
                    [(m.start(), "get", m) for m in GET.finditer(text)], key=lambda e: e[0])
        fid = None
        for pos, kind, v in ev:
            if kind == "open":
                fid = v
                continue
            cid = "".join(v.group(i) for i in range(2, 6)) if v.group(2) else None
            ver = v.group(7)
            ver = int(ver) if ver.isdigit() else defs.get(ver)
            out.append((fid, cid, ver, v.group(1) == "opt", f"ps2/proto/src/{f}:{line_of(text, pos)}"))
    return out


def writers(root):
    """{chunk id: [(file id or None, version or None, 'path:line')]}"""
    w = {}
    tools = os.path.join(root, "tools")
    for dp, dn, fn in os.walk(tools):
        dn[:] = [d for d in dn if d not in ("ci", "ext", "__pycache__")]
        for f in sorted(fn):
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, root)
            if f.endswith(".py"):
                text = open(p, errors="replace").read()
                files = [(m.start(), m.group(1) or m.group(2)) for m in W_FILE.finditer(text)]
                for m in W_TUPLE.finditer(text):
                    fid = None
                    for pos, i in files:
                        if pos < m.start():
                            fid = i
                    v = m.group(2)
                    w.setdefault(m.group(1), []).append((fid, int(v) if v.isdigit() else None, f"{rel}:{line_of(text, m.start())}"))
            elif f.endswith(".c"):
                text = open(p, errors="replace").read()
                files = [(m.start(), m.group(1) or m.group(2)) for m in W_FILE.finditer(text)]
                for m in W_C.finditer(text):
                    fid = None
                    for pos, i in files:
                        if pos < m.start():
                            fid = i
                    v = m.group(2)
                    w.setdefault(m.group(1), []).append((fid, int(v) if v.isdigit() else None, f"{rel}:{line_of(text, m.start())}"))
    return w


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    if "--root" in sys.argv:
        root = sys.argv[sys.argv.index("--root") + 1]
    root = os.path.abspath(root)
    defs = defines(root)
    wr = writers(root)
    fails, unresolved, n = [], [], 0
    for fid, cid, ver, opt, loc in readers(root, defs):
        n += 1
        name = f"{fid or '?'}/{cid or '?'}"
        if cid is None or ver is None:
            unresolved.append(f"{name} {loc}: chunk id or version is not a literal")
            continue
        cands = wr.get(cid, [])
        same = [c for c in cands if c[0] == fid] or [c for c in cands if c[0] is None] or cands
        if not same:
            unresolved.append(f"{name} {loc}: no writer spells out chunk {cid}")
            continue
        if any(c[1] is None for c in same):
            unresolved.append(f"{name} {loc}: a writer's version is not a literal ({same[0][2]})")
            continue
        if ver not in {c[1] for c in same}:
            bad = same[0]
            fails.append(f"chunkver: {name} reader v{ver} ({loc}) writer v{bad[1]} ({bad[2]})")
    for x in unresolved:
        print("chunkver: unresolved " + x)
    for x in fails:
        print(x)
    print(f"chunkver: {'FAIL' if fails else 'ok'} ({n} reads, {len(unresolved)} unresolved, {len(fails)} mismatches)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
