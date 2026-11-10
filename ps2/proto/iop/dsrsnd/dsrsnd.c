/* dsrsnd.irx: IOP side of the sound runtime (EE side src/sound.c, protocol src/dsrsnd_rpc.h).
 * One RPC thread applies the EE's commands through libsd (uploads on SPU2 DMA channel 0, voice registers); one stream
 * thread keeps up to SND_STREAMS disc streams fed: every few ms it reads each stream voice's next address (NAX), and
 * once the voice has moved into the other half of its 2 x chunk ring, it reads the next chunk of every channel from
 * the file (ioman: host: under PCSX2, cdrom0: from a disc) and DMAs it over the half just played (channel 1). Block
 * flags are rewritten for the ring: the first block of half 0 sets the loop start, the last block of half 1 jumps back
 * to it. The same thread loads sample banks (SNDF_LOAD): SND_LOAD_STEP bytes from the file into SPU2 RAM per pass, so
 * the EE spends neither RAM nor time on them. libsd calls of both threads go through one semaphore.
 * It is also the EE's disc server (dsrsnd_rpc.h SNDF_DISC_*): a third thread, below every other one, reads the EE's
 * requests one SND_DISC_PAGE at a time. The two disc threads take turns at the drive through cd_sema (the BIOS CD file
 * system takes one reader at a time); the stream thread never waits for it: when the drive is busy it asks for it
 * (cd_want) and the disc thread leaves it after its current page. */
#include "irx_imports.h"
#include "dsrsnd_rpc.h"

IRX_ID("dsrsnd", 1, 0);

#define STREAM_DMA 1  /* SPU2 DMA channel of the stream thread (refills and bank loads) */
#define POLL_US 4000  /* stream thread period */
#define STREAM_PRIO 30       /* the stream thread: above the RPC threads (40-42), below the CD driver */
/* the same while it holds the drive: below every thread but the disc thread (120). A CD read keeps its thread busy
 * until the data is in, so at 45 a read-ahead top-up (up to 128 KB, ~150 ms) held up fileio for its whole length:
 * every EE printf in that time waited (frames of 100-480 ms; 57-59 frames over 100 ms on the ISO route, 2026-11-06).
 * Above the disc thread, so the EE's page reads never hold a top-up back. */
#define STREAM_PRIO_DISC 119
/* read-ahead per stream: one disc read fills this much, the refills then come from IOP RAM. Each read from a stream
 * pulls the drive away from the game's own pack reads and back (two seeks); with 16 KB refills the looping area
 * ambience alone cut a cdrom0: boot load from 7 s to 37 s in PCSX2 (2026-11-03). */
#define STREAM_RA (128 * 1024)
/* Refills come from the read-ahead without the drive; the drive is only taken to top the read-ahead up, from the
 * moment it is down to half (~2.6 s of 22 kHz stereo left). Before (2026-11-05, ISO world_route) every refill waited
 * for the EE's disc lock while the EE streamed the map: 298 of 358 s locked out, 137 underruns. */
#define STREAM_RA_TOP (STREAM_RA / 2)
#define STREAM_RA_ALSO (STREAM_RA * 3 / 4)  /* the others top up with it from here */
/* below this the stream is starving (~1.3 s left, or a half it cannot fill): reported in status.starving */
#define STREAM_RA_LOW (STREAM_RA / 4)
/* ioman's file table holds 16 handles (2 taken before the game), shared by the EE's packs (world blocks, menu.pak,
 * pack.c's job cache) and this module's streams and bank load: in a world build with enemies streaming in it ran full
 * (ISO 2026-11-05, smoke: 11 EE handles + 3 streams). An open of this module's own that finds it full is retried on
 * later passes for up to OPEN_RETRY_MS instead of failing the stream or bank. */
#define EMFILE_ 24
#define OPEN_RETRY_MS 10000
#define ADSR1_FLAT SD_SET_ADSR1(SD_ADSR_AR_LINEARi, 0, 0xf, 0xf)  /* instant attack, no decay, sustain level max */
#define ADSR2_FLAT(rr) SD_SET_ADSR2(SD_ADSR_SR_LINEARi, 0x7f, SD_ADSR_RR_LINEARd, (rr))  /* sustain held */

/* Two servers, one per EE client (frames / ops), each with its own thread and receive buffer: a client DMAs its
 * arguments straight into the server's buffer, so two calls in flight on one server overwrote each other (the ops
 * call never got served: a hang walking into the Burg, 2026-11-03) */
static SifRpcDataQueue_t queue[3];
static SifRpcServerData_t server[3];
static unsigned int rpc_buf[3][(sizeof(SndFrame) + 63) / 4] __attribute__((aligned(64)));
static SndStatus status __attribute__((aligned(64)));
static int sd_sema = -1;
/* the drive (ioman: cdrom0:, or host:): the stream thread and the disc thread take turns, one call at a time */
static int cd_sema = -1;
static volatile int cd_want;     /* the stream thread found the drive busy: the disc thread leaves it after its page */
static unsigned int cd_want_ms;  /* when it first did */

typedef struct {
    volatile int state;
    int fd, loop, channels, chunk, next_fill, silent;
    int close_fd;  /* stopped by an RPC: closed by the stream thread once it has the drive */
    unsigned int offset, size, pos, spu_buf;
    unsigned char *buf;  /* chunk * channels */
    unsigned char *ra;   /* STREAM_RA bytes read ahead (NULL: no IOP RAM for it, read per refill) */
    unsigned int ra_pos, ra_len;  /* ring: next byte to play from it, bytes held (in play order, loops unrolled) */
    unsigned int rd_pos;          /* file position (0..size) of the next byte read into the read-ahead */
    unsigned int half_ms, last_fill;  /* one ring half's play time; when the last half was filled (ms) */
    int due, starved;    /* due: the voice left a half that is not refilled yet; starved: keyed off by an underrun */
    SndStreamOpen req;   /* SNDS_OPENING: the open the stream thread does once it has the drive */
    unsigned int open_t0;  /* when its open first found ioman's file table full (0: it did not) */
} Stream;
static Stream streams[SND_STREAMS];
static unsigned char *stream_mem;  /* SND_STREAMS x 2 x SND_STREAM_CHUNK_MAX, then SND_LOAD_STEP for loads */
static SndLoad load;               /* the bank load in progress (load_fd >= 0) or queued (load_new) */
static int load_fd = -1, load_new;
static unsigned int load_pos;
static int stream_sema = -1;       /* guards streams[] between the two threads */
/* The RPC threads only post stream opens / stops and bank loads here (req_sema, held for a copy); the stream thread
 * takes them at the start of its next pass. Before, they waited for stream_sema, which the stream thread holds over
 * its drive calls: a music change or bank load during a read-ahead top-up kept the EE's frame or ops RPC waiting for
 * the whole read (frames of 200-480 ms after "snd: stream" / "snd: map", ISO world_cross 2026-11-05). */
static int req_sema = -1;
static SndStreamOpen open_req[SND_STREAMS];
static int open_pend[SND_STREAMS], stop_pend[SND_STREAMS], load_pend;
static SndLoad load_req;

static inline void sd_lock(void) { WaitSema(sd_sema); }
static inline void sd_unlock(void) { SignalSema(sd_sema); }
static inline int vcore(int v) { return v >= 24; }
static inline int vreg(int v) { return SD_VOICE(vcore(v), v % 24); }

static void init_spu(void)
{
    sceSdInit(0);
    for (int c = 0; c < 2; c++) {
        sceSdSetCoreAttr(c | SD_CORE_EFFECT_ENABLE, 0);  /* no reverb: its work area would overwrite samples */
        sceSdSetCoreAttr(c | SD_CORE_MUTE_ENABLE, 0);
        sceSdSetParam(c | SD_PARAM_EVOLL, 0);
        sceSdSetParam(c | SD_PARAM_EVOLR, 0);
        sceSdSetParam(c | SD_PARAM_MVOLL, 0x3fff);
        sceSdSetParam(c | SD_PARAM_MVOLR, 0x3fff);
        sceSdSetSwitch(c | SD_SWITCH_KOFF, 0xffffff);
        for (int v = 0; v < 24; v++) {
            sceSdSetParam(SD_VOICE(c, v) | SD_VPARAM_VOLL, 0);
            sceSdSetParam(SD_VOICE(c, v) | SD_VPARAM_VOLR, 0);
            sceSdSetParam(SD_VOICE(c, v) | SD_VPARAM_ADSR1, ADSR1_FLAT);
            sceSdSetParam(SD_VOICE(c, v) | SD_VPARAM_ADSR2, ADSR2_FLAT(8));
        }
    }
    /* core 0's output is core 1's external input (MMIX defaults); core 1 drives the DAC */
    sceSdSetParam(1 | SD_PARAM_AVOLL, 0x7fff);
    sceSdSetParam(1 | SD_PARAM_AVOLR, 0x7fff);
    sceSdSetParam(1 | SD_PARAM_BVOLL, 0);
    sceSdSetParam(1 | SD_PARAM_BVOLR, 0);
    status.ok = 1;
}

