/* Enemy pathfinding on DSR's navmesh: the MSB navmesh parts near a block's enemies, exported after the placements in
 * enemies.bin (tools/export_enemies.py "NAV1": welded vertices, triangle neighbours, an x/z grid). A* over the
 * triangles, then the funnel (string pulling) through the shared edges gives the next corner to walk to. */
#ifndef NAV_H
#define NAV_H

#define NAV_BLOCKS 5  /* = ENEMY_BLOCKS (enemy.h) */

/* enemies.bin of world block `block` (0 = the boot area; size 0 = unknown): copies its navmesh section, if any, into
 * an own block (the caller may free the blob). Returns the number of triangles (0: none, enemies walk straight). */
int nav_load(int block, const void *enemies_bin, unsigned size);
void nav_free(int block);
/* Navmesh bytes held (RAM report) */
unsigned nav_bytes(void);

/* Triangle under p (x/z inside, surface within [-1.0, +2.0] m of p's height, the closest), -1 if none */
int nav_tri_at(int block, const float p[3]);
/* Where a character at `from` walks next to reach `to`: out = the first corner of the shortest path through the
 * navmesh triangles (to itself when it is in sight on the mesh). When `to` cannot be reached, the path leads to the
 * reachable triangle closest to it. Returns 0 when `from` is not on this block's navmesh (then: walk straight), 1 when
 * `to` was not reached (out of search nodes or not connected), 2 when it was. */
int nav_next(int block, const float from[3], const float to[3], float out[3]);
/* nav_next searches run this frame / in total (HUD, budget) */
extern int nav_searches, nav_searches_total;

#endif
