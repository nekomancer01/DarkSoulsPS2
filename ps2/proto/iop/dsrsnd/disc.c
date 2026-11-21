/* dsrsnd.irx, disc part: the drive and everything that reads it (the module's overview: dsrsnd.c).
 * - files on the disc: a table of start sectors from the ISO9660 directory, read by sector (df_*), the media and its
 *   layers, the drive counters (dstat) and the "dsrsnd: drive" log;
 * - the EE's disc server (dsrsnd_rpc.h SNDF_DISC_*): its request slots, the IOP page cache, the disc thread;
 * - the drive lock: one reader of the drive at a time (the BIOS CD file system takes one), the disc thread or the
 *   sound part's stream thread (snd.c). The stream thread never waits for it: drive_poll asks for it (cd_want) when
 *   it is busy, and the disc thread leaves it after its current page. */
#include "irx_imports.h"
#include "dsrsnd_rpc.h"
#include "dsrsnd_i.h"

/* the drive (ioman: cdrom0:, or host:): the stream thread and the disc thread take turns, one call at a time */
static int cd_sema = -1;
static volatile int cd_want;     /* the stream thread found the drive busy: the disc thread leaves it after its page */
static unsigned int cd_want_ms;  /* when it first did */

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
SndDiscStat dstat __attribute__((aligned(64)));

/* The drive's media and layers (budget doc "Disc and read-ahead", Sony's dual-layer notes): a DVD-9 is parallel track
 * path, LSNs carry on across the break, layer 0 reads in its own spin mode (SCECdSpinDvdDL0, constant ~2.2 MB/s) and
 * layer 1 in the normal one. The fast seek window (seeks inside it move the lens only) is ~+/-4371 sectors on CD,
 * +/-14764 on DVD-5 and +/-13360 on DVD-9. On a real dual-layer disc dstat.layer1 is the drive's own break, else the one
 * the ISO builder planned (df_layout), counted but not switched. */
static int cd_layer = -1, cd_dual;  /* layer of the last read (-1 none yet); 1: a real dual-layer disc */
static unsigned int cd_window = 4371;

/* Who uses the drive (the "dsrsnd: drive" line every DRIVE_LOG_MS): reads, seeks, far seeks, KB and ms by reader (the
 * EE's requests / stream read-ahead / bank loads), and the EE reads that had to seek back right after one of the
 * other two (the return trip a stream top-up costs the map reader). */
#define DRIVE_LOG_MS 10000u
#define DISC_AGE_MS 1000u  /* ASSUMED: about the look-ahead of the map reader */
static unsigned int dstat_aged;  /* requests served out of sweep order for their age */
int cd_who;
static int cd_last_who;  /* 0 EE request, 1 stream read-ahead, 2 bank load */
static struct { unsigned int reads, seeks, far, kb, ms; } who_st[4];  /* [3]: EE reads right after another reader */
static unsigned int who_t0;
/* far seeks by the files they went between (from -> to, indices into df_tab; -1: outside every file) */
#define FAR_PAIRS 12
static struct { short a, b; unsigned short n, who; } far_tab[FAR_PAIRS];
static int df_file_at(unsigned int lsn)
{
    for (int i = 0; i < df_n; i++)
        if (lsn >= df_tab[i].lsn && lsn < df_tab[i].lsn + (df_tab[i].size + 2047) / 2048 + 1)
            return i;
    return -1;
}
static void far_note(unsigned int from, unsigned int to)
{
    int a = df_file_at(from), b = df_file_at(to), free_i = -1;
    for (int i = 0; i < FAR_PAIRS; i++) {
        if (far_tab[i].n && far_tab[i].a == a && far_tab[i].b == b) {
            far_tab[i].n++;
            return;
        }
        if (!far_tab[i].n && free_i < 0)
            free_i = i;
    }
    if (free_i >= 0)
        far_tab[free_i].a = a, far_tab[free_i].b = b, far_tab[free_i].n = 1, far_tab[free_i].who = cd_who;
}
void who_log(void)
{
    unsigned int now = now_ms();
    if (now - who_t0 < DRIVE_LOG_MS)
        return;
    who_t0 = now;
    if (who_st[0].reads + who_st[1].reads + who_st[2].reads)
        printf("dsrsnd: drive 10 s: ee %u reads %u seeks %u far %u KB %u ms | streams %u reads %u seeks %u far %u KB %u ms | "
               "banks %u reads %u seeks %u far %u KB %u ms | ee back from them %u seeks %u far %u ms | aged %u\n",
               who_st[0].reads, who_st[0].seeks, who_st[0].far, who_st[0].kb, who_st[0].ms,
               who_st[1].reads, who_st[1].seeks, who_st[1].far, who_st[1].kb, who_st[1].ms,
               who_st[2].reads, who_st[2].seeks, who_st[2].far, who_st[2].kb, who_st[2].ms,
               who_st[3].seeks, who_st[3].far, who_st[3].ms, dstat_aged);
    dstat_aged = 0;
    for (int i = 0; i < FAR_PAIRS; i++)
        if (far_tab[i].n > 1)
            printf("dsrsnd: far seeks %u x %s -> %s (%s)\n", far_tab[i].n, far_tab[i].a < 0 ? "?" : df_tab[far_tab[i].a].name,
                   far_tab[i].b < 0 ? "?" : df_tab[far_tab[i].b].name, far_tab[i].who == 1 ? "stream" : far_tab[i].who ? "bank" : "ee");
    memset(far_tab, 0, sizeof far_tab);
    memset(who_st, 0, sizeof who_st);
}

