/* Skinned characters from model.bin (tools/export_model.py: the Warrior; tools/export_chr.py: enemies), any number of
 * models, each drawn any number of times per frame with its own pose (VU1 skinning, vu1skin2.vsm). */
#ifndef MODEL_H
#define MODEL_H

#include "render.h"

#define MAX_BONES 64

typedef struct { float q[4], t[3]; } BoneLocal;
typedef struct Model Model;

/* blob: model.bin (128-byte aligned, kept while the model is used: textures and anims are read in place).
 * Builds the VU1 skin streams (heap). NULL on a bad blob or more than 61 bones (VU1 palette room). */
Model *mdl_load(const void *blob);
/* The same in steps (enemy.c streams types in during play): begin parses and allocates, step builds the VU1 skin
 * batches until `budget` EE ticks have passed and returns 1 once all are built (only then use the model) */
Model *mdl_load_begin(const void *blob);
int mdl_load_step(Model *m, unsigned budget);
/* Upload the textures; returns free VRAM bytes afterwards, or -1 if they did not fit */
int mdl_upload(Model *m, GSGLOBAL *gs);
int mdl_bones(const Model *m);
/* Parent bone of each bone (-1 = a root), in skeleton order: parents come first */
const short *mdl_parents(const Model *m);
/* After mdl_load + mdl_upload: keep only header / parents / ref / groups / anims in an own block so the blob (of
 * blob_size bytes, anims last as export_chr.py writes them) can be freed; 0 if out of memory */
int mdl_compact(Model *m, unsigned blob_size);
/* mdl_upload for a compacted model (mdl_compact): its textures from blob, the model.bin it was loaded from */
int mdl_upload_from(Model *m, const void *blob, GSGLOBAL *gs);
/* Give the model's VRAM textures back (render.c r_vram_free_*, between frames) once nothing draws them: ntex = 0 */
void mdl_free_textures(Model *m);
/* Free a model (streams, mdl_compact block); its VRAM textures stay allocated */
void mdl_free(Model *m);
/* Free everything but the struct (which keeps the VRAM texture handles for mdl_share_textures); not drawable after */
void mdl_strip(Model *m);
/* dst draws with src's VRAM textures (a reloaded copy of the same model) instead of uploading its own */
void mdl_share_textures(Model *dst, const Model *src);
/* Index of the baked animation with this Havok/TAE id, or -1 */
int mdl_anim(const Model *m, int id);
/* The Havok/TAE id of baked animation `anim` (sound cues, src/sound.c) */
int mdl_anim_id(const Model *m, int anim);
float mdl_anim_length(const Model *m, int anim);
/* Local pose of an animation at time t (seconds, 30 fps samples, nlerp between frames); anim -1 = reference pose */
void mdl_sample(const Model *m, int anim, float t, int loop, BoneLocal *out);
/* out = a*(1-k) + b*k */
void mdl_blend(const Model *m, const BoneLocal *a, const BoneLocal *b, float k, BoneLocal *out);
/* World 3x4 matrix per bone (rotation rows m[0..8], translation m[9..11]) for the pose placed at pos / yaw */
void mdl_world(const Model *m, const BoneLocal *pose, V3 pos, float yaw, float (*world)[12]);
/* A bone-local point through a world matrix from mdl_world */
V3 mdl_point(const float *world, const float *local);
/* Skin and draw (into the current VU1 chain) at pos with facing yaw (model forward is -Z, game forward at yaw 0
 * is +Z); tint: i-frame gold */
void mdl_draw(const Model *m, const BoneLocal *pose, V3 pos, float yaw, int tint);
/* The same from world matrices already built by mdl_world for this pose (pos: where the area light is taken) */
void mdl_draw_world(const Model *m, const float (*world)[12], V3 pos, int tint);
/* Groups whose model.bin flags share a bit with `bits` are not drawn (default 0: all). Player bits from
 * export_model.py's DSR hide rule (EquipParamProtector invisibleFlag vs #NN# materials): */
#define MDL_HIDE_HELMET 2      /* the helmet's own meshes */
#define MDL_HIDE_UNDER_HELM 4  /* head meshes the worn helmet hides */
void mdl_hide(Model *m, unsigned bits);

/* The player's model (player.c): the same calls on the Warrior loaded by model_init */
int model_init(const void *blob);
int model_upload(GSGLOBAL *gs);
/* R2: after model_upload, keep only what drawing / sampling needs (mdl_compact) and free the pack blob */
int model_compact(void *blob, unsigned size);
int model_anim(int id);
float model_anim_length(int anim);
void model_sample(int anim, float t, int loop, BoneLocal *out);
void model_blend(const BoneLocal *a, const BoneLocal *b, float k, BoneLocal *out);
void model_draw(const BoneLocal *pose, V3 pos, float yaw, int tint);
/* Helm on (DSR hide rule for the head under it) or off (head, face and hair shown) */
void model_helmet(int on);

#endif
