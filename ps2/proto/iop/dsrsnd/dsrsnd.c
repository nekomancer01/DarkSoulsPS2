/* dsrsnd.irx: IOP side of the sound runtime (EE side src/sound.c, protocol src/dsrsnd_rpc.h).
 * One RPC thread applies the EE's commands through libsd (uploads on SPU2 DMA channel 0, voice registers); one stream
 * thread keeps up to SND_STREAMS disc streams fed: every few ms it reads each stream voice's next address (NAX), and
 * once the voice has moved into the other half of its 2 x chunk ring, it reads the next chunk of every channel from
 * the file (ioman: host: under PCSX2, cdrom0: from a disc) and DMAs it over the half just played (channel 1). Block
 * flags are rewritten for the ring: the first block of half 0 sets the loop start, the last block of half 1 jumps back
 * to it. The same thread loads sample banks (SNDF_LOAD): SND_LOAD_STEP bytes from the file into SPU2 RAM per pass, so
 * the EE spends neither RAM nor time on them. libsd calls of both threads go through one semaphore. */
#include "irx_imports.h"
#include "dsrsnd_rpc.h"

IRX_ID("dsrsnd", 1, 0);

#define STREAM_DMA 1  /* SPU2 DMA channel of the stream thread (refills and bank loads) */
#define POLL_US 4000  /* stream thread period */
/* read-ahead per stream: one disc read fills this much, the refills then come from IOP RAM. Each read from a stream
 * pulls the drive away from the game's own pack reads and back (two seeks); with 16 KB refills the looping area
 * ambience alone cut a cdrom0: boot load from 7 s to 37 s in PCSX2 (2026-11-03). */
#define STREAM_RA (128 * 1024)
#define ADSR1_FLAT SD_SET_ADSR1(SD_ADSR_AR_LINEARi, 0, 0xf, 0xf)  /* instant attack, no decay, sustain level max */
#define ADSR2_FLAT(rr) SD_SET_ADSR2(SD_ADSR_SR_LINEARi, 0x7f, SD_ADSR_RR_LINEARd, (rr))  /* sustain held */

/* Two servers, one per EE client (frames / ops), each with its own thread and receive buffer: a client DMAs its
 * arguments straight into the server's buffer, so two calls in flight on one server overwrote each other (the ops
 * call never got served: a hang walking into the Burg, 2026-11-03) */
static SifRpcDataQueue_t queue[3];
static SifRpcServerData_t server[3];
static unsigned int rpc_buf[3][(sizeof(SndFrame) + 63) / 4] __attribute__((aligned(64)));
static SndStatus status __attribute__((aligned(64)));
static SndDisc disc_reply __attribute__((aligned(64)));
static int sd_sema = -1;
/* the disc lock (dsrsnd_rpc.h SNDF_DISC_*): this module's open / read / lseek / close of the disc happen only in the
 * stream thread while it holds it, taken with PollSema so that thread never waits on the EE (which may itself wait
 * on the RPC threads) */
static int disc_sema = -1;

typedef struct {
    volatile int state;
    int fd, loop, channels, chunk, next_fill, silent;
    int close_fd;  /* stopped by an RPC: closed by the stream thread once it has the disc */
    unsigned int offset, size, pos, spu_buf;
    unsigned char *buf;  /* chunk * channels */
    unsigned char *ra;   /* STREAM_RA bytes read ahead (NULL: no IOP RAM for it, read per refill) */
    unsigned int ra_pos, ra_len;
    SndStreamOpen req;   /* SNDS_OPENING: the open the stream thread does once it has the disc */
} Stream;
static Stream streams[SND_STREAMS];
static unsigned char *stream_mem;  /* SND_STREAMS x 2 x SND_STREAM_CHUNK_MAX, then SND_LOAD_STEP for loads */
static SndLoad load;               /* the bank load in progress (load_fd >= 0) or queued (load_new) */
static int load_fd = -1, load_new;
static unsigned int load_pos;
static int stream_sema = -1;       /* guards streams[] between the two threads */

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

/* n bytes of the stream file at the current position, through the read-ahead buffer (n <= what is left of the
 * file: the caller wraps loops) */
