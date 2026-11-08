/* lua0: Lua 5.0 VM for DSR's AI scripts (lua0.h). Instruction semantics follow Lua 5.0's lvm.c / lopcodes.h:
 * op 6 bits, C 9, B 9, A 8 (low to high), Bx 18, sBx = Bx - 131071, RK operands >= 250 are constants. */
#include <malloc.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua0.h"

enum { OP_MOVE, OP_LOADK, OP_LOADBOOL, OP_LOADNIL, OP_GETUPVAL, OP_GETGLOBAL, OP_GETTABLE, OP_SETGLOBAL, OP_SETUPVAL,
       OP_SETTABLE, OP_NEWTABLE, OP_SELF, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_POW, OP_UNM, OP_NOT, OP_CONCAT, OP_JMP,
       OP_EQ, OP_LT, OP_LE, OP_TEST, OP_CALL, OP_TAILCALL, OP_RETURN, OP_FORLOOP, OP_TFORLOOP, OP_TFORPREP,
       OP_SETLIST, OP_SETLISTO, OP_CLOSE, OP_CLOSURE };
#define GET_OP(i) ((i) & 0x3F)
#define GET_A(i) ((int)((i) >> 24))
#define GET_B(i) ((int)(((i) >> 15) & 0x1FF))
#define GET_C(i) ((int)(((i) >> 6) & 0x1FF))
#define GET_BX(i) ((int)(((i) >> 6) & 0x3FFFF))
#define GET_SBX(i) (GET_BX(i) - 131071)
#define MAXSTACK 250
#define FPF 32  /* LFIELDS_PER_FLUSH */

#define STACK_SIZE 6144
#define MAX_CI 160

/* ---- objects ---- */

typedef struct GCH {
    struct GCH *next;
    unsigned char type, mark, fixed;
} GCH;

struct LStr {
    GCH h;
    struct LStr *hnext;
    unsigned hash, len;
    char s[1];
};

typedef struct { LVal key, val; } Node;

struct LTable {
    GCH h;
    LVal *arr;
    unsigned asize, hsize, hused;  /* hsize: power of two or 0; hused: slots with a key (tombstones included) */
    Node *node;
};

typedef struct Proto {
    const unsigned *code;
    LVal *k;
    struct Proto **p;
    unsigned ncode, nk, np, line;
    unsigned char nups, nparams, maxstack;
    const char *chunk;
} Proto;

typedef struct UpVal {
    GCH h;
    LVal *v;
    LVal value;
    struct UpVal *open_next;
} UpVal;

typedef struct {
    GCH h;
    const Proto *p;
    int nups;
    UpVal *up[1];
} LClosure;

typedef struct {
    GCH h;
    LCFunc f;
    LVal up;
} CClosure;

enum { GC_STR = 1, GC_TABLE, GC_LCL, GC_CCL, GC_UPVAL };

typedef struct {
    LClosure *cl;
    LVal *func, *base;
    const unsigned *pc;
    int nresults;  /* wanted by the caller, -1 = all */
} CallInfo;

static LVal stack[STACK_SIZE];
static LVal *top;   /* end of the last multi-result call / return */
static CallInfo cis[MAX_CI];
static int nci;
static UpVal *open_up;
static GCH *allgc;
/* GC (lua0_gc_step): marks alternate between cycles. An object is live in this cycle when mark == gc_cur; new objects
 * are born with it, so the sweep that follows a mark (spread over frames) keeps them; a string interned again while
 * dead but not yet swept is revived (new_str). sweep_pp: the sweep's place in allgc, NULL between cycles. */
static unsigned char gc_cur;
static GCH **sweep_pp;
static unsigned gc_before, gc_since, gc_mark_us, gc_sweep_us, gc_step_max_us, gc_steps;
static LStr **strt;
static unsigned strt_size, strt_count;
static LVal globals;
static LVal methods[8];
static LVal (*fallbacks[8])(int, LVal);
static jmp_buf *err_jmp;
char lua0_errmsg[160];
unsigned lua0_heap, lua0_since_gc, lua0_instructions;

/* ---- memory ---- */

/* Small objects (<= LP_MAX bytes with the header) come from a pool of their own, one free list per 16-byte size
 * class: the VM allocates and frees all the time, and in the general heap its objects pinned holes between the
 * large blocks enemy.c streams (model.bin 379K OUT OF RAM with 3 MB free, world build with two blocks' enemies,
 * 2026-11-01). Larger ones (code, big tables) and a full pool fall back to malloc. 8-byte header: the class, the
 * size (lua0_malloc_live). TECH_DEBT world-5: with two blocks' AI scripts 394 KB stay live after a sweep, the pool
 * stays full and up to ~180 KB of garbage per collection goes through malloc (30-minute soak, 2026-11-01). */
