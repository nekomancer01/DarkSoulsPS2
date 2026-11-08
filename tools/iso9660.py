"""Bootable PS2 disc image in pure Python: the disc tools/ps2-iso.sh makes (SYSTEM.CNF, the ELF under its boot id,
every pack in data/ under DATA\\, ISO9660 level 1 names), for make iso, the in-browser converter (tools/pages,
Pyodide) and anywhere xorriso is missing. The layer plan (plan(): budget doc proposal 3) decides the sector order.

    python3 tools/iso9660.py <proto.elf> <data dir> <out.iso> [boot id=DSRP_000.01]

Layout: sectors 0-15 zero, 16 primary volume descriptor, 17 terminator, the L / M path tables (18 / 19 on a CD image,
257 / 258 on a DVD one: the BIOS reads a DVD's there, cdvdman CD_newmedia), then one
sector per directory (the BIOS CDVD driver reads only a directory's first sector: checked; the root takes 32 on a DVD
image, the rest empty), DSRLAYER.BIN, then the files in plan order, each from a 32 KB ECC block (LSN % 16 == 0).
A dual-layer image: layer 0 ends where its volume descriptor says (a whole block), layer 1 opens with a copy of it in
its first 32 sectors (at 0 and 16), then its files; files of both layers are in the one directory tree, by absolute LSN.
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


ECC = 16                   # sectors per ECC block: the drive's smallest read (32 KB)
LAYER_SECTORS = 2084960    # ASSUMED: one DVD-9 layer, ~4.27 GB (the DVD-9 spec's 8.54 GB / 2, less a margin)
LAYER1_RESERVED = 32       # layer 1 starts with sectors nothing reads (Sony): its volume descriptor copies go there
WINDOW = {"cd": 4371, "dvd": 14764, "dvd9": 13360}  # fast seek window, +/- sectors (budget doc, Sony's figures)
LAYOUT_NAME = "DSRLAYER.BIN"


def write_iso(entries, out, volid="DSRPS2", t=None, media="dvd", dual=False, copies=(), align=ECC):
    """entries: [(iso path like "DATA/C0000.PAK", source path or bytes, layer)] in sector order -> out.iso.
    media "dvd": the root directory takes 32 sectors, which is what makes PCSX2 run the image as a DVD (it compares
    the halves of the root's both-endian size, CDVDcommon FindDiskType); "cd": one. dual: layer 1 starts at the first entry with layer 1
    (on a whole block; the layer-0 volume descriptor says layer 0 ends there and a copy of it opens layer 1, in its
    reserved sectors: how PCSX2 finds the break, CDVDisoReader FindLayer1Start); else one layer. copies: [(iso path
    of a layer-0 file, its layer-1 copy's iso path)]: the copy is not in a directory, only in DSRLAYER.BIN. Every file
    starts on an ECC block (align sectors). Returns (image bytes, {iso path: (lba, size)})."""
    t = time.time() if t is None else t
    root = _Dir(b"\0", None)
    files = []  # [dir, name, source, size, lba, layer, listed]
    copy_of = {c: o for o, c in copies}
    for path, src, layer in entries:
        parts = path.split("/")
        size = len(src) if isinstance(src, (bytes, bytearray)) else os.path.getsize(src)
        if path in copy_of:  # a layer-1 copy: placed, not listed
            files.append([None, path, src, size, 0, layer, False])
            continue
        d = root
        for p in parts[:-1]:
            d = d.dirs.setdefault(p, _Dir(p.encode(), d))
        f = [d, (parts[-1] + ";1").encode(), src, size, 0, layer, True]
        if any(x[1] == f[1] for x in d.files):
            raise ValueError(f"ISO name clash: {path}")
        d.files.append(f)
        files.append(f)
    dirs, q = [], [root]
    while q:
        d = q.pop(0)
        dirs.append(d)
        q += [d.dirs[k] for k in sorted(d.dirs)]
    for i, d in enumerate(dirs):
        d.num = i + 1
    # PCSX2 runs an image under 452849 sectors as a DVD only when the root directory's size differs between bytes
    # 166-167 and 171-172 of the volume descriptor (CDVDcommon FindDiskType): a size of 64 KB or more (both-endian
    # fields, low byte 0). The BIOS reads the first sector only; the rest stays empty
    root_secs = 32 if media != "cd" else 1
    # the BIOS file system reads a DVD's L path table from sector 257, whatever the volume descriptor says (cdvdman
    # CD_newmedia: "DVD Read mode", where a UDF bridge disc has it after its anchor at 256); a CD's from the descriptor
    pt = 257 if media != "cd" else 18
    lba = pt + 2
    for d in dirs:
        d.lba = lba
        lba += root_secs if d is root else 1
    up = lambda n: -(-n // align) * align  # noqa: E731
    l1 = None
    for f in files:
        if dual and f[5] == 1 and l1 is None:  # the break: layer 0 to whole blocks, then layer 1's reserved start
            l1 = up(lba)
            lba = l1 + LAYER1_RESERVED
        lba = up(lba)
        f[4] = lba
        lba += (f[3] + SECTOR - 1) // SECTOR
    total = up(lba)
    layer0 = l1 if l1 is not None else total

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

    def dsize(d):
        return SECTOR * (root_secs if d is root else 1)

    def dir_sector(d):
        recs = [_record(b"\0", d.lba, dsize(d), True, t), _record(b"\1", (d.parent or d).lba, dsize(d.parent or d), True, t)]
        kids = [(c.name, c.lba, SECTOR, True) for c in d.dirs.values()] + [(f[1], f[4], f[3], False) for f in d.files]
        recs += [_record(n, l, s, isd, t) for n, l, s, isd in sorted(kids, key=lambda k: k[0])]
        b = b"".join(recs)
        if len(b) > SECTOR:
            raise ValueError(f"directory {d.name!r} needs {len(b)} B > one sector (the BIOS reads only the first)")
        return b.ljust(dsize(d), b"\0")

    pad = lambda s, n: s.encode().upper().ljust(n)[:n]  # noqa: E731

    def pvd(size):
        v = (b"\1CD001\1\0" + pad("PLAYSTATION", 32) + pad(volid, 32) + bytes(8) + _both32(size) + bytes(32) +
             _both16(1) + _both16(1) + _both16(SECTOR) + _both32(len(lpt)) + struct.pack("<I", pt) + bytes(4) +
             struct.pack(">I", pt + 1) + bytes(4) + _record(b"\0", root.lba, dsize(root), True, t) +
             pad("", 128) + pad("", 128) + pad("", 128) + pad("DSR PS2 PROTO", 128) + pad("", 37) * 3 +
             _date17(t) * 2 + b"0" * 16 + b"\0" + b"0" * 16 + b"\0" + b"\1\0")
        assert len(v) == 883, len(v)
        return v.ljust(SECTOR, b"\0")
    where = {}
    for (path, _, _), f in zip(entries, files):
        where[path] = (f[4], f[3])
    with open(out, "wb") as o:
        o.write(bytes(16 * SECTOR))
        o.write(pvd(layer0))
        o.write((b"\xffCD001\1").ljust(SECTOR, b"\0"))
        o.write(bytes((pt - 18) * SECTOR))
        o.write(lpt.ljust(SECTOR, b"\0"))
        o.write(mpt.ljust(SECTOR, b"\0"))
        for d in dirs:
            o.write(dir_sector(d))
        pos = pt + 2 + sum(root_secs if d is root else 1 for d in dirs)
        for f in files:
            if l1 is not None and pos <= l1 <= f[4]:  # layer 1 opens with copies of the volume descriptor: at its first
                o.write(bytes((l1 - pos) * SECTOR))  # sector (PCSX2 looks there) and 16 sectors in (cdvdman's layer search)
                for _ in range(2):
                    o.write(pvd(total - l1))
                    o.write((b"\xffCD001\1").ljust(SECTOR, b"\0"))
                    o.write(bytes(14 * SECTOR))
                pos = l1 + 32
            o.write(bytes((f[4] - pos) * SECTOR))
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
            pos = f[4] + (size + SECTOR - 1) // SECTOR
        o.write(bytes((total - pos) * SECTOR))
    return total * SECTOR, where, l1


# ---- layer plan (budget doc proposal 3, "Where data goes on DVD-9") ----
# Walk-connected regions: world blocks joined by world.pak's neighbour masks, less the links below (per area pair: the
# game crosses them behind a warp, so a layer change there is behind DSR's own load). Each region keeps its blocks,
# their character packs and its areas' audio (data/snd/mAA.pak) together, in neighbour order, on one layer.
NOT_WALKED = {
    frozenset(("m10_02_00_00", "m18_01_00_00_a")): "Asylum -> Firelink is the crow flight: event 11810000 warps "
                                                    "(cutscene 100225), there is no walk between the two",
}
BOOT_PACKS = ("menu.pak", "world.pak", "common.pak", "c0000.pak", "snd/main.pak")


def regions(data):
    """[[block names in neighbour order]] for the world build (the boot block's region first), else one per area pack"""
    blocks = _world_blocks(data)
    if not blocks:
        return [[p[:-4]] for p in sorted(os.listdir(data)) if p.startswith("m") and p.endswith(".pak")]
    seen, out = set(), []
    boot = next((i for i, b in enumerate(blocks) if b[1]), 0)
    for start in [boot] + list(range(len(blocks))):
        if start in seen:
            continue
        seen.add(start)
        reg, q = [], [start]
        while q:
            i = q.pop(0)
            reg.append(blocks[i][0])
            for k in range(len(blocks)):
                linked = blocks[i][2] >> k & 1 or blocks[k][2] >> i & 1
                if linked and k not in seen and frozenset((blocks[i][0], blocks[k][0])) not in NOT_WALKED:
                    seen.add(k)
                    q.append(k)
        out.append(reg)
    return out


def plan(data, layers=None, layer0_cap=None):
    """the disc's file order [(pack rel path, layer)], the shared packs copied to layer 1, dual (bool), report lines.
    Layer 0: the boot packs, the boot block's region, the packs every region reads in play (character sounds); layer 1:
    the other regions, the one with the most data (it streams the most) last, at the outer edge, after a copy of the
    shared packs. A single-layer image keeps the same order and copies (DSRLAYER.BIN's planned break), so it counts the
    layer changes its dual-layer pressing would make. layers None: dual only when the data needs it; 2: dual;
    layer0_cap (sectors): an earlier break, for tests. ValueError when a region cannot sit on one layer."""
    paks = sorted(os.path.relpath(os.path.join(r, f), data).replace(os.sep, "/")
                  for r, _, fs in os.walk(data) for f in fs if f.endswith(".pak"))
    secs = {p: -(-os.path.getsize(os.path.join(data, p)) // SECTOR) for p in paks}
    size = lambda ps: sum(-(-secs[p] // ECC) * ECC for p in ps)  # noqa: E731
    boot = [p for p in BOOT_PACKS if p in secs]
    regs = regions(data)
    used, groups = set(boot), []
    for ri, reg in enumerate(regs):
        g = []
        for i, name in enumerate(reg):
            own = [name + ".pak"] + [p for p in paks if p.startswith(name + "/")]
            if ri == 0 and i == 0:  # the boot block's characters are the top-level c*.pak
                own += [p for p in paks if "/" not in p and p[0] == "c"]
            if i == (len(reg) - 1) // 2:  # the areas' audio (BGM, ambience) in the middle of the region's blocks
                own += sorted({"snd/" + n[:3] + ".pak" for n in reg})
            g += [p for p in own if p in secs and p not in used and p not in g]
        used.update(g)
        groups.append((reg, g))
    shared = [p for p in paks if p not in used]  # character sounds (snd/cXXXX.pak): any region may read them
    rest = sorted(groups[1:], key=lambda x: size(x[1]))
    l0 = boot + (groups[0][1] if groups else []) + shared
    copies = shared[:] if rest else []
    l1 = [p for _, g in rest for p in g]
    total = size(l0) + size(copies) + size(l1)
    cap = layer0_cap or LAYER_SECTORS
    dual = layers == 2 or (layers is None and total + 64 > LAYER_SECTORS)
    if dual and not rest:
        raise ValueError("a dual-layer image needs a second walk-connected region (there is one)")
    if size(l0) + 64 > cap:
        raise ValueError(f"layer 0 needs {size(l0)} sectors for the boot packs and the first region "
                         f"({'+'.join(groups[0][0])}), over {cap}: a walk-connected region cannot cross the break")
    if dual and size(copies) + size(l1) + LAYER1_RESERVED > LAYER_SECTORS:
        raise ValueError(f"layer 1 needs {size(copies) + size(l1)} sectors, over {LAYER_SECTORS}")
    if not dual and total + 64 > LAYER_SECTORS:
        raise ValueError(f"{total} sectors do not fit one layer ({LAYER_SECTORS})")
    order = [(p, 0) for p in l0] + [(p, 1) for p in copies] + [(p, 1) for p in l1]
    mb = lambda n: n * SECTOR >> 20  # noqa: E731
    rep = [f"layers: {2 if dual else 1}; layer 0 {mb(size(l0))} MB (boot, {'+'.join(groups[0][0]) if groups else '-'}, "
           f"{len(shared)} shared packs)" + (f", {'layer 1' if dual else 'planned layer 1'} {mb(size(copies) + size(l1))} "
                                             f"MB (" + ", ".join('+'.join(r) for r, _ in rest) +
                                             f"; {len(copies)} shared packs copied)" if rest else "")]
    return order, copies, dual, rep


def window_report(where, blocks, media):
    """each block's pack, characters and audio against the fast seek window: lines to print"""
    w = WINDOW.get(media, WINDOW["dvd"])
    out = []
    for name, packs in blocks:
        lb = [where[p] for p in packs if p in where]
        if not lb:
            continue
        span = max(a + -(-n // SECTOR) for a, n in lb) - min(a for a, _ in lb)
        out.append(f"  window {name}: {span * SECTOR >> 20} MB from its pack over its characters and audio"
                   + ("" if span <= w else f" (over the {w * SECTOR >> 20} MB fast seek window)"))
    return out


def iso_path(rel):
    return "DATA/" + "/".join(iso_name(c) for c in rel.split("/"))


def build(elf, data, out, boot_id="DSRP_000.01", media=None, layers=None, layer0_mb=None, log=print):
    """the disc tools/ps2-iso.sh makes from an ELF and a data/ dir; returns the image size.
    media "dvd" (default; env ISO_MEDIA) or "cd" (PCSX2 runs it as a CD: the old test images); layers None / 1 / 2
    (env ISO_LAYERS); layer0_mb: a layer-0 cap that forces the break (env ISO_LAYER0_MB, tests)."""
    media = media or os.environ.get("ISO_MEDIA", "dvd")
    layers = layers or (int(os.environ["ISO_LAYERS"]) if os.environ.get("ISO_LAYERS") else None)
    layer0_mb = layer0_mb or (float(os.environ["ISO_LAYER0_MB"]) if os.environ.get("ISO_LAYER0_MB") else None)
    if layer0_mb:
        layers = 2
    order, copies, dual, rep = plan(data, layers, int(layer0_mb * 512) if layer0_mb else None)
    for r in rep:
        log(r)
    cnf = f"BOOT2 = cdrom0:\\{boot_id};1\r\nVER = 1.00\r\nVMODE = NTSC\r\n".encode()
    copy_paths = [(iso_path(p), iso_path(p) + "@1") for p in copies]
    entries = [("SYSTEM.CNF", cnf, 0), (boot_id, elf, 0), (LAYOUT_NAME, bytes(SECTOR), 0)]
    seen = set()
    for p, layer in order:
        ip = iso_path(p) + ("@1" if p in seen else "")
        seen.add(p)
        entries.append((ip, os.path.join(data, p), layer))
    size, where, l1 = write_iso(entries, out, media=media, dual=dual, copies=copy_paths)
    first1 = next((e[0] for e in entries if e[2] == 1), None)
    brk = l1 if dual else where[first1][0] if first1 else 0
    pairs = [(where[a][0], where[b][0]) for a, b in copy_paths]
    # DSRLAYER.BIN (dsrsnd.c df_layout): u32 "DSRL", version 1, layer 1's first LSN (dual: the real break; one layer:
    # the planned one, 0 = no second region), volume sectors, copies n, then n x {u32 layer-0 LSN, u32 layer-1 LSN}
    layout = struct.pack("<4s4I", b"DSRL", 1, brk, size // SECTOR, len(pairs)) + b"".join(
        struct.pack("<2I", a, b) for a, b in pairs)
    if len(layout) > SECTOR:
        raise ValueError("DSRLAYER.BIN: too many copies for one sector")
    with open(out, "r+b") as o:  # its sector is in place: fill it in
        o.seek(where[LAYOUT_NAME][0] * SECTOR)
        o.write(layout.ljust(SECTOR, b"\0"))
    log(f"layout: media {media}, {'dual layer, break' if dual else 'one layer, planned break'} at LSN {brk}, "
        f"{len(pairs)} layer-1 copies")
    blocks = []
    paks = [p for p, _ in order]
    for reg in regions(data):
        for n in reg:
            blocks.append((n, [iso_path(p) for p in [n + ".pak", "snd/" + n[:3] + ".pak"] + paks
                               if p == n + ".pak" or p.startswith(n + "/") or p == "snd/" + n[:3] + ".pak"]))
    for line in window_report(where, blocks, "dvd9" if dual else media):
        log(line)
    return size


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        n = build(sys.argv[1], sys.argv[2], sys.argv[3], *sys.argv[4:5])
    except ValueError as e:  # the layer plan or a directory does not fit: no image
        sys.exit(f"iso9660: {e}")
    print(f"ISO: {sys.argv[3]} ({n} B)")


if __name__ == "__main__":
    main()
