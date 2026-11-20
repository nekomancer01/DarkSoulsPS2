"""Chunk streams (Polaris 3.3): the container every exported file the runtime reads in place is written in.

  python3 tools/chunkfile.py <file>...    list each file's id and chunks (id, version, payload bytes)

Layout (little-endian, every part 16-byte aligned; the C side is ps2/proto/src/chunk.h, tools/native/chunkw.h):
  header  u32 'CHNK', u32 file id (four characters, e.g. b"WRLD"), u32 total bytes (header to footer end), u32 chunks
  chunk   u32 id (four characters), u16 version, u16 flags (0), u32 payload bytes, u32 0; payload, zero-padded to 16
  footer  u32 'CEND', u32 total bytes, u32 0, u32 0
Ids are unique in a file. The runtime asks for each chunk by id and the version it reads and refuses any other
version, a missing required chunk, another file id and a truncated file (a "TEST error: chunk:" line). Bump a chunk's
version when its payload layout changes; adding a chunk needs no bump.

Payloads hold offsets, never pointers: the runtime reads them in place (base + offset), so the same loader runs in a
64-bit host build (TECH_DEBT core-1).
"""
import struct
import sys

MAGIC, END = b"CHNK", b"CEND"
HDR = 16

# The VU1 layout stamp (tools/vu1/layout.py layout_hash) of a file whose payloads hold VIF packets (chunk.h CHUNK_VU1L)
VU1L = b"VU1L"


def _id(i):
    i = i.encode() if isinstance(i, str) else bytes(i)
    assert len(i) == 4, f"chunk id {i!r}: four characters"
    return i


def pack(file_id, chunks):
    """file_id: four characters; chunks: [(id, version, payload bytes)] in file order -> the file's bytes"""
    ids = [_id(c[0]) for c in chunks]
    assert len(set(ids)) == len(ids), f"duplicate chunk ids {ids}"
    body = bytearray()
    for cid, (_, ver, data) in zip(ids, chunks):
        assert 0 <= ver <= 0xFFFF, f"chunk {cid}: version {ver}"
        data = bytes(data)
        body += struct.pack("<4sHHII", cid, ver, 0, len(data), 0) + data + b"\0" * (-len(data) % 16)
    total = HDR + len(body) + HDR
    return struct.pack("<4s4sII", MAGIC, _id(file_id), total, len(chunks)) + bytes(body) + struct.pack("<4sIII", END, total, 0, 0)


def write(path, file_id, chunks):
    """pack() to path; returns the byte count"""
    b = pack(file_id, chunks)
    with open(path, "wb") as f:
        f.write(b)
    return len(b)


def vu1_chunk():
    """('VU1L', 1, u32 layout hash): the stamp of the VU1 layout this build's packets are written for"""
    from vu1layout import L
    return (VU1L, 1, struct.pack("<I", L.layout_hash()))


class ChunkError(ValueError):
    pass


class File:
    """a chunk file read back: .id (bytes), .chunks {id: (version, payload memoryview)}, .order [ids]"""

    def __init__(self, blob, file_id=None, name="?"):
        b = memoryview(blob)
        if len(b) < 2 * HDR or bytes(b[:4]) != MAGIC:
            raise ChunkError(f"{name}: not a chunk file")
        self.id = bytes(b[4:8])
        if file_id is not None and self.id != _id(file_id):
            raise ChunkError(f"{name}: file id {self.id!r}, expected {_id(file_id)!r}")
        total, n = struct.unpack_from("<II", b, 8)
        if total > len(b) or bytes(b[total - HDR:total - HDR + 4]) != END or struct.unpack_from("<I", b, total - 12)[0] != total:
            raise ChunkError(f"{name}: truncated ({len(b)} of {total} bytes)")
        self.name, self.chunks, self.order, o = name, {}, [], HDR
        for _ in range(n):
            cid, ver, _f, ln, _r = struct.unpack_from("<4sHHII", b, o)
            if o + HDR + ln > total - HDR:
                raise ChunkError(f"{name}: chunk {cid!r} runs past the end")
            self.chunks[cid] = (ver, b[o + HDR:o + HDR + ln])
            self.order.append(cid)
            o += HDR + ln + (-ln % 16)
        if o != total - HDR:
            raise ChunkError(f"{name}: {n} chunks do not fill {total} bytes")
        self.size = total

    def get(self, cid, version):
        """payload bytes of chunk cid at version; ChunkError when missing or another version"""
        c = self.chunks.get(_id(cid))
        if c is None:
            raise ChunkError(f"{self.name}: no chunk {_id(cid)!r}")
        if c[0] != version:
            raise ChunkError(f"{self.name}: chunk {_id(cid)!r} version {c[0]}, this tool reads {version}")
        return bytes(c[1])

    def opt(self, cid, version):
        """as get, None when the chunk is missing"""
        return self.get(cid, version) if _id(cid) in self.chunks else None


def read(path, file_id=None):
    with open(path, "rb") as f:
        return File(f.read(), file_id, path)


def main():
    for p in sys.argv[1:]:
        try:
            f = read(p)
        except ChunkError as e:
            print(e)
            continue
        print(f"{p}: {f.id.decode(errors='replace')} {f.size} B, {len(f.order)} chunks")
        for cid in f.order:
            ver, data = f.chunks[cid]
            print(f"  {cid.decode(errors='replace')} v{ver} {len(data)} B")


if __name__ == "__main__":
    main()