#define LP_SIZE (384u << 10)
#define LP_MAX 512
/* LUA_POOL_GROW (default on; 0 = the fixed pool): a full pool takes another LP_CHUNK block instead of sending small
 * objects to malloc. With five blocks' AI scripts 575 KB stay live (ISO Burg route, 2026-11-08: pool 384 KB full,
 * 276 KB of small Lua objects in malloc), and those short-lived 16-512 byte blocks pinned holes between the enemy
 * models (sdk303 _txt "No More Excuses - Use the IOP": keep long- and short-lived allocations apart). The chunks stay
 * for the run (the free lists recycle their objects); at most LP_CHUNKS of them. */
#ifndef LUA_POOL_GROW
#define LUA_POOL_GROW 1
#endif
#define LP_CHUNK (64u << 10)
#define LP_CHUNKS 12
static unsigned char *lp;
static unsigned lp_used, lp_cap = LP_SIZE, lp_chunks;
static void *lp_free[LP_MAX / 16 + 1];
unsigned lua0_pool_used, lua0_pool_free, lua0_malloc_live;  /* pool watermark, bytes on its free lists, fallback bytes */

static void *lmem(unsigned n)
{
    unsigned c = (n + 8 + 15) / 16;
    unsigned char *b = NULL;
    if (c <= LP_MAX / 16) {
        if (lp_free[c])
            b = lp_free[c], lp_free[c] = *(void **)(b + 8), lua0_pool_free -= c * 16;
        else if (lp && lp_used + c * 16 <= lp_cap)
            b = lp + lp_used, lp_used += c * 16, lua0_pool_used += c * 16;
        else if (LUA_POOL_GROW && lp && lp_chunks < LP_CHUNKS && (b = memalign(16, LP_CHUNK))) {
            lp = b, lp_cap = LP_CHUNK, lp_chunks++;  /* the rest of the old block stays unused (< 512 B) */
            lp_used = c * 16, lua0_pool_used += c * 16;
        }
    }
    if (!b) {
        b = malloc(n + 8);
        c = 0;
    }
    if (!b)
        return NULL;
    ((unsigned *)b)[0] = c, ((unsigned *)b)[1] = n + 8;
    if (!c)
        lua0_malloc_live += n + 8;
    return b + 8;
}

static void lrel(void *p)
{
    if (!p)
        return;
    unsigned char *b = (unsigned char *)p - 8;
    unsigned c = *(unsigned *)b;
    if (c) {
        *(void **)(b + 8) = lp_free[c];
        lp_free[c] = b;
        lua0_pool_free += c * 16;
    } else {
        lua0_malloc_live -= ((unsigned *)b)[1];
        free(b);
    }
}

static void *gc_alloc(unsigned size, int type)
{
    GCH *o = lmem(size);
    if (!o)
        lua0_error("out of memory (%u bytes)", size);
    memset(o, 0, size);
    o->type = (unsigned char)type;
    o->mark = gc_cur;
    o->next = allgc;
    allgc = o;
    lua0_heap += size, lua0_since_gc += size;
    return o;
}

static void *raw_alloc(unsigned size)
{
    void *p = lmem(size ? size : 1);
    if (!p)
        lua0_error("out of memory (%u bytes)", size);
    memset(p, 0, size ? size : 1);
    lua0_heap += size, lua0_since_gc += size;
    return p;
}

static void raw_free(void *p, unsigned size)
{
    if (!p)
        return;
    lrel(p);
    lua0_heap -= size < lua0_heap ? size : lua0_heap;
}

void lua0_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lua0_errmsg, sizeof lua0_errmsg, fmt, ap);
    va_end(ap);
    if (err_jmp)
        longjmp(*err_jmp, 1);
    printf("lua0: unprotected error: %s\n", lua0_errmsg);
    abort();
}

/* ---- strings ---- */

static unsigned str_hash(const char *s, unsigned len)
{
    unsigned h = len ^ 0x9E3779B9u;
    for (unsigned i = 0; i < len; i++)
        h ^= (h << 5) + (h >> 2) + (unsigned char)s[i];
    return h;
}

static void strt_resize(unsigned n)
{
    LStr **t = calloc(n, sizeof *t);
    for (unsigned i = 0; i < strt_size; i++)
        for (LStr *s = strt[i], *nx; s; s = nx) {
            nx = s->hnext;
            s->hnext = t[s->hash & (n - 1)];
            t[s->hash & (n - 1)] = s;
        }
    free(strt);
    strt = t, strt_size = n;
}

static LStr *new_str(const char *s, unsigned len)
{
    unsigned h = str_hash(s, len);
    for (LStr *e = strt[h & (strt_size - 1)]; e; e = e->hnext)
        if (e->hash == h && e->len == len && !memcmp(e->s, s, len)) {
            e->h.mark = gc_cur;  /* dead and not yet swept: reachable again */
            return e;
        }
    LStr *e = gc_alloc(sizeof(LStr) + len, GC_STR);
    memcpy(e->s, s, len);
    e->s[len] = 0;
    e->len = len, e->hash = h;
    e->hnext = strt[h & (strt_size - 1)];
    strt[h & (strt_size - 1)] = e;
    if (++strt_count > strt_size * 2)
        strt_resize(strt_size * 2);
    return e;
}

