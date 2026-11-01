/* Event flag store (flags.h): DSR's id layout over a lazily filled page pool */
#include <stdio.h>
#include <string.h>
#include "flags.h"

#define PAGE_WORDS 32
#define NO_PAGE 0xFFFF

/* Area code -> block index inside a group (DSR flag array, 0x500 bytes per block) */
static const u16 AREA_CODES[18] = {0, 100, 101, 102, 110, 120, 121, 130, 131, 132, 140, 141, 150, 151, 160, 170, 180, 181};

static u32 pool[FLAG_PAGES][PAGE_WORDS];
static u16 page_of[FLAG_SECTIONS];  /* section slot -> pool page or NO_PAGE */
static u16 slot_of[FLAG_PAGES];     /* pool page -> section slot */
static int pages_used;
static unsigned changes;
static FlagChangeFn change_cb;
static int warned_full;

/* Section slot of an id (0..FLAG_SECTIONS-1), or -1 outside the layout */
static int flag_slot(u32 id)
{
    if (id >= 100000000u)
        return -1;
    int g = id / 10000000, area = (id / 10000) % 1000, sec = (id / 1000) % 10;
    int gi;
    switch (g) {
    case 0: gi = 0; break;
    case 1: gi = 1; break;
    case 5: gi = 2; break;
    case 6: gi = 3; break;
    case 7: gi = 4; break;
    default: return -1;
    }
    int ai = -1;
    for (int i = 0; i < 18; i++)
        if (AREA_CODES[i] == area) {
            ai = i;
            break;
        }
    if (ai < 0 || (gi == 0 && ai != 0))
        return -1;
    int block = gi == 0 ? 0 : 1 + (gi - 1) * 18 + ai;
    return block * 10 + sec;
}

void flags_init(void)
{
    memset(pool, 0, sizeof pool);
    memset(page_of, 0xFF, sizeof page_of);
    pages_used = 0;
    warned_full = 0;
    changes++;
}

int flag_get(u32 id)
{
    int s = flag_slot(id);
    if (s < 0 || page_of[s] == NO_PAGE)
        return 0;
    unsigned n = id % 1000;
    return (pool[page_of[s]][n >> 5] >> (31 - (n & 31))) & 1;
}

void flag_set(u32 id, int on)
{
    int s = flag_slot(id);
    if (s < 0)
        return;
    unsigned p = page_of[s];
    if (p == NO_PAGE) {
        if (!on)
            return;
        if (pages_used >= FLAG_PAGES) {
            if (!warned_full++)
                printf("flags: OUT OF PAGES (%d), flag %u dropped\n", FLAG_PAGES, (unsigned)id);
            return;
        }
        p = pages_used++;
        page_of[s] = p;
        slot_of[p] = s;
    }
    unsigned n = id % 1000;
    u32 m = 0x80000000u >> (n & 31), *w = &pool[p][n >> 5];
    if (!!(*w & m) == !!on)
        return;
    *w ^= m;
    changes++;
    if (change_cb)
        change_cb(id, !!on);
}

void flag_range_set(u32 first, u32 last, int on)
{
    for (u32 id = first; id <= last && id >= first;) {
        if (flag_slot(id) < 0) {
            id = (id / 10000 + 1) * 10000;  /* invalid area / group: skip the rest of this section group */
            continue;
        }
        flag_set(id, on);
        id++;
    }
}

int flag_range_count(u32 first, u32 last)
{
    int c = 0;
    for (u32 id = first; id <= last && id >= first;) {
        int s = flag_slot(id);
        if (s < 0) {
            id = (id / 10000 + 1) * 10000;
            continue;
        }
        if (page_of[s] == NO_PAGE) {
            id = (id / 1000 + 1) * 1000;
            continue;
        }
        c += flag_get(id);
        id++;
    }
    return c;
}

u32 flag_value_get(u32 first, int nbits)
{
    u32 v = 0;
    for (int i = 0; i < nbits; i++)
        v = (v << 1) | flag_get(first + i);
    return v;
}

void flag_value_set(u32 first, int nbits, u32 value)
{
    for (int i = 0; i < nbits; i++)
        flag_set(first + i, (value >> (nbits - 1 - i)) & 1);
}

unsigned flags_changes(void) { return changes; }
void flags_set_callback(FlagChangeFn fn) { change_cb = fn; }
int flags_pages_used(void) { return pages_used; }
int flags_resident_bytes(void) { return sizeof pool + sizeof page_of + sizeof slot_of; }

int flags_save(void *dst, int cap)
{
    u8 *o = dst;
    int n = 0;
    for (int p = 0; p < pages_used; p++) {
        int nz = 0;
        for (int k = 0; k < PAGE_WORDS && !nz; k++)
            nz = pool[p][k] != 0;
        n += nz;
    }
    int size = 4 + n * (4 + PAGE_WORDS * 4);
    if (size > cap)
        return -1;
    u16 hdr[2] = {n, 0};
    memcpy(o, hdr, 4);
    o += 4;
    for (int p = 0; p < pages_used; p++) {
        int nz = 0;
        for (int k = 0; k < PAGE_WORDS && !nz; k++)
            nz = pool[p][k] != 0;
        if (!nz)
            continue;
        u16 h[2] = {slot_of[p], 0};
        memcpy(o, h, 4);
        memcpy(o + 4, pool[p], PAGE_WORDS * 4);
        o += 4 + PAGE_WORDS * 4;
    }
    return size;
}

int flags_load(const void *src, int len)
{
    const u8 *b = src;
    flags_init();
    if (len < 4)
        return -1;
    u16 hdr[2];
    memcpy(hdr, b, 4);
    int n = hdr[0];
    if (n > FLAG_PAGES || len < 4 + n * (4 + PAGE_WORDS * 4))
        return -1;
    for (int i = 0; i < n; i++) {
        const u8 *e = b + 4 + i * (4 + PAGE_WORDS * 4);
        u16 h[2];
        memcpy(h, e, 4);
        if (h[0] >= FLAG_SECTIONS || page_of[h[0]] != NO_PAGE) {
            flags_init();
            return -1;
        }
        page_of[h[0]] = pages_used;
        slot_of[pages_used] = h[0];
        memcpy(pool[pages_used], e + 4, PAGE_WORDS * 4);
        pages_used++;
    }
    changes++;
    return 0;
}
