#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <dmaKit.h>
#include "model.h"
#include "vu1.h"
#include "map.h"

typedef struct {
    char magic[4];
    unsigned bones, verts, tris, anims, textures, groups;
    unsigned parents, ref, vert_off, tri_off, anim_off, tex_off, group_off, uv_off;
} Header;

typedef struct { unsigned w, h, clut, pix; } TexEntry;
typedef struct { int tex; unsigned first, count, flags; } Group;

typedef struct { short q[4], t[3], pad; } PackedBone;
typedef struct {
    float pa[3], pb[3];
    signed char na[3];
    unsigned char ba, bb, wa, r, g, b;
    unsigned char metal, spec;  /* _s map metalness / specular intensity x 255 (exporter; 0 in older blobs) */
} Vertex;

/* VU1 skinning streams (vu1skin.vsm), built once per model: unindexed batches of <= VU1_SKIN_TRIS triangles per
 * group, one 16-byte aligned slot of SK_V vertices per batch and stream */
#define SK_V ((VU1_SKIN_TRIS * 3 + 1) & ~1)  /* even: 16-byte slots */
#define MAX_TEX 12  /* the Warrior: 7 armour / weapon / skin + back of head, face, eyes, hair */
#define MAX_ANIMS 64

struct Model {
    const unsigned char *bin;
    const Header *hdr;
    const short *parents;
    const PackedBone *ref;
    const Vertex *verts;
    const unsigned short *tris;
    const Group *groups;
    const float *uvs;  /* [tri][3][2] texels */
    /* compact anims (tools/export_chr.py append_compact_anims): base pose + per-frame data of the moving bones only */
    struct {
        int id, frames, fps, nrot, ntra, stride;
        const PackedBone *data;  /* full anims: frames * bones; compact: the base pose */
        const unsigned char *rot, *tra;
        const unsigned char *fdata;
    } anims[MAX_ANIMS];
    int nanims;
    GSTEXTURE tex[MAX_TEX];
    int ntex;
    short (*sk_pa)[SK_V][4];  /* bone A position * 4096, 4 * bone A */
    short (*sk_pb)[SK_V][4];  /* bone B position * 4096, 4 * bone B */
    short (*sk_n)[SK_V][4];   /* normal (bone A space) * 4096, weight A * 4096 */
    float (*sk_st)[SK_V][2];  /* st normalized, or colour u8x4 if untextured */
    void *sk_block;  /* the one allocation the sk_ arrays live in */
    unsigned char (*sk_ms)[SK_V][4];  /* (0, 0, m, f) x 255 for vu1skin2's specular; NULL when the model has none */
    struct { short group, tris, spec; } *sk_batch;  /* spec: the batch has metal / specular (vu1skin2 TexSpec) */
    int nsk;
    unsigned hide;  /* group flag bits not drawn (model.h MDL_HIDE_*) */
    void *own;      /* mdl_compact's copy of the sections kept after load (freed by mdl_free) */
};

static short fx12(float v)
{
    v *= 4096.0f;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (short)(v < 0 ? v - 0.5f : v + 0.5f);
}