static LVal sval(LStr *s) { LVal v = {LT_STR, 0, {0}}; v.u.s = s; return v; }
LVal lua0_str(const char *s) { return sval(new_str(s, strlen(s))); }
const char *lua0_cstr(LVal v) { return v.t == LT_STR ? v.u.s->s : NULL; }

/* ---- tables ---- */

static LTable *new_table(unsigned na, unsigned nh)
{
    LTable *t = gc_alloc(sizeof(LTable), GC_TABLE);
    if (na)
        t->arr = raw_alloc(na * sizeof(LVal)), t->asize = na;
    if (nh) {
        unsigned n = 4;
        while (n < nh * 2)
            n <<= 1;
        t->node = raw_alloc(n * sizeof(Node)), t->hsize = n;
    }
    return t;
}

static LVal tval(LTable *t) { LVal v = {LT_TABLE, 0, {0}}; v.u.h = t; return v; }

static unsigned key_hash(LVal k)
{
    switch (k.t) {
    case LT_STR: return k.u.s->hash;
    case LT_NUM: {
        float f = k.u.n == 0 ? 0 : k.u.n;  /* -0 == 0 */
        unsigned u;
        memcpy(&u, &f, 4);
        return u ^ (u >> 15) ^ (u << 7);
    }
    case LT_BOOL: return (unsigned)k.u.b + 1;
    default: return (unsigned)((unsigned long)k.u.p >> 3) * 2654435761u;
    }
}

static int raw_eq(LVal a, LVal b)
{
    if (a.t != b.t)
        return 0;
    switch (a.t) {
    case LT_NIL: return 1;
    case LT_NUM: return a.u.n == b.u.n;
    case LT_BOOL: return a.u.b == b.u.b;
    case LT_UDATA: return a.u.p == b.u.p && a.tag == b.tag;
    default: return a.u.p == b.u.p;
    }
}

static int arr_index(LVal k, unsigned asize)
{
    if (k.t != LT_NUM)
        return -1;
    int i = (int)k.u.n;
    return (float)i == k.u.n && i >= 1 && (unsigned)i <= asize ? i - 1 : -1;
}

static Node *hfind(const LTable *t, LVal k)
{
    if (!t->hsize)
        return NULL;
    unsigned m = t->hsize - 1;
    for (unsigned i = key_hash(k) & m, n = 0; n < t->hsize; i = (i + 1) & m, n++) {
        Node *nd = &t->node[i];
        if (nd->key.t == LT_NIL)
            return NULL;
        if (raw_eq(nd->key, k))
            return nd;
    }
    return NULL;
}

static LVal tget(const LTable *t, LVal k)
{
    int ai = arr_index(k, t->asize);
    if (ai >= 0)
        return t->arr[ai];
    Node *nd = hfind(t, k);
    return nd ? nd->val : LNIL;
}

static void tset(LTable *t, LVal k, LVal v);

static void rehash(LTable *t, unsigned extra_int)
{
    /* count live hash entries; integer keys continuing the array part move into it */
    unsigned live = 0;
    for (unsigned i = 0; i < t->hsize; i++)
        live += t->node[i].key.t != LT_NIL && t->node[i].val.t != LT_NIL;
    unsigned na = t->asize;
    if (extra_int) {
        na = na ? na : 1;
        while (na < extra_int)
            na *= 2;
        LVal *arr = raw_alloc(na * sizeof(LVal));
        if (t->asize)
            memcpy(arr, t->arr, t->asize * sizeof(LVal)), raw_free(t->arr, t->asize * sizeof(LVal));
        t->arr = arr, t->asize = na;
    }
    Node *old = t->node;
    unsigned oldn = t->hsize, n = 4;
    while (n < (live + 1) * 2)
        n <<= 1;
    t->node = raw_alloc(n * sizeof(Node)), t->hsize = n, t->hused = 0;
    for (unsigned i = 0; i < oldn; i++)
        if (old[i].key.t != LT_NIL && old[i].val.t != LT_NIL)
            tset(t, old[i].key, old[i].val);
    raw_free(old, oldn * sizeof(Node));
}

static void tset(LTable *t, LVal k, LVal v)
{
    int ai = arr_index(k, t->asize);
    if (ai >= 0) {
        t->arr[ai] = v;
        return;
    }
    if (k.t == LT_NIL)
        lua0_error("table index is nil");
    Node *nd = hfind(t, k);
    if (nd) {
        nd->val = v;
        return;
    }
    if (v.t == LT_NIL)
        return;
    /* t[#arr + 1] = v grows the array part (lists built by index, e.g. ret[i] = ... in common_func) */
    if (k.t == LT_NUM && k.u.n == (float)(t->asize + 1) && t->asize < 4096) {
        rehash(t, t->asize + 1);
        tset(t, k, v);
        return;
    }
    if ((t->hused + 1) * 4 > t->hsize * 3) {
        rehash(t, 0);
        tset(t, k, v);
        return;
    }
    unsigned m = t->hsize - 1, i = key_hash(k) & m;
    while (t->node[i].key.t != LT_NIL)
        i = (i + 1) & m;
    t->node[i].key = k, t->node[i].val = v;
    t->hused++;
}