/* ---- streams ---- */

static void stream_close(Stream *s, int slot)
{
    int v = SND_STREAM_VOICE(slot);
    sd_lock();
    sceSdSetSwitch(1 | SD_SWITCH_KOFF, 3 << (v % 24));
    sd_unlock();
    if (s->fd >= 0)
        s->close_fd = s->fd;
    s->fd = -1;
    s->state = SNDS_IDLE;
}

/* ms since boot (bus clock 36.864 MHz; no 64-bit divide in an IRX) */
static unsigned int now_ms(void)
{
    iop_sys_clock_t c;
    GetSystemTime(&c);
    return c.hi * 116508u + c.lo / 36864u;
}

/* ---- files on the disc, read by sector ----
 * Every disc file this module reads (the EE's packs, streams, banks) is found once in a table of start sectors and
 * sizes, built from the ISO9660 directory at the first cdrom0: open, and then read with sceCdRead: no BIOS CD file
 * system open (a directory search of ~0.1-0.3 s per pack in PCSX2), no ioman file handle (16 in all, which ran full
 * once enemy types streamed in), the same reads for every file. The Ratchet & Clank / Sly Cooper pattern (a sector
 * table, read by sector), with the table made from the disc itself so tools/ps2-iso.sh and the web converter need no
 * change. Other paths (host:), or a disc whose directory cannot be read, go through ioman as before. The df_* calls
 * mirror open / lseek / read / close; the caller holds the drive (cd_sema) for all of them. */
#define DF_FILES 512    /* files on the disc (92 on the 2026-11-06 ISO) */
#define DF_DIRS 64      /* directories (6) */
#define DF_NAME 44      /* "DATA\10010000\C1000.PAK" without ";1" (longest 26) */
#define DF_HANDLES 64   /* opens at once: the EE's packs, streams, bank load */
#define DF_FD 0x1000    /* handle h is fd DF_FD + h, above any ioman fd */
#define DF_TRIES 4      /* reads of a sector run before an error */
typedef struct {
    unsigned int lsn, size;
    unsigned int lsn1;  /* its copy on layer 1 (DSRLAYER.BIN), 0: none */
    char name[DF_NAME];
} DfFile;
static DfFile *df_tab;  /* DF_FILES, from _start */
static int df_n, df_state;  /* files in df_tab; 0 not scanned yet, 1 ready, -1 no table (ioman for everything) */
static struct {
    int file;  /* index in df_tab, -1 free */
    unsigned int pos;
    unsigned int lsn;  /* the copy this handle reads: the one on the layer the drive was on at its open */
} df_h[DF_HANDLES];
static unsigned char df_sec[2048] __attribute__((aligned(64)));  /* partial sectors, directory sectors */
static sceCdRMode df_mode = {.trycount = 0, .spindlctrl = SCECdSpinNom, .datapattern = SCECdSecS2048};
static unsigned int df_reads, df_errors;
static unsigned int cd_head;  /* the sector after the last one read: where the drive's head is (disc_step's sweep) */
static unsigned int df_volume;  /* sectors in the volume (layer 0's PVD; the end of what a page-cache block may read) */
static SndDiscStat dstat __attribute__((aligned(64)));

/* The drive's media and layers (budget doc "Disc and read-ahead", Sony's dual-layer notes): a DVD-9 is parallel track
 * path, LSNs carry on across the break, layer 0 reads in its own spin mode (SCECdSpinDvdDL0, constant ~2.2 MB/s) and
 * layer 1 in the normal one. The fast seek window (seeks inside it move the lens only) is ~+/-4371 sectors on CD,
 * +/-14764 on DVD-5 and +/-13360 on DVD-9. On a real dual-layer disc dstat.layer1 is the drive's own break, else the one
 * the ISO builder planned (df_layout), counted but not switched. */
static int cd_layer = -1, cd_dual;  /* layer of the last read (-1 none yet); 1: a real dual-layer disc */
static unsigned int cd_window = 4371;

static int cd_read1(unsigned int lsn, unsigned int n, void *buf)
{
    int l = dstat.layer1 && lsn >= dstat.layer1, lc = cd_layer >= 0 && l != cd_layer;
    dstat.layer_changes += lc;
    cd_layer = l;
    df_mode.spindlctrl = cd_dual && !l ? SCECdSpinDvdDL0 : SCECdSpinNom;
    if (lsn != cd_head) {
        unsigned int d = lsn > cd_head ? lsn - cd_head : cd_head - lsn;
        dstat.seeks++;
        dstat.far_seeks += d > cd_window || lc;
    }
    unsigned int t0 = now_ms();
    for (int t = 0; t < DF_TRIES; t++) {
        if (sceCdRead(lsn, n, buf, &df_mode)) {
            sceCdSync(0);
            if (!sceCdGetError()) {
                df_reads++;
                cd_head = lsn + n;
                dstat.drive_ms += now_ms() - t0;
                dstat.drive_kb += n * 2;
                return 0;
            }
        }
        df_errors++;
        DelayThread(10000);
    }
    dstat.drive_ms += now_ms() - t0;
    printf("dsrsnd: sector read %u x %u failed (error %d)\n", lsn, n, sceCdGetError());
    return -1;
}

/* n sectors from lsn into buf (4-byte aligned): 0, or -1 after DF_TRIES failed reads. A run across the layer break
 * goes in two reads (one per layer and spin mode) */
static int cd_sectors(unsigned int lsn, unsigned int n, void *buf)
{
    unsigned int b = dstat.layer1;
    if (b && lsn < b && lsn + n > b) {
        if (cd_read1(lsn, b - lsn, buf) < 0)
            return -1;
        return cd_read1(b, lsn + n - b, (unsigned char *)buf + (b - lsn) * 2048);
    }
    return cd_read1(lsn, n, buf);
}

/* the disc in the drive (at the first cdrom0: open): CD or DVD, and on a DVD whether it has two layers */
static void cd_media(void)
{
    int t = sceCdGetDiskType();
    dstat.media = SNDM_CD;
    if (t == SCECdPS2DVD || t == SCECdDVDV) {
        int dual = 0;
        unsigned int l1 = 0;
        dstat.media = SNDM_DVD;
        cd_window = 14764;
        sceCdMmode(SCECdMmodeDvd);
        /* (PCSX2's BIOS cdvdman, v2.20: the call returns without an answer, ok != 1; df_scan then looks for layer 1's
         * volume descriptor where layer 0's volume ends) */
        if (sceCdReadDvdDualInfo(&dual, &l1) == 1 && dual && l1) {
            dstat.media = SNDM_DVD9, dstat.layer1 = l1, cd_dual = 1;
            cd_window = 13360;
        }
    }
    printf("dsrsnd: media %s (disc type 0x%02x)%s", dstat.media == SNDM_DVD9 ? "DVD-9" : dstat.media == SNDM_DVD ? "DVD" : "CD",
           t, cd_dual ? "" : "\n");
    if (cd_dual)
        printf(", layer 1 from LSN %u\n", dstat.layer1);
}

/* a DVD whose drive did not say it has two layers: layer 1 starts where layer 0's volume ends, with a copy of the volume
 * descriptor there (parallel track path; tools/iso9660.py writes one, and PCSX2 finds the break the same way). One read
 * try: past the end of a single-layer disc it fails */
static void cd_layer1_probe(void)
{
    if (dstat.media != SNDM_DVD || !df_volume)
        return;
    if (!sceCdRead(df_volume, 1, df_sec, &df_mode))
        return;
    sceCdSync(0);
    if (sceCdGetError() || memcmp(df_sec, "\1CD001", 6))
        return;
    dstat.media = SNDM_DVD9, dstat.layer1 = df_volume, cd_dual = 1, cd_window = 13360;
    cd_head = df_volume + 1;
    printf("dsrsnd: media DVD-9, layer 1 from LSN %u (its volume descriptor)\n", df_volume);
}

static int df_upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

/* DSRLAYER.BIN (tools/iso9660.py): the layout the ISO builder planned, u32 magic "DSRL", version 1, layer 1's first
 * LSN (0: no second region), the volume's sectors, n copies, then n x {layer-0 LSN, layer-1 LSN} of the packs both
 * layers read in play (df_open picks the copy on the drive's layer). On a single-layer disc its break stands in for the drive's,
 * for the layer change count (dsrsnd_rpc.h SNDM_*); a real dual-layer disc keeps the drive's own */
