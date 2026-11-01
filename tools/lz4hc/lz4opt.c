/* Optimal-parse LZ4 block compressor for tools/pack.py; drop-in for tools/native/lz4pack.c (same exported functions,
 * same plain LZ4 block format, so ps2/proto/src/pack.c decodes it unchanged).
 *
 * Match finder: hash chains over the whole block (blocks are <= 64 KB, so every position is in the window), depth
 * limited by OPT_DEPTH, stopping early at OPT_NICE bytes. Parse: backward dynamic programming over exact byte costs.
 *   F[i] = bytes to code src[i..n) when a new sequence (token) starts at i
 *        = 1 + min( (n-i) + llb(n-i)                          -- the rest as one literal run
 *                   min_{j>=i} (j-i) + llb(j-i) + G[j] )       -- j-i literals, then a match at j
 *   G[j] = min_{4<=M<=maxlen(j)} 2 + mlb(M) + F[j+M]
 * llb / mlb are the extra length bytes (0 below 15, then one per 255). The literal-run minimum is split in the ranges
 * where llb is constant (0..14, 15..269, then 255-wide steps) and kept with sliding-window minima, so the parse is
 * O(n * candidate lengths). Candidate lengths: every M up to OPT_ALLLEN, then maxlen and lengths where mlb changes.
 * Follows the LZ4 end rules (last 5 bytes literals, last match starts >= 12 bytes before the end), so any LZ4 decoder
 * accepts the output.
 * Build: gcc -O2 -shared -fPIC -o liblz4opt.so lz4opt.c */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MINMATCH 4
#define MAXBLOCK 65536
#define HBITS 15

static int opt_depth = 4096;  /* hash-chain steps per position */
static int opt_nice = 1024;   /* stop searching at a match this long */
#define OPT_ALLLEN 64

void lz4opt_set(int depth, int nice) { opt_depth = depth, opt_nice = nice; }

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t hash4(uint32_t v) { return (v * 2654435761u) >> (32 - HBITS); }
static int xlen(int n) { return n < 15 ? 0 : 1 + (n - 15) / 255; }

static uint8_t *put_len(uint8_t *o, size_t n)
{
    while (n >= 255)
        *o++ = 255, n -= 255;
    *o++ = (uint8_t)n;
    return o;
}

/* Sliding-window minimum of key[] over j in [lo, hi], queried with lo/hi decreasing (backward pass) */
typedef struct { int *q, h, t; } Mono;
static void mono_push(Mono *m, const int *key, int j)
{
    while (m->t > m->h && key[m->q[m->t - 1]] >= key[j])
        m->t--;
    m->q[m->t++] = j;
}
static int mono_min(Mono *m, const int *key, int hi)  /* drop entries > hi; returns index or -1 */
{
    while (m->t > m->h && m->q[m->h] > hi)
        m->h++;
    return m->t > m->h ? m->q[m->h] : -1;
}

