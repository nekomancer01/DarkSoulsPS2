/* dsrsnd.irx, sound part: libsd (voice commands, uploads on SPU2 DMA channel 0), the disc streams and the sample
 * bank loads (the module's overview: dsrsnd.c). It reads the disc through the disc part (disc.c: df_*, the drive
 * lock drive_poll / drive_give). */
#include "irx_imports.h"
#include "dsrsnd_rpc.h"
#include "dsrsnd_i.h"

#define DISC_WAIT_LOG_MS 100  /* a gap between stream passes longer than this is logged (the plan's one-page bound) */
#define LOAD_TRIP 8  /* SND_LOAD_STEP reads of a bank per trip of the drive (ASSUMED: under a stream's half second) */
#define STREAM_DMA 1  /* SPU2 DMA channel of the stream thread (refills and bank loads) */
/* the stream thread's priority while it holds the drive (STREAM_PRIO otherwise, dsrsnd_i.h): below every thread
 * but the disc thread (120). A CD read keeps its thread busy until the data is in, so at 45 a read-ahead top-up
 * (up to 128 KB, ~150 ms) held up fileio for its whole length: every EE printf in that time waited (frames of
 * 100-480 ms; 57-59 frames over 100 ms on the ISO route, 2026-11-06).
 * Above the disc thread, so the EE's page reads never hold a top-up back. */
#define STREAM_PRIO_DISC 119
/* read-ahead per stream: one disc read fills this much, the refills then come from IOP RAM. Each read from a stream
 * pulls the drive away from the game's own pack reads and back (two seeks); with 16 KB refills the looping area
 * ambience alone cut a cdrom0: boot load from 7 s to 37 s in PCSX2 (2026-11-03). */
/* Burg plan B8b: 160 KB (DSRSND_DEFS=-DSTREAM_RA_KB=128 = before). With three streams playing their top-ups were
 * ~2 seeks a second on their own (two per top-up, one every ~2.6 s each); at 160 KB one every ~3.3 s. 192 KB measured
 * 7.27 -> 6.95 seeks/s on world_route but left the IOP 309 KB free (gate 250); 160 KB costs 3 x 32 KB (~405 KB free) */
#ifndef STREAM_RA_KB
#define STREAM_RA_KB 160
#endif
#define STREAM_RA (STREAM_RA_KB * 1024)
/* Refills come from the read-ahead without the drive; the drive is only taken to top the read-ahead up, from the
 * moment it is down to half (~2.6 s of 22 kHz stereo left). Before (2026-11-05, ISO world_route) every refill waited
 * for the EE's disc lock while the EE streamed the map: 298 of 358 s locked out, 137 underruns. */
#define STREAM_RA_TOP (STREAM_RA / 2)
#define STREAM_RA_ALSO (STREAM_RA * 3 / 4)  /* the others top up with it from here */
/* below this the stream is starving (~1.3 s left, or a half it cannot fill): reported in status.starving */
#define STREAM_RA_LOW (STREAM_RA / 4)
/* opens that find ioman's file table full (EMFILE_, dsrsnd_i.h) are retried for this long */
#define OPEN_RETRY_MS 10000
#define ADSR1_FLAT SD_SET_ADSR1(SD_ADSR_AR_LINEARi, 0, 0xf, 0xf)  /* instant attack, no decay, sustain level max */
#define ADSR2_FLAT(rr) SD_SET_ADSR2(SD_ADSR_SR_LINEARi, 0x7f, SD_ADSR_RR_LINEARd, (rr))  /* sustain held */

SndStatus status __attribute__((aligned(64)));
static int sd_sema = -1;
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

static inline int stream_ra_more(const Stream *s) { return s->loop || s->rd_pos < s->size; }
/* a playing stream whose read-ahead could take more of the file */
static inline int stream_ra_want(const Stream *s)
{
    return s->ra && !s->silent && (s->state == SNDS_PLAYING || s->state == SNDS_ENDING) && stream_ra_more(s);
}

/* Top the read-ahead up to full from the file (the caller holds the drive: disc_take). Loops are unrolled into it: at the end
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
        cd_who = 1;
        int r = df_read(s->fd, s->ra + w, n);
        cd_who = 0;
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
    cd_who = 2;
    int r = df_read(load_fd, b, n);
    cd_who = 0;
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

static void stream_start(Stream *s);

/* the drive for this pass of the stream thread, taken on first use: 1 held, -1 the disc thread reads a page (it
 * leaves the drive after it: drive_poll asked for it), 0 not asked yet */
static int disc_take(int *d)
{
    if (!*d) {
        unsigned int waited;
        if (!drive_poll(&waited)) {
            *d = -1;
        } else {
            if (waited)
                cd_wait_note(waited);
            *d = 1;
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

void stream_thread(void *arg)
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
        who_log();
        /* A bank goes LOAD_TRIP steps (128 KB, ~150 ms of the drive) per trip: one SND_LOAD_STEP per pass sent the
         * drive to the bank and back to the EE's map reads for every 16 KB, two far seeks (~240 ms) a step; the banks
         * of a block's enemies took ~3 s of drive time in the 10 s a Burg block was loading (ISO, 2026-11-12). */
        if ((load_new || load_fd >= 0) && disc_take(&disc)) {
            load_step();  /* after the refills: a stream never waits for a bank */
            for (int k = 1; k < LOAD_TRIP && load_fd >= 0 && !load_new; k++)
                load_step();
        }
        if (disc > 0) {
            ChangeThreadPriority(TH_SELF, STREAM_PRIO);  /* back up before the drive goes to the disc thread */
            drive_give();
        }
        SignalSema(stream_sema);
    }
}

/* SNDF_STREAM: queue the open for the stream thread (it needs the drive) */
void snd_stream_open(const SndStreamOpen *o)
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

void snd_frame(const SndFrame *f)
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

/* ---- setup and the RPC side (dsrsnd.c) ---- */

void snd_init(void)
{
    sd_lock();
    init_spu();
    sd_unlock();
}

void snd_load_post(const void *data)
{
    WaitSema(req_sema);
    memcpy(&load_req, data, sizeof load_req);
    load_req.path[sizeof load_req.path - 1] = 0;
    load_pend = 1;
    SignalSema(req_sema);
}

int snd_init_semas(void)
{
    iop_sema_t sm = {.attr = 0, .option = 0, .initial = 1, .max = 1};
    sd_sema = CreateSema(&sm);
    stream_sema = CreateSema(&sm);
    req_sema = CreateSema(&sm);
    return sd_sema >= 0 && stream_sema >= 0 && req_sema >= 0;
}

int snd_init_mem(void)
{
    stream_mem = AllocSysMemory(ALLOC_FIRST, SND_STREAMS * 2 * SND_STREAM_CHUNK_MAX + SND_LOAD_STEP, NULL);
    return stream_mem != NULL;
}

void snd_init_ra(void)
{
    unsigned char *ra = AllocSysMemory(ALLOC_FIRST, SND_STREAMS * STREAM_RA, NULL);
    if (!ra)
        printf("dsrsnd: no IOP RAM for stream read-ahead, reading per refill\n");
    for (int i = 0; i < SND_STREAMS; i++)
        streams[i].fd = streams[i].close_fd = -1, streams[i].ra = ra ? ra + i * STREAM_RA : NULL;
}
