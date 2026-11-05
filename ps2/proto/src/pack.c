/* Asset pack loader (format: tools/pack.py). Every open / read / close is a request (io_*, below) served by dsrsnd's
 * disc server from cdrom0:, or by fio (its NOWAIT mode) from host:; the IOP fills one staging buffer while the EE
 * decompresses the other. */
#define NEWLIB_PORT_AWARE
#include <kernel.h>
#include <fileio.h>
#include <sifrpc.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "pack.h"
#include "dsrsnd_rpc.h"

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
static void log_flush(void);

static unsigned ticks(void)
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
        fioInit();
        if (pack_cdrom && !pack_disc_boot())
            printf("pack: no disc server (dsrsnd), cdrom0: through fio\n");
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

void (*pack_wait_hook)(void);

/* ---- requests ----
 * Every open / read / close of a pack is a request in one of IO_SLOTS slots; its ticket is the slot + 1. From cdrom0:
 * the requests go to dsrsnd's disc server (dsrsnd_rpc.h SNDF_DISC_*), the only reader of the drive: the EE queues
 * them without waiting (one non-blocking RPC carries up to SND_DISC_BATCH), the IOP serves sync ones first, then the
 * background ones in order, 64 KB at a time between its sound stream refills, and DMAs each slot's SndDiscDone into
 * io_done once the data is in; the EE polls it uncached. Before (until 2026-11-05) the EE and dsrsnd took turns at the
 * drive through a lock that the EE held from a read's issue until a later frame collected it: on the ISO, sound
 * streams were locked out for 298 of 358 s of a Firelink-Burg-Parish route (137 underruns), every blocking try that
 * lost to a refill stalled a frame (median 185 ms), and each small read cost at least two frames (51-99 ms).
 * host: (PCSX2 HostFs, ps2link), or a disc without dsrsnd, keeps plain fio: one NOWAIT request at a time, taken from
 * the slots in the same order. */
#define IO_SLOTS SND_DISC_SLOTS
enum { IO_FREE, IO_QUEUED, IO_SENT, IO_DONE };
typedef struct {
    unsigned seq, order, off, size;
    int state, op, prio, fd, result, drop;  /* drop: nobody collects it (a close), freed when it lands */
    unsigned char *dst;
    const char *path;  /* SNDD_OPEN: the caller's, kept until it lands */
} IoReq;
static IoReq io[IO_SLOTS];
static unsigned io_seq, io_order;
static int io_srv;       /* 1: requests go to dsrsnd's disc server */
static int io_run = -1;  /* fio: the slot whose NOWAIT request is in flight */
static int io_fio_disc;  /* fio served a cdrom0: request: the disc server must not start next to it */
static struct { unsigned slot, seq; } io_cxl[IO_SLOTS];  /* cancels not sent yet */
static int io_ncxl;
static SifRpcClientData_t io_cl __attribute__((aligned(64)));
static int io_cl_busy;
static SndDiscReq io_tx[SND_DISC_BATCH] __attribute__((aligned(64)));
static SndDiscStat io_rx __attribute__((aligned(64)));
static SndDiscDone io_done[IO_SLOTS] __attribute__((aligned(64)));

static inline volatile SndDiscDone *done_rec(int k) { return (volatile SndDiscDone *)((unsigned)&io_done[k] | 0x20000000); }

int __attribute__((weak)) pack_disc_boot(void) { return 0; }

int pack_disc_attach(void)
{
    static SndDiscInit init __attribute__((aligned(64)));
    if (io_srv)
        return 1;
    if (io_fio_disc) {
        printf("pack: disc server too late, cdrom0: stays on fio\n");
        return 0;
    }
    for (int tries = 0; !io_cl.server; tries++) {
        if (SifBindRpc(&io_cl, DSRSND_RPC_DISC_ID, 0) < 0 || tries > 1000)
            return 0;
        if (!io_cl.server)
            nopdelay();
    }
    SyncDCache(io_done, (char *)io_done + sizeof io_done - 1);  /* no cached line over what the IOP DMAs */
    for (int k = 0; k < IO_SLOTS; k++)
        done_rec(k)->seq = 0;
    init.done = (unsigned)io_done;
    SifCallRpc(&io_cl, SNDF_DISC_INIT, 0, &init, sizeof init, &io_rx, sizeof io_rx, NULL, NULL);
    const volatile SndDiscStat *st = (const volatile SndDiscStat *)((unsigned)&io_rx | 0x20000000);
    printf("pack: cdrom0: through dsrsnd's disc server; IOP RAM free %u KB (%u KB before dsrsnd's buffers)\n",
           st->iop_free >> 10, st->iop_free_boot >> 10);
    io_srv = 1;
    return 1;
}

