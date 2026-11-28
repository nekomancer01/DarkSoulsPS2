/* Map visibility (mapvis.h): the frame's camera, the chunk / box view tests, foliage thinning and the streamer's
 * item visibility (pvs.c rows and draw groups) */
#include "mapvis.h"
#include "maplog.h"

float mp_cam[12], mp_cam_f, mp_cam_fy;
#ifdef MAP_DBG
int mp_vis_why;  /* mp_vis_test's reason for a 0: D_FAR / D_SMALL / D_VIEW */
#endif
/* mapstream.c's item visibility (pvs.c): item id of instance k (a near cell part, then a resident item) has a chunk
 * some camera cell within PVS_STREAM_R sees; textures, the sky and maps without pvs.bin count as seen */
static int ms_pvs_vis(int k, unsigned id)
{
    if (k < 0 || k >= MAP_INST || !mp_inst[k].ctab)
        return 1;
    const MapInst *m = &mp_inst[k];
    if (id < m->nctab) {
        unsigned n = m->ctab[id].near & ~CELL_PART;
        return !n || pvs_range(k, id, m->ctab[id].pad, n, 0);
    }
    id -= m->nctab;
    if (!m->rrange || id >= m->nritem || !m->rrange[id][1])
        return 1;
    return pvs_range(k, m->rrange[id][2], m->rrange[id][0], m->rrange[id][1], 0);
}

/* MS_DG (default on; make DEFS=-DMS_DG=0 = pvs only): an item none of whose chunks the draw groups of the recent floors
 * show (drawgrp.c dg_stream_shown) counts as unseen too: Firelink's cells from inside the Burg. A near part goes by
 * its cell's coarse chunks (the same pieces; its own chunks are known only once it is in). */
#ifndef MS_DG
#define MS_DG 1
#endif
static int ms_dg_vis(int k, unsigned id)
{
    const MapInst *m = &mp_inst[k];
    if (!dg_area(k) || !m->order)
        return 1;
    unsigned a, n;
    if (id < m->nctab) {
        unsigned c = id;
        while (c > 0 && (m->ctab[c].near & CELL_PART))  /* a later part of a cell: the cell's own entry */
            c--;
        a = m->ctab[c].first, n = m->ctab[c].count;
        if (!n)
            return 1;
        for (unsigned j = a; j < a + n; j++)
            if (dg_stream_shown(k, m->order[j]))
                return 1;
        return 0;
    }
    id -= m->nctab;
    if (!m->rrange || id >= m->nritem || !(n = m->rrange[id][1]))
        return 1;
    a = m->rrange[id][0];
    for (unsigned c = a; c < a + n; c++)
        if (dg_stream_shown(k, c))
            return 1;
    return 0;
}
int mp_vis(int k, unsigned id)
{
    if (!ms_pvs_vis(k, id))
        return 0;
    return !MS_DG || k < 0 || k >= MAP_INST || !mp_inst[k].ctab || ms_dg_vis(k, id);
}
unsigned mp_vis_gen(int k) { return pvs_gen(k) + (MS_DG ? dg_stream_gen() << 16 : 0); }
/* Foliage chunks hold tufts sorted by rank: draw those whose rank is below the density for the chunk's distance
 * (whole tufts; the ranks are spread evenly between the chunk's lowest and highest). Returns triangles to draw. */
unsigned mp_grass_tris(const Chunk *c, const float *e)
{
    float d2 = 0;
    for (int k = 0; k < 3; k++) {
        float o = e[k] < c->mn[k] ? c->mn[k] - e[k] : e[k] > c->mx[k] ? e[k] - c->mx[k] : 0;
        d2 += o * o;
    }
    float rho = d2 > GRASS_D0 * GRASS_D0 ? GRASS_D0 * GRASS_D0 / d2 : 1;
    float lo = (c->flags >> 16 & 255) * (1 / 255.0f), hi = (c->flags >> 24) * (1 / 255.0f);
    if (lo >= rho)
        return 0;
    if (hi <= rho)
        return c->tris;
    /* a prefix must end on a qword in all three streams (18 / 12 / 12 bytes per triangle): VIF only skips to the
     * next word after an UNPACK, so leftover vertex data in the last qword would run as VIF codes (the full chunk
     * is zero-padded, i.e. VIF NOPs). So: whole tufts, in steps of a multiple of 8 triangles. */
    unsigned per = c->flags >> 8 & 255, step = per;
    while (step & 7)
        step += per;
    unsigned n = (unsigned)(c->tris * (rho - lo) / (hi - lo) + 0.5f * step) / step * step;
    return n < c->tris ? n : c->tris;
}

int mp_visible(const Chunk *c, const float *e, int sky)
{
    PROF_BEGIN
    MP_BEGIN
    int r = mp_vis_test(c, e, sky);
    MP_END(MP_CULL)
    PROF_END(mp_prof_vis)
    return r;
}