static void df_layout(void)
{
    for (int i = 0; i < df_n; i++)
        if (!strcmp(df_tab[i].name, "DSRLAYER.BIN")) {
            unsigned int w[5];
            if (cd_sectors(df_tab[i].lsn, 1, df_sec) < 0)
                return;
            memcpy(w, df_sec, sizeof w);
            if (w[0] != 0x4C525344u || w[1] != 1)
                return;
            unsigned int nc = w[4] < (2048 - 20) / 8 ? w[4] : (2048 - 20) / 8, found = 0;
            for (unsigned int c = 0; c < nc; c++) {  /* layer-1 copies: {layer-0 LSN, layer-1 LSN} */
                unsigned int pr[2];
                memcpy(pr, df_sec + 20 + 8 * c, 8);
                for (int k = 0; k < df_n; k++)
                    if (df_tab[k].lsn == pr[0])
                        df_tab[k].lsn1 = pr[1], found++;
            }
            if (nc)
                printf("dsrsnd: %u of %u packs have a layer-1 copy\n", found, nc);
            if (cd_dual && w[2] && w[2] != dstat.layer1)
                printf("dsrsnd: layer break at LSN %u, the ISO builder planned %u\n", dstat.layer1, w[2]);
            if (!cd_dual && w[2]) {
                dstat.layer1 = w[2];
                printf("dsrsnd: planned layer break at LSN %u (one layer on this disc: counted, not switched)\n", w[2]);
            }
            return;
        }
}

/* the ISO9660 directory tree (primary volume descriptor at sector 16) into df_tab, breadth first */
static void df_scan(void)
{
    static struct {
        unsigned int lsn, size;
        char path[DF_NAME];
    } dirs[DF_DIRS];
    unsigned int t0 = now_ms();
    int nd = 0;
    df_state = -1, df_n = 0;
    if (!df_tab)
        return;
    sceCdDiskReady(0);
    cd_media();
    if (cd_sectors(16, 1, df_sec) < 0 || memcmp(df_sec, "\1CD001", 6)) {
        printf("dsrsnd: no ISO9660 volume on the disc, cdrom0: through ioman\n");
        return;
    }
    memcpy(&df_volume, df_sec + 80, 4);
    unsigned char pvd_root[34];
    memcpy(pvd_root, df_sec + 156, sizeof pvd_root);
    cd_layer1_probe();  /* (reads into df_sec) */
    const unsigned char *root = pvd_root;
    memcpy(&dirs[0].lsn, root + 2, 4);
    memcpy(&dirs[0].size, root + 10, 4);
    dirs[0].path[0] = 0;
    nd = 1;
    for (int d = 0; d < nd; d++) {
        unsigned int secs = (dirs[d].size + 2047) / 2048;
        for (unsigned int s = 0; s < secs; s++) {
            if (cd_sectors(dirs[d].lsn + s, 1, df_sec) < 0)
                return;
            if (s && !df_sec[0])
                break;  /* the empty rest of a directory (a DVD image's 64 KB root, tools/iso9660.py) */
            for (unsigned int i = 0; i < 2048 && df_sec[i];) {  /* records never cross a sector; 0 pads the rest */
                const unsigned char *r = df_sec + i;
                unsigned int len = r[0], nl = r[32];
                i += len;
                if (len < 34 || nl > len - 33 || (nl == 1 && r[33] <= 1))  /* "." and ".." */
                    continue;
                char name[DF_NAME];
                unsigned int k = 0, pl = strlen(dirs[d].path);
                if (pl + nl + 2 > sizeof name)
                    continue;
                memcpy(name, dirs[d].path, pl);
                k = pl;
                if (pl)
                    name[k++] = '\\';
                for (unsigned int c = 0; c < nl && r[33 + c] != ';'; c++)
                    name[k++] = df_upper(r[33 + c]);
                name[k] = 0;
                if (r[25] & 2) {
                    if (nd < DF_DIRS) {
                        memcpy(&dirs[nd].lsn, r + 2, 4);
                        memcpy(&dirs[nd].size, r + 10, 4);
                        memcpy(dirs[nd].path, name, k + 1);
                        nd++;
                    }
                } else if (df_n < DF_FILES) {
                    DfFile *f = &df_tab[df_n++];
                    memcpy(&f->lsn, r + 2, 4);
                    f->lsn1 = 0;
                    memcpy(&f->size, r + 10, 4);
                    memcpy(f->name, name, k + 1);
                }
            }
        }
    }
    df_state = 1;
    df_layout();
    printf("dsrsnd: disc table: %d files in %d directories, read by sector (%u ms)\n", df_n, nd, now_ms() - t0);
}

static int df_open(const char *path)
{
    const char *p = path;
    static const char dev[] = "cdrom0:";
    for (int i = 0; dev[i]; i++, p++)
        if (df_upper(*p) != df_upper(dev[i]))
            return open(path, O_RDONLY);
    if (!df_state)
        df_scan();
    while (*p == '\\' || *p == '/')
        p++;
    char name[DF_NAME];
    unsigned int k = 0;
    for (; *p && *p != ';' && k < sizeof name - 1; p++)
        name[k++] = *p == '/' ? '\\' : df_upper(*p);
    name[k] = 0;
    int f = -1;
    for (int i = 0; df_state > 0 && i < df_n && f < 0; i++)
        if (!strcmp(df_tab[i].name, name))
            f = i;
    if (f < 0)  /* not in the table (or no table): what the BIOS file system makes of it */
        return open(path, O_RDONLY);
    for (int h = 0; h < DF_HANDLES; h++)
        if (df_h[h].file < 0) {
            df_h[h].file = f, df_h[h].pos = 0;
            /* a pack read in play on both layers has a copy on each (tools/iso9660.py plan): the one on this layer */
            df_h[h].lsn = df_tab[f].lsn1 && cd_layer == 1 ? df_tab[f].lsn1 : df_tab[f].lsn;
            return DF_FD + h;
        }
    return -EMFILE_;
}

static inline int df_is(int fd) { return fd >= DF_FD && fd < DF_FD + DF_HANDLES && df_h[fd - DF_FD].file >= 0; }

static int df_close(int fd)
{
    if (!df_is(fd))
        return close(fd);
    df_h[fd - DF_FD].file = -1;
    return 0;
}

static int df_lseek(int fd, unsigned int off)
{
    if (!df_is(fd))
        return lseek(fd, off, SEEK_SET);
    df_h[fd - DF_FD].pos = off;
    return off;
}

/* n bytes at the handle's position: whole sectors straight into dst (4-byte aligned there), the partial ones through
 * df_sec. Bytes read, or -1 (a read error before any byte) */
static int df_read(int fd, void *dst, unsigned int n)
{
    if (!df_is(fd))
        return read(fd, dst, n);
    unsigned int *pos = &df_h[fd - DF_FD].pos;
    const DfFile *f = &df_tab[df_h[fd - DF_FD].file];
    if (*pos >= f->size)
        return 0;
    if (n > f->size - *pos)
        n = f->size - *pos;
    unsigned char *d = dst;
    unsigned int left = n;
    while (left) {
        unsigned int lsn = df_h[fd - DF_FD].lsn + *pos / 2048, skip = *pos & 2047, c;
        if (!skip && left >= 2048 && !((unsigned int)d & 3)) {
            c = left / 2048;
            if (cd_sectors(lsn, c, d) < 0)
                break;
            c *= 2048;
        } else {
            if (cd_sectors(lsn, 1, df_sec) < 0)
                break;
            c = 2048 - skip < left ? 2048 - skip : left;
            memcpy(d, df_sec + skip, c);
        }
        d += c, left -= c, *pos += c;
    }
    return left == n && n ? -1 : (int)(n - left);
}

/* df_read of n bytes at off into p, for a buffer buf that has 2048 bytes before p and 2048 after p + n: the sectors go
 * in one read, straight to their place when that is 4-byte aligned, else to buf and moved down */
static int df_pread_page(int fd, unsigned int off, unsigned char *p, unsigned int n, unsigned char *buf)
{
    if (!df_is(fd))
        return lseek(fd, off, SEEK_SET) < 0 ? -1 : read(fd, p, n);
    const DfFile *f = &df_tab[df_h[fd - DF_FD].file];
    if (off >= f->size)
        return 0;
    if (n > f->size - off)
        n = f->size - off;
    unsigned int skip = off & 2047, secs = (skip + n + 2047) / 2048;
    unsigned char *s = p - skip, *t = (unsigned int)s & 3 ? buf : s;
    if (cd_sectors(df_h[fd - DF_FD].lsn + off / 2048, secs, t) < 0)
        return -1;
    if (t != s)
        memmove(p, t + skip, n);
    return n;
}

