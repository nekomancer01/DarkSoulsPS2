"""Collect unique Japanese strings (UTF-16LE and Shift-JIS) from the DSR exe and data files into jp-strings.sqlite.
Usage: extract.py <game dir> [subdir ...]   (.dcx files are unpacked first; BND containers are scanned as-is)"""
import os, re, sqlite3, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
# Random binary bytes often decode as valid kanji, so a string must be NUL-terminated, contain kana, and consist only of
# ASCII, kana, common kanji, Japanese punctuation and fullwidth forms.
KANA = re.compile(r"[ぁ-ゖァ-ヺー]")
OK = re.compile(r"[\x20-\x7e　-〿ぁ-ゖァ-ー一-龥！-～ｦ-ﾟ‐-…■-●]+")
U16 = re.compile(rb"(?:[\x20-\x7e]\x00|[\x00-\xff][\x30\x4e-\x9f\xff])+(?=\x00\x00)")
SJIS = re.compile(rb"(?:[\x20-\x7e]|[\x81-\x9f\xe0-\xef][\x40-\x7e\x80-\xfc])+(?=\x00)")


def undcx(b):
    if b[:4] != b"DCX\0":
        return b
    i = b.find(b"DCA\0")
    return zlib.decompress(b[i + 8:]) if i >= 0 else b


def common_kanji(s):
    """Every kanji is JIS level 1 (Shift-JIS 0x889F-0x9872), the ~3000 everyday ones; code bytes mostly land outside."""
    for ch in s:
        if "\u4e00" <= ch <= "\u9fa5":
            try:
                b = ch.encode("cp932")
            except UnicodeEncodeError:
                return False
            if not b"\x88\x9f" <= b <= b"\x98\x72":
                return False
    return True


def good(s):
    return (len(s) >= 4 and OK.fullmatch(s) and len(KANA.findall(s)) >= 1 and common_kanji(s)
            and not re.search(r"[$`@\\^|~{}\[\]]", s) or False)


def strings(b):
    for m in U16.finditer(b):
        for off in (0, 1):  # runs found mid-character are misaligned by one byte
            raw = m.group()[off:]
            s = raw[:len(raw) // 2 * 2].decode("utf-16le", "ignore").strip()
            if good(s):
                yield s
                break
    for m in SJIS.finditer(b):
        try:
            s = m.group().decode("cp932").strip()
        except UnicodeDecodeError:
            continue
        if good(s):
            yield s


def main():
    game, subs = sys.argv[1], sys.argv[2:]
    con = sqlite3.connect(os.path.join(HERE, "jp-strings.sqlite"))
    con.execute("""CREATE TABLE IF NOT EXISTS strings(s TEXT PRIMARY KEY, n INTEGER, sources TEXT,
        en TEXT, model TEXT, translated_at TEXT)""")
    files = [os.path.join(game, "DarkSoulsRemastered.exe")]
    for d in subs:
        for root, _, names in os.walk(os.path.join(game, d)):
            files += [os.path.join(root, n) for n in sorted(names)]
    seen = {}
    for f in files:
        rel = os.path.relpath(f, game)
        try:
            data = undcx(open(f, "rb").read())
        except (OSError, zlib.error) as e:
            print("skip", rel, e)
            continue
        for s in strings(data):
            if len(s) <= 400:
                n, src = seen.get(s, (0, []))
                if len(src) < 5 and rel not in src:
                    src.append(rel)
                seen[s] = (n + 1, src)
    con.execute("DELETE FROM strings WHERE en IS NULL")  # a rescan replaces everything not yet translated
    for s, (n, src) in seen.items():
        con.execute("""INSERT INTO strings(s, n, sources) VALUES(?,?,?)
            ON CONFLICT(s) DO UPDATE SET n=excluded.n, sources=excluded.sources""", (s, n, ";".join(src)))
    con.commit()
    print(f"{len(files)} files, {len(seen)} unique Japanese strings")


main()
