/* mem.c: memory regions (mem.h). The pages / bump / pool parts use no PS2 header, so tools/ci/test_mem.sh builds
 * them on the host too. */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mem.h"

/* ---- pages ---- */

void mem_pages_init(MemPages *m, void *base, unsigned bytes, unsigned page, unsigned *bits)
{
    m->base = base, m->page = page, m->npages = bytes / page, m->used = m->peak = 0, m->bits = bits;
    memset(bits, 0, (m->npages + 31) / 32 * 4);
}

static int page_taken(const MemPages *m, unsigned i) { return m->bits[i >> 5] >> (i & 31) & 1; }

void *mem_pages_alloc(MemPages *m, unsigned n)
{
    for (unsigned i = 0, run = 0; n && i < m->npages; i++) {
        run = page_taken(m, i) ? 0 : run + 1;
        if (run == n) {
            for (unsigned k = i + 1 - n; k <= i; k++)
                m->bits[k >> 5] |= 1u << (k & 31);
            m->used += n;
            m->peak = m->used > m->peak ? m->used : m->peak;
            return m->base + (size_t)(i + 1 - n) * m->page;
        }
    }
    return NULL;
}

void mem_pages_free(MemPages *m, void *p, unsigned n)
{
    if (!p)
        return;
    unsigned i0 = (unsigned)((unsigned char *)p - m->base) / m->page;
    for (unsigned k = i0; k < i0 + n && k < m->npages; k++)
        if (page_taken(m, k))
            m->bits[k >> 5] &= ~(1u << (k & 31)), m->used--;
}

void mem_pages_mark(MemPages *m, unsigned first, unsigned n, int on)
{
    for (unsigned k = first; k < first + n && k < m->npages; k++) {
        unsigned b = 1u << (k & 31);
        if (on && !(m->bits[k >> 5] & b))
            m->bits[k >> 5] |= b, m->used++;
        else if (!on && (m->bits[k >> 5] & b))
            m->bits[k >> 5] &= ~b, m->used--;
    }
    m->peak = m->used > m->peak ? m->used : m->peak;
}

/* ---- bump ---- */

void mem_bump_init(MemBump *b, void *base, unsigned size)
{
    b->base = base, b->size = base ? size : 0, b->used = b->peak = b->fails = 0;
}

void *mem_bump_alloc(MemBump *b, unsigned n, unsigned align)
{
    align = align ? align : 16;
    size_t at = ((size_t)b->base + b->used + align - 1) & ~(size_t)(align - 1);
    unsigned end = (unsigned)(at - (size_t)b->base) + n;
    if (!b->base || end > b->size) {
        b->fails++;
        return NULL;
    }
    b->used = end;
    b->peak = end > b->peak ? end : b->peak;
    return (void *)at;
}

/* ---- pool ---- */

void mem_pool_init(MemPool *p, void *slots, unsigned short *gen, unsigned size, unsigned n)
{
    p->slots = slots, p->gen = gen, p->size = size, p->n = n, p->used = p->peak = 0;
    p->free_head = n ? 0 : ~0u;
    for (unsigned i = 0; i < n; i++)
        *(unsigned *)(p->slots + (size_t)i * size) = i + 1 < n ? i + 1 : ~0u, gen[i] = 0;
}

void *mem_pool_alloc(MemPool *p)
{
    if (p->free_head == ~0u)
        return NULL;
    unsigned char *s = p->slots + (size_t)p->free_head * p->size;
    p->free_head = *(unsigned *)s;
    p->used++;
    p->peak = p->used > p->peak ? p->used : p->peak;
    return s;
}

void mem_pool_free(MemPool *p, void *slot)
{
    if (!slot)
        return;
    unsigned i = mem_pool_index(p, slot);
    p->gen[i]++;
    *(unsigned *)slot = p->free_head;
    p->free_head = i;
    p->used--;
}

