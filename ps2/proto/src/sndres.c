/* sndres.c: the SPU2 sample heap's residency decisions (eviction order, quarantine-aware hole walk) over a plain
 * array, so the host can test them (tools/ci/test_sndres.sh). No SIF, no globals, no floats. TECH_DEBT audio-2. */
#include "sndres.h"

int sndres_victim(const SndResBank *b, int n, unsigned frame, int wanted_too, int for_room)
{
    int best = -1;
    for (int i = 0; i < n; i++) {
        const SndResBank *k = &b[i];
        if (!k->used || k->keep || k->busy || k->loading || frame < k->guard_until)
            continue;
        if (for_room && !k->size)
            continue;
        if (k->wanted && !wanted_too)
            continue;
        if (best < 0 || (b[best].wanted && !k->wanted) ||
            (b[best].wanted == k->wanted && k->last_used < b[best].last_used))
            best = i;
    }
    return best;
}

/* the end of the occupied range covering [a, a + size), 0 when free (spu 0 = being placed, not a range) */
static unsigned occupied_end(const SndResBank *b, int n, unsigned a, unsigned size, unsigned frame, int ignore_q)
{
    for (int i = 0; i < n; i++) {
        const SndResBank *k = &b[i];
        int live = k->used || (!ignore_q && k->size && frame <= k->dead_frame + 2);
        if (live && k->size && k->spu && a < k->spu + k->size && k->spu < a + size)
            return k->spu + k->size;
    }
    return 0;
}

unsigned sndres_hole(const SndResBank *b, int n, unsigned lo, unsigned hi, unsigned size, unsigned frame,
                     int ignore_quarantine)
{
    for (unsigned a = (lo + 63) & ~63u; a + size <= hi;) {
        unsigned end = occupied_end(b, n, a, size, frame, ignore_quarantine);
        if (!end)
            return a;
        a = (end + 63) & ~63u;
    }
    return 0;
}

int sndres_plan(SndResBank *b, int n, unsigned lo, unsigned hi, unsigned size, unsigned frame, int wanted_too,
                int *out)
{
    int m = 0;
    while (!sndres_hole(b, n, lo, hi, size, frame, 1)) {
        int k = sndres_victim(b, n, frame, wanted_too, 1);
        if (k < 0)
            return -1;
        b[k].used = 0;
        out[m++] = k;
    }
    /* only the victims inside the hole found: the LRU order may have taken others first that it does not need */
    unsigned a = sndres_hole(b, n, lo, hi, size, frame, 1);
    int kept = 0;
    for (int i = 0; i < m; i++) {
        SndResBank *k = &b[out[i]];
        if (k->spu < a + size && a < k->spu + k->size)
            out[kept++] = out[i];
        else
            k->used = 1;
    }
    return kept;
}

unsigned sndres_largest_hole(const SndResBank *b, int n, unsigned lo, unsigned hi, unsigned frame)
{
    unsigned best = 0;
    for (unsigned a = (lo + 63) & ~63u; a < hi;) {
        unsigned end = occupied_end(b, n, a, 1, frame, 0);
        if (end) {
            a = (end + 63) & ~63u;
            continue;
        }
        unsigned next = hi;  /* the run ends at the nearest occupied start past a */
        for (int i = 0; i < n; i++) {
            const SndResBank *k = &b[i];
            int live = k->used || (k->size && frame <= k->dead_frame + 2);
            if (live && k->size && k->spu > a && k->spu < next)
                next = k->spu;
        }
        if (next - a > best)
            best = next - a;
        a = (next + 63) & ~63u;
    }
    return best;
}
