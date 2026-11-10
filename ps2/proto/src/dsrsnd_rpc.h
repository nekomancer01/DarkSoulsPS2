/* Protocol between the EE sound runtime (src/sound.c) and its IOP module (iop/dsrsnd/dsrsnd.c, dsrsnd.irx).
 * The EE owns every decision (SPU2 memory map, voice allocation, 3D volumes); the IOP only applies them through libsd
 * and keeps the two disc streams fed.
 *   SNDF_INIT    (no args) -> SndStatus: libsd cold init, both cores unmuted, no reverb
 *   SNDF_LOAD    SndLoad -> SndStatus: the IOP reads a byte range of a file into SPU2 RAM in the background (between
 *                stream refills, SND_LOAD_STEP bytes at a time); status.load_done = its id once it is in
 *   SNDF_FRAME   SndFrame -> SndStatus: this frame's voice commands, key offs before key ons
 *   SNDF_STREAM  SndStreamOpen -> SndStatus: (re)start a disc stream in a slot
 *   SNDF_DISC_INIT   SndDiscInit -> SndDiscStat: where the EE wants the completion records (once, at boot)
 *   SNDF_DISC_SUBMIT SndDiscReq[n] -> SndDiscStat: queue opens / reads / closes / cancels for the disc server
 * Disc server (src/pack.c's cdrom0: backend): the stream thread is the only reader of the drive. The BIOS CD file system
 * takes one reader at a time, so every cdrom0: open / read / close of the EE goes through here too: the EE queues
 * requests (up to SND_DISC_SLOTS at once, without waiting), the thread serves the most urgent one SND_DISC_PAGE bytes
 * at a time between stream refills and bank load steps, sends each page to the EE by SIF DMA and then the request's
 * SndDiscDone record into the EE's ring (done[slot]); the EE polls that record uncached.
 * Voices: 0-23 core 0, 24-47 core 1. Stream slot s plays on core 1 voices SND_STREAM_VOICE(s) (left) and + 1 (right).
 * Streamed data (tools/export_audio.py str.bin): SPU2 ADPCM in chunks of `chunk` bytes per channel, channels
 * interleaved by chunk ([L chunk][R chunk][L chunk]...); the IOP rewrites the block flags for its ring buffer. */
#ifndef DSRSND_RPC_H
#define DSRSND_RPC_H

#define DSRSND_RPC_ID 0x0D5A5D01      /* frames (SNDF_INIT, SNDF_FRAME) */
#define DSRSND_RPC_OPS_ID 0x0D5A5D02  /* ops (SNDF_LOAD, SNDF_STREAM) */
#define DSRSND_RPC_DISC_ID 0x0D5A5D03 /* disc server (SNDF_DISC_*) */
#define SNDF_INIT 1
#define SNDF_LOAD 2
#define SNDF_FRAME 3
#define SNDF_STREAM 4
#define SNDF_DISC_INIT 5
#define SNDF_DISC_SUBMIT 6

#define SND_LOAD_STEP 16384
#define SND_CMD_MAX 96
#define SND_VOICES 48
#define SND_STREAMS 3  /* 0 music, 1 area ambience, 2 long emitter / one-shot sounds */
#define SND_STREAM_VOICE(s) (44 - 2 * (s))  /* slot 0: 44 / 45, 1: 42 / 43, 2: 40 / 41 */
#define SND_STREAM_CHUNK_MAX 8192

/* command ops */
#define SNDC_KEYON 1       /* voice, addr (SSA), pitch, voll, volr, adsr2 (release) */
#define SNDC_KEYOFF 2      /* voice */
#define SNDC_VOL 3         /* voice, voll, volr */
#define SNDC_PITCH 4       /* voice, pitch */
#define SNDC_MASTER 5      /* voll, volr: both cores' master volume */
#define SNDC_STREAM_VOL 6  /* voice = slot, voll, volr */
#define SNDC_STREAM_STOP 7 /* voice = slot */

typedef struct {
    unsigned char op, voice;
    unsigned short pitch;
    unsigned int addr;
    unsigned short voll, volr, adsr2, pad;
} SndCmd;  /* 16 bytes */

typedef struct {
    unsigned int ncmds, pad[3];
    SndCmd cmd[SND_CMD_MAX];
} SndFrame;

/* stream states */
#define SNDS_IDLE 0
#define SNDS_PLAYING 1
#define SNDS_ENDING 2  /* non-looping stream past its data: silence until the voices are keyed off */
#define SNDS_ERROR 3
#define SNDS_OPENING 4  /* opened by the EE, waiting for the disc */

typedef struct {
    unsigned int endx[2];        /* ENDX switch of core 0 / core 1: voices that reached an end block */
    unsigned int stream_state[SND_STREAMS];
    unsigned int stream_bytes[SND_STREAMS];  /* bytes of the stream read so far (all channels) */
    unsigned int errors, ok;     /* failed reads / opens since boot; 1 once SNDF_INIT ran */
    unsigned int load_done, load_failed;  /* id of the last finished / failed SNDF_LOAD */
    unsigned int late;           /* stream refills since boot that came over 1.5 ring halves after the last one */
    unsigned int underruns;      /* streams muted since boot: the voice reached a half not refilled yet (it would have
                                    replayed stale audio); keyed on again from the same file position once data is in */
    unsigned int starving;       /* bit per stream slot: its read-ahead is low or a refill waits for the drive (the
                                    disc thread leaves the drive to the stream thread after its current page) */
    unsigned int pad;
} SndStatus;  /* 64 bytes */

