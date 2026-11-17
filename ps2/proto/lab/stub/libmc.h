/* Lab stub (Polaris 1.7c): no memory card in the Lab (mcInit fails: save.c falls back to fio, where mc0: fails too) */
#ifndef LAB_LIBMC_H
#define LAB_LIBMC_H
#define MC_TYPE_MC 0
#define MC_WAIT 0
#define MC_NOWAIT 1
static inline int mcInit(int type) { (void)type; return -1; }
static inline int mcOpen(int port, int slot, const char *name, int mode) { (void)port, (void)slot, (void)name, (void)mode; return -1; }
static inline int mcWrite(int fd, const void *buf, int size) { (void)fd, (void)buf, (void)size; return -1; }
static inline int mcClose(int fd) { (void)fd; return -1; }
static inline int mcMkDir(int port, int slot, const char *name) { (void)port, (void)slot, (void)name; return -1; }
static inline int mcSync(int mode, int *cmd, int *result) { (void)mode, (void)cmd; if (result) *result = -1; return 1; }
#endif