void pack_disc_stat(unsigned out[4])
{
    const volatile SndDiscStat *st = (const volatile SndDiscStat *)((unsigned)&io_rx | 0x20000000);
    out[0] = io_srv ? st->iop_free : 0, out[1] = io_srv ? st->wait_max_ms : 0;
    out[2] = io_srv ? st->waits_100 : 0, out[3] = io_srv ? st->kb : 0;
}

static void io_land(int k, int res)
{
    IoReq *q = &io[k];
    q->result = res;
    q->state = q->drop ? IO_FREE : IO_DONE;
}

/* disc server: the requests that landed, then the next batch out if the last RPC is back */
static void srv_pump(void)
{
    for (int k = 0; k < IO_SLOTS; k++) {
        IoReq *q = &io[k];
        volatile SndDiscDone *d = done_rec(k);
        if (q->state != IO_SENT || d->seq != q->seq)
            continue;
        int res = d->result;
        unsigned nh = d->nhead, nt = d->ntail;
        if (q->op == SNDD_READ && res > 0 && (nh || nt)) {  /* the bytes outside whole qwords, written uncached */
            unsigned char *a = q->dst, *b = q->dst + res - nt;
            if (nh)
                SyncDCache(a, a + nh - 1);
            if (nt)
                SyncDCache(b, b + nt - 1);
            for (unsigned i = 0; i < nh; i++)
                ((volatile unsigned char *)((unsigned)a | 0x20000000))[i] = d->edge[i];
            for (unsigned i = 0; i < nt; i++)
                ((volatile unsigned char *)((unsigned)b | 0x20000000))[i] = d->edge[nh + i];
        }
        io_land(k, res);
    }
    if (io_cl_busy && SifCheckStatRpc(&io_cl))
        return;
    io_cl_busy = 0;
    int n = 0;
    while (io_ncxl && n < SND_DISC_BATCH) {
        io_ncxl--;
        io_tx[n++] = (SndDiscReq){.op = SNDD_CANCEL, .slot = io_cxl[io_ncxl].slot, .seq = io_cxl[io_ncxl].seq};
    }
    while (n < SND_DISC_BATCH) {
        int k = -1;
        for (int i = 0; i < IO_SLOTS; i++)
            if (io[i].state == IO_QUEUED && (k < 0 || (int)(io[i].order - io[k].order) < 0))
                k = i;
        if (k < 0)
            break;
        IoReq *q = &io[k];
        SndDiscReq *r = &io_tx[n++];
        memset(r, 0, sizeof *r);
        r->seq = q->seq, r->slot = k, r->op = q->op, r->prio = q->prio, r->fd = q->fd;
        r->offset = q->off, r->size = q->size, r->dst = (unsigned)q->dst;
        if (q->path)
            snprintf(r->path, sizeof r->path, "%s", q->path);
        q->state = IO_SENT;
    }
    if (!n)
        return;
    SifCallRpc(&io_cl, SNDF_DISC_SUBMIT, SIF_RPC_M_NOWAIT, io_tx, n * sizeof *io_tx, &io_rx, sizeof io_rx, NULL, NULL);
    io_cl_busy = 1;
}

/* fio: the request in flight if it ended, then the next one (sync first, then in order) */
static void fio_pump(void)
{
    for (;;) {
        if (io_run >= 0) {
            int r;
            if (fioSync(FIO_NOWAIT, &r) != FIO_COMPLETE)
                return;
            fioSetBlockMode(FIO_WAIT);
            io_land(io_run, r);
            io_run = -1;
            log_flush();
        }
        int k = -1;
        for (int i = 0; i < IO_SLOTS; i++)
            if (io[i].state == IO_QUEUED &&
                (k < 0 || io[i].prio < io[k].prio || (io[i].prio == io[k].prio && (int)(io[i].order - io[k].order) < 0)))
                k = i;
        if (k < 0)
            return;
        IoReq *q = &io[k];
        io_fio_disc |= pack_cdrom > 0;
        if (q->op == SNDD_CLOSE) {
            io_land(k, fioClose(q->fd));
            continue;
        }
        if (q->op == SNDD_READ)
            fioLseek(q->fd, q->off, SEEK_SET);
        fioSetBlockMode(FIO_NOWAIT);
        if (q->op == SNDD_OPEN)
            fioOpen(q->path, O_RDONLY);
        else
            fioRead(q->fd, q->dst, q->size);
        q->state = IO_SENT;
        io_run = k;
    }
}

