/* Host test of the streamed items' LZ4 reader and in-place decoder (tools/ci/test_mslz.sh; ps2/proto/src/mslz.c), built 64-bit.
 * argv[1] is a directory test_mslz.sh filled: lzidx.bin, mapres.lz.bin, cells.lz.bin, the raw mapres.bin and cells.disc.bin and
 * manifest.txt (name, "cells N", "cell i off bytes", "item rel bytes", "grp rel bytes"). Every cell table entry, item and group
 * is decoded in a slot sized as the streamer sizes it (raw + LZ_SLACK(packed), the read at the slot's end, 64-aligned; all
 * raw: raw bytes, read at the start) one frame per call, and compared with the raw file; then every refusal. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mslz.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static unsigned char *load(const char *dir, const char *fn, unsigned *n)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, fn);
    FILE *f = fopen(p, "rb");
    if (!f)
        return printf("FAIL: cannot open %s\n", p), exit(1), NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    unsigned char *b = aligned_alloc(64, ((size_t)len + 64) & ~(size_t)63);
    if (fread(b, 1, len, f) != (size_t)len)
        exit(1);
    fclose(f);
    *n = (unsigned)len;
    return b;
}

#define GUARD 128
/* decode run r (raw bytes) of payload into a guarded slot and compare with want; returns frames it took (calls) */
static int one(const MsLz *z, int res, const MsLzRun *r, const unsigned char *payload, unsigned raw, const unsigned char *want,
               unsigned per, const char *what)
{
    int packed = mslz_any_packed(z, r);
    unsigned slot_bytes = packed ? raw + LZ_SLACK(r->packed) : raw, at = packed ? (slot_bytes - r->packed) & ~63u : 0;
    unsigned char *mem = malloc(slot_bytes + 2 * GUARD + 64), *slot = mem + GUARD;
    memset(mem, 0xEE, slot_bytes + 2 * GUARD + 64);
    memcpy(slot + at, payload + r->off, r->packed);
    unsigned done = 0, calls = 0;
    int rc;
    while ((rc = mslz_decode(z, res, r, slot, at, &done, per)) == 0 && ++calls < 100000)
        ;
    int ok = rc == 1 && done == r->frames && !memcmp(slot, want, raw);
    for (unsigned i = 1; i <= GUARD; i++)
        ok &= mem[GUARD + slot_bytes - 1 + i] == 0xEE && mem[GUARD - i] == 0xEE;
    if (!ok)
        printf("FAIL: %s (rc %d, %u/%u frames, packed %u raw %u, slot %u)\n", what, rc, done, r->frames, r->packed, raw, slot_bytes), fails++;
    free(mem);
    return ok;
}

