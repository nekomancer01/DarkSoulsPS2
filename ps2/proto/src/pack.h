/* Asset packs (tools/pack.py): LZ4 in 64 KB blocks, loaded at runtime from host: (PCSX2 HostFs, root = the ELF's
 * folder) or cdrom0:. Reads go to the IOP as requests (dsrsnd's disc server from cdrom0:, fio NOWAIT from host:) while
 * the EE decompresses the previous batch of blocks. */
#ifndef PACK_H
#define PACK_H

typedef struct {
    int fd;
    unsigned count;
    struct PackEntry *entries;
} Pack;

/* Where packs live: "host:data/" under PCSX2 (HostFs on) and ps2link; booted from a disc, pack.c maps these paths
 * onto cdrom0:\\DATA\\ (`make iso`, tools/ps2-iso.sh) */
#define PACK_ROOT "host:data/"
#ifndef CHR_DIR
#define CHR_DIR ""  /* enemy / common packs of the area, under PACK_ROOT: "" for Firelink, "<area>/" otherwise (Makefile MAP) */
#endif

/* Open a pack and read its table of contents; 0 on failure */
int pack_open(Pack *p, const char *path);
/* Load one entry into a new 128-byte aligned heap buffer (written back from the D-cache, ready for DMA).
 * Returns NULL if missing or on a read/decompression error; *size = its raw size. Free with free(). */
void *pack_load(Pack *p, const char *name, unsigned *size);
/* The same with the output and staging buffers from get / put (sizes rounded: put gets the size get was asked for;
 * the caller hands the output back with put(out, (*size + 127) & ~127)): enemy.c reads streamed models into the
 * near-cell arena, so their temporary buffers leave no holes in the heap */
void *pack_load_with(Pack *p, const char *name, unsigned *size, void *(*get)(unsigned), void (*put)(void *, unsigned));
void pack_close(Pack *p);
/* Entries stored raw (tools/pack.py name=raw:file) can be read in place: file offset and size of the data; 0 if the
 * entry is missing or compressed */
int pack_entry_raw(Pack *p, const char *name, unsigned *offset, unsigned *size);
/* Raw (decoded) size of an entry, 0 if the pack has none */
unsigned pack_entry_size(const Pack *p, const char *name);
/* Background reads of bytes at a file offset into dst (64-byte aligned: the D-cache lines are written back and
 * dropped first, and nothing may write them until it lands). pack_req_read returns a ticket (0: every request slot is
 * taken, try again next frame); several can be in flight, served in order of submission after any synchronous load.
 * pack_req_done: 1 once ticket t landed (*result = bytes read or < 0; the ticket is spent), then invalidate dst's cache
 * lines before reading it on the EE. pack_req_cancel drops one: a read already started stops after its current 64 KB
 * page; it returns once nothing more can land in dst. */
int pack_req_read(Pack *p, unsigned offset, void *dst, unsigned bytes);
int pack_req_done(int t, int *result);
void pack_req_cancel(int t);
/* pack_req_cancel without the wait: 1 when the ticket is gone, 0 when the read stops after its current page and still
 * lands through pack_req_done (dst stays written until then). mapstream.c: the blocking cancel held the frame for that
 * page and its seek, 0.2-1 s on the disc (Burg soak, 2026-11-07). */
int pack_req_cancel_async(int t);
/* the same with one implicit ticket: returns 0 while the last one is not collected by pack_async_done */
int pack_read_async(Pack *p, unsigned offset, void *dst, unsigned bytes);
int pack_async_done(int *result);
/* Move requests along (sends queued ones, collects landed ones); every request call does it too */
void pack_pump(void);
/* Background load of one entry, open to decode (the decode a few blocks per step, PACK_JOB_DECODE_MS), without a
 * blocking wait (from cdrom0: an open is a directory search of ~0.1-0.3 s and a 300 KB read up to ~1 s in PCSX2:
 * synchronous, each was a frame stall). Start it, then call pack_job_step once a frame: 0 pending, 1 done (out = the
 * raw entry, size raw, from get(alloc): put it back with alloc), -1 failed (logged). Each step's request is a
 * background request of its own. Cancel drops a job that is no longer wanted. */