/* bytes the read-ahead still has to read from the file (0: all of a non-looping stream is in) */
static inline int stream_ra_more(const Stream *s) { return s->loop || s->rd_pos < s->size; }
/* a playing stream whose read-ahead could take more of the file */
static inline int stream_ra_want(const Stream *s)
{
    return s->ra && !s->silent && (s->state == SNDS_PLAYING || s->state == SNDS_ENDING) && stream_ra_more(s);
}

/* Top the read-ahead up to full from the file (the caller holds the drive, cd_sema). Loops are unrolled into it: at the end
 * of a looping stream the read goes on from its start. Reads end on a sector boundary where they can. */
static int stream_ra_fill(Stream *s)
{
    while (s->ra_len < STREAM_RA && stream_ra_more(s)) {
        if (s->rd_pos >= s->size) {
            s->rd_pos = 0;
            df_lseek(s->fd, s->offset);
        }
        unsigned int w = (s->ra_pos + s->ra_len) % STREAM_RA, n = STREAM_RA - s->ra_len;
        if (n > STREAM_RA - w)
            n = STREAM_RA - w;
        if (n > s->size - s->rd_pos)
            n = s->size - s->rd_pos;
        else if (n > 2048)
            n -= (s->offset + s->rd_pos + n) & 2047;
        int r = df_read(s->fd, s->ra + w, n);
        if (r <= 0)
            return -1;
        s->ra_len += r, s->rd_pos += r;
    }
    return 0;
}

/* n bytes of the stream at the current position: from the read-ahead (the caller checked it holds them; no disc), or
 * without one from the file (the caller holds the drive, n <= what is left of the file: it wraps loops) */
static int stream_read(Stream *s, unsigned char *dst, unsigned int n)
{
    if (!s->ra)
        return df_read(s->fd, dst, n);
    if (n > s->ra_len)
        n = s->ra_len;
    unsigned int a = STREAM_RA - s->ra_pos < n ? STREAM_RA - s->ra_pos : n;
    memcpy(dst, s->ra + s->ra_pos, a);
    if (n > a)
        memcpy(dst + a, s->ra, n - a);
    s->ra_pos = (s->ra_pos + n) % STREAM_RA, s->ra_len -= n;
    return n;
}

/* Read the next chunk of every channel (looping or padding with silence at the end) and send it to ring half h.
 * 1 (nothing sent): the read-ahead does not hold the chunk yet, it waits for a top-up. */
static int stream_fill(Stream *s, int slot, int h)
{
    int want = s->chunk * s->channels, got = 0;
    if (!s->silent && s->ra) {
        unsigned int need = want;
        if (!s->loop && need > s->size - s->pos)
            need = s->size - s->pos;
        if (s->ra_len < need)
            return 1;
    }
    if (!s->silent) {
        while (got < want) {
            if (s->pos >= s->size) {
                if (!s->loop)
                    break;
                s->pos = 0;
                if (!s->ra)
                    df_lseek(s->fd, s->offset);
            }
            unsigned int n = want - got;
            if (n > s->size - s->pos)
                n = s->size - s->pos;
            int r = stream_read(s, s->buf + got, n);
            if (r <= 0) {
                status.errors++;
                return -1;
            }
            got += r, s->pos += r;
            status.stream_bytes[slot] += r;
        }
    }
    if (got < want) {
        memset(s->buf + got, 0, want - got);
        s->silent++;  /* 1: this half ends the data, 2: a whole half of silence queued */
    }
    for (int c = 0; c < s->channels; c++) {
        unsigned char *b = s->buf + c * s->chunk;
        for (int i = 0; i < s->chunk; i += 16)
            b[i + 1] = h == 0 && i == 0 ? 6 : h == 1 && i + 16 >= s->chunk ? 3 : 2;
        sd_lock();
        sceSdVoiceTrans(STREAM_DMA, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, b,
                        (u32 *)(s->spu_buf + (c * 2 + h) * s->chunk), s->chunk);
        sceSdVoiceTransStatus(STREAM_DMA, 1);
        sd_unlock();
    }
    return 0;
}

/* One step of the bank load: open it when new, then SND_LOAD_STEP bytes into SPU2 RAM */
static unsigned int load_t0;  /* when the queued load first found the file table full (0: it did not) */
static void load_step(void)
{
    if (load_new) {
        if (load_fd >= 0)
            df_close(load_fd);
        load_fd = df_open(load.path);
        load_pos = 0;
        if (load_fd == -EMFILE_ && (!load_t0 || now_ms() - load_t0 < OPEN_RETRY_MS)) {
            load_t0 = load_t0 ? load_t0 : now_ms() | 1;
            load_fd = -1;
            return;  /* no handle free: again next pass */
        }
        load_new = 0, load_t0 = 0;
        if (load_fd < 0 || df_lseek(load_fd, load.offset) < 0) {
            printf("dsrsnd: load %s failed (%d)\n", load.path, load_fd);
            if (load_fd >= 0)
                df_close(load_fd);
            load_fd = -1;
            status.errors++;
            status.load_failed = load.id;
            return;
        }
    }
    if (load_fd < 0)
        return;
    unsigned char *b = stream_mem + SND_STREAMS * 2 * SND_STREAM_CHUNK_MAX;
    unsigned int n = load.size - load_pos > SND_LOAD_STEP ? SND_LOAD_STEP : load.size - load_pos;
    int r = df_read(load_fd, b, n);
    if (r != (int)n) {
        df_close(load_fd);
        load_fd = -1;
        status.errors++;
        status.load_failed = load.id;
        return;
    }
    sd_lock();
    sceSdVoiceTrans(STREAM_DMA, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, b, (u32 *)(load.spu_addr + load_pos), (n + 63) & ~63);
    sceSdVoiceTransStatus(STREAM_DMA, 1);
    sd_unlock();
    load_pos += n;
    if (load_pos >= load.size) {
        df_close(load_fd);
        load_fd = -1;
        status.load_done = load.id;
    }
}

/* ---- disc server (dsrsnd_rpc.h SNDF_DISC_*) ----
 * The pattern of the PS2 games that stream audio from the disc while they load (Jak & Daxter's OVERLORD, the 989snd
 * "stream-safe" reads of Ratchet & Clank and Sly Cooper): one IOP thread owns the drive, the EE only queues requests
 * and polls for their end, audio is served before any game read, and a long read is cut into pages so a refill never
 * waits for more than one. Here the EE's requests sit in slots (the slot the EE chose), the most urgent and then the
 * oldest is served one page per pass of the stream thread, read into one of DISC_BUFS IOP buffers and sent to the EE
 * by SIF DMA while the next page is read. SIF DMA moves whole 16-byte qwords: the bytes of dst outside them travel in
 * the request's SndDiscDone (edge), which goes last, on the same DMA channel, after the data it reports. */
#define DISC_BUFS 3
#define DISC_WAIT_LOG_MS 100  /* a gap between stream passes longer than this is logged (the plan's one-page bound) */
typedef struct {
    SndDiscReq r;
    unsigned int done, order;  /* bytes sent; submission order (oldest first within a priority) */
    unsigned int t0;           /* submitted (ms), for dstat.lat_* */
    volatile int live, cancel;
} DiscJob;
static DiscJob dq[SND_DISC_SLOTS];
static unsigned int dq_order;
static int dq_sema = -1;  /* dq[].live and .order between the RPC thread and the stream thread */
static unsigned int ee_done;  /* EE physical address of its SndDiscDone ring, 0 before SNDF_DISC_INIT */
static SndDiscDone done_rec[SND_DISC_SLOTS] __attribute__((aligned(64)));
/* DISC_BUF bytes each: a page lands DISC_SLACK bytes in (at the EE address mod 16), so the sector read around it
 * (df_pread_page) has a sector of room on both sides */
#define DISC_SLACK 2048
#define DISC_BUF (DISC_SLACK + SND_DISC_PAGE + 2048 + 32)
static unsigned char *dbuf[DISC_BUFS];
static int dbuf_dma[DISC_BUFS], dbuf_n, dbuf_next;
static int dma_send(void *src, unsigned int ee, unsigned int size)
{
    SifDmaTransfer_t t = {.src = src, .dest = (void *)(ee & 0x1fffffff), .size = size, .attr = 0};
    int st, id;
    for (;;) {
        CpuSuspendIntr(&st);
        id = sceSifSetDma(&t, 1);
        CpuResumeIntr(st);
        if (id)
            return id;
        DelayThread(100);  /* the SIF DMA queue is full */
    }
}

/* the disc thread takes the drive: after the stream thread, if it asked */
static void cd_take(void)
{
    while (cd_want)
        DelayThread(1000);
    WaitSema(cd_sema);
}

static int disc_work(void)
{
    for (int i = 0; i < SND_DISC_SLOTS; i++)
        if (dq[i].live)
            return 1;
    return 0;
}