/* ---- disc server ---- */
#define SND_DISC_SLOTS 32          /* requests in flight; the EE's slot index is also its SndDiscDone's */
#define SND_DISC_BATCH 8           /* requests per SNDF_DISC_SUBMIT */
/* bytes per disc read of an EE request: the stream thread waits at most one for the drive. 64 KB pages made waits of
 * up to 164 ms on the ISO (the drive gave 430-770 KB/s with the seeks between map and audio reads, 2026-11-05) */
#define SND_DISC_PAGE (32 * 1024)
#define SNDD_OPEN 1    /* path -> result fd (< 0 error) */
#define SNDD_READ 2    /* fd, offset, size bytes to EE dst -> result bytes read */
#define SNDD_CLOSE 3   /* fd -> result */
#define SNDD_CANCEL 4  /* stop the read in slot / seq after its current page (result -1); takes no slot itself */
#define SNDD_PRIO_SYNC 0  /* the EE waits for it (pack_open, pack_load): before every background request */
#define SNDD_PRIO_BG 1    /* background jobs (enemy models, sound banks, menus): DSR's "Default" read queue */
/* MS_READQ (pack.h): DSR's own file reader keeps five queues and serves them strictly in this order: Default,
 * BackRead_MapHit, BackRead_MapDefault, BackRead_MapTpf, Prefetch (the strings FUN_14059f530 names them with; the
 * per-frame service FUN_14059e300 drains them in index order). The background reads of the world map the same way;
 * inside one priority the server still goes in disc order. Lower is more urgent. */
#define SNDD_PRIO_HIT 2   /* a block's collision (tiles, lite sets): BackRead_MapHit */
#define SNDD_PRIO_MAP 3   /* map cells wanted now (near parts, coarse cells in view): BackRead_MapDefault */
#define SNDD_PRIO_TPF 4   /* map textures (mip 0, streamed mip 1), the relief: BackRead_MapTpf */
#define SNDD_PRIO_PREF 5  /* read-ahead and out-of-view cells: Prefetch */

typedef struct {
    unsigned int seq;        /* the EE's request number, echoed in its SndDiscDone */
    unsigned short slot;     /* 0 .. SND_DISC_SLOTS - 1, free on the EE until its SndDiscDone arrives */
    unsigned char op, prio;
    int fd;
    unsigned int offset, size, dst;  /* dst: EE address, any alignment (the edges come in the SndDiscDone) */
    unsigned int pad;
    char path[96];           /* SNDD_OPEN: cdrom0:\... */
} SndDiscReq;  /* 128 bytes */

typedef struct {
    unsigned char edge[32];  /* bytes of dst the DMA cannot write (not within 16-byte aligned qwords): head, then tail */
    int result;
    unsigned short nhead, ntail;
    unsigned int pad[5];
    unsigned int seq;        /* last: written by the DMA after the rest; == the request's seq once it is done */
} SndDiscDone;  /* 64 bytes */

typedef struct {
    unsigned int done;  /* EE address of SndDiscDone[SND_DISC_SLOTS] (64-byte aligned) */
    unsigned int pad[3];
} SndDiscInit;

/* media (SndDiscStat.media): what the disc server found in the drive at its first cdrom0: open */
#define SNDM_NONE 0   /* not scanned yet, or no disc (host: runs) */
#define SNDM_CD 1
#define SNDM_DVD 2    /* single layer */
#define SNDM_DVD9 3   /* dual layer (parallel track path): layer1 = the first LSN of layer 1 */
/* the drive's own layer: on a single-layer disc the ISO builder's planned break (DSRLAYER.BIN, tools/iso9660.py) stands
 * in for layer 1, so a DVD-5 test image counts the layer changes a DVD-9 pressing of the same layout would make */

typedef struct {
    unsigned int iop_free_boot, iop_free;  /* free IOP RAM before dsrsnd's buffers, and now (bytes) */
    unsigned int reqs, pages, kb;          /* served since boot (EE requests; kb: bytes sent to the EE) */
    unsigned int wait_max_ms, waits_100;   /* longest gap between stream service passes while a stream played, > 100 ms */
    /* drive counters since boot (every sector read: EE pages, stream read-ahead, banks, the directory scan) */
    unsigned int media, layer1;            /* SNDM_*; layer 1's first LSN (0: one layer), planned one on a single layer */
    unsigned int drive_kb, drive_ms;       /* KB the drive read; ms spent in sceCdRead + sceCdSync */
    unsigned int seeks, far_seeks;         /* reads that did not start at the head; of those beyond the fast seek window */
    unsigned int layer_changes;            /* reads on the other layer than the last one */
    unsigned int lat_ms, lat_n, lat_max;   /* EE reads: submit to done, summed / count / longest (ms) */
    unsigned int cache_hits, cache_miss;   /* EE pages served from the IOP page cache / read from the drive into it */
    unsigned int cache_kb;                 /* the page cache's size (0: none) */
    unsigned int cache_direct;             /* of the cache's pages, those sent to the EE without a copy */
    unsigned int pad[2];
} SndDiscStat;  /* 96 bytes */

typedef struct {
    unsigned int id, offset, size, spu_addr;  /* size: multiple of 64 */
    char path[96];
} SndLoad;

typedef struct {
    unsigned int slot, offset, size, loop;  /* byte offset and size of the interleaved data in the file; loop 1 = repeat */
    unsigned int chunk, spu_buf;            /* bytes per channel per chunk (<= SND_STREAM_CHUNK_MAX, multiple of 64);
                                               SPU2 RAM for 2 x chunk per channel (left ring, then right) */
    unsigned short channels, pitch, voll, volr;
    char path[96];                          /* IOP path: host:... or cdrom0:\...;1 */
} SndStreamOpen;

#endif
