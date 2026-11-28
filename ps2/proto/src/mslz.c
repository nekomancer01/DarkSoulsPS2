/* LZ4 frames for the streamed map items: index reader and in-place decoder (mslz.h) */
#include <stdio.h>
#include <string.h>
#include "mslz.h"

#define RAW_FLAG 0x80000000u

static int bad(const MsLz *z, const char *fmt, unsigned a, unsigned b)
{
    chunk_refusals++;
    printf("TEST error: lz4: %s: ", z->name ? z->name : "?");
    printf(fmt, a, b);
    printf(" (re-export it with this build's tools)\n");
    return 0;
}

int mslz_open(MsLz *z, const void *blob, unsigned size, unsigned ncell, const char *name)
{
    ChunkFile f;
    unsigned hs = 0, cs = 0, rs = 0;
    memset(z, 0, sizeof *z);
    if (!chunk_open(&f, blob, size, CHUNK_ID('L', 'Z', 'I', 'X'), name))
        return 0;
    const unsigned *hd = chunk_get(&f, CHUNK_ID('L', 'Z', 'H', 'D'), 1, &hs);
    const unsigned *ce = hd ? chunk_get(&f, CHUNK_ID('C', 'E', 'L', 'I'), 1, &cs) : NULL;
    const unsigned *rf = ce ? chunk_get(&f, CHUNK_ID('R', 'E', 'S', 'F'), 1, &rs) : NULL;
    if (!rf)
        return 0;
    MsLz t = {f.base, name, 0, 0, 0, 0, 0, ce, rf};
    if (hs < 32)
        return bad(&t, "header chunk has %u bytes, expected %u", hs, 32);
    t.ncell = hd[0], t.nres = hd[1], t.cbytes = hd[2], t.rbytes = hd[3], t.rraw = hd[4];
    if (t.ncell != ncell)
        return bad(&t, "index has %u cells, cell table %u", t.ncell, ncell);
    if (cs != 8u * t.ncell || rs != 8u * t.nres)
        return bad(&t, "index chunks have %u / %u bytes, header counts disagree", cs, rs);
    for (unsigned i = 0; i < t.nres; i++) {
        unsigned rel = rf[2 * i], d = rf[2 * i + 1] & ~RAW_FLAG;
        if (i ? rel <= rf[2 * i - 2] : rel != 0)
            return bad(&t, "frame %u rel not ascending%.0u", i, 0);
        if (rel >= t.rraw)
            return bad(&t, "frame %u rel not ascending (past the %u raw bytes)", i, t.rraw);
        if (d >= t.rbytes || (i && d < (rf[2 * i - 1] & ~RAW_FLAG)))
            return bad(&t, "frame %u past payload (%u bytes)", i, t.rbytes);
    }
    for (unsigned i = 0; i < t.ncell; i++) {
        unsigned d = ce[2 * i], pk = ce[2 * i + 1] & ~RAW_FLAG;
        if (d > t.cbytes || pk > t.cbytes - d)
            return bad(&t, "cell %u past payload (%u bytes)", i, t.cbytes);
    }
    *z = t;
    return 1;
}

int mslz_cell(const MsLz *z, unsigned cell, unsigned raw, MsLzRun *out)
{
    if (cell >= z->ncell || !(z->celi[2 * cell + 1] & ~RAW_FLAG) || !raw)
        return 0;
    out->off = z->celi[2 * cell], out->packed = z->celi[2 * cell + 1] & ~RAW_FLAG;
    out->frame0 = cell | MSLZ_CELL, out->frames = 1, out->raw = raw;
    return 1;
}

static unsigned frame_rel(const MsLz *z, unsigned i)
{
    return i < z->nres ? z->resf[2 * i] : z->rraw;
}

static unsigned frame_disc(const MsLz *z, unsigned i)
{
    return i < z->nres ? z->resf[2 * i + 1] & ~RAW_FLAG : z->rbytes;
}

/* the frame starting at raw offset rel: its index, or nres when none does (rel == the raw bytes: the end) */
static int find_edge(const MsLz *z, unsigned rel, unsigned *at)
{
    unsigned lo = 0, hi = z->nres;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (z->resf[2 * m] < rel)
            lo = m + 1;
        else
            hi = m;
    }
    *at = lo;
    return lo < z->nres ? z->resf[2 * lo] == rel : rel == z->rraw;
}

int mslz_res(const MsLz *z, unsigned rel, unsigned bytes, MsLzRun *out)
{
    unsigned a, e;
    if (!bytes || !find_edge(z, rel, &a) || a >= z->nres || rel + bytes < rel || !find_edge(z, rel + bytes, &e) || e <= a) {
        chunk_refusals++;
        printf("TEST error: lz4: %s: item 0x%x+0x%x not on frame edges (re-export it with this build's tools)\n",
               z->name ? z->name : "?", rel, bytes);
        return 0;
    }
    out->off = frame_disc(z, a), out->packed = frame_disc(z, e) - out->off, out->frame0 = a, out->frames = e - a;
    out->raw = bytes;
    return 1;
}

int mslz_any_packed(const MsLz *z, const MsLzRun *r)
{
    if (r->frame0 & MSLZ_CELL)
        return !(z->celi[2 * (r->frame0 & ~MSLZ_CELL) + 1] & RAW_FLAG);
    for (unsigned i = 0; i < r->frames; i++)
        if (!(z->resf[2 * (r->frame0 + i) + 1] & RAW_FLAG))
            return 1;
    return 0;
}

int mslz_decode(const MsLz *z, int res, const MsLzRun *r, unsigned char *slot, unsigned at, unsigned *done,
                unsigned max_frames)
{
    if (!res) {  /* a near part: one frame of r->raw bytes (the cell table's size, mslz_cell) */
        if (*done >= r->frames)
            return 1;
        unsigned c = r->frame0 & ~MSLZ_CELL;
        const unsigned char *in = slot + at;
        if (z->celi[2 * c + 1] & RAW_FLAG ? r->packed != r->raw || (memmove(slot, in, r->packed), 0)
                                           : pack_lz4_decode(in, r->packed, slot, r->raw) != (int)r->raw)
            return bad(z, "cell %u corrupt%.0u", c, 0), -1;
        *done = r->frames;
        return 1;
    }
    unsigned rel0 = frame_rel(z, r->frame0);
    for (unsigned n = 0; *done < r->frames && n < max_frames; n++, ++*done) {
        unsigned i = r->frame0 + *done, d = frame_disc(z, i), pk = frame_disc(z, i + 1) - d;
        unsigned rel = frame_rel(z, i), raw = frame_rel(z, i + 1) - rel;
        const unsigned char *in = slot + at + (d - r->off);
        unsigned char *out = slot + (rel - rel0);
        if (z->resf[2 * i + 1] & RAW_FLAG ? pk != raw || (memmove(out, in, pk), 0)
                                           : pack_lz4_decode(in, pk, out, raw) != (int)raw)
            return bad(z, "frame %u corrupt%.0u", i, 0), -1;
    }
    return *done >= r->frames;
}