/* the request in j ends with result res: its SndDiscDone goes to the EE behind its data */
static void disc_finish(DiscJob *j, int res)
{
    int k = j->r.slot;
    done_rec[k].result = res;
    done_rec[k].seq = j->r.seq;
    /* the slot is free before its record goes: the RPC thread runs above this one, and the EE reuses the slot as soon
     * as it sees the record (a request into a slot still live here was dropped, a hang at boot) */
    WaitSema(dq_sema);
    j->live = 0;
    SignalSema(dq_sema);
    dstat.reqs++;
    if (j->r.op == SNDD_READ) {
        unsigned int w = now_ms() - j->t0;
        dstat.lat_ms += w, dstat.lat_n++;
        dstat.lat_max = w > dstat.lat_max ? w : dstat.lat_max;
    }
    if (ee_done)
        dma_send(&done_rec[k], ee_done + k * sizeof(SndDiscDone), sizeof(SndDiscDone));
}

/* ---- the IOP page cache (budget doc proposal 4; SCEE's IOP streaming talk) ----
 * The IOP has ~1 MB that nothing used (997 KB free after dsrsnd, 2026-11-08). PC_PAGES 32 KB pages of it hold the last
 * ECC blocks (16 sectors, LSN % 16 == 0: the drive's smallest read) that background requests (map streaming) read, LRU.
 * A page read again (the route walks back and forth over block seams; cancelled reads are asked for again) costs no
 * seek and no drive time. Pages are read-only disc data, so the EE's and the cache's views never differ. Sync loads (whole
 * packs at boot / warps) go past it: they would flush it with data nobody reads twice. At least PC_KEEP_FREE of IOP RAM
 * stays free for MCMAN / MCSERV (~250 KB at their peak while loading). DSRSND_NO_CACHE builds go without it. */
#define PC_SECS 16
#define PC_PAGE (PC_SECS * 2048)
#define PC_PAGES 16  /* 512 KB */
#define PC_KEEP_FREE (256 * 1024)
static unsigned char *pc_mem;
static unsigned int pc_lsn[PC_PAGES], pc_use[PC_PAGES], pc_clock;
static int pc_dma[PC_PAGES];  /* SIF DMA in flight straight from the page (pc_read direct): not reused until done */
static int pc_n, pc_last;     /* pages; the page pc_block returned last */

/* the page holding the ECC block from lsn (16-aligned), read on a miss; NULL on a read error */
static const unsigned char *pc_block(unsigned int lsn)
{
    int v = 0;
    for (int i = 0; i < pc_n; i++) {
        if (pc_use[i] && pc_lsn[i] == lsn) {
            pc_use[i] = ++pc_clock;
            dstat.cache_hits++;
            pc_last = i;
            return pc_mem + i * PC_PAGE;
        }
        if (pc_use[i] < pc_use[v])
            v = i;
    }
    /* the block ends at the layer break (layer 1 starts with the drive's reserved sectors and is a seek away) and at the
     * end of layer 0's volume (the ISO builder pads every layer to whole blocks) */
    unsigned int n = PC_SECS, end = dstat.layer1 && lsn < dstat.layer1 ? dstat.layer1 : lsn < df_volume ? df_volume : 0;
    if (end && lsn + n > end)
        n = end - lsn;
    while (pc_dma[v] && sceSifDmaStat(pc_dma[v]) >= 0)  /* the EE is still being sent bytes of it */
        DelayThread(100);
    pc_dma[v] = 0;
    pc_use[v] = 0;
    if (cd_sectors(lsn, n, pc_mem + v * PC_PAGE) < 0)
        return NULL;
    pc_lsn[v] = lsn, pc_use[v] = ++pc_clock;
    dstat.cache_miss++;
    pc_last = v;
    return pc_mem + v * PC_PAGE;
}

/* n bytes of file fd from off into p through the cache (the caller holds the drive): -1 on a read error. When they lie
 * in one block at the same address mod 16 as p, *direct is set to them in the page and nothing is copied (the page is
 * then sent to the EE straight from the cache: pc_dma); else *direct is NULL and p has them. A 32 KB copy costs the
 * IOP ~1 ms: with every page copied the boot settle took ~1.8 s longer (PCSX2 smoke, 2026-11-08). */
static int pc_read(int fd, unsigned int off, unsigned char *p, unsigned int n, unsigned char **direct)
{
    const DfFile *f = &df_tab[df_h[fd - DF_FD].file];
    *direct = NULL;
    if (off >= f->size)
        return 0;
    if (n > f->size - off)
        n = f->size - off;
    unsigned int left = n;
    while (left) {
        unsigned int lsn = df_h[fd - DF_FD].lsn + off / 2048, blk = lsn & ~(PC_SECS - 1u);
        unsigned int in = (lsn - blk) * 2048 + (off & 2047), c = PC_PAGE - in;
        if (c > left)
            c = left;
        const unsigned char *pg = pc_block(blk);
        if (!pg)
            return left == n ? -1 : (int)(n - left);
        if (c == n && !(((unsigned int)(pg + in) ^ (unsigned int)p) & 15)) {
            *direct = (unsigned char *)pg + in;
            dstat.cache_direct++;
            return n;
        }
        memcpy(p, pg + in, c);
        p += c, off += c, left -= c;
    }
    return n;
}

/* One page of the read in j. Page ends are 16-byte aligned on the EE (the first page is shorter when dst is not), so
 * only the first page has head bytes and only the last a tail. The page lands at the same address mod 16 in the IOP
 * buffer as on the EE, so the qwords it DMAs are aligned on both sides. A background read of a disc file goes through
 * the page cache with pages that end at an ECC block (LSN % 16), less the bytes that keep the EE end aligned (they open
 * the next page, from the cached block). */
static void disc_page(DiscJob *j)
{
    SndDiscReq *r = &j->r;
    SndDiscDone *d = &done_rec[r->slot];
    if (j->cancel || j->done >= r->size) {
        disc_finish(j, j->cancel ? -1 : (int)j->done);
        return;
    }
    unsigned int ee = r->dst + j->done, n = r->size - j->done;
    int cached = pc_n && r->prio >= SNDD_PRIO_BG && df_is(r->fd);
    if (cached) {
        unsigned int pos = r->offset + j->done, lsn = df_h[r->fd - DF_FD].lsn + pos / 2048;
        unsigned int to_blk = ((lsn | (PC_SECS - 1u)) + 1 - lsn) * 2048 - (pos & 2047);  /* bytes to the block's end */
        if (n > to_blk) {
            unsigned int m = to_blk - ((ee + to_blk) & 15);  /* the EE end on a qword: the rest opens the next page */
            if (!m)
                m = to_blk + PC_PAGE - ((ee + to_blk + PC_PAGE) & 15);
            n = m < n ? m : n;
        }
    } else if (n > SND_DISC_PAGE - (ee & 15))
        n = SND_DISC_PAGE - (ee & 15);
    int b = dbuf_next;
    dbuf_next = (b + 1) % dbuf_n;
    while (dbuf_dma[b] && sceSifDmaStat(dbuf_dma[b]) >= 0)  /* its last page still on its way */
        DelayThread(100);
    dbuf_dma[b] = 0;
    unsigned char *p = dbuf[b] + DISC_SLACK + (ee & 15);
    cd_take();
    unsigned char *direct = NULL;
    int got = cached ? pc_read(r->fd, r->offset + j->done, p, n, &direct)
                     : df_pread_page(r->fd, r->offset + j->done, p, n, dbuf[b]);
    int pc_slot = pc_last;
    SignalSema(cd_sema);
    if (direct)
        p = direct;
    if (got <= 0) {
        disc_finish(j, j->done ? (int)j->done : got < 0 ? got : 0);
        return;
    }
    unsigned int a = (ee + 15) & ~15u, e = (ee + got) & ~15u;
    if (!j->done) {
        unsigned int h = (a < ee + got ? a : ee + got) - ee;
        memcpy(d->edge, p, h);
        d->nhead = h, d->ntail = 0;
    }
    if (a < e) {
        int id = dma_send(p + (a - ee), a, e - a);
        if (direct)
            pc_dma[pc_slot] = id;
        else
            dbuf_dma[b] = id;
    }
    if (a <= e && ee + got > e) {  /* the tail: the last page (or a short read) */
        d->ntail = ee + got - e;
        memcpy(d->edge + d->nhead, p + (e - ee), d->ntail);
    }
    j->done += got;
    dstat.pages++, dstat.kb += got >> 10;
    if (got < (int)n || j->done >= r->size)
        disc_finish(j, j->done);
}

/* the disc sector job j reads next (an open, a close or a host: read: where the head is, so it goes now) */
static unsigned int job_lsn(const DiscJob *j)
{
    if (j->r.op != SNDD_READ || !df_is(j->r.fd))
        return cd_head;
    return df_h[j->r.fd - DF_FD].lsn + (j->r.offset + j->done) / 2048;
}

