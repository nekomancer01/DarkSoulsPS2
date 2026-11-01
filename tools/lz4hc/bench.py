"""Compare the optimal-parse LZ4 compressor (liblz4opt.so) with pack.py's greedy one (tools/native/liblz4pack.so) on
every pack in ps2/proto/data: packed size per pack, compression time, and decode time with the PS2 decoder itself
(lz4_decode is cut out of ps2/proto/src/pack.c and compiled for the host, so the round trip checks the real code).
Decode time on the host is a proxy for the EE; the sequence count (tokens) is printed too, since the EE loop cost is
per token plus per byte.
Usage: bench.py [pak ...]   (default: ps2/proto/data/*.pak)"""
import ctypes, glob, os, re, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
RAW = 0x80000000


def build(so, srcs, extra=""):
    if not os.path.exists(so) or any(os.path.getmtime(so) < os.path.getmtime(s) for s in srcs):
        if os.system(f"gcc -O2 -shared -fPIC {extra} -o '{so}' " + " ".join(f"'{s}'" for s in srcs)):
            sys.exit(f"bench.py: building {so} failed")
    return ctypes.CDLL(so)


def bind(L):
    L.lz4_compress.restype = ctypes.c_size_t
    L.lz4_compress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    L.lz4_decompress.restype = ctypes.c_long
    L.lz4_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    return L


def ps2_decoder():
    """lz4_decode from pack.c plus a timing loop, compiled for the host"""
    src = open(os.path.join(ROOT, "ps2/proto/src/pack.c")).read()
    m = re.search(r"static int lz4_decode\(.*?\n}\n", src, re.S)
    if not m:
        sys.exit("bench.py: lz4_decode not found in pack.c")
    c = os.path.join(HERE, "build", "ps2dec.c")
    os.makedirs(os.path.dirname(c), exist_ok=True)
    open(c, "w").write("#include <string.h>\n" + m.group(0) + """
/* decode nb blocks (sizes in cs, data back to back) reps times into dst (block bytes each); returns total bytes or -1 */
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
    L = build(os.path.join(HERE, "build", "libps2dec.so"), [c], "-fno-tree-loop-distribute-patterns")
    L.ps2_decode_all.restype = ctypes.c_long
    return L


def blocks_of(path, greedy):
    """yields (pak, entry name, [raw block bytes]) for every entry"""
    d = open(path, "rb").read()
    magic, ver, count, hdr = struct.unpack_from("<4s3I", d)
    for e in range(count):
        name, off, raw, packed, nb, bs = struct.unpack_from("<32s5I", d, 16 + 64 * e)
        cs = struct.unpack_from(f"<{nb}I", d, off)
        p, out = off + 4 * nb, []
        buf = ctypes.create_string_buffer(bs)
        for b in range(nb):
            n, want = cs[b] & ~RAW, min(bs, raw - b * bs)
            if cs[b] & RAW:
                out.append(d[p:p + n])
            else:
                if greedy.lz4_decompress(d[p:p + n], n, buf, bs) != want:
                    sys.exit(f"bench.py: {path} {name} block {b} does not decode")
                out.append(buf.raw[:want])
            p += n
        yield name.rstrip(b"\0").decode(), out


def tokens(c):
    """number of sequences in an LZ4 block"""
    s, n, t = 0, len(c), 0
    while s < n:
        tok = c[s]; s += 1; t += 1
        l = tok >> 4
        if l == 15:
            while True:
                b = c[s]; s += 1; l += b
                if b != 255: break
        s += l
        if s >= n: break
        s += 2
        if tok & 15 == 15:
            while c[s] == 255: s += 1
            s += 1
    return t


def compress(L, blocks):
    out = ctypes.create_string_buffer(65536 + 65536 // 255 + 64)
    res, t0 = [], time.perf_counter()
    for raw in blocks:
        n = L.lz4_compress(raw, len(raw), out, len(raw) - 1)
        res.append((out.raw[:n], len(raw)) if n else (raw, len(raw) | RAW))
    return res, time.perf_counter() - t0


def decode_time(D, res, reps):
    comp = [(c, r) for c, r in res if not r & RAW]
    if not comp:
        return 0.0, 0
    s = b"".join(c for c, _ in comp)
    nb = len(comp)
    cs = (ctypes.c_uint * nb)(*[len(c) for c, _ in comp])
    raw = (ctypes.c_uint * nb)(*[r for _, r in comp])
    dst = ctypes.create_string_buffer(sum(r for _, r in comp))
    if D.ps2_decode_all(s, cs, raw, nb, dst, 1) < 0:
        sys.exit("bench.py: pack.c decoder rejected a block")
    blob = b"".join(d for d in [dst.raw])
    t0 = time.perf_counter()
    D.ps2_decode_all(s, cs, raw, nb, dst, reps)
    return (time.perf_counter() - t0) / reps, blob


def main():
    paks = sys.argv[1:] or sorted(glob.glob(os.path.join(ROOT, "ps2/proto/data/*.pak")))
    greedy = bind(build(os.path.join(ROOT, "tools/native/liblz4pack.so"), [os.path.join(ROOT, "tools/native/lz4pack.c")]))
    opt = bind(build(os.path.join(HERE, "liblz4opt.so"), [os.path.join(HERE, "lz4opt.c")]))
    D = ps2_decoder()
    tot = dict(raw=0, g=0, o=0, tg=0, to=0, dg=0, do=0, kg=0, ko=0)
    print(f"{'pak':18} {'raw':>9} {'greedy':>9} {'opt':>9} {'saved':>6} {'c.greedy':>8} {'c.opt':>7} "
          f"{'dec.g ms':>8} {'dec.o ms':>8} {'tok.g':>7} {'tok.o':>7}")
    for pak in paks:
        row = dict(raw=0, g=0, o=0, tg=0, to=0, dg=0, do=0, kg=0, ko=0)
        for name, blocks in blocks_of(pak, greedy):
            rg, tg = compress(greedy, blocks)
            ro, to = compress(opt, blocks)
            dg, bg = decode_time(D, rg, 20)
            do, bo = decode_time(D, ro, 20)
            want = b"".join(b for b, (_, r) in zip(blocks, ro) if not r & RAW)
            if bo and bo != want:
                sys.exit(f"bench.py: {pak} {name}: optimal-parse output decodes wrong with pack.c's decoder")
            sz = lambda res: sum(4 + len(c) for c, _ in res)
            row["raw"] += sum(map(len, blocks))
            row["g"] += sz(rg); row["o"] += sz(ro)
            row["tg"] += tg; row["to"] += to; row["dg"] += dg; row["do"] += do
            row["kg"] += sum(tokens(c) for c, r in rg if not r & RAW)
            row["ko"] += sum(tokens(c) for c, r in ro if not r & RAW)
        for k in tot:
            tot[k] += row[k]
        pr(os.path.basename(pak), row)
    pr("TOTAL", tot)


def pr(name, r):
    print(f"{name:18} {r['raw']:9} {r['g']:9} {r['o']:9} {100 * (1 - r['o'] / max(1, r['g'])):5.1f}% "
          f"{r['tg']:7.2f}s {r['to']:6.2f}s {1000 * r['dg']:8.2f} {1000 * r['do']:8.2f} {r['kg']:7} {r['ko']:7}")


main()
