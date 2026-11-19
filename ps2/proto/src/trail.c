/* trail.c: the player's weapon trail (trail.h). DSR's tracer 250 (FRPG_SfxBnd_CommonEffects, action 40): the blade's
 * base and tip each frame of a SpawnFFX_Blade window, kept for TrailMaxTime (0.3 s), at most TrailLength (18) samples,
 * drawn additive with the tracer's colour. Each quad between two samples fades with age (alpha 1 -> 0 over
 * TrailMaxTime, TrailFadeMode 1 read as fading by age: ASSUMED); across the blade the texture's brightness ramp is
 * vertex alpha (untextured: no VRAM; TECH_DEBT fx-16). TRAIL=0: no trail (the old look). */
#include <string.h>
#include "trail.h"
#include "combat_data.h"
#include "render.h"
#include "vmath.h"

#ifndef TRAIL
#define TRAIL 1
#endif
#define TRAIL_N 18      /* the tracer's TrailLength */
#define TRAIL_GAP 0.1f  /* s without a sample: the next one starts a new trail (no quad across the gap) */

static struct {
    float a[3], b[3], t;
    int cut;  /* 1: no quad between this sample and the one before it */
} ring[TRAIL_N];
static int head, count, drawn;
static float now, last;

void trail_tick(float dt) { now += dt; }

void trail_sample(const float base[3], const float tip[3])
{
    if (!TRAIL || NTRAILS <= 0)
        return;
    int cut = !count || now - last > TRAIL_GAP;
    if (count && now == last)  /* a frozen frame (dt 0): replace, not add */
        head = (head + TRAIL_N - 1) % TRAIL_N, count--;
    memcpy(ring[head].a, base, 12), memcpy(ring[head].b, tip, 12);
    ring[head].t = now, ring[head].cut = cut;
    head = (head + 1) % TRAIL_N;
    if (count < TRAIL_N)
        count++;
    last = now;
}

int trail_drawn(void) { return drawn; }

void trail_draw(void)
{
    drawn = 0;
    if (!TRAIL || NTRAILS <= 0 || count < 2)
        return;
    float life = trail_time > 0 ? trail_time : 0.3f;
    const int *c = trail_rgba;
    ClipVert v[4];
    for (int k = 0; k + 1 < count; k++) {
        int i = (head + TRAIL_N - 1 - k) % TRAIL_N, j = (i + TRAIL_N - 1) % TRAIL_N;  /* i newer, j older */
        if (ring[i].cut)
            continue;
        float ai = 1 - (now - ring[i].t) / life, aj = 1 - (now - ring[j].t) / life;
        if (aj <= 0)
            break;
        ai = ai > 1 ? 1 : ai;
        /* across the blade, the tracer texture s00037's brightness ramp (64 x 64, flat along u, along v: 0.45 at
         * the tip edge, 1.0 a quarter in, 0 at the base; ASSUMED v = 0 at the tip) as vertex alpha in three rows */
        static const float row_t[3] = {0, 0.25f, 1}, row_k[3] = {0.45f, 1, 0};
        float wi[3][3], wj[3][3];
        for (int r = 0; r < 3; r++)
            for (int m = 0; m < 3; m++)
                wi[r][m] = ring[i].b[m] + (ring[i].a[m] - ring[i].b[m]) * row_t[r],
                wj[r][m] = ring[j].b[m] + (ring[j].a[m] - ring[j].b[m]) * row_t[r];
        for (int r = 0; r < 2; r++) {
            const float *p[4] = {wi[r], wi[r + 1], wj[r + 1], wj[r]};
            const float al[4] = {ai * row_k[r], ai * row_k[r + 1], aj * row_k[r + 1], aj * row_k[r]};
            for (int q = 0; q < 4; q++) {
                V3 w = r_view(v3(p[q][0], p[q][1], p[q][2]));
                v[q] = (ClipVert){w.x, w.y, w.z, 0, 0, (float)c[0], (float)c[1], (float)c[2], c[3] * al[q]};
            }
            r_tri_clip(NULL, &v[0], &v[1], &v[2], 0, R_BLEND | R_ADD);
            r_tri_clip(NULL, &v[0], &v[2], &v[3], 0, R_BLEND | R_ADD);
        }
        drawn++;
    }
}
