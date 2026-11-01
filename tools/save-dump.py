#!/usr/bin/env python3
"""Read a DSR-PS2 save file (ps2/proto/src/save.h layout) and print the header, progression and set event flags;
exit 1 if the header / size / CRC check fails.
  save-dump.py <SAVE0.BIN> [--flags]       (from build/save/<name>N.bin, or a card via tools/ps2-mc.sh get)
  save-dump.py <file> --expect souls=777 flag=11010000 ...   check values (exit 1 on a mismatch)"""
import struct, sys, zlib

AREAS = [0, 100, 101, 102, 110, 120, 121, 130, 131, 132, 140, 141, 150, 151, 160, 170, 180, 181]
GROUPS = [0, 1, 5, 6, 7]


def slot_base(slot):
    """section slot (flags.c flag_slot) -> first flag id of that section"""
    block, sec = divmod(slot, 10)
    g, a = (0, 0) if block == 0 else ((block - 1) // 18 + 1, (block - 1) % 18)
    return GROUPS[g] * 10000000 + AREAS[a] * 10000 + sec * 1000


def parse(b):
    magic, ver, hdr, total, crc, seq, nprog, nflag, _ = struct.unpack_from("<IHHIIIIII", b)
    b = b[:total]
    ok = magic == 0x53525344 and total == len(b) == hdr + nprog + nflag and crc == zlib.crc32(b[hdr:]) & 0xFFFFFFFF
    p = b[hdr:hdr + nprog]
    souls, hum, lvl = struct.unpack_from("<3i", p)
    stats = list(p[12:20])
    hp, estus, estus_max, bonfire, map_id = struct.unpack_from("<4iI", p, 20)
    pos = struct.unpack_from("<4f", p, 40)
    play, deaths = struct.unpack_from("<Ii", p, 56)
    equip = list(struct.unpack_from("<19i", p, 64))
    ninv, = struct.unpack_from("<i", p, 140)
    inv = [struct.unpack_from("<IHH", p, 144 + 8 * i) for i in range(max(0, min(ninv, 256)))]
    f = b[hdr + nprog:]
    npages, = struct.unpack_from("<H", f)
    flags = []
    for i in range(npages):
        slot, = struct.unpack_from("<H", f, 4 + i * 132)
        words = struct.unpack_from("<32I", f, 8 + i * 132)
        base = slot_base(slot)
        flags += [base + n for n in range(1000) if words[n >> 5] >> (31 - (n & 31)) & 1]
    return dict(ok=ok, version=ver, seq=seq, size=len(b), souls=souls, humanity=hum, level=lvl, stats=stats, hp=hp,
                estus=estus, estus_max=estus_max, bonfire=bonfire, map=f"m{map_id >> 24:02d}_{map_id >> 16 & 255:02d}_"
                f"{map_id >> 8 & 255:02d}_{map_id & 255:02d}", pos=pos, play_time=play, deaths=deaths, equip=equip,
                inv=inv, flag_pages=npages, flags=flags)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return 2
    d = parse(open(a[0], "rb").read())
    fl = d.pop("flags")
    print(" ".join(f"{k}={v}" for k, v in d.items() if k != "inv"))
    print("inv", " ".join(f"{i:#x}x{n}" for i, n, _ in d["inv"]))
    print(f"flags on: {len(fl)}", " ".join(map(str, fl)) if "--flags" in a else "")
    bad = not d["ok"]
    if "--expect" in a:
        for e in a[a.index("--expect") + 1:]:
            k, v = e.split("=")
            got = (int(v) in fl) if k == "flag" else d.get(k)
            want = True if k == "flag" else int(v)
            print(f"expect {e}: {'ok' if got == want else f'FAIL (got {got})'}")
            bad |= got != want
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
