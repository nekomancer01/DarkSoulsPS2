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

/* Disc lock (A12 sound, iop/dsrsnd): from a disc the IOP's sound module reads cdrom0: itself (streams, bank loads), and
 * two IOP threads in the BIOS CD file system at once hung the game (a bank load next to an EE pack open, 2026-11-03) or
 * thrashed the drive (boot load 4.2 -> 40 s). Every cdrom0: fio request takes the lock first and keeps it until its
 * result is collected; a background read only tries (busy: the caller's next frame). No hooks: host: or no sound. */
int (*pack_disc_lock)(int wait);
void (*pack_disc_unlock)(void);
static int disc_depth;

static int disc_get(int wait)
{
    if (!disc_depth && pack_cdrom > 0 && pack_disc_lock && !pack_disc_lock(wait))
        return 0;
    disc_depth++;
    return 1;
}

static void disc_put(void)
{
    if (disc_depth && !--disc_depth && pack_cdrom > 0 && pack_disc_unlock)
        pack_disc_unlock();
}

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

const char *pack_resolve(const char *path, char *out, unsigned cap) { return pack_path(path, out, cap); }

int pack_open(Pack *p, const char *path)
{
    async_finish();
    unsigned head[4];
    char cd_path[96];
    memset(p, 0, sizeof *p);
    fioInit();
    path = pack_path(path, cd_path, sizeof cd_path);
    disc_get(1);
    p->fd = fioOpen(path, O_RDONLY);
    int ok = p->fd >= 0 && fioRead(p->fd, head, 16) == 16 && !memcmp(head, "DSRP", 4) && head[1] == 1;
    if (ok) {
        p->count = head[2];
        p->entries = malloc(p->count * sizeof *p->entries);
        ok = p->entries &&
             fioRead(p->fd, p->entries, p->count * sizeof *p->entries) == (int)(p->count * sizeof *p->entries);
    }
    disc_put();
    if (!ok && p->fd >= 0)
        pack_close(p);
    return ok;
}

void pack_close(Pack *p)
{
    async_finish();
    if (p->fd >= 0) {
        disc_get(1);
        fioClose(p->fd);
        disc_put();
    }
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
        memmove(o, s, l);  /* in-place decode (pack_job_step): the input sits ahead of the output in one buffer */
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
static void log_flush(void);

static void async_finish(void)
{
    if (async_busy == 1) {
        fioSync(FIO_WAIT, &async_result);
        fioSetBlockMode(FIO_WAIT);
        async_busy = 2;  /* done, result not collected yet */
        disc_put();
    }
    log_flush();
}

void pack_async_wait(void) { async_finish(); }  /* save.c: own fio calls need FIO_WAIT mode */

/* stdout / stderr also go through fio (libcglue _write -> fioWrite): a printf while a read is pending would take
 * the async slot's result (a 57-byte log line once came back as a cell read). The link wraps _write (Makefile
 * --wrap=_write): while a read is pending, log text is queued here and written once the read is over. Waiting for
 * the read instead stalled the frame for the rest of it: from cdrom0: a 128 KB world read takes 0.2-1 s in PCSX2, and
 * the logging during block loads cost 10.5 s of stalls in one tests/world_cross.txt run (2026-11-03). A line that
 * does not fit the queue waits for the read as before. */
static char log_q[8192];
static unsigned log_n;
int __real__write(int fd, const void *buf, size_t n);
static void log_flush(void)
{
    if (log_n && async_busy != 1) {
        unsigned n = log_n;
        log_n = 0;
        __real__write(1, log_q, n);
    }
}
int __wrap__write(int fd, const void *buf, size_t n)
{
    if (async_busy == 1 && (fd == 1 || fd == 2) && log_n + n <= sizeof log_q) {
        memcpy(log_q + log_n, buf, n);
        log_n += n;
        return n;
    }
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
    if (async_busy || !disc_get(0))
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
        disc_put();
    }
    if (!async_busy)
        return 0;
    async_busy = 0;
    *result = async_result;
    log_flush();
    return 1;
}

/* Background load of one entry (pack_job_*): open, table of contents and data each go through the async slot, the
 * data is read into the tail of the output buffer and decoded in place (an LZ4 block never grows on decode, and a
 * stored block keeps its size, so the write position stays JOB_MARGIN bytes behind the unread input) */
#define JOB_MARGIN 1024
enum { J_OPEN, J_OPENING, J_HEAD, J_HEADING, J_DATA, J_READING, J_CLOSE };
static PackJob *slot_job;  /* the job whose request holds the async slot */

void pack_job_start(PackJob *j, const char *path, const char *name, void *(*get)(unsigned), void (*put)(void *, unsigned))
{
    memset(j, 0, sizeof *j);
    const char *q = pack_path(path, j->path, sizeof j->path);  /* cdrom0: name, or path itself for host: */
    if (q != j->path)
        snprintf(j->path, sizeof j->path, "%s", q);
    j->name = name, j->get = get, j->put = put, j->fd = -1;
    j->state = J_OPEN;
}

static void job_close(PackJob *j)
{
    disc_get(1);
    fioClose(j->fd), j->fd = -1;
    disc_put();
}

static int job_fail(PackJob *j, const char *why)
{
    printf("pack: %s %s: %s\n", j->path, j->name, why);
    if (j->out)
        j->put(j->out, j->alloc), j->out = NULL;
    if (j->fd >= 0 && !async_busy)
        job_close(j);
    j->state = j->fd >= 0 ? J_CLOSE : -1;  /* a pending read keeps the fd: closed on a later step */
    return j->fd >= 0 ? 0 : -1;
}

