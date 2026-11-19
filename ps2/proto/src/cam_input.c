/* cam_input.c: the camera's yaw and pitch from the right stick and the lock-on target (moved out of frame.c's input
 * phase so the Lab's script runner steers the camera the same way; player.h) */
#include <math.h>
#include <stdio.h>
#include "player.h"
#include "testscript.h"

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
