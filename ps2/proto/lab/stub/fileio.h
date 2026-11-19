/* Lab stub (Polaris 1.7c): the EE's fio calls over host files (lab/lab_fio.c). "host:" paths are the build directory
 * (absolute host paths stay absolute); other devices (mc0:, cdrom0:) fail, as with no card in the slot. */
#ifndef LAB_FILEIO_H
#define LAB_FILEIO_H
#include <io_common.h>
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#define O_RDONLY FIO_O_RDONLY
int fioOpen(const char *name, int mode);
int fioClose(int fd);
int fioRead(int fd, void *buf, int size);
int fioWrite(int fd, const void *buf, int size);
int fioLseek(int fd, int offset, int whence);
int fioRemove(const char *name);
int fioMkdir(const char *path);
int fioDopen(const char *name);
int fioDclose(int fd);
#endif
