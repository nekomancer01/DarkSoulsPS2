"""Find the virtual address of every Japanese exe string in jp-strings.sqlite (UTF-16LE and Shift-JIS, NUL-terminated).
Writes '<hex VA>\t<string>' lines for JpRefs.java. Usage: locate.py <exe> <out.tsv>"""
import os, sqlite3, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def sections(b):
    pe = struct.unpack_from("<I", b, 0x3c)[0]
    n, opt = struct.unpack_from("<HxxxxxxxxxxxxH", b, pe + 6)
    base = struct.unpack_from("<Q", b, pe + 24 + 24)[0]
    out, off = [], pe + 24 + opt
    for i in range(n):
        vsize, va, rsize, raw = struct.unpack_from("<IIII", b, off + 40 * i + 8)
        out.append((raw, raw + rsize, base + va))
    return out


def main():
    b = open(sys.argv[1], "rb").read()
    secs = sections(b)
    rows = [r[0] for r in sqlite3.connect(os.path.join(HERE, "jp-strings.sqlite")).execute(
        "SELECT s FROM strings WHERE sources LIKE '%DarkSoulsRemastered.exe%'")]
    found = 0
    with open(sys.argv[2], "w", encoding="utf-8") as out:
        for s in rows:
            hit = False
            for enc in (s.encode("utf-16le") + b"\0\0", s.encode("cp932", "ignore") + b"\0"):
                i = b.find(enc)
                while i >= 0:
                    for lo, hi, va in secs:
                        if lo <= i < hi:
                            out.write(f"{va + i - lo:x}\t{s}\n")
                            hit = True
                    i = b.find(enc, i + 1)
            found += hit
    print(f"{found}/{len(rows)} strings located")


main()