/* fio: wait for the request in flight (fio back in WAIT mode) */
static void fio_finish(void)
{
    if (io_run >= 0) {
        int r;
        fioSync(FIO_WAIT, &r);
        fioSetBlockMode(FIO_WAIT);
        io_land(io_run, r);
        io_run = -1;
    }
    log_flush();
}

static void io_pump(void)
{
    if (io_srv)
        srv_pump();
    else
        fio_pump();
}

/* a new request; its ticket, 0 when every slot is taken */
static int io_submit(int op, int prio, int fd, const char *path, unsigned off, void *dst, unsigned size)
{
    int k = -1;
    for (int pass = 0; pass < 2 && k < 0; pass++) {
        for (int i = 0; i < IO_SLOTS && k < 0; i++)
            if (io[i].state == IO_FREE)
                k = i;
        if (k < 0)
            io_pump();  /* a dropped close may land now */
    }
    if (k < 0)
        return 0;
    if (!++io_seq)
        io_seq = 1;  /* 0: a SndDiscDone never written */
    io[k] = (IoReq){.seq = io_seq, .order = io_order++, .off = off, .size = size, .state = IO_QUEUED, .op = op,
                    .prio = prio, .fd = fd, .dst = dst, .path = path};
    if (op == SNDD_READ && size)
        SyncDCache(dst, (char *)dst + size - 1);  /* no dirty line written back over the DMA'd data later */
    io_pump();
    return k + 1;
}

/* 1 once ticket t landed (*res its result; the ticket is free again) */
static int io_poll(int t, int *res)
{
    if (!t)
        return 0;
    io_pump();
    IoReq *q = &io[t - 1];
    if (q->state != IO_DONE)
        return 0;
    *res = q->result;
    q->state = IO_FREE;
    return 1;
}

static unsigned ticks(void);
static int io_wait(int t, int hook)
{
    int r = -1;
#ifdef SLOW_FRAME_LOG
    unsigned t0 = ticks();
    int op = t ? io[t - 1].op : 0, prio = t ? io[t - 1].prio : 0;
#endif
    while (t && !io_poll(t, &r)) {
        if (hook && pack_wait_hook)
            pack_wait_hook();
        else if (!io_srv)
            fio_finish();
        else
            for (volatile int i = 0; i < 2000; i++)  /* not every cycle on the bus */
                ;
    }
#ifdef SLOW_FRAME_LOG
    unsigned ms = (ticks() - t0) / 294912;
    if (t && ms >= 150)
        printf("pack: waited %u ms for a%s %s\n", ms, prio == SNDD_PRIO_SYNC ? " sync" : " background",
               op == SNDD_OPEN ? "open" : op == SNDD_READ ? "read" : "close");
#endif
    return r;
}

/* a request that must go out: waits for a free slot */
static int io_submit_wait(int op, int prio, int fd, const char *path, unsigned off, void *dst, unsigned size)
{
    int t;
    while (!(t = io_submit(op, prio, fd, path, off, dst, size)))
        io_wait(0, 0), io_srv ? (void)0 : fio_finish();
    return t;
}

static int io_sync(int op, int fd, const char *path, unsigned off, void *dst, unsigned size)
{
    return io_wait(io_submit_wait(op, SNDD_PRIO_SYNC, fd, path, off, dst, size), 1);
}

/* drop ticket t: a request not sent yet is gone, a read in flight stops after its current page (disc server); waits
 * until nothing more lands in its buffer. Returns its result (-1 when it never ran) */
static int io_cancel(int t)
{
    if (!t)
        return -1;
    IoReq *q = &io[t - 1];
    if (q->state == IO_QUEUED) {
        q->state = IO_FREE;
        return -1;
    }
    if (io_srv && q->state == IO_SENT && q->op == SNDD_READ && io_ncxl < IO_SLOTS)
        io_cxl[io_ncxl].slot = t - 1, io_cxl[io_ncxl++].seq = q->seq;
    return io_wait(t, 0);
}

