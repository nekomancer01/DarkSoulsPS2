/* map.bin and the files split from it as chunk files (Polaris 4.8 step 4, stage 1; tools/mapwrap.py): a VU1L chunk and
 * one payload chunk holding the exporters' bytes unchanged, the payload at a fixed offset (MAPF_AT_*), so every offset
 * inside stays relative to the payload's start and the loaders take it as their base. Stage 2 (sections with their own
 * versions) is TECH_DEBT core-1's follow-up.
 * MAP_CHUNKS (default 1; make MAP_CHUNKS=0 with packs built the same way): 0 = the files as the exporters write them. */
#ifndef MAPFILE_H
#define MAPFILE_H

#include "pack.h"
#include "chunk.h"

#ifndef MAP_CHUNKS
#define MAP_CHUNKS 1
#endif

/* file id, payload chunk id and the payload's offset in the file, per file (tools/mapwrap.py KINDS) */
#define MAPF_MAP CHUNK_ID('M', 'A', 'P', 'F'), CHUNK_ID('M', 'A', 'P', 'D'), 128u
#define MAPF_TAB CHUNK_ID('M', 'T', 'A', 'B'), CHUNK_ID('T', 'A', 'B', 'D'), 2048u
#define MAPF_RES CHUNK_ID('M', 'R', 'E', 'S'), CHUNK_ID('R', 'E', 'S', 'D'), 2048u
#define MAPF_TEX CHUNK_ID('M', 'T', 'E', 'X'), CHUNK_ID('T', 'E', 'X', 'D'), 2048u
#define MAPF_CELLS CHUNK_ID('C', 'E', 'L', 'F'), CHUNK_ID('C', 'B', 'L', 'B'), 2048u
#define MAPF_VERSION 1

/* a raw pack entry's payload range (pack_entry_raw, then past the wrapper: offset + at, size less the wrapper; no
 * read: the entry must have passed mapf_check once, world.c world_init / mapstream.c mapstream_open). 0 when the entry
 * is missing or not raw. */
int mapf_entry(Pack *pk, const char *name, unsigned fid, unsigned pid, unsigned at, unsigned *off, unsigned *size);
/* boot / block-open check of a raw entry (reads its first `at` bytes and waits for the disc: never per frame): a chunk file of fid with pid at version
 * MAPF_VERSION starting at `at` and this ELF's VU1 layout; 0 and a "TEST error: chunk:" line when not. 1 for a
 * missing entry (the callers say what is missing). With MAP_CHUNKS 0: 0 for a chunk file (a pack built wrapped). */
int mapf_check(Pack *pk, const char *name, unsigned fid, unsigned pid, unsigned at);
/* a whole file in memory (pack_load): its payload (and its byte count in *psize); NULL and a TEST error line when it is
 * not a chunk file of fid. With MAP_CHUNKS 0: blob itself. */
const void *mapf_payload(const void *blob, unsigned size, const char *name, unsigned fid, unsigned pid, unsigned at,
                         unsigned *psize);

#endif
