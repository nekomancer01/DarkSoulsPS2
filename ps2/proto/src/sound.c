/* Sound runtime (A12, src/sound.h). Data: tools/export_audio.py (bank.bin / vag.bin / str.bin / cue.bin / emit.bin in
 * data/snd/{main,mAA,cXXXX}.pak). IOP side: iop/dsrsnd (dsrsnd.irx, embedded with ps2sdk's libsd.irx).
 *
 * SPU2 RAM: [SPU_HEAP0, SPU_STREAMS0) holds the resident banks (main, the area, enemy banks as their types load), each
 * one block the IOP reads straight from the pack's raw vag.bin; [SPU_STREAMS0, 2 MB) the 3 stream rings. No reverb.
 * Voices 0-39 and 46-47 play effects (one per channel), 40-45 the stream slots (dsrsnd_rpc.h). Each frame the commands
 * (key on / off, volumes) go to the IOP in one RPC that does not wait (its result, the ENDX bits, is read the next
 * frame); stream opens and bank loads go through a second client, also without waiting.
 * DSR's FMOD does the 3D part with custom rolloff curves per event (envelopes, not exported: TECH_DEBT audio-4); here
 * an event is full volume up to its min distance and fades linearly to 0 at its max distance from the listener, which
 * is the camera (ASSUMED), panned by the angle to the camera's right axis. */
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopheap.h>
#include <sbv_patches.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sound.h"
#include "dsrsnd_rpc.h"
#include "pack.h"
#include "render.h"
#include "collision.h"

extern unsigned char dsrsnd_irx[], libsd_irx[];
extern unsigned int size_dsrsnd_irx, size_libsd_irx;

#define SPU_HEAP0 0x5040u  /* above the cores' input / output areas */
#define SPU_STREAMS0 (0x200000u - SND_STREAMS * 2 * 2 * SND_STREAM_CHUNK_MAX)
#define STREAM_CHUNK 8192u  /* tools/export_audio.py STREAM_CHUNK */
#define MAX_BANKS 16
#define MAX_LOADS 16
#define VOL_MAX 0x3fff
#define MASTER_VOL 0x2400
#define BODY_Y 1.0f        /* ASSUMED: character sounds play 1 m above the feet (dummy polys not used) */
#define EMIT_ONESHOT_MIN 4.0f  /* ASSUMED: a non-looping MSB emitter replays every 4-12 s while in range */
#define EMIT_ONESHOT_VAR 8.0f
#define MUSIC_FADE 1.0f    /* ASSUMED: music / ambience fade in and out over 1 s */
#define MAX_ENTS 64

typedef struct { unsigned off, bytes; unsigned short rate; unsigned char ch, flags; unsigned chunk; } SSample;
typedef struct {
    unsigned id;
    char type;
    unsigned char flags, nwaves, cat;
    unsigned short first, maxpb;
    float vol, vol_rand, min_d, max_d, pitch_rand;
} SEvent;
typedef struct { unsigned short sample, weight; } SWave;
typedef struct { int id; unsigned short first, n; } SCueAnim;
typedef struct { float t0, t1; int id; signed char type; unsigned char kind; signed char dummy, slot; } SCue;
typedef struct { int entity, id; unsigned char type, shape, map, pad; float pos[3], rot, dim[3]; } SEmit;
_Static_assert(sizeof(SSample) == 16 && sizeof(SEvent) == 32 && sizeof(SCue) == 16 && sizeof(SEmit) == 40, "snd");
#define SF_LOOP 1
#define SF_STREAM 2
#define SF_DROP 4
#define EF_LOOP 1
#define EF_3D 2

typedef struct {
    int voice, stream;  /* playing voice (-1) or stream slot (-1) */
    float next;         /* one-shot: seconds to the next play */
} EmitRt;

typedef struct {
    char name[8];
    int used, keep, ready, load_id, failed;
    unsigned dead_frame;  /* unloaded: its SPU2 range is reused 2 frames later (key offs reach the IOP first) */
    void *blob, *cue_blob, *emit_blob;
    const SSample *smp;
    const SEvent *ev;
    const SWave *wav;
    unsigned nsmp, nev, nwav;
    unsigned spu, vag_off, vag_size, str_off, str_size;
    const SCueAnim *canim;
    const SCue *cue;
    unsigned ncanim;
    int armor;
    const SEmit *emit;
    unsigned nemit;
    EmitRt *ert;
    char path[96];
} Bank;

typedef struct {
    int on, chan, stereo, pair, loop, is3d, emitter, slot;
    Bank *bank;
    char type;
    unsigned id, owner, kon_frame, kon_seq;
    float pos[3], min_d, max_d, vol, left, slot_end;
    int slot_anim;
    unsigned short l, r;
} Voice;

typedef struct {
    int on, stopping, smp, open_frame, emitter;
    Bank *bank;
    char type;
    unsigned id;
    float vol, cur;  /* target and current volume (0..1) */
    unsigned short l, r;
} Slot;

SndStats snd_stats;
static int on;
static Bank banks[MAX_BANKS];
static Voice voices[SND_VOICES];
static Slot slots[SND_STREAMS];
static unsigned frame_no, rng = 0x2545F491;
static unsigned rpc_seq, inflight_seq, st_seq;  /* frame RPCs sent; the one in flight; the one st comes from */
static SifRpcClientData_t cl_frame __attribute__((aligned(64))), cl_ops __attribute__((aligned(64)));
static SndFrame fbuf __attribute__((aligned(64)));      /* frame RPC send buffer (in flight) */
static SndStatus fstat __attribute__((aligned(64)));    /* its result */
static SndStatus ostat __attribute__((aligned(64)));    /* ops client result (unused) */
static SifRpcClientData_t cl_disc __attribute__((aligned(64)));  /* disc lock (pack.c pack_disc_lock) */
static SndDisc dstat __attribute__((aligned(64)));
static union { SndLoad ld; SndStreamOpen so; } obuf __attribute__((aligned(64)));
static SndCmd cmds[SND_CMD_MAX];
static int ncmds, frame_busy, ops_busy;
static SndStatus st;  /* last status read back */
static int load_sent;  /* id of the load the IOP is working on, 0 none */
static int next_load_id = 1;
static struct { int entity, on; } ents[MAX_ENTS];
static int nents;
static int area_aa = -1, area_bb = -1;
static Bank *area_bank, *main_bank;
static V3 eye, right_v;
static struct { unsigned owner; int anim; float t; } owners[1 + 64];