static int build_skin_streams(Model *m)
{
    const Header *hdr = m->hdr;
    const TexEntry *te = (const TexEntry *)(m->bin + hdr->tex_off);
    int n = 0;
    for (unsigned g = 0; g < hdr->groups; g++)
        n += (m->groups[g].count + VU1_SKIN_TRIS - 1) / VU1_SKIN_TRIS;
    /* one block for all of them (streamed enemy types load / unload all the time: six blocks per type fragmented the
     * heap until a model no longer fit with 2 MB free, 2026-11-01) */
    int ms = 0;
    for (unsigned i = 0; i < hdr->verts && !ms; i++)
        ms = m->verts[i].metal | m->verts[i].spec;
#ifdef SPEC_TEST  /* debug (make EE_DBGINFOFLAGS='-DSPEC_TEST=0.85,0.8'): metal m, specular s on every vertex */
    ms = 1;
#endif
#define SK_SZ(f) ((n * sizeof *m->f + 63) & ~63u)
    unsigned total = SK_SZ(sk_pa) + SK_SZ(sk_pb) + SK_SZ(sk_n) + SK_SZ(sk_st) + SK_SZ(sk_batch) + (ms ? SK_SZ(sk_ms) : 0);
    unsigned char *q = memalign(64, total ? total : 64);
    m->sk_block = q;
    if (!q)
        return 0;
    m->sk_pa = (void *)q, q += SK_SZ(sk_pa);
    m->sk_pb = (void *)q, q += SK_SZ(sk_pb);
    m->sk_n = (void *)q, q += SK_SZ(sk_n);
    m->sk_st = (void *)q, q += SK_SZ(sk_st);
    m->sk_batch = (void *)q, q += SK_SZ(sk_batch);
    m->sk_ms = ms ? (void *)q : NULL;
#undef SK_SZ
    m->nsk = 0;
    for (unsigned g = 0; g < hdr->groups; g++) {
        const Group *gr = &m->groups[g];
        int textured = gr->tex >= 0 && gr->tex < (int)hdr->textures;
        float iw = textured ? 1.0f / te[gr->tex].w : 0, ih = textured ? 1.0f / te[gr->tex].h : 0;
        for (unsigned t0 = gr->first; t0 < gr->first + gr->count; t0 += VU1_SKIN_TRIS, m->nsk++) {
            int b = m->nsk;
            unsigned nt = gr->first + gr->count - t0;
            if (nt > VU1_SKIN_TRIS)
                nt = VU1_SKIN_TRIS;
            m->sk_batch[b].group = g;
            m->sk_batch[b].tris = nt;
            m->sk_batch[b].spec = 0;
            memset(m->sk_st[b], 0, sizeof *m->sk_st);  /* stream padding is read by VIF as NOPs */
            if (m->sk_ms)
                memset(m->sk_ms[b], 0, sizeof *m->sk_ms);
            for (unsigned k = 0; k < nt * 3; k++) {
                const Vertex *v = &m->verts[m->tris[t0 * 3 + k]];
                for (int c = 0; c < 3; c++) {
                    m->sk_pa[b][k][c] = fx12(v->pa[c]);
                    m->sk_pb[b][k][c] = fx12(v->pb[c]);
                    m->sk_n[b][k][c] = fx12(v->na[c] / 127.0f);
                }
                m->sk_pa[b][k][3] = 4 * v->ba;
                m->sk_pb[b][k][3] = 4 * v->bb;
                m->sk_n[b][k][3] = fx12(v->wa / 255.0f);
                if (textured) {
                    if (m->sk_ms) {  /* f = s m + 0.04 s^2 (1 - m): the specular weight of vu1skin2's colour */
                        float mm = v->metal / 255.0f, ss = v->spec / 255.0f;
#ifdef SPEC_TEST
                        mm = ((const float[2]){SPEC_TEST})[0], ss = ((const float[2]){SPEC_TEST})[1];
#endif
                        float f = ss * mm + 0.04f * ss * ss * (1 - mm);
                        unsigned char *q = m->sk_ms[b][k];
                        q[0] = q[1] = 0, q[2] = (unsigned char)(mm * 255 + 0.5f), q[3] = (unsigned char)(f * 255 + 0.5f);
                        m->sk_batch[b].spec |= q[2] | q[3];
                    }
                    m->sk_st[b][k][0] = m->uvs[t0 * 6 + k * 2] * iw;
                    m->sk_st[b][k][1] = m->uvs[t0 * 6 + k * 2 + 1] * ih;
                } else {
                    unsigned char *col = (unsigned char *)m->sk_st[b] + k * 4;
                    col[0] = v->r, col[1] = v->g, col[2] = v->b, col[3] = 0x80;
                }
            }
            for (unsigned k = nt * 3; k < SK_V; k++)  /* zero padding up to the slot end */
                memset(m->sk_pa[b][k], 0, 8), memset(m->sk_pb[b][k], 0, 8), memset(m->sk_n[b][k], 0, 8);
        }
    }
    FlushCache(0);  /* the streams go to VIF1 by DMA straight from memory */
    return 1;
}

