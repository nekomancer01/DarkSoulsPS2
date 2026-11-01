"""Asset packs for the PS2 prototype: several files in one archive, each LZ4-compressed in independent 64 KB blocks
so the PS2 can decompress one batch of blocks while the IOP reads the next (ps2/proto/src/pack.c).

Layout (little-endian), one pack per area (m10_02_00_00.pak: map + collision) or character (c0000.pak):
  header   char magic[4] "DSRP", u32 version 1, u32 entry count, u32 header bytes (multiple of 2048)
  entries  {char name[32]; u32 offset; u32 raw size; u32 packed size; u32 blocks; u32 block size; u32 pad[3]} = 64 B
  data     per entry at a 2048-byte (CD sector) aligned offset: u32 csize[blocks] (bit 31 set = block stored raw),
           then the blocks back to back. A block decompresses to block size bytes (the last one to the remainder).
Usage: pack.py <out.pak> <name>=<file> [<name>=raw:<file> ...]
  raw: every block stored uncompressed, so byte k of the entry sits at offset + 4 x blocks + k and can be read in place
  (map cells streamed at runtime: src/mapstream.c reads byte ranges with pack.c pack_read_async)
Compressor: tools/native/liblz4pack.so (gcc -O2 -shared -fPIC -o liblz4pack.so lz4pack.c); every block is
round-trip checked with its reference decoder."""
import ctypes, os, struct, sys, time

SECTOR = 2048
BLOCK = 65536
NATIVE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "native")


def lib():
    so = os.path.join(NATIVE, "liblz4pack.so")
    src = os.path.join(NATIVE, "lz4pack.c")
    if not os.path.exists(so) or os.path.getmtime(so) < os.path.getmtime(src):
        if os.system(f"gcc -O2 -shared -fPIC -o '{so}' '{src}'"):
            sys.exit("pack.py: building liblz4pack.so failed")
    L = ctypes.CDLL(so)
    L.lz4_compress.restype = ctypes.c_size_t
    L.lz4_compress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    L.lz4_decompress.restype = ctypes.c_long
    L.lz4_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    return L


def pack_entry(L, data, store=False):
    sizes, blobs = [], []
    out = ctypes.create_string_buffer(BLOCK + BLOCK // 255 + 64)
    chk = ctypes.create_string_buffer(BLOCK)
    for o in range(0, len(data), BLOCK):
        raw = data[o:o + BLOCK]
        n = 0 if store else L.lz4_compress(raw, len(raw), out, len(raw) - 1)  # must save a byte, else stored raw
        if n:
            c = out.raw[:n]
            if L.lz4_decompress(c, n, chk, BLOCK) != len(raw) or chk.raw[:len(raw)] != raw:
                sys.exit("pack.py: LZ4 round trip failed")
            sizes.append(n)
            blobs.append(c)
        else:
            sizes.append(len(raw) | 0x80000000)
            blobs.append(raw)
    return struct.pack(f"<{len(sizes)}I", *sizes) + b"".join(blobs), len(sizes)


def main():
    out, items = sys.argv[1], [a.split("=", 1) for a in sys.argv[2:]]
    L = lib()
    t0 = time.time()
    hdr_bytes = (16 + 64 * len(items) + SECTOR - 1) // SECTOR * SECTOR
    body, entries = bytearray(), []
    for name, path in items:
        store = path.startswith("raw:")
        data = open(path[4:] if store else path, "rb").read()
        packed, nb = pack_entry(L, data, store)
        off = hdr_bytes + len(body)
        entries.append(struct.pack("<32s8I", name.encode(), off, len(data), len(packed), nb, BLOCK, 0, 0, 0))
        body += packed
        body += b"\0" * (-len(body) % SECTOR)
        print(f"  {name}: {len(data)} -> {len(packed)} bytes ({100 * len(packed) / max(1, len(data)):.0f}%)")
    head = struct.pack("<4s3I", b"DSRP", 1, len(items), hdr_bytes) + b"".join(entries)
    with open(out, "wb") as f:
        f.write(head + b"\0" * (hdr_bytes - len(head)) + body)
    print(f"{out}: {len(items)} entries, {hdr_bytes + len(body)} bytes, {time.time() - t0:.1f} s")


main()
