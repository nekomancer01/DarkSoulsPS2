/* libmapx: the map exporter (tools/export_map.py and the modules it calls) rewritten in C, one stage at a time.

   Each stage is a plain C function over flat arrays. Until every stage is ported, export_map.py calls them through
   ctypes (tools/mapx_native.py) and keeps its Python code as the reference: MAPX=0 runs the Python path, and both
   must write the same bytes. Once the last stage lands, a C driver calls the same functions with no Python.

   Bit-exact rules (the output must match the Python export byte for byte, natively and in the in-browser converter):
   - IEEE double / float in the same operations and order as the Python or numpy code; built with -ffp-contract=off
     and no fast math (no FMA, no reassociation).
   - Python's sum() of floats is Neumaier-compensated since 3.12 (mx_py_sum); numpy's add.reduce is a pairwise sum
     with 8 accumulators (mx_np_pairwise); Python min() / max() keep the first of equal values.
   - libm only where the Python code calls libm too (math.sqrt / **, both correctly rounded or the same library):
     sqrt, floor, rint are exact everywhere.
   - In the browser the library is a Pyodide side module: libc from Pyodide (malloc, qsort, memcpy), no threads
     (MX_THREADS 0). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef MX_THREADS
#define MX_THREADS 1
#endif
#define MX_API __attribute__((visibility("default")))

/* out of memory (the in-browser converter's heap has a cap): trap instead of writing through NULL (address 0 in wasm) */
static inline void *mx_ok(void *p, size_t n)
{
    if (!p && n)
        __builtin_trap();
    return p;
}
static inline void *mx_alloc(size_t n) { return mx_ok(malloc(n ? n : 1), 1); }
static inline void *mx_realloc(void *p, size_t n) { return mx_ok(realloc(p, n ? n : 1), 1); }
#define MX_NEW(T, n) ((T *)mx_alloc(sizeof(T) * (size_t)(n)))
static inline void *mx_zalloc(size_t n, size_t sz) { return mx_ok(calloc(n ? n : 1, sz), 1); }
#define MX_ZNEW(T, n) ((T *)mx_zalloc((size_t)(n), sizeof(T)))

static inline double mx_fabs(double x) { return __builtin_fabs(x); }
static inline double mx_floor(double x) { return __builtin_floor(x); }
static inline double mx_rint(double x) { return __builtin_rint(x); }  /* round half to even: Python round(), np.rint */
static inline double mx_sqrt(double x) { return __builtin_sqrt(x); }
static inline int mx_isfinite(double x) { return __builtin_isfinite(x); }

/* Python 3.12+ sum() of a float sequence: 0 + x0, then Neumaier compensation, the compensation added at the end when
   non-zero and finite (Python/bltinmodule.c builtin_sum_impl) */
typedef struct { double f, c; } mx_psum;
static inline void mx_psum_init(mx_psum *s, double first) { s->f = 0.0 + first; s->c = 0.0; }
static inline void mx_psum_add(mx_psum *s, double x)
{
    double t = s->f + x;
    if (mx_fabs(s->f) >= mx_fabs(x))
        s->c += (s->f - t) + x;
    else
        s->c += (x - t) + s->f;
    s->f = t;
}
static inline double mx_psum_get(const mx_psum *s) { return (s->c != 0.0 && mx_isfinite(s->c)) ? s->f + s->c : s->f; }
static inline double mx_py_sum3(double a, double b, double c)
{
    mx_psum s;
    mx_psum_init(&s, a);
    mx_psum_add(&s, b);
    mx_psum_add(&s, c);
    return mx_psum_get(&s);
}

/* numpy's pairwise_sum (umath loops) over a strided array of n doubles */
double mx_np_pairwise(const double *a, long long n, long long stride);
/* numpy add.reduce along an axis of n elements (stride in doubles): what x.sum(axis) gives per output element */
double mx_np_sum(const double *a, long long n, long long stride);

/* MD5 (RFC 1321) of n bytes */
void mx_md5(const void *msg, size_t n, uint8_t out[16]);

/* parallel for: fn(ctx, i) for i in [0, n), on up to mx_threads() threads (1 in the browser); every i must write
   only its own outputs, so the result does not depend on the thread count */
int mx_threads(void);
void mx_parallel_for(long long n, void (*fn)(void *ctx, long long i), void *ctx);

/* open-addressing hash map from a key of k int64s to a dense id (insertion order) */
typedef struct {
    int k;
    long long n, cap;
    long long *keys;  /* n x k, by id */
    long long *slot;  /* cap: id + 1, 0 = empty */
} mx_map;
void mx_map_init(mx_map *m, int k, long long hint);
void mx_map_free(mx_map *m);
long long mx_map_get(const mx_map *m, const long long *key);  /* id or -1 */
long long mx_map_put(mx_map *m, const long long *key, int *added);  /* id (new ids count up from 0) */

/* numpy default_rng (rng.c) */
typedef struct mx_rng mx_rng;
uint64_t mx_rng_next64(mx_rng *r);
double mx_rng_random(mx_rng *r);
uint64_t mx_rng_bounded(mx_rng *r, uint64_t max);
long long mx_rng_integers(mx_rng *r, long long n);
long long mx_rng_choice_p(mx_rng *r, long long n, const double *p);
void mx_rng_choice_noreplace(mx_rng *r, long long pop, long long size, long long *out);
void mx_nearest_d2(long long nq, const double *Q, long long ns, double *S, double *out);