Model *mdl_load(const void *blob)
{
    const Header *hdr = blob;
    if (!blob || memcmp(hdr->magic, "DSRM", 4) || hdr->bones > 61 || hdr->verts > 65535)  /* 61: VU1 palette room */
        return NULL;
    Model *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->bin = blob;
    m->hdr = hdr;
    m->parents = (const short *)(m->bin + hdr->parents);
    m->ref = (const PackedBone *)(m->bin + hdr->ref);
    m->verts = (const Vertex *)(m->bin + hdr->vert_off);
    m->tris = (const unsigned short *)(m->bin + hdr->tri_off);
    m->groups = (const Group *)(m->bin + hdr->group_off);
    m->uvs = (const float *)(m->bin + hdr->uv_off);
    const unsigned char *p = m->bin + hdr->anim_off;
    for (m->nanims = 0; m->nanims < (int)hdr->anims && m->nanims < MAX_ANIMS; m->nanims++) {
        const unsigned *h = (const unsigned *)p;
        typeof(m->anims[0]) *a = &m->anims[m->nanims];
        a->id = h[0];
        a->frames = h[1] & 0x7FFFFFFF;
        a->fps = 30;
        if (!(h[1] & 0x80000000)) {
            a->data = (const PackedBone *)(p + 8);
            p += 8 + a->frames * hdr->bones * sizeof(PackedBone);
            continue;
        }
        /* u8 fps, rotated, translated, pad; u8 bone ids (padded to 4); base pose; frames */
        a->fps = p[8], a->nrot = p[9], a->ntra = p[10];
        a->rot = p + 12, a->tra = p + 12 + a->nrot;
        p += 12 + ((a->nrot + a->ntra + 3) & ~3);
        a->data = (const PackedBone *)p;
        p += hdr->bones * sizeof(PackedBone);
        a->fdata = p;
        a->stride = (a->nrot * 8 + a->ntra * 6 + 3) & ~3;
        p += a->frames * a->stride;
    }
    if (!build_skin_streams(m))
        return free(m), NULL;
    return m;
}

/* 8-bit palettized textures (PSMT8, CLUT in CSM1 order) into the VRAM left after the frame/Z buffers, placed by
 * render.c's block allocator (V6). V5: the exporters write CT32 CLUTs; characters are drawn alpha-tested only (VU1
 * skin TEST GEQUAL 0x40, no blending), so they go up as CT16 (RGB5A1, A = alpha >= 0x40; gsKit's TEXA maps A to
 * 0 / 0x80): 512 B instead of 1 KB per CLUT. Returns the bytes of VRAM still free, or -1 if a texture did not fit. */
int mdl_upload(Model *m, GSGLOBAL *gs)
{
    const TexEntry *te = (const TexEntry *)(m->bin + m->hdr->tex_off);
    static u16 clut16[256] __attribute__((aligned(64)));
    FlushCache(0);
    for (m->ntex = 0; m->ntex < (int)m->hdr->textures && m->ntex < MAX_TEX; m->ntex++) {
        GSTEXTURE *t = &m->tex[m->ntex];
        memset(t, 0, sizeof *t);
        t->Width = te[m->ntex].w;
        t->Height = te[m->ntex].h;
        t->PSM = GS_PSM_T8;
#ifdef V5_OFF  /* A/B measurement builds: CT32 CLUTs as exported */
        t->ClutPSM = GS_PSM_CT32;
#else
        t->ClutPSM = GS_PSM_CT16;
#endif
        t->ClutStorageMode = GS_CLUT_STORAGE_CSM1;
        t->Mem = (u32 *)(m->bin + te[m->ntex].pix);
        const u32 *c32 = (const u32 *)(m->bin + te[m->ntex].clut);
        dmaKit_wait_fast();  /* the previous texture's CLUT upload still reads clut16 */
        for (int i = 0; i < 256; i++) {
            u32 c = c32[i];
            clut16[i] = (c >> 3 & 0x1F) | (c >> 11 & 0x1F) << 5 | (c >> 19 & 0x1F) << 10 | ((c >> 24) >= 0x40) << 15;
        }
        SyncDCache(clut16, clut16 + 256);
        t->Clut = t->ClutPSM == GS_PSM_CT16 ? (u32 *)clut16 : (u32 *)c32;
        t->Filter = GS_FILTER_LINEAR;
        t->Vram = r_vram_tex(gs, t->Width, t->Height, t->PSM);
        t->VramClut = r_vram_clut(gs, t->ClutPSM);
        if (t->Vram == GSKIT_ALLOC_ERROR || t->VramClut == GSKIT_ALLOC_ERROR)
            return -1;
        gsKit_texture_upload(gs, t);
        t->Clut = NULL;  /* clut16 is reused by the next texture */
    }
    return 4 * 1024 * 1024 - gs->CurrentPointer;
}

int mdl_bones(const Model *m) { return m->hdr->bones; }

