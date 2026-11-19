/* Host test of src/mem.c's pages / bump / pool parts (Polaris 1.2): tools/ci/test_mem.sh */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "mem.h"

int main(void)
{
    /* pages: first fit of contiguous runs, free, reuse */
    static unsigned char block[16 * 4096];
    unsigned bits[1];
    MemPages m;
    mem_pages_init(&m, block, sizeof block, 4096, bits);
    assert(m.npages == 16);
    unsigned char *a = mem_pages_alloc(&m, 3), *b = mem_pages_alloc(&m, 5), *c = mem_pages_alloc(&m, 8);
    assert(a == block && b == block + 3 * 4096 && c == block + 8 * 4096 && m.used == 16);
    assert(!mem_pages_alloc(&m, 1));
    mem_pages_free(&m, b, 5);
    assert(mem_pages_alloc(&m, 4) == b && m.used == 15 && m.peak == 16);
    /* bump: alignment, marks, full */
    static unsigned char bb[1024];
    MemBump r;
    mem_bump_init(&r, bb, sizeof bb);
    void *x = mem_bump_alloc(&r, 3, 16), *y = mem_bump_alloc(&r, 10, 128);
    assert(x == bb && ((size_t)y & 127) == 0);
    unsigned mk = mem_bump_mark(&r);
    assert(mem_bump_alloc(&r, 500, 16));
    mem_bump_reset(&r, mk);
    assert(r.used == mk && r.peak > mk);
    assert(!mem_bump_alloc(&r, 2000, 16) && r.fails == 1);
    /* pool: slots, generations, reuse order */
    static unsigned char slots[4 * 32];
    unsigned short gen[4];
    MemPool p;
    mem_pool_init(&p, slots, gen, 32, 4);
    void *s[4];
    for (int i = 0; i < 4; i++)
        s[i] = mem_pool_alloc(&p), assert(s[i] && mem_pool_index(&p, s[i]) == (unsigned)i);
    assert(!mem_pool_alloc(&p) && p.used == 4);
    mem_pool_free(&p, s[2]);
    assert(gen[2] == 1 && mem_pool_alloc(&p) == s[2] && p.peak == 4);
    printf("test_mem: ok\n");
    return 0;
}
