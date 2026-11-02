"""Bootable PS2 disc image in pure Python: the same disc as tools/ps2-iso.sh (SYSTEM.CNF, the ELF under its boot id,
every pack in data/ under DATA\\, ISO9660 level 1 names, packs in tools/ps2-iso.sh's sector order) without xorriso,
for the in-browser converter (tools/pages, Pyodide) and anywhere xorriso is missing.

    python3 tools/iso9660.py <proto.elf> <data dir> <out.iso> [boot id=DSRP_000.01]

Layout: sectors 0-15 zero, 16 primary volume descriptor, 17 terminator, 18 / 19 the L / M path tables, then one
sector per directory (the BIOS CDVD driver reads only a directory's first sector: checked), then the files in order.
No Rock Ridge, no Joliet (tools/ps2-iso.sh says why). Pack names follow pack.c pack_path(): '_' dropped, upper case,
the last 8 characters ("data/m10_01_00_00/c1000.pak" -> DATA/10010000/C1000.PAK).
"""
import os, struct, sys, time

SECTOR = 2048


def iso_name(component):
    """one path component -> ISO9660 level 1 name (tools/ps2-iso.sh iso_name, pack.c pack_path)"""
    b, e = (component.split(".", 1) + [""])[:2] if "." in component else (component, "")
    b = b.replace("_", "").upper()
    b = b[-8:] if len(b) > 8 else b
    return b + ("." + e.upper()[:3] if e else "")


def pack_order(data):
    """the packs under data/ (relative paths) in tools/ps2-iso.sh's order: boot packs, then the world blocks breadth
    first from the boot block (each block's pack followed by its character packs), then the other areas"""
    paks = sorted(os.path.relpath(os.path.join(r, f), data).replace(os.sep, "/")
                  for r, _, fs in os.walk(data) for f in fs if f.endswith(".pak"))
    top = [p for p in paks if "/" not in p]
    order = [p for p in ("menu.pak", "world.pak", "common.pak", "c0000.pak") if p in top]

    def area(name, chrs):
        order.extend(p for p in [name + ".pak"] + chrs if p in paks and p not in order)
    blocks = _world_blocks(data)
    boot = next((i for i, b in enumerate(blocks) if b[1]), None)
    if boot is not None:
        seen, queue = {boot}, [boot]
        while queue:
            i = queue.pop(0)
            name = blocks[i][0]
            area(name, [p for p in top if p[0] == "c"] if i == boot else [p for p in paks if p.startswith(name + "/")])
            for k in range(len(blocks)):
                if blocks[i][2] >> k & 1 and k not in seen:
                    seen.add(k)
                    queue.append(k)
    for p in paks:
        if "/" not in p and p not in order:
            area(p[:-4], [q for q in paks if q.startswith(p[:-4] + "/")] if p[0] == "m" else [])
    return order + [p for p in paks if p not in order]


def _world_blocks(data):
    """data/world.pak (tools/export_world.py world.bin): [(pack name, boot flag, neighbour mask)]"""
    try:
        d = open(os.path.join(data, "world.pak"), "rb").read()
    except OSError:
        return []
    off, raw, _, blocks, bs = struct.unpack_from("<5I", d, 16 + 32)
    cs, p, w = struct.unpack_from(f"<{blocks}I", d, off), off + 4 * blocks, b""
    for c in cs:
        k = c & 0x7FFFFFFF
        w += d[p:p + k] if c >> 31 else _lz4(d[p:p + k], min(bs, raw - len(w)))
        p += k
    assert w[:4] == b"DSW1", "world.pak: no world.bin"
    fmt = "<24s16s4f2I" if struct.unpack_from("<I", w, 4)[0] == 2 else "<16s16s6f2I"
    rows = [struct.unpack_from(fmt, w, 16 + 64 * i) for i in range(struct.unpack_from("<I", w, 8)[0])]
    return [(r[0].rstrip(b"\0").decode(), r[-2] & 1, r[-1]) for r in rows]


def _lz4(src, n):
    """LZ4 block decode (tools/pack.py entries)"""
    out, i = bytearray(), 0
    while i < len(src):
        t = src[i]
        i += 1
        lit = t >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= len(src):
            break
        off = src[i] | src[i + 1] << 8
        i += 2
        m = t & 15
        if m == 15:
            while True:
                b = src[i]
                i += 1
                m += b
                if b != 255:
                    break
        for _ in range(m + 4):
            out.append(out[-off])
    return bytes(out[:n])


def _both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def _both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def _date7(t):
    g = time.gmtime(t)
    return bytes([g.tm_year - 1900, g.tm_mon, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, 0])


def _date17(t):
    g = time.gmtime(t)
    return time.strftime("%Y%m%d%H%M%S", g).encode() + b"00\0"


def _record(name, lba, size, is_dir, t):
    n = len(name)
    r = struct.pack("<BB", 0, 0) + _both32(lba) + _both32(size) + _date7(t) + bytes([2 if is_dir else 0, 0, 0]) + \
        _both16(1) + bytes([n]) + name
    if len(r) % 2:
        r += b"\0"
    return bytes([len(r)]) + r[1:]


