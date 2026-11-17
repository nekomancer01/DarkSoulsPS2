/* Character update omission levels (see omit.h for the DSR mapping) */
#include "omit.h"

unsigned omit_ran, omit_skipped, omit_lv[4];

static int lv_index(int level) { return level <= 0 ? 0 : level <= 2 ? 1 : level <= 5 ? 2 : 3; }

void omit_rank(OmitIn *in, int n)
{
    /* in view first, nearest first (insertion sort: n <= ACTIVE_MAX) */
    for (int i = 1; i < n; i++) {
        OmitIn x = in[i];
        int j = i;
        while (j > 0 && (in[j - 1].in_view < x.in_view ||
                         (in[j - 1].in_view == x.in_view && in[j - 1].cam_d2 > x.cam_d2)))
            in[j] = in[j - 1], j--;
        in[j] = x;
    }
    const float near2 = OMIT_NEAR_M * OMIT_NEAR_M;
    int k = 0;  /* rank among the characters in view */
    for (int i = 0; i < n; i++) {
        OmitIn *c = &in[i];
        int lv;
        if (c->force_full)
            lv = 0;
        else if (c->in_view)
            lv = k < OMIT_FULL_N ? 0 : k < OMIT_MID_N || c->cam_d2 < near2 ? 2 : 5;
        else
            lv = c->alert || c->cam_d2 < near2 ? 5 : 30;
        if (c->in_view)
            k++;
        /* a character that moved to a better level updates at once */
        if (lv < c->o->level)
            c->o->now = 1;
        c->o->level = (signed char)lv;
    }
}

int omit_step(Omit *o, float dt, unsigned frame, unsigned phase, float *run_dt)
{
    o->dt += dt;
    int lv = o->level < 0 ? 0 : o->level;
    if (lv > 0 && !o->now && (frame + phase) % (unsigned)(lv + 1)) {
        omit_skipped++;
        return 0;
    }
    *run_dt = o->dt > OMIT_DT_MAX && dt <= OMIT_DT_MAX ? OMIT_DT_MAX : o->dt;
    o->dt = 0, o->now = 0;
    omit_ran++;
    omit_lv[lv_index(lv)]++;
    return 1;
}

void omit_reset(Omit *o)
{
    o->level = OMIT_OFF, o->now = 1, o->dt = 0;
}
