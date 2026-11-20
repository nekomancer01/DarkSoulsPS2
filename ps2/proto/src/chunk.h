/* Chunk streams (Polaris 3.3): the exported files the runtime reads in place.
 *
 * A file is a header, chunks, a footer; every field little-endian, every part 16-byte aligned:
 *   header  u32 'CHNK', u32 file id (four characters: what the file is, e.g. 'WRLD'), u32 total bytes (header to
 *           footer end), u32 chunk count
 *   chunk   u32 id (four characters), u16 version, u16 flags (0), u32 payload bytes, u32 0; the payload, zero-padded
 *           to 16 bytes. Ids are unique in a file, except 'PAD ' chunks (version 0, zero payload) a writer puts
 *           before a chunk whose payload must start at a multiple of more than 16 bytes (128 for model data); the
 *           order is the writer's.
 *   footer  u32 'CEND', u32 total bytes, u32 0, u32 0
 * Writers: tools/chunkfile.py (exporters), tools/native/chunkw.h (C tools: pvs_bake, libmapx). Readers: this (runtime and
 * Lab), tools/chunkfile.py (Python tools that read an export back).
 *
 * A loader asks for each chunk by id and the one version it reads. A chunk of another version, a missing required
 * chunk, a file of another id and a truncated file are refused with a "TEST error: chunk:" line naming the file (and the
 * chunk and both versions), so packs from an older or newer build fail loudly instead of being misread. A chunk's
 * version is bumped when its payload layout changes; a new optional chunk needs no bump.
 *
 * Payloads are read in place: a loader keeps the chunk's base pointer and turns offsets stored in the payload into
 * addresses as it reads (CHUNK_AT), never by patching pointers into the blob, so the same loader runs in a 64-bit host
 * build (TECH_DEBT core-1). */
#ifndef CHUNK_H
#define CHUNK_H

#define CHUNK_ID(a, b, c, d) ((unsigned)(a) | (unsigned)(b) << 8 | (unsigned)(c) << 16 | (unsigned)(d) << 24)
#define CHUNK_MAGIC CHUNK_ID('C', 'H', 'N', 'K')
#define CHUNK_END CHUNK_ID('C', 'E', 'N', 'D')
#define CHUNK_HDR 16u
/* size argument when the caller does not know the buffer's size (a raw entry streamed by offset): the footer at the
 * header's total must be there and agree, which a file cut short fails */
#define CHUNK_SIZE_UNKNOWN 0xFFFFFFFFu

typedef struct {
    const unsigned char *base;  /* the file (16-byte aligned) */
    const char *name;           /* for messages: "world.bin", "m10_01_00_00/dg.bin" */
    unsigned file_id, size, n;  /* size: header + chunks + footer */
    unsigned have;              /* bytes of the file at base (chunk_open: size; chunk_open_part: the head) */
} ChunkFile;

/* the base-plus-offset accessor: a typed view of bytes at offset off of base (no pointer is stored in the blob) */
#define CHUNK_AT(type, base, off) ((const type *)((const unsigned char *)(base) + (off)))

/* check blob as a chunk file of file_id: magic, id, total within size, footer, every chunk inside. 0 (and a TEST
 * error line) when it is not; f is zeroed then. A NULL blob is no file: 0 without a message. */
int chunk_open(ChunkFile *f, const void *blob, unsigned size, unsigned file_id, const char *name);
/* a file read in pieces (a raw pack entry read by offset): head holds its first have bytes, size is the whole entry's.
 * Checks the header against size (a total past it: truncated) and the chunk headers that lie within head; chunk_get /
 * chunk_opt then find chunks whose header lies within head (the payload may lie beyond: chunk_offset gives its file
 * offset for the read). The footer is not checked (not read). */
int chunk_open_part(ChunkFile *f, const void *head, unsigned have, unsigned size, unsigned file_id, const char *name);
/* file offset of a payload chunk_get returned (from the file's start) */
#define chunk_offset(f, payload) ((unsigned)((const unsigned char *)(payload) - (f)->base))
/* the payload of chunk id at version (its byte count in *size when size is not NULL). Missing or another version:
 * NULL and a TEST error line. */
const void *chunk_get(const ChunkFile *f, unsigned id, unsigned version, unsigned *size);
/* as chunk_get, but a missing chunk is no error (NULL, *size 0); another version still is */
const void *chunk_opt(const ChunkFile *f, unsigned id, unsigned version, unsigned *size);
/* "WRLD" for messages (buf: 5 bytes) */
const char *chunk_name(unsigned id, char *buf);
/* refusals so far (tests and the boot log read it) */
extern unsigned chunk_refusals;

/* The VU1 layout stamp (tools/vu1/layout.py layout_hash) of a file whose payloads hold VIF packets: chunk 'VU1L'
 * version 1, payload u32 hash. 1 when it matches this ELF's VU1L_HASH or the file has no stamp; 0 and a TEST error
 * naming the file and both hashes when it differs. map.bin keeps its stamp in the cell table word until phase 4 and
 * checks it through chunk_vu1_check_hash. */
#define CHUNK_VU1L CHUNK_ID('V', 'U', '1', 'L')
int chunk_vu1_check(const ChunkFile *f);
int chunk_vu1_check_hash(const char *name, unsigned hash, unsigned mask);

#endif
