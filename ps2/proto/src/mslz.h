/* LZ4 frames for the streamed map items (MAP_LZ4; tools/lz4items.py writes lzidx.bin, mapres.lz.bin, cells.lz.bin).
 *
 * lzidx.bin is a chunk file 'LZIX': LZHD v1 (32 bytes: cells, res frames, cells.lz bytes, mapres.lz bytes, mapres raw
 * bytes, 0 x3), CELI v1 (8 bytes per cell table entry: offset in the cells.lz payload, packed bytes | 0x80000000 when
 * stored raw; an empty entry is 0, 0) and RESF v1 (8 bytes per mapres frame: raw offset in mapres.bin, offset in the
 * mapres.lz payload | 0x80000000 when stored raw). The frames of mapres.lz lie back to back: a frame's packed size is the
 * next frame's offset minus its own (the last: to the payload's end), its raw size likewise from rel (the last: to the
 * raw bytes). Frames are the edges of the items mapblock.c res_open builds, plus raw gap frames; <= 64 KB raw.
 *
 * A run (an item or a group of consecutive items, or one near part) is read as one disc range and decoded in place in
 * its arena slot: the slot holds raw + LZ_SLACK(packed) bytes, the read lands at the slot's end (at, 64-aligned) and each
 * frame is decoded forward over the input, which stays ahead of the output. mslz_decode is resumable per frame so the
 * streamer can spread a run over several frames of the game.
 *
 * The index is read in place (base + offset) and never patched (TECH_DEBT core-1); the same code runs in a 64-bit host
 * build (tools/ci/test_mslz.sh). */
#ifndef MSLZ_H
#define MSLZ_H

#include "chunk.h"

typedef struct {
    const unsigned char *base;
    const char *name;
    unsigned ncell, nres, cbytes, rbytes, rraw;  /* LZHD */
    const unsigned *celi, *resf;                  /* 2 words per cell / frame */
} MsLz;

/* a disc range in the .lz payload: packed bytes at off, holding frames [frame0, frame0 + frames) of mapres (frame0 has
 * MSLZ_CELL set for a near part: then frames is 1 and frame0 & ~MSLZ_CELL is the cell table entry); raw: the run's
 * decoded bytes (the slot is raw + LZ_SLACK(packed) when mslz_any_packed) */
typedef struct { unsigned off, packed, frame0, frames, raw; } MsLzRun;
#define MSLZ_CELL 0x80000000u

/* in-place slack of a packed run: the slot is raw + LZ_SLACK(packed) bytes (tools/lz4items.py LZ_SLACK) */
#define LZ_SLACK(p) (((((p) >> 8) + 32u + 63u) & ~63u) + 64u)
/* raw bytes above which a compressed run would not fit one 64 KB page with its slack: stored raw */
#define LZ_GRP_CAP (65536u - LZ_SLACK(65536u))

#define MAPF_RESZ CHUNK_ID('M', 'R', 'E', 'S'), CHUNK_ID('R', 'E', 'S', 'Z'), 2048u
#define MAPF_CELLSZ CHUNK_ID('C', 'E', 'L', 'F'), CHUNK_ID('C', 'B', 'L', 'Z'), 2048u

/* check blob (lzidx.bin as read; size or CHUNK_SIZE_UNKNOWN) against a cell table of ncell entries. 1; 0 and a
 * "TEST error:" line when it is not (chunk_refusals counts them) */
int mslz_open(MsLz *z, const void *blob, unsigned size, unsigned ncell, const char *name);
/* the run of cell table entry cell, whose blob is raw bytes (the cell table's bytes: the index does not repeat it).
 * 1; 0 for an empty entry (no bytes) or a cell past the table */
int mslz_cell(const MsLz *z, unsigned cell, unsigned raw, MsLzRun *out);
/* the run of the mapres.bin range [rel, rel + bytes), which must lie on frame edges. 1; 0 and a TEST error line when not */
int mslz_res(const MsLz *z, unsigned rel, unsigned bytes, MsLzRun *out);
/* 0 when every frame of the run is stored raw (the read then lands at the slot's start and needs no decode) */
int mslz_any_packed(const MsLz *z, const MsLzRun *r);
/* frames [*done, r->frames) of run r (at most max_frames of them; *done advances): the run's bytes were read at
 * slot + at, the output goes to slot (mapres: frame 0 of the run at slot + 0). 1 all done, 0 more left, -1 corrupt (a TEST
 * error line): every frame must decode to exactly its raw size (a near part: r->raw, from mslz_cell). */
int mslz_decode(const MsLz *z, int res, const MsLzRun *r, unsigned char *slot, unsigned at, unsigned *done,
                unsigned max_frames);
/* src/pack.c (the one LZ4 block decoder: s, n bytes in; dst, cap bytes out; bytes written or -1) */
int pack_lz4_decode(const unsigned char *s, unsigned n, unsigned char *dst, unsigned cap);

#endif