#ifndef MEM_HOST_TEST
/* ---- the engine's regions (EE) ---- */
#include <kernel.h>

/* Boot reservations. Static is above what the tables need (302 KB on the 2026-11 Steam DSR, English): their sizes
 * depend on the player's game version and language, and a table that does not fit takes the heap (counted in the
 * "mem:" line), so the reserve is a soft limit. Boot-temp holds pack_load's two 256 KB staging buffers plus the AI
 * packs. Both come back at mem_boot_seal, before the world arena is sized from the heap's free bytes; what is left is
 * one hole low in the heap, so they stay small: with 1536 + 1024 KB the largest free block at the routes' heap low
 * was up to 1 MB smaller than main's (golden routes, 2026-11-18). */
#define MEM_STATIC_RESERVE ((512 + 2 * MEM_FRAME_KB + 64) * 1024)  /* + the frame buffers + g2d's loading pair */
#define MEM_TEMP_RESERVE (640 * 1024)
/* each of the two frame buffers, from the static region. 0 until the first user (packet building, Polaris 1.4) sizes
 * it: 2 x 16 KB of idle buffers were 32 KB less heap at the Burg low (golden routes, 2026-11-18). With 0 every
 * mem_frame call takes the heap (TEST warn) */
#define MEM_FRAME_KB 208  /* g2d.c: WORLD 40 + UI 168; with g2d's 2 x 32 KB loading pair, the 2 x 240 KB gsKit's queue gave up (Polaris 1.6) */

MemState mem_state = MEM_BOOT;
unsigned char mem_tag;
static MemBump st, tmp, fr[2];
static int cur_fr, sealed, main_tid = -1;
static void *tmp_hold;
#define TMP_N 32
static struct { void *p; unsigned used; } tmp_blk[TMP_N];  /* boot-temp's blocks, newest last (mem_release pops) */
static unsigned tmp_n;  /* boot-temp's last contents between mem_boot_seal and mem_boot_done */
static unsigned st_fallback, st_fallback_n, st_reserved, st_tail, tmp_peak, fr_peak[2], fr_over;
#define FR_OVER_N 64
static void *fr_heap[2][FR_OVER_N];
static unsigned fr_nheap[2];
/* counters: heap calls per state (alloc, free), per legacy tag in play, untagged in play, from another thread */
static unsigned calls[MEM_NSTATES][2], play_tag[MEMT_N], play_untagged, other_thread;
static const char *const tag_name[MEMT_N] = {
#define MEM_TAG_NAME(t) #t,
    MEM_TAGS(MEM_TAG_NAME)
#undef MEM_TAG_NAME
};

static void check_thread(void)
{
    if (main_tid >= 0 && GetThreadId() != main_tid) {
        other_thread++;
#ifdef MEM_STRICT
        printf("TEST error: mem: region or heap call off the main thread (thread %d)\n", GetThreadId());
#endif
    }
}

void mem_set_state(MemState s)
{
    if (sealed || s == MEM_BOOT)
        mem_state = s;
}

void mem_boot_init(void)
{
    main_tid = GetThreadId();
    st_reserved = MEM_STATIC_RESERVE;
    void *s = memalign(128, st_reserved);
    void *t = memalign(128, MEM_TEMP_RESERVE);
    mem_bump_init(&st, s, s ? st_reserved : 0);
    mem_bump_init(&tmp, t, t ? MEM_TEMP_RESERVE : 0);
    for (int i = 0; i < 2; i++)
        mem_bump_init(&fr[i], MEM_FRAME_KB ? mem_static(MEM_FRAME_KB * 1024, 64) : NULL, MEM_FRAME_KB * 1024);
    if (!s || !t)
        printf("mem: boot reservations failed (static %p, temp %p): the heap serves them\n", s, t);
}

