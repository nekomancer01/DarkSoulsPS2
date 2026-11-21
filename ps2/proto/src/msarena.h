/* msarena.h: the streamer's page arena (Polaris 4.1 split of msarena.c, code moved as it was). One block taken from the
 * heap at boot, cut into 64 KB pages of 256-byte granules: a MemPages instance (mem.h) with the granule as its page,
 * plus the page grid on top of it (free granules per page, the kind of group a page holds), the two pools at the top
 * (structural, misc), and single pages borrowed from the heap. It knows nothing of map items: what lies in a page and
 * whether it may go is the residency planner's (msevict.c). State is hidden in msarena.c; only the streamer's files
 * (mapstream.c, ms*.c) include this (tools/ci/partown.py).
 *
 * Page numbers: [0, npages) the arena (items below ilim, then the misc pool from mpage0, the structural pool from
 * spage0), [npages, npages + MS_XMAX) the slots of borrowed heap pages (an empty slot is marked full). Granule g lies
 * in page g / MS_GPP. */
#ifndef MSARENA_H
#define MSARENA_H

#define MS_PAGE (64u << 10)
#define MS_GRAN 256u
#define MS_GPP (MS_PAGE / MS_GRAN)  /* granules per page */
#define MS_XMAX 96                  /* pages that may be borrowed (6 MB) */
#ifndef MS_POOL
#define MS_POOL (8u << 20)
#endif
#include "cvar.h"
#ifndef MS_LIFE
#define MS_LIFE cv_MS_LIFE  /* runtime switch, default 1 (src/cvar.h; mapstream_i.h has the design) */
#endif
/* EE heap left outside the arena when it is allocated (as v1): the 1.5 MB reserve + 1.5 MB for what loads later */
#define MS_RESERVE (3u << 20)
/* The top of the arena is kept for mapstream_alloc (block lite sets up to 1.7 MB, relief ~0.9 MB, enemy model.bin
 * buffers up to 0.6 MB): map items never go there. Without it the pinned and drawn items filled every page, model
 * buffers fell back to the heap and fragmented it (c2540 model.bin OUT OF RAM in world_cross, host ELF, 2026-11-06);
 * v1 dropped drawn cells for them instead, which hung PCSX2 when a queued chain still read them. Sized for one enemy
 * model.bin (lite sets and relief clear windows below it): 2 MB cost the route's map items 2 MB and its worst frame
 * went from 42 to 219 cells missing (ISO, 2026-11-06). ASSUMED size. */
#define MS_OTHER (768u << 10)
/* MS_LIFE page kinds (mapstream_i.h has the design) */
enum { MS_K_ANY, MS_K_LONG, MS_K_SHORT, MS_K_SMALL };
#define MS_TIDY_LO 4u   /* ASSUMED (the research note's marks): whole free item pages kept */
#define MS_TIDY_HI 8u

/* the arena now (first call: taken from the heap); 0 when the heap had no room */
int msa_ensure(void);
int msa_ready(void);
/* pages: the arena's, the item pages (below the misc pool), all slots (+ borrowed), the item pages now (ilim: all
 * pages without pools while mapstream_alloc holds more than MS_OTHER), the misc / structural pools' first pages */
unsigned msa_npages(void);
unsigned msa_ipages(void);
unsigned msa_ntot(void);
unsigned msa_ilim(void);
unsigned msa_mpage0(void);
unsigned msa_spage0(void);
int msa_pools_on(void);
/* free granules in page p; granule of a pointer (~0u: not the arena's) and back; a granule's bit */
unsigned msa_pfree(unsigned p);
unsigned msa_gran(const void *ptr);
unsigned char *msa_ptr(unsigned g);
int msa_used(unsigned g);
/* mark granules used / free (an old copy of a moved group stays used until no chain reads it) */
void msa_mark(unsigned g0, unsigned n, int on);
/* n bytes (<= a page) inside one page of the item pages and borrowed ones, pages in use first; NULL: none */
void *msa_alloc(unsigned n);
/* the same for a group of this kind (MS_LIFE) */
void *msa_alloc_k(unsigned n, int kind);
/* n bytes in a run of whole free pages in [bot, top), the highest such run, placed at its top */
void *msa_alloc_range(unsigned n, unsigned bot, unsigned top);
/* n bytes (<= a page) inside one page of the pools, pages in use first (MS_LEND) */
void *msa_alloc_lend(unsigned n);
/* n bytes inside one page outside pages [x0, x1) (below ilim): fullest page with a fitting hole first */
void *msa_alloc_ex(unsigned n, unsigned x0, unsigned x1);
/* n bytes inside one page of [p0, p1), pages already in use from the bottom first (collision tiles); NULL: none */
void *msa_alloc_small(unsigned n, unsigned p0, unsigned p1);
void msa_free(void *ptr, unsigned n);
/* item pages wholly free now (borrowed ones too, not those being given back); free granules of the item pages and
 * borrowed ones */
unsigned msa_whole_free(void);
unsigned msa_items_free(void);
/* where the free granules are (the "no room" line): items (below ilim and borrowed), misc pool, structural pool; for
 * each the free KB, the pages wholly free and the largest free run inside one page (KB) */
void msa_free_report(unsigned out[3][3]);
/* scratch words per page slot (msa_ntot of them) for the planner's page scores (msevict.c make_window / defrag) */
unsigned *msa_scratch(int which);
/* borrowed heap pages (MS_BORROW): pages now, slot i in use / being given back, borrow one (its slot, -1: none), give
 * slot i back (its granules must all be free) */
unsigned msa_nx(void);
int msa_x_on(unsigned i);
int msa_x_ret(unsigned i);
void msa_x_set_ret(unsigned i, int on);
int msa_x_borrow(void);
void msa_x_back(unsigned i);
/* the heap's free bytes (sbrk room + free list) */
unsigned msa_heap_free(void);

#endif