/* table.getn (Lua 5.0): field n if numeric, else the border of the sequence 1..n */
static unsigned tgetn(const LTable *t)
{
    LVal n = tget(t, lua0_str("n"));
    if (n.t == LT_NUM)
        return (unsigned)n.u.n;
    unsigned i = 0;
    while (tget(t, lnum((float)(i + 1))).t != LT_NIL)
        i++;
    return i;
}

LVal lua0_newtable(void) { return tval(new_table(0, 0)); }
LVal lua0_get(LVal t, LVal k) { return t.t == LT_TABLE ? tget(t.u.h, k) : LNIL; }
void lua0_set(LVal t, LVal k, LVal v) { if (t.t == LT_TABLE) tset(t.u.h, k, v); }
LVal lua0_getglobal(const char *name) { return tget(globals.u.h, lua0_str(name)); }
void lua0_setglobal(const char *name, LVal v) { tset(globals.u.h, lua0_str(name), v); }

LVal lua0_cfunc(LCFunc f, LVal up)
{
    CClosure *c = gc_alloc(sizeof(CClosure), GC_CCL);
    c->f = f, c->up = up;
    LVal v = {LT_CFUNC, 0, {0}};
    v.u.p = c;
    return v;
}

void lua0_register(const char *name, LCFunc f)
{
    LVal v = lua0_cfunc(f, LNIL);
    ((GCH *)v.u.p)->fixed = 1;
    lua0_setglobal(name, v);
}

void lua0_methods(int tag, LVal table, LVal (*fallback)(int, LVal))
{
    methods[tag & 7] = table;
    fallbacks[tag & 7] = fallback;
}

/* ---- loading (tools/lua50.py --pack) ---- */

typedef struct { const unsigned char *b, *end; } Rd;

static unsigned rd_u32(Rd *r)
{
    if (r->b + 4 > r->end)
        lua0_error("truncated pack");
    unsigned v;
    memcpy(&v, r->b, 4);
    r->b += 4;
    return v;
}

static const char *cur_chunk;

static Proto *rd_proto(Rd *r)
{
    Proto *f = raw_alloc(sizeof(Proto));
    unsigned h = rd_u32(r);
    f->nups = h & 0xFF, f->nparams = (h >> 8) & 0xFF, f->maxstack = (h >> 16) & 0xFF;
    f->line = rd_u32(r);
    f->ncode = rd_u32(r), f->nk = rd_u32(r), f->np = rd_u32(r);
    f->chunk = cur_chunk;
    if (r->b + 4 * f->ncode > r->end)
        lua0_error("truncated code");
    unsigned *code = raw_alloc(4 * f->ncode);
    memcpy(code, r->b, 4 * f->ncode);
    r->b += 4 * f->ncode;
    f->code = code;
    f->k = raw_alloc(sizeof(LVal) * (f->nk ? f->nk : 1));
    for (unsigned i = 0; i < f->nk; i++) {
        unsigned t = rd_u32(r);
        if (t == LT_NUM) {
            unsigned u = rd_u32(r);
            float x;
            memcpy(&x, &u, 4);
            f->k[i] = lnum(x);
        } else if (t == LT_STR) {
            unsigned len = rd_u32(r);
            if (r->b + len > r->end)
                lua0_error("truncated string");
            LStr *s = new_str((const char *)r->b, len);
            s->h.fixed = 1;
            r->b += (len + 3) & ~3u;
            f->k[i] = sval(s);
        } else
            f->k[i] = LNIL;
    }
    f->p = raw_alloc(sizeof(Proto *) * (f->np ? f->np : 1));
    for (unsigned i = 0; i < f->np; i++)
        f->p[i] = rd_proto(r);
    return f;
}

static LClosure *new_lclosure(const Proto *p)
{
    LClosure *c = gc_alloc(sizeof(LClosure) + sizeof(UpVal *) * (p->nups ? p->nups : 1), GC_LCL);
    c->p = p, c->nups = p->nups;
    return c;
}

int lua0_load(const void *blob, int size)
{
    Rd r = {blob, (const unsigned char *)blob + size};
    jmp_buf jb, *prev = err_jmp;
    err_jmp = &jb;
    if (setjmp(jb)) {
        err_jmp = prev;
        printf("lua0: load error: %s\n", lua0_errmsg);
        return -1;
    }
    if (size < 8 || memcmp(blob, "DSRL", 4))
        lua0_error("not a lua0 pack");
    r.b += 4;
    unsigned n = rd_u32(&r), done = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned len = rd_u32(&r);
        char *name = raw_alloc(len + 1);
        memcpy(name, r.b, len);
        r.b += (len + 3) & ~3u;
        cur_chunk = name;
        Proto *p = rd_proto(&r);
        LClosure *cl = new_lclosure(p);
        LVal f = {LT_LFUNC, 0, {0}};
        f.u.p = cl;
        err_jmp = prev;
        if (lua0_call(f, NULL, 0, NULL, 0) < 0)
            printf("lua0: %s: %s\n", name, lua0_errmsg);
        else
            done++;
        err_jmp = &jb;
    }
    err_jmp = prev;
    return (int)done;
}

