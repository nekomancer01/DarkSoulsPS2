/* cam_input.c: the camera's yaw and pitch from the right stick and the lock-on target, and its follow behind the
 * player (moved out of frame.c's input and simulate phases so the Lab's script runner steers and places the camera the
 * same way; enemy updates read where it looks: player.h) */
#include <math.h>
#include <stdio.h>
#include "player.h"
#include "testscript.h"
#include "collision.h"
#include "render.h"

#define CAM_WALL (R_NEAR > 0.25f ? R_NEAR + 0.05f : 0.3f)  /* camera margin in front of walls (m) */

void cam_input(const Player *pl, const Input *in, float *cam_yaw, float *cam_pitch, float dt)
{
    *cam_yaw += in->cx * 2.5f * dt;
    {   /* lock-on: the camera turns to put the target behind the player (LockCamParam row 0; turn rate ASSUMED) */
        float tp[3];
        if (player_lock_target(pl, tp)) {
            float want = atan2f(tp[0] - pl->pos.x, tp[2] - pl->pos.z), d = want - *cam_yaw;
            while (d > 3.14159265f) d -= 6.2831853f;
            while (d < -3.14159265f) d += 6.2831853f;
            *cam_yaw += d * (dt * 6 < 1 ? dt * 6 : 1);
            static int lg;
            if (test_active() && !(lg++ % 30))
                printf("lock: cam %.2f want %.2f player %.2f target %.1f %.1f\n", *cam_yaw, want, pl->yaw, tp[0], tp[2]);
        }
    }
    *cam_pitch += in->cy * 1.5f * dt;
    /* LockCamParam row 0: rotRangeMinX -40 degrees (camera below the target, looking up) */
    *cam_pitch = *cam_pitch < -0.698f ? -0.698f : *cam_pitch > 1.2f ? 1.2f : *cam_pitch;
}

void cam_follow(const Player *pl, float cam_yaw, float cam_pitch, float dt, int col_ok, float *cam_y, float *cam_dist,
                V3 *eye, V3 *target)
{
    /* third-person orbit camera behind the player */
    /* LockCamParam row 0: camDistTarget 4.0 m, chrOrgOffset_Y 1.42 m */
    /* the follow height eases after the feet (steps, landings) instead of copying them; teleports snap */
    float ky = 1 - expf(-dt / 0.15f);
    *cam_y = fabsf(pl->pos.y - *cam_y) > 3 ? pl->pos.y : *cam_y + (pl->pos.y - *cam_y) * ky;
    *target = v3(pl->pos.x, *cam_y + 1.42f, pl->pos.z);
    float d = test_build_cam_dist(4.0f);
    *eye = v3add(*target, v3(-sinf(cam_yaw) * cosf(cam_pitch) * d, sinf(cam_pitch) * d,
                             -cosf(cam_yaw) * cosf(cam_pitch) * d));
    /* camera collision: pull in at once in front of the first wall hit (CAM_WALL margin: the near plane plus
     * 0.05 m, 0.3 m with the old 0.25 m plane), ease back out at 4 m/s */
    float want = d;
    ColHit ch;
    float ray0[3] = {target->x, target->y, target->z}, ray1[3] = {eye->x, eye->y, eye->z};
    if (col_ok && col_ray(ray0, ray1, COL_CAMERA, &ch))
        want = ch.t * d - CAM_WALL > 0.2f ? ch.t * d - CAM_WALL : 0.2f;
    *cam_dist = want < *cam_dist ? want : *cam_dist + 4.0f * dt > want ? want : *cam_dist + 4.0f * dt;
    *eye = v3add(*target, v3scale(v3sub(*eye, *target), *cam_dist / d));
    test_build_camera(eye, target);  /* ENEMY_VIEW check build */
    r_camera(*eye, *target);
}
