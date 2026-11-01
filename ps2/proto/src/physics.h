/* Small EE rigid-body sim for breakable-object pieces (TECH_DEBT physics-1): oriented boxes (the pieces' Havok
 * hkpBoxShape + convex radius) with gravity, contacts against the map collision (collision.h: the floor plane under
 * each box from col_floor, walls by col_ray), sphere contacts between bodies, Coulomb friction, restitution, and
 * sleep once settled. DSR runs Havok (hkpRigidBody per piece); this is a stand-in with the same data. */
#ifndef PHYSICS_H
#define PHYSICS_H

typedef struct Body {
    float p[3], q[4];        /* piece frame: position, rotation (x y z w) */
    float v[3], w[3];        /* linear / angular velocity (world) */
    float half[3], c[3];     /* box half extents (convex radius included) and centre in the piece frame */
    float inv_mass, inv_i[3];/* inverse mass, inverse inertia (piece frame, principal axes) */
    float fr, rest;          /* hkpMaterial friction / restitution */
    float rs;                /* sphere radius for body-body contacts */
    float still;             /* seconds below the sleep thresholds */
    int group;               /* bodies of one group skip each other while `nocol` > 0 (a freshly broken object) */
    float nocol;
    int asleep, active;
    float fl_o[3], fl_n[3];  /* cached floor plane under the body (col_floor), queried at (fl_x, fl_z) */
    float fl_x, fl_z;
    int fl_ok, fl_age;
    float ray_o[3];          /* where the next wall ray starts (box centre WALL_EVERY frames ago) */
    int ray_age;
} Body;

#define PHYS_GRAVITY 9.8f
#define PHYS_SUBSTEPS 1
#define PHYS_MAX 128        /* bodies one phys_step handles */

/* One frame for n bodies (inactive ones skipped): map queries once per body per frame, integration and contacts in
 * PHYS_SUBSTEPS substeps. Returns the number of awake bodies. */
int phys_step(Body *b, int n, float dt);
/* Wake every sleeping body within radius r of point c (another object broke next to them) */
void phys_wake(Body *b, int n, const float c[3], float r);
/* Piece-frame point -> world */
void phys_point(const Body *b, const float local[3], float out[3]);

/* Map queries and contacts in the last phys_step (HUD) */
extern int phys_queries, phys_contacts;

#endif
