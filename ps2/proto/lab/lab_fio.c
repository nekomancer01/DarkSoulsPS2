/* lab_fio.c: the EE's fio calls over host files (Polaris 1.7c), for the modules that read and write through them in
 * the Lab: testscript.c (the script), save.c (host: saves) and the trace recorder. "host:" is the build directory,
 * as PCSX2's HostFs root is (absolute host paths stay absolute: lab.py hands scripts over by their full path), except
 * host:save/, which is the build's host/save/ so the Lab's saves never touch the console tests' build/save/. Any other
 * device (mc0:, cdrom0:) fails as with no card or disc. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fileio.h>
#include "lab.h"

static FILE *fds[32];

static const char *host_path(const char *name, char *out, unsigned cap)
{
    if (strncmp(name, "host:", 5))
        return NULL;
    if (name[5] == '/')
        return snprintf(out, cap, "%s", name + 5), out;
    if (!strncmp(name + 5, "save/", 5)) {
        snprintf(out, cap, "%s/host/save", lab_root);
        mkdir(out, 0755);
        return snprintf(out, cap, "%s/host/%s", lab_root, name + 5), out;
    }
    return lab_path(name, out, cap);
}

int fioOpen(const char *name, int mode)
{
    char p[512];
    int fd;
    for (fd = 3; fd < 32 && fds[fd]; fd++)
        ;
    if (fd == 32 || !host_path(name, p, sizeof p))
        return -1;
    const char *m = (mode & 3) == FIO_O_RDONLY ? "rb" : mode & FIO_O_TRUNC ? "wb" : "r+b";
    FILE *f = fopen(p, m);
    if (!f && (mode & FIO_O_CREAT))
        f = fopen(p, "wb");
    return f ? (fds[fd] = f, fd) : -1;
}

int fioClose(int fd)
{
    if (fd < 3 || fd >= 32 || !fds[fd])
        return -1;
    fclose(fds[fd]), fds[fd] = NULL;
    return 0;
}

int fioRead(int fd, void *buf, int size) { return fd >= 3 && fd < 32 && fds[fd] ? (int)fread(buf, 1, size, fds[fd]) : -1; }
int fioWrite(int fd, const void *buf, int size)
{
    return fd >= 3 && fd < 32 && fds[fd] ? (int)fwrite(buf, 1, size, fds[fd]) : -1;
}
int fioLseek(int fd, int offset, int whence)
{
    if (fd < 3 || fd >= 32 || !fds[fd] || fseek(fds[fd], offset, whence))
        return -1;
    return (int)ftell(fds[fd]);
}
int fioRemove(const char *name)
{
    char p[512];
    return host_path(name, p, sizeof p) && !remove(p) ? 0 : -1;
}
int fioMkdir(const char *path)
{
    char p[512];
    return host_path(path, p, sizeof p) && !mkdir(p, 0755) ? 0 : -1;
}
int fioDopen(const char *name) { (void)name; return -1; }
int fioDclose(int fd) { (void)fd; return -1; }
