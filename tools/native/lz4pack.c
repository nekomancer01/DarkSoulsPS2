/* LZ4 block-format compressor for tools/pack.py (decompressed on the PS2 by ps2/proto/src/pack.c).
 * Greedy matcher with a 4-way bucketed hash table; output is plain LZ4 block format (no frame).
 * Build: gcc -O2 -shared -fPIC -o liblz4pack.so lz4pack.c */
#include <stdint.h>
#include <string.h>

#define HBITS 16
#define WAYS 4
#define MINMATCH 4
#define MAXOFF 65535

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t hash(uint32_t v) { return (v * 2654435761u) >> (32 - HBITS); }

static uint8_t *put_len(uint8_t *o, size_t n)
{
    while (n >= 255) {
        *o++ = 255;
        n -= 255;
    }
    *o++ = (uint8_t)n;
    return o;
}

static uint8_t *emit(uint8_t *o, const uint8_t *lit, size_t nlit, size_t off, size_t mlen)
{
    uint8_t *tok = o++;
    *tok = (uint8_t)((nlit >= 15 ? 15 : nlit) << 4);
    if (nlit >= 15)
        o = put_len(o, nlit - 15);
    memcpy(o, lit, nlit);
    o += nlit;
    if (mlen) {
        *o++ = (uint8_t)off;
        *o++ = (uint8_t)(off >> 8);
        size_t m = mlen - MINMATCH;
        *tok |= m >= 15 ? 15 : (uint8_t)m;
        if (m >= 15)
            o = put_len(o, m - 15);
    }
    return o;
}

/* Returns the compressed size, or 0 if it would not fit in cap (caller stores the block raw) */
size_t lz4_compress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    static uint32_t tab[1 << HBITS][WAYS];
    memset(tab, 0xFF, sizeof tab);
    uint8_t *o = dst, *end = dst + cap;
    size_t anchor = 0, i = 0;
    /* LZ4 rules: the last match starts >= 12 bytes before the end, the last 5 bytes are literals */
    size_t limit = n > 12 ? n - 12 : 0;
    while (i < limit) {
        uint32_t v = rd32(src + i), h = hash(v);
        size_t best = 0, boff = 0;
        for (int w = 0; w < WAYS; w++) {
            uint32_t c = tab[h][w];
            if (c == 0xFFFFFFFFu || i - c > MAXOFF || rd32(src + c) != v)
                continue;
            size_t l = MINMATCH;
            while (i + l < n - 5 && src[c + l] == src[i + l])
                l++;
            if (l > best)
                best = l, boff = i - c;
        }
        memmove(tab[h] + 1, tab[h], (WAYS - 1) * sizeof(uint32_t));
        tab[h][0] = (uint32_t)i;
        if (!best) {
            i++;
            continue;
        }
        if (o + (i - anchor) + (i - anchor) / 255 + 16 + best / 255 > end)
            return 0;
        o = emit(o, src + anchor, i - anchor, boff, best);
        for (size_t k = i + 1; k < i + best && k < limit; k += 2) {  /* index part of the match */
            uint32_t hk = hash(rd32(src + k));
            memmove(tab[hk] + 1, tab[hk], (WAYS - 1) * sizeof(uint32_t));
            tab[hk][0] = (uint32_t)k;
        }
        i += best;
        anchor = i;
    }
    if (o + (n - anchor) + (n - anchor) / 255 + 16 > end)
        return 0;
    o = emit(o, src + anchor, n - anchor, 0, 0);
    return (size_t)(o - dst);
}

/* Reference decoder (same algorithm as pack.c) for the round-trip check in pack.py; returns bytes written or -1 */
long lz4_decompress(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    const uint8_t *s = src, *se = src + n;
    uint8_t *o = dst, *oe = dst + cap;
    while (s < se) {
        uint8_t tok = *s++;
        size_t l = tok >> 4;
        if (l == 15) {
            uint8_t b;
            do { b = *s++; l += b; } while (b == 255);
        }
        if (o + l > oe || s + l > se)
            return -1;
        memcpy(o, s, l);
        o += l, s += l;
        if (s >= se)
            break;
        size_t off = s[0] | (s[1] << 8);
        s += 2;
        size_t m = (tok & 15);
        if (m == 15) {
            uint8_t b;
            do { b = *s++; m += b; } while (b == 255);
        }
        m += MINMATCH;
        if (off == 0 || o - dst < (long)off || o + m > oe)
            return -1;
        const uint8_t *r = o - off;
        while (m--)
            *o++ = *r++;
    }
    return (long)(o - dst);
}
