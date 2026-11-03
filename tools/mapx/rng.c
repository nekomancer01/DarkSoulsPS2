/* numpy's default_rng (PCG64 XSL-RR) and the Generator methods the exporter calls, bit for bit: random(), choice(n,
   p=...) (cumsum, normalised by its last value, searchsorted right), choice(n, size, replace=False) (tail shuffle or
   Floyd's algorithm, then a shuffle; both draw with random_bounded_uint64), integers(n). The state comes from numpy (bit_generator.state) or mx_rng_seed. */
#include "mapx.h"

struct mx_rng {
    uint64_t sh, sl, ih, il; /* 128-bit state and increment */
    int has32;
    uint32_t u32;
};

/* 128-bit multiply-add without __int128 (wasm32) */
static void mul128(uint64_t ah, uint64_t al, uint64_t bh, uint64_t bl, uint64_t *rh, uint64_t *rl)
{
    uint64_t a0 = al & 0xffffffffu, a1 = al >> 32, b0 = bl & 0xffffffffu, b1 = bl >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    *rl = (mid << 32) | (p00 & 0xffffffffu);
    *rh = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32) + ah * bl + al * bh;
}

static void step(mx_rng *r)
{
    uint64_t h, l;
    mul128(r->sh, r->sl, 0x2360ED051FC65DA4ULL, 0x4385DF649FCCF645ULL, &h, &l);
    l += r->il;
    h += r->ih + (l < r->il);
    r->sh = h, r->sl = l;
}

uint64_t mx_rng_next64(mx_rng *r)
{
    step(r);
    uint64_t x = r->sh ^ r->sl;
    unsigned rot = (unsigned)(r->sh >> 58);
    return (x >> rot) | (x << ((64 - rot) & 63));
}

uint32_t mx_rng_next32(mx_rng *r)
{
    if (r->has32) {
        r->has32 = 0;
        return r->u32;
    }
    uint64_t v = mx_rng_next64(r);
    r->has32 = 1;
    r->u32 = (uint32_t)(v >> 32);
    return (uint32_t)v;
}

double mx_rng_random(mx_rng *r) { return (double)(mx_rng_next64(r) >> 11) * (1.0 / 9007199254740992.0); }

MX_API mx_rng *mx_rng_new(uint64_t sh, uint64_t sl, uint64_t ih, uint64_t il, int has32, uint32_t u32)
{
    mx_rng *r = MX_NEW(mx_rng, 1);
    r->sh = sh, r->sl = sl, r->ih = ih, r->il = il, r->has32 = has32, r->u32 = u32;
    return r;
}

MX_API void mx_rng_free(mx_rng *r) { free(r); }

MX_API double mx_rng_random_py(mx_rng *r) { return mx_rng_random(r); }

/* numpy random_bounded_uint64(rng = max, use_masked = 0): Lemire on 32 bits for max < 2^32 */
uint64_t mx_rng_bounded(mx_rng *r, uint64_t max)
{
    if (max == 0)
        return 0;
    if (max <= 0xffffffffULL) {
        if (max == 0xffffffffULL)
            return mx_rng_next32(r);
        uint32_t excl = (uint32_t)max + 1;
        uint64_t m = (uint64_t)mx_rng_next32(r) * excl;
        uint32_t left = (uint32_t)m;
        if (left < excl) {
            uint32_t thr = (uint32_t)(0xffffffffu - (uint32_t)max) % excl;
            while (left < thr) {
                m = (uint64_t)mx_rng_next32(r) * excl;
                left = (uint32_t)m;
            }
        }
        return m >> 32;
    }
    /* 64-bit Lemire (not reached by the exporter's sizes) */
    uint64_t excl = max + 1;
    for (;;) {
        uint64_t x = mx_rng_next64(r), hi, lo;
        uint64_t a0 = x & 0xffffffffu, a1 = x >> 32, b0 = excl & 0xffffffffu, b1 = excl >> 32;
        uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
        uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
        lo = (mid << 32) | (p00 & 0xffffffffu);
        hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
        if (lo >= excl || lo >= (0 - excl) % excl)
            return hi;
    }
}

static void shuffle_int(mx_rng *r, long long n, long long first, long long *data)
{
    for (long long i = n - 1; i >= first; i--) {
        long long j = (long long)mx_rng_bounded(r, (uint64_t)i);
        long long t = data[i];
        data[i] = data[j];
        data[j] = t;
    }
}

/* Generator.integers(n) (int64, 0 <= x < n) */
MX_API long long mx_rng_integers(mx_rng *r, long long n) { return (long long)mx_rng_bounded(r, (uint64_t)(n - 1)); }

/* Generator.choice(n, p=p): p already as numpy computed it (an array of n doubles) */
MX_API long long mx_rng_choice_p(mx_rng *r, long long n, const double *p)
{
    double *cdf = MX_NEW(double, n);
    double s = 0;
    for (long long i = 0; i < n; i++)
        cdf[i] = s = (i ? s + p[i] : p[i]);
    double last = cdf[n - 1];
    for (long long i = 0; i < n; i++)
        cdf[i] /= last;
    double u = mx_rng_random(r);
    long long lo = 0, hi = n;  /* searchsorted(side="right"): first cdf[i] > u */
    while (lo < hi) {
        long long m = (lo + hi) / 2;
        if (cdf[m] <= u)
            lo = m + 1;
        else
            hi = m;
    }
    free(cdf);
    return lo;
}

/* Generator.choice(pop, size, replace=False, shuffle=True) -> out[size] */
MX_API void mx_rng_choice_noreplace(mx_rng *r, long long pop, long long size, long long *out)
{
    if (pop > 10000 && size > pop / 50) {  /* tail shuffle */
        long long *idx = MX_NEW(long long, pop);
        for (long long i = 0; i < pop; i++)
            idx[i] = i;
        shuffle_int(r, pop, pop - size > 1 ? pop - size : 1, idx);
        memcpy(out, idx + (pop - size), sizeof(long long) * (size_t)size);
        free(idx);
        return;
    }
    uint64_t set_size = (uint64_t)(1.2 * (double)size), mask = set_size;  /* _gen_mask: smallest 2^k - 1 >= value */
    mask |= mask >> 1, mask |= mask >> 2, mask |= mask >> 4, mask |= mask >> 8, mask |= mask >> 16, mask |= mask >> 32;
    set_size = 1 + mask;
    uint64_t *hs = MX_NEW(uint64_t, set_size);
    memset(hs, 0xff, sizeof(uint64_t) * set_size);
    for (long long j = pop - size; j < pop; j++) {
        uint64_t val = mx_rng_bounded(r, (uint64_t)j), loc = val & mask;
        while (hs[loc] != ~0ULL && hs[loc] != val)
            loc = (loc + 1) & mask;
        if (hs[loc] == ~0ULL) {
            hs[loc] = val;
            out[j - pop + size] = (long long)val;
        } else {
            loc = (uint64_t)j & mask;
            while (hs[loc] != ~0ULL)
                loc = (loc + 1) & mask;
            hs[loc] = (uint64_t)j;
            out[j - pop + size] = j;
        }
    }
    free(hs);
    shuffle_int(r, size, 1, out);
}

MX_API void mx_rng_state(const mx_rng *r, uint64_t *out6)
{
    out6[0] = r->sh, out6[1] = r->sl, out6[2] = r->ih, out6[3] = r->il, out6[4] = (uint64_t)r->has32, out6[5] = r->u32;
}
