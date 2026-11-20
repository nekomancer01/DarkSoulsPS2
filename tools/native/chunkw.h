/* Chunk stream writer for the C tools (Polaris 3.3): pvs_bake, libmapx. Header-only; the format is at the top of
 * ps2/proto/src/chunk.h and tools/chunkfile.py (the Python writer the exporters use). Output is byte-identical to
 * tools/chunkfile.py pack() for the same chunks.
 *
 *   ChunkW w; chunkw_begin(&w, "PVS ");
 *   chunkw_start(&w, "PHDR", 1); chunkw_put(&w, hdr, sizeof hdr); chunkw_end(&w);
 *   ...
 *   chunkw_finish(&w);  then w.buf / w.len (malloc'd; free(w.buf)), or chunkw_save(&w, path)
 * A failed allocation sets w.err; every call after it does nothing and chunkw_save returns 0. */
#ifndef CHUNKW_H
#define CHUNKW_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *buf;
    size_t len, cap, open;  /* open: offset of the chunk being written + 1 (0 none) */
    unsigned count;
    int err;
} ChunkW;

static void chunkw_u32(unsigned char *p, unsigned v)
{
    p[0] = v & 0xFF, p[1] = v >> 8 & 0xFF, p[2] = v >> 16 & 0xFF, p[3] = v >> 24 & 0xFF;
}

static void chunkw_put(ChunkW *w, const void *data, size_t n)
{
    if (w->err || !n)
        return;
    if (w->len + n > w->cap) {
        size_t cap = w->cap ? w->cap : 4096;
        while (cap < w->len + n)
            cap *= 2;
        unsigned char *b = realloc(w->buf, cap);
        if (!b) {
            w->err = 1;
            return;
        }
        w->buf = b, w->cap = cap;
    }
    if (data)
        memcpy(w->buf + w->len, data, n);
    else
        memset(w->buf + w->len, 0, n);
    w->len += n;
}

static void chunkw_begin(ChunkW *w, const char id[4])
{
    memset(w, 0, sizeof *w);
    unsigned char h[16] = {'C', 'H', 'N', 'K'};
    memcpy(h + 4, id, 4);
    chunkw_put(w, h, 16);
}

static void chunkw_start(ChunkW *w, const char id[4], unsigned version)
{
    unsigned char h[16] = {0};
    memcpy(h, id, 4);
    h[4] = version & 0xFF, h[5] = version >> 8 & 0xFF;
    w->open = w->len + 1;
    chunkw_put(w, h, 16);
}

static void chunkw_end(ChunkW *w)
{
    if (w->err || !w->open)
        return;
    size_t at = w->open - 1, n = w->len - at - 16;
    chunkw_u32(w->buf + at + 8, (unsigned)n);
    chunkw_put(w, NULL, (16 - n % 16) % 16);
    w->open = 0, w->count++;
}

static void chunkw_finish(ChunkW *w)
{
    unsigned char f[16] = {'C', 'E', 'N', 'D'};
    chunkw_end(w);
    chunkw_put(w, f, 16);
    if (w->err)
        return;
    chunkw_u32(w->buf + 8, (unsigned)w->len);
    chunkw_u32(w->buf + 12, w->count);
    chunkw_u32(w->buf + w->len - 12, (unsigned)w->len);
}

static int chunkw_save(ChunkW *w, const char *path)
{
    FILE *f;
    if (w->err || !(f = fopen(path, "wb")))
        return 0;
    int ok = fwrite(w->buf, 1, w->len, f) == w->len;
    return fclose(f) == 0 && ok;
}

#endif