static inline float frand(void)
{
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) / 16777216.0f;
}

/* ---- RPC ---- */

static void ops_wait(void)
{
    while (ops_busy && SifCheckStatRpc(&cl_ops))
        ;
    ops_busy = 0;
}

static void frame_wait(void)
{
    if (frame_busy) {
        while (SifCheckStatRpc(&cl_frame))
            ;
        frame_busy = 0;
        memcpy(&st, (void *)((unsigned)&fstat | 0x20000000), sizeof st);  /* uncached: the IOP wrote it */
        st_seq = inflight_seq;
    }
}

/* Send the queued commands (async); a previous frame still in flight keeps them queued unless the queue is full */
static void flush(int force)
{
    if (frame_busy && SifCheckStatRpc(&cl_frame) && !force && ncmds < SND_CMD_MAX - 16)
        return;
    frame_wait();
    fbuf.ncmds = ncmds;
    memcpy(fbuf.cmd, cmds, ncmds * sizeof(SndCmd));
    unsigned size = (16 + ncmds * sizeof(SndCmd) + 63) & ~63u;
    SifCallRpc(&cl_frame, SNDF_FRAME, SIF_RPC_M_NOWAIT, &fbuf, size, &fstat, sizeof fstat, NULL, NULL);
    frame_busy = 1;
    inflight_seq = ++rpc_seq;
    ncmds = 0;
}

static void cmd(int op, int voice, unsigned addr, int pitch, int l, int r, int adsr2)
{
    if (ncmds >= SND_CMD_MAX)
        flush(1);
    cmds[ncmds++] = (SndCmd){.op = op, .voice = voice, .pitch = pitch, .addr = addr, .voll = l, .volr = r,
                             .adsr2 = adsr2};
}

static int ops_call(int fn, unsigned size)
{
    ops_wait();
    SifCallRpc(&cl_ops, fn, SIF_RPC_M_NOWAIT, &obuf, (size + 15) & ~15u, &ostat, sizeof ostat, NULL, NULL);
    ops_busy = 1;
    return 1;
}

/* The disc lock for pack.c. lock(1) is a blocking call (the EE's next fio request waits for the answer anyway);
 * pack_job_step's try (disc_try_async) and unlock go out without waiting: a blocking try waited 180-550 ms for the IOP
 * whenever dsrsnd was reading the disc (a stream refill or bank load step), each a frame stall while enemy types and
 * their banks streamed in (ISO 2026-11-04). A try's answer is collected by the next lock call (one frame later); one
 * that got the lock but was never asked again is given back by snd_update. */
static int disc_req;              /* 0 none, 1 a try in flight, 2 a try that got the lock (not claimed), 3 unlock */
static unsigned disc_req_frame;

static int disc_reply_got(void) { return ((volatile SndDisc *)((unsigned)&dstat | 0x20000000))->got; }  /* uncached */

static int disc_collect(int wait)
{
    if (disc_req == 1 || disc_req == 3) {
        if (SifCheckStatRpc(&cl_disc)) {
            if (!wait)
                return 0;
            while (SifCheckStatRpc(&cl_disc))
                ;
        }
        disc_req = disc_req == 1 && disc_reply_got() ? 2 : 0;
        disc_req_frame = frame_no;
    }
    return 1;
}

static int disc_lock(int wait)
{
    disc_collect(1);  /* a background try in flight: its answer first */
    if (disc_req == 2) {
        disc_req = 0;
        return 1;
    }
    SifCallRpc(&cl_disc, wait ? SNDF_DISC_LOCK : SNDF_DISC_TRY, 0, NULL, 0, &dstat, sizeof dstat, NULL, NULL);
    return disc_reply_got();
}

/* pack.c's background jobs (pack_job_step): 1 when the lock is held, else 0 and a try is on its way (or still) */
static int disc_try_async(void)
{
    if (!disc_collect(0))
        return 0;
    if (disc_req == 2) {
        disc_req = 0;
        return 1;
    }
    SifCallRpc(&cl_disc, SNDF_DISC_TRY, SIF_RPC_M_NOWAIT, NULL, 0, &dstat, sizeof dstat, NULL, NULL);
    disc_req = 1, disc_req_frame = frame_no;
    return 0;
}

/* a disc stream runs short (status of the last finished frame RPC, a frame or two old): the EE's background reads
 * wait so the IOP's read-ahead top-up gets the drive */
static int disc_yield(void) { return st.starving != 0; }

static void disc_unlock(void)
{
    disc_collect(1);
    SifCallRpc(&cl_disc, SNDF_DISC_UNLOCK, SIF_RPC_M_NOWAIT, NULL, 0, &dstat, sizeof dstat, NULL, NULL);
    disc_req = 3;
}

/* ---- SPU2 heap ---- */

static int range_used(unsigned a, unsigned size)
{
    for (int i = 0; i < MAX_BANKS; i++) {
        const Bank *b = &banks[i];
        int live = b->used || (b->vag_size && frame_no <= b->dead_frame + 2);
        if (live && b->vag_size && b->spu && a < b->spu + b->vag_size && b->spu < a + size)  /* spu 0: being placed */
            return (int)(b->spu + b->vag_size - a);  /* skip past it */
    }
    return 0;
}

static unsigned spu_alloc(unsigned size)
{
    for (unsigned a = SPU_HEAP0; a + size <= SPU_STREAMS0;) {
        int skip = range_used(a, size);
        if (!skip)
            return a;
        a = (a + skip + 63) & ~63u;
    }
    return 0;
}

static void spu_totals(void)
{
    unsigned used = 0;
    int n = 0;
    for (int i = 0; i < MAX_BANKS; i++)
        if (banks[i].used) {
            used += banks[i].vag_size, n++;
        }
    snd_stats.spu_used = used, snd_stats.spu_free = SPU_STREAMS0 - SPU_HEAP0 - used, snd_stats.banks = n;
}

/* ---- banks ---- */

static void voice_off(int v);
static void slot_stop(int s);