size_t lz4_compress(const uint8_t *src, size_t n_, uint8_t *dst, size_t cap)
{
    int n = (int)n_;
    if (n_ > MAXBLOCK)
        return 0;
    static int head[1 << HBITS], prev[MAXBLOCK], mlen[MAXBLOCK + 1], moff[MAXBLOCK + 1];
    static int F[MAXBLOCK + 1], K[MAXBLOCK + 1], choiceM[MAXBLOCK + 1], choiceJ[MAXBLOCK + 1];
    static int q1[MAXBLOCK + 1], q2[MAXBLOCK + 1];
    const int INF = 1 << 29;
    int mflimit = n - 12, mend = n - 5;  /* match start < mflimit... end <= n-5 */

    /* 1. longest match per position */
    memset(head, 0xFF, sizeof head);
    for (int i = 0; i < n; i++) {
        mlen[i] = 0;
        if (i > mflimit || i + MINMATCH > n)
            continue;
        uint32_t v = rd32(src + i), h = hash4(v);
        int best = 0, boff = 0, c = head[h], steps = opt_depth;
        if (i > 0 && mlen[i - 1] > MINMATCH) {  /* continuation of the previous match: free lower bound */
            best = mlen[i - 1] - 1, boff = moff[i - 1];
            if (i + best > mend)
                best = mend - i;
            if (best < MINMATCH)
                best = 0;
        }
        while (c >= 0 && steps-- > 0 && best < opt_nice) {
            if (rd32(src + c) == v && src[c + best] == src[i + best]) {
                int l = MINMATCH;
                while (i + l < mend && src[c + l] == src[i + l])
                    l++;
                if (l > best)
                    best = l, boff = i - c;
            }
            c = prev[c];
        }
        if (best > mend - i)
            best = mend - i;
        if (best >= MINMATCH)
            mlen[i] = best, moff[i] = boff;
        prev[i] = head[h];
        head[h] = i;
    }

    /* 2. backward DP. K[j] = G[j] + j, so (j-i) + G[j] = K[j] - i */
    Mono w0 = {q1, 0, 0}, w1 = {q2, 0, 0};  /* w0: j in [i, i+14]; w1: j in [i+15, i+269] */
    int far_best = INF, far_j = -1;           /* j >= i+270, approximated as one step band (llb 2..) */
    F[n] = 1;                                 /* empty input still needs one token */
    K[n] = INF;
    choiceJ[n] = -1;
    for (int i = n - 1; i >= 0; i--) {
        /* G[i] */
        int g = INF, gm = 0;
        if (mlen[i] >= MINMATCH) {
            int L = mlen[i];
            int top = L < OPT_ALLLEN ? L : OPT_ALLLEN;
            for (int M = MINMATCH; M <= top; M++) {
                int c = 2 + xlen(M - MINMATCH) + F[i + M];
                if (c < g)
                    g = c, gm = M;
            }
            for (int M = L; M > top && M > L - 8; M--) {
                int c = 2 + xlen(M - MINMATCH) + F[i + M];
                if (c < g)
                    g = c, gm = M;
            }
            for (int M = 15 + MINMATCH + 255 - 1; M < L; M += 255) {  /* just below each mlb step */
                if (M > top) {
                    int c = 2 + xlen(M - MINMATCH) + F[i + M];
                    if (c < g)
                        g = c, gm = M;
                }
            }
        }
        choiceM[i] = gm;
        K[i] = g >= INF ? INF : g + i;
        /* windows: push i into w0; move i+15 from w0 range into w1; i+270 into far */
        mono_push(&w0, K, i);
        if (i + 15 <= n - 1)
            mono_push(&w1, K, i + 15);
        if (i + 270 <= n - 1) {
            int j = i + 270;
            if (K[j] < INF && K[j] + 1 < far_best)  /* approx: llb(270..) ~ 2; refined when emitting */
                far_best = K[j] + 1, far_j = j;
        }
        /* queues are pushed with descending j: the head holds the largest j, mono_min drops j > hi */
        int best = 1 + (n - i) + xlen(n - i), bj = -1, c, j;
        if ((j = mono_min(&w0, K, i + 14)) >= 0 && (c = 1 + K[j] - i) < best)
            best = c, bj = j;
        if ((j = mono_min(&w1, K, i + 269)) >= 0 && (c = 1 + K[j] - i + 1) < best)
            best = c, bj = j;
        if (far_j >= 0 && (c = 1 + far_best - i + (far_j - i - 15) / 255) < best)
            best = c, bj = far_j;
        F[i] = best;
        choiceJ[i] = bj;
    }

    /* 3. emit */
    uint8_t *o = dst, *end = dst + cap;
    int i = 0;
    for (;;) {
        int j = choiceJ[i];
        int nlit = (j < 0 ? n : j) - i;
        int M = j < 0 ? 0 : choiceM[j];
        if (o + 1 + xlen(nlit) + nlit + (M ? 2 + xlen(M - MINMATCH) : 0) > end)
            return 0;
        uint8_t *tok = o++;
        *tok = (uint8_t)((nlit >= 15 ? 15 : nlit) << 4);
        if (nlit >= 15)
            o = put_len(o, nlit - 15);
        memcpy(o, src + i, nlit);
        o += nlit;
        if (j < 0)
            break;
        int off = moff[j];
        *o++ = (uint8_t)off;
        *o++ = (uint8_t)(off >> 8);
        int m = M - MINMATCH;
        *tok |= m >= 15 ? 15 : (uint8_t)m;
        if (m >= 15)
            o = put_len(o, m - 15);
        i = j + M;
    }
    return (size_t)(o - dst);
}

/* Reference decoder, identical to tools/native/lz4pack.c (same algorithm as pack.c) */
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
