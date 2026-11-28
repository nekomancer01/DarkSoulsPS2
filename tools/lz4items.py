"""LZ4 frames for the streamed items of a world block (MAP_LZ4): the report that decided it, and the writer.

Write mode:
  python3 -I tools/lz4items.py <maptab.disc.bin> <mapres.bin> <cells.disc.bin> <out mapres.lz.bin> <out cells.lz.bin> <out lzidx.bin>
  mapres.lz.bin   mapres.bin as LZ4 frames back to back, cut at the edges of the items mapblock.c res_open builds (this file's
                  res_items) plus raw gap frames, every frame <= 64 KB raw; a frame is compressed only when it gets at
                  least 1 byte smaller and is <= LZ_GRP_CAP raw (so raw + LZ_SLACK(packed) fits one arena page)
  cells.lz.bin    the near parts of cells.disc.bin in disc order, compressed the same way, each part 64-aligned and
                  placed on the drive's 32 KB blocks with discalign.place counting the wrapped file's payload offset
  lzidx.bin       chunk file LZIX (tools/chunkfile.py): LZHD (32 B: cells, res frames, cells.lz bytes, mapres.lz bytes,
                  mapres raw bytes, 0 x3), CELI (per cell table entry: u32 disc offset, u32 packed bytes | RAW_FLAG when
                  stored raw), RESF (per res frame: u32 raw offset, u32 disc offset | RAW_FLAG when stored raw; frame i
                  is rel[i+1]-rel[i] raw and disc[i+1]-disc[i] packed bytes, the last runs to the ends); offsets are
                  from the payload start. maptab.disc.bin is not changed: its bytes stay the raw size the arena
                  allocates. A block without a cell table / cells.bin writes none of the three (its mapres.bin stays raw).
  LZ4_OPT=1 (environment): tools/lz4hc/lz4opt.c optimal parse instead of lz4pack greedy; LZ4_STORE=1: nothing compressed.
  tools/pack.py wraps the outputs (Task 3: kinds mapresz / cellsz); LZ_SLACK / LZ_GRP_CAP are mirrored in src/mslz.h.

Report mode:
Usage: python3 -I tools/lz4items.py --report --build DIR [--opt] [--burg-only-opt] [--jobs N]
  DIR = a build directory with gen/<block>/ (maptab.disc.bin, mapres.bin, cells.disc.bin) from `make worldpacks`.
  Reads files only. Prints a markdown table, per block and kind:
    near       the cell table's near blobs in cells.disc.bin (one item per entry, CELL_PART entries too)
    res-chunk  mapstream items of the resident chunk streams in mapres.bin (ps2/proto/src/mapblock.c res_open, ported)
    res-tex    the TC_RES mip 0 / mip 1 pixels of mapres.bin (one item per texture level, as res_open)
  Every item is cut into frames of up to FRAME raw bytes (one arena page, 64 KB: the LZ4 window), each frame one LZ4
  block (tools/native/lz4pack.c greedy, `lz4pack`; tools/lz4hc/lz4opt.c optimal parse, `lz4opt` with --opt), stored
  raw when it does not get smaller. A frame costs 4 bytes in the packed size (its size word, as tools/lz4hc/bench.py).
  Decode ms/MB: the real pack.c lz4_decode (cut out and compiled for the host as bench.py does), per MB of raw bytes
  of the frames that are compressed; pack / opt.  Compilers are built in a temp dir; nothing in the tree is written."""
import atexit, ctypes, glob, multiprocessing, os, re, shutil, struct, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
if HERE not in sys.path:
    sys.path.insert(0, HERE)  # run as a script with -I: not on the path
import chunkfile  # noqa: E402
import discalign  # noqa: E402
import mapwrap  # noqa: E402
RAW_FLAG = 0x80000000  # lzidx.bin: the frame / part is stored raw
LZ_PAGE = 64 << 10  # mapstream.c MS_PAGE
LZ_GRP_CAP = 65152  # a frame / part above this is stored raw: raw + LZ_SLACK(packed) must fit one page (src/mslz.h)


