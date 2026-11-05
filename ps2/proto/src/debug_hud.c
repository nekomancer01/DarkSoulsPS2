/* Select debug overlay and debug world drawing (debug_hud.h). Moved out of main.c's frame loop as is. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "debug_hud.h"
#include "render.h"
#include "collision.h"
#include "combat.h"
#include "map.h"
#include "vu1.h"
#include "pack.h"
#include "world.h"
#include "enemy.h"
#include "objects.h"
#include "event.h"
#include "fx.h"
#include "testscript.h"

int debug_hud_default(int test_opts)
{
#if defined(COL_TEST) || defined(HUD_DEBUG)
    (void)test_opts;
    return 1;  /* the scripted check builds read their numbers off the overlay */
#else
    return (test_opts & TEST_HUD) != 0;
#endif
}

void debug_draw_world(void)
{
    if (!combat_dummy.hp_max)
        return;
    /* debug dummy: a box of its hurtbox size, red once dead; the blade capsule axis */
    float r = combat_dummy.radius, h = combat_dummy.height;
    r_box(v3(combat_dummy.pos[0], combat_dummy.pos[1], combat_dummy.pos[2]), m3_rot_y(combat_dummy.yaw),
          v3(0, h / 2, 0), v3(r, h / 2, r), combat_dummy.dead ? 0xC0 : 0x90, 0x60, combat_dummy.dead ? 0x40 : 0x90);
    if (combat_hit_active)
        r_line(v3(combat_hit_a[0], combat_hit_a[1], combat_hit_a[2]),
               v3(combat_hit_b[0], combat_hit_b[1], combat_hit_b[2]), 0xFF, 0xFF, 0x40);
}

