/* map.bin and the files split from it as chunk files: see mapfile.h */
#include <stdio.h>
#include <string.h>
#include "mapfile.h"

int mapf_entry(Pack *pk, const char *name, unsigned fid, unsigned pid, unsigned at, unsigned *off, unsigned *size)
{
    (void)fid, (void)pid;
    if (!pack_entry_raw(pk, name, off, size))
        return 0;
    if (MAP_CHUNKS && *size) {
        if (*size < at + 32) {  /* (a header-only file: nothing in it) */
            *size = 0;
            return 1;
        }
        *off += at, *size -= at + 16;  /* (the footer; the payload's 16-byte padding stays in: an upper bound) */
    }
    return 1;
}

int mapf_check(Pack *pk, const char *name, unsigned fid, unsigned pid, unsigned at)
{
    static unsigned char head[2048] __attribute__((aligned(64)));
    unsigned off, size, n;
    if (!MAP_CHUNKS || !pack_entry_raw(pk, name, &off, &size) || !size)
        return 1;
    if (at > sizeof head || size < at)
        return printf("TEST error: chunk: %s: %u bytes, too short for its header\n", name, size), 0;
    int r = 0, t;
    while (!(t = pack_req_read(pk, off, head, at)))
        pack_pump();
    while (!pack_req_done(t, &r))
        ;
    if (r != (int)at)
        return printf("TEST error: chunk: %s: header not read\n", name), 0;
    ChunkFile f;
    if (!chunk_open_part(&f, head, at, size, fid, name) || !chunk_vu1_check(&f))
        return 0;
    const void *p = chunk_get(&f, pid, MAPF_VERSION, &n);
    if (!p)
        return 0;
    if (chunk_offset(&f, p) != at)
        return printf("TEST error: chunk: %s: payload at %u, this build reads it at %u\n", name, chunk_offset(&f, p),
                      at), 0;
    return 1;
}

const void *mapf_payload(const void *blob, unsigned size, const char *name, unsigned fid, unsigned pid, unsigned at,
                         unsigned *psize)
{
    if (!MAP_CHUNKS || !blob) {
        if (psize)
            *psize = size;
        return blob;
    }
    ChunkFile f;
    if (!chunk_open(&f, blob, size, fid, name) || !chunk_vu1_check(&f))
        return NULL;
    unsigned n;
    const void *p = chunk_get(&f, pid, MAPF_VERSION, &n);
    if (p && chunk_offset(&f, p) != at) {
        printf("TEST error: chunk: %s: payload at %u, this build reads it at %u\n", name, chunk_offset(&f, p), at);
        return NULL;
    }
    if (p && psize)
        *psize = n;
    return p;
}