static int cd_read1(unsigned int lsn, unsigned int n, void *buf)
{
    int w_seek = 0, w_far = 0;
    int l = dstat.layer1 && lsn >= dstat.layer1, lc = cd_layer >= 0 && l != cd_layer;
    dstat.layer_changes += lc;
    cd_layer = l;
    df_mode.spindlctrl = cd_dual && !l ? SCECdSpinDvdDL0 : SCECdSpinNom;
    if (lsn != cd_head) {
        unsigned int d = lsn > cd_head ? lsn - cd_head : cd_head - lsn;
        dstat.seeks++;
        dstat.far_seeks += d > cd_window || lc;
        w_seek = 1, w_far = d > cd_window || lc;
        if (w_far && df_tab)
            far_note(cd_head ? cd_head - 1 : 0, lsn);
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
                who_st[cd_who].reads++, who_st[cd_who].seeks += w_seek, who_st[cd_who].far += w_far;
                who_st[cd_who].kb += n * 2, who_st[cd_who].ms += now_ms() - t0;
                if (!cd_who && cd_last_who && w_seek)
                    who_st[3].seeks++, who_st[3].far += w_far, who_st[3].ms += now_ms() - t0;
                cd_last_who = cd_who;
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

int df_open(const char *path)
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

int df_close(int fd)
{
    if (!df_is(fd))
        return close(fd);
    df_h[fd - DF_FD].file = -1;
    return 0;
}

int df_lseek(int fd, unsigned int off)
{
    if (!df_is(fd))
        return lseek(fd, off, SEEK_SET);
    df_h[fd - DF_FD].pos = off;
    return off;
}

/* n bytes at the handle's position: whole sectors straight into dst (4-byte aligned there), the partial ones through
 * df_sec. Bytes read, or -1 (a read error before any byte) */
int df_read(int fd, void *dst, unsigned int n)
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

/* ---- disc server (dsrsnd_rpc.h SNDF_DISC_*) ----
 * The pattern of the PS2 games that stream audio from the disc while they load (Jak & Daxter's OVERLORD, the 989snd
 * "stream-safe" reads of Ratchet & Clank and Sly Cooper): one IOP thread owns the drive, the EE only queues requests
 * and polls for their end, audio is served before any game read, and a long read is cut into pages so a refill never
 * waits for more than one. Here the EE's requests sit in slots (the slot the EE chose), the most urgent and then the
 * oldest is served one page per pass of the stream thread, read into one of DISC_BUFS IOP buffers and sent to the EE
 * by SIF DMA while the next page is read. SIF DMA moves whole 16-byte qwords: the bytes of dst outside them travel in
 * the request's SndDiscDone (edge), which goes last, on the same DMA channel, after the data it reports. */
#define DISC_BUFS 3
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

unsigned int df_lsn(int fd, unsigned int off) { return df_is(fd) ? df_h[fd - DF_FD].lsn + off / 2048 : DRIVE_ANY; }

static int disc_work(void)
{
    unsigned int lsn, t0;
    if (snd_drive_want(&lsn, &t0) >= 0)
        return 1;
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
    if (j->r.op == SNDD_READ || j->r.op == SNDD_READV) {
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

/* Burg plan B8b gap reads (default on; DSRSND_DEFS=-DGAP_KB=0 = seek as before): a background read whose first block
 * lies a little ahead of the head (at most GAP_KB, forward, same layer) reads the blocks in between into the page
 * cache instead of seeking over them. The map streamer asks per item and its runs average 1.2 items (B4's ISO
 * world_route: ~8 runs a second, each a seek): the gaps between them are items not wanted yet or already in, so the
 * cache then often serves the next item without the drive. On DVD a 96 KB gap costs ~40 ms of reading against ~40 ms
 * for a short seek and leaves its bytes in the cache. ASSUMED threshold: tuned by the runs a second it saves */
#ifndef GAP_KB
#define GAP_KB 96
#endif
#define GAP_SECS (GAP_KB / 2)
static unsigned int gap_reads, gap_blocks;
static int pc_has(unsigned int lsn)
{
    for (int i = 0; i < pc_n; i++)
        if (pc_use[i] && pc_lsn[i] == lsn)
            return 1;
    return 0;
}
static void gap_fill(unsigned int blk)
{
    if (!GAP_SECS || cd_head >= blk || blk - cd_head > GAP_SECS || (cd_head & (PC_SECS - 1u)) || pc_has(blk) ||
        (dstat.layer1 && (cd_head < dstat.layer1) != (blk < dstat.layer1)))
        return;
    for (unsigned int b = cd_head; b < blk; b += PC_SECS) {
        if (!pc_has(b) && !pc_block(b))
            return;
        gap_blocks++;
        if (cd_head != b + PC_SECS)  /* a cached block between: the head stayed; stop, the seek is not saved */
            return;
    }
    if (!(++gap_reads & 255))
        printf("dsrsnd: gap reads %u (%u blocks of 32 KB read through instead of a seek)\n", gap_reads, gap_blocks);
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
        if (left == n)
            gap_fill(blk);
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

/* SNDD_READV: the got bytes of run position j->done at p (p qword aligned for a qword of the run) to the segments they
 * fall in; the last segment's tail into d's edge. The id of the last DMA (0: none) */
static int scatter_send(DiscJob *j, const unsigned char *p, unsigned int got, SndDiscDone *d)
{
    const SndDiscSeg *sg = (const SndDiscSeg *)j->r.path;
    unsigned int pos = 0, lo = j->done, hi = j->done + got;
    int id = 0;
    if (!j->done)
        d->nhead = 0, d->ntail = 0;
    for (unsigned int i = 0; i < j->r.nseg && pos < hi; pos += sg[i++].bytes) {
        unsigned int s0 = pos > lo ? pos : lo, s1 = pos + sg[i].bytes < hi ? pos + sg[i].bytes : hi;
        if (!sg[i].dst || s1 <= s0)
            continue;
        unsigned int e = s1 == j->r.size ? s1 & ~15u : s1;  /* only the run's end may fall inside a qword */
        if (e > s0)
            id = dma_send((void *)(p + (s0 - lo)), sg[i].dst + (s0 - pos), e - s0);
        if (s1 > e) {
            d->ntail = s1 - e;
            memcpy(d->edge, p + (e - lo), d->ntail);
        }
    }
    return id;
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
    int v = r->op == SNDD_READV;  /* scatter: ee counts from the run's start (its segments are qword aligned there) */
    unsigned int ee = (v ? 0 : r->dst) + j->done, n = r->size - j->done;
    int cached = pc_n && r->prio == SNDD_PRIO_BG && df_is(r->fd);
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
    if (got <= 0 || (v && got < (int)n)) {
        if (v)
            printf("dsrsnd: scatter read seq %u failed at %u of %u (page %u, got %d%s)\n", r->seq, j->done, r->size, n,
                   got, cached ? ", cached" : "");
        disc_finish(j, v ? -1 : j->done ? (int)j->done : got < 0 ? got : 0);
        return;
    }
    if (v) {
        int id = scatter_send(j, p, got, d);
        if (id)
            *(direct ? &pc_dma[pc_slot] : &dbuf_dma[b]) = id;
        j->done += got;
        dstat.pages++, dstat.kb += got >> 10;
        if (j->done >= r->size)
            disc_finish(j, j->done);
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
    if ((j->r.op != SNDD_READ && j->r.op != SNDD_READV) || !df_is(j->r.fd))
        return cd_head;
    return df_h[j->r.fd - DF_FD].lsn + (j->r.offset + j->done) / 2048;
}

/* The next request: the most urgent priority first; within it the one whose next sector is the first at or after the
 * head, else the lowest (one sweep across the disc, then back: the request queue "sorted by LSN" of Sony's streaming
 * talks and the next-file-on-CD rule of console streamers, so queued map reads cost one pass over the disc instead
 * of a seek back and forth per read in submission order). A read in progress stays first: its next page is at the
 * head. Ties go to the oldest. */
static void disc_step(void)
{
    DiscJob *j = NULL, *old = NULL;
    unsigned int jd = 0;
    /* the sound part's next drive work (one queue, snd.c DSRSND_ONE_QUEUE): urgent before everything, background in the
     * sweep with the EE's background reads, as one more of them */
    unsigned int a_lsn = 0, a_t0 = 0;
    int a = snd_drive_want(&a_lsn, &a_t0);
    unsigned int ad = a < 0 ? 0 : a_lsn == DRIVE_ANY ? 0 : a_lsn - (cd_head - 1);
    WaitSema(dq_sema);
    for (int i = 0; i < SND_DISC_SLOTS; i++) {
        DiscJob *c = &dq[i];
        if (!c->live)
            continue;
        /* sectors ahead of the head, wrapping for those behind it; from the last sector read, which a read in
         * progress continues in when its page ended inside a sector */
        unsigned int d = job_lsn(c) - (cd_head - 1);
        if (!j || c->r.prio < j->r.prio ||
            (c->r.prio == j->r.prio && (d < jd || (d == jd && (int)(c->order - j->order) < 0))))
            j = c, jd = d;
        if (!old || (int)(c->order - old->order) < 0)
            old = c;
    }
    /* No request waits for the sweep longer than DISC_AGE_MS: with requests arriving ahead of the head all the time
     * (the pack of the block the player stands in) the sweep never wrapped round to another pack's, and map reads of
     * the neighbour block sat 6-11 s in the map reader's 8 slots while the near level waited behind them (ISO
     * iso_burg_taurus, "cell 0.66 37 KB in 6185 ms", 2026-11-12). The oldest one of the same urgency goes first then. */
    if (j && old && old != j && old->r.prio <= j->r.prio && now_ms() - old->t0 > DISC_AGE_MS)
        j = old, dstat_aged++;
    SignalSema(dq_sema);
    /* the sound part's work: urgent before everything; background like one more background request (after a sync one;
     * the one further back in the sweep waits, unless one of the two waited past DISC_AGE_MS: then the older) */
    int pick = a == 0 || (a == 1 && !j);
    if (a == 1 && j && j->r.prio == SNDD_PRIO_BG) {
        unsigned int now = now_ms();
        int a_aged = now - a_t0 > DISC_AGE_MS, j_aged = now - j->t0 > DISC_AGE_MS;
        pick = a_aged || j_aged ? a_aged && (!j_aged || (int)(a_t0 - j->t0) <= 0) : ad <= jd;
    }
    if (pick) {
        cd_take();
        snd_drive_step();
        SignalSema(cd_sema);
        return;
    }
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
    case SNDD_READV:
        disc_page(j);
        break;
    default:
        disc_finish(j, -1);
        break;
    }
}

/* SNDF_DISC_SUBMIT (RPC thread): into their slots; a cancel only marks its target */
void disc_submit(const SndDiscReq *q, int n)
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
        if (r->op == SNDD_READV) {  /* a segment list that does not add up is dropped with an error result */
            const SndDiscSeg *sg = (const SndDiscSeg *)r->path;
            unsigned int sum = 0, ok = r->nseg && r->nseg <= SND_SEGS;
            for (unsigned int k = 0; ok && k < r->nseg; k++)
                ok = !(sg[k].dst & 15) && (k + 1 == r->nseg || !(sg[k].bytes & 15)), sum += sg[k].bytes;
            if (!ok || sum != r->size) {
                printf("dsrsnd: scatter read seq %u: bad segment list (%u segments, %u of %u bytes)\n", r->seq, r->nseg,
                       sum, r->size);
                memcpy(&j->r, r, sizeof *r);
                j->r.op = 0;  /* finished with -1 by disc_step's default case */
                j->done = 0, j->cancel = 0, j->order = dq_order++, j->t0 = now_ms();
                j->live = 1;
                continue;
            }
        }
        memcpy(&j->r, r, sizeof *r);
        j->done = 0, j->cancel = 0, j->order = dq_order++, j->t0 = now_ms();
        j->live = 1;
    }
    SignalSema(dq_sema);
    dstat.iop_free = QueryTotalFreeMemSize();
}

/* the disc thread: below every other thread, so its waits in the CD driver never hold up an RPC (fileio's printf
 * included: a busy stream thread delayed each EE printf by up to a page, ISO 2026-11-05) */
void disc_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (!disc_work())
            DelayThread(POLL_US);
        disc_step();
    }
}

/* ---- the drive lock, for the sound part (snd.c) ---- */

int drive_poll(unsigned int *waited)
{
    *waited = 0;
    if (PollSema(cd_sema) < 0) {
        if (!cd_want)
            cd_want_ms = now_ms();
        cd_want = 1;
        return 0;
    }
    if (cd_want)
        *waited = now_ms() - cd_want_ms;
    cd_want = 0;
    return 1;
}

void drive_give(void) { SignalSema(cd_sema); }

/* ---- setup (dsrsnd.c _start) ---- */

int disc_init_semas(void)
{
    iop_sema_t sm = {.attr = 0, .option = 0, .initial = 1, .max = 1};
    dq_sema = CreateSema(&sm);
    cd_sema = CreateSema(&sm);
    return dq_sema >= 0 && cd_sema >= 0;
}

int disc_init_bufs(void)
{
    /* the disc pages before the read-ahead: the EE's reads need at least one, the read-ahead can do without */
    while (dbuf_n < DISC_BUFS && (dbuf[dbuf_n] = AllocSysMemory(ALLOC_FIRST, DISC_BUF, NULL)))
        dbuf_n++;
    if (!dbuf_n)
        return 0;
    df_tab = AllocSysMemory(ALLOC_FIRST, DF_FILES * sizeof(DfFile), NULL);  /* none: cdrom0: through ioman */
    for (int h = 0; h < DF_HANDLES; h++)
        df_h[h].file = -1;
    return 1;
}

void disc_init_cache(void)
{
#ifndef DSRSND_NO_CACHE
    unsigned int f = QueryTotalFreeMemSize(), want = f > PC_KEEP_FREE ? (f - PC_KEEP_FREE) / PC_PAGE : 0;
    for (pc_n = want < PC_PAGES ? want : PC_PAGES; pc_n && !(pc_mem = AllocSysMemory(ALLOC_FIRST, pc_n * PC_PAGE, NULL));)
        pc_n--;
    dstat.cache_kb = pc_n * (PC_PAGE >> 10);
#endif
}

void disc_set_done(unsigned int ee) { ee_done = ee & 0x1fffffff; }

int disc_pages(void) { return dbuf_n; }