class _Dir:
    def __init__(self, name, parent):
        self.name, self.parent, self.dirs, self.files, self.lba, self.num = name, parent, {}, [], 0, 0


def write_iso(entries, out, volid="DSRPS2", t=None):
    """entries: [(iso path like "DATA/C0000.PAK", source path or bytes)] in sector order -> out.iso"""
    t = time.time() if t is None else t
    root = _Dir(b"\0", None)
    files = []  # (dir, name, source, size)
    for path, src in entries:
        parts = path.split("/")
        d = root
        for p in parts[:-1]:
            d = d.dirs.setdefault(p, _Dir(p.encode(), d))
        size = len(src) if isinstance(src, (bytes, bytearray)) else os.path.getsize(src)
        f = [d, (parts[-1] + ";1").encode(), src, size, 0]
        if any(x[1] == f[1] for x in d.files):
            raise ValueError(f"ISO name clash: {path}")
        d.files.append(f)
        files.append(f)
    # directories in path-table order (breadth first, children by name), numbered from 1
    dirs, q = [], [root]
    while q:
        d = q.pop(0)
        dirs.append(d)
        q += [d.dirs[k] for k in sorted(d.dirs)]
    for i, d in enumerate(dirs):
        d.num = i + 1
    lba = 20
    for d in dirs:
        d.lba = lba
        lba += 1
    for f in files:
        f[4] = lba
        lba += (f[3] + SECTOR - 1) // SECTOR
    total = lba

    def ptable(big):
        out = b""
        for d in dirs:
            name = d.name if d is not root else b"\0"
            e = bytes([len(name), 0]) + struct.pack(">I" if big else "<I", d.lba) + \
                struct.pack(">H" if big else "<H", d.parent.num if d.parent else 1) + name
            out += e + (b"\0" if len(name) % 2 else b"")
        return out
    lpt, mpt = ptable(False), ptable(True)
    assert len(lpt) <= SECTOR

    def dir_sector(d):
        recs = [_record(b"\0", d.lba, SECTOR, True, t), _record(b"\1", (d.parent or d).lba, SECTOR, True, t)]
        kids = [(c.name, c.lba, SECTOR, True) for c in d.dirs.values()] + [(f[1], f[4], f[3], False) for f in d.files]
        recs += [_record(n, l, s, isd, t) for n, l, s, isd in sorted(kids, key=lambda k: k[0])]
        b = b"".join(recs)
        if len(b) > SECTOR:
            raise ValueError(f"directory {d.name!r} needs {len(b)} B > one sector (the BIOS reads only the first)")
        return b.ljust(SECTOR, b"\0")

    pad = lambda s, n: s.encode().upper().ljust(n)[:n]  # noqa: E731
    pvd = (b"\1CD001\1\0" + pad("PLAYSTATION", 32) + pad(volid, 32) + bytes(8) + _both32(total) + bytes(32) +
           _both16(1) + _both16(1) + _both16(SECTOR) + _both32(len(lpt)) + struct.pack("<I", 18) + bytes(4) +
           struct.pack(">I", 19) + bytes(4) + _record(b"\0", root.lba, SECTOR, True, t) +
           pad("", 128) + pad("", 128) + pad("", 128) + pad("DSR PS2 PROTO", 128) + pad("", 37) * 3 +
           _date17(t) * 2 + b"0" * 16 + b"\0" + b"0" * 16 + b"\0" + b"\1\0")
    assert len(pvd) == 883, len(pvd)
    with open(out, "wb") as o:
        o.write(bytes(16 * SECTOR))
        o.write(pvd.ljust(SECTOR, b"\0"))
        o.write((b"\xffCD001\1").ljust(SECTOR, b"\0"))
        o.write(lpt.ljust(SECTOR, b"\0"))
        o.write(mpt.ljust(SECTOR, b"\0"))
        for d in dirs:
            o.write(dir_sector(d))
        for f in files:
            src, size = f[2], f[3]
            if isinstance(src, (bytes, bytearray)):
                o.write(src)
            else:
                with open(src, "rb") as s:
                    while True:
                        b = s.read(1 << 20)
                        if not b:
                            break
                        o.write(b)
            o.write(bytes(-size % SECTOR))
    return total * SECTOR


def build(elf, data, out, boot_id="DSRP_000.01"):
    """the disc tools/ps2-iso.sh makes from an ELF and a data/ dir; returns the image size"""
    cnf = f"BOOT2 = cdrom0:\\{boot_id};1\r\nVER = 1.00\r\nVMODE = NTSC\r\n".encode()
    entries = [("SYSTEM.CNF", cnf), (boot_id, elf)]
    for p in pack_order(data):
        entries.append(("DATA/" + "/".join(iso_name(c) for c in p.split("/")), os.path.join(data, p)))
    return write_iso(entries, out)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    n = build(sys.argv[1], sys.argv[2], sys.argv[3], *sys.argv[4:5])
    print(f"ISO: {sys.argv[3]} ({n} B)")


if __name__ == "__main__":
    main()