static void bank_free(Bank *b)
{
    for (int v = 0; v < SND_VOICES; v++)
        if (voices[v].on && voices[v].bank == b)
            voice_off(v);
    for (int s = 0; s < SND_STREAMS; s++)
        if (slots[s].on && slots[s].bank == b) {
            cmd(SNDC_STREAM_STOP, s, 0, 0, 0, 0, 0);
            slots[s].on = 0;
        }
    free(b->blob), free(b->cue_blob), free(b->emit_blob), free(b->ert);
    b->blob = b->cue_blob = b->emit_blob = NULL, b->ert = NULL;
    b->used = b->ready = 0;
    b->dead_frame = frame_no;
    if (b == area_bank) {  /* its emitters' voices (they may play other banks' samples) */
        for (int v = 0; v < SND_VOICES; v++)
            if (voices[v].on && voices[v].emitter >= 0)
                voice_off(v);
        for (int s = 0; s < SND_STREAMS; s++)
            if (slots[s].on && slots[s].emitter)
                slot_stop(s);
        area_bank = NULL;
    }
    spu_totals();
}

/* Evict unkept banks (enemy types gone) until size fits; 0 if it cannot */
static unsigned spu_make_room(unsigned size)
{
    unsigned a = spu_alloc(size);
    for (int i = 0; !a && i < MAX_BANKS; i++)
        if (banks[i].used && !banks[i].keep) {
            bank_free(&banks[i]);
            a = spu_alloc(size);
        }
    return a;
}

static Bank *bank_find(const char *name)
{
    for (int i = 0; i < MAX_BANKS; i++)
        if (banks[i].used && !strcmp(banks[i].name, name))
            return &banks[i];
    return NULL;
}

static Bank *bank_setup(Bank *b, const char *name, int keep, const char *path, int raw);

static Bank *bank_free_slot(void)
{
    for (int i = 0; i < MAX_BANKS; i++)
        if (!banks[i].used && (!banks[i].vag_size || frame_no > banks[i].dead_frame + 2))
            return &banks[i];
    return NULL;
}

static Bank *bank_load(const char *name, int keep)
{
    Bank *b = bank_find(name);
    if (b) {
        b->keep |= keep;
        return b;
    }
    if (!(b = bank_free_slot()))
        return NULL;
    char path[64];
    snprintf(path, sizeof path, PACK_ROOT "snd/%s.pak", name);
    Pack pk;
    if (!pack_open(&pk, path)) {
        printf("snd: no %s\n", path);
        return NULL;
    }
    memset(b, 0, sizeof *b);
    b->blob = pack_load(&pk, "bank.bin", NULL);
    b->cue_blob = pack_load(&pk, "cue.bin", NULL);
    b->emit_blob = pack_load(&pk, "emit.bin", NULL);
    int raw = pack_entry_raw(&pk, "vag.bin", &b->vag_off, &b->vag_size) &&
              pack_entry_raw(&pk, "str.bin", &b->str_off, &b->str_size);
    pack_close(&pk);
    return bank_setup(b, name, keep, path, raw);
}

/* A bank's tables (blob, cue_blob, emit_blob, vag / str offsets read into b) made usable; its VAG queued for the IOP.
 * NULL (all freed) on a bad pack. */
static Bank *bank_setup(Bank *b, const char *name, int keep, const char *path, int raw)
{
    snprintf(b->name, sizeof b->name, "%s", name);
    b->keep = keep;
    const unsigned *h = b->blob;
    if (!raw || !h || memcmp(h, "DSRB", 4) || h[1] != 2) {
        printf("snd: %s: bad pack\n", path);
        free(b->blob), free(b->cue_blob), free(b->emit_blob);
        b->blob = NULL;
        return NULL;
    }
    char cd[96];
    snprintf(b->path, sizeof b->path, "%s", pack_resolve(path, cd, sizeof cd));  /* what the IOP opens */
    b->nsmp = h[2], b->nev = h[3], b->nwav = h[4];
    b->smp = (const SSample *)(h + 8);
    b->ev = (const SEvent *)(b->smp + b->nsmp);
    b->wav = (const SWave *)(b->ev + b->nev);
    b->armor = -1;
    const unsigned *c = b->cue_blob;
    if (c && !memcmp(c, "DSRC", 4) && c[1] == 1) {
        b->ncanim = c[2];
        b->armor = (int)c[4];
        b->canim = (const SCueAnim *)(c + 8);
        b->cue = (const SCue *)(b->canim + b->ncanim);
    }
    const unsigned *e = b->emit_blob;
    if (e && !memcmp(e, "DSRE", 4) && e[1] == 1 && e[2]) {
        b->nemit = e[2];
        b->emit = (const SEmit *)(e + 4);
        b->ert = calloc(b->nemit, sizeof(EmitRt));
        for (unsigned i = 0; b->ert && i < b->nemit; i++)
            b->ert[i] = (EmitRt){.voice = -1, .stream = -1, .next = frand() * EMIT_ONESHOT_VAR};
        if (!b->ert)
            b->nemit = 0;
    }
    b->used = 1;
    if (b->vag_size <= 64)
        b->vag_size = 0, b->ready = 1;  /* nothing resident (cue-only packs) */
    else if (!(b->spu = spu_make_room(b->vag_size))) {
        printf("snd: %s: no SPU2 room for %u K (free %u K)\n", name, b->vag_size >> 10, snd_stats.spu_free >> 10);
        b->vag_size = 0, b->failed = 1;  /* tables stay: its cues and events are known, without samples */
    } else
        b->load_id = next_load_id++;
    spu_totals();
    printf("snd: %s: %u samples, %u events, %u cue anims, %u emitters, SPU %u K at %#x, streams %u K\n", name,
           b->nsmp, b->nev, b->ncanim, b->nemit, b->vag_size >> 10, b->spu, b->str_size >> 10);
    return b;
}

/* Send the oldest queued bank load once the IOP finished the last one */
static void loads_pump(void)
{
    if (load_sent && (int)(st.load_done - load_sent) < 0 && st.load_failed != (unsigned)load_sent)
        return;
    if (load_sent) {
        for (int i = 0; i < MAX_BANKS; i++)
            if (banks[i].used && banks[i].load_id == load_sent) {
                banks[i].ready = st.load_failed != (unsigned)load_sent;
                banks[i].failed = !banks[i].ready;
            }
        load_sent = 0;
    }
    Bank *next = NULL;
    for (int i = 0; i < MAX_BANKS; i++)
        if (banks[i].used && !banks[i].ready && !banks[i].failed && banks[i].load_id &&
            (!next || banks[i].load_id < next->load_id))
            next = &banks[i];
    if (!next || ops_busy)
        return;
    obuf.ld = (SndLoad){.id = next->load_id, .offset = next->vag_off, .size = next->vag_size, .spu_addr = next->spu};
    snprintf(obuf.ld.path, sizeof obuf.ld.path, "%s", next->path);
    ops_call(SNDF_LOAD, sizeof obuf.ld);
    load_sent = next->load_id;
}