typedef struct {
    int state, fd;
    unsigned off, packed, raw, blocks, bsize, alloc;
    int res, tk;      /* the ticket of its request in flight (0 none); res: a cached header's size */
    int fdc, reopen;  /* its fd's slot in pack.c's handle cache (-1 none); an open retried after closing cached ones */
    unsigned cs[64], b, pos;  /* decode: block sizes (the output overwrites the table in the buffer), next block */
    const char *xname[2];     /* set after pack_job_start: other entries to look up in the pack's table, found ... */
    unsigned xoff[2], xsize[2];  /* ... as pack_entry_raw would (offset / size of a raw entry; size 0 missing / packed) */
    const unsigned char *src;
    unsigned char *out, *in;
    const char *name;
    void *(*get)(unsigned);
    void (*put)(void *, unsigned);
    char path[96];
    /* sink mode (pack_job_sink, before the first step): the entry is not decoded into one buffer of its raw size. Its
     * packed blocks are read a run at a time into a staging buffer (get(bsize + PACK_STAGE_IN)), each block decoded
     * there and handed to sink(ctx, raw offset, bytes, n) in order; sink returns 0 to fail the job (out of RAM). On
     * success out is NULL: the data is wherever the sink put it. */
    int (*sink)(void *ctx, unsigned off, const unsigned char *p, unsigned n);
    void *sink_ctx;
    unsigned rb_end, rb_bytes;  /* sink mode: the end of the block run read now, its packed bytes */
    unsigned head[512] __attribute__((aligned(64)));  /* the pack's first sector (header + entry table) */
} PackJob;
void pack_job_start(PackJob *j, const char *path, const char *name, void *(*get)(unsigned), void (*put)(void *, unsigned));
int pack_job_step(PackJob *j);
/* after pack_job_start: deliver the entry to sink in order instead of one buffer (see PackJob.sink) */
void pack_job_sink(PackJob *j, int (*sink)(void *, unsigned, const unsigned char *, unsigned), void *ctx);
void pack_job_cancel(PackJob *j);
/* as pack_job_cancel without waiting for a read in flight: the job is left as cancelled at once, its buffer and file go
 * back once the read lands (PACK_JOB_ABANDON) */
void pack_job_abandon(PackJob *j);
/* Kept for its callers: there is no shared async slot any more, every request has its own */
void pack_async_free(void);
/* Wait for pack_read_async's read (its result stays for pack_async_done) and, from host:, for fio's request in flight
 * (fio back in WAIT mode). Other fio users (save.c) call it first. */
void pack_async_wait(void);
/* The device path pack_open reads for `path` (cdrom0:\\DATA\\... when booted from a disc): what an IOP module opens to
 * read a raw entry itself (src/sound.c). Returns path or out. */
const char *pack_resolve(const char *path, char *out, unsigned cap);
/* Last load failure ("OUT OF RAM: <entry> needs ..K, free ..K" / "READ ERROR: <entry>"), empty if none; also printed */
extern char pack_error[96];
/* Called over and over while pack_open / pack_load wait for the IOP (src/loading.c draws the loading screen there);
 * it may queue background requests of its own. NULL: the wait spins. */
extern void (*pack_wait_hook)(void);
/* 1 when packs come from cdrom0: (boot device, set on the first pack_open), 0 for host:, -1 before */
extern int pack_cdrom;
/* cdrom0: goes through dsrsnd's disc server (iop/dsrsnd, the only reader of the drive while it streams sound).
 * pack_disc_boot is called once, when the first pack path is resolved to cdrom0: (src/sound.c: loads the IOP modules,
 * then pack_disc_attach); without it, or if it fails, cdrom0: is read through fio. */
int pack_disc_boot(void);
int pack_disc_attach(void);
/* the disc server's counters (dsrsnd_rpc.h SndDiscStat) as of its last reply; 0 without the server (host: runs).
 * pack_disc_poll asks for a fresh copy (an empty submit) when no request went out since the last call */
int pack_disc_stats(void *out /* SndDiscStat */);
void pack_disc_poll(void);
/* the media the disc server found (dsrsnd_rpc.h SNDM_*: 0 none yet / host:) */
unsigned pack_disc_media(void);

/* Totals since boot: compressed bytes read, raw bytes produced, EE ticks (294.912 MHz) spent in pack_load and the
 * part of those spent waiting for the IOP, heap bytes handed out by pack_load */
typedef struct { unsigned read, raw, ticks, wait_ticks, heap; } PackStats;
extern PackStats pack_stats;

#endif
