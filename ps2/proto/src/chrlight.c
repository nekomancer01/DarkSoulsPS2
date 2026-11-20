/* Character lights (Polaris 2.2 PR 3, chrlight.h).
 *
 * DSR lights a character with the area's LightBank row (ambient + one directional light, export_map.char_light) and
 * the deferred point lights around it (FRPG_Deferred_PointLight, dsr_light.py): colour x falloff x max(N.L, 0). The
 * port had added the point lights (the player's lantern, the bonfires) to the ambient term, so a bonfire lit a
 * character's back as much as its front and the lookup walked every effect instance on every draw (~43 us per
 * breakable at iso_burg_taurus). Here the point lights are gathered once a frame (chrl_frame) and each character takes
 * the two strongest at its centre as directional lights in the skin programs' LIGHT block (layout.py LIGHT); the rest,
 * and any light within CHRL_NEAR of the centre (no stable direction: the player's own lantern on the player), add to
 * the ambient term as before.
 *
 * Falloff as before (cited in notes/EXE_FINDINGS.md, "Character point lights"): the player light linear,
 * saturate((end - d) / (end - begin)), measured feet to feet (= light at feet + 1 m to the character's centre);
 * effect lights cubic, saturate(((end - d) / (end - begin))^3), to the character's centre 1 m above the feet.
 *
 * CHR_LIGHTS 0 (make DEFS=-DCHR_LIGHTS=0) is the old path: map_char_light, every point light in the ambient term. */
#include <math.h>
#include "chrlight.h"
#include "map.h"
#include "fx.h"

#ifndef CHR_LIGHTS
#define CHR_LIGHTS 1
#endif
#define CHRL_MAX 32       /* point lights of a frame (the burg has ~6 lit at once) */
#define CHRL_NEAR 0.25f   /* ASSUMED (TECH_DEBT char-4): closer than this a light goes to the ambient term */
#define CHRL_CENTRE 1.0f  /* the character's centre above its feet (as fx_char_light) */

static struct {
    float pos[3], rgb[3], end, inv;  /* inv = 1 / (end - begin) */
    float end2;                      /* end squared: the quick reject */
    int cubic;
} cl[CHRL_MAX];
static int cl_n;

void chrl_frame(void)
{
    cl_n = 0;
    if (!CHR_LIGHTS)
        return;
    float feet[3], rgb[3], b, e;
    if (map_player_light(feet, rgb, &b, &e)) {  /* (light at feet + 1 m: the distance stays feet to feet) */
        for (int c = 0; c < 3; c++)
            cl[0].pos[c] = feet[c] + (c == 1) * CHRL_CENTRE, cl[0].rgb[c] = rgb[c];
        cl[0].end = e, cl[0].inv = 1 / (e - b), cl[0].end2 = e * e, cl[0].cubic = 0;
        cl_n = 1;
    }
    static FxLightOut fl[CHRL_MAX];
    int n = fx_lights(fl, CHRL_MAX - cl_n);
    for (int i = 0; i < n; i++, cl_n++) {
        for (int c = 0; c < 3; c++)
            cl[cl_n].pos[c] = fl[i].pos[c], cl[cl_n].rgb[c] = fl[i].rgb[c];
        cl[cl_n].end = fl[i].end, cl[cl_n].inv = 1 / (fl[i].end - fl[i].begin);
        cl[cl_n].end2 = fl[i].end * fl[i].end, cl[cl_n].cubic = 1;
    }
}

void chrl_pick(V3 pos, float amb[3], float sun[3], float dir[3], int *np, float pdir[2][3], float pcol[2][3])
{
    *np = 0;
    if (!CHR_LIGHTS) {
        map_char_light(pos, amb, sun, dir);
        return;
    }
    map_char_base(amb, sun, dir);
    float ctr[3] = {pos.x, pos.y + CHRL_CENTRE, pos.z}, best[2] = {0, 0};
    int bi[2] = {-1, -1};
    float att[CHRL_MAX], d[CHRL_MAX][3], dist[CHRL_MAX];
    for (int i = 0; i < cl_n; i++) {
        float d2 = 0;
        for (int c = 0; c < 3; c++)
            d[i][c] = cl[i].pos[c] - ctr[c], d2 += d[i][c] * d[i][c];
        att[i] = 0;
        if (d2 >= cl[i].end2)
            continue;
        dist[i] = sqrtf(d2);
        float a = (cl[i].end - dist[i]) * cl[i].inv;
        a = a > 1 ? 1 : cl[i].cubic ? a * a * a : a;
        if (a <= 0)
            continue;
        att[i] = a;
        if (dist[i] < CHRL_NEAR)  /* (no direction: ambient) */
            continue;
        float w = a * (cl[i].rgb[0] + cl[i].rgb[1] + cl[i].rgb[2]);
        if (w > best[0])
            best[1] = best[0], bi[1] = bi[0], best[0] = w, bi[0] = i;
        else if (w > best[1])
            best[1] = w, bi[1] = i;
    }
    for (int i = 0; i < cl_n; i++)  /* the rest: ambient, as map_char_light */
        if (att[i] > 0 && i != bi[0] && i != bi[1])
            for (int c = 0; c < 3; c++)
                amb[c] += cl[i].rgb[c] * att[i];
    for (int k = 0; k < 2 && bi[k] >= 0; k++) {
        int i = bi[k];
        float r = 1 / dist[i];
        for (int c = 0; c < 3; c++)
            pdir[k][c] = d[i][c] * r, pcol[k][c] = cl[i].rgb[c] * att[i];
        *np = k + 1;
    }
}