/* Keep only what drawing / sampling needs after mdl_load + mdl_upload (header, parents, reference pose, groups,
 * anims) in one new block, so the caller can free the blob: vertices, triangles, UVs and texture pixels live on in
 * the VU1 streams and VRAM. anim_end = the blob's size (enemy model.bin keeps its anims at the end). */
int mdl_compact(Model *m, unsigned blob_size)
{
    const Header *h = m->hdr;
    unsigned np = (h->bones * 2 + 3) & ~3u, nr = h->bones * sizeof(PackedBone), ng = h->groups * sizeof(Group);
    /* the anim section ends at the next section after it (the player's model.bin has textures, groups and UVs
     * behind its anims; enemy model.bin keeps them at the end) */
    unsigned anim_end = blob_size, after[] = {h->vert_off, h->tri_off, h->tex_off, h->group_off, h->uv_off};
    for (int i = 0; i < 5; i++)
        if (after[i] > h->anim_off && after[i] < anim_end)
            anim_end = after[i];
    unsigned na = anim_end > h->anim_off ? anim_end - h->anim_off : 0;
    unsigned char *b = memalign(16, sizeof(Header) + np + nr + ng + na);
    if (!b)
        return 0;
    unsigned char *q = b;
    memcpy(q, h, sizeof(Header)), q += sizeof(Header);
    memcpy(q, m->parents, np), m->parents = (const short *)q, q += np;
    memcpy(q, m->ref, nr), m->ref = (const PackedBone *)q, q += nr;
    memcpy(q, m->groups, ng), m->groups = (const Group *)q, q += ng;
    memcpy(q, m->bin + h->anim_off, na);
    for (int i = 0; i < m->nanims; i++) {
        long d = q - (m->bin + h->anim_off);
        m->anims[i].data = (const PackedBone *)((const unsigned char *)m->anims[i].data + d);
        if (m->anims[i].rot)
            m->anims[i].rot += d, m->anims[i].tra += d, m->anims[i].fdata += d;
    }
    m->hdr = (const Header *)b;
    m->bin = NULL, m->verts = NULL, m->tris = NULL, m->uvs = NULL;
    for (int i = 0; i < m->ntex; i++)
        m->tex[i].Mem = m->tex[i].Clut = NULL;
    m->own = b;
    return 1;
}

void mdl_strip(Model *m)
{
    free(m->sk_block), free(m->own);
    m->sk_block = NULL;
    m->sk_pa = m->sk_pb = m->sk_n = NULL, m->sk_st = NULL, m->sk_ms = NULL, m->sk_batch = NULL, m->own = NULL;
    m->nsk = 0, m->nanims = 0;
}

void mdl_free(Model *m)
{
    if (!m)
        return;
    mdl_strip(m);
    free(m);
}

/* Textures already in VRAM (an earlier load of the same model): use them instead of uploading */
void mdl_share_textures(Model *dst, const Model *src)
{
    dst->ntex = src->ntex;
    memcpy(dst->tex, src->tex, sizeof dst->tex);
}

int mdl_anim(const Model *m, int id)
{
    for (int i = 0; i < m->nanims; i++)
        if (m->anims[i].id == id)
            return i;
    return -1;
}

float mdl_anim_length(const Model *m, int anim) { return (m->anims[anim].frames - 1) / (float)m->anims[anim].fps; }

static void unpack(const PackedBone *b, BoneLocal *o)
{
    for (int k = 0; k < 4; k++)
        o->q[k] = b->q[k] / 32767.0f;
    for (int k = 0; k < 3; k++)
        o->t[k] = b->t[k] / 4096.0f;
}

void mdl_blend(const Model *m, const BoneLocal *a, const BoneLocal *b, float k, BoneLocal *out)
{
    for (unsigned i = 0; i < m->hdr->bones; i++) {
        float d = 0, s = 1, l = 0;
        for (int c = 0; c < 4; c++)
            d += a[i].q[c] * b[i].q[c];
        if (d < 0)
            s = -1;  /* shortest path */
        for (int c = 0; c < 4; c++) {
            out[i].q[c] = a[i].q[c] * (1 - k) + s * b[i].q[c] * k;
            l += out[i].q[c] * out[i].q[c];
        }
        l = 1.0f / sqrtf(l);
        for (int c = 0; c < 4; c++)
            out[i].q[c] *= l;
        for (int c = 0; c < 3; c++)
            out[i].t[c] = a[i].t[c] * (1 - k) + b[i].t[c] * k;
    }
}

