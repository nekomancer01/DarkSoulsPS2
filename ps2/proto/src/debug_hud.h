/* Select debug overlay (fps, VU1 chains, collision, load, world blocks, enemies, objects, events, effects) and the
 * debug-only world drawing, kept out of main.c's frame loop. */
#ifndef DEBUG_HUD_H
#define DEBUG_HUD_H

#include "player.h"

/* What the overlay shows that only main.c knows (its locals and boot results) */
typedef struct {
    float cam_yaw, cam_pitch, cam_dist, cam_y;  /* orbit camera; cam_y = the eased follow height */
    float avg_dt;                               /* smoothed real frame time (s) */
    float t_cull, t_char, t_wait;               /* smoothed ms: map chain build, characters, VU1 wait */
    unsigned t_load;                            /* boot load, EE ticks */
    unsigned ram_free;                          /* EE RAM free (bytes) */
    int tris_drawn, vram_free;
    int model_ok, map_ok, col_ok;
    int enemies, breakables;                    /* roster sizes from enemy_init / objects_init */
} DebugHud;

/* Overlay on at boot: the scripted check builds (COL_TEST, HUD_DEBUG) and "option hud" scripts read it */
int debug_hud_default(int test_opts);
/* The overlay text for this frame (call between hud_draw and menu_draw) */
void debug_hud_draw(const DebugHud *d, const Player *pl, float dt);
/* Debug shapes in the world: the COMBAT_DUMMY hurtbox and the blade capsule axis (no-op without a dummy) */
void debug_draw_world(void);

#endif
