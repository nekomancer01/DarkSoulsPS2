/* Bonfire rest and the death loop (bonfire.h) */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bonfire.h"
#include "enemy.h"
#include "event.h"
#include "progress.h"

/* A bonfire object's spawn point (MSB SpawnPoint event 復活イベントNN at region 復活ポイントNN) has the object's entity
 * id + 1000: every bonfire 2009[03] registers in the game's MSBs follows it (Firelink 1021960 -> 1022960, Burg
 * 1011961 -> 1012961; tools/export_emevd.py exports the spawn points). ASSUMED rule: the exe's lookup is not found
 * (TECH_DEBT a4-1) */
#define SPAWN_OFFSET 1000
/* Walking within this of the bloodstain (feet, horizontal) gives its souls back. ASSUMED (TECH_DEBT a4-2): DSR's
 * pickup is the bloodstain SFX's touch check */
#define STAIN_R 1.0f
#define STAIN_DY 2.0f

static u32 area_id;
static V3 ground;      /* last spot the player stood on (alive): the bloodstain's place on a death */
static int ground_ok;
static Player *pl;     /* bonfire_update's player, for a rest from the action button */
static int armed;      /* the player has been away from the stain since it was dropped (one dropped at the respawn
                        * point is not picked up on the spawn) */

void bonfire_init(const char *area)
{
    area_id = progress_map_id(area);
}

void bonfire_rest(s32 entity)
{
    float p[3], yaw;
    prog.bonfire = entity;
    if (event_entity(entity + SPAWN_OFFSET, p, &yaw))
        prog.respawn = entity + SPAWN_OFFSET;
    else
        printf("bonfire %d: no spawn point %d, respawn point stays %d\n", (int)entity, (int)(entity + SPAWN_OFFSET),
               (int)prog.respawn);
    progress_touch();
    if (pl)
        bonfire_revive(pl);
    printf("bonfire %d rest: respawn %d, HP %d, Estus %d\n", (int)entity, (int)prog.respawn, pl ? pl->hp : -1,
           (int)prog.estus);
}

void bonfire_death(const Player *p)
{
    V3 at = ground_ok ? ground : p->pos;
    /* DSR: the souls and humanity held go into a bloodstain where the player last stood; an older stain is lost */
    prog.stain_map = 0;
    if (prog.souls > 0 || prog.humanity > 0) {
        prog.stain_map = area_id;
        prog.stain_pos[0] = at.x, prog.stain_pos[1] = at.y, prog.stain_pos[2] = at.z;
        prog.stain_souls = prog.souls, prog.stain_humanity = prog.humanity;
    }
    armed = 0;
    printf("bonfire: death, bloodstain %d souls %d humanity at (%.2f %.2f %.2f)\n", (int)prog.souls,
           (int)prog.humanity, at.x, at.y, at.z);
    prog.souls = prog.humanity = 0;
    progress_touch();
}

int bonfire_spawn(V3 *pos, float *yaw)
{
    float p[3];
    if (!prog.respawn || !event_entity(prog.respawn, p, yaw))
        return 0;
    *pos = v3(p[0], p[1], p[2]);  /* the spawn point regions sit on the floor (Firelink 復活ポイント00 = c0000_0000) */
    return 1;
}

void bonfire_revive(Player *p)
{
    p->hp = p->hp_max;
    prog.estus = prog.estus_max;  /* ASSUMED (TECH_DEBT a4-3): DSR refills to the bonfire's kindling level */
    progress_touch();
    enemy_respawn();
    event_rest();  /* temporary flags, Restart / End on rest (a death reloads the map in DSR: TECH_DEBT a4-4) */
    ground = p->pos, ground_ok = 1;
}

void bonfire_update(Player *p, float dt)
{
    (void)dt;
    pl = p;
    if (p->hp > 0 && !p->airborne && p->state != ST_DEAD)
        ground = p->pos, ground_ok = 1;
    if (!prog.stain_map || prog.stain_map != area_id || p->hp <= 0)
        return;
    float dx = p->pos.x - prog.stain_pos[0], dz = p->pos.z - prog.stain_pos[2];
    if (dx * dx + dz * dz > STAIN_R * STAIN_R || fabsf(p->pos.y - prog.stain_pos[1]) > STAIN_DY) {
        armed = 1;
        return;
    }
    if (!armed)
        return;
    progress_add_souls(prog.stain_souls);
    prog.humanity += prog.stain_humanity;
    printf("bonfire: bloodstain picked up: %d souls, %d humanity\n", (int)prog.stain_souls, (int)prog.stain_humanity);
    prog.stain_map = 0, prog.stain_souls = prog.stain_humanity = 0;
    progress_touch();
}

void bonfire_draw(void)
{
    if (!prog.stain_map || prog.stain_map != area_id)
        return;
    /* ASSUMED stand-in (TECH_DEBT a4-2): a flat green box for DSR's bloodstain SFX */
    r_box(v3(prog.stain_pos[0], prog.stain_pos[1], prog.stain_pos[2]), m3_rot_y(0), v3(0, 0.1f, 0),
          v3(0.35f, 0.1f, 0.35f), 0x30, 0xC0, 0x50);
}

int bonfire_test_cmd(const char *cmd, int *pass, int *fail)
{
    char what[16];
    int v;
    if (sscanf(cmd, "hurt %d", &v) == 1) {  /* damage to the player (an enemy hit without the sweep) */
        if (pl && pl->hp > 0) {
            pl->hp -= v;
            if (pl->hp <= 0)
                pl->hp = -1;  /* the ESD death branches test f1000() < 0 */
        }
        printf("TEST ev hurt %d: HP %d\n", v, pl ? pl->hp : -999);
        return 1;
    }
    float off;
    if (sscanf(cmd, "stain go %f", &off) == 1) {  /* onto the bloodstain, or `off` m east of it facing it */
        if (pl && prog.stain_map)
            pl->pos = v3(prog.stain_pos[0] + off, prog.stain_pos[1], prog.stain_pos[2]), pl->yaw = -1.5707963f,
            pl->vel = v3(0, 0, 0), pl->fall_y0 = pl->pos.y;
        printf("TEST ev stain go %.1f: %s\n", off, prog.stain_map ? "ok" : "no stain");
        return 1;
    }
    if (sscanf(cmd, "expect %15s %d", what, &v) != 2)
        return 0;
    int got;
    if (!strcmp(what, "estus"))
        got = prog.estus;
    else if (!strcmp(what, "hp"))  /* -1 dead, else HP */
        got = pl ? pl->hp : -999;
    else if (!strcmp(what, "stain"))  /* souls in the bloodstain of this area, -1 none */
        got = prog.stain_map == area_id ? prog.stain_souls : -1;
    else if (!strcmp(what, "stainx") || !strcmp(what, "stainz"))  /* its position, whole metres */
        got = (int)floorf(prog.stain_pos[what[5] == 'x' ? 0 : 2]);
    else if (!strcmp(what, "souls"))
        got = prog.souls;
    else if (!strcmp(what, "humanity"))
        got = prog.humanity;
    else if (!strcmp(what, "lastbonfire"))
        got = (int)prog.bonfire;
    else
        return 0;
    int ok = got == v;
    *pass += ok, *fail += !ok;
    printf("TEST ev expect %s %d: %d %s\n", what, v, got, ok ? "PASS" : "FAIL");
    return 1;
}