/* The next request: the most urgent priority first; within it the one whose next sector is the first at or after the
 * head, else the lowest (one sweep across the disc, then back: the request queue "sorted by LSN" of Sony's streaming
 * talks and the next-file-on-CD rule of console streamers, so queued map reads cost one pass over the disc instead
 * of a seek back and forth per read in submission order). A read in progress stays first: its next page is at the
 * head. Ties go to the oldest. */
/* The map queues (SNDD_PRIO_HIT and below) age: a request waiting DISC_AGE_MS counts one queue more urgent, up to
 * SNDD_PRIO_HIT. DSR serves its queues strictly in order, but its drive is fast; here the first A/B of the queue order
 * (ISO, 2026-11-10) left reads waiting up to 14 s while the urgent queues stayed busy (ASSUMED value: about one
 * re-plan of the map streamer). */
#define DISC_AGE_MS 1500u
static unsigned int job_prio(const DiscJob *c, unsigned int now)
{
    unsigned int p = c->r.prio;
    if (p > SNDD_PRIO_HIT) {
        unsigned int up = (now - c->t0) / DISC_AGE_MS;
        p = up >= p - SNDD_PRIO_HIT ? SNDD_PRIO_HIT : p - up;
    }
    return p;
}

static void disc_step(void)
{
    DiscJob *j = NULL;
    unsigned int jd = 0, jp = 0, now = now_ms();
    WaitSema(dq_sema);
    for (int i = 0; i < SND_DISC_SLOTS; i++) {
        DiscJob *c = &dq[i];
        if (!c->live)
            continue;
        /* sectors ahead of the head, wrapping for those behind it; from the last sector read, which a read in
         * progress continues in when its page ended inside a sector */
        unsigned int d = job_lsn(c) - (cd_head - 1), cp = job_prio(c, now);
        if (!j || cp < jp || (cp == jp && (d < jd || (d == jd && (int)(c->order - j->order) < 0))))
            j = c, jd = d, jp = cp;
    }
    SignalSema(dq_sema);
    if (!j)
        return;
    switch (j->r.op) {
    case SNDD_OPEN: {
        char path[sizeof j->r.path];
        strncpy(path, j->r.path, sizeof path - 1);
        path[sizeof path - 1] = 0;
        cd_take();
        int fd = df_open(path);
        SignalSema(cd_sema);
        if (fd < 0)
            printf("dsrsnd: disc open %s failed (%d)\n", path, fd);
        disc_finish(j, fd);
        break;
    }
    case SNDD_CLOSE: {
        cd_take();
        int r = df_close(j->r.fd);
        SignalSema(cd_sema);
        disc_finish(j, r);
        break;
    }
    case SNDD_READ:
        disc_page(j);
        break;
    default:
        disc_finish(j, -1);
        break;
    }
}

/* SNDF_DISC_SUBMIT (RPC thread): into their slots; a cancel only marks its target */
static void disc_submit(const SndDiscReq *q, int n)
{
    WaitSema(dq_sema);
    for (int i = 0; i < n; i++) {
        const SndDiscReq *r = &q[i];
        if (r->slot >= SND_DISC_SLOTS)
            continue;
        DiscJob *j = &dq[r->slot];
        if (r->op == SNDD_CANCEL) {
            if (j->live && j->r.seq == r->seq)
                j->cancel = 1;
            continue;
        }
        if (j->live) {
            printf("dsrsnd: disc slot %u busy (seq %u, new %u)\n", r->slot, j->r.seq, r->seq);
            continue;
        }
        memcpy(&j->r, r, sizeof *r);
        j->done = 0, j->cancel = 0, j->order = dq_order++, j->t0 = now_ms();
        j->live = 1;
    }
    SignalSema(dq_sema);
    dstat.iop_free = QueryTotalFreeMemSize();
}

/* how long the stream thread waited for the drive (the disc thread's page in progress), while a stream played */
static void cd_wait_note(unsigned int w)
{
    int play = 0;
    for (int i = 0; i < SND_STREAMS; i++)
        play |= streams[i].state == SNDS_PLAYING || streams[i].state == SNDS_ENDING;
    if (!play)
        return;
    if (w > dstat.wait_max_ms)
        dstat.wait_max_ms = w;
    if (w > DISC_WAIT_LOG_MS) {
        dstat.waits_100++;
        printf("dsrsnd: streams waited %u ms for the drive\n", w);
    }
}

/* the disc thread: below every other thread, so its waits in the CD driver never hold up an RPC (fileio's printf
 * included: a busy stream thread delayed each EE printf by up to a page, ISO 2026-11-05) */
static void disc_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (!disc_work())
            DelayThread(POLL_US);
        disc_step();
    }
}

static void stream_start(Stream *s);

/* the drive for this pass of the stream thread, taken on first use: 1 held, -1 the disc thread reads a page (it
 * leaves the drive after it: cd_want), 0 not asked yet */
static int disc_take(int *d)
{
    if (!*d) {
        if (PollSema(cd_sema) < 0) {
            if (!cd_want)
                cd_want_ms = now_ms();
            cd_want = 1, *d = -1;
        } else {
            if (cd_want)
                cd_wait_note(now_ms() - cd_want_ms);
            cd_want = 0, *d = 1;
            /* below the RPC threads while this thread reads: a cdrom0: read keeps its thread busy until the data is
             * in, and at priority 30 that held the EE's frame RPCs back for the whole read (frames of 200-650 ms
             * whenever a top-up ran, ISO world_route 2026-11-05) */
            ChangeThreadPriority(TH_SELF, STREAM_PRIO_DISC);
        }
    }
    return *d > 0;
}

/* Key a starved stream's voices on again: both ring halves from where its data stopped (nothing skipped, nothing
 * replayed). 1 once it plays, 0 while the read-ahead is still short. */
static int stream_resume(Stream *s, int slot, unsigned int now)
{
    if (s->ra && !s->silent) {  /* both halves in the read-ahead, or the first would be taken and the second wait */
        unsigned int need = 2 * s->chunk * s->channels;
        if (!s->loop && need > s->size - s->pos)
            need = s->size - s->pos;
        if (s->ra_len < need)
            return 0;
    }
    int r = stream_fill(s, slot, 0);
    if (r == 0)
        r = stream_fill(s, slot, 1);
    if (r)
        return r < 0 ? r : 0;
    int v = SND_STREAM_VOICE(slot);
    sd_lock();
    for (int c = 0; c < 2; c++)
        sceSdSetAddr(vreg(v + c) | SD_VADDR_SSA, s->spu_buf + (c < s->channels ? c : 0) * 2 * s->chunk);
    sceSdSetSwitch(1 | SD_SWITCH_KON, (s->channels == 2 ? 3 : 1) << (v % 24));
    sd_unlock();
    printf("dsrsnd: stream %d resumed after %u ms\n", slot, now - s->last_fill);
    s->starved = s->due = 0, s->next_fill = 0, s->last_fill = now;
    if (s->silent)
        s->state = SNDS_ENDING;
    return 1;
}

static void take_requests(void);

