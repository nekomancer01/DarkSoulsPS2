/* Protocol between the EE sound runtime (src/sound.c) and its IOP module (iop/dsrsnd/dsrsnd.c, dsrsnd.irx).
 * The EE owns every decision (SPU2 memory map, voice allocation, 3D volumes); the IOP only applies them through libsd
 * and keeps the two disc streams fed.
 *   SNDF_INIT    (no args) -> SndStatus: libsd cold init, both cores unmuted, no reverb
 *   SNDF_LOAD    SndLoad -> SndStatus: the IOP reads a byte range of a file into SPU2 RAM in the background (between
 *                stream refills, SND_LOAD_STEP bytes at a time); status.load_done = its id once it is in
 *   SNDF_FRAME   SndFrame -> SndStatus: this frame's voice commands, key offs before key ons
 *   SNDF_STREAM  SndStreamOpen -> SndStatus: (re)start a disc stream in a slot
 * Voices: 0-23 core 0, 24-47 core 1. Stream slot s plays on core 1 voices SND_STREAM_VOICE(s) (left) and + 1 (right).
 * Streamed data (tools/export_audio.py str.bin): SPU2 ADPCM in chunks of `chunk` bytes per channel, channels
 * interleaved by chunk ([L chunk][R chunk][L chunk]...); the IOP rewrites the block flags for its ring buffer. */
#ifndef DSRSND_RPC_H
#define DSRSND_RPC_H

#define DSRSND_RPC_ID 0x0D5A5D01
#define SNDF_INIT 1
#define SNDF_LOAD 2
#define SNDF_FRAME 3
#define SNDF_STREAM 4

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

typedef struct {
    unsigned int endx[2];        /* ENDX switch of core 0 / core 1: voices that reached an end block */
    unsigned int stream_state[SND_STREAMS];
    unsigned int stream_bytes[SND_STREAMS];  /* bytes of the stream read so far (all channels) */
    unsigned int errors, ok;     /* failed reads / opens since boot; 1 once SNDF_INIT ran */
    unsigned int load_done, load_failed;  /* id of the last finished / failed SNDF_LOAD */
    unsigned int pad[2];
} SndStatus;  /* 64 bytes */

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