void *mem_static(unsigned n, unsigned align)
{
    check_thread();
    void *p = sealed ? NULL : mem_bump_alloc(&st, n, align);
    if (!p) {  /* full or sealed: the heap, as before regions */
        st_fallback += n, st_fallback_n++;
        p = memalign(align < 16 ? 16 : align, n);
    }
    return p;
}

void *mem_temp(unsigned n, unsigned align)
{
    check_thread();
    if (sealed)
        return NULL;
    unsigned before = tmp.used;
    void *p = mem_bump_alloc(&tmp, n, align);
    tmp_peak = tmp.peak;
    if (p && tmp_n < TMP_N)
        tmp_blk[tmp_n].p = p, tmp_blk[tmp_n++].used = before;
    return p;
}

unsigned mem_temp_mark(void) { return mem_bump_mark(&tmp); }
void mem_temp_reset(unsigned mark)
{
    mem_bump_reset(&tmp, mark);
    while (tmp_n && tmp_blk[tmp_n - 1].used >= mark)
        tmp_n--;
}
/* (after the seal the heap reuses boot-temp's range: no boot-temp pointer may outlive the seal) */
/* sealed: boot-temp's range lives on as tmp_hold (tmp_held bytes) until mem_boot_done frees it; a pointer into it is
 * still boot-temp's (mem_release must not free() the middle of that heap block) */
static unsigned tmp_held;
int mem_in_temp(const void *p)
{
    const unsigned char *q = p;
    if (tmp.base)
        return q >= tmp.base && q < tmp.base + tmp.size;
    return tmp_hold && q >= (const unsigned char *)tmp_hold && q < (const unsigned char *)tmp_hold + tmp_held;
}
int mem_in_static(const void *p) { return st.base && (const unsigned char *)p >= st.base && (const unsigned char *)p < st.base + st.size; }

void mem_boot_done(void)
{
    free(tmp_hold);
    tmp_hold = NULL, tmp_held = 0;
}

void mem_release(void *p)
{
    if (p && tmp_n && p == tmp_blk[tmp_n - 1].p)  /* the newest boot-temp block: popped (the common AI pack) */
        mem_temp_reset(tmp_blk[tmp_n - 1].used);
    else if (p && !mem_in_temp(p) && !mem_in_static(p))
        free(p);
}

void mem_boot_seal(void)
{
    if (sealed)
        return;
    /* (what is left in boot-temp is dead by contract: the area's AI pack and the common one, both copied by the VM) */
    unsigned tmp_left = tmp.used;
    /* boot-temp shrinks to what is still in it and goes at mem_boot_done, after the arena is sized: main freed the
     * area's AI pack after world_systems too, so the arena sees the heap it saw before regions (iso_burg_taurus with
     * enemies: freeing it here grew the arena 9216 -> 9408 KB and took 204 KB off the heap low) */
    if (tmp.base && tmp_left) {
        void *q = realloc(tmp.base, (tmp_left + 127) & ~127u);
        if (q != tmp.base)
            printf("TEST error: mem: boot-temp moved on shrink\n");
        tmp_hold = q, tmp_held = tmp_left;
    } else
        free(tmp.base);
    mem_bump_init(&tmp, NULL, 0);
    tmp_n = 0;  /* (no block is popped after the seal: mem_release leaves what lies in tmp_hold to mem_boot_done) */
    if (st.base) {
        unsigned keep = (st.used + 127) & ~127u;
        void *q = realloc(st.base, keep ? keep : 128);  /* shrink in place: the tables point into it */
        if (q != st.base)
            printf("TEST error: mem: static region moved on shrink (%p -> %p)\n", (void *)st.base, q);
        st_tail = st.size - keep;
        st.size = keep;
    }
    sealed = 1;
    mem_state = MEM_LOAD;  /* the boot loading screen is still up: loading.c's close is the start of play */
    printf("mem: sealed: static %u KB used of %u KB (tail %u KB returned), heap fallback %u KB in %u, boot-temp peak "
           "%u KB of %u KB (%u KB still in it, released)\n",
           st.used >> 10, st_reserved >> 10, st_tail >> 10, st_fallback >> 10, st_fallback_n, tmp_peak >> 10,
           MEM_TEMP_RESERVE >> 10, tmp_left >> 10);
}

