/* Map collision from the game's Havok collision meshes (tools/export_collision.py -> collision.bin): floor height,
 * capsule push-out with wall sliding, ray test for the camera. Standalone: no gsKit / render.h dependency.
 * Points are float[3] in map space (x, y up, z); a V3 can be passed as &v.x. */
#ifndef COLLISION_H
#define COLLISION_H

/* Triangle masks: bit n = MSB1 collision HitFilterID n (soulstruct CollisionHitFilter) */
#define COL_SOLID 0x1FFu              /* 0-8: solid to the player (8 = Normal) */
#define COL_WATER ((1u << 9) | (1u << 16))
#define COL_DEATH ((1u << 13) | (1u << 14) | (1u << 15)) /* death camera, lethal fall, kill plane */
#define COL_CAMERA COL_SOLID          /* what the camera ray should stop at */

#define COL_WALKABLE_NY 0.7f /* normal.y at or above this is floor (about 45 degrees); below it is wall */

typedef struct {
    float y;               /* col_floor: surface height; col_ray: unused */
    float t;               /* col_ray: hit fraction along from -> to */
    float n[3];            /* unit triangle normal (facing up for floors, towards the ray origin for rays) */
    int tri;               /* triangle index (in its block's collision.bin) */
    int block;             /* world block (src/world.c) whose collision.bin holds tri; 0 = the boot area */
    unsigned char hit_filter, material; /* MSB HitFilterID; hkx subpart material (footstep sound / hit effect) */
} ColHit;

/* Load collision from collision.bin in memory (main.c loads it from the area pack; kept while the area is loaded);
 * NULL fails. Returns 1 on success. Allocates a 2-byte stamp per
 * triangle (~100 KB for Firelink). */
int col_init(const void *blob);
int col_tris(void);
/* World blocks (src/world.c): up to COL_BLOCKS collision.bin blobs in one world frame, all queried together.
 * col_init(blob) = col_load(0, blob); col_tris() = block 0's triangles */
#define COL_BLOCKS 5
int col_load(int block, const void *blob);
void col_unload(int block);
int col_block_tris(int block);
/* EMEVD "Activate Hit" (src/event.c): switch triangles [first, first + count) of a block off (0) or on again; queries
 * skip switched-off triangles (a bitmap of ntris / 8 bytes, allocated on the first switch-off). 0 = bad range. */
int col_tris_enable(int block, unsigned first, unsigned count, int on);
/* Collision tiles (src/coltile.c, tools/export_collision.py COL_TILE): a paged collision.bin keeps its index resident
 * and its pages (one tile's fragment data each) come and go. col_index_bytes: the index's size from the file's start
 * (head = its first n bytes; 0 = not paged; a value > n = read that many bytes and ask again). col_load_index loads
 * the index alone (every page absent); col_load still takes a whole file. col_pages: pages of a loaded block (0 none),
 * tile = the tile size; col_page: its box (map space), file offset and size; col_page_set: its data in memory (NULL:
 * absent; the buffer stays the caller's). Queries skip the triangles of absent pages. */
unsigned col_index_bytes(const void *head, unsigned n);
int col_load_index(int block, const void *blob);
int col_pages(int block, float *tile);
int col_page(int block, int page, float lo[3], float hi[3], unsigned *offset, unsigned *bytes);
void col_page_set(int block, int page, const void *data);
int col_page_in(int block, int page);
/* Wall contacts resolved by col_push_capsule since start (for the HUD / debugging) */
extern int col_contacts;

/* Highest triangle of `mask` under (x, z) whose surface lies in [y_bottom, y_top] (ray cast straight down from
 * y_top). Vertical triangles are ignored. Returns 1 and fills hit (may be NULL) when found. */
int col_floor(float x, float z, float y_top, float y_bottom, unsigned mask, ColHit *hit);
/* col_floor in one world block's collision only (map.c: each block's draw groups follow its own part under the player) */
int col_floor_block(int block, float x, float z, float y_top, float y_bottom, unsigned mask, ColHit *hit);

/* Push a vertical capsule standing at feet `pos` out of non-floor triangles (normal.y < COL_WALKABLE_NY),
 * horizontally only, so the caller's move slides along walls. The capsule spans pos.y + step .. pos.y + height
 * (radius r, rounded ends): anything lower than `step` (stairs, kerbs) is left to col_floor.
 * Returns the number of contacts resolved (0 = was free). */
int col_push_capsule(float pos[3], float r, float height, float step, unsigned mask);

/* Move feet `pos` by the horizontal (dx, dz) in substeps of at most r / 2, pushing out after each: tunnel-free wall
 * sliding. y is not changed. Returns 1 if any wall was touched. */
int col_move(float pos[3], float dx, float dz, float r, float height, float step, unsigned mask);

/* One walk step for a character: col_move, then the floor under the result within [y - step_down, y + step_up].
 * Returns 1 and sets pos[1] to the floor if there is one, 0 if not (x/z still moved, y unchanged: falling / ledge,
 * the caller decides). hit (may be NULL) gets the floor triangle. */
int col_walk(float pos[3], float dx, float dz, float r, float height, float step_up, float step_down, unsigned mask,
             ColHit *hit);

/* Segment from -> to against triangles of `mask` (two-sided). Returns 1 with the nearest hit (hit->t in [0, 1]).
 * Camera: eye = from + (to - from) * max(0, t - margin / length). */
int col_ray(const float from[3], const float to[3], unsigned mask, ColHit *hit);

/* Player capsule (NpcParam hitRadius / hitHeight) and step limits used by the prototype */
#define COL_PLAYER_R 0.4f
#define COL_PLAYER_H 1.5f
#define COL_STEP_UP 0.6f
#define COL_STEP_DOWN 1.2f

#endif
