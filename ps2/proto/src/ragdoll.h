/* Character ragdolls from DSR's chr hkx (tools/havok_ragdoll.py -> chr.bin v6, tools/export_chr.py): one capsule body
 * per main bone (pelvis, spine, head, limbs: 19 on a humanoid) and a ragdoll joint (ball socket + cone + twist limits)
 * from each body to its parent. DSR uses the same bodies as the character's hit capsules and, once it dies, as the
 * ragdoll that the body falls into.
 *
 * Here: rag_hit tests a weapon capsule against the bodies posed by the animation; a dead character's bodies are then
 * simulated as rigid bodies (position-based dynamics: substeps, joint / limit / floor constraints solved on positions
 * and orientations, velocities from the change) and drive the skeleton. Standalone apart from collision.h / model.h. */
#ifndef RAGDOLL_H
#define RAGDOLL_H

#include "model.h"

/* chr.bin v6 ragdoll record (tools/export_chr.py RAG_BODY) */
typedef struct {
    short bone, parent;   /* skeleton bone the body follows; joint parent body (-1 = the root body) */
    float a[3], b[3];     /* capsule ends in the bone's space */
    float ba[3], bb[3];   /* capsule ends in the body frame */
    float radius, mass;
    float q[4], t[3];     /* body frame in the bone's space */
    float piv_c[3], piv_p[3];    /* joint pivot in this body's frame / in the parent body's frame */
    float axis_c[3], axis_p[3];  /* joint twist axis in this body's frame / in the parent body's frame */
    float cone, twist_min, twist_max;
} RagBody;
_Static_assert(sizeof(RagBody) == 148, "export_chr.py RAG_BODY");

/* Weapon capsule a-b (radius r) against the bodies posed by `world` (mdl_world of the same model): index of the
 * nearest body it touches, -1 none */
int rag_hit(const RagBody *body, int n, const float (*world)[12], const float a[3], const float b[3], float r);

#define RAG_MAX 24
typedef struct {
    const RagBody *def;
    int n, on;
    float x[RAG_MAX][3], q[RAG_MAX][4];   /* centre (capsule middle) and orientation, world */
    float v[RAG_MAX][3], w[RAG_MAX][3];   /* linear / angular velocity, world */
    float com[RAG_MAX][3];                /* capsule middle in the body frame */
    float im[RAG_MAX], ii[RAG_MAX];       /* inverse mass, inverse (isotropic) inertia */
    float perp_p[RAG_MAX][3], perp_c[RAG_MAX][3];  /* twist reference axes (bind pose) */
    float fy[RAG_MAX], fn[RAG_MAX][3];    /* floor plane under each body: height, normal */
    unsigned char fok[RAG_MAX];
    float still, age;
    int asleep;
} Ragdoll;

/* Start from the animated pose: `world` now and `prev` one frame of dt earlier (velocities), all three mdl_world
 * matrices of the same model; `bind` (its reference pose) sets the twist references (twist 0 = bind, ASSUMED) */
void rag_start(Ragdoll *r, const RagBody *body, int n, const float (*bind)[12], const float (*world)[12],
               const float (*prev)[12], float dt);
/* Advance by dt (substeps, floor from the map collision) */
void rag_step(Ragdoll *r, float dt);
/* A capsule (the player) walking into the bodies pushes them (DSR: corpses can be kicked about); wakes it */
void rag_push(Ragdoll *r, const float feet[3], float radius, float height, const float vel[3]);
/* The skeleton from the bodies: bones with a body take its frame, the others keep their `base` local transform under
 * their parent. out = local pose for mdl_draw at (pos, yaw) */
void rag_pose(const Ragdoll *r, const Model *m, const BoneLocal *base, V3 pos, float yaw, BoneLocal *out);

#endif