void mdl_sample(const Model *m, int anim, float t, int loop, BoneLocal *out)
{
    static BoneLocal fa[MAX_BONES], fb[MAX_BONES];
    if (anim < 0 || anim >= m->nanims) {
        for (unsigned i = 0; i < m->hdr->bones; i++)
            unpack(&m->ref[i], &out[i]);
        return;
    }
    int n = m->anims[anim].frames;
    float f = t * m->anims[anim].fps;
    if (loop)
        f = fmodf(f, (float)(n - 1));
    if (f < 0)
        f = 0;
    if (f > n - 1)
        f = n - 1;
    int i0 = (int)f, i1 = i0 + 1 < n ? i0 + 1 : i0;
    if (m->anims[anim].rot) {  /* compact: base pose, then the moving bones of both frames */
        const typeof(m->anims[0]) *c = &m->anims[anim];
        for (unsigned i = 0; i < m->hdr->bones; i++)
            unpack(&c->data[i], &fa[i]);
        memcpy(fb, fa, m->hdr->bones * sizeof *fa);
        for (int k = 0; k < 2; k++) {
            const short *d = (const short *)(c->fdata + (k ? i1 : i0) * c->stride);
            BoneLocal *o = k ? fb : fa;
            for (int r = 0; r < c->nrot; r++, d += 4)
                for (int q = 0; q < 4; q++)
                    o[c->rot[r]].q[q] = d[q] / 32767.0f;
            for (int r = 0; r < c->ntra; r++, d += 3)
                for (int q = 0; q < 3; q++)
                    o[c->tra[r]].t[q] = d[q] / 4096.0f;
        }
        mdl_blend(m, fa, fb, f - i0, out);
        return;
    }
    const PackedBone *a = m->anims[anim].data + i0 * m->hdr->bones, *b = m->anims[anim].data + i1 * m->hdr->bones;
    for (unsigned i = 0; i < m->hdr->bones; i++) {
        unpack(&a[i], &fa[i]);
        unpack(&b[i], &fb[i]);
    }
    mdl_blend(m, fa, fb, f - i0, out);
}

/* 3x4 matrix: rotation rows m[0..8] + translation m[9..11] */
typedef struct { float m[12]; } M34;

static M34 from_qt(const float *q, const float *t)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    M34 r = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
              2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
              2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), t[0], t[1], t[2]}};
    return r;
}

static M34 mul34(const M34 *a, const M34 *b)
{
    M34 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            r.m[i * 3 + j] = a->m[i * 3] * b->m[j] + a->m[i * 3 + 1] * b->m[3 + j] + a->m[i * 3 + 2] * b->m[6 + j];
        r.m[9 + i] = a->m[i * 3] * b->m[9] + a->m[i * 3 + 1] * b->m[10] + a->m[i * 3 + 2] * b->m[11] + a->m[9 + i];
    }
    return r;
}

void mdl_world(const Model *m, const BoneLocal *pose, V3 pos, float yaw, float (*world)[12])
{
    M34 *w = (M34 *)world;
    /* model -> world: rotate by yaw + pi (model faces -Z), then translate */
    float c = cosf(yaw + 3.14159265f), s = sinf(yaw + 3.14159265f);
    M34 place = {{c, 0, s, 0, 1, 0, -s, 0, c, pos.x, pos.y, pos.z}};
    for (unsigned i = 0; i < m->hdr->bones; i++) {
        M34 local = from_qt(pose[i].q, pose[i].t);
        w[i] = m->parents[i] < 0 ? mul34(&place, &local) : mul34(&w[m->parents[i]], &local);
    }
}

V3 mdl_point(const float *w, const float *l)
{
    return v3(w[0] * l[0] + w[1] * l[1] + w[2] * l[2] + w[9], w[3] * l[0] + w[4] * l[1] + w[5] * l[2] + w[10],
              w[6] * l[0] + w[7] * l[1] + w[8] * l[2] + w[11]);
}

