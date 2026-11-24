/* Host test of src/sndres.c's eviction order and quarantine-aware hole walk (TECH_DEBT audio-2): tools/ci/test_sndres.sh */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sndres.h"

int main(void)
{
    SndResBank b[6];

    /* victim: skips keep / busy / loading / guarded / unused, not-wanted before wanted, then oldest */
    memset(b, 0, sizeof b);
    for (int i = 0; i < 6; i++) {
        b[i].used = 1;
        b[i].size = 64;
        b[i].last_used = 100 + i;
    }
    b[0].keep = 1;
    b[1].busy = 1;
    b[2].loading = 1;
    b[3].guard_until = 50;
    b[4].used = 0;
    assert(sndres_victim(b, 6, 10, 1, 0) == 5);  /* only 5 qualifies at frame 10... */
    assert(sndres_victim(b, 6, 50, 1, 0) == 3);  /* ...3 once its guard is over (older than 5) */
    b[3].wanted = 1;
    assert(sndres_victim(b, 6, 50, 1, 0) == 5);  /* not-wanted first */
    assert(sndres_victim(b, 6, 50, 0, 0) == 5);
    b[5].wanted = 1;
    assert(sndres_victim(b, 6, 50, 0, 0) == -1);  /* wanted_too = 0 */
    assert(sndres_victim(b, 6, 50, 1, 0) == 3);   /* both wanted: oldest */
    b[3].size = 0;
    assert(sndres_victim(b, 6, 50, 1, 1) == 5);   /* for_room skips size 0 */
    assert(sndres_victim(b, 6, 50, 1, 0) == 3);
    assert(sndres_victim(b, 0, 50, 1, 0) == -1);

    /* hole: first fit, 64-aligned, quarantine */
    memset(b, 0, sizeof b);
    b[0].used = 1; b[0].spu = 0x1000; b[0].size = 0x100;
    b[1].used = 1; b[1].spu = 0x1200; b[1].size = 0x100;
    b[2].spu = 0x1100; b[2].size = 0x100; b[2].dead_frame = 10;  /* freed at 10: occupied through 12 */
    assert(sndres_hole(b, 3, 0x1000, 0x2000, 0x80, 12, 0) == 0x1300);
    assert(sndres_hole(b, 3, 0x1000, 0x2000, 0x80, 13, 0) == 0x1100);
    assert(sndres_hole(b, 3, 0x1000, 0x2000, 0x80, 12, 1) == 0x1100);
    assert(sndres_hole(b, 3, 0x1000, 0x2000, 0x100, 13, 0) == 0x1100);
    assert(sndres_hole(b, 3, 0x1000, 0x2000, 0x200, 13, 0) == 0x1300);
    assert(sndres_hole(b, 3, 0x1000, 0x1300, 0x200, 13, 0) == 0);  /* none */
    b[0].size = 0x90;  /* ends unaligned: next start rounds up to 64 */
    b[2].size = 0;
    assert(sndres_hole(b, 3, 0x1000, 0x2000, 0x40, 99, 0) == 0x10c0);  /* 0x1090 rounded up to 64 */
    b[3].used = 1; b[3].spu = 0; b[3].size = 0x500;  /* spu 0: being placed, ignored */
    assert(sndres_hole(b, 4, 0x1000, 0x2000, 0x40, 99, 0) == 0x10c0);

    /* largest hole */
    memset(b, 0, sizeof b);
    b[0].used = 1; b[0].spu = 0x1000; b[0].size = 0x100;
    b[1].used = 1; b[1].spu = 0x1400; b[1].size = 0x100;
    b[2].spu = 0x1100; b[2].size = 0x100; b[2].dead_frame = 10;
    assert(sndres_largest_hole(b, 3, 0x1000, 0x2000, 11) == 0xB00);  /* 0x1500..0x2000 */
    assert(sndres_largest_hole(b, 3, 0x1000, 0x1500, 11) == 0x200);  /* 0x1200..0x1400 (0x1100 still quarantined) */
    assert(sndres_largest_hole(b, 3, 0x1000, 0x1500, 13) == 0x300);  /* 0x1100..0x1400 */
    assert(sndres_largest_hole(b, 0, 0x1000, 0x2000, 0) == 0x1000);
    puts("test_sndres: ok");
    return 0;
}
