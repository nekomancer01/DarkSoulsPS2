/* mem.h: memory regions (Polaris 1.2). One page allocator interface and the region kinds the engine allocates from,
 * plus the state machine that tells boot / load / play apart for the malloc-in-play check.
 *
 * Kinds:
 *   pages   MemPages: fixed-size pages over a block the caller hands in (first fit of n contiguous pages, or marks
 *           by a caller with its own placement). Knows nothing of the streamer: mapstream's arena (msarena.c) is an
 *           instance of it since Polaris 4.1, with its 256-byte granules as the pages.
 *   bump    MemBump: a stack with marks over one block; the building block of the regions below.
 *   static  boot lifetime (params, item / menu / help tables, ESD programs): mem_static. Reserved generously at boot,
 *           shrunk in place at mem_boot_seal (the base must not move), the tail goes back to the heap. Sizes come
 *           from the player's game data (version, language), so nothing here is a measured constant.
 *   temp    boot-temp: loads freed after use (pack staging and block-size tables, the area's AI pack until the VM has
 *           its copy): mem_temp / mem_temp_mark / mem_temp_reset, released whole at mem_boot_seal. Later it serves
 *           loading screens and warps.
 *   frame   two bump buffers swapped once a frame (mem_frame_swap). Buffer N is reset only after the DMA chains built
 *           from it have been waited on (the swap runs after vu1's wait for the previous frame), so anything a chain
 *           references lives until then. An overflow falls back to malloc ("TEST warn", counted) and is freed at the
 *           buffer's next reset: never NULL into a chain. The two VU1 chain buffers are not in it (Polaris 1.4).
 *   pool    MemPool: fixed-size slots with a free list and a generation per slot (handles, Polaris 5.1).
 *   block   lifetime of a world block: the interface only here (its pages come from the streamer's arena, a
 *           MemPages instance since Polaris 4.1; no new boot reservation, which would shrink the arena).
 *   scratchpad  the EE's 16 KB scratchpad RAM as a claimed resource: one claimant at a time, nothing kept across a
 *           release, never a DMA REF source. No user yet (the first is packet building, Polaris 1.4).
 *
 * States: MEM_BOOT until mem_boot_seal, MEM_LOAD while a loading screen is up (warps, deaths, bonfire reloads),
 * MEM_PLAY otherwise. hitch.c's malloc / calloc / realloc / memalign / free wrappers count every call per state and,
 * in play, per legacy tag: the code that still allocates from the heap while playing wraps itself in
 * mem_legacy_begin(tag) / mem_legacy_end() (MEM_TAGS below, a capped table that later phases shrink). An untagged
 * allocation in play is counted as such; the MEM_STRICT build prints a "TEST error" with its caller. Region calls and
 * the heap wrappers must come from the main thread (counted, MEM_STRICT errors otherwise). The counters are one
 * increment each: no string work on the hot path. */
#ifndef MEM_H
#define MEM_H

#include <stddef.h>

/* ---- pages ---- */
typedef struct {
    unsigned char *base;
    unsigned page, npages, used, peak;
    unsigned *bits;  /* npages bits, 1 = taken; (npages + 31) / 32 words from the caller */
} MemPages;
void mem_pages_init(MemPages *m, void *base, unsigned bytes, unsigned page, unsigned *bits);
void *mem_pages_alloc(MemPages *m, unsigned n);
void mem_pages_free(MemPages *m, void *p, unsigned n);
/* page i taken; mark pages [first, first + n) taken (on) or free, counting used / peak (an allocator with its own
 * placement on top: the streamer's arena, msarena.c, whose pages here are its 256-byte granules) */
static inline int mem_pages_taken(const MemPages *m, unsigned i) { return m->bits[i >> 5] >> (i & 31) & 1; }
void mem_pages_mark(MemPages *m, unsigned first, unsigned n, int on);

/* ---- bump ---- */
typedef struct {
    unsigned char *base;
    unsigned size, used, peak, fails;
} MemBump;
void mem_bump_init(MemBump *b, void *base, unsigned size);
void *mem_bump_alloc(MemBump *b, unsigned n, unsigned align);  /* NULL when full (the caller falls back) */
static inline unsigned mem_bump_mark(const MemBump *b) { return b->used; }
static inline void mem_bump_reset(MemBump *b, unsigned mark) { b->used = mark < b->used ? mark : b->used; }