static int stream_read(Stream *s, unsigned char *dst, unsigned int n)
{
    if (!s->ra)
        return read(s->fd, dst, n);
    if (s->ra_pos >= s->ra_len) {
        unsigned int left = s->size - s->pos, want = left < STREAM_RA ? left : STREAM_RA;
        int r = read(s->fd, s->ra, want);
        if (r <= 0)
            return r;
        s->ra_pos = 0, s->ra_len = r;
    }
    if (n > s->ra_len - s->ra_pos)
        n = s->ra_len - s->ra_pos;
    memcpy(dst, s->ra + s->ra_pos, n);
    s->ra_pos += n;
    return n;
}

/* Read the next chunk of every channel (looping or padding with silence at the end) and send it to ring half h */
static int stream_fill(Stream *s, int slot, int h)
{
    int want = s->chunk * s->channels, got = 0;
    if (!s->silent) {
        while (got < want) {
            if (s->pos >= s->size) {
                if (!s->loop)
                    break;
                s->pos = 0, s->ra_pos = s->ra_len = 0;
                lseek(s->fd, s->offset, SEEK_SET);
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
static void load_step(void)
{
    if (load_new) {
        load_new = 0;
        if (load_fd >= 0)
            close(load_fd);
        load_fd = open(load.path, O_RDONLY);
        load_pos = 0;
        if (load_fd < 0 || lseek(load_fd, load.offset, SEEK_SET) < 0) {
            printf("dsrsnd: load %s failed (%d)\n", load.path, load_fd);
            if (load_fd >= 0)
                close(load_fd);
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
    int r = read(load_fd, b, n);
    if (r != (int)n) {
        close(load_fd);
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
        close(load_fd);
        load_fd = -1;
        status.load_done = load.id;
    }
}

static void stream_start(Stream *s);

static void stream_thread(void *arg)
{
    (void)arg;
    for (;;) {
        DelayThread(POLL_US);
        WaitSema(stream_sema);
        if (PollSema(disc_sema) < 0) {  /* the EE is reading the disc: refills wait (the ring plays on) */
            SignalSema(stream_sema);
            continue;
        }
        for (int i = 0; i < SND_STREAMS; i++) {
            Stream *s = &streams[i];
            if (s->close_fd >= 0)
                close(s->close_fd), s->close_fd = -1;
            if (s->state == SNDS_OPENING)
                stream_start(s);
            if (s->state != SNDS_PLAYING && s->state != SNDS_ENDING)
                continue;
            sd_lock();
            unsigned int nax = sceSdGetAddr(vreg(SND_STREAM_VOICE(i)) | SD_VADDR_NAX);
            sd_unlock();
            int cur = nax >= s->spu_buf + s->chunk;  /* half the left voice is playing */
            if (cur == s->next_fill)
                continue;
            if (s->silent >= 2) {  /* the voice has reached the silence after the data */
                stream_close(s, i);
                close(s->close_fd), s->close_fd = -1;
                continue;
            }
            if (stream_fill(s, i, s->next_fill) < 0) {
                stream_close(s, i);
                close(s->close_fd), s->close_fd = -1;
                s->state = SNDS_ERROR;
                continue;
            }
            if (s->silent)
                s->state = SNDS_ENDING;
            s->next_fill ^= 1;
        }
        for (int i = 0; i < SND_STREAMS; i++)
            status.stream_state[i] = streams[i].state;
        load_step();  /* after the refills: a stream never waits for a bank */
        SignalSema(disc_sema);
        SignalSema(stream_sema);
    }
}

/* SNDF_STREAM: queue the open for the stream thread (it needs the disc) */
static void stream_open(const SndStreamOpen *o)
{
    if (o->slot >= SND_STREAMS || o->channels < 1 || o->channels > 2 || o->chunk > SND_STREAM_CHUNK_MAX ||
        (o->chunk & 63) || !o->chunk)
        return;
    WaitSema(stream_sema);
    Stream *s = &streams[o->slot];
    if (s->state != SNDS_IDLE && s->state != SNDS_ERROR && s->state != SNDS_OPENING)
        stream_close(s, o->slot);
    memcpy(&s->req, o, sizeof *o);
    s->state = SNDS_OPENING;
    status.stream_state[o->slot] = s->state;
    SignalSema(stream_sema);
}

/* the stream thread, holding stream_sema and the disc: open and fill a queued stream, key its voices on */
static void stream_start(Stream *s)
{
    const SndStreamOpen *o = &s->req;
    char path[sizeof o->path];
    strncpy(path, o->path, sizeof path - 1);
    path[sizeof path - 1] = 0;
    s->fd = open(path, O_RDONLY);
    if (s->fd < 0) {
        printf("dsrsnd: open %s failed (%d)\n", path, s->fd);
        status.errors++;
        s->state = SNDS_ERROR;
        return;
    }
    s->offset = o->offset, s->size = o->size, s->loop = o->loop, s->chunk = o->chunk, s->channels = o->channels;
    s->spu_buf = o->spu_buf, s->pos = 0, s->silent = 0, s->next_fill = 0, s->ra_pos = s->ra_len = 0;
    s->buf = stream_mem + o->slot * 2 * SND_STREAM_CHUNK_MAX;
    status.stream_bytes[o->slot] = 0;
    lseek(s->fd, s->offset, SEEK_SET);
    if (stream_fill(s, o->slot, 0) < 0 || stream_fill(s, o->slot, 1) < 0) {
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
            WaitSema(stream_sema);
            if (streams[c->voice].state != SNDS_IDLE)
                stream_close(&streams[c->voice], c->voice);
            status.stream_state[c->voice] = SNDS_IDLE;
            SignalSema(stream_sema);
        }
    }
}

static void *rpc_server(int fno, void *data, int size)
{
    (void)size;
    switch (fno) {
    case SNDF_INIT:
        sd_lock();
        init_spu();
        sd_unlock();
        break;
    case SNDF_LOAD:
        WaitSema(stream_sema);
        memcpy(&load, data, sizeof load);
        load.path[sizeof load.path - 1] = 0;
        load_new = 1;
        SignalSema(stream_sema);
        break;
    case SNDF_FRAME:
        frame(data);
        break;
    case SNDF_STREAM:
        stream_open(data);
        break;
    case SNDF_DISC_LOCK:
        disc_reply.got = WaitSema(disc_sema) == 0;
        return &disc_reply;
    case SNDF_DISC_TRY:
        disc_reply.got = PollSema(disc_sema) == 0;
        return &disc_reply;
    case SNDF_DISC_UNLOCK:
        SignalSema(disc_sema);
        disc_reply.got = 0;
        return &disc_reply;
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
    disc_sema = CreateSema(&sm);
    stream_mem = AllocSysMemory(ALLOC_FIRST, SND_STREAMS * 2 * SND_STREAM_CHUNK_MAX + SND_LOAD_STEP, NULL);
    if (sd_sema < 0 || stream_sema < 0 || disc_sema < 0 || !stream_mem)
        return MODULE_NO_RESIDENT_END;
    unsigned char *ra = AllocSysMemory(ALLOC_FIRST, SND_STREAMS * STREAM_RA, NULL);
    if (!ra)
        printf("dsrsnd: no IOP RAM for stream read-ahead, reading per refill\n");
    for (int i = 0; i < SND_STREAMS; i++)
        streams[i].fd = streams[i].close_fd = -1, streams[i].ra = ra ? ra + i * STREAM_RA : NULL;
    int id;
    for (int k = 0; k < 3; k++) {  /* frame server, ops server, disc lock server (waits in WaitSema for the EE) */
        iop_thread_t t = {.attr = TH_C, .thread = rpc_thread, .priority = 40 + k, .stacksize = 0x800, .option = 0};
        if ((id = CreateThread(&t)) < 0)
            return MODULE_NO_RESIDENT_END;
        StartThread(id, (void *)k);
    }
    /* the stream thread runs above the RPC thread (and below the CD driver) so a refill is not held up by uploads */
    iop_thread_t st = {.attr = TH_C, .thread = stream_thread, .priority = 30, .stacksize = 0x800, .option = 0};
    id = CreateThread(&st);
    if (id < 0)
        return MODULE_NO_RESIDENT_END;
    StartThread(id, NULL);
    printf("dsrsnd: ready (%d streams)\n", SND_STREAMS);
    return MODULE_RESIDENT_END;
}
