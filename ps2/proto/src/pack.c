/* Asset pack loader (format: tools/pack.py). fio is used directly (not newlib's read) for its NOWAIT mode: the IOP
 * fills one staging buffer while the EE decompresses the other. */
#define NEWLIB_PORT_AWARE
#include <kernel.h>
#include <fileio.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "pack.h"

struct PackEntry {
    char name[32];
    unsigned offset, raw, packed, blocks, block_size, pad[3];
};

/* compressed bytes per read batch (>= one stored 64 KB block); the two buffers shrink to the entry's packed size (an
 * entry that fits one batch reads exactly as before): a streamed enemy model.bin (~0.3 MB raw, ~0.15 MB packed) needed
 * 2 x 256 KB more heap next to its own buffer, which the fragmented heap of a world build with two blocks' enemies
 * could not give (2026-11-01) */
#define STAGE (256 * 1024)
#define STAGE_MIN (64 * 1024 + 64)
#define RAW_FLAG 0x80000000u

PackStats pack_stats;
static void async_finish(void);

static inline unsigned ticks(void)
{
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

/* Boot device (tools-hwperf): booted from a disc, libcglue's cwd (argv[0]'s device) is "cdrom0:..." and PACK_ROOT
 * paths are mapped onto the ISO layout of `make iso` (tools/ps2-iso.sh): "host:data/m10_01_00_00/c1000.pak" ->
 * "cdrom0:\DATA\10010000\C1000.PAK;1" (ISO9660 level 1: upper case, '_' dropped, names cut to their last 8 chars).
 * host: (PCSX2 HostFs, ps2link) paths pass through. make DEFS=-DPACK_CDROM=0/1 forces one. */
int pack_cdrom = -1;

static const char *pack_path(const char *path, char *out, unsigned cap)
{
    if (pack_cdrom < 0) {
#ifdef PACK_CDROM
        pack_cdrom = PACK_CDROM;
#else
        char cwd[64];
        pack_cdrom = getcwd(cwd, sizeof cwd) && !strncmp(cwd, "cdrom", 5);
#endif
        printf("pack: packs from %s\n", pack_cdrom ? "cdrom0:\\DATA\\" : PACK_ROOT);
    }
    if (!pack_cdrom || strncmp(path, "host:data/", 10))
        return path;
    unsigned n = snprintf(out, cap, "cdrom0:\\DATA");
    for (const char *s = path + 10; *s && n + 16 < cap;) {
        const char *e = s;
        while (*e && *e != '/' && *e != '.')
            e++;
        char name[32];
        unsigned k = 0;
        for (const char *c = s; c < e && k < sizeof name - 1; c++)
            if (*c != '_')
                name[k++] = *c >= 'a' && *c <= 'z' ? *c - 32 : *c;
        name[k] = 0;
        n += snprintf(out + n, cap - n, "\\%s", name + (k > 8 ? k - 8 : 0));
        if (*e == '.') {  /* extension: up to 3 chars, then the end */
            out[n++] = '.';
            for (e++; *e && *e != '/' && n < cap - 4; e++)
                out[n++] = *e >= 'a' && *e <= 'z' ? *e - 32 : *e;
            n += snprintf(out + n, cap - n, ";1");
            break;
        }
        s = *e ? e + 1 : e;
    }
    return out;
}

int pack_open(Pack *p, const char *path)
{
    async_finish();
    unsigned head[4];
    char cd_path[96];
    memset(p, 0, sizeof *p);
    fioInit();
    path = pack_path(path, cd_path, sizeof cd_path);
    p->fd = fioOpen(path, O_RDONLY);
    if (p->fd < 0)
        return 0;
    if (fioRead(p->fd, head, 16) != 16 || memcmp(head, "DSRP", 4) || head[1] != 1) {
        pack_close(p);
        return 0;
    }
    p->count = head[2];
    p->entries = malloc(p->count * sizeof *p->entries);
    if (!p->entries || fioRead(p->fd, p->entries, p->count * sizeof *p->entries) != (int)(p->count * sizeof *p->entries)) {
        pack_close(p);
        return 0;
    }
    return 1;
}

void pack_close(Pack *p)
{
    async_finish();
    if (p->fd >= 0)
        fioClose(p->fd);
    free(p->entries);
    p->fd = -1;
    p->entries = NULL;
}

/* LZ4 block decoder; returns bytes written or -1 */
static int lz4_decode(const unsigned char *s, unsigned n, unsigned char *dst, unsigned cap)
{
    const unsigned char *se = s + n;
    unsigned char *o = dst, *oe = dst + cap;
    while (s < se) {
        unsigned tok = *s++, l = tok >> 4;
        if (l == 15) {
            unsigned b;
            do
                l += b = *s++;
            while (b == 255);
        }
        if (o + l > oe || s + l > se)
            return -1;
        memcpy(o, s, l);
        o += l, s += l;
        if (s >= se)
            break;
        unsigned off = s[0] | s[1] << 8, m = tok & 15;
        s += 2;
        if (m == 15) {
            unsigned b;
            do
                m += b = *s++;
            while (b == 255);
        }
        m += 4;
        if (!off || (unsigned)(o - dst) < off || o + m > oe)
            return -1;
        const unsigned char *r = o - off;
        if (off >= 8)
            while (m >= 8) {  /* non-overlapping 8-byte steps */
                memcpy(o, r, 8);
                o += 8, r += 8, m -= 8;
            }
        while (m--)
            *o++ = *r++;
    }
    return o - dst;
}

/* Blocks [b, end) whose compressed sizes fit one staging buffer */
static unsigned batch_end(const unsigned *cs, unsigned b, unsigned nb, unsigned *bytes, unsigned stage)
{
    unsigned e = b, sum = 0;
    while (e < nb && sum + (cs[e] & ~RAW_FLAG) <= stage)
        sum += cs[e++] & ~RAW_FLAG;
    *bytes = sum;
    return e;
}

char pack_error[96];

/* One background read at a time (pack_read_async); fio has a single async slot, so every other fio call here waits
 * for it first and keeps its result for pack_async_done */
static int async_busy, async_result;

static void async_finish(void)
{
    if (async_busy) {
        fioSync(FIO_WAIT, &async_result);
        fioSetBlockMode(FIO_WAIT);
        async_busy = 2;  /* done, result not collected yet */
    }
}

void pack_async_wait(void) { async_finish(); }  /* save.c: own fio calls need FIO_WAIT mode */

/* stdout / stderr also go through fio (libcglue _write -> fioWrite): a printf while a read is pending would take
 * the async slot's result (a 57-byte log line once came back as a cell read). The link wraps _write (Makefile
 * --wrap=_write) so every write waits for the pending read first. */
int __real__write(int fd, const void *buf, size_t n);
int __wrap__write(int fd, const void *buf, size_t n)
{
    async_finish();
    return __real__write(fd, buf, n);
}

int pack_entry_raw(Pack *p, const char *name, unsigned *offset, unsigned *size)
{
    for (unsigned i = 0; i < p->count; i++) {
        struct PackEntry *e = &p->entries[i];
        if (strncmp(e->name, name, sizeof e->name))
            continue;
        unsigned nb = e->blocks;
        if (e->packed != nb * 4 + e->raw)  /* some block compressed: not readable in place */
            return 0;
        *offset = e->offset + nb * 4, *size = e->raw;
        return 1;
    }
    return 0;
}

int pack_read_async(Pack *p, unsigned offset, void *dst, unsigned bytes)
{
    if (async_busy)
        return 0;
    SyncDCache(dst, (char *)dst + bytes - 1);  /* no dirty lines written back over the DMA'd data later */
    fioLseek(p->fd, offset, SEEK_SET);
    fioSetBlockMode(FIO_NOWAIT);
    fioRead(p->fd, dst, bytes);
    async_busy = 1;
    return 1;
}

int pack_async_done(int *result)
{
    if (async_busy == 1) {
        if (fioSync(FIO_NOWAIT, &async_result) != FIO_COMPLETE)
            return 0;
        fioSetBlockMode(FIO_WAIT);
    }
    if (!async_busy)
        return 0;
    async_busy = 0;
    *result = async_result;
    return 1;
}

static void *heap_get(unsigned n) { return memalign(128, n); }
static void heap_put(void *p, unsigned n) { (void)n; free(p); }

void *pack_load(Pack *p, const char *name, unsigned *size)
{
    return pack_load_with(p, name, size, heap_get, heap_put);
}

void *pack_load_with(Pack *p, const char *name, unsigned *size, void *(*get)(unsigned), void (*put)(void *, unsigned))
{
    async_finish();
    struct PackEntry *e = NULL;
    for (unsigned i = 0; i < p->count; i++)
        if (!strncmp(p->entries[i].name, name, sizeof e->name))
            e = &p->entries[i];
    if (!e)
        return NULL;
    unsigned t0 = ticks(), wait = 0, nb = e->blocks;
    unsigned char *out = get((e->raw + 127) & ~127u);
    unsigned *cs = memalign(64, (nb * 4 + 63) & ~63u);  /* fioRead DMAs into it: no cache line shared with the heap's */
    unsigned st = e->packed < STAGE ? (e->packed + 63) & ~63u : STAGE;
    st = st < STAGE_MIN ? STAGE_MIN : st;
    int one = e->packed <= st;  /* one batch: the second buffer is never read into */
    unsigned char *stage[2] = {get(st), one ? NULL : get(st)};
    int ok = out && cs && stage[0] && (stage[1] || one), mem_ok = ok;
    if (!ok) {  /* loud: a NULL here used to end up as garbage in a DMA chain */
        struct mallinfo mi = mallinfo();
        snprintf(pack_error, sizeof pack_error, "OUT OF RAM: %s needs %uK (%s), free %uK", name, (e->raw + 1023) >> 10,
                 !out ? "output" : !cs ? "block sizes" : "staging", /* which allocation failed */
                 (unsigned)(32 * 1024 * 1024 - (unsigned)sbrk(0) + mi.fordblks) >> 10);
        printf("pack: %s\n", pack_error);
    }
    if (ok) {
        fioLseek(p->fd, e->offset, SEEK_SET);
        ok = fioRead(p->fd, cs, nb * 4) == (int)(nb * 4);
        InvalidDCache(cs, (char *)cs + nb * 4 - 1);
    }
    unsigned b = 0, bytes, end = ok ? batch_end(cs, 0, nb, &bytes, st) : 0, pos = 0;
    int cur = 0, got = ok ? fioRead(p->fd, stage[0], bytes) : -1;
    while (ok && b < nb) {
        if (got != (int)bytes) {
            ok = 0;
            break;
        }
        InvalidDCache(stage[cur], stage[cur] + st - 1);  /* drop stale lines over the DMA'd data */
        /* next batch: queue the read on the IOP, decompress this one meanwhile */
        unsigned nbytes = 0, nend = end < nb ? batch_end(cs, end, nb, &nbytes, st) : end;
        if (nend > end) {
            SyncDCache(stage[cur ^ 1], stage[cur ^ 1] + nbytes - 1);  /* no dirty line written back over the DMA */
            fioSetBlockMode(FIO_NOWAIT);
            fioRead(p->fd, stage[cur ^ 1], nbytes);
        }
        const unsigned char *src = stage[cur];
        for (; b < end && ok; b++) {
            unsigned c = cs[b] & ~RAW_FLAG, want = e->raw - pos < e->block_size ? e->raw - pos : e->block_size;
            if (cs[b] & RAW_FLAG) {
                memcpy(out + pos, src, c);
                ok = c == want;
            } else {
                ok = lz4_decode(src, c, out + pos, want) == (int)want;
            }
            src += c;
            pos += want;
        }
        if (nend > end) {
            unsigned w = ticks();
            fioSync(FIO_WAIT, &got);
            fioSetBlockMode(FIO_WAIT);
            wait += ticks() - w;
        }
        cur ^= 1;
        end = nend;
        bytes = nbytes;
    }
    if (stage[0])
        put(stage[0], st);
    if (stage[1])
        put(stage[1], st);
    free(cs);
    if (!ok || pos != e->raw) {
        if (mem_ok) {
            snprintf(pack_error, sizeof pack_error, "READ ERROR: %s", name);
            printf("pack: %s\n", pack_error);
        }
        if (out)
            put(out, (e->raw + 127) & ~127u);
        return NULL;
    }
    SyncDCache(out, out + e->raw - 1);  /* VIF1/GIF DMA read the data straight from memory */
    pack_stats.read += e->packed;
    pack_stats.raw += e->raw;
    pack_stats.ticks += ticks() - t0;
    pack_stats.wait_ticks += wait;
    pack_stats.heap += (e->raw + 127) & ~127u;
    if (size)
        *size = e->raw;
    return out;
}