/* Block until every queued bank is in SPU2 RAM (boot) */
static void loads_wait(void)
{
    pack_async_wait();  /* a background read holds the disc lock (pack.c) the IOP's load needs */
    for (int k = 0; k < 20000; k++) {
        ops_wait();
        flush(1);
        frame_wait();
        loads_pump();
        int pending = 0;
        for (int i = 0; i < MAX_BANKS; i++)
            pending |= banks[i].used && !banks[i].ready && !banks[i].failed && banks[i].load_id;
        if (!pending)
            return;
    }
    printf("snd: bank loads timed out\n");
}

static const SEvent *event_in(const Bank *b, char type, unsigned id)
{
    int lo = 0, hi = (int)b->nev - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        const SEvent *e = &b->ev[m];
        int c = e->type != type ? (e->type < type ? -1 : 1) : e->id < id ? -1 : e->id > id ? 1 : 0;
        if (!c)
            return e;
        if (c < 0)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return NULL;
}

static const SEvent *event_find(char type, unsigned id, Bank **out)
{
    for (int i = 0; i < MAX_BANKS; i++) {
        Bank *b = &banks[i];
        if (!b->used)
            continue;
        const SEvent *e = event_in(b, type, id);
        if (e && e->nwaves) {
            *out = b;
            return e;
        }
    }
    return NULL;
}

/* ---- voices ---- */

static void voice_off(int v)
{
    Voice *vo = &voices[v];
    if (!vo->on)
        return;
    cmd(SNDC_KEYOFF, v, 0, 0, 0, 0, 0);
    vo->on = 0;
    if (vo->pair >= 0 && voices[vo->pair].on && voices[vo->pair].pair == v) {
        cmd(SNDC_KEYOFF, vo->pair, 0, 0, 0, 0, 0);
        voices[vo->pair].on = 0;
    }
}

static int fx_voice(int v) { return v < SND_STREAM_VOICE(SND_STREAMS - 1) || v >= SND_STREAM_VOICE(0) + 2; }

static float voice_gain(const Voice *v)
{
    float l = v->l, r = v->r;
    return (l > r ? l : r) / VOL_MAX;
}

static int voice_alloc(void)
{
    int best = -1;
    float best_g = 2;
    for (int v = 0; v < SND_VOICES; v++) {
        if (!fx_voice(v))
            continue;
        if (!voices[v].on)
            return v;
        if (voices[v].kon_frame == frame_no)
            continue;  /* not the other channel of a sample starting now */
        /* steal: the quietest one-shot, then the oldest */
        float g = voice_gain(&voices[v]) + (voices[v].loop ? 1 : 0) + (voices[v].emitter >= 0 ? 0.5f : 0);
        if (g < best_g || (g == best_g && best >= 0 && voices[v].kon_frame < voices[best].kon_frame))
            best = v, best_g = g;
    }
    if (best >= 0)
        voice_off(best);
    return best;
}

/* Volume (0..1) and pan of a point for the listener */
static void spatial(const float p[3], float min_d, float max_d, float vol, unsigned short *l, unsigned short *r)
{
    float dx = p[0] - eye.x, dy = p[1] - eye.y, dz = p[2] - eye.z;
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (max_d < min_d + 0.5f)
        max_d = min_d + 0.5f;
    float att = d <= min_d ? 1 : d >= max_d ? 0 : (max_d - d) / (max_d - min_d);
    float x = d > 1e-3f ? (dx * right_v.x + dy * right_v.y + dz * right_v.z) / d : 0;
    float g = vol * att * VOL_MAX;
    float gl = g * (x > 0 ? 1 - x : 1), gr = g * (x < 0 ? 1 + x : 1);
    *l = gl > VOL_MAX ? VOL_MAX : (unsigned short)gl;
    *r = gr > VOL_MAX ? VOL_MAX : (unsigned short)gr;
}

static int pitch_of(int rate) { return rate * 4096 / 48000; }

static const SSample *pick_wave(const Bank *b, const SEvent *e, int *index)
{
    unsigned total = 0;
    for (int i = 0; i < e->nwaves; i++)
        total += b->wav[e->first + i].weight;
    unsigned k = total ? (unsigned)(frand() * total) : 0;
    int w = 0;
    for (; w < e->nwaves - 1; w++) {
        unsigned wt = b->wav[e->first + w].weight;
        if (k < wt)
            break;
        k -= wt;
    }
    *index = b->wav[e->first + w].sample;
    return *index < (int)b->nsmp ? &b->smp[*index] : NULL;
}

static void slot_start(int s, Bank *b, const SEvent *e, int smp, float vol, int fade, int emitter);

static int play(Bank *b, const SEvent *e, const float *pos, unsigned owner, int emitter, float gain)
{
    int si;
    const SSample *sm = pick_wave(b, e, &si);
    if (!sm || (sm->flags & SF_DROP)) {
        snd_stats.dropped++;
        return -1;
    }
    if (sm->flags & SF_STREAM) {  /* a long one-shot: the effect stream slot */
        slot_start(2, b, e, si, e->vol * gain, 0, 0);
        return -1;
    }
    if (!b->ready)
        return -1;
    /* max playbacks: the oldest of this event makes room */
    if (e->maxpb) {
        int n = 0, oldest = -1;
        for (int v = 0; v < SND_VOICES; v++)
            if (voices[v].on && voices[v].chan == 0 && voices[v].id == e->id && voices[v].type == e->type) {
                n++;
                if (oldest < 0 || voices[v].kon_frame < voices[oldest].kon_frame)
                    oldest = v;
            }
        if (n >= e->maxpb && oldest >= 0)
            voice_off(oldest);
    }
    float vol = e->vol * gain;
    if (e->vol_rand > 0)  /* ASSUMED: FMOD Designer's volume randomisation in dB, down from the event volume */
        vol *= powf(10, -frand() * e->vol_rand / 20);
    int is3d = pos && (e->flags & EF_3D);
    int first = -1;
    for (int c = 0; c < sm->ch && c < 2; c++) {
        int v = voice_alloc();
        if (v < 0)
            break;
        Voice *vo = &voices[v];
        memset(vo, 0, sizeof *vo);
        vo->on = 1, vo->chan = c, vo->stereo = sm->ch > 1, vo->pair = -1, vo->loop = (e->flags & EF_LOOP) != 0;
        vo->is3d = is3d, vo->emitter = emitter, vo->slot = -1, vo->bank = b, vo->type = e->type, vo->id = e->id;
        vo->owner = owner, vo->kon_frame = frame_no, vo->kon_seq = rpc_seq + 1, vo->min_d = e->min_d, vo->max_d = e->max_d, vo->vol = vol;
        if (pos)
            memcpy(vo->pos, pos, 12);
        float secs = (sm->bytes / sm->ch) / 16 * 28 / (float)sm->rate;
        vo->left = vo->loop ? 1e9f : secs + 0.1f;
        if (is3d)
            spatial(vo->pos, vo->min_d, vo->max_d, vol, &vo->l, &vo->r);
        else
            vo->l = vo->r = vol > 1 ? VOL_MAX : (unsigned short)(vol * VOL_MAX);
        if (vo->stereo)  /* stereo samples: channel 0 left, 1 right */
            c == 0 ? (vo->r = 0) : (vo->l = 0);
        cmd(SNDC_KEYON, v, b->spu + sm->off + c * (sm->bytes / sm->ch), pitch_of(sm->rate), vo->l, vo->r, 8);
        if (first < 0)
            first = v;
        else
            voices[first].pair = v, vo->pair = first;
    }
    if (first >= 0)
        snd_stats.played++;
    return first;
}

