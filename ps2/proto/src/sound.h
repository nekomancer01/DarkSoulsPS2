/* Sound (A12): DSR's FMOD events re-encoded for the SPU2 (tools/export_audio.py: data/snd/<name>.pak), played by an IOP
 * module of our own (iop/dsrsnd, protocol src/dsrsnd_rpc.h). Sound effects are resident SPU2 ADPCM on 42 voices with
 * 3D volume / pan from the camera; music, area ambience and long emitter loops stream from disc on 3 stereo slots.
 * What plays them: TAE sound events of the player and enemies (snd_anim), MSB sound emitters (music regions, wind,
 * water) switched by EMEVD 2010[03], EMEVD PlaySE 2010[02], and the area's 5.1 ambience per map. Every call is a no-op
 * when the IOP module did not load. */
#ifndef SOUND_H
#define SOUND_H

/* Sound type numbers (TAE / MSB / EMEVD), event letters "acfopsmvxbg" */
enum { SND_A, SND_C, SND_F, SND_O, SND_P, SND_S, SND_M, SND_V, SND_X, SND_B, SND_G };

/* Owners: who a sound belongs to (cue slots, stopping a character's sounds) */
#define SND_OWNER_PLAYER 1u
#define SND_OWNER_ENEMY(i) (0x100u + (unsigned)(i))

/* Boot: load the IOP modules, init the SPU2, load data/snd/main.pak (the player's sounds) into SPU2 RAM. 0 = no sound */
int snd_init(void);
/* The map the player is in (mAA_BB_CC_DD): loads the area pack when AA changes (its banks, emitters), switches the
 * area ambience. Waits for the area's samples on the first call (boot), not later. */
void snd_map(const char *map_id);
/* An enemy type loaded / unloaded (enemy.c type streaming): its bank (data/snd/cXXXX.pak) follows while SPU2 RAM has
 * room. model = "c2250". */
void snd_chr_load(const char *model);
void snd_chr_unload(const char *model);
/* Once a frame per character: the anim playing (TAE id, -1 none) and its time; the cues between the owner's last call
 * and t play (a new anim from its start, a looping one across the wrap). model 0 = the player (main.pak cues), else
 * the enemy model number (2250). pos = feet. */
void snd_anim(unsigned owner, int model, int anim, float t, const float pos[3]);
/* Play an event once: pos NULL = 2D (at the listener). Returns 0 if it does not exist or has no samples loaded. */
int snd_play(int type, int id, const float pos[3], unsigned owner);
/* Stop every sound of an owner (a character unloaded / dead) */
void snd_stop_owner(unsigned owner);
/* EMEVD 2010[03]: MSB sound emitter by entity id on / off; 0 if no emitter has that id */
int snd_entity(int entity, int on);
/* Once a frame after the camera is set: listener, emitters, voice volumes, streams, then one RPC to the IOP */
void snd_update(float dt);

typedef struct {
    int on, voices, streams, banks;     /* module up, voices playing, stream slots playing, banks loaded */
    unsigned spu_used, spu_free;        /* SPU2 sample heap bytes */
    unsigned played, missing, dropped;  /* events started; not found; found but without a sample (dropped tier) */
    unsigned errors;                    /* IOP read / open errors */
    unsigned late, underruns;           /* disc stream refills late / streams muted for want of data (dsrsnd) */
    int music, ambience;                /* event id on the music / ambience slot, 0 none */
} SndStats;
extern SndStats snd_stats;

#endif