/* ---- pool ---- */
typedef struct {
    unsigned char *slots;
    unsigned short *gen;  /* per slot, bumped on free: a handle is (index, generation) */
    unsigned size, n, free_head, used, peak;
} MemPool;
/* slots: n x size bytes (size a multiple of 4, at least 4), gen: n shorts */
void mem_pool_init(MemPool *p, void *slots, unsigned short *gen, unsigned size, unsigned n);
void *mem_pool_alloc(MemPool *p);  /* NULL when full */
void mem_pool_free(MemPool *p, void *slot);
static inline unsigned mem_pool_index(const MemPool *p, const void *slot)
{
    return (unsigned)((const unsigned char *)slot - p->slots) / p->size;
}

/* ---- the engine's regions ---- */
typedef enum { MEM_BOOT, MEM_LOAD, MEM_PLAY, MEM_NSTATES } MemState;
extern MemState mem_state;
void mem_set_state(MemState s);

/* boot: reserve the static and boot-temp regions (main, before the first pack load) */
void mem_boot_init(void);
/* boot lifetime, align a power of two (128 for DMA sources, as pack_load's buffers); falls back to the heap (logged)
 * once the reservation is full or after the seal */
void *mem_static(unsigned n, unsigned align);
void *mem_temp(unsigned n, unsigned align);  /* NULL after the seal or when full (the caller uses the heap) */
unsigned mem_temp_mark(void);
void mem_temp_reset(unsigned mark);
int mem_in_temp(const void *p);
int mem_in_static(const void *p);
/* free a pointer that may live in a region (static, boot-temp: nothing to do) or in the heap */
void mem_release(void *p);
/* shrink static in place, release boot-temp, state -> MEM_LOAD (loading.c moves to MEM_PLAY when the boot loading
 * screen closes). Before the world arena is made (world_systems), so
 * the arena sees the heap it saw before regions existed */
void mem_boot_seal(void);
/* after the world arena is made: boot-temp's last contents (the AI packs the VM copied) go back to the heap, where main
 * freed them */
void mem_boot_done(void);

/* frame */
void *mem_frame(unsigned n, unsigned align);
void mem_frame_swap(void);  /* after the previous frame's chains were waited on (main loop) */

/* scratchpad: one claimant at a time */
void *mem_spr_claim(const char *who);
void mem_spr_release(void);

/* legacy heap use in play, by scope (one tag at a time; nesting keeps the outer tag) */
#define MEM_TAGS(X) X(NONE) X(WORLD) X(ENEMY) X(OBJ) X(EVENT) X(FX) X(SND) X(UI) X(PLAYER) X(SAVE) X(TEST)
#define MEM_TAG_ENUM(t) MEMT_##t,
typedef enum { MEM_TAGS(MEM_TAG_ENUM) MEMT_N } MemTag;
#undef MEM_TAG_ENUM
extern unsigned char mem_tag;
static inline unsigned mem_legacy_begin(MemTag t)
{
    unsigned old = mem_tag;
    if (!old)
        mem_tag = (unsigned char)t;
    return old;
}
static inline void mem_legacy_end(unsigned old) { mem_tag = (unsigned char)old; }
/* MEM_LEGACY(WORLD) { ... }: the block runs once with that tag */
#define MEM_LEGACY(t) for (unsigned mt_ = mem_legacy_begin(MEMT_##t), mo_ = 1; mo_; mo_ = 0, mem_legacy_end(mt_))

/* hitch.c's heap wrappers: count one call (kind 0 alloc, 1 free); ra = the caller */
void mem_count(int kind, void *ra);
/* one "mem:" log line (hitch.c, each period) */
void mem_log(void);
/* debug HUD: static KB, frame high-water KB (both buffers), heap allocations in play since boot */
void mem_hud(unsigned *static_kb, unsigned *frame_kb, unsigned *play_allocs);

#endif