/* ---- upvalues ---- */

static UpVal *find_upval(LVal *level)
{
    UpVal **pp = &open_up;
    for (UpVal *u; (u = *pp) && u->v >= level; pp = &u->open_next)
        if (u->v == level)
            return u;
    UpVal *u = gc_alloc(sizeof(UpVal), GC_UPVAL);
    u->v = level;
    u->open_next = *pp;
    *pp = u;
    return u;
}

static void close_upvals(LVal *level)
{
    while (open_up && open_up->v >= level) {
        UpVal *u = open_up;
        u->value = *u->v;
        u->v = &u->value;
        open_up = u->open_next;
    }
}

/* ---- the VM ---- */

static const char *type_name(LVal v)
{
    static const char *const n[] = {"nil", "boolean", "number", "string", "table", "function", "function", "userdata"};
    return n[v.t & 7];
}

static LVal get_index(LVal o, LVal k)
{
    if (o.t == LT_TABLE)
        return tget(o.u.h, k);
    if (o.t == LT_UDATA) {
        LVal m = methods[o.tag & 7];
        LVal v = m.t == LT_TABLE ? tget(m.u.h, k) : LNIL;
        if (v.t == LT_NIL && fallbacks[o.tag & 7])
            v = fallbacks[o.tag & 7](o.tag, k);
        return v;
    }
    lua0_error("attempt to index a %s value (key %s)", type_name(o), k.t == LT_STR ? k.u.s->s : "?");
    return LNIL;
}

static float tonum(LVal v, const char *what)
{
    if (v.t == LT_NUM)
        return v.u.n;
    if (v.t == LT_STR) {
        char *e;
        float f = strtof(v.u.s->s, &e);
        if (e != v.u.s->s && !*e)
            return f;
    }
    lua0_error("attempt to perform arithmetic (%s) on a %s value", what, type_name(v));
    return 0;
}

static int less_than(LVal a, LVal b, int eq)
{
    if (a.t == LT_NUM && b.t == LT_NUM)
        return eq ? a.u.n <= b.u.n : a.u.n < b.u.n;
    if (a.t == LT_STR && b.t == LT_STR) {
        int c = strcmp(a.u.s->s, b.u.s->s);
        return eq ? c <= 0 : c < 0;
    }
    lua0_error("attempt to compare %s with %s", type_name(a), type_name(b));
    return 0;
}

static LStr *tostr(LVal v)
{
    char buf[32];
    if (v.t == LT_STR)
        return v.u.s;
    if (v.t == LT_NUM) {
        if (v.u.n == (float)(int)v.u.n)
            snprintf(buf, sizeof buf, "%d", (int)v.u.n);
        else
            snprintf(buf, sizeof buf, "%.7g", (double)v.u.n);
        return new_str(buf, strlen(buf));
    }
    lua0_error("attempt to concatenate a %s value", type_name(v));
    return NULL;
}

static float powf_i(float a, float b)
{
    /* x ^ n in AI scripts: small integer powers (distance squares); others via exp/log would need libm's powf */
    int n = (int)b;
    if ((float)n == b && n >= 0 && n < 32) {
        float r = 1;
        while (n--)
            r *= a;
        return r;
    }
    return powf(a, b);
}