def LZ_SLACK(p):
    """arena slack after raw bytes for decoding in place a frame of p packed bytes (src/mslz.h LZ_SLACK)"""
    return (((p >> 8) + 32 + 63) & ~63) + 64
HDR = 80  # maptab.bin header (tools/mapsplit.py), = sizeof(Header)
CELL = 48  # MapCell, cell table entries start 16 bytes after the table's count word
CHUNK = 64  # sizeof(Chunk)
TEXE = 32  # texmap.py entry
RES_ITEM = 64 << 10  # mapblock.c RES_ITEM
FRAME = 64 << 10  # raw bytes per frame: one arena page (mapstream.c MS_PAGE) = the LZ4 window
CELL_PART = 0x80000000
CHUNK_COARSE, CHUNK_GRASS = 64, 16
TC_RES = 0x80000000
PSM_T4 = 0x14
KINDS = ("near", "res-chunk", "res-tex")
# go / no-go (the plan's gate, reports/polaris/2026-11-28-lz4-streamed-items.md Task 1): the Burg blocks' streamed
# bytes at least 15 % smaller with lz4pack (packed / raw <= 0.85); the decode bound is a sanity check on the host
GO_RATIO, GO_MS_MB = 0.85, 6.0


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def cell_table(tab):
    """maptab bytes -> [(off, bytes, near, first, count)] of the cell table, header dict"""
    chunk_off, tex_off = struct.unpack_from("<2I", tab, 36)
    cell_off = u32(tab, 76)
    h = dict(chunks=u32(tab, 4), ntex=u32(tab, 8), chunk_off=chunk_off, tex_off=tex_off, cell_off=cell_off)
    if tab[3:4] != b"5" or not cell_off:
        return [], h
    t = HDR + cell_off - chunk_off
    n = u32(tab, t)
    rows = [struct.unpack_from("<5I", tab, t + 16 + CELL * i + 24) for i in range(n)]
    return rows, h


def chunk_range(ch, a, b):
    """ports mapblock.c chunk_range: (lo, hi) bytes of the streams of resident chunks [a, b) in mapres.bin"""
    lo, hi = None, 0
    for j in range(a, b):
        tris, nv, pos, col = ch[j][2], ch[j][3], ch[j][4], ch[j][6]
        if nv:
            e = (col + ((nv * 4 + 15) & ~15) + 16 + tris * 4 + 15) & ~15
        else:
            e = (col + tris * 12 + 15) & ~15
        lo = pos - 16 if lo is None or pos - 16 < lo else lo
        hi = max(hi, e)
    return (lo or 0), hi


