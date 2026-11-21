/* dsrsnd.irx inside: what its parts share (dsrsnd.c module start and RPC servers, disc.c the drive, snd.c sound) */
#ifndef DSRSND_I_H
#define DSRSND_I_H

#define POLL_US 4000  /* stream thread / idle disc thread period */
/* ioman's file table holds 16 handles (2 taken before the game), shared by the EE's packs (world blocks, menu.pak,
 * pack.c's job cache) and this module's streams and bank load: in a world build with enemies streaming in it ran full
 * (ISO 2026-11-05, smoke: 11 EE handles + 3 streams). An open of this module's own that finds it full is retried on
 * later passes for up to OPEN_RETRY_MS instead of failing the stream or bank. */
#define EMFILE_ 24

/* ms since boot (bus clock 36.864 MHz; no 64-bit divide in an IRX) */
static inline unsigned int now_ms(void)
{
    iop_sys_clock_t c;
    GetSystemTime(&c);
    return c.hi * 116508u + c.lo / 36864u;
}

/* ---- disc.c ---- */
extern SndDiscStat dstat;
extern int cd_who;  /* the reader of the next drive calls (the drive log): 0 EE request, 1 stream read-ahead, 2 bank */
/* files on the disc by sector (cdrom0:), else ioman: open / close / lseek / read; the caller holds the drive */
int df_open(const char *path);
int df_close(int fd);
int df_lseek(int fd, unsigned int off);
int df_read(int fd, void *dst, unsigned int n);
/* the drive lock for the stream thread: 1 taken (*waited: ms since it first asked, 0 if it did not have to), 0 busy
 * (the disc thread leaves it after its current page); drive_give hands it back */
int drive_poll(unsigned int *waited);
void drive_give(void);
void who_log(void);
#define DRIVE_ANY 0xffffffffu  /* drive work that reads nothing (an open, a close): served where the head is */
unsigned int df_lsn(int fd, unsigned int off);  /* the sector of byte off of fd, DRIVE_ANY when not read by sector */  /* the "dsrsnd: drive" lines, every DRIVE_LOG_MS */
void disc_submit(const SndDiscReq *q, int n);
void disc_thread(void *arg);
int disc_init_semas(void);
int disc_init_bufs(void);
void disc_init_cache(void);
void disc_set_done(unsigned int ee);
int disc_pages(void);

/* ---- snd.c ---- */
#define STREAM_PRIO 30  /* the stream thread: above the RPC threads (40-42), below the CD driver */
extern SndStatus status;
void snd_init(void);
void snd_load_post(const void *data);
void snd_frame(const SndFrame *f);
void snd_stream_open(const SndStreamOpen *o);
void stream_thread(void *arg);
int snd_init_semas(void);
int snd_init_mem(void);
void snd_init_ra(void);
/* one queue (snd.c DSRSND_ONE_QUEUE): the sound part's next drive work, -1 none, 0 urgent (before every EE request),
 * 1 background (*lsn where it reads, DRIVE_ANY anywhere; *t0 since when it waits); snd_drive_step does one step of it */
int snd_drive_want(unsigned int *lsn, unsigned int *t0);
void snd_drive_step(void);

#endif
