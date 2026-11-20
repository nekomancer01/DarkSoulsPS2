/* camera.c: the camera (moved out of render.c, Polaris 1.7c-3): the view basis set once a frame (r_camera), read by
 * drawing and by simulate (enemy.c picks pose updates and omission levels with r_camera_basis / r_box_classify,
 * npc.c its visibility), so the Lab runs the same code with the eye from a trace. render.h declares it. */
#include <math.h>
#include "camera_i.h"

static V3 cam_eye, cam_right, cam_up, cam_fwd;

void r_camera(V3 eye, V3 target)
{
    cam_eye = eye;
    cam_fwd = v3norm(v3sub(target, eye));
    cam_right = v3norm(v3cross(v3(0, 1, 0), cam_fwd));
    cam_up = v3cross(cam_fwd, cam_right);
}

/* r_camera_override: another eye for the skinned draws between it and r_camera_override(NULL, ...) (shadow.c's
 * silhouette pass draws the player from the sun); r_view and the culling helpers keep the frame's camera */
static int ov_on;
static V3 ov[4];
static float ov_f[2];

void r_camera_override(const V3 *basis, float focal, float focal_y)
{
    ov_on = basis != NULL;
    if (ov_on)
        ov[0] = basis[0], ov[1] = basis[1], ov[2] = basis[2], ov[3] = basis[3], ov_f[0] = focal, ov_f[1] = focal_y;
}

void r_camera_basis(V3 *eye, V3 *right, V3 *up, V3 *fwd, float *focal, float *focal_y, float *zscale)
{
    if (ov_on) {
        *eye = ov[0], *right = ov[1], *up = ov[2], *fwd = ov[3], *focal = ov_f[0], *focal_y = ov_f[1];
        *zscale = ZMAX * NEAR;
        return;
    }
    *eye = cam_eye, *right = cam_right, *up = cam_up, *fwd = cam_fwd;
    *focal = FOCAL_X;
    *focal_y = FOCAL_Y;
    *zscale = ZMAX * NEAR;
}

static V3 to_view(V3 p)
{
    V3 d = v3sub(p, cam_eye);
    return v3(v3dot(d, cam_right), v3dot(d, cam_up), v3dot(d, cam_fwd));
}

V3 r_view(V3 p) { return to_view(p); }

/* Bounding-sphere frustum test: one view transform and four plane distances per chunk */
int r_box_classify(const float *mn, const float *mx)
{
    V3 c = v3((mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f);
    float dx = mx[0] - c.x, dy = mx[1] - c.y, dz = mx[2] - c.z, r = sqrtf(dx * dx + dy * dy + dz * dz);
    V3 v = to_view(c);
    if (v.z < NEAR - r)
        return 0;
    /* side planes |x| = t z with t = tan(half fov) (+40 px margin); signed distance = (|x| - t z) / sqrt(1 + t^2) */
    const float tx = (SCREEN_W / 2 + 40) / FOCAL_X, ty = (SCREEN_H / 2 + 40) / FOCAL_Y;
    const float kx = 1.0f / sqrtf(1 + tx * tx), ky = 1.0f / sqrtf(1 + ty * ty);
    if ((fabsf(v.x) - tx * v.z) * kx > r || (fabsf(v.y) - ty * v.z) * ky > r)
        return 0;
    /* VU1-safe: the whole sphere in front of the near plane and inside the guard band */
    const float tg = GUARD / FOCAL, kg = 1.0f / sqrtf(1 + tg * tg);
    if (v.z - r >= NEAR && (fabsf(v.x) - tg * v.z) * kg < -r && (fabsf(v.y) - tg * v.z) * kg < -r)
        return 1;
    return 2;
}