static void io_close(int fd)
{
    int t = io_submit_wait(SNDD_CLOSE, SNDD_PRIO_BG, fd, NULL, 0, NULL, 0);
    if (io[t - 1].state == IO_DONE)
        io[t - 1].state = IO_FREE;
    else
        io[t - 1].drop = 1;
}

int pack_req_read(Pack *p, unsigned offset, void *dst, unsigned bytes)
{
    return io_submit(SNDD_READ, SNDD_PRIO_BG, p->fd, NULL, offset, dst, bytes);
}

int pack_req_done(int t, int *result) { return io_poll(t, result); }

void pack_req_cancel(int t) { io_cancel(t); }

void pack_pump(void) { io_pump(); }

int pack_open(Pack *p, const char *path)
{
    static unsigned head[16] __attribute__((aligned(64)));
    char cd_path[96];
    memset(p, 0, sizeof *p);
    path = pack_path(path, cd_path, sizeof cd_path);
    p->fd = io_sync(SNDD_OPEN, -1, path, 0, NULL, 0);
    int ok = p->fd >= 0 && io_sync(SNDD_READ, p->fd, NULL, 0, head, 16) == 16;
    InvalidDCache(head, (char *)head + sizeof head - 1);
    ok = ok && !memcmp(head, "DSRP", 4) && head[1] == 1;
    if (ok) {
        unsigned n = head[2] * sizeof *p->entries;
        p->count = head[2];
        p->entries = memalign(64, (n + 63) & ~63u);  /* DMA'd into: no cache line shared with the heap's */
        ok = p->entries && io_sync(SNDD_READ, p->fd, NULL, 16, p->entries, n) == (int)n;
        if (ok)
            InvalidDCache(p->entries, (char *)p->entries + n - 1);
    }
    if (!ok && p->fd >= 0)
        pack_close(p);
    return ok;
}