int snd_play(int type, int id, const float pos[3], unsigned owner)
{
    if (!on || type < 0 || type > SND_G)
        return 0;
    char t = "acfopsmvxbg"[type];
    if (t == 'x' || t == 'b')  /* EMEVD with a material type: the default (id itself, material 1) */
        t = 'c';
    Bank *b;
    const SEvent *e = event_find(t, (unsigned)id, &b);
    if (!e) {
        snd_stats.missing++;
        return 0;
    }
    float p[3];
    if (pos)
        p[0] = pos[0], p[1] = pos[1], p[2] = pos[2];
    return play(b, e, pos ? p : NULL, owner, -1, 1) >= 0;
}

void snd_stop_owner(unsigned owner)
{
    if (!on)
        return;
    for (int v = 0; v < SND_VOICES; v++)
        if (voices[v].on && voices[v].owner == owner)
            voice_off(v);
}

/* ---- streams ---- */

static void slot_start(int s, Bank *b, const SEvent *e, int smp, float vol, int fade, int emitter)
{
    Slot *sl = &slots[s];
    if (sl->on && !sl->stopping && sl->bank == b && sl->smp == smp) {
        sl->vol = vol;
        return;
    }
    const SSample *sm = &b->smp[smp];
    if (sm->chunk != STREAM_CHUNK || sm->ch < 1 || sm->ch > 2 || ops_busy)
        return;  /* busy: tried again next frame */
    *sl = (Slot){.on = 1, .smp = smp, .bank = b, .type = e->type, .id = e->id, .vol = vol, .cur = fade ? 0 : vol,
                 .open_frame = frame_no, .emitter = emitter};
    unsigned short v0 = (unsigned short)(sl->cur * VOL_MAX);
    sl->l = sl->r = v0;
    obuf.so = (SndStreamOpen){.slot = s, .offset = b->str_off + sm->off, .size = sm->bytes,
                              .loop = (e->flags & EF_LOOP) != 0, .chunk = sm->chunk,
                              .spu_buf = SPU_STREAMS0 + s * 2 * 2 * SND_STREAM_CHUNK_MAX, .channels = sm->ch,
                              .pitch = pitch_of(sm->rate), .voll = v0, .volr = v0};
    snprintf(obuf.so.path, sizeof obuf.so.path, "%s", b->path);
    ops_call(SNDF_STREAM, sizeof obuf.so);
    printf("snd: stream %d: %c%09u (%u K, %u ch, %s)\n", s, e->type, e->id, sm->bytes >> 10, sm->ch,
           (e->flags & EF_LOOP) ? "loop" : "once");
}

static void slot_stop(int s)
{
    if (slots[s].on)
        slots[s].stopping = 1, slots[s].vol = 0;
}

static void slots_update(float dt)
{
    int n = 0;
    for (int s = 0; s < SND_STREAMS; s++) {
        Slot *sl = &slots[s];
        if (!sl->on)
            continue;
        /* the IOP ended it (non-looping data done, or a read error) */
        if (frame_no > sl->open_frame + 30 &&  /* the open has reached the IOP by then */
            (st.stream_state[s] == SNDS_IDLE || st.stream_state[s] == SNDS_ERROR)) {
            sl->on = 0;
            continue;
        }
        float step = dt / MUSIC_FADE;
        sl->cur += sl->vol > sl->cur ? fminf(step, sl->vol - sl->cur) : -fminf(step, sl->cur - sl->vol);
        if (sl->stopping && sl->cur <= 0) {
            cmd(SNDC_STREAM_STOP, s, 0, 0, 0, 0, 0);
            sl->on = 0;
            continue;
        }
        unsigned short v = (unsigned short)(fminf(sl->cur, 1) * VOL_MAX);
        if (v != sl->l) {
            cmd(SNDC_STREAM_VOL, s, 0, 0, v, v, 0);
            sl->l = sl->r = v;
        }
        n++;
    }
    snd_stats.streams = n;
    snd_stats.music = slots[0].on ? (int)slots[0].id : 0;
    snd_stats.ambience = slots[1].on ? (int)slots[1].id : 0;
}

/* ---- MSB emitters ---- */

static int ent_on(int entity)
{
    for (int i = 0; i < nents; i++)
        if (ents[i].entity == entity)
            return ents[i].on;
    return 1;  /* on unless an event switched it off */
}

int snd_entity(int entity, int state)
{
    if (!on)
        return 0;
    int i = 0;
    while (i < nents && ents[i].entity != entity)
        i++;
    if (i == nents && nents < MAX_ENTS)
        ents[nents++].entity = entity;
    if (i < MAX_ENTS)
        ents[i].on = state;
    int found = 0;
    for (int k = 0; k < MAX_BANKS; k++)
        for (unsigned j = 0; banks[k].used && j < banks[k].nemit; j++)
            found |= banks[k].emit[j].entity == entity;
    return found || !area_bank;
}