/* Run from the current top CallInfo until it (the entry frame) returns */
static void execute(int entry)
{
    CallInfo *ci;
    LClosure *cl;
    LVal *base, *k;
    const unsigned *pc;
#define RELOAD() (ci = &cis[nci - 1], cl = ci->cl, base = ci->base, k = cl->p->k, pc = ci->pc)
#define RA (base + a)
#define RB (base + GET_B(i))
#define RKB (GET_B(i) >= MAXSTACK ? &k[GET_B(i) - MAXSTACK] : base + GET_B(i))
#define RKC (GET_C(i) >= MAXSTACK ? &k[GET_C(i) - MAXSTACK] : base + GET_C(i))
    RELOAD();
    for (;;) {
        unsigned i = *pc++;
        int a = GET_A(i);
        lua0_instructions++;
        switch (GET_OP(i)) {
        case OP_MOVE: *RA = *RB; break;
        case OP_LOADK: *RA = k[GET_BX(i)]; break;
        case OP_LOADBOOL:
            *RA = lbool(GET_B(i));
            if (GET_C(i))
                pc++;
            break;
        case OP_LOADNIL:
            for (int r = a; r <= GET_B(i); r++)
                base[r] = LNIL;
            break;
        case OP_GETUPVAL: *RA = *cl->up[GET_B(i)]->v; break;
        case OP_GETGLOBAL: *RA = tget(globals.u.h, k[GET_BX(i)]); break;
        case OP_GETTABLE: { LVal t = *RB; *RA = get_index(t, *RKC); break; }
        case OP_SETGLOBAL: tset(globals.u.h, k[GET_BX(i)], *RA); break;
        case OP_SETUPVAL: *cl->up[GET_B(i)]->v = *RA; break;
        case OP_SETTABLE:
            if (RA->t != LT_TABLE)
                lua0_error("attempt to index a %s value (set)", type_name(*RA));
            tset(RA->u.h, *RKB, *RKC);
            break;
        case OP_NEWTABLE: {
            int b = GET_B(i);
            *RA = tval(new_table((unsigned)((b & 7) << (b >> 3)), GET_C(i) ? 1u << (GET_C(i) - 1) : 0));
            break;
        }
        case OP_SELF: {
            LVal o = *RB;
            RA[1] = o;
            *RA = get_index(o, *RKC);
            break;
        }
        case OP_ADD: { float x = tonum(*RKB, "add"), y = tonum(*RKC, "add"); *RA = lnum(x + y); break; }
        case OP_SUB: { float x = tonum(*RKB, "sub"), y = tonum(*RKC, "sub"); *RA = lnum(x - y); break; }
        case OP_MUL: { float x = tonum(*RKB, "mul"), y = tonum(*RKC, "mul"); *RA = lnum(x * y); break; }
        case OP_DIV: { float x = tonum(*RKB, "div"), y = tonum(*RKC, "div"); *RA = lnum(x / y); break; }
        case OP_POW: { float x = tonum(*RKB, "pow"), y = tonum(*RKC, "pow"); *RA = lnum(powf_i(x, y)); break; }
        case OP_UNM: *RA = lnum(-tonum(*RB, "unm")); break;
        case OP_NOT: *RA = lbool(!ltruthy(*RB)); break;
        case OP_CONCAT: {
            int b = GET_B(i), c = GET_C(i);
            char buf[256];
            unsigned n = 0;
            for (int r = b; r <= c; r++) {
                LStr *s = tostr(base[r]);
                unsigned l = s->len < sizeof buf - 1 - n ? s->len : sizeof buf - 1 - n;
                memcpy(buf + n, s->s, l);
                n += l;
            }
            *RA = sval(new_str(buf, n));
            break;
        }
        case OP_JMP: pc += GET_SBX(i); break;
        /* EQ / LT / LE / TEST: the next instruction is a JMP, skipped when the test fails */
        case OP_EQ: if (raw_eq(*RKB, *RKC) != a) pc++; break;
        case OP_LT: if (less_than(*RKB, *RKC, 0) != a) pc++; break;
        case OP_LE: if (less_than(*RKB, *RKC, 1) != a) pc++; break;
        case OP_TEST: {
            LVal v = *RB;
            if ((!ltruthy(v)) == GET_C(i))
                pc++;
            else
                *RA = v;
            break;
        }
        case OP_CALL:
        case OP_TAILCALL: {  /* TAILCALL as a plain call: the RETURN A 0 the compiler emits after it passes all results on */
            int b = GET_B(i), c = GET_OP(i) == OP_TAILCALL ? 0 : GET_C(i);
            LVal *func = RA;
            int nargs = b ? b - 1 : (int)(top - func - 1);
            int nres = c - 1;
            if (func->t == LT_CFUNC) {
                CClosure *cc = func->u.p;
                LVal out[8];
                ci->pc = pc;
                int n = cc->f(func + 1, nargs, out, &cc->up);
                RELOAD();
                func = base + a;
                if (nres < 0) {
                    for (int r = 0; r < n; r++)
                        func[r] = out[r];
                    top = func + n;
                } else
                    for (int r = 0; r < nres; r++)
                        func[r] = r < n ? out[r] : LNIL;
                break;
            }
            if (func->t != LT_LFUNC)
                lua0_error("attempt to call a %s value", type_name(*func));
            if (nci >= MAX_CI)
                lua0_error("stack overflow (calls)");
            LClosure *ncl = func->u.p;
            const Proto *p = ncl->p;
            LVal *nb = func + 1;
            if (nb + p->maxstack + 2 > stack + STACK_SIZE)
                lua0_error("stack overflow");
            for (int r = nargs; r < p->maxstack; r++)
                nb[r] = LNIL;
            ci->pc = pc;
            CallInfo *n = &cis[nci++];
            n->cl = ncl, n->func = func, n->base = nb, n->pc = p->code, n->nresults = nres;
            RELOAD();
            break;
        }
        case OP_RETURN: {
            int b = GET_B(i);
            LVal *src = RA;
            int n = b ? b - 1 : (int)(top - src);
            close_upvals(base);
            LVal *dst = ci->func;
            int want = ci->nresults;
            nci--;
            if (want < 0) {
                for (int r = 0; r < n; r++)
                    dst[r] = src[r];
                top = dst + n;
            } else
                for (int r = 0; r < want; r++)
                    dst[r] = r < n ? src[r] : LNIL;
            if (nci == entry)
                return;
            RELOAD();
            break;
        }
        case OP_FORLOOP: {
            float step = tonum(RA[2], "for step"), idx = tonum(RA[0], "for index") + step, lim = tonum(RA[1], "for limit");
            if (step > 0 ? idx <= lim : idx >= lim) {
                pc += GET_SBX(i);
                RA[0] = lnum(idx);
            }
            break;
        }
        case OP_SETLIST: {
            if (RA->t != LT_TABLE)
                lua0_error("SETLIST on a non-table");
            int bc = GET_BX(i), n = (bc & (FPF - 1)) + 1;
            bc &= ~(FPF - 1);
            for (; n > 0; n--)
                tset(RA->u.h, lnum((float)(bc + n)), RA[n]);
            break;
        }
        case OP_CLOSE: close_upvals(RA); break;
        case OP_CLOSURE: {
            const Proto *p = cl->p->p[GET_BX(i)];
            LClosure *ncl = new_lclosure(p);
            for (int j = 0; j < p->nups; j++) {
                unsigned ps = *pc++;
                ncl->up[j] = GET_OP(ps) == OP_GETUPVAL ? cl->up[GET_B(ps)] : find_upval(base + GET_B(ps));
            }
            LVal v = {LT_LFUNC, 0, {0}};
            v.u.p = ncl;
            *RA = v;
            break;
        }
        default:
            lua0_error("unsupported opcode %d in %s:%u", GET_OP(i), cl->p->chunk, cl->p->line);
        }
    }
}

