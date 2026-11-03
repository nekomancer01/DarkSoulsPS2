/* Asset packs (tools/pack.py): LZ4 in 64 KB blocks, loaded at runtime from host: (PCSX2 HostFs, root = the ELF's
 * folder) or cdrom0: later. Reads go to the IOP asynchronously (fio NOWAIT) while the EE decompresses the previous
 * batch of blocks. */
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
/* Background read of bytes at a file offset into dst (64-byte aligned; written back from the D-cache first). One at a
 * time: returns 0 while another is pending. pack_async_done: 1 once it finished (*result = bytes read or < 0), then
 * invalidate dst's cache lines before reading it on the EE. pack_load / pack_open wait for a pending read first. */
int pack_read_async(Pack *p, unsigned offset, void *dst, unsigned bytes);
int pack_async_done(int *result);
/* Background load of one entry, open to decode, without a blocking fio call (from cdrom0: an open is a directory search
 * of ~0.1-0.3 s and a 300 KB read up to ~1 s in PCSX2: synchronous, each was a frame stall). Start it, then call
 * pack_job_step once a frame: 0 pending, 1 done (out = the raw entry, size raw, from get(alloc): put it back with
 * alloc), -1 failed (logged). Shares the one async slot with pack_read_async (a step waits while another read is in
 * flight). Cancel drops a job that is no longer wanted. */
typedef struct {
    int state, fd;
    unsigned off, packed, raw, blocks, bsize, alloc;
    int res, parked;  /* result of its request kept by pack_async_free */
    unsigned char *out, *in;
    const char *name;
    void *(*get)(unsigned);
    void (*put)(void *, unsigned);
    char path[96];
} PackJob;
void pack_job_start(PackJob *j, const char *path, const char *name, void *(*get)(unsigned), void (*put)(void *, unsigned));
int pack_job_step(PackJob *j);
void pack_job_cancel(PackJob *j);
/* Free the async slot now: a job's pending request is waited for and its result kept for the job's next step.
 * Loops that wait for their own reads without running the frame (world.c settle) call it first. */
void pack_async_free(void);
/* Wait for a pending background read (fio back in WAIT mode; its result stays for pack_async_done). Other fio users
 * (save.c) call it first. */
void pack_async_wait(void);
/* Last load failure ("OUT OF RAM: <entry> needs ..K, free ..K" / "READ ERROR: <entry>"), empty if none; also printed */
extern char pack_error[96];
/* 1 when packs come from cdrom0: (boot device, set on the first pack_open), 0 for host:, -1 before */
extern int pack_cdrom;

/* Totals since boot: compressed bytes read, raw bytes produced, EE ticks (294.912 MHz) spent in pack_load and the
 * part of those spent waiting for the IOP, heap bytes handed out by pack_load */
typedef struct { unsigned read, raw, ticks, wait_ticks, heap; } PackStats;
extern PackStats pack_stats;

#endif