int main(int argc, char **argv)
{
    unsigned nidx, nres_lz, ncell_lz, nres, ncells_raw, ncell = 0, line = 0, packed_runs = 0, raw_runs = 0, runs = 0;
    MsLzRun short_cell = {0};  /* a packed near part seen, for the wrong-size refusal */
    unsigned short_want = 0;
    if (argc < 2)
        return printf("usage: test_mslz <dir>\n"), 1;
    unsigned char *idx = load(argv[1], "lzidx.bin", &nidx), *rlz = load(argv[1], "mapres.lz.bin", &nres_lz);
    unsigned char *clz = load(argv[1], "cells.lz.bin", &ncell_lz), *res = load(argv[1], "mapres.bin", &nres);
    unsigned char *cells = load(argv[1], "cells.disc.bin", &ncells_raw);
    char buf[512], name[256] = "?";
    snprintf(buf, sizeof buf, "%s/manifest.txt", argv[1]);
    FILE *mf = fopen(buf, "r");
    if (!mf)
        return printf("FAIL: no manifest\n"), 1;
    unsigned a, b, c;
    while (fgets(buf, sizeof buf, mf)) {  /* first pass: the cell count */
        if (!line++)
            sscanf(buf, "%255s", name);
        else if (sscanf(buf, "cells %u", &ncell) == 1)
            break;
    }
    MsLz z;
    CHECK(mslz_open(&z, idx, nidx, ncell, "lzidx.bin") && chunk_refusals == 0, "open the index");
    CHECK(z.ncell == ncell && z.nres > 0 && z.rraw == nres && z.rbytes == nres_lz && z.cbytes == ncell_lz, "LZHD sizes");
    for (unsigned per = 1; per <= 2; per++) {
        rewind(mf);
        while (fgets(buf, sizeof buf, mf)) {
            MsLzRun r;
            char w[400];
            if (sscanf(buf, "cell %u %u %u", &a, &b, &c) == 3) {  /* entry a: bytes at b of cells.disc.bin, c of them */
                if (!c) {
                    CHECK(!mslz_cell(&z, a, 0, &r), "empty entry: no run");
                    continue;
                }
                snprintf(w, sizeof w, "%s: cell %u (%u B) per %u", name, a, c, per);
                if (!mslz_cell(&z, a, c, &r))
                    printf("FAIL: %s: no run\n", w), fails++;
                else if (one(&z, 0, &r, clz, c, cells + b, per, w) && per == 1) {
                    runs++, mslz_any_packed(&z, &r) ? packed_runs++ : raw_runs++;
                    if (!short_cell.raw && c > 64 && mslz_any_packed(&z, &r))  /* kept for the wrong-size refusal below */
                        short_cell = r, short_want = c;
                }
            } else if (sscanf(buf, "item %u %u", &a, &b) == 2 || sscanf(buf, "grp %u %u", &a, &b) == 2) {
                snprintf(w, sizeof w, "%s: %.4s 0x%x+0x%x per %u", name, buf, a, b, per);
                if (!mslz_res(&z, a, b, &r))
                    printf("FAIL: %s: no run\n", w), fails++;
                else if (one(&z, 1, &r, rlz, b, res + a, per, w) && per == 1)
                    runs++, mslz_any_packed(&z, &r) ? packed_runs++ : raw_runs++;
            }
        }
    }
    fclose(mf);
    printf("%s: %u runs decoded in place (%u packed, %u stored raw)\n", name, runs, packed_runs, raw_runs);
    CHECK(packed_runs && (raw_runs || strcmp(name, "fixture")), "both packed and raw runs");

    printf("-- expected refusals:\n");
    unsigned r0 = chunk_refusals;
    MsLzRun run;
    unsigned dummy = 0;
    CHECK(!mslz_open(&z, idx, nidx, ncell + 1, "lzidx.bin") && chunk_refusals == ++r0, "wrong cell count refused");
    r0 = chunk_refusals;
    CHECK(!mslz_open(&z, res, nres, ncell, "mapres.bin") && chunk_refusals == ++r0, "not a chunk file refused");
    CHECK(!mslz_open(&z, idx, nidx - 16, ncell, "cut.bin") && chunk_refusals == ++r0, "truncated index refused");
    mslz_open(&z, idx, nidx, ncell, "lzidx.bin");
    r0 = chunk_refusals;
    CHECK(!mslz_res(&z, 1, 64, &run) && chunk_refusals == ++r0, "start off a frame edge refused");
    CHECK(!mslz_res(&z, 0, z.resf[2] - 1, &run) && chunk_refusals == ++r0, "end off a frame edge refused");
    CHECK(!mslz_res(&z, z.rraw, 16, &run) && chunk_refusals == ++r0, "past the raw bytes refused");
    CHECK(!mslz_res(&z, 0, 0, &run) && chunk_refusals == ++r0, "empty item refused");
    CHECK(mslz_res(&z, 0, z.rraw, &run) && run.frames == z.nres && run.packed == z.rbytes, "the whole file is one run");
    CHECK(!mslz_cell(&z, ncell, 1, &run) && !mslz_cell(&z, ~0u, 1, &run), "cell past the table: no run");
    if (short_cell.raw) {  /* a packed near part decoded with a raw size the cell table does not have: refused */
        MsLzRun w = short_cell;
        w.raw = short_want - 16;
        unsigned slot_bytes = short_want + LZ_SLACK(w.packed), at = (slot_bytes - w.packed) & ~63u, d = 0;
        unsigned char *s = calloc(1, slot_bytes + 64);
        memcpy(s + at, clz + w.off, w.packed);
        r0 = chunk_refusals;
        CHECK(mslz_decode(&z, 0, &w, s, at, &d, 1) == -1 && chunk_refusals == ++r0, "near part of the wrong raw size refused");
        free(s);
    }

    /* a corrupt frame: a packed frame marked raw, a raw frame marked packed */
    unsigned char *bad = malloc(nidx + 64);
    unsigned char *copy = (unsigned char *)(((size_t)bad + 15) & ~(size_t)15);
    memcpy(copy, idx, nidx);
    MsLz y;
    CHECK(mslz_open(&y, copy, nidx, ncell, "copy.bin"), "open the copy");
    unsigned *rf = (unsigned *)(copy + ((const unsigned char *)z.resf - idx));
    unsigned pk = z.nres;
    for (unsigned i = 0; i < z.nres; i++)
        if (!(rf[2 * i + 1] & 0x80000000u)) {
            pk = i;
            break;
        }
    if (pk < z.nres) {
        MsLzRun fr = {0, 0, pk, 1};
        unsigned char *slot = calloc(1, 2 << 20);
        rf[2 * pk + 1] |= 0x80000000u;
        r0 = chunk_refusals;
        CHECK(mslz_decode(&y, 1, &fr, slot, 0, &dummy, 1) == -1 && chunk_refusals == ++r0, "packed frame flagged raw: corrupt");
        rf[2 * pk + 1] &= ~0x80000000u;
        dummy = 0;
        memset(slot, 0xFF, 2 << 20);  /* packed bytes that are no LZ4 of the frame's size */
        fr.off = z.resf[2 * pk + 1] & ~0x80000000u;
        CHECK(mslz_decode(&y, 1, &fr, slot, 0, &dummy, 1) == -1 && chunk_refusals == ++r0, "garbage frame: corrupt");
        free(slot);
    } else
        printf("(no packed frame in this block: corrupt-frame checks skipped)\n");
    /* index field checks: rel not ascending, frame 0 not at 0, a disc offset past the payload */
    if (z.nres > 1) {
        r0 = chunk_refusals;
        unsigned s1 = rf[2], s0 = rf[0], d1 = rf[3];
        rf[2] = 0;
        CHECK(!mslz_open(&y, copy, nidx, ncell, "copy.bin") && chunk_refusals == ++r0, "rel not ascending refused");
        rf[2] = s1, rf[0] = 16;
        CHECK(!mslz_open(&y, copy, nidx, ncell, "copy.bin") && chunk_refusals == ++r0, "frame 0 not at 0 refused");
        rf[0] = s0, rf[3] = z.rbytes + 64;
        CHECK(!mslz_open(&y, copy, nidx, ncell, "copy.bin") && chunk_refusals == ++r0, "frame past the payload refused");
        rf[3] = d1;
        CHECK(mslz_open(&y, copy, nidx, ncell, "copy.bin"), "restored copy opens");
    }
    free(bad);
    if (fails)
        printf("test_mslz: %d failed\n", fails);
    else
        printf("test_mslz: %s ok\n", name);
    return fails != 0;
}
