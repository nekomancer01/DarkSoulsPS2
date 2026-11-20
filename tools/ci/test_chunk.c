/* Host test of the chunk streams (Polaris 3.3; tools/ci/test_chunk.sh): the C writer (tools/native/chunkw.h) and the
 * runtime reader (ps2/proto/src/chunk.c), built 64-bit. Writes argv[1] for test_chunk.sh to compare with tools/chunkfile.py,
 * then checks every refusal: another version, a missing chunk, another file id, a pre-3.3 file, a truncated file (cut
 * mid-chunk and at a chunk boundary, known and unknown size), a chunk length past the end, a VU1 stamp mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chunk.h"
#include "vu1_layout.h"
#include "../native/chunkw.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static void *aligned_copy(const void *b, size_t n)
{
    void *p = aligned_alloc(16, (n + 15) & ~(size_t)15);
    memcpy(p, b, n);
    return p;
}

int main(int argc, char **argv)
{
    ChunkW w;
    const unsigned hdr[3] = {1, 2, 3}, stamp = VU1L_HASH;
    chunkw_begin(&w, "TEST");
    chunkw_start(&w, "HEAD", 2), chunkw_put(&w, hdr, sizeof hdr), chunkw_end(&w);
    chunkw_start(&w, "DATA", 1), chunkw_put(&w, "abcdefghijklmnopq", 17), chunkw_end(&w);
    chunkw_start(&w, "NONE", 7), chunkw_end(&w);
    chunkw_start(&w, "VU1L", 1), chunkw_put(&w, &stamp, 4), chunkw_end(&w);
    chunkw_finish(&w);
    if (w.err)
        return printf("FAIL: chunkw allocation\n"), 1;
    if (argc > 1 && !chunkw_save(&w, argv[1]))
        return printf("FAIL: chunkw_save %s\n", argv[1]), 1;
    unsigned char *b = aligned_copy(w.buf, w.len);
    unsigned n = (unsigned)w.len, sz;
    ChunkFile f;

    CHECK(chunk_open(&f, b, n, CHUNK_ID('T', 'E', 'S', 'T'), "test.bin") && f.n == 4, "open a good file");
    const unsigned *h = chunk_get(&f, CHUNK_ID('H', 'E', 'A', 'D'), 2, &sz);
    CHECK(h && sz == 12 && h[2] == 3 && ((size_t)h & 15) == 0, "HEAD v2 in place, 16-byte aligned");
    const char *d = chunk_get(&f, CHUNK_ID('D', 'A', 'T', 'A'), 1, &sz);
    CHECK(d && sz == 17 && !memcmp(d, "abcdefghijklmnopq", 17), "DATA v1");
    CHECK(chunk_get(&f, CHUNK_ID('N', 'O', 'N', 'E'), 7, &sz) && sz == 0, "empty chunk");
    CHECK(chunk_open(&f, b, CHUNK_SIZE_UNKNOWN, CHUNK_ID('T', 'E', 'S', 'T'), "test.bin"), "open, size unknown");
    CHECK(chunk_vu1_check(&f), "VU1 stamp matches");
    CHECK(chunk_refusals == 0, "no refusal so far");

    printf("-- expected refusals:\n");
    unsigned r = chunk_refusals;
    CHECK(!chunk_get(&f, CHUNK_ID('H', 'E', 'A', 'D'), 3, NULL) && chunk_refusals == ++r, "bumped version refused");
    CHECK(!chunk_get(&f, CHUNK_ID('M', 'I', 'S', 'S'), 1, NULL) && chunk_refusals == ++r, "missing chunk refused");
    CHECK(!chunk_opt(&f, CHUNK_ID('M', 'I', 'S', 'S'), 1, &sz) && sz == 0 && chunk_refusals == r, "missing optional: no error");
    CHECK(!chunk_opt(&f, CHUNK_ID('D', 'A', 'T', 'A'), 2, NULL) && chunk_refusals == ++r, "optional of another version refused");
    CHECK(!chunk_open(&f, b, n, CHUNK_ID('W', 'R', 'L', 'D'), "test.bin") && chunk_refusals == ++r, "other file id refused");
    CHECK(!chunk_open(&f, "DSW1\5\0\0\0", 8, CHUNK_ID('T', 'E', 'S', 'T'), "old.bin") && chunk_refusals == ++r,
          "pre-3.3 file refused");
    CHECK(!chunk_open(&f, b, n - 40, CHUNK_ID('T', 'E', 'S', 'T'), "cut.bin") && chunk_refusals == ++r, "truncated mid-chunk");
    CHECK(!chunk_open(&f, b, n - 16, CHUNK_ID('T', 'E', 'S', 'T'), "cut.bin") && chunk_refusals == ++r, "truncated at the footer");
    {   /* cut at a chunk boundary, size unknown: the bytes after the cut are not a footer */
        unsigned char *c = aligned_copy(b, n);
        memset(c + n - 48, 0, 48);
        CHECK(!chunk_open(&f, c, CHUNK_SIZE_UNKNOWN, CHUNK_ID('T', 'E', 'S', 'T'), "cut.bin") && chunk_refusals == ++r,
              "truncated, size unknown");
        memcpy(c, b, n);
        c[16 + 8] = 0xFF;  /* HEAD's length past the end */
        CHECK(!chunk_open(&f, c, n, CHUNK_ID('T', 'E', 'S', 'T'), "bad.bin") && chunk_refusals == ++r, "chunk past the end");
        memcpy(c, b, n);
        c[n - 16 - 16 + 0] ^= 1;  /* the stamp */
        CHECK(chunk_open(&f, c, n, CHUNK_ID('T', 'E', 'S', 'T'), "vu1.bin") && !chunk_vu1_check(&f) && chunk_refusals == ++r,
              "VU1 stamp mismatch refused");
        free(c);
    }
    CHECK(!chunk_open(&f, NULL, 0, CHUNK_ID('T', 'E', 'S', 'T'), "none") && chunk_refusals == r, "no file: no message");
    free(b), free(w.buf);
    printf("test_chunk: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