void pack_close(Pack *p)
{
    if (p->fd >= 0)
        io_close(p->fd);  /* in order behind the pack's reads */
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

/* stdout / stderr also go through fio (libcglue _write -> fioWrite): a printf while a fio NOWAIT request is in flight
 * would take its result (a 57-byte log line once came back as a cell read). The link wraps _write (Makefile
 * --wrap=_write): meanwhile log text is queued here and written once the request is over. Waiting for the request
 * instead stalled the frame for the rest of it: from cdrom0: a 128 KB world read took 0.2-1 s in PCSX2, and the logging
 * during block loads cost 10.5 s of stalls in one tests/world_cross.txt run (2026-11-03). A line that does not fit the
 * queue waits for the request as before. The disc server sends no fio requests, so this only matters for fio. */
static char log_q[8192];
static unsigned log_n;
int __real__write(int fd, const void *buf, size_t n);
static void log_flush(void)
{
    if (log_n && io_run < 0) {
        unsigned n = log_n;
        log_n = 0;
        __real__write(1, log_q, n);
    }
}
int __wrap__write(int fd, const void *buf, size_t n)
{
    if (io_run >= 0 && (fd == 1 || fd == 2) && log_n + n <= sizeof log_q) {
        memcpy(log_q + log_n, buf, n);
        log_n += n;
        return n;
    }
    fio_finish();
    return __real__write(fd, buf, n);
}

unsigned pack_entry_size(const Pack *p, const char *name)
{
    for (unsigned i = 0; i < p->count; i++)
        if (!strncmp(p->entries[i].name, name, sizeof p->entries[i].name))
            return p->entries[i].raw;
    return 0;
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

/* the one-at-a-time background read of pack_read_async (items.c's icons and descriptions) */
static int legacy_t;

int pack_read_async(Pack *p, unsigned offset, void *dst, unsigned bytes)
{
    if (legacy_t)
        return 0;
    legacy_t = pack_req_read(p, offset, dst, bytes);
    return legacy_t != 0;
}

int pack_async_done(int *result)
{
    if (!io_poll(legacy_t, result))
        return 0;
    legacy_t = 0;
    return 1;
}

void pack_async_wait(void)
{
    while (legacy_t && io[legacy_t - 1].state != IO_DONE)
        io_srv ? io_pump() : fio_finish();
    if (!io_srv)
        fio_finish();  /* save.c: its own fio calls need FIO_WAIT mode */
}

void pack_async_free(void) {}  /* every read has a slot of its own now: nothing to free */

/* Background load of one entry (pack_job_*): open, table of contents and data are each a background request, the
 * data is read into the tail of the output buffer and decoded in place (an LZ4 block never grows on decode, and a
 * stored block keeps its size, so the write position stays JOB_MARGIN bytes behind the unread input) */
#define JOB_MARGIN 1024
enum { J_OPEN, J_OPENING, J_HEAD, J_HEADING, J_DATA, J_READING, J_DECODE, J_CLOSE };
/* EE ms of LZ4 decode per pack_job_step: a model.bin took 10-20 ms in one frame (enemy types streaming in, smoke
 * frame 83.6 ms, 2026-11-04); a 64 KB block is ~1-2 ms */
#define PACK_JOB_DECODE_MS 3

void pack_job_start(PackJob *j, const char *path, const char *name, void *(*get)(unsigned), void (*put)(void *, unsigned))
{
    memset(j, 0, sizeof *j);
    const char *q = pack_path(path, j->path, sizeof j->path);  /* cdrom0: name, or path itself for host: */
    if (q != j->path)
        snprintf(j->path, sizeof j->path, "%s", q);
    j->name = name, j->get = get, j->put = put, j->fd = -1, j->fdc = -1;
    j->state = J_OPEN;
}

/* Handles of packs jobs read, with the pack's first sector (header + entry table): the next job on the same pack skips
 * the open and the header read. From cdrom0: an open is an ISO directory search and the header a seek + read, ~0.1-0.3
 * s each on the async slot: a type's sound bank (bank, cue, emit: three jobs) opened its pack three times, and a type
 * that streamed back in opened both of its packs again (134 opens in one route_enemies.txt run, 2026-11-05, TECH_DEBT
 * world-8). ioman's file table holds 16 handles, shared with the world block packs, menu.pak, mapstream and dsrsnd's
 * streams and bank load: with 4 cached it ran full once enemy types streamed in (ISO 2026-11-05, smoke: 11 EE handles
 * + 3 streams, -24 for dsrsnd's bank opens). An open that finds it full closes the idle cached handles and is tried
 * again on later steps for up to JOB_OPEN_TRIES frames. */
#define JOB_FDS 2
#define JOB_OPEN_TRIES 300
#define EMFILE_ 24
static struct {
    char path[96];
    int fd, busy;  /* busy: a job reads it now */
    unsigned use;
    unsigned head[512] __attribute__((aligned(64)));
} jfd[JOB_FDS] = {[0 ... JOB_FDS - 1] = {.fd = -1}};
static unsigned jfd_clock;

static void fd_close(int fd) { io_close(fd); }

/* close the cached handles no job reads (an open failed: maybe the driver's limit) ; 1 if any */
static int jfd_flush(void)
{
    int n = 0;
    for (int k = 0; k < JOB_FDS; k++)
        if (jfd[k].fd >= 0 && !jfd[k].busy)
            fd_close(jfd[k].fd), jfd[k].fd = -1, n++;
    return n;
}

static void job_close(PackJob *j)
{
    if (j->fdc >= 0) {  /* back to the cache, still open */
        jfd[j->fdc].busy = 0, jfd[j->fdc].use = ++jfd_clock;
        j->fdc = -1, j->fd = -1;
        return;
    }
    fd_close(j->fd), j->fd = -1;
}

static int job_fail(PackJob *j, const char *why)
{
    printf("pack: %s %s: %s\n", j->path, j->name, why);
    if (j->fdc >= 0 && (!strcmp(why, "read error") || !strcmp(why, "corrupt entry")))  /* a bad handle: closed with j */
        jfd[j->fdc].fd = -1, jfd[j->fdc].busy = 0, j->fdc = -1;
    if (j->out)
        j->put(j->out, j->alloc), j->out = NULL;
    if (j->fd >= 0 && !j->tk)
        job_close(j);
    j->state = j->tk ? J_CLOSE : -1;  /* a pending request keeps the fd: closed on a later step */
    return j->tk ? 0 : -1;
}

/* the result of j's request once it landed (or the cached header, tk 0), else 0 */
static int job_done(PackJob *j, int *res)
{
    if (!j->tk) {
        *res = j->res;
        return 1;
    }
    if (!io_poll(j->tk, res))
        return 0;
    j->tk = 0;
    return 1;
}

int pack_job_step(PackJob *j)
{
    unsigned *head = j->head;  /* the first sector: header + up to 31 entries */
    int res;
    switch (j->state) {
    case J_OPEN:
        for (int k = 0; k < JOB_FDS; k++)
            if (jfd[k].fd >= 0 && !jfd[k].busy && !strcmp(jfd[k].path, j->path)) {
                /* cached: its first sector as the header read would have returned it */
                jfd[k].busy = 1, j->fdc = k, j->fd = jfd[k].fd;
                memcpy(head, jfd[k].head, sizeof j->head);
                SyncDCache(head, (char *)head + sizeof j->head - 1);  /* J_HEADING invalidates the cache lines */
                j->res = sizeof j->head, j->tk = 0, j->state = J_HEADING;
                return pack_job_step(j);
            }
        if (!(j->tk = io_submit(SNDD_OPEN, SNDD_PRIO_BG, -1, j->path, 0, NULL, 0)))
            return 0;  /* every request slot taken: next frame */
        j->state = J_OPENING;
        return 0;
    case J_OPENING:
        if (!job_done(j, &res))
            return 0;
        if ((j->fd = res) < 0) {
            if (res == -EMFILE_ && j->reopen++ < JOB_OPEN_TRIES) {  /* the file table: the idle cached handles go first */
                jfd_flush();
                j->fd = -1, j->state = J_OPEN;
                return 0;
            }
            return job_fail(j, "no pack");
        }
        j->state = J_HEAD;
        /* fall through */
    case J_HEAD:
        if (!(j->tk = io_submit(SNDD_READ, SNDD_PRIO_BG, j->fd, NULL, 0, head, sizeof j->head)))
            return 0;
        j->state = J_HEADING;
        return 0;
    case J_HEADING: {
        if (!job_done(j, &res))
            return 0;
        InvalidDCache(head, (char *)head + sizeof j->head - 1);
        if (res < 16 || memcmp(head, "DSRP", 4) || head[1] != 1 || 16 + head[2] * 64 > (unsigned)res)
            return job_fail(j, "bad pack header");
        if (j->fdc < 0 && res == (int)sizeof j->head) {  /* into the cache: a free slot, else the least recently used */
            int k = -1;
            for (int c = 0; c < JOB_FDS; c++)
                if (!jfd[c].busy && (k < 0 || jfd[c].fd < 0 || (jfd[k].fd >= 0 && jfd[c].use < jfd[k].use)))
                    k = c;
            if (k >= 0) {
                if (jfd[k].fd >= 0)
                    fd_close(jfd[k].fd);
                snprintf(jfd[k].path, sizeof jfd[k].path, "%s", j->path);
                memcpy(jfd[k].head, head, sizeof j->head);
                jfd[k].fd = j->fd, jfd[k].busy = 1, j->fdc = k;
            }
        }
        const struct PackEntry *e = (const struct PackEntry *)(head + 4);
        unsigned i = 0;
        while (i < head[2] && strncmp(e[i].name, j->name, sizeof e->name))
            i++;
        if (i == head[2])
            return job_fail(j, "no such entry");
        for (int x = 0; x < 2; x++)
            for (unsigned k = 0; j->xname[x] && k < head[2]; k++)
                if (!strncmp(e[k].name, j->xname[x], sizeof e->name) && e[k].packed == e[k].blocks * 4 + e[k].raw)
                    j->xoff[x] = e[k].offset + e[k].blocks * 4, j->xsize[x] = e[k].raw;
        e += i;
        j->off = e->offset, j->packed = e->packed, j->raw = e->raw, j->blocks = e->blocks, j->bsize = e->block_size;
        j->state = J_DATA;
    }   /* fall through */
    case J_DATA:
        if (!j->out) {
            j->alloc = (j->raw + JOB_MARGIN + 64 + 127) & ~127u;
            if (j->packed > j->raw + j->blocks * 4 || !(j->out = j->get(j->alloc)))
                return job_fail(j, j->out ? "entry grows on decode" : "OUT OF RAM");
        }
        j->in = j->out + ((j->alloc - j->packed) & ~63u);
        if (!(j->tk = io_submit(SNDD_READ, SNDD_PRIO_BG, j->fd, NULL, j->off, j->in, j->packed)))
            return 0;
        j->state = J_READING;
        return 0;
    case J_READING: {
        if (!job_done(j, &res))
            return 0;
        job_close(j);
        if (res != (int)j->packed)
            return job_fail(j, "read error");
        InvalidDCache(j->in, j->in + j->packed - 1);
        if (j->blocks > sizeof j->cs / sizeof j->cs[0])
            return job_fail(j, "too many blocks");
        memcpy(j->cs, j->in, j->blocks * 4);  /* the decoded output overwrites the block size table */
        j->src = j->in + j->blocks * 4, j->b = 0, j->pos = 0;
        j->state = J_DECODE;
    }   /* fall through */
    case J_DECODE: {
        unsigned t0 = ticks();
        while (j->b < j->blocks && (ticks() - t0) < PACK_JOB_DECODE_MS * 294912u) {
            unsigned c = j->cs[j->b] & ~RAW_FLAG, want = j->raw - j->pos < j->bsize ? j->raw - j->pos : j->bsize;
            if (j->cs[j->b] & RAW_FLAG ? (memmove(j->out + j->pos, j->src, c), c != want)
                                       : lz4_decode(j->src, c, j->out + j->pos, want) != (int)want)
                return job_fail(j, "corrupt entry");
            j->src += c, j->pos += want, j->b++;
        }
        if (j->b < j->blocks)
            return 0;
        if (j->pos != j->raw)
            return job_fail(j, "corrupt entry");
        SyncDCache(j->out, j->out + j->raw - 1);
        pack_stats.read += j->packed, pack_stats.raw += j->raw;
        j->state = -2;
        return 1;
    }
    case J_CLOSE:  /* failed with its request still pending: drop the result, close */
        if (!job_done(j, &res))
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
    if (j->tk) {  /* a read stops after the page in flight; an open is let finish and closed */
        int res = io_cancel(j->tk);
        j->tk = 0;
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
    struct PackEntry *e = NULL;
    for (unsigned i = 0; i < p->count; i++)
        if (!strncmp(p->entries[i].name, name, sizeof e->name))
            e = &p->entries[i];
    if (!e)
        return NULL;
    unsigned t0 = ticks(), wait = 0, nb = e->blocks;
    unsigned char *out = get((e->raw + 127) & ~127u);
    unsigned *cs = memalign(64, (nb * 4 + 63) & ~63u);  /* DMA'd into: no cache line shared with the heap's */
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
        unsigned w = ticks();
        ok = io_sync(SNDD_READ, p->fd, NULL, e->offset, cs, nb * 4) == (int)(nb * 4);
        wait += ticks() - w;
        InvalidDCache(cs, (char *)cs + nb * 4 - 1);
    }
    unsigned b = 0, bytes, end = ok ? batch_end(cs, 0, nb, &bytes, st) : 0, pos = 0, foff = e->offset + nb * 4;
    int cur = 0, tk = ok ? io_submit_wait(SNDD_READ, SNDD_PRIO_SYNC, p->fd, NULL, foff, stage[0], bytes) : 0;
    foff += bytes;
    while (ok && b < nb) {
        unsigned w = ticks();
        int got = io_wait(tk, 1);
        tk = 0;
        wait += ticks() - w;
        if (got != (int)bytes) {
            ok = 0;
            break;
        }
        InvalidDCache(stage[cur], stage[cur] + st - 1);  /* drop stale lines over the DMA'd data */
        /* next batch: queue its read, decompress this one meanwhile (the disc server keeps serving the sound streams:
         * nothing holds the drive while the EE decodes) */
        unsigned nbytes = 0, nend = end < nb ? batch_end(cs, end, nb, &nbytes, st) : end;
        if (nend > end) {
            tk = io_submit_wait(SNDD_READ, SNDD_PRIO_SYNC, p->fd, NULL, foff, stage[cur ^ 1], nbytes);
            foff += nbytes;
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
        cur ^= 1;
        end = nend;
        bytes = nbytes;
    }
    if (tk)
        io_wait(tk, 0);  /* a read still on its way into a staging buffer that goes back now */
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
