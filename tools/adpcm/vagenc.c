/* SPU2 ADPCM (PS1/PS2 "VAG" block format) encoder and decoder for tools/adpcm/adpcm.py.
 * A 16-byte block holds 28 samples: byte 0 = shift (low nibble, 0..12) | filter << 4 (0..4), byte 1 = flags
 * (1 = loop end, 2 = repeat, 4 = loop start), 14 bytes of 4-bit signed residuals, low nibble first.
 * Decode: s = (r << 12 >> shift) + (s1 * f0 + s2 * f1 + 32) / 64, clamped to 16 bit, with the SPU's filter table.
 * The encoder tries every filter and shift per block against the real decoder state and keeps the one with the
 * least squared error, so encoder and decoder never drift.
 * Build: gcc -O2 -shared -fPIC -o libvagenc.so vagenc.c */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static const int F0[5] = {0, 60, 115, 98, 122};
static const int F1[5] = {0, 0, -52, -55, -60};

static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

/* Encodes one block for a given filter/shift; returns squared error, writes nibbles and the end state */
static int64_t try_block(const int16_t *x, int filt, int shift, int *s1, int *s2, uint8_t *nib)
{
    int a = *s1, b = *s2;
    int64_t err = 0;
    for (int i = 0; i < 28; i++) {
        int pred = (a * F0[filt] + b * F1[filt] + 32) >> 6;
        int d = x[i] - pred;
        /* residual r with (r << 12) >> shift ~ d, rounded */
        int r = shift <= 12 ? ((d << shift) + (d >= 0 ? 2048 : -2048)) / 4096 : 0;
        if (r > 7) r = 7;
        if (r < -8) r = -8;
        int v = clamp16(((r << 12) >> shift) + pred);
        int64_t e = (int64_t)(x[i] - v);
        err += e * e;
        nib[i] = (uint8_t)(r & 15);
        b = a, a = v;
    }
    *s1 = a, *s2 = b;
    return err;
}

/* Encodes n mono samples into blocks; loop_start < 0 = one-shot (the last block gets flag 1 = end + mute, and a
 * silent block follows as the SPU needs), else the stream loops back to the block holding loop_start.
 * Returns bytes written (out needs (n / 28 + 2) * 16). */
size_t vag_encode(const int16_t *pcm, size_t n, uint8_t *out, long loop_start)
{
    size_t nb = (n + 27) / 28;
    int s1 = 0, s2 = 0;
    int16_t x[28];
    uint8_t *o = out;
    long lb = loop_start >= 0 ? loop_start / 28 : -1;
    for (size_t k = 0; k < nb; k++) {
        size_t m = n - k * 28 < 28 ? n - k * 28 : 28;
        memset(x, 0, sizeof x);
        memcpy(x, pcm + k * 28, m * 2);
        int64_t best = INT64_MAX;
        int bf = 0, bs = 0, bs1 = 0, bs2 = 0;
        uint8_t nib[28], bnib[28];
        for (int f = 0; f < 5; f++)
            for (int s = 0; s <= 12; s++) {
                int t1 = s1, t2 = s2;
                int64_t e = try_block(x, f, s, &t1, &t2, nib);
                if (e < best)
                    best = e, bf = f, bs = s, bs1 = t1, bs2 = t2, memcpy(bnib, nib, 28);
            }
        s1 = bs1, s2 = bs2;
        o[0] = (uint8_t)(bs | bf << 4);
        o[1] = 0;
        if (lb >= 0 && (long)k == lb)
            o[1] |= 4;
        if (k == nb - 1)
            o[1] |= lb >= 0 ? 3 : 1;
        else if (lb >= 0)
            o[1] |= 2;
        for (int i = 0; i < 14; i++)
            o[2 + i] = (uint8_t)(bnib[2 * i] | bnib[2 * i + 1] << 4);
        o += 16;
    }
    if (lb < 0) {  /* one-shot: trailing silent block with end flag */
        memset(o, 0, 16);
        o[1] = 7;
        o += 16;
    }
    return (size_t)(o - out);
}

/* Decodes nblocks blocks to 28 * nblocks samples (ignores loop flags); returns samples written */
size_t vag_decode(const uint8_t *in, size_t nblocks, int16_t *out)
{
    int s1 = 0, s2 = 0;
    for (size_t k = 0; k < nblocks; k++, in += 16) {
        int shift = in[0] & 15, f = (in[0] >> 4) & 7;
        if (f > 4)
            f = 0;
        for (int i = 0; i < 28; i++) {
            int r = (in[2 + i / 2] >> (4 * (i & 1))) & 15;
            r = (r ^ 8) - 8;
            int v = clamp16(((r << 12) >> shift) + ((s1 * F0[f] + s2 * F1[f] + 32) >> 6));
            *out++ = (int16_t)v;
            s2 = s1, s1 = v;
        }
    }
    return nblocks * 28;
}