static void stream_thread(void *arg)
{
    (void)arg;
    for (;;) {
        DelayThread(POLL_US);
        WaitSema(stream_sema);
        take_requests();
        unsigned int now = now_ms(), starving = 0;
        int disc = 0;
        for (int i = 0; i < SND_STREAMS; i++) {
            Stream *s = &streams[i];
            if (s->close_fd >= 0 && disc_take(&disc))
                df_close(s->close_fd), s->close_fd = -1;
            if (s->state == SNDS_OPENING && disc_take(&disc))
                stream_start(s);
            if (s->state != SNDS_PLAYING && s->state != SNDS_ENDING)
                continue;
            int r = 0;
            if (s->starved) {
                if (!s->ra && !disc_take(&disc))
                    r = 0;
                else
                    r = stream_resume(s, i, now);
            } else {
                sd_lock();
                unsigned int nax = sceSdGetAddr(vreg(SND_STREAM_VOICE(i)) | SD_VADDR_NAX);
                sd_unlock();
                int cur = nax >= s->spu_buf + s->chunk;  /* half the left voice is playing */
                /* last_fill may be later than this pass's start (a stream started in it): no gap then */
                unsigned int gap = (int)(now - s->last_fill) > 0 ? now - s->last_fill : 0;
                /* Underrun: the voice went back into a half still waiting for its refill (it would replay stale audio),
                 * or more than two halves passed since the last refill (a wrap this thread did not see). Mute it and
                 * key it on again once the read-ahead holds two halves. */
                if (!s->silent && ((s->due && cur == s->next_fill) || gap > 2 * s->half_ms)) {
                    int v = SND_STREAM_VOICE(i);
                    sd_lock();
                    sceSdSetSwitch(1 | SD_SWITCH_KOFF, 3 << (v % 24));
                    sd_unlock();
                    s->starved = 1, s->due = 0;
                    status.underruns++;
                    printf("dsrsnd: stream %d refill gap %u ms (half %u ms) UNDERRUN, muted\n", i, gap, s->half_ms);
                    starving |= 1u << i;
                    continue;
                }
                if (cur == s->next_fill)
                    r = 0;
                else if (s->silent >= 2) {  /* the voice has reached the silence after the data */
                    stream_close(s, i);
                    if (disc_take(&disc))
                        df_close(s->close_fd), s->close_fd = -1;
                    continue;
                } else if (!s->ra && !disc_take(&disc))
                    s->due = 1;
                else if ((r = stream_fill(s, i, s->next_fill)) == 1)
                    s->due = 1, r = 0;  /* the read-ahead runs short: the top-up below, then the next pass */
                else if (r == 0) {
                    if (gap > s->half_ms * 3 / 2) {
                        status.late++;
                        printf("dsrsnd: stream %d refill gap %u ms (half %u ms)\n", i, gap, s->half_ms);
                    }
                    s->due = 0, s->last_fill = now;
                    if (s->silent)
                        s->state = SNDS_ENDING;
                    s->next_fill ^= 1;
                }
            }
            if (r < 0) {
                stream_close(s, i);
                if (disc_take(&disc))
                    df_close(s->close_fd), s->close_fd = -1;
                s->state = SNDS_ERROR;
                continue;
            }
        }
        /* top the read-ahead up once one is down to half: the drive only now (after the disc thread's page in progress).
         * With the drive, every stream below STREAM_RA_ALSO tops up too: one trip of the drive for all of them, each one a
         * seek away from the EE's reads and back */
        int top = 0;
        for (int i = 0; i < SND_STREAMS; i++) {
            Stream *s = &streams[i];
            top |= stream_ra_want(s) && s->ra_len <= STREAM_RA_TOP;
        }
        for (int i = 0; i < SND_STREAMS && top && disc_take(&disc); i++) {
            Stream *s = &streams[i];
            if (stream_ra_want(s) && s->ra_len <= STREAM_RA_ALSO && stream_ra_fill(s) < 0) {
                status.errors++;
                stream_close(s, i);
                df_close(s->close_fd), s->close_fd = -1;
                s->state = SNDS_ERROR;
            }
        }
        for (int i = 0; i < SND_STREAMS; i++) {
            Stream *s = &streams[i];
            if (s->state == SNDS_PLAYING && (s->starved || s->due || (stream_ra_want(s) && s->ra_len < STREAM_RA_LOW)))
                starving |= 1u << i;
        }
        for (int i = 0; i < SND_STREAMS; i++)
            status.stream_state[i] = streams[i].state;
        status.starving = starving;
        if ((load_new || load_fd >= 0) && disc_take(&disc))
            load_step();  /* after the refills: a stream never waits for a bank */
        if (disc > 0) {
            ChangeThreadPriority(TH_SELF, STREAM_PRIO);  /* back up before the drive goes to the disc thread */
            SignalSema(cd_sema);
        }
        SignalSema(stream_sema);
    }
}

/* SNDF_STREAM: queue the open for the stream thread (it needs the drive) */
static void stream_open(const SndStreamOpen *o)
{
    if (o->slot >= SND_STREAMS || o->channels < 1 || o->channels > 2 || o->chunk > SND_STREAM_CHUNK_MAX ||
        (o->chunk & 63) || !o->chunk)
        return;
    WaitSema(req_sema);
    memcpy(&open_req[o->slot], o, sizeof *o);
    open_pend[o->slot] = 1, stop_pend[o->slot] = 0;
    SignalSema(req_sema);
}

/* the stream thread, holding stream_sema, at the start of a pass: the opens, stops and loads the RPC threads posted */
static void take_requests(void)
{
    WaitSema(req_sema);
    for (int i = 0; i < SND_STREAMS; i++) {
        Stream *s = &streams[i];
        if ((stop_pend[i] || open_pend[i]) && s->state != SNDS_IDLE && s->state != SNDS_ERROR &&
            s->state != SNDS_OPENING)
            stream_close(s, i);
        if (stop_pend[i])
            s->state = SNDS_IDLE, stop_pend[i] = 0;
        if (open_pend[i]) {
            memcpy(&s->req, &open_req[i], sizeof s->req);
            s->state = SNDS_OPENING, s->open_t0 = 0, open_pend[i] = 0;
        }
    }
    if (load_pend)
        memcpy(&load, &load_req, sizeof load), load_new = 1, load_t0 = 0, load_pend = 0;
    SignalSema(req_sema);
}

/* the stream thread, holding stream_sema and the drive: open and fill a queued stream, key its voices on */
static void stream_start(Stream *s)
{
    const SndStreamOpen *o = &s->req;
    char path[sizeof o->path];
    strncpy(path, o->path, sizeof path - 1);
    path[sizeof path - 1] = 0;
    s->fd = df_open(path);
    if (s->fd == -EMFILE_ && (!s->open_t0 || now_ms() - s->open_t0 < OPEN_RETRY_MS)) {
        s->open_t0 = s->open_t0 ? s->open_t0 : now_ms() | 1;
        s->fd = -1;
        return;  /* no handle free: stays SNDS_OPENING, again next pass */
    }
    s->open_t0 = 0;
    if (s->fd < 0) {
        printf("dsrsnd: open %s failed (%d)\n", path, s->fd);
        status.errors++;
        s->state = SNDS_ERROR;
        return;
    }
    s->offset = o->offset, s->size = o->size, s->loop = o->loop, s->chunk = o->chunk, s->channels = o->channels;
    s->spu_buf = o->spu_buf, s->pos = 0, s->silent = 0, s->next_fill = 0, s->ra_pos = s->ra_len = s->rd_pos = 0;
    s->loop = s->loop && s->size;  /* an empty loop would never end a read */
    s->due = s->starved = 0;
    s->half_ms = (unsigned int)s->chunk / 16 * 28 * 4096 / (48u * (o->pitch ? o->pitch : 4096));
    s->buf = stream_mem + o->slot * 2 * SND_STREAM_CHUNK_MAX;
    status.stream_bytes[o->slot] = 0;
    df_lseek(s->fd, s->offset);
    int bad = s->ra && stream_ra_fill(s) < 0;
    if (bad)
        status.errors++;
    if (bad || stream_fill(s, o->slot, 0) || stream_fill(s, o->slot, 1)) {  /* the read-ahead holds both halves */
        stream_close(s, o->slot);
        s->state = SNDS_ERROR;
        return;
    }
    int v = SND_STREAM_VOICE(o->slot);
    sd_lock();
    for (int c = 0; c < 2; c++) {
        int r = vreg(v + c), ch = c < o->channels ? c : 0;
        unsigned short l = o->channels == 1 || c == 0 ? o->voll : 0, rr = o->channels == 1 || c == 1 ? o->volr : 0;
        if (c >= o->channels && o->channels == 1)
            l = rr = 0;  /* mono: the second voice stays silent */
        sceSdSetParam(r | SD_VPARAM_VOLL, l);
        sceSdSetParam(r | SD_VPARAM_VOLR, rr);
        sceSdSetParam(r | SD_VPARAM_PITCH, o->pitch);
        sceSdSetParam(r | SD_VPARAM_ADSR1, ADSR1_FLAT);
        sceSdSetParam(r | SD_VPARAM_ADSR2, ADSR2_FLAT(8));
        sceSdSetAddr(r | SD_VADDR_SSA, s->spu_buf + ch * 2 * s->chunk);
    }
    sceSdSetSwitch(1 | SD_SWITCH_KON, (o->channels == 2 ? 3 : 1) << (v % 24));
    sd_unlock();
    s->state = s->silent ? SNDS_ENDING : SNDS_PLAYING;
    s->last_fill = now_ms();
    status.stream_state[o->slot] = s->state;
}

static void stream_volume(int slot, int l, int r)
{
    if (slot >= SND_STREAMS)
        return;
    Stream *s = &streams[slot];
    int v = SND_STREAM_VOICE(slot);
    sd_lock();
    if (s->channels == 2) {
        sceSdSetParam(vreg(v) | SD_VPARAM_VOLL, l);
        sceSdSetParam(vreg(v + 1) | SD_VPARAM_VOLR, r);
    } else {
        sceSdSetParam(vreg(v) | SD_VPARAM_VOLL, l);
        sceSdSetParam(vreg(v) | SD_VPARAM_VOLR, r);
    }
    sd_unlock();
}

/* ---- voice commands ---- */