def res_items(tab, h):
    """the (kind, offset in mapres.bin, bytes) items mapblock.c res_open makes for one block, in its order"""
    base = HDR - h["chunk_off"]  # map.bin offset -> maptab offset
    ch = [struct.unpack_from("<iIHHIII", tab, base + h["chunk_off"] + CHUNK * j) for j in range(h["chunks"])]
    rows, _ = cell_table(tab)
    # ctab[i].first / count (MapCell words 3, 4)
    items, last = [], 0
    n = len(rows)
    for i in range(n + 1):
        a = rows[i][3] if i < n else last
        e = a + rows[i][4] if i < n else h["chunks"]
        if i < n:
            last = max(last, e)
        j = a
        while j < e:
            lo, hi = chunk_range(ch, j, j + 1)
            g = j + 1
            coarse = ch[j][1] & (CHUNK_COARSE | CHUNK_GRASS)
            while g < e and (ch[g][1] & (CHUNK_COARSE | CHUNK_GRASS)) == coarse:
                l2, h2 = chunk_range(ch, g, g + 1)
                if not (h2 - lo <= RES_ITEM and l2 >= lo):
                    break
                hi = max(hi, h2)
                g += 1
            items.append(("res-chunk", lo, hi - lo))
            j = g
    for t in range(h["ntex"]):
        e = base + h["tex_off"] + TEXE * t
        w, hh, psm = struct.unpack_from("<2HB", tab, e)
        for lv in range(2):
            p = u32(tab, e + 16 + 4 * lv)
            if not p & TC_RES:
                continue
            wl, hl = w >> lv, hh >> lv
            items.append(("res-tex", p & ~TC_RES, wl * hl // 2 if psm == PSM_T4 else wl * hl))
    return items


def near_items(tab):
    """cells.disc.bin items: one per cell table entry with bytes; (items, overlap count). Entries that repeat a range
    seen before are not counted again (overlaps = entries inside a range another entry already covers)"""
    rows, _ = cell_table(tab)
    seen, items, ov = set(), [], 0
    spans = sorted((r[0], r[1]) for r in rows if r[1])
    end = 0
    for off, n in spans:
        if (off, n) in seen or off < end:
            ov += 1
            end = max(end, off + n)
            continue
        seen.add((off, n))
        items.append(("near", off, n))
        end = off + n
    return items, ov


def frames(data, items):
    """items [(kind, off, bytes)] of data -> raw frame bytes of up to FRAME, per item in order"""
    out = []
    for _, off, n in items:
        if off + n > len(data):
            raise SystemExit(f"lz4items: item at {off}+{n} is past the end of its file ({len(data)} bytes)")
        for s in range(0, n, FRAME):
            out.append(data[off + s:off + min(n, s + FRAME)])
    return out


def build_lib(tmp, name, srcs, extra=""):
    so = os.path.join(tmp, name)
    if os.system(f"gcc -O2 -shared -fPIC {extra} -o '{so}' " + " ".join(f"'{s}'" for s in srcs)):
        sys.exit(f"lz4items: building {name} failed")
    L = ctypes.CDLL(so)
    if not hasattr(L, "lz4_compress"):
        return L
    L.lz4_compress.restype = ctypes.c_size_t
    L.lz4_compress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    return L


def ps2_decoder(tmp):
    """lz4_decode from pack.c, compiled for the host (as tools/lz4hc/bench.py ps2_decoder)"""
    src = open(os.path.join(ROOT, "ps2/proto/src/pack.c")).read()
    m = re.search(r"static int lz4_decode\(.*?\n}\n", src, re.S)
    if not m:
        sys.exit("lz4items: lz4_decode not found in pack.c")
    c = os.path.join(tmp, "ps2dec.c")
    open(c, "w").write("#include <string.h>\n" + m.group(0) + """
long ps2_decode_all(const unsigned char *s, const unsigned *cs, const unsigned *raw, int nb, unsigned char *dst,
                    int reps)
{
    long tot = 0;
    for (int r = 0; r < reps; r++) {
        const unsigned char *p = s;
        tot = 0;
        for (int b = 0; b < nb; b++) {
            int n = lz4_decode(p, cs[b], dst + tot, raw[b]);
            if (n != (int)raw[b])
                return -1;
            p += cs[b], tot += n;
        }
    }
    return tot;
}
""")
    L = build_lib(tmp, "libps2dec.so", [c], "-fno-tree-loop-distribute-patterns")
    L.ps2_decode_all.restype = ctypes.c_long
    return L


def compress(L, frs):
    """-> [(bytes, stored raw)] per frame"""
    out = ctypes.create_string_buffer(FRAME + FRAME // 255 + 64)
    res = []
    for raw in frs:
        n = L.lz4_compress(raw, len(raw), out, len(raw) - 1) if len(raw) > 16 else 0
        res.append((out.raw[:n], False) if n else (raw, True))
    return res


def decode_ms_mb(D, frs, res):
    """(seconds, raw MB) the pack.c decoder needs for the compressed frames; the round trip is checked"""
    comp = [(c, f) for (c, st), f in zip(res, frs) if not st]
    if not comp:
        return 0.0, 0.0
    s = b"".join(c for c, _ in comp)
    nb = len(comp)
    cs = (ctypes.c_uint * nb)(*[len(c) for c, _ in comp])
    raw = (ctypes.c_uint * nb)(*[len(f) for _, f in comp])
    tot = sum(len(f) for _, f in comp)
    dst = ctypes.create_string_buffer(tot)
    if D.ps2_decode_all(s, cs, raw, nb, dst, 1) != tot or dst.raw != b"".join(f for _, f in comp):
        sys.exit("lz4items: pack.c decoder does not round-trip a frame")
    reps = max(1, int(2e8 // tot))  # ~200 MB decoded per measurement
    t0 = time.perf_counter()
    D.ps2_decode_all(s, cs, raw, nb, dst, reps)
    return (time.perf_counter() - t0) / reps, tot / 1048576


LIBS = {}


def work(job):
    """(block dir, use opt) -> {kind: dict}: raw frames and their compressions"""
    d, opt = job
    tab = open(os.path.join(d, "maptab.disc.bin"), "rb").read()
    rows, h = cell_table(tab)
    out = {}
    ni, ov = near_items(tab)
    cells = open(os.path.join(d, "cells.disc.bin"), "rb").read() if ni else b""
    res = open(os.path.join(d, "mapres.bin"), "rb").read() if os.path.exists(os.path.join(d, "mapres.bin")) else b""
    ri = res_items(tab, h) if res and rows else []
    sets = {"near": (cells, ni), "res-chunk": (res, [i for i in ri if i[0] == "res-chunk"]),
            "res-tex": (res, [i for i in ri if i[0] == "res-tex"])}
    for k, (data, items) in sets.items():
        frs = frames(data, items)
        r = dict(items=len(items), overlaps=ov if k == "near" else None, frames=frs,
                 raw=sum(map(len, frs)), maxitem=max([n for _, _, n in items], default=0),
                 pack=compress(LIBS["pack"], frs), opt=compress(LIBS["opt"], frs) if opt else None)
        out[k] = r
    return os.path.basename(d), out


def packed(res):
    return sum(4 + len(c) for c, _ in res)


def rawstored(res):
    return sum(len(c) for c, st in res if st)


def fmt_row(name, kind, r, D, tot):
    dp, mb = decode_ms_mb(D, r["frames"], r["pack"])
    do = decode_ms_mb(D, r["frames"], r["opt"])[0] if r["opt"] else None
    tot[kind]["raw"] += r["raw"]
    tot[kind]["pack"] += packed(r["pack"])
    tot[kind]["opt"] += packed(r["opt"]) if r["opt"] else 0
    tot[kind]["rs_p"] += rawstored(r["pack"])
    tot[kind]["rs_o"] += rawstored(r["opt"]) if r["opt"] else 0
    tot[kind]["dp"] += dp
    tot[kind]["mb"] += mb
    tot[kind]["do"] += do or 0
    tot[kind]["items"] += r["items"]
    tot[kind]["maxitem"] = max(tot[kind]["maxitem"], r["maxitem"])
    return row_text(name, kind, r["items"], r["overlaps"], r["raw"], packed(r["pack"]),
                    packed(r["opt"]) if r["opt"] else None, rawstored(r["pack"]),
                    rawstored(r["opt"]) if r["opt"] else None, r["maxitem"], dp, do, mb)


def row_text(name, kind, items, ov, raw, pk, op, rsp, rso, maxitem, dp, do, mb):
    pct = lambda a: 100.0 * a / raw if raw else 0.0
    st = f"{pct(rsp):.1f}" + (f" / {pct(rso):.1f}" if op is not None else "")
    dec = (f"{1000 * dp / mb:.2f}" if mb else "-") + ((f" / {1000 * do / mb:.2f}" if mb else " / -") if op is not None else "")
    return (f"| {name} | {kind} | {items} | {'-' if ov is None else ov} | {raw / 1024:.1f} | {pk / 1024:.1f} | "
            f"{'-' if op is None else f'{op / 1024:.1f}'} | {st} | {maxitem / 1024:.1f} | {dec} |")


def zero():
    return dict(raw=0, pack=0, opt=0, rs_p=0, rs_o=0, dp=0.0, do=0.0, mb=0.0, items=0, maxitem=0)


_W = {}


def env_on(value):
    """an environment flag's value is on (tools/ci/switches.py finds the reads by their literal os.environ.get)"""
    return value not in (None, "", "0")


def writer_libs():
    """(compressor, reference decompressor): lz4pack, or lz4opt's optimal parse with LZ4_OPT=1"""
    if not _W:
        import pack
        _W["dec"] = _W["comp"] = pack.lib()
        if env_on(os.environ.get("LZ4_OPT")):
            tmp = tempfile.mkdtemp(prefix="lz4items")
            atexit.register(shutil.rmtree, tmp, True)
            _W["comp"] = build_lib(tmp, "lz4opt.so", [os.path.join(ROOT, "tools/lz4hc/lz4opt.c")])
    return _W["comp"], _W["dec"]


def pack_frame(raw, store=False):
    """one frame / part -> (bytes, stored raw). Compressed only when it saves at least 1 byte, is > 16 bytes and
    <= LZ_GRP_CAP; round trip checked with the reference decoder"""
    if store or len(raw) <= 16 or len(raw) > LZ_GRP_CAP:
        return bytes(raw), True
    comp, dec = writer_libs()
    out = ctypes.create_string_buffer(len(raw) + len(raw) // 255 + 64)
    n = comp.lz4_compress(bytes(raw), len(raw), out, len(raw) - 1)
    if not n:
        return bytes(raw), True
    chk = ctypes.create_string_buffer(len(raw) + 64)
    dec.lz4_decompress.restype = ctypes.c_long
    dec.lz4_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    if dec.lz4_decompress(out.raw[:n], n, chk, len(raw)) != len(raw) or chk.raw[:len(raw)] != bytes(raw):
        sys.exit("lz4items: LZ4 round trip failed")
    if len(raw) + LZ_SLACK(n) > LZ_PAGE:
        sys.exit(f"lz4items: frame of {len(raw)} -> {n} B does not fit a page in place")
    return out.raw[:n], False


def res_frames(tab, h, res):
    """[(rel, raw bytes, is item)] tiling mapres.bin: the res_open items cut every FRAME bytes, raw gaps between"""
    spans = sorted({(o, n) for _, o, n in res_items(tab, h) if n})
    out, pos = [], 0

    def gap(a, b):
        for s in range(a, b, FRAME):
            out.append((s, min(b, s + FRAME) - s, False))

    for o, n in spans:
        if o < pos:
            raise ValueError(f"mapres.bin item at {o}+{n} overlaps the one before (ends at {pos})")
        if o + n > len(res):
            raise ValueError(f"mapres.bin item at {o}+{n} is past the end of the file ({len(res)} bytes)")
        gap(pos, o)
        for s in range(0, n, FRAME):
            out.append((o + s, min(n, s + FRAME) - s, True))
        pos = o + n
    gap(pos, len(res))
    return out


def cells_base():
    """offset of cells.lz.bin's payload in its pack entry (tools/mapwrap.py; kind cellsz once it exists)"""
    return mapwrap.PAYLOAD.get("cellsz", mapwrap.PAYLOAD["cells"]) if mapwrap.ON else 0


def build(tab, res, cells, store=None):
    """(maptab.disc.bin, mapres.bin, cells.disc.bin bytes) -> dict(res_lz, cells_lz, lzidx, stats), None for a block
    without a cell table or cells.bin (its mapres.bin stays raw, no lzidx.bin)"""
    store = env_on(os.environ.get("LZ4_STORE")) if store is None else store
    rows, h = cell_table(tab)
    if not rows or not cells:
        return None
    ni = near_items(tab)[0]  # (entries naming one blob twice are one item; ranges that merely overlap are an error)
    for (_, a, an), (_, b, _) in zip(ni, ni[1:]):
        if b < a + an:
            raise ValueError(f"cell table: near parts at {a}+{an} and {b} overlap")
    # res frames, back to back
    rel, disc, blob, pos = [], [], bytearray(), 0
    for r, n, _item in res_frames(tab, h, res):
        b, st = pack_frame(res[r:r + n], store or not _item)
        rel.append(r)
        disc.append(len(blob) | (RAW_FLAG if st else 0))
        blob += b
    res_lz = bytes(blob)
    # near parts in disc order, 64-aligned, on the drive's blocks
    base, out, placed = cells_base(), bytearray(), {}
    for _, off, n in ni:
        if off + n > len(cells):
            raise ValueError(f"near part {off}+{n} is past the end of cells.bin ({len(cells)} bytes)")
        b, st = pack_frame(cells[off:off + n], store)
        at = discalign.place(base + ((len(out) + 63) & ~63), len(b)) - base
        out += b"\0" * (at - len(out)) + b
        placed[(off, n)] = (at, len(b) | (RAW_FLAG if st else 0))
    out += b"\0" * (-len(out) % 64)
    celi = [placed.get((r[0], r[1]), (0, 0)) if r[1] else (0, 0) for r in rows]
    hd = struct.pack("<8I", len(rows), len(rel), len(out), len(res_lz), len(res), 0, 0, 0)
    idx = chunkfile.pack("LZIX", [(b"LZHD", 1, hd),
                                  (b"CELI", 1, b"".join(struct.pack("<2I", *c) for c in celi)),
                                  (b"RESF", 1, b"".join(struct.pack("<2I", r, d) for r, d in zip(rel, disc)))])
    stats = dict(res_raw=len(res), res_lz=len(res_lz), cells_raw=sum(n for _, _, n in ni), cells_lz=len(out),
                 frames=len(rel), parts=len(ni))
    return dict(res_lz=res_lz, cells_lz=bytes(out), lzidx=idx, stats=stats)


def read_index(blob):
    """lzidx.bin bytes -> dict(cells, frames, cells_lz, res_lz, res_raw, celi [(disc, packed, raw?)], resf [(rel, disc, raw?)])"""
    f = chunkfile.File(blob, "LZIX", "lzidx.bin")
    hd = struct.unpack("<8I", f.get("LZHD", 1))
    ce, rf = f.get("CELI", 1), f.get("RESF", 1)
    assert len(ce) == 8 * hd[0] and len(rf) == 8 * hd[1], "lzidx.bin: CELI / RESF sizes do not match LZHD"
    celi = [(a, b & ~RAW_FLAG, bool(b & RAW_FLAG)) for a, b in struct.iter_unpack("<2I", ce)]
    resf = [(a, b & ~RAW_FLAG, bool(b & RAW_FLAG)) for a, b in struct.iter_unpack("<2I", rf)]
    return dict(cells=hd[0], frames=hd[1], cells_lz=hd[2], res_lz=hd[3], res_raw=hd[4], celi=celi, resf=resf)


def unpack(tab, idx, res_lz, cells_lz):
    """the write's inverse: -> (mapres.bin bytes, [near part bytes per cell table entry])"""
    dec = writer_libs()[1]
    dec.lz4_decompress.restype = ctypes.c_long
    dec.lz4_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]

    def one(src, raw_n, st):
        if st:
            return bytes(src)
        buf = ctypes.create_string_buffer(raw_n + 64)
        if dec.lz4_decompress(bytes(src), len(src), buf, raw_n) != raw_n:
            raise ValueError("lz4items: a frame does not decode to its raw size")
        return buf.raw[:raw_n]

    ix = read_index(idx)
    res = bytearray()
    for i, (rel, d, st) in enumerate(ix["resf"]):
        rend = ix["resf"][i + 1][0] if i + 1 < ix["frames"] else ix["res_raw"]
        dend = ix["resf"][i + 1][1] if i + 1 < ix["frames"] else ix["res_lz"]
        res += one(res_lz[d:dend], rend - rel, st)
    rows, _ = cell_table(tab)
    parts = []
    for (d, pk, st), r in zip(ix["celi"], rows):
        parts.append(one(cells_lz[d:d + pk], r[1], st) if r[1] else b"")
    return bytes(res), parts


def write_main(a):
    src_t, src_r, src_c, dst_r, dst_c, dst_i = a
    tab = open(src_t, "rb").read()
    res = open(src_r, "rb").read() if os.path.exists(src_r) else b""
    cells = open(src_c, "rb").read() if os.path.exists(src_c) else b""
    r = build(tab, res, cells)
    if r is None:
        for p in (dst_r, dst_c, dst_i):
            if os.path.exists(p):
                os.remove(p)  # (no cells.bin: nothing compressed, stale outputs go)
        print("lz4items: no cells.bin, mapres.bin stays raw")
        return
    for path, b in ((dst_r, r["res_lz"]), (dst_c, r["cells_lz"]), (dst_i, r["lzidx"])):
        tmp = f"{path}.{os.getpid()}"
        with open(tmp, "wb") as f:
            f.write(b)
        os.replace(tmp, path)
    s = r["stats"]
    print(f"lz4items: {s['frames']} res frames, {s['parts']} near parts, mapres.bin {s['res_raw'] >> 10} -> "
          f"{s['res_lz'] >> 10} KB, cells {s['cells_raw'] >> 10} -> {s['cells_lz'] >> 10} KB")


def main():
    a = sys.argv[1:]
    if len(a) == 6 and "--report" not in a:
        return write_main(a)
    if "--report" not in a or "--build" not in a:
        sys.exit(__doc__)
    build = a[a.index("--build") + 1]
    opt = "--opt" in a
    jobs = int(a[a.index("--jobs") + 1]) if "--jobs" in a else min(8, os.cpu_count() or 1)
    dirs = sorted(d for d in glob.glob(os.path.join(build, "gen", "m*_*"))
                  if os.path.exists(os.path.join(d, "maptab.disc.bin")) and os.path.exists(os.path.join(d, "cells.disc.bin")))
    if not dirs:
        sys.exit(f"lz4items: no gen/<block>/maptab.disc.bin under {build}")
    tmp = tempfile.mkdtemp(prefix="lz4items")
    try:
        LIBS["pack"] = build_lib(tmp, "lz4pack.so", [os.path.join(ROOT, "tools/native/lz4pack.c")])
        LIBS["opt"] = build_lib(tmp, "lz4opt.so", [os.path.join(ROOT, "tools/lz4hc/lz4opt.c")]) if opt else None
        D = ps2_decoder(tmp)
        with multiprocessing.get_context("fork").Pool(jobs) as pool:
            results = dict(pool.map(work, [(d, opt) for d in dirs], chunksize=1))
        lines = ["| block | kind | items | overlaps | raw KB | packed KB (lz4pack) | packed KB (lz4opt) | stored raw % "
                 "(pack / opt) | page-cap raw KB (largest item) | host decode ms/MB (pack / opt) |",
                 "|---|---|---|---|---|---|---|---|---|---|"]
        tot = {k: zero() for k in KINDS}
        burg = {k: zero() for k in KINDS}
        for name, out in sorted(results.items()):
            for k in KINDS:
                lines.append(fmt_row(name, k, out[k], D, tot))
                if name.startswith("m10_01_00_00"):
                    fmt_row(name, k, out[k], D, burg)
        for title, t in (("Burg blocks (m10_01_00_00_*)", burg), ("All blocks", tot)):
            for k in KINDS + ("all",):
                s = t[k] if k != "all" else {f: (sum(t[x][f] for x in KINDS) if f != "maxitem" else max(t[x][f] for x in KINDS))
                                              for f in t["near"]}
                lines.append(row_text(f"**{title}**", k, s["items"], None, s["raw"], s["pack"], s["opt"] if opt else None,
                                      s["rs_p"], s["rs_o"], s["maxitem"], s["dp"], s["do"], s["mb"]))
        b = {f: sum(burg[x][f] for x in KINDS) for f in burg["near"]}
        best = b["opt"] if opt else b["pack"]
        ratio = best / b["raw"] if b["raw"] else 1.0
        ms = 1000 * (b["do"] if opt else b["dp"]) / b["mb"] if b["mb"] else 0.0
        go = ratio <= GO_RATIO and ms <= GO_MS_MB
        print("\n".join(lines))
        print(f"\nBurg blocks: packed / raw = {ratio:.3f} ({'lz4opt' if opt else 'lz4pack'}), host decode {ms:.2f} ms/MB; "
              f"thresholds: packed / raw <= {GO_RATIO}, decode <= {GO_MS_MB} ms/MB")
        print(f"**{'GO' if go else 'NO-GO'}**: LZ4 frames on streamed items (Burg packed/raw {ratio:.3f}, {ms:.2f} ms/MB)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
