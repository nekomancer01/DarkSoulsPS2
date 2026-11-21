"""Disc layout of a world block's streamed near parts (Polaris 4.7): cells.bin with every part placed on the drive's
32 KB ECC blocks, and maptab.bin's cell table pointing at the new places.

    discalign.py <maptab.bin> <cells.bin> <out maptab.bin> <out cells.bin>
    discalign.py --check <pack> ...     (the rule below on built packs: exit 1 and a list when a part breaks it)

The drive reads whole 32 KB ECC blocks (16 sectors; Sony's DVD notes, budget doc "Disc and read-ahead"). A near part
(src/mapstream.c reads each cell table entry's blob of cells.bin as one item, NEAR_PART <= 64 KB) that crosses one more
block boundary than its size needs costs the drive a whole extra block: on main 3386712 that was 844 of 2692 blocks
(31 %) over the 14 world blocks' 1866 parts. Here a part starts on the next block when, where it would go, it would
touch more blocks than ceil(bytes / 32 KB); otherwise it stays packed after the one before (64-byte aligned, as
tools/mapcells.py writes them). Parts keep their order (the cells' Z order is the disc's), so the cost is padding:
9.3 % of cells.bin (5 MB over the 14 blocks, ~1 % of the image). The rule holds on the disc because tools/pack.py
starts an entry of 32 KB or more on a 32 KB boundary of its pack (raw: its data after the block size table) and
tools/iso9660.py starts every pack on an ECC block.

The cell table (DSM5, tools/mapcells.py write_cells): at maptab offset HDR + cell_off - chunk_off (the runtime resolves
maptab offsets from map.bin's start, tools/mapsplit.py), u32 cells, flags, 0, 0, then 48 bytes per entry with the
blob's offset and bytes in cells.bin at +24 / +28. Nothing else points into cells.bin (chunk records inside a blob are
relative to it), so only those two words move. The offline tools (mapshot.py, lodview.py, blockreport.py) read the
export's own cells.bin with map.bin: they are not touched.

DISC_ALIGN=0 (environment, make DISC_ALIGN=0): the two files are copied unchanged (the layout before 2026-11-21)."""
import os, shutil, struct, sys

ECC = 32768
HDR = 80


def table(maptab):
    """maptab bytes -> (position of the cell table's first entry, entries) or (None, 0) without one"""
    if len(maptab) < HDR or maptab[3:4] != b"5":
        return None, 0
    chunk_off = struct.unpack_from("<I", maptab, 36)[0]
    cell_off = struct.unpack_from("<I", maptab, 76)[0]
    if not cell_off:
        return None, 0
    t = HDR + cell_off - chunk_off
    return t + 16, struct.unpack_from("<I", maptab, t)[0]


def blocks(a, n):
    """ECC blocks a part of n bytes at a touches"""
    return (a + n - 1) // ECC - a // ECC + 1 if n else 0


def place(pos, n):
    """where a part of n bytes goes at or after pos (64-byte aligned): there, unless it would touch more blocks than
    its size needs; then at the next block"""
    if n and blocks(pos, n) > -(-n // ECC):
        return -(-pos // ECC) * ECC
    return pos


def layout(maptab, cells):
    """(new maptab, new cells, parts, padding bytes)"""
    t, n = table(maptab)
    if t is None:
        return bytes(maptab), bytes(cells), 0, 0
    rows = [struct.unpack_from("<2I", maptab, t + 48 * i + 24) for i in range(n)]
    order = sorted(range(n), key=lambda i: rows[i][0])
    out, mt, pad, parts, moved = bytearray(), bytearray(maptab), 0, 0, {}
    for i in order:
        off, nb = rows[i]
        if off + nb > len(cells):
            raise ValueError(f"cell table entry {i}: {off}+{nb} past cells.bin ({len(cells)} B)")
        key = (off, nb)
        if key in moved:  # (two entries naming one blob: keep them together)
            struct.pack_into("<I", mt, t + 48 * i + 24, moved[key])
            continue
        pos = (len(out) + 63) & ~63
        at = place(pos, nb)
        pad += at - len(out)
        out += b"\0" * (at - len(out))
        moved[key] = at
        struct.pack_into("<I", mt, t + 48 * i + 24, at)
        out += cells[off:off + nb]
        parts += 1 if nb else 0
    out += b"\0" * (-len(out) % 64)
    return bytes(mt), bytes(out), parts, pad


def check_pack(path):
    """the rule on a built pack: [problem lines] (empty: every near part touches the blocks its size needs, and the
    pack's entries are placed as tools/pack.py places them)"""
    d = open(path, "rb").read()
    if d[:4] != b"DSRP":
        return []
    n = struct.unpack_from("<I", d, 8)[0]
    ent = {}
    bad = []
    for i in range(n):
        name, off, raw, packed, nblk, bs = struct.unpack_from("<32s5I", d, 16 + 64 * i)
        name = name.rstrip(b"\0").decode()
        ent[name] = (off, raw, packed, nblk)
        data = off + 4 * nblk
        if packed >= ECC and data % ECC:
            bad.append(f"{path}: entry {name} ({packed} B) does not start its blocks on a 32 KB boundary ({data % ECC})")
        elif packed < ECC and packed and blocks(off, packed) > 1:
            bad.append(f"{path}: entry {name} ({packed} B) crosses a 32 KB boundary")
    if "cells.bin" in ent and "maptab.bin" in ent:
        mo, mraw, mpacked, mn = ent["maptab.bin"]
        co, craw, cpacked, cn = ent["cells.bin"]
        if mpacked != mraw + 4 * mn or cpacked != craw + 4 * cn:
            return bad + [f"{path}: maptab.bin / cells.bin not stored raw"]
        mt = d[mo + 4 * mn:mo + 4 * mn + mraw]
        c0 = co + 4 * cn
        t, cnt = table(mt)
        over = 0
        for i in range(cnt if t is not None else 0):
            off, nb = struct.unpack_from("<2I", mt, t + 48 * i + 24)
            if blocks(c0 + off, nb) > -(-nb // ECC):
                over += 1
                if over <= 5:
                    bad.append(f"{path}: near part {i} ({nb} B at {off} in cells.bin) touches "
                               f"{blocks(c0 + off, nb)} ECC blocks, needs {-(-nb // ECC)}")
        if over > 5:
            bad.append(f"{path}: ... {over} near parts in all touch more 32 KB blocks than they need")
    return bad


def main():
    if sys.argv[1:2] == ["--check"]:
        bad = [b for p in sys.argv[2:] for b in check_pack(p)]
        for b in bad:
            print(b)
        sys.exit(1 if bad else 0)
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    src_t, src_c, dst_t, dst_c = sys.argv[1:]
    if os.environ.get("DISC_ALIGN", "1") == "0" or not os.path.exists(src_c):
        shutil.copyfile(src_t, dst_t)
        if os.path.exists(src_c):
            shutil.copyfile(src_c, dst_c)
        elif os.path.exists(dst_c):
            os.remove(dst_c)  # (no cells.bin: none in the pack either)
        return
    mt, cb, parts, pad = layout(open(src_t, "rb").read(), open(src_c, "rb").read())
    for path, b in ((dst_t, mt), (dst_c, cb)):
        tmp = f"{path}.{os.getpid()}"
        with open(tmp, "wb") as f:
            f.write(b)
        os.replace(tmp, path)
    print(f"discalign: {parts} near parts on 32 KB blocks, {pad >> 10} KB padding, cells.bin "
          f"{os.path.getsize(src_c) >> 10} -> {len(cb) >> 10} KB")


if __name__ == "__main__":
    main()