int lua0_call(LVal f, const LVal *args, int nargs, LVal *ret, int nret)
{
    int entry = nci;
    LVal *fbase = nci ? cis[nci - 1].base + cis[nci - 1].cl->p->maxstack : stack;
    if (nci && top > fbase)
        fbase = top;
    jmp_buf jb, *prev = err_jmp;
    LVal *saved_top = top;
    err_jmp = &jb;
    if (setjmp(jb)) {
        close_upvals(fbase);
        nci = entry, top = saved_top, err_jmp = prev;
        return -1;
    }
    fbase[0] = f;
    for (int a = 0; a < nargs; a++)
        fbase[1 + a] = args[a];
    int n;
    if (f.t == LT_CFUNC) {
        CClosure *cc = f.u.p;
        LVal out[8];
        n = cc->f(fbase + 1, nargs, out, &cc->up);
        for (int r = 0; r < nret && r < n; r++)
            ret[r] = out[r];
    } else if (f.t == LT_LFUNC) {
        LClosure *cl = f.u.p;
        if (nci >= MAX_CI || fbase + 1 + cl->p->maxstack + 2 > stack + STACK_SIZE)
            lua0_error("stack overflow");
        for (int r = nargs; r < cl->p->maxstack; r++)
            fbase[1 + r] = LNIL;
        CallInfo *c = &cis[nci++];
        c->cl = cl, c->func = fbase, c->base = fbase + 1, c->pc = cl->p->code, c->nresults = -1;
        execute(entry);
        n = (int)(top - fbase);
        for (int r = 0; r < nret && r < n; r++)
            ret[r] = fbase[r];
    } else
        lua0_error("attempt to call a %s value", type_name(f));
    top = saved_top, err_jmp = prev;
    return n;
}

/* ---- GC ---- */

static void mark_obj(GCH *o);

void lua0_mark(LVal v)
{
    if (v.t == LT_STR || v.t == LT_TABLE || v.t == LT_LFUNC || v.t == LT_CFUNC)
        mark_obj(v.u.p);
}

static void mark_obj(GCH *o)
{
    if (!o || o->mark == gc_cur)
        return;
    o->mark = gc_cur;
    switch (o->type) {
    case GC_TABLE: {
        LTable *t = (LTable *)o;
        for (unsigned i = 0; i < t->asize; i++)
            lua0_mark(t->arr[i]);
        for (unsigned i = 0; i < t->hsize; i++)
            if (t->node[i].key.t != LT_NIL)
                lua0_mark(t->node[i].key), lua0_mark(t->node[i].val);
        break;
    }
    case GC_LCL: {
        LClosure *c = (LClosure *)o;
        for (int i = 0; i < c->nups; i++)
            mark_obj(&c->up[i]->h);
        break;
    }
    case GC_CCL: lua0_mark(((CClosure *)o)->up); break;
    case GC_UPVAL: lua0_mark(*((UpVal *)o)->v); break;
    }
}

static unsigned now_ticks(void)
{
    unsigned t;
    __asm__ volatile("mfc0 %0, $9" : "=r"(t));
    return t;
}

/* Mark (atomic, between script calls): roots are the globals, the method tables and extra_roots. Fixed objects (the
 * globals table, registered C functions, strings in loaded code) are never freed, so they need no walk of allgc:
 * the sweep keeps them, and the only one with children (globals) is a root. */
static void gc_mark(void (*extra_roots)(void))
{
    unsigned t0 = now_ticks();
    gc_before = lua0_heap, gc_since = lua0_since_gc;
    gc_cur ^= 1;
    lua0_mark(globals);
    for (int i = 0; i < 8; i++)
        lua0_mark(methods[i]);
    if (extra_roots)
        extra_roots();
    lua0_since_gc = 0;
    sweep_pp = &allgc;
    gc_mark_us = (now_ticks() - t0) / 295, gc_sweep_us = gc_step_max_us = gc_steps = 0;
}

