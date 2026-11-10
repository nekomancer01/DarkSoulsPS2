/* lua0: a small Lua 5.0 virtual machine for DSR's compiled AI scripts (script/<name>.luabnd).
 *
 * Runs the original Lua 5.0 instructions, re-serialized offline by tools/lua50.py --pack (lua_Number float instead of
 * double, no debug info). Covers what DSR's AI uses: every 5.0 opcode except varargs / TFORLOOP / SETLISTO (none in
 * aiCommon or the map scripts), tables, closures with upvalues, interned strings, C functions with one upvalue,
 * light userdata with a per-tag method table (the ai / goal objects), and a mark-and-sweep GC that runs only
 * between script calls (lua0_gc). No metatables, coroutines, varargs or a compiler (loadstring is the engine's job).
 * See notes/PORTING.md "Enemy AI: Lua 5.0 VM". */
#ifndef LUA0_H
#define LUA0_H

enum { LT_NIL, LT_BOOL, LT_NUM, LT_STR, LT_TABLE, LT_LFUNC, LT_CFUNC, LT_UDATA };

typedef struct LStr LStr;
typedef struct LTable LTable;
typedef struct LVal {
    unsigned char t, tag;  /* tag: userdata type (method table index) */
    union {
        float n;
        int b;
        void *p;
        LStr *s;
        LTable *h;
    } u;
} LVal;

/* C function: args[0..nargs) (for a method call args[0] is self); write up to 8 results to out, return their
 * count. up = the function's upvalue (nil for plain registered functions). Errors: lua0_error. */
typedef int (*LCFunc)(LVal *args, int nargs, LVal *out, const LVal *up);

#define LNIL ((LVal){LT_NIL, 0, {0}})
static inline LVal lnum(float n) { LVal v = {LT_NUM, 0, {0}}; v.u.n = n; return v; }
static inline LVal lbool(int b) { LVal v = {LT_BOOL, 0, {0}}; v.u.b = b != 0; return v; }
static inline LVal lud(void *p, int tag) { LVal v = {LT_UDATA, (unsigned char)tag, {0}}; v.u.p = p; return v; }
static inline int ltruthy(LVal v) { return !(v.t == LT_NIL || (v.t == LT_BOOL && !v.u.b)); }

int lua0_init(void);
/* Load a lua50.py pack (all chunks) and run each main chunk in order; returns chunks run, -1 on a bad blob.
 * The blob may be freed afterwards. */
int lua0_load(const void *blob, int size);
LVal lua0_str(const char *s);
const char *lua0_cstr(LVal v);  /* string contents, or NULL */
LVal lua0_getglobal(const char *name);
void lua0_setglobal(const char *name, LVal v);
LVal lua0_newtable(void);
LVal lua0_get(LVal table, LVal key);
void lua0_set(LVal table, LVal key, LVal val);
LVal lua0_cfunc(LCFunc f, LVal up);  /* a C closure value (GC'd unless stored somewhere reachable) */
void lua0_register(const char *name, LCFunc f);
/* Method table for userdata of this tag (0..7): GETTABLE / SELF on such a userdata read from it. fallback is called
 * for a missing key (its upvalue = the key string) and returns a stand-in value. */
void lua0_methods(int tag, LVal table, LVal (*fallback)(int tag, LVal key));
/* Call f(args) protected: returns the number of results (<= nret stored in ret), or -1 after a script error
 * (message in lua0_errmsg). */
int lua0_call(LVal f, const LVal *args, int nargs, LVal *ret, int nret);
extern char lua0_errmsg[160];
void lua0_error(const char *fmt, ...);
/* Mark-and-sweep. Roots: globals, method tables, and whatever extra_roots marks via lua0_mark. Only between calls.
 * lua0_gc: a whole collection now (finishing any sweep in progress first). lua0_gc_step: once more than threshold bytes
 * were allocated since the last mark, mark (all at once) and then sweep at most budget_us per call, one call per frame,
 * until the cycle ends; returns 1 when it did GC work. */
void lua0_gc(void (*extra_roots)(void));
int lua0_gc_step(void (*extra_roots)(void), unsigned threshold, unsigned budget_us);
void lua0_mark(LVal v);
/* Heap bytes in use, and allocated since the last collection */
extern unsigned lua0_heap, lua0_since_gc;
extern unsigned lua0_instructions;  /* executed (profiling) */

#endif
