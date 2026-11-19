/* Lab stub (Polaris 1.7b): the EE kernel calls the simulate modules make, as no-ops on the host */
#ifndef LAB_KERNEL_H
#define LAB_KERNEL_H
#include <tamtypes.h>
static inline void SyncDCache(void *a, void *b) { (void)a, (void)b; }
static inline void iSyncDCache(void *a, void *b) { (void)a, (void)b; }
static inline void InvalidDCache(void *a, void *b) { (void)a, (void)b; }
static inline void FlushCache(int m) { (void)m; }
static inline int GetThreadId(void) { return 1; }
static inline int DelayThread(int us) { (void)us; return 0; }
#endif
