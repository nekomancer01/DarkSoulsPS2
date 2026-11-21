/* dsrsnd.irx: IOP side of the sound runtime and the EE's disc server (EE side src/sound.c and src/pack.c, protocol
 * src/dsrsnd_rpc.h). Three parts:
 * - snd.c, sound: one RPC thread applies the EE's commands through libsd (uploads on SPU2 DMA channel 0, voice
 *   registers); one stream thread keeps up to SND_STREAMS disc streams fed: every few ms it reads each stream voice's
 *   next address (NAX), and once the voice has moved into the other half of its 2 x chunk ring, it reads the next chunk
 *   of every channel from the file (ioman: host: under PCSX2, cdrom0: from a disc) and DMAs it over the half just
 *   played (channel 1). Block flags are rewritten for the ring: the first block of half 0 sets the loop start, the last
 *   block of half 1 jumps back to it. The same thread loads sample banks (SNDF_LOAD): SND_LOAD_STEP bytes from the file
 *   into SPU2 RAM per step, so the EE spends neither RAM nor time on them. libsd calls of both threads go through one
 *   semaphore.
 * - disc.c, the drive: the EE's disc server (dsrsnd_rpc.h SNDF_DISC_*): a disc thread, below every other one, reads
 *   the EE's requests one SND_DISC_PAGE at a time. The two disc threads take turns at the drive through one lock (the
 *   BIOS CD file system takes one reader at a time); the stream thread never waits for it: when the drive is busy it
 *   asks for it and the disc thread leaves it after its current page.
 * - this file: the RPC servers and the module start. */
#include "irx_imports.h"
#include "dsrsnd_rpc.h"
#include "dsrsnd_i.h"

IRX_ID("dsrsnd", 1, 0);

/* Two servers, one per EE client (frames / ops), each with its own thread and receive buffer: a client DMAs its
 * arguments straight into the server's buffer, so two calls in flight on one server overwrote each other (the ops
 * call never got served: a hang walking into the Burg, 2026-11-03) */
static SifRpcDataQueue_t queue[3];
static SifRpcServerData_t server[3];
/* each server's receive buffer holds the largest call it takes (until 2026-11-21 all three were a SndFrame's size, in
 * words where bytes were meant: 4.7 KB for ~2.8 KB used) */
#define RPC_UP(n) (((n) + 63) & ~63)
#define RPC_OPS_BYTES (sizeof(SndLoad) > sizeof(SndStreamOpen) ? sizeof(SndLoad) : sizeof(SndStreamOpen))
#define RPC_DISC_BYTES (SND_DISC_BATCH * sizeof(SndDiscReq) > sizeof(SndDiscInit) ? SND_DISC_BATCH * sizeof(SndDiscReq) \
                                                                                : sizeof(SndDiscInit))
static unsigned char rpc_frame_buf[RPC_UP(sizeof(SndFrame))] __attribute__((aligned(64)));
static unsigned char rpc_ops_buf[RPC_UP(RPC_OPS_BYTES)] __attribute__((aligned(64)));
static unsigned char rpc_disc_buf[RPC_UP(RPC_DISC_BYTES)] __attribute__((aligned(64)));
static void *const rpc_buf[3] = {rpc_frame_buf, rpc_ops_buf, rpc_disc_buf};

static void *rpc_server(int fno, void *data, int size)
{
    switch (fno) {
    case SNDF_INIT:
        snd_init();
        break;
    case SNDF_LOAD:
        snd_load_post(data);
        break;
    case SNDF_FRAME:
        snd_frame(data);
        break;
    case SNDF_STREAM:
        snd_stream_open(data);
        break;
    case SNDF_DISC_INIT:
        disc_set_done(((const SndDiscInit *)data)->done);
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
    int sems = snd_init_semas();
    sems = disc_init_semas() && sems;
    dstat.iop_free_boot = QueryTotalFreeMemSize();
    if (!snd_init_mem() || !sems)
        return MODULE_NO_RESIDENT_END;
    if (!disc_init_bufs())  /* the disc pages before the read-ahead: the EE's reads need one, the read-ahead can do without */
        return MODULE_NO_RESIDENT_END;
    snd_init_ra();
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
    disc_init_cache();
    dstat.iop_free = QueryTotalFreeMemSize();
    printf("dsrsnd: ready (%d streams, %d disc pages of %u KB, page cache %u KB); IOP RAM free %u KB before dsrsnd's "
           "buffers, %u KB after\n", SND_STREAMS, disc_pages(), SND_DISC_PAGE >> 10, dstat.cache_kb,
           dstat.iop_free_boot >> 10, dstat.iop_free >> 10);
    return MODULE_RESIDENT_END;
}