/* Nearest point of an emitter's region to the listener (MSB1 regions: boxes / cylinders stand on their position) */
static void emit_point(const SEmit *m, float out[3])
{
    float dx = eye.x - m->pos[0], dy = eye.y - m->pos[1], dz = eye.z - m->pos[2];
    float c = cosf(m->rot), s = sinf(m->rot);
    float lx = c * dx - s * dz, lz = s * dx + c * dz, ly = dy;  /* into the region's frame (yaw only) */
    switch (m->shape) {
    case 2: {  /* sphere */
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        float k = d > m->dim[0] ? m->dim[0] / d : 1;
        lx *= k, ly *= k, lz *= k;
        break;
    }
    case 1:
    case 3: {  /* circle / cylinder */
        float d = sqrtf(lx * lx + lz * lz);
        float k = d > m->dim[0] ? m->dim[0] / d : 1;
        lx *= k, lz *= k;
        ly = m->shape == 3 ? fmaxf(0, fminf(ly, m->dim[2])) : 0;
        break;
    }
    case 4:
    case 5:  /* rectangle / box */
        lx = fmaxf(-m->dim[0] / 2, fminf(lx, m->dim[0] / 2));
        lz = fmaxf(-m->dim[1] / 2, fminf(lz, m->dim[1] / 2));
        ly = m->shape == 5 ? fmaxf(0, fminf(ly, m->dim[2])) : 0;
        break;
    default:
        lx = ly = lz = 0;
    }
    out[0] = m->pos[0] + c * lx + s * lz;
    out[1] = m->pos[1] + ly;
    out[2] = m->pos[2] - s * lx + c * lz;
}

static void emitters_update(float dt)
{
    Bank *b = area_bank;
    float best_m = 0, best_a = 0;
    const SEvent *em = NULL, *ea = NULL;
    int sm_m = -1, sm_a = -1;
    for (unsigned i = 0; b && i < b->nemit; i++) {
        const SEmit *m = &b->emit[i];
        EmitRt *rt = &b->ert[i];
        Bank *eb;
        const SEvent *e = m->type <= SND_G ? event_find("acfopsmvxbg"[m->type], (unsigned)m->id, &eb) : NULL;
        float p[3];
        emit_point(m, p);
        float dx = p[0] - eye.x, dy = p[1] - eye.y, dz = p[2] - eye.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        int live = e && ent_on(m->entity) && d < e->max_d;
        if (!live) {
            if (rt->voice >= 0 && voices[rt->voice].on && voices[rt->voice].emitter == (int)i)
                voice_off(rt->voice);
            rt->voice = -1;
            continue;
        }
        int si;
        const SSample *sm = pick_wave(eb, e, &si);
        if (!sm || (sm->flags & SF_DROP))
            continue;
        float att = d <= e->min_d ? 1 : (e->max_d - d) / fmaxf(0.5f, e->max_d - e->min_d);
        if (sm->flags & SF_STREAM) {  /* music regions, long ambience: the loudest one per slot */
            if (m->type == SND_M || e->cat == 1) {
                if (att > best_m)
                    best_m = att, em = e, sm_m = si;
            } else if (att > best_a)
                best_a = att, ea = e, sm_a = si;
            continue;
        }
        if (rt->voice >= 0 && voices[rt->voice].on && voices[rt->voice].emitter == (int)i) {
            memcpy(voices[rt->voice].pos, p, 12);  /* the nearest point follows the listener */
            continue;
        }
        rt->voice = -1;
        if (!(e->flags & EF_LOOP) && (rt->next -= dt) > 0)
            continue;
        rt->next = EMIT_ONESHOT_MIN + frand() * EMIT_ONESHOT_VAR;
        int v = play(eb, e, p, 0, (int)i, 1);
        if (v >= 0 && (e->flags & EF_LOOP))
            rt->voice = v;
    }
    /* stream slots 0 (music) and 2 (long emitter loops): the loudest region each; its volume follows the distance */
    for (int k = 0; k < 2; k++) {
        int s = k ? 2 : 0, si = k ? sm_a : sm_m;
        const SEvent *e = k ? ea : em;
        float att = k ? best_a : best_m;
        Slot *sl = &slots[s];
        if (sl->on && !sl->emitter)
            continue;  /* a long one-shot (snd_play) has the slot until it ends */
        if (!e) {
            if (sl->on)
                slot_stop(s);
            continue;
        }
        Bank *eb;
        event_find(e->type, e->id, &eb);
        if (!sl->on)
            slot_start(s, eb, e, si, e->vol * att, 1, 1);
        else if (sl->id != e->id || sl->type != e->type)
            slot_stop(s);  /* another region took over: fade the old one out first */
        else if (!sl->stopping)
            sl->vol = e->vol * att;
    }
}

/* ---- TAE cues ---- */

static int owner_index(unsigned owner)
{
    if (owner == SND_OWNER_PLAYER)
        return 0;
    unsigned i = owner - 0x100u;
    return i < 64 ? 1 + (int)i : -1;
}

static const SCueAnim *cue_anim(const Bank *b, int id)
{
    int lo = 0, hi = (int)b->ncanim - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (b->canim[m].id == id)
            return &b->canim[m];
        if (b->canim[m].id < id)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return NULL;
}

static int floor_material(const float pos[3])
{
    ColHit h;
    if (!col_floor(pos[0], pos[2], pos[1] + 0.5f, pos[1] - 1.5f, COL_SOLID, &h))
        return 1;
    return h.material % 100;  /* ASSUMED: materials >= 100 (101, 102, 201, 301, ...) play their last two digits */
}

static void cue_fire(const Bank *cb, const SCue *c, unsigned owner, int anim, const float pos[3])
{
    char t = "acfopsmvxbg"[c->type < 0 || c->type > SND_G ? 1 : c->type];
    unsigned id = (unsigned)c->id;
    Bank *b;
    const SEvent *e = NULL;
    if (t == 'x' || t == 'b') {  /* material variants: c(id / 100 * 100 + material), else the id itself */
        int mat = t == 'x' ? floor_material(pos) : cb->armor;
        if (mat >= 0)
            e = event_find('c', id / 100 * 100 + (unsigned)mat, &b);
        if (!e)
            e = event_find('c', id, &b);
    } else
        e = event_find(t, id, &b);
    if (!e) {
        snd_stats.missing++;
        return;
    }
    float p[3] = {pos[0], pos[1] + BODY_Y, pos[2]};
    int v = play(b, e, p, owner, -1, 1);
    if (v >= 0 && c->slot >= 0 && (e->flags & EF_LOOP)) {  /* 129 / 130 slot loops end with their window */
        voices[v].slot = c->slot, voices[v].slot_anim = anim, voices[v].slot_end = c->t1;
        if (voices[v].pair >= 0)
            voices[voices[v].pair].slot = -1;
    }
}