void *mem_frame(unsigned n, unsigned align)
{
    check_thread();
    void *p = mem_bump_alloc(&fr[cur_fr], n, align);
    if (!p) {  /* never NULL into a chain: the heap, freed at this buffer's next reset */
        fr_over++;
        p = memalign(align < 16 ? 16 : align, n);
        if (p && fr_nheap[cur_fr] < FR_OVER_N)
            fr_heap[cur_fr][fr_nheap[cur_fr]++] = p;
        else if (p)
            printf("TEST error: mem: frame overflow list full (%u B leaked)\n", n);
        if (fr_over <= 4)
            printf("TEST warn: mem: frame buffer full (%u KB), %u B from the heap\n", MEM_FRAME_KB, n);
    }
    return p;
}

void mem_frame_swap(void)
{
    cur_fr ^= 1;  /* the buffer filled two frames ago: its chains were waited on before this call */
    fr_peak[cur_fr] = fr[cur_fr].peak;
    for (unsigned i = 0; i < fr_nheap[cur_fr]; i++)
        free(fr_heap[cur_fr][i]);
    fr_nheap[cur_fr] = 0;
    mem_bump_reset(&fr[cur_fr], 0);
}

static const char *spr_who;
void *mem_spr_claim(const char *who)
{
    check_thread();
    if (spr_who) {
        printf("TEST error: mem: scratchpad claimed by %s while %s holds it\n", who, spr_who);
        return NULL;
    }
    spr_who = who;
    return (void *)0x70000000;
}
void mem_spr_release(void) { spr_who = NULL; }

void mem_count(int kind, void *ra)
{
    (void)ra;
    calls[mem_state][kind]++;
    if (main_tid >= 0 && GetThreadId() != main_tid)
        other_thread++;
    if (mem_state != MEM_PLAY || kind)
        return;
    if (mem_tag)
        play_tag[mem_tag]++;
    else {
        play_untagged++;
#ifdef MEM_STRICT
        static int busy;
        if (!busy && play_untagged <= 16) {  /* printf may allocate: no recursion */
            busy = 1;
            printf("TEST error: mem: heap allocation during play outside a legacy tag (caller 0x%08x)\n", (unsigned)ra);
            busy = 0;
        }
#endif
    }
}

void mem_log(void)
{
    char buf[256];
    int n = 0;
    for (int t = 1; t < MEMT_N && n < (int)sizeof buf - 24; t++)
        if (play_tag[t])
            n += snprintf(buf + n, sizeof buf - n, " %s %u", tag_name[t], play_tag[t]);
    buf[n] = 0;
    printf("mem: static %u KB (tail %u KB returned, heap fallback %u KB) boot-temp peak %u KB | frame hw %u / %u KB "
           "over %u | heap calls boot %u/%u load %u/%u play %u/%u | play allocs untagged %u%s | off-thread %u\n",
           st.used >> 10, st_tail >> 10, st_fallback >> 10, tmp_peak >> 10, fr[0].peak >> 10, fr[1].peak >> 10,
           fr_over, calls[MEM_BOOT][0], calls[MEM_BOOT][1], calls[MEM_LOAD][0], calls[MEM_LOAD][1], calls[MEM_PLAY][0],
           calls[MEM_PLAY][1], play_untagged, buf, other_thread);
}

void mem_hud(unsigned *static_kb, unsigned *frame_kb, unsigned *play_allocs)
{
    *static_kb = st.used >> 10;
    *frame_kb = (fr[0].peak > fr[1].peak ? fr[0].peak : fr[1].peak) >> 10;
    *play_allocs = calls[MEM_PLAY][0];
}
#endif
