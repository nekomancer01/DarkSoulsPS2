/* libmapx shared helpers: numpy's summation, MD5, the hash map, the thread pool (see mapx.h) */
#include "mapx.h"
#if MX_THREADS
#include <pthread.h>
#include <unistd.h>
#endif

double mx_np_pairwise(const double *a, long long n, long long st)
{
    if (n < 8) {
        double res = -0.0;
        for (long long i = 0; i < n; i++)
            res += a[i * st];
        return res;
    }
    if (n <= 128) {
        double r[8], res;
        long long i;
        for (int j = 0; j < 8; j++)
            r[j] = a[j * st];
        for (i = 8; i < n - (n % 8); i += 8)
            for (int j = 0; j < 8; j++)
                r[j] += a[(i + j) * st];
        res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; i++)
            res += a[i * st];
        return res;
    }
    long long n2 = n / 2;
    n2 -= n2 % 8;
    return mx_np_pairwise(a, n2, st) + mx_np_pairwise(a + n2 * st, n - n2, st);
}

double mx_np_sum(const double *a, long long n, long long st)
{
    /* along a contiguous last axis the reduce is the pairwise sum of the whole row (checked against numpy 2.x) */
    return n > 0 ? mx_np_pairwise(a, n, st) : 0.0;
}

/* ---- MD5 ---- */
static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const uint8_t md5_r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(uint32_t h[4], const uint8_t *p)
{
    uint32_t w[16], a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) f = (b & c) | (~b & d), g = i;
        else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) & 15;
        else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) & 15;
        else f = c ^ (b | ~d), g = (7 * i) & 15;
        uint32_t t = d;
        d = c;
        c = b;
        uint32_t x = a + f + md5_k[i] + w[g];
        b = b + (x << md5_r[i] | x >> (32 - md5_r[i]));
        a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d;
}

void mx_md5(const void *msg, size_t n, uint8_t out[16])
{
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const uint8_t *p = msg;
    size_t i = 0;
    for (; i + 64 <= n; i += 64)
        md5_block(h, p + i);
    uint8_t tail[128] = {0};
    size_t r = n - i;
    memcpy(tail, p + i, r);
    tail[r] = 0x80;
    size_t tl = r + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++)
        tail[tl - 8 + k] = (uint8_t)(bits >> (8 * k));
    md5_block(h, tail);
    if (tl == 128)
        md5_block(h, tail + 64);
    for (int k = 0; k < 4; k++)
        for (int j = 0; j < 4; j++)
            out[4 * k + j] = (uint8_t)(h[k] >> (8 * j));
}

/* ---- hash map ---- */
static unsigned long long map_hash(const long long *key, int k)
{
    unsigned long long h = 0x9e3779b97f4a7c15ULL;
    for (int i = 0; i < k; i++) {
        h ^= (unsigned long long)key[i] + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
    }
    return h;
}

void mx_map_init(mx_map *m, int k, long long hint)
{
    m->k = k;
    m->n = 0;
    m->cap = 64;
    while (m->cap < 2 * hint)
        m->cap *= 2;
    m->keys = MX_NEW(long long, (size_t)m->cap / 2 * k);
    m->slot = MX_ZNEW(long long, m->cap);
}

void mx_map_free(mx_map *m)
{
    free(m->keys);
    free(m->slot);
    m->keys = m->slot = NULL;
}

static long long map_find(const mx_map *m, const long long *key)
{
    unsigned long long i = map_hash(key, m->k) & (unsigned long long)(m->cap - 1);
    for (;;) {
        long long s = m->slot[i];
        if (!s || !memcmp(m->keys + (s - 1) * m->k, key, sizeof(long long) * m->k))
            return (long long)i;
        i = (i + 1) & (unsigned long long)(m->cap - 1);
    }
}

long long mx_map_get(const mx_map *m, const long long *key)
{
    return m->slot[map_find(m, key)] - 1;
}

long long mx_map_put(mx_map *m, const long long *key, int *added)
{
    long long i = map_find(m, key);
    if (m->slot[i]) {
        if (added)
            *added = 0;
        return m->slot[i] - 1;
    }
    if (2 * (m->n + 1) > m->cap) {  /* grow: rehash every id */
        m->cap *= 2;
        free(m->slot);
        m->slot = MX_ZNEW(long long, m->cap);
        m->keys = mx_realloc(m->keys, sizeof(long long) * (size_t)m->cap / 2 * m->k);
        for (long long id = 0; id < m->n; id++)
            m->slot[map_find(m, m->keys + id * m->k)] = id + 1;
        i = map_find(m, key);
    }
    memcpy(m->keys + m->n * m->k, key, sizeof(long long) * m->k);
    m->slot[i] = ++m->n;
    if (added)
        *added = 1;
    return m->n - 1;
}

/* ---- threads ---- */
int mx_threads(void)
{
#if MX_THREADS
    static int n;
    if (!n) {
        const char *e = getenv("MAPX_THREADS");
        n = e ? atoi(e) : (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1)
            n = 1;
        if (n > 64)
            n = 64;
    }
    return n;
#else
    return 1;
#endif
}

#if MX_THREADS
typedef struct {
    long long n, next;
    pthread_mutex_t mu;
    void (*fn)(void *, long long);
    void *ctx;
} pf_job;

static void *pf_worker(void *arg)
{
    pf_job *j = arg;
    for (;;) {
        pthread_mutex_lock(&j->mu);
        long long i = j->next++;
        pthread_mutex_unlock(&j->mu);
        if (i >= j->n)
            return NULL;
        j->fn(j->ctx, i);
    }
}
#endif

void mx_parallel_for(long long n, void (*fn)(void *ctx, long long i), void *ctx)
{
    int t = mx_threads();
    if (t > n)
        t = (int)n;
#if MX_THREADS
    if (t > 1) {
        pf_job j = {n, 0, PTHREAD_MUTEX_INITIALIZER, fn, ctx};
        pthread_t th[64];
        int started = 0;
        for (int k = 1; k < t; k++)
            if (!pthread_create(&th[started], NULL, pf_worker, &j))
                started++;
        pf_worker(&j);
        for (int k = 0; k < started; k++)
            pthread_join(th[k], NULL);
        return;
    }
#endif
    for (long long i = 0; i < n; i++)
        fn(ctx, i);
}
