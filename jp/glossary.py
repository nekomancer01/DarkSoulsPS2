"""Build a Japanese -> English glossary of proper nouns from the game's own text (msg/JAPANESE vs msg/ENGLISH: same
FMG files, same IDs), plus a few hand-checked terms that appear only in internal names. Writes the glossary table in
jp-strings.sqlite. Usage: glossary.py <game dir>"""
import os, re, sqlite3, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
# Names the msg files do not pair up (titles, nicknames, internal shorthand).
MANUAL = {
    "カアス": "Kaathe", "闇撫で": "Darkstalker", "闇撫でのカアス": "Darkstalker Kaathe", "フラムト": "Frampt",
    "世界の蛇": "primordial serpent", "王の器": "Lordvessel", "シース": "Seath", "白竜シース": "Seath the Scaleless",
    "使命者": "Chosen Undead", "不死の勇者": "Chosen Undead", "篝火": "bonfire", "エスト": "Estus",
    "アノール・ロンド": "Anor Londo", "小ロンド": "New Londo", "病み村": "Blighttown", "イザリス": "Izalith",
    "ヘルカイト": "Hellkite", "飛竜": "drake", "グウィン": "Gwyn", "黒騎士": "Black Knight", "銀騎士": "Silver Knight",
    "人間性": "humanity", "亡者": "Hollow", "白霊": "white phantom", "黒霊": "black phantom", "闇霊": "dark spirit",
}


# Name-file entries that are also everyday words in internal strings (サイズ = Scythe or "size").
GENERIC = {"サイズ", "回復", "テスト用", "テスト"}


def undcx(b):
    return zlib.decompress(b[b.find(b"DCA\0") + 8:]) if b[:4] == b"DCX\0" else b


def bnd3(b):
    """BND3 with format 0x74: 0x18-byte entries (flags, size, offset, id, name offset, uncompressed size)."""
    assert b[:4] == b"BND3" and b[0xC] == 0x74, "unexpected BND3 format"
    n = struct.unpack_from("<I", b, 0x10)[0]
    for i in range(n):
        _, size, off, _, name_off, _ = struct.unpack_from("<IIIIII", b, 0x20 + 0x18 * i)
        name = b[name_off:b.index(b"\0", name_off)].decode("cp932").rsplit("\\", 1)[-1]
        yield name, b[off:off + size]


def fmg(b):
    """DS1 FMG (version 1): 12-byte groups (offset index, first ID, last ID), int32 string offsets, UTF-16LE."""
    groups, strings, offs = struct.unpack_from("<III", b, 0xC)
    out = {}
    for g in range(groups):
        idx, first, last = struct.unpack_from("<iii", b, 0x1C + 12 * g)
        for k in range(last - first + 1):
            o = struct.unpack_from("<i", b, offs + 4 * (idx + k))[0]
            if o > 0:
                end = o
                while b[end:end + 2] != b"\0\0":
                    end += 2
                out[first + k] = b[o:end].decode("utf-16le")
    return out


def main():
    msg = os.path.join(sys.argv[1], "msg")
    pairs = {}
    for bnd in ("item.msgbnd.dcx", "menu.msgbnd.dcx"):
        jp = {n: fmg(d) for n, d in bnd3(undcx(open(os.path.join(msg, "JAPANESE", bnd), "rb").read()))}
        en = {n: fmg(d) for n, d in bnd3(undcx(open(os.path.join(msg, "ENGLISH", bnd), "rb").read()))}
        # *_name_ files only (weapons, armor, rings, items, spells, NPCs, places): menu and help text pairs are UI
        # phrasing ("無効" = "Do not use amiibo") that would steer translations wrong.
        for name in [n for n in jp.keys() & en.keys() if n.endswith("_name_.fmg")]:
            for i, j in jp[name].items():
                e = en[name].get(i)
                # names only: short, one line, no sentences
                if e and 2 <= len(j) <= 16 and "\n" not in j + e and len(e) <= 48 and not re.search(r"[。、？！0-9０-９]", j):
                    pairs.setdefault(j, e)
    for g in GENERIC:
        pairs.pop(g, None)
    pairs.update(MANUAL)
    con = sqlite3.connect(os.path.join(HERE, "jp-strings.sqlite"))
    con.execute("DROP TABLE IF EXISTS glossary")
    con.execute("CREATE TABLE glossary(jp TEXT PRIMARY KEY, en TEXT)")
    con.executemany("INSERT INTO glossary VALUES(?,?)", pairs.items())
    con.commit()
    print(f"{len(pairs)} glossary terms ({len(MANUAL)} manual)")


main()