void snd_anim(unsigned owner, int model, int anim, float t1, const float pos[3])
{
    int oi = owner_index(owner);
    if (!on || oi < 0)
        return;
    float t0 = owners[oi].t;
    if (owners[oi].owner != owner || owners[oi].anim != anim)
        t0 = -1;  /* a new anim: its cues at 0 play too */
    owners[oi].owner = owner, owners[oi].anim = anim, owners[oi].t = t1;
    /* slot sounds of this owner: their window ended, or another anim plays */
    for (int v = 0; v < SND_VOICES; v++)
        if (voices[v].on && voices[v].owner == owner && voices[v].slot >= 0 &&
            (voices[v].slot_anim != anim || t1 >= voices[v].slot_end || t1 < t0))
            voice_off(v);
    if (anim < 0)
        return;
    char name[8];
    snprintf(name, sizeof name, model ? "c%04d" : "main", model);
    const Bank *cb = model ? bank_find(name) : main_bank;
    if (!cb || !cb->ncanim)
        return;
    const SCueAnim *a = cue_anim(cb, anim);
    if (!a && anim >= 10000)
        a = cue_anim(cb, anim % 10000);
    if (!a)
        return;
    for (int i = 0; i < a->n; i++) {
        const SCue *c = &cb->cue[a->first + i];
        float s = c->t0;
        int hit = t1 >= t0 ? (s > t0 && s <= t1) : (s > t0 || s <= t1);  /* t1 < t0: the loop wrapped */
        if (hit)
            cue_fire(cb, c, owner, anim, pos);
    }
}

/* ---- areas, enemy banks ---- */

static struct { Bank *b; const SEvent *e; int si; } amb;  /* area ambience waiting for the ops client */

void snd_map(const char *map_id)
{
    if (!on || !map_id || map_id[0] != 'm')
        return;
    int aa = atoi(map_id + 1), bb = atoi(map_id + 4);
    if (aa == area_aa && bb == area_bb)
        return;
    int first = area_aa < 0;
    if (aa != area_aa) {
        if (area_bank)
            bank_free(area_bank);
        char name[8];
        snprintf(name, sizeof name, "m%02d", aa);
        area_bank = bank_load(name, 1);
        if (first)
            loads_wait();
    }
    area_aa = aa, area_bb = bb;
    /* area ambience (frpg_xmAA, 5.1 downmixed): y + AA BB 00000, on stream slot 1 */
    Bank *b;
    const SEvent *e = event_find('y', (unsigned)(aa * 10000000 + bb * 100000), &b);
    int si;
    const SSample *sm = e ? pick_wave(b, e, &si) : NULL;
    amb.e = NULL;
    if (sm && (sm->flags & SF_STREAM)) {
        /* boot: started by the first snd_update, after the load (a stream reading the disc during it made every
         * cdrom0: pack read seek away and back); later: once the ops client is free (never wait in a frame) */
        if (first || ops_busy)
            amb.b = b, amb.e = e, amb.si = si;
        else
            slot_start(1, b, e, si, e->vol, !first, 0);
    } else
        slot_stop(1);
    printf("snd: map %s, area bank %s, ambience %s\n", map_id, area_bank ? area_bank->name : "-", sm ? "on" : "none");
}

/* Enemy types' banks load in the background, one at a time through pack.c's job slot (and its disc lock), the tables
 * in three jobs (bank.bin, then cue.bin, emit.bin; the first also finds vag.bin / str.bin); then the VAG goes to the
 * IOP as every bank's does. Loaded synchronously a type's bank took 0.4-0.6 s of one frame from cdrom0: (a pack open
 * and three loads, ISO 2026-11-04, TECH_DEBT load-6). Until it is in, the type's sounds do not play. */
#define CHR_QUEUE 16
static char chr_queue[CHR_QUEUE][8];
static int chr_nq;
static struct { char name[8]; int stage; PackJob job; void *part[3]; unsigned xoff[2], xsize[2]; } chr_ld;
static const char *const chr_part[3] = {"bank.bin", "cue.bin", "emit.bin"};
static void *job_heap_get(unsigned n) { return memalign(128, n); }
static void job_heap_put(void *p, unsigned n) { (void)n; free(p); }

static void chr_path(char *path, unsigned cap, const char *name) { snprintf(path, cap, PACK_ROOT "snd/%s.pak", name); }

static void chr_part_start(void)
{
    char path[64];
    chr_path(path, sizeof path, chr_ld.name);
    pack_job_start(&chr_ld.job, path, chr_part[chr_ld.stage], job_heap_get, job_heap_put);
    if (chr_ld.stage == 0)
        chr_ld.job.xname[0] = "vag.bin", chr_ld.job.xname[1] = "str.bin";
}

static void chr_drop(void)
{
    if (chr_ld.name[0])
        pack_job_cancel(&chr_ld.job);
    for (int k = 0; k < 3; k++)
        free(chr_ld.part[k]);
    memset(chr_ld.part, 0, sizeof chr_ld.part);
    chr_ld.name[0] = 0;
}

static void chr_pump(void)
{
    if (!chr_ld.name[0]) {
        if (!chr_nq)
            return;
        memset(&chr_ld, 0, sizeof chr_ld);
        snprintf(chr_ld.name, sizeof chr_ld.name, "%s", chr_queue[0]);
        memmove(chr_queue[0], chr_queue[1], --chr_nq * sizeof chr_queue[0]);
        chr_part_start();
    }
    int r = pack_job_step(&chr_ld.job);
    if (!r)
        return;
    if (r < 0 && chr_ld.stage == 0) {  /* no bank.bin: no pack for this model */
        chr_ld.name[0] = 0;
        return;
    }
    if (r > 0)
        chr_ld.part[chr_ld.stage] = chr_ld.job.out;
    if (chr_ld.stage == 0)
        memcpy(chr_ld.xoff, chr_ld.job.xoff, sizeof chr_ld.xoff), memcpy(chr_ld.xsize, chr_ld.job.xsize, sizeof chr_ld.xsize);
    if (++chr_ld.stage < 3) {
        chr_part_start();
        return;
    }
    Bank *b = bank_find(chr_ld.name) ? NULL : bank_free_slot();
    if (!b) {
        for (int k = 0; k < 3; k++)
            free(chr_ld.part[k]);
    } else {
        memset(b, 0, sizeof *b);
        b->blob = chr_ld.part[0], b->cue_blob = chr_ld.part[1], b->emit_blob = chr_ld.part[2];
        b->vag_off = chr_ld.xoff[0], b->vag_size = chr_ld.xsize[0], b->str_off = chr_ld.xoff[1], b->str_size = chr_ld.xsize[1];
        char path[64];
        chr_path(path, sizeof path, chr_ld.name);
        bank_setup(b, chr_ld.name, 0, path, chr_ld.xoff[0] && chr_ld.xoff[1]);  /* both raw, as pack_entry_raw */
    }
    memset(chr_ld.part, 0, sizeof chr_ld.part);
    chr_ld.name[0] = 0;
}