void debug_hud_draw(const DebugHud *d, const Player *pl, float dt)
{
    char buf[64];
    r_text_outlined(30, 46, 2, player_state_name(pl));
    snprintf(buf, sizeof buf, "SP %d/%d", (int)pl->stamina, (int)pl->stamina_max);
    r_text_outlined(240, 28, 2, buf);
    if (pl->state == ST_ATTACK || pl->state == ST_ROLL) {
        snprintf(buf, sizeof buf, "T %d.%02d%s%s", (int)pl->t, (int)(pl->t * 100) % 100, pl->hitting ? " HIT" : "",
                 pl->invincible ? " IFRAME" : "");
        r_text_outlined(30, 62, 2, buf);
    }
    r_text_outlined(470, 410, 2, d->model_ok ? "DSR PS2 PROTO" : "MODEL.BIN BAD");
#if defined(COL_TEST) && COL_TEST == 7
    {   /* plunge check: every ESD state change since the start, "group0/group1" */
        static char path[96];
        static int last0 = -2, last1 = -2;
        int s0 = player_esd_state(0), s1 = player_esd_state(1), n = strlen(path);
        if ((s0 != last0 || s1 != last1) && n < (int)sizeof path - 12)
            snprintf(path + n, sizeof path - n, "%s%d/%d", n ? ">" : "", s0, s1);
        last0 = s0, last1 = s1;
        r_text_outlined(30, 78, 2, path);
    }
#endif
    if (d->col_ok) {  /* collision debug: feet position, wall pushes this frame, camera pulled in */
        static int last_contacts;
        snprintf(buf, sizeof buf, "COL %d %d %d W%d C%d", (int)floorf(pl->pos.x), (int)floorf(pl->pos.y),
                 (int)floorf(pl->pos.z), col_contacts - last_contacts, (int)(d->cam_dist * 10 + 0.5f));
        last_contacts = col_contacts;
        r_text_outlined(30, 390, 2, buf);
        /* step smoothness: largest per-frame height change of the feet / camera target over the last second (cm),
         * height in cm, HP */
        static float last_y, last_cy, jf, jc, jt;
        float df = fabsf(pl->pos.y - last_y) * 100, dc = fabsf(d->cam_y - last_cy) * 100;
        if (df < 300) jf = df > jf ? df : jf;
        if (dc < 300) jc = dc > jc ? dc : jc;
        last_y = pl->pos.y, last_cy = d->cam_y;
        static unsigned frames;
        snprintf(buf, sizeof buf, "Y %d J%d/%d HP %d F%u", (int)(pl->pos.y * 100), (int)(jf + 0.5f),
                 (int)(jc + 0.5f), pl->hp, ++frames);
        r_text_outlined(30, 374, 2, buf);
        /* damage debug: last hit (damage, attack rating x motion value vs defence, reaction level), dummy HP,
         * the latest fall's damage */
        snprintf(buf, sizeof buf, "HIT %d AR%d/DEF%d LV%d DUMMY %d FALL %d D%d", combat_last.damage,
                 (int)(combat_last.atk + 0.5f), (int)combat_last.def, combat_last.level, combat_dummy.hp,
                 pl->last_fall_damage, pl->deaths);
        r_text_outlined(30, 358, 2, buf);
        if ((jt += dt) > 1)
            jt = jf = jc = 0;
    }
    /* where the player stands and looks (testscript `spawn x y z yaw` / `cam yaw pitch`), for spot reports */
    snprintf(buf, sizeof buf, "POS %.1f %.1f %.1f CAM %.2f %.2f", pl->pos.x, pl->pos.y, pl->pos.z, d->cam_yaw,
             d->cam_pitch);
    r_text_outlined(470, 410, 2, buf);
    snprintf(buf, sizeof buf, d->vram_free < 0 ? "VRAM FULL" : "VRAM FREE %dK", d->vram_free / 1024);
    r_text_outlined(470, 390, 2, buf);
    snprintf(buf, sizeof buf, "%d FPS %dK TRIS%s", (int)(1 / d->avg_dt + 0.5f), d->tris_drawn / 1000,
             d->map_ok ? "" : " NO MAP");
    r_text_outlined(470, 370, 2, buf);
    snprintf(buf, sizeof buf, "VU1 %d  EE %d", map_vu1_batches, map_ee_tris);
    r_text_outlined(470, 350, 2, buf);
    /* VU1 chain qwords (R1): map / characters, this frame and peak, early segment kicks */
    snprintf(buf, sizeof buf, "CHN %d/%d %d/%d S%d CLUT %d+%d", vu1_stats.last[0], vu1_stats.peak[0],
             vu1_stats.last[1], vu1_stats.peak[1], vu1_stats.segments, vu1_stats.clut_loads[0],
             vu1_stats.clut_loads[1]);
    r_text_outlined(30, 246, 2, buf);
    snprintf(buf, sizeof buf, "MAP %d CHR %d VUW %d MS", (int)d->t_cull, (int)d->t_char, (int)d->t_wait);
    r_text_outlined(400, 330, 2, buf);
    /* load: wall time, compressed MB read, EE RAM left between heap and stack */
    snprintf(buf, sizeof buf, "LOAD %d.%02dS IO %d.%02dS %d.%dM RAM %d.%dM", (int)(d->t_load / 294912000),
             (int)(d->t_load / 2949120 % 100), (int)(pack_stats.wait_ticks / 294912000),
             (int)(pack_stats.wait_ticks / 2949120 % 100), (int)(pack_stats.read >> 20),
             (int)((pack_stats.read & 0xFFFFF) * 10 >> 20), (int)(d->ram_free >> 20),
             (int)((d->ram_free & 0xFFFFF) * 10 >> 20));
    r_text_outlined(238, 310, 2, buf);
    if (world_stats.blocks) {  /* world blocks (src/world.c): under the player / full / loaded, reading, pools */
        snprintf(buf, sizeof buf, "WLD B%d F%d W%X L%d/%d R%d %d/%dK %dMS", world_stats.current,
                 world_stats.full, world_stats.want, world_stats.loaded, world_stats.blocks, world_stats.reading,
                 world_stats.pool_used_kb, world_stats.pool_kb, world_stats.full_ms);
        r_text_outlined(238, 294, 2, buf);
    }
    /* nearest enemy: AI goal, distance, HP, action anim, ESD states; enemies updated / drawn / spawned */
    enemy_status(buf, sizeof buf);
    r_text_outlined(30, 294, 2, buf);
    snprintf(buf, sizeof buf, "EN %d/%d/%d", enemy_active, enemy_drawn, d->enemies);
    r_text_outlined(30, 278, 2, buf);
    /* breakables: instances / broken, live pieces / awake, map queries this frame */
    snprintf(buf, sizeof buf, "OBJ %d/%d PC %d/%d Q%d %dUS", d->breakables, objects_stats.broken,
             objects_stats.pieces, objects_stats.awake, objects_stats.queries, objects_stats.us);
    r_text_outlined(30, 262, 2, buf);
    event_status(buf, sizeof buf);  /* EMEVD: instances / waiting, fired, last fired id, stubs */
    r_text_outlined(238, 278, 2, buf);
    /* effects: particles alive / drawn, instances in range, GS packets, EE ms (update + draw) */
    snprintf(buf, sizeof buf, "FX %d/%d I%d B%d %d.%dMS", fx_stats.particles, fx_stats.drawn, fx_stats.active,
             fx_stats.batches, (int)fx_stats.ms, (int)(fx_stats.ms * 10) % 10);
    r_text_outlined(30, 230, 2, buf);
}