void mdl_draw(const Model *m, const BoneLocal *pose, V3 pos, float yaw, int tint)
{
    static M34 world[MAX_BONES];
    static float palette[MAX_BONES][16] __attribute__((aligned(16)));
    const Header *hdr = m->hdr;
    mdl_world(m, pose, pos, yaw, (float (*)[12])world);
    /* VU1 palette: columns of view * world, and the light direction in bone space in col0-2 .w */
    V3 eye, right, up, fwd;
    float focal, focal_y, zscale;
    r_camera_basis(&eye, &right, &up, &fwd, &focal, &focal_y, &zscale);
    M34 view = {{right.x, right.y, right.z, up.x, up.y, up.z, fwd.x, fwd.y, fwd.z,
                 -(right.x * eye.x + right.y * eye.y + right.z * eye.z),
                 -(up.x * eye.x + up.y * eye.y + up.z * eye.z),
                 -(fwd.x * eye.x + fwd.y * eye.y + fwd.z * eye.z)}};
    /* DSR-fitted light of the area (map.c map_char_light: area LightBank row, tone map, bonfire) */
    float amb[3], sun[3], L[3];
    map_char_light(pos, amb, sun, L);
    vu1_skin_light(amb, sun);
    {   /* specular terms of the area (map.c map_char_spec: GI_EnvSpc probe x envSpc, LightBank specular light) */
        float env_up[3], env_down[3], spec_i[3], spec_dir[3];
        map_char_spec(env_up, env_down, spec_i, spec_dir);
        vu1_skin_spec(env_up, env_down, spec_i, spec_dir);
    }
    for (unsigned i = 0; i < hdr->bones; i++) {
        M34 m = mul34(&view, &world[i]);
        const float *R = world[i].m;
        float *o = palette[i];
        for (int j = 0; j < 3; j++) {
            o[j * 4] = m.m[j], o[j * 4 + 1] = m.m[3 + j], o[j * 4 + 2] = m.m[6 + j];
            o[j * 4 + 3] = R[j] * L[0] + R[3 + j] * L[1] + R[6 + j] * L[2];
        }
        o[12] = m.m[9], o[13] = m.m[10], o[14] = m.m[11], o[15] = 0;
    }
    /* light: amb + sun max(n . L, 0) for texture modulation, vertex colour * 0x80 (0.5 + 0.75 max(n . L, 0)) when
     * untextured; i-frames add a warm gold */
    static const float tint_tex[3] = {70, 45, 0}, tint_flat[3] = {90, 60, 0}, none[3] = {0, 0, 0};
    vu1_skin_begin(palette, hdr->bones);
    for (int b = 0; b < m->nsk; b++) {
        const Group *g = &m->groups[m->sk_batch[b].group];
        if (g->flags & m->hide)
            continue;
        GSTEXTURE *gt = g->tex >= 0 && g->tex < m->ntex ? (GSTEXTURE *)&m->tex[g->tex] : NULL;
        vu1_skin_add(gt, g->flags & 1, m->sk_batch[b].tris, !tint ? none : gt ? tint_tex : tint_flat, m->sk_pa[b],
                     m->sk_pb[b], m->sk_n[b], m->sk_st[b],
                     g->flags >> 28 ? g->flags >> 28 : 1 /* Blinn 2^k from the _s roughness (export_model) */,
                     m->sk_ms && m->sk_batch[b].spec ? m->sk_ms[b] : NULL);
    }
}

/* ---- the player's model (player.c keeps the single-model calls) ---- */
static Model *pm;

int model_init(const void *blob)
{
    if (!(pm = mdl_load(blob)))
        return 0;
    model_helmet(1);  /* the Warrior starts with the helm on */
    return 1;
}
int model_upload(GSGLOBAL *gs) { return mdl_upload(pm, gs); }
int model_compact(void *blob, unsigned size)
{
    if (!pm || !mdl_compact(pm, size))
        return 0;
    free(blob);
    return 1;
}
int model_anim(int id) { return mdl_anim(pm, id); }
float model_anim_length(int anim) { return mdl_anim_length(pm, anim); }
void model_sample(int anim, float t, int loop, BoneLocal *out) { mdl_sample(pm, anim, t, loop, out); }
void model_blend(const BoneLocal *a, const BoneLocal *b, float k, BoneLocal *out) { mdl_blend(pm, a, b, k, out); }
void model_draw(const BoneLocal *pose, V3 pos, float yaw, int tint) { mdl_draw(pm, pose, pos, yaw, tint); }

void mdl_hide(Model *m, unsigned bits) { m->hide = bits & ~1u & ~0xF0000000u; }  /* bit 0 cull, 28-31 spec k */
void model_helmet(int on) { mdl_hide(pm, on ? MDL_HIDE_UNDER_HELM : MDL_HIDE_HELMET); }
