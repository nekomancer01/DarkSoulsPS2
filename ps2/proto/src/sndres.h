/* sndres.h: the SPU2 sample heap's residency decisions over a plain array (sound.c feeds it; host test
 * tools/ci/test_sndres.sh). TECH_DEBT audio-2. */
#ifndef SNDRES_H
#define SNDRES_H

typedef struct {
    unsigned spu, size;          /* placed range; size 0 = no SPU2 range */
    unsigned last_used;          /* frame of the last key-on / stream open */
    unsigned dead_frame;         /* frame it was freed (quarantine: occupied until dead_frame + 2) */
    unsigned guard_until;        /* not a victim before this frame (placed by a retry) */
    unsigned char used, keep, wanted, busy, loading;
} SndResBank;

/* the bank to evict: used, !keep, !busy, !loading, frame >= guard_until, and size > 0 when for_room;
 * !wanted before wanted, then min last_used; -1 none. wanted_too = 0 considers only !wanted banks. */
int sndres_victim(const SndResBank *b, int n, unsigned frame, int wanted_too, int for_room);
/* first-fit start of size bytes in [lo, hi), 64-aligned; 0 none. A bank is occupied when used, or when freed
 * at most 2 frames ago and !ignore_quarantine. */
unsigned sndres_hole(const SndResBank *b, int n, unsigned lo, unsigned hi, unsigned size, unsigned frame,
                     int ignore_quarantine);
/* the victims (indices in out, sndres_victim order with for_room) whose eviction leaves a hole of size bytes in
 * [lo, hi) once their quarantine ends: their count, 0 when a hole exists already (quarantine ignored), -1 when no set
 * of victims opens one (then nothing should be evicted). b is a scratch copy: the chosen victims are marked unused. */
int sndres_plan(SndResBank *b, int n, unsigned lo, unsigned hi, unsigned size, unsigned frame, int wanted_too,
                int *out);
unsigned sndres_largest_hole(const SndResBank *b, int n, unsigned lo, unsigned hi, unsigned frame);

#endif
