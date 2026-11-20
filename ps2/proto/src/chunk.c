/* Chunk streams (chunk.h; format at the top of chunk.h and tools/chunkfile.py) */
#include <stdio.h>
#include <string.h>
#include "chunk.h"
#include "vu1_layout.h"

unsigned chunk_refusals;

static unsigned rd(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

const char *chunk_name(unsigned id, char *buf)
{
    for (int i = 0; i < 4; i++) {
        unsigned char c = id >> (8 * i) & 0xFF;
        buf[i] = c >= 32 && c < 127 ? (char)c : '?';
    }
    buf[4] = 0;
    return buf;
}

static int refuse(ChunkFile *f, const char *name, const char *why, unsigned a, unsigned b)
{
    chunk_refusals++;
    printf("TEST error: chunk: %s: ", name ? name : "?");
    printf(why, a, b);
    printf(" (re-export it with this build's tools)\n");
    if (f)
        memset(f, 0, sizeof *f);
    return 0;
}

int chunk_open(ChunkFile *f, const void *blob, unsigned size, unsigned file_id, const char *name)
{
    const unsigned char *b = blob;
    char n1[5], n2[5];
    memset(f, 0, sizeof *f);
    if (!b)
        return 0;
    if (size != CHUNK_SIZE_UNKNOWN && size < 2 * CHUNK_HDR)
        return refuse(f, name, "truncated (%u bytes, a chunk file has at least %u)", size, 2 * CHUNK_HDR);
    if (rd(b) != CHUNK_MAGIC) {
        chunk_refusals++;
        printf("TEST error: chunk: %s: not a chunk file (starts %s; an export from before Polaris 3.3?) "
               "(re-export it with this build's tools)\n", name ? name : "?", chunk_name(rd(b), n1));
        return 0;
    }
    if (rd(b + 4) != file_id) {
        chunk_refusals++;
        printf("TEST error: chunk: %s: file id %s, this build reads %s\n", name ? name : "?", chunk_name(rd(b + 4), n1),
               chunk_name(file_id, n2));
        return 0;
    }
    unsigned total = rd(b + 8), count = rd(b + 12);
    if (total < 2 * CHUNK_HDR || total & 15)
        return refuse(f, name, "bad total %u bytes%.0u", total, 0);
    if (size != CHUNK_SIZE_UNKNOWN && total > size)
        return refuse(f, name, "truncated (%u of %u bytes)", size, total);
    if (rd(b + total - CHUNK_HDR) != CHUNK_END || rd(b + total - CHUNK_HDR + 4) != total)
        return refuse(f, name, "truncated (no footer at %u bytes)%.0u", total, 0);
    /* every chunk inside, the count right */
    unsigned o = CHUNK_HDR, k = 0;
    while (o < total - CHUNK_HDR) {
        unsigned len = rd(b + o + 8);
        if (len > total - CHUNK_HDR - o - CHUNK_HDR)
            return refuse(f, name, "chunk %u runs past the end (%u bytes)", k, len);
        o += CHUNK_HDR + ((len + 15) & ~15u), k++;
    }
    if (o != total - CHUNK_HDR || k != count)
        return refuse(f, name, "%u chunks found, header says %u", k, count);
    f->base = b, f->name = name, f->file_id = file_id, f->size = total, f->n = count;
    return 1;
}

static const void *find(const ChunkFile *f, unsigned id, unsigned version, unsigned *size, int required)
{
    char n1[5];
    if (size)
        *size = 0;
    if (!f || !f->base)
        return NULL;
    for (unsigned o = CHUNK_HDR, k = 0; k < f->n; k++) {
        const unsigned char *c = f->base + o;
        unsigned len = rd(c + 8), ver = (unsigned)c[4] | (unsigned)c[5] << 8;
        if (rd(c) == id) {
            if (ver != version) {
                chunk_refusals++;
                printf("TEST error: chunk: %s: chunk %s version %u, this build reads version %u (re-export it with "
                       "this build's tools)\n", f->name ? f->name : "?", chunk_name(id, n1), ver, version);
                return NULL;
            }
            if (size)
                *size = len;
            return c + CHUNK_HDR;
        }
        o += CHUNK_HDR + ((len + 15) & ~15u);
    }
    if (required) {
        chunk_refusals++;
        printf("TEST error: chunk: %s: no chunk %s (version %u)\n", f->name ? f->name : "?", chunk_name(id, n1), version);
    }
    return NULL;
}

const void *chunk_get(const ChunkFile *f, unsigned id, unsigned version, unsigned *size)
{
    return find(f, id, version, size, 1);
}

const void *chunk_opt(const ChunkFile *f, unsigned id, unsigned version, unsigned *size)
{
    return find(f, id, version, size, 0);
}

int chunk_vu1_check_hash(const char *name, unsigned hash, unsigned mask)
{
    if ((hash & mask) == (VU1L_HASH & mask))
        return 1;
    chunk_refusals++;
    printf("TEST error: chunk: %s: exported for VU1 layout %08x, this ELF has %08x (re-export it)\n", name ? name : "?",
           hash & mask, VU1L_HASH & mask);
    return 0;
}

int chunk_vu1_check(const ChunkFile *f)
{
    unsigned n, r0 = chunk_refusals;
    const unsigned char *p = chunk_opt(f, CHUNK_VU1L, 1, &n);
    if (!p)
        return f && f->base && chunk_refusals == r0;  /* no stamp: nothing to check; another version: refused */
    return n >= 4 ? chunk_vu1_check_hash(f->name, rd(p), ~0u) : 0;
}