/* the result of j's request once it finished (or was parked by pack_async_free), else 0 */
static int job_done(PackJob *j, int *res)
{
    if (j->parked) {
        j->parked = 0;
        *res = j->res;
        return 1;
    }
    if (async_busy == 1 && fioSync(FIO_NOWAIT, &async_result) != FIO_COMPLETE)
        return 0;
    fioSetBlockMode(FIO_WAIT);
    if (async_busy == 1)
        disc_put();
    async_busy = 0, slot_job = NULL;
    *res = async_result;
    log_flush();
    return 1;
}

void pack_async_free(void)
{
    if (!slot_job)
        return;
    async_finish();
    slot_job->res = async_result, slot_job->parked = 1;
    async_busy = 0, slot_job = NULL;
}

int pack_job_step(PackJob *j)
{
    static unsigned head[512] __attribute__((aligned(64)));  /* the first sector: header + up to 31 entries */
    int res;
    switch (j->state) {
    case J_OPEN:
        if (async_busy || !disc_get(0))
            return 0;  /* another read is in flight: next frame */
        fioInit();
        fioSetBlockMode(FIO_NOWAIT);
        fioOpen(j->path, O_RDONLY);
        async_busy = 1, slot_job = j, j->state = J_OPENING;
        return 0;
    case J_OPENING:
        if (!job_done(j, &res))
            return 0;
        if ((j->fd = res) < 0)
            return job_fail(j, "no pack");
        j->state = J_HEAD;
        /* fall through */
    case J_HEAD:
        if (async_busy || !disc_get(0))
            return 0;
        SyncDCache(head, (char *)head + sizeof head - 1);
        fioLseek(j->fd, 0, SEEK_SET);
        fioSetBlockMode(FIO_NOWAIT);
        fioRead(j->fd, head, sizeof head);
        async_busy = 1, slot_job = j, j->state = J_HEADING;
        return 0;
    case J_HEADING: {
        if (!job_done(j, &res))
            return 0;
        InvalidDCache(head, (char *)head + sizeof head - 1);
        if (res < 16 || memcmp(head, "DSRP", 4) || head[1] != 1 || 16 + head[2] * 64 > (unsigned)res)
            return job_fail(j, "bad pack header");
        const struct PackEntry *e = (const struct PackEntry *)(head + 4);
        unsigned i = 0;
        while (i < head[2] && strncmp(e[i].name, j->name, sizeof e->name))
            i++;
        if (i == head[2])
            return job_fail(j, "no such entry");
        e += i;
        j->off = e->offset, j->packed = e->packed, j->raw = e->raw, j->blocks = e->blocks, j->bsize = e->block_size;
        j->state = J_DATA;
    }   /* fall through */
    case J_DATA:
        if (async_busy)
            return 0;
        if (!j->out) {
            j->alloc = (j->raw + JOB_MARGIN + 64 + 127) & ~127u;
            if (j->packed > j->raw + j->blocks * 4 || !(j->out = j->get(j->alloc)))
                return job_fail(j, j->out ? "entry grows on decode" : "OUT OF RAM");
        }
        if (!disc_get(0))
            return 0;
        j->in = j->out + ((j->alloc - j->packed) & ~63u);
        SyncDCache(j->in, j->in + j->packed - 1);
        fioLseek(j->fd, j->off, SEEK_SET);
        fioSetBlockMode(FIO_NOWAIT);
        fioRead(j->fd, j->in, j->packed);
        async_busy = 1, slot_job = j, j->state = J_READING;
        return 0;
    case J_READING: {
        if (!job_done(j, &res))
            return 0;
        job_close(j);
        if (res != (int)j->packed)
            return job_fail(j, "read error");
        InvalidDCache(j->in, j->in + j->packed - 1);
        unsigned nb = j->blocks, cs[nb ? nb : 1], pos = 0;
        memcpy(cs, j->in, nb * 4);  /* the decoded output overwrites the block size table */
        const unsigned char *src = j->in + nb * 4;
        for (unsigned b = 0; b < nb; b++) {
            unsigned c = cs[b] & ~RAW_FLAG, want = j->raw - pos < j->bsize ? j->raw - pos : j->bsize;
            if (cs[b] & RAW_FLAG ? (memmove(j->out + pos, src, c), c != want)
                                 : lz4_decode(src, c, j->out + pos, want) != (int)want)
                return job_fail(j, "corrupt entry");
            src += c, pos += want;
        }
        if (pos != j->raw)
            return job_fail(j, "corrupt entry");
        SyncDCache(j->out, j->out + j->raw - 1);
        pack_stats.read += j->packed, pack_stats.raw += j->raw;
        j->state = -2;
        return 1;
    }
    case J_CLOSE:  /* failed with its read still pending: drop the result, close */
        if (slot_job == j || j->parked) {
            if (!job_done(j, &res))
                return 0;
        }
        if (async_busy)
            return 0;
        if (j->fd >= 0)
            job_close(j);
        j->state = -1;
        return -1;
    }
    return j->state == -2 ? 1 : -1;
}

/* Drop a job before it is done (its pending request finishes first) */
void pack_job_cancel(PackJob *j)
{
    if (j->state == J_OPENING || j->state == J_HEADING || j->state == J_READING) {
        int res;
        if (slot_job == j)
            pack_async_free();
        job_done(j, &res);
        if (j->state == J_OPENING && res >= 0)
            j->fd = res;
    }
    if (j->fd >= 0)
        job_close(j);
    if (j->out && j->state != -2)
        j->put(j->out, j->alloc);
    j->out = NULL;
    j->state = -1;
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
    disc_get(1);
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
    disc_put();
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