static void frame(const SndFrame *f)
{
    unsigned int kon[2] = {0, 0}, koff[2] = {0, 0};
    unsigned int n = f->ncmds < SND_CMD_MAX ? f->ncmds : SND_CMD_MAX;
    for (unsigned int i = 0; i < n; i++) {
        const SndCmd *c = &f->cmd[i];
        if (c->op == SNDC_KEYOFF && c->voice < SND_VOICES)
            koff[vcore(c->voice)] |= 1u << (c->voice % 24);
    }
    sd_lock();
    for (unsigned int i = 0; i < n; i++) {
        const SndCmd *c = &f->cmd[i];
        int r = vreg(c->voice);
        switch (c->op) {
        case SNDC_KEYON:
            if (c->voice >= SND_VOICES)
                break;
            sceSdSetParam(r | SD_VPARAM_VOLL, c->voll);
            sceSdSetParam(r | SD_VPARAM_VOLR, c->volr);
            sceSdSetParam(r | SD_VPARAM_PITCH, c->pitch);
            sceSdSetParam(r | SD_VPARAM_ADSR1, ADSR1_FLAT);
            sceSdSetParam(r | SD_VPARAM_ADSR2, ADSR2_FLAT(c->adsr2 & 0x1f));
            sceSdSetAddr(r | SD_VADDR_SSA, c->addr);
            kon[vcore(c->voice)] |= 1u << (c->voice % 24);
            koff[vcore(c->voice)] &= ~(1u << (c->voice % 24));
            break;
        case SNDC_VOL:
            if (c->voice < SND_VOICES) {
                sceSdSetParam(r | SD_VPARAM_VOLL, c->voll);
                sceSdSetParam(r | SD_VPARAM_VOLR, c->volr);
            }
            break;
        case SNDC_PITCH:
            if (c->voice < SND_VOICES)
                sceSdSetParam(r | SD_VPARAM_PITCH, c->pitch);
            break;
        case SNDC_MASTER:
            for (int k = 0; k < 2; k++) {
                sceSdSetParam(k | SD_PARAM_MVOLL, c->voll);
                sceSdSetParam(k | SD_PARAM_MVOLR, c->volr);
            }
            break;
        default:
            break;
        }
    }
    for (int k = 0; k < 2; k++)
        if (koff[k])
            sceSdSetSwitch(k | SD_SWITCH_KOFF, koff[k]);
    for (int k = 0; k < 2; k++)
        if (kon[k])
            sceSdSetSwitch(k | SD_SWITCH_KON, kon[k]);
    status.endx[0] = sceSdGetSwitch(0 | SD_SWITCH_ENDX);
    status.endx[1] = sceSdGetSwitch(1 | SD_SWITCH_ENDX);
    sd_unlock();
    /* stream volume / stop: after the voice registers (their own locks) */
    for (unsigned int i = 0; i < n; i++) {
        const SndCmd *c = &f->cmd[i];
        if (c->op == SNDC_STREAM_VOL)
            stream_volume(c->voice, c->voll, c->volr);
        else if (c->op == SNDC_STREAM_STOP && c->voice < SND_STREAMS) {
            int v = SND_STREAM_VOICE(c->voice);  /* silent at once; the stream thread closes it on its next pass */
            sd_lock();
            sceSdSetSwitch(1 | SD_SWITCH_KOFF, 3 << (v % 24));
            sd_unlock();
            WaitSema(req_sema);
            stop_pend[c->voice] = 1, open_pend[c->voice] = 0;
            SignalSema(req_sema);
            status.stream_state[c->voice] = SNDS_IDLE;
        }
    }
}

static void *rpc_server(int fno, void *data, int size)
{
    switch (fno) {
    case SNDF_INIT:
        sd_lock();
        init_spu();
        sd_unlock();
        break;
    case SNDF_LOAD:
        WaitSema(req_sema);
        memcpy(&load_req, data, sizeof load_req);
        load_req.path[sizeof load_req.path - 1] = 0;
        load_pend = 1;
        SignalSema(req_sema);
        break;
    case SNDF_FRAME:
        frame(data);
        break;
    case SNDF_STREAM:
        stream_open(data);
        break;
    case SNDF_DISC_INIT:
        ee_done = ((const SndDiscInit *)data)->done & 0x1fffffff;
        return &dstat;
    case SNDF_DISC_SUBMIT:
        disc_submit(data, size / (int)sizeof(SndDiscReq));
        return &dstat;
    default:
        break;
    }
    return &status;
}

static void rpc_thread(void *arg)
{
    int k = (int)arg;
    sceSifInitRpc(0);
    sceSifSetRpcQueue(&queue[k], GetThreadId());
    static const unsigned int ids[3] = {DSRSND_RPC_ID, DSRSND_RPC_OPS_ID, DSRSND_RPC_DISC_ID};
    sceSifRegisterRpc(&server[k], ids[k], rpc_server, rpc_buf[k], NULL, NULL, &queue[k]);
    sceSifRpcLoop(&queue[k]);
}

int _start(int argc, char *argv[])
{
    (void)argc, (void)argv;
    iop_sema_t sm = {.attr = 0, .option = 0, .initial = 1, .max = 1};
    sd_sema = CreateSema(&sm);
    stream_sema = CreateSema(&sm);
    req_sema = CreateSema(&sm);
    dq_sema = CreateSema(&sm);
    cd_sema = CreateSema(&sm);
    dstat.iop_free_boot = QueryTotalFreeMemSize();
    stream_mem = AllocSysMemory(ALLOC_FIRST, SND_STREAMS * 2 * SND_STREAM_CHUNK_MAX + SND_LOAD_STEP, NULL);
    if (sd_sema < 0 || stream_sema < 0 || req_sema < 0 || dq_sema < 0 || cd_sema < 0 || !stream_mem)
        return MODULE_NO_RESIDENT_END;
    /* the disc pages before the read-ahead: the EE's reads need at least one, the read-ahead can do without */
    while (dbuf_n < DISC_BUFS && (dbuf[dbuf_n] = AllocSysMemory(ALLOC_FIRST, DISC_BUF, NULL)))
        dbuf_n++;
    if (!dbuf_n)
        return MODULE_NO_RESIDENT_END;
    df_tab = AllocSysMemory(ALLOC_FIRST, DF_FILES * sizeof(DfFile), NULL);  /* none: cdrom0: through ioman */
    for (int h = 0; h < DF_HANDLES; h++)
        df_h[h].file = -1;
    unsigned char *ra = AllocSysMemory(ALLOC_FIRST, SND_STREAMS * STREAM_RA, NULL);
    if (!ra)
        printf("dsrsnd: no IOP RAM for stream read-ahead, reading per refill\n");
    for (int i = 0; i < SND_STREAMS; i++)
        streams[i].fd = streams[i].close_fd = -1, streams[i].ra = ra ? ra + i * STREAM_RA : NULL;
    int id;
    for (int k = 0; k < 3; k++) {  /* frame server, ops server, disc server (queues only: never waits for the drive) */
        iop_thread_t t = {.attr = TH_C, .thread = rpc_thread, .priority = 40 + k, .stacksize = 0x800, .option = 0};
        if ((id = CreateThread(&t)) < 0)
            return MODULE_NO_RESIDENT_END;
        StartThread(id, (void *)k);
    }
    /* the stream thread runs above the RPC thread (and below the CD driver) so a refill is not held up by uploads */
    iop_thread_t st = {.attr = TH_C, .thread = stream_thread, .priority = STREAM_PRIO, .stacksize = 0x800, .option = 0};
    id = CreateThread(&st);
    if (id < 0)
        return MODULE_NO_RESIDENT_END;
    StartThread(id, NULL);
    /* the disc thread below everything (IOP priorities run 1-127, lower is more urgent) */
    iop_thread_t dt = {.attr = TH_C, .thread = disc_thread, .priority = 120, .stacksize = 0x800, .option = 0};
    if ((id = CreateThread(&dt)) < 0)
        return MODULE_NO_RESIDENT_END;
    StartThread(id, NULL);
#ifndef DSRSND_NO_CACHE
    {
        unsigned int f = QueryTotalFreeMemSize(), want = f > PC_KEEP_FREE ? (f - PC_KEEP_FREE) / PC_PAGE : 0;
        for (pc_n = want < PC_PAGES ? want : PC_PAGES; pc_n && !(pc_mem = AllocSysMemory(ALLOC_FIRST, pc_n * PC_PAGE, NULL));)
            pc_n--;
        dstat.cache_kb = pc_n * (PC_PAGE >> 10);
    }
#endif
    dstat.iop_free = QueryTotalFreeMemSize();
    printf("dsrsnd: ready (%d streams, %d disc pages of %u KB, page cache %u KB); IOP RAM free %u KB before dsrsnd's "
           "buffers, %u KB after\n", SND_STREAMS, dbuf_n, SND_DISC_PAGE >> 10, dstat.cache_kb, dstat.iop_free_boot >> 10,
           dstat.iop_free >> 10);
    return MODULE_RESIDENT_END;
}
