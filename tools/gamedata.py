"""Game values the engine reads at runtime instead of compiling them in, so the PS2 ELF holds no game data and can be
built once (CI) for every player (tools/pages: the in-browser converter only has to produce the packs).

An exporter (export_player.py, export_combat.py, export_progress.py) declares its record with the types below and
writes it with write(); the matching C header (ps2/proto/src/<name>_data.h) is generated from the same declaration
without the game and committed:

    python3 tools/gamedata.py header player > ps2/proto/src/player_data.h     (tools/ci/offline.sh checks they match)

The C side (src/gamedata.c) loads the blob from c0000.pak at boot, checks its layout id and relocates its pointers.

Blob format (little endian, version 1):
    0x00 'GDAT'   u32 layout id (crc32 of the generated header, also in the header as <NAME>_LAYOUT)
    0x08 u32 size of the whole blob   0x0C u32 relocation count   0x10 u32 relocation table offset   0x14..0x1F 0
    0x20 the record (the header's <Name>Data struct), then what its pointers point to, then the relocation table:
         blob offsets of every pointer field; a pointer holds a blob offset until the loader adds the blob's address.
Layout follows the EE GCC ABI: natural alignment, struct alignment = largest member, size padded to it."""
import os, struct, sys, zlib

MAGIC = b"GDAT"
HDR = 0x20


class Scalar:
    def __init__(self, c, fmt):
        self.c, self.fmt = c, fmt
        self.size = self.align = struct.calcsize("<" + fmt)


S8, U8, S16, INT, FLOAT = Scalar("signed char", "b"), Scalar("unsigned char", "B"), Scalar("short", "h"), \
    Scalar("int", "i"), Scalar("float", "f")
CHAR = Scalar("char", "B")


class Ptr:
    """const T *: the value is a list of T values (a str for CHAR: written NUL-terminated)"""
    size = align = 4

    def __init__(self, target):
        self.target = target


class Struct:
    def __init__(self, c, fields, comment=None):
        """fields: (name, type) or (name, type, count) for a fixed array, count may be a (2, n) tuple for [a][b]"""
        self.c, self.comment, self.fields = c, comment, []
        off = align = 0
        for f in fields:
            name, t, n = (f + (1,))[:3] if len(f) == 2 else f
            dims = n if isinstance(n, tuple) else (n,)
            count = 1
            for d in dims:
                count *= d
            off = (off + t.align - 1) & -t.align
            self.fields.append((name, t, len(f) == 3 and dims, count, off))
            off += t.size * count
            align = max(align, t.align)
        self.align = align
        self.size = (off + align - 1) & -align

    def c_decl(self):
        def cty(t):
            return t.c if not isinstance(t, Ptr) else f"const {t.target.c} *"
        L = [f"/* {self.comment} */"] if self.comment else []
        L.append("typedef struct {")
        for name, t, dims, _, _ in self.fields:
            ty = cty(t)
            sep = "" if ty.endswith("*") else " "
            L.append(f"    {ty}{sep}{name}{''.join(f'[{d}]' for d in dims) if dims else ''};")
        L.append(f"}} {self.c};")
        # the EE compiler checks that this layout is the one write() packs (every compile of the header)
        checks = [f"sizeof({self.c}) == {self.size}"] + [f"offsetof({self.c}, {name}) == {o}" for name, _, _, _, o in
                                                           self.fields]
        L.append(f"_Static_assert({' && '.join(checks)}, \"{self.c} layout differs from tools/gamedata.py\");")
        return L


class Writer:
    def __init__(self):
        self.b = bytearray(HDR)
        self.relocs, self.queue = [], []

    def place(self, t, n=1):
        while len(self.b) % t.align:
            self.b.append(0)
        o = len(self.b)
        self.b += bytes(t.size * n)
        return o

    def put(self, t, v, o):
        if isinstance(t, Scalar):
            struct.pack_into("<" + t.fmt, self.b, o, v)
        elif isinstance(t, Ptr):
            self.queue.append((o, t.target, v))
        else:
            missing = {name for name, *_ in t.fields} - set(v)
            extra = set(v) - {name for name, *_ in t.fields}
            assert not missing and not extra, f"{t.c}: missing {sorted(missing)}, unknown {sorted(extra)}"
            for name, ft, dims, count, fo in t.fields:
                val = v[name]
                if dims:
                    flat = _flatten(val, len(dims))
                    assert len(flat) == count, f"{t.c}.{name}: {len(flat)} values for {count}"
                    for i, x in enumerate(flat):
                        self.put(ft, x, o + fo + i * ft.size)
                else:
                    self.put(ft, val, o + fo)

    def finish(self, layout):
        while self.queue:
            slot, t, vals = self.queue.pop(0)
            if isinstance(vals, str):
                vals = list(vals.encode("ascii") + b"\0")
            o = self.place(t, max(1, len(vals)))
            for i, x in enumerate(vals):
                self.put(t, x, o + i * t.size)
            struct.pack_into("<I", self.b, slot, o)
            self.relocs.append(slot)
        while len(self.b) % 4:
            self.b.append(0)
        rel = len(self.b)
        self.b += struct.pack(f"<{len(self.relocs)}I", *sorted(self.relocs))
        struct.pack_into("<4s4I", self.b, 0, MAGIC, layout, len(self.b), len(self.relocs), rel)
        return bytes(self.b)


def _flatten(v, depth):
    return [x for row in v for x in _flatten(row, depth - 1)] if depth > 1 else list(v)


# --- one record per exporter -----------------------------------------------------------------------------------------
def header(name, structs, record, defines=(), doc=""):
    """C header text for a record: <NAME>_LAYOUT, the types, extern const <Record> *<name>_data, plus `defines`
    (lines: compile-time constants that are the exporter's own choices, and macros mapping the old names)"""
    guard = f"{name.upper()}_DATA_H"
    body = [f"/* Generated by tools/gamedata.py header {name} (layout of tools/export_{name}.py). Do not edit. Holds no",
            " * game data: the values load at boot from c0000.pak (src/gamedata.c). */", f"#ifndef {guard}",
            f"#define {guard}", "", "#include <stddef.h>", ""]
    if doc:
        body += [f"/* {doc} */"]
    body += list(defines[0]) + [""] if defines and defines[0] else []
    for s in structs:
        body += s.c_decl() + [""]
    body += [f"extern const {record.c} *{name}_data;  /* NULL until gamedata_init() */"]
    body += list(defines[1]) if len(defines) > 1 else []
    text = "\n".join(body + ["", "#endif", ""])
    lid = zlib.crc32(text.encode())
    return text.replace(f"#define {guard}\n", f"#define {guard}\n#define {name.upper()}_LAYOUT 0x{lid:08x}u\n", 1), lid


def write(out, name, record, value):
    """Write the blob for exporter `name` (its module's HEADER() gives the layout id)"""
    mod = _module(name)
    _, lid = mod.HEADER()
    w = Writer()
    o = w.place(record)
    assert o == HDR
    w.put(record, value, o)
    data = w.finish(lid)
    os.makedirs(os.path.dirname(os.path.abspath(out)) or ".", exist_ok=True)
    with open(out, "wb") as f:
        f.write(data)
    return len(data)


def _module(name):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    return __import__(f"export_{name}")


def main():
    if len(sys.argv) != 3 or sys.argv[1] != "header":
        sys.exit("usage: gamedata.py header <player|combat|progress>")
    sys.stdout.write(_module(sys.argv[2]).HEADER()[0])


if __name__ == "__main__":
    main()