void snd_chr_load(const char *model)
{
    if (!on || !model || model[0] != 'c' || bank_find(model) || !strcmp(chr_ld.name, model))
        return;
    for (int i = 0; i < chr_nq; i++)
        if (!strcmp(chr_queue[i], model))
            return;
    if (chr_nq < CHR_QUEUE)
        snprintf(chr_queue[chr_nq++], sizeof chr_queue[0], "%s", model);
}

void snd_chr_unload(const char *model)
{
    if (!on)
        return;
    for (int i = 0; i < chr_nq; i++)
        if (!strcmp(chr_queue[i], model)) {
            memmove(chr_queue[i], chr_queue[i + 1], (chr_nq - i - 1) * sizeof chr_queue[0]);
            chr_nq--;
            return;
        }
    if (!strcmp(chr_ld.name, model)) {
        chr_drop();
        return;
    }
    Bank *b = bank_find(model);
    if (b && !b->keep)
        bank_free(b);
}

/* ---- frame ---- */

void snd_update(float dt)
{
    if (!on)
        return;
    frame_no++;
    V3 up, fwd;
    float f0, f1, f2;
    r_camera_basis(&eye, &right_v, &up, &fwd, &f0, &f1, &f2);
    if (frame_busy && !SifCheckStatRpc(&cl_frame))
        frame_wait();
    if (ops_busy && !SifCheckStatRpc(&cl_ops))
        ops_busy = 0;
    if (amb.e && !ops_busy && amb.b->used) {
        slot_start(1, amb.b, amb.e, amb.si, amb.e->vol, 1, 0);
        amb.e = NULL;
    }
    loads_pump();
    disc_collect(0);
    /* a try's lock nobody claimed: back to dsrsnd. Its job asks again before snd_update (world_update, enemy_update
     * run first), so a lock collected in an earlier frame and still here was not wanted this frame */
    if (disc_req == 2 && frame_no > disc_req_frame)
        disc_unlock();
    if (st.late != snd_stats.late || st.underruns != snd_stats.underruns) {
        printf("snd: stream refills late %u, underruns %u\n", st.late, st.underruns);
        snd_stats.late = st.late, snd_stats.underruns = st.underruns;
    }
    chr_pump();

    /* voices that ended: ENDX from the last finished frame RPC, or past their length */
    int n = 0;
    for (int v = 0; v < SND_VOICES; v++) {
        Voice *vo = &voices[v];
        if (!vo->on)
            continue;
        vo->left -= dt;
        int endx = st.endx[v / 24] >> (v % 24) & 1;
        if ((!vo->loop && endx && vo->kon_seq <= st_seq) || vo->left < -1) {
            vo->on = 0;
            continue;
        }
        n++;
    }
    emitters_update(dt);
    for (int v = 0; v < SND_VOICES; v++) {  /* 3D volumes */
        Voice *vo = &voices[v];
        if (!vo->on || !vo->is3d || vo->kon_frame == frame_no)
            continue;
        unsigned short l, r;
        spatial(vo->pos, vo->min_d, vo->max_d, vo->vol, &l, &r);
        if (vo->stereo)
            vo->chan == 0 ? (r = 0) : (l = 0);
        if (abs((int)l - vo->l) > 64 || abs((int)r - vo->r) > 64 || (!l && vo->l) || (!r && vo->r)) {
            vo->l = l, vo->r = r;
            cmd(SNDC_VOL, v, 0, 0, l, r, 0);
        }
    }
    slots_update(dt);
    snd_stats.voices = n;
    snd_stats.errors = st.errors;
    flush(0);
}

/* ---- boot ---- */

int snd_init(void)
{
    int ret = 0;
    SifInitIopHeap();
    SifLoadFileInit();
    sbv_patch_enable_lmb();  /* LoadModuleBuffer on older BIOSes */
    int id = SifExecModuleBuffer(libsd_irx, size_libsd_irx, 0, NULL, &ret);
    if (id < 0 || ret == 1) {
        printf("snd: libsd.irx failed (%d, %d)\n", id, ret);
        return 0;
    }
    id = SifExecModuleBuffer(dsrsnd_irx, size_dsrsnd_irx, 0, NULL, &ret);
    if (id < 0 || ret == 1) {
        printf("snd: dsrsnd.irx failed (%d, %d)\n", id, ret);
        return 0;
    }
    static const unsigned ids[3] = {DSRSND_RPC_ID, DSRSND_RPC_OPS_ID, DSRSND_RPC_DISC_ID};
    for (int k = 0; k < 3; k++) {
        SifRpcClientData_t *c = k == 2 ? &cl_disc : k ? &cl_ops : &cl_frame;
        for (int tries = 0; tries < 1000; tries++) {
            if (SifBindRpc(c, ids[k], 0) < 0)
                return 0;
            if (c->server)
                break;
            nopdelay();
        }
        if (!c->server) {
            printf("snd: no RPC server\n");
            return 0;
        }
    }
    SifCallRpc(&cl_frame, SNDF_INIT, 0, NULL, 0, &fstat, sizeof fstat, NULL, NULL);
    memcpy(&st, (void *)((unsigned)&fstat | 0x20000000), sizeof st);
    if (!st.ok)
        return 0;
    on = 1;
    pack_disc_lock = disc_lock, pack_disc_unlock = disc_unlock, pack_disc_try_async = disc_try_async;
    pack_disc_yield = disc_yield;
    for (int v = 0; v < SND_VOICES; v++)
        voices[v].pair = -1, voices[v].emitter = -1, voices[v].slot = -1;
    /* master -5 dB (ASSUMED headroom): music + combat at full master clipped 751 samples in a 58 s Burg run */
    cmd(SNDC_MASTER, 0, 0, 0, MASTER_VOL, MASTER_VOL, 0);
    main_bank = bank_load("main", 1);
    loads_wait();
    snd_stats.on = 1;
    printf("snd: on, SPU2 heap %u K used, %u K free\n", snd_stats.spu_used >> 10, snd_stats.spu_free >> 10);
    return 1;
}