/* view depth of box c's nearest (*zmax: farthest) point and its radius squared */
float mp_box_depth(const Chunk *c, const float *e, float *zmax, float *r2)
{
    float zc = 0, ext = 0, rr = 0;
    for (int k = 0; k < 3; k++) {
        float h = (c->mx[k] - c->mn[k]) * 0.5f;
        zc += mp_cam[9 + k] * (c->mn[k] + h - e[k]);
        ext += fabsf(mp_cam[9 + k]) * h;
        rr += h * h;
    }
    if (zmax)
        *zmax = zc + ext;
    if (r2)
        *r2 = rr;
    return zc - ext;
}

/* the last chunk visible_ measured and its box's farthest view depth (draw_chunk's edge fog test, same eye) */
const Chunk *mp_vis_c;
float mp_vis_zmax, mp_vis_zmin;

/* the frustum test of render.c r_box_classify (the box's bounding sphere against the view planes with a 40 px margin,
 * then the near plane and guard band) fused with the distance tests: one view transform of the box centre. Its plane
 * constants for this frame's camera (map_draw: vis_frame) */
static float vc_tx, vc_ty, vc_kx, vc_ky, vc_tg, vc_kg;

static void vis_frame(float fx, float fy)
{
    vc_tx = (R_SCREEN_W / 2 + 40) / fx, vc_ty = (R_SCREEN_H / 2 + 40) / fy;
    vc_kx = 1.0f / sqrtf(1 + vc_tx * vc_tx), vc_ky = 1.0f / sqrtf(1 + vc_ty * vc_ty);
    vc_tg = R_GUARD / fx, vc_kg = 1.0f / sqrtf(1 + vc_tg * vc_tg);
}

int mp_vis_test(const Chunk *c, const float *e, int sky)
{
    float h0 = (c->mx[0] - c->mn[0]) * 0.5f, h1 = (c->mx[1] - c->mn[1]) * 0.5f, h2 = (c->mx[2] - c->mn[2]) * 0.5f;
    float d0 = c->mn[0] + h0 - e[0], d1 = c->mn[1] + h1 - e[1], d2 = c->mn[2] + h2 - e[2];
    float vz = mp_cam[9] * d0 + mp_cam[10] * d1 + mp_cam[11] * d2, r2 = h0 * h0 + h1 * h1 + h2 * h2;
    if (!sky && !(c->flags & CHUNK_BACKDROP)) {  /* beyond FAR the edge fog is complete: blended mist / shafts end there (a fog-coloured card would show
                  * on the sky), opaque pieces at CULL_FAR (a fog-coloured silhouette, like DSR's distant background) */
        float ext = fabsf(mp_cam[9]) * h0 + fabsf(mp_cam[10]) * h1 + fabsf(mp_cam[11]) * h2, z = vz - ext;
        mp_vis_zmax = vz + ext, mp_vis_zmin = z, mp_vis_c = c;
#ifndef MAP_DBG_ALL
        float zcut = c->flags & CHUNK_BLEND ? FAR : CULL_FAR;
        if (z > zcut || (z > SMALL_Z && r2 * mp_cam_f * mp_cam_f < SMALL_PX * SMALL_PX * z * z)) {
#ifdef MAP_DBG
            mp_vis_why = z > zcut ? D_FAR : D_SMALL;
#endif
            return 0;
        }
#else
        (void)z;
#endif
    }
#ifdef MAP_DBG
    mp_vis_why = D_VIEW;
#endif
    float r = sqrtf(r2);
    if (vz < R_NEAR - r)
        return 0;
    float ax = fabsf(mp_cam[3] * d0 + mp_cam[4] * d1 + mp_cam[5] * d2), ay = fabsf(mp_cam[6] * d0 + mp_cam[7] * d1 + mp_cam[8] * d2);
    if ((ax - vc_tx * vz) * vc_kx > r || (ay - vc_ty * vz) * vc_ky > r)
        return 0;
    /* VU1-safe: the whole sphere in front of the near plane and inside the guard band */
    if (vz - r >= R_NEAR && (ax - vc_tg * vz) * vc_kg < -r && (ay - vc_tg * vz) * vc_kg < -r)
        return 1;
    return 2;
}

/* map_draw: this frame's camera (render.c r_camera_basis) and the view test's plane constants */
void mp_cam_set(void)
{
    mp_vis_c = NULL;
    V3 ey, rt, up, fw;
    float fx, fy, zs;
    r_camera_basis(&ey, &rt, &up, &fw, &fx, &fy, &zs);
    const float b[12] = {ey.x, ey.y, ey.z, rt.x, rt.y, rt.z, up.x, up.y, up.z, fw.x, fw.y, fw.z};
    memcpy(mp_cam, b, sizeof mp_cam);
    mp_cam_f = fx, mp_cam_fy = fy;
    vis_frame(fx, fy);
}