/* Free unmarked objects from sweep_pp on until the list ends (returns 1) or `budget` ticks have passed (0 = no
 * limit). Objects allocated since the mark go in at the head of allgc, behind the sweep, and are live. */
static int gc_sweep(unsigned budget)
{
    unsigned t0 = now_ticks(), n = 0;
    GCH **pp = sweep_pp;
    while (*pp) {
        GCH *o = *pp;
        if (o->mark == gc_cur || o->fixed) {
            pp = &o->next;
        } else {
            *pp = o->next;
            unsigned size = 0;
            switch (o->type) {
            case GC_STR: {
                LStr *s = (LStr *)o;
                LStr **b = &strt[s->hash & (strt_size - 1)];
                while (*b != s)
                    b = &(*b)->hnext;
                *b = s->hnext;
                strt_count--;
                size = sizeof(LStr) + s->len;
                break;
            }
            case GC_TABLE: {
                LTable *t = (LTable *)o;
                size = sizeof(LTable) + t->asize * sizeof(LVal) + t->hsize * sizeof(Node);
                lrel(t->arr), lrel(t->node);
                break;
            }
            case GC_LCL: size = sizeof(LClosure) + sizeof(UpVal *) * ((LClosure *)o)->nups; break;
            case GC_CCL: size = sizeof(CClosure); break;
            case GC_UPVAL: size = sizeof(UpVal); break;
            }
            lua0_heap -= size < lua0_heap ? size : lua0_heap;
            lrel(o);
        }
        if (budget && !(++n & 63) && now_ticks() - t0 > budget)
            break;
    }
    sweep_pp = pp;
    unsigned us = (now_ticks() - t0) / 295;
    gc_sweep_us += us, gc_steps++;
    if (us > gc_step_max_us)
        gc_step_max_us = us;
    if (*pp)
        return 0;
    sweep_pp = NULL;
    /* one line per collection: the heap-drift check (TECH_DEBT world-5) reads free heap right after a sweep, where
     * the VM holds only live objects and what was allocated during the sweep; between sweeps up to LUA_GC_KB of
     * garbage (ai.c ai_end_frame) is still held. "in" = the mark (one frame) + the sweep (spread over frames) */
    struct mallinfo mi = mallinfo();
    printf("lua0: gc %uK -> %uK (+%uK since) in %u us, pool %uK used %uK free, malloc %uK, heap free %uK in-use %uK"
           " (mark %u us, sweep %u us in %u steps, longest %u us)\n",
           gc_before >> 10, lua0_heap >> 10, gc_since >> 10, gc_mark_us + gc_sweep_us, lua0_pool_used >> 10,
           lua0_pool_free >> 10, lua0_malloc_live >> 10, (unsigned)mi.fordblks >> 10, (unsigned)mi.uordblks >> 10,
           gc_mark_us, gc_sweep_us, gc_steps, gc_step_max_us);
    return 1;
}

void lua0_gc(void (*extra_roots)(void))
{
    if (nci)
        return;
    if (sweep_pp)
        gc_sweep(0);  /* finish the cycle in progress: its mark is stale for the full one below */
    gc_mark(extra_roots);
    gc_sweep(0);
}

int lua0_gc_step(void (*extra_roots)(void), unsigned threshold, unsigned budget_us)
{
    if (nci)
        return 0;
    if (!sweep_pp) {
        if (lua0_since_gc <= threshold)
            return 0;
        gc_mark(extra_roots);
    }
    gc_sweep(budget_us * 295u);
    return 1;
}

int lua0_gc_busy(void) { return sweep_pp != NULL; }

/* ---- base library (what DSR's scripts use: math.abs, table.getn; loadstring is installed by the AI) ---- */

static int l_abs(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    float x = n > 0 ? tonum(a[0], "abs") : 0;
    o[0] = lnum(x < 0 ? -x : x);
    return 1;
}

static int l_getn(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    if (n < 1 || a[0].t != LT_TABLE)
        lua0_error("table.getn: table expected");
    o[0] = lnum((float)tgetn(a[0].u.h));
    return 1;
}

static int l_floor(LVal *a, int n, LVal *o, const LVal *up)
{
    (void)up;
    float x = n > 0 ? tonum(a[0], "floor") : 0;
    int f = (int)x;
    o[0] = lnum((float)(f > x ? f - 1 : f));
    return 1;
}

int lua0_init(void)
{
    if (!lp)
        lp = memalign(16, LP_SIZE);  /* at boot, before the near-cell arena (the AI scripts load in enemy_init) */
    strt_size = 1024;
    strt = calloc(strt_size, sizeof *strt);
    globals = tval(new_table(0, 512));
    ((GCH *)globals.u.p)->fixed = 1;
    LVal m = lua0_newtable(), t = lua0_newtable();
    lua0_setglobal("math", m);
    lua0_setglobal("table", t);
    lua0_set(m, lua0_str("abs"), lua0_cfunc(l_abs, LNIL));
    lua0_set(m, lua0_str("floor"), lua0_cfunc(l_floor, LNIL));
    lua0_set(t, lua0_str("getn"), lua0_cfunc(l_getn, LNIL));
    top = stack;
    return 1;
}
