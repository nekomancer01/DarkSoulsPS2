#include <stdio.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <dmaKit.h>
#include "model.h"
#include "vu1.h"
#include "map.h"
#include "vmath.h"
#include "mapstream.h"
#ifdef VU1_CHECK
#include <stdio.h>
#endif

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

/* VU1 skinning streams (vu1skin2.vsm), built once per model: unindexed batches of <= VU1_SKIN_TRIS triangles per
 * group, each batch one prebuilt VIF packet (vu1_skin_packet, renderer plan PR 8) holding its streams */
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
    /* the textures the skin batches' st are built for (skin_plan / skin_step, load time only): the model's own, or for
     * animated objects the pack's shared texture model's (mdl_load_tex). nte also decides at draw time which packets
     * hold st (textured) and which colours */
    const TexEntry *te;
    int nte;
    void *sk_block;  /* the one allocation the batch table, the packets and the windows live in */
    unsigned sk_bytes;  /* its size (mapstream_free when it came from the near-cell arena) */
    unsigned char *sk_next;  /* skin_step: where the next batch's packet goes */
    int sk_has_ms;   /* the model has metal / specular streams */
    /* spec: the batch has metal / specular (vu1skin2 TexSpec); t0: its first triangle; win: its bone window; pkt /
     * bytes: its VIF packet, streams in it: pa / pb = bone A / B position * 4096, 4 * palette slot (s16x4); n = normal
     * (bone A space) * 4096, weight A * 4096; ms = (0, 0, m, f) x 255 for vu1skin2's specular (NULL when the model
     * has none); st = st normalized (f32x2), or colour u8x4 if untextured */
    struct SkBatch {
        short group, tris, spec, win;
        unsigned t0;
        int bytes;
        void *pkt, *pa, *pb, *n, *ms, *st;
    } *sk_batch;
    int nsk, nplan;
    /* bone windows (models with more than MDL_WIN_BONES bones, else nwin = 0 and the palette is uploaded whole):
     * window w uploads win_n[w] (slot, skeleton bone) pairs from win_ids + 2 win_first[w] before its first batch;
     * the palette slots it does not name keep what earlier windows put there (vu1_skin_window). Batch vertex bone
     * offsets are palette slots. */
    unsigned char *win_ids;
    unsigned short *win_first, *win_n;
    int nwin;
    int win_built;              /* skin_step: the last window applied to build_slot */
    unsigned char build_slot[256];  /* skin_step: skeleton bone -> palette slot after window win_built */
    unsigned hide;  /* group flag bits not drawn (model.h MDL_HIDE_*) */
    void *own;      /* mdl_compact's copy of the sections kept after load (freed by mdl_free) */
};

static short fx12(float v)
{
    v *= 4096.0f;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (short)(v < 0 ? v - 0.5f : v + 0.5f);
}

/* Batch plan: batches of <= VU1_SKIN_TRIS triangles per group in draw order; with more than MDL_WIN_BONES bones each
 * batch also gets a bone window, greedily: a batch joins the current window while the bones it needs fit in the
 * MDL_WIN_BONES palette slots together with the window's others; a batch is cut short where the next triangle's bones
 * would not fit, and the next batch starts a new window. A new window keeps what the slots hold: a bone already in a
 * slot costs no upload, new bones go to slots the window does not need (empty ones first). DSR's own meshes use <= 38
 * bones each (FLVER mesh bone tables, all 171 chr/ FLVERs), so a window usually spans whole meshes and consecutive
 * windows share the spine / root bones. fill = 0: count batches, windows and uploads only, and add up the batches'
 * packet bytes (vu1_skin_packet, ms = the model has metal / specular) in plan_bytes. */
static unsigned plan_bytes;
static void skin_plan(Model *m, int fill, int ms, int *nb, int *nw, int *nid)
{
    const Header *hdr = m->hdr;
    int windowed = hdr->bones > MDL_WIN_BONES, b = 0, w = 0, id = 0, nneed = 0;
    short occ[MDL_WIN_BONES], where[256];  /* slot -> bone / bone -> slot, -1 none */
    unsigned char need[MDL_WIN_BONES];     /* the current window's batches use the slot */
    memset(occ, -1, sizeof occ), memset(where, -1, sizeof where), memset(need, 0, sizeof need);
    if (fill && windowed)
        m->win_first[0] = 0, m->win_n[0] = 0;
    for (unsigned g = 0; g < hdr->groups; g++) {
        const Group *gr = &m->groups[g];
        unsigned t = gr->first, end = gr->first + gr->count;
        while (t < end) {
            unsigned t0 = t, nt = 0;
            int cut = 0;
            while (nt < VU1_SKIN_TRIS && t < end) {
                if (windowed) {
                    unsigned char tb[6];
                    int n = 0, extra = 0;
                    for (int k = 0; k < 3; k++) {
                        const Vertex *v = &m->verts[m->tris[t * 3 + k]];
                        unsigned char c[2] = {v->ba, v->bb};
                        for (int j = 0; j < 2; j++) {
                            int dup = 0;
                            for (int i = 0; i < n && !dup; i++)
                                dup = tb[i] == c[j];
                            if (!dup)
                                tb[n++] = c[j], extra += where[c[j]] < 0 || !need[where[c[j]]];
                        }
                    }
                    if (nneed + extra > MDL_WIN_BONES) {
                        if (nt) {  /* the batch ends here; the next one starts a new window */
                            cut = 1;
                            break;
                        }
                        /* the batch's first triangle: the window starts here (a fresh one holds any 6 bones) */
                        memset(need, 0, sizeof need), nneed = 0, w++;
                        if (fill)
                            m->win_first[w] = id, m->win_n[w] = 0;
                        continue;
                    }
                    for (int i = 0; i < n; i++) {
                        int s = where[tb[i]];
                        if (s >= 0 && need[s])
                            continue;
                        if (s < 0) {  /* upload: an empty slot, else the first one this window does not need */
                            for (s = 0; s < MDL_WIN_BONES && occ[s] >= 0; s++)
                                ;
                            if (s == MDL_WIN_BONES)
                                for (s = 0; need[s]; s++)
                                    ;
                            if (occ[s] >= 0)
                                where[occ[s]] = -1;
                            occ[s] = tb[i], where[tb[i]] = s;
                            if (fill)
                                m->win_ids[2 * id] = s, m->win_ids[2 * id + 1] = tb[i], m->win_n[w]++;
                            id++;
                        }
                        need[s] = 1, nneed++;
                    }
                }
                nt++, t++;
            }
            if (!fill)
                plan_bytes += vu1_skin_packet(NULL, nt, gr->tex >= 0 && gr->tex < m->nte, ms, NULL);
            if (fill) {
                m->sk_batch[b].group = g, m->sk_batch[b].tris = nt, m->sk_batch[b].t0 = t0;
                m->sk_batch[b].win = windowed ? w : -1, m->sk_batch[b].spec = 0;
            }
            b++;
            if (cut) {  /* the next batch starts a new window */
                memset(need, 0, sizeof need), nneed = 0, w++;
                if (fill)
                    m->win_first[w] = id, m->win_n[w] = 0;
            }
        }
    }
    *nb = b, *nw = windowed ? w + 1 : 0, *nid = id;
}

static int skin_alloc(Model *m)
{
    const Header *hdr = m->hdr;
    int n, nw, nid;
    int ms = 0;
    for (unsigned i = 0; i < hdr->verts && !ms; i++)
        ms = m->verts[i].metal | m->verts[i].spec;
#ifdef SPEC_TEST  /* debug (make EE_DBGINFOFLAGS='-DSPEC_TEST=0.85,0.8'): metal m, specular s on every vertex */
    ms = 1;
#endif
    plan_bytes = 0;
    skin_plan(m, 0, ms, &n, &nw, &nid);
    /* one block for all of them (streamed enemy types load / unload all the time: six blocks per type fragmented the
     * heap until a model no longer fit with 2 MB free, 2026-11-01) */
    /* the packets, each the size of its batch (plan_bytes), packed in build order by skin_step */
    unsigned tab = (n * sizeof *m->sk_batch + 15) & ~15u, pkts = plan_bytes;
    unsigned wsz = nw ? (nw * 4 + 2 * nid + 15) & ~15u : 0;
    unsigned total = tab + pkts + wsz;
    unsigned char *q = memalign(64, total ? total : 64);
    /* the heap with no run that large (the Taurus Demon's 1.65 MB block with 4.8 MB free in pieces, ISO 2026-11-08):
     * the near-cell arena (mapstream_alloc clears cells for it; granule aligned) */
    if (!q && total && mapstream_ready()) {
        static const float none[3];
        q = mapstream_alloc(total, none);
        if (q)
            printf("model: skin block %u KB from the near-cell arena (no heap run)\n", total >> 10);
    }
    m->sk_block = q, m->sk_bytes = total;
    if (!q)
        return 0;
    memset(q, 0, total);  /* stream padding is read by VIF as NOPs */
    m->sk_batch = (void *)q, q += tab;
    m->sk_next = q, q += pkts;
    m->sk_has_ms = ms;
    m->nwin = nw;
    if (nw) {
        m->win_first = (void *)q, m->win_n = m->win_first + nw, m->win_ids = (unsigned char *)(m->win_n + nw);
    }
    m->win_built = -1;
    skin_plan(m, 1, ms, &n, &nw, &nid);
    m->nplan = n;
    m->nsk = 0;
    return 1;
}

/* The planned skin batches from nsk on, until `budget` EE ticks have passed: 1 when every batch is built */
static int skin_step(Model *m, unsigned budget)
{
    const TexEntry *te = m->te;
    unsigned c0;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c0));
    while (m->nsk < m->nplan) {
        int b = m->nsk;
        unsigned g = m->sk_batch[b].group, t0 = m->sk_batch[b].t0, nt = m->sk_batch[b].tris;
        const Group *gr = &m->groups[g];
        int textured = gr->tex >= 0 && gr->tex < m->nte;
        float iw = textured ? 1.0f / te[gr->tex].w : 0, ih = textured ? 1.0f / te[gr->tex].h : 0;
        unsigned char *slot = m->build_slot;  /* skeleton bone -> palette slot (identity without windows) */
        if (m->sk_batch[b].win < 0) {
            if (m->win_built != -2)
                for (int i = 0; i < 256; i++)
                    slot[i] = i;
            m->win_built = -2;
        } else
            while (m->win_built < m->sk_batch[b].win) {  /* the windows up to this batch's, in order */
                int w = ++m->win_built;
                const unsigned char *u = m->win_ids + 2 * m->win_first[w];
                for (int i = 0; i < m->win_n[w]; i++)
                    slot[u[2 * i + 1]] = u[2 * i];
            }
        {
            struct SkBatch *sb = &m->sk_batch[b];
            void *s[5];
            sb->pkt = m->sk_next;
            sb->bytes = vu1_skin_packet(sb->pkt, nt, textured, m->sk_has_ms, s);
            m->sk_next += sb->bytes;
            sb->pa = s[0], sb->pb = s[1], sb->n = s[2], sb->ms = s[3], sb->st = s[4];
            short (*pa)[4] = sb->pa, (*pb)[4] = sb->pb, (*nr)[4] = sb->n;
            unsigned char (*msq)[4] = sb->ms;
            float (*st)[2] = sb->st;
            for (unsigned k = 0; k < nt * 3; k++) {
                const Vertex *v = &m->verts[m->tris[t0 * 3 + k]];
                for (int c = 0; c < 3; c++) {
                    pa[k][c] = fx12(v->pa[c]);
                    pb[k][c] = fx12(v->pb[c]);
                    nr[k][c] = fx12(v->na[c] / 127.0f);
                }
                pa[k][3] = 4 * slot[v->ba];
                pb[k][3] = 4 * slot[v->bb];
                nr[k][3] = fx12(v->wa / 255.0f);
                if (textured) {
                    if (msq) {  /* f = s m + 0.04 s^2 (1 - m): the specular weight of vu1skin2's colour */
                        float mm = v->metal / 255.0f, ss = v->spec / 255.0f;
#ifdef SPEC_TEST
                        mm = ((const float[2]){SPEC_TEST})[0], ss = ((const float[2]){SPEC_TEST})[1];
#endif
                        float f = ss * mm + 0.04f * ss * ss * (1 - mm);
                        unsigned char *q = msq[k];
                        q[0] = q[1] = 0, q[2] = (unsigned char)(mm * 255 + 0.5f), q[3] = (unsigned char)(f * 255 + 0.5f);
                        sb->spec |= q[2] | q[3];
                    }
                    st[k][0] = m->uvs[t0 * 6 + k * 2] * iw;
                    st[k][1] = m->uvs[t0 * 6 + k * 2 + 1] * ih;
                } else {
                    unsigned char *col = (unsigned char *)st + k * 4;
                    col[0] = v->r, col[1] = v->g, col[2] = v->b, col[3] = 0x80;
                }
            }
        }
        m->nsk++;
        unsigned c;
        __asm__ volatile("mfc0 %0, $9" : "=r"(c));
        if (c - c0 >= budget && m->nsk < m->nplan)
            return 0;
    }
    FlushCache(0);  /* the streams go to VIF1 by DMA straight from memory */
    return 1;
}

static Model *load_begin(const void *blob, const Model *tex);

Model *mdl_load_begin(const void *blob)
{
    return load_begin(blob, NULL);
}

static Model *load_begin(const void *blob, const Model *tex)
{
    const Header *hdr = blob;
    if (!blob || memcmp(hdr->magic, "DSRM", 4) || hdr->bones > MDL_MAX_BONES || hdr->verts > 65535)
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
    m->te = (const TexEntry *)(m->bin + hdr->tex_off), m->nte = hdr->textures;
    if (tex && tex->te && !hdr->textures)  /* groups index the shared texture model's textures */
        m->te = tex->te, m->nte = tex->nte;
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
    if (!skin_alloc(m))
        return free(m), NULL;
    return m;
}

int mdl_load_step(Model *m, unsigned budget) { return skin_step(m, budget); }

Model *mdl_load(const void *blob)
{
    return mdl_load_tex(blob, NULL);
}

Model *mdl_load_tex(const void *blob, const Model *tex)
{
    Model *m = load_begin(blob, tex);
    if (m)
        skin_step(m, ~0u);
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
        if (t->Vram == GSKIT_ALLOC_ERROR || t->VramClut == GSKIT_ALLOC_ERROR) {
            r_vram_free_tex(t->Vram, t->Width, t->Height, t->PSM);
            r_vram_free_clut(t->VramClut, t->ClutPSM);
            mdl_free_textures(m);  /* the ones before it: a failed upload holds no VRAM */
            return -1;
        }
        gsKit_texture_upload(gs, t);
        t->Clut = NULL;  /* clut16 is reused by the next texture */
    }
    return r_vram_avail(gs);
}

int mdl_upload_from(Model *m, const void *blob, GSGLOBAL *gs)
{
    const Header *h = m->hdr;
    const unsigned char *bin = m->bin;
    m->hdr = blob, m->bin = blob;
    int r = mdl_upload(m, gs);
    m->hdr = h, m->bin = bin;
    for (int i = 0; i < m->ntex; i++)
        m->tex[i].Mem = m->tex[i].Clut = NULL;
    return r;
}

void mdl_free_textures(Model *m)
{
    for (int i = 0; i < m->ntex; i++) {
        r_vram_free_tex(m->tex[i].Vram, m->tex[i].Width, m->tex[i].Height, m->tex[i].PSM);
        r_vram_free_clut(m->tex[i].VramClut, m->tex[i].ClutPSM);
    }
    m->ntex = 0;
}

int mdl_bones(const Model *m) { return m->hdr->bones; }
const short *mdl_parents(const Model *m) { return m->parents; }

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
    m->bin = NULL, m->verts = NULL, m->tris = NULL, m->uvs = NULL, m->te = NULL;
    for (int i = 0; i < m->ntex; i++)
        m->tex[i].Mem = m->tex[i].Clut = NULL;
    m->own = b;
    return 1;
}

void mdl_strip(Model *m)
{
    if (mapstream_owns(m->sk_block))
        mapstream_free(m->sk_block, m->sk_bytes);
    else
        free(m->sk_block);
    free(m->own);
    m->sk_block = NULL;
    m->sk_batch = NULL, m->sk_next = NULL, m->own = NULL;
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

int mdl_anim_id(const Model *m, int anim) { return m->anims[anim].id; }

float mdl_anim_length(const Model *m, int anim) { return (m->anims[anim].frames - 1) / (float)m->anims[anim].fps; }

static void unpack(const PackedBone *b, BoneLocal *o)
{
    for (int k = 0; k < 4; k++)
        o->q[k] = b->q[k] / 32767.0f;
    for (int k = 0; k < 3; k++)
        o->t[k] = b->t[k] / 4096.0f;
}

/* One bone of the nlerp; k 0 or 1 copies the end (keyframes are unit quaternions to int16 precision) */
static void blend1(const BoneLocal *a, const BoneLocal *b, float k, BoneLocal *o)
{
    if (k <= 0 || k >= 1) {
        *o = k <= 0 ? *a : *b;
        return;
    }
    float d = 0, s = 1, l = 0;
    for (int c = 0; c < 4; c++)
        d += a->q[c] * b->q[c];
    if (d < 0)
        s = -1;  /* shortest path */
    for (int c = 0; c < 4; c++) {
        o->q[c] = a->q[c] * (1 - k) + s * b->q[c] * k;
        l += o->q[c] * o->q[c];
    }
    l = 1.0f / sqrtf(l);
    for (int c = 0; c < 4; c++)
        o->q[c] *= l;
    for (int c = 0; c < 3; c++)
        o->t[c] = a->t[c] * (1 - k) + b->t[c] * k;
}

void mdl_blend(const Model *m, const BoneLocal *a, const BoneLocal *b, float k, BoneLocal *out)
{
    if (k <= 0 || k >= 1) {
        if (out != (k <= 0 ? a : b))
            memmove(out, k <= 0 ? a : b, m->hdr->bones * sizeof *out);
        return;
    }
    for (unsigned i = 0; i < m->hdr->bones; i++)
        blend1(&a[i], &b[i], k, &out[i]);
}

void mdl_sample(const Model *m, int anim, float t, int loop, BoneLocal *out)
{
    BoneLocal fa[m->hdr->bones], fb[m->hdr->bones];  /* VLA: <= 14 KB of stack at MDL_MAX_BONES */
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
    float k = f - i0;
    if (m->anims[anim].rot) {  /* compact: base pose, then the moving bones of both frames */
        const typeof(m->anims[0]) *c = &m->anims[anim];
        unsigned nb = m->hdr->bones;
        unsigned char mv[MDL_MAX_BONES];  /* 1: the bone has keyed rotation or translation */
        memset(mv, 0, nb);
        for (int r = 0; r < c->nrot; r++)
            mv[c->rot[r]] = 1;
        for (int r = 0; r < c->ntra; r++)
            mv[c->tra[r]] = 1;
        /* bones without keys hold the base pose in both frames: the blend leaves them as they are */
        for (unsigned i = 0; i < nb; i++)
            if (!mv[i])
                unpack(&c->data[i], &out[i]);
            else
                unpack(&c->data[i], &fa[i]), fb[i] = fa[i];
        int k0 = k >= 1 ? 1 : 0, k1 = k <= 0 ? 1 : 2;  /* at k 0 only frame i0 counts, at k 1 only i1 */
        for (int kk = k0; kk < k1; kk++) {
            const short *d = (const short *)(c->fdata + (kk ? i1 : i0) * c->stride);
            BoneLocal *o = kk ? fb : fa;
            for (int r = 0; r < c->nrot; r++, d += 4)
                for (int q = 0; q < 4; q++)
                    o[c->rot[r]].q[q] = d[q] / 32767.0f;
            for (int r = 0; r < c->ntra; r++, d += 3)
                for (int q = 0; q < 3; q++)
                    o[c->tra[r]].t[q] = d[q] / 4096.0f;
        }
        for (unsigned i = 0; i < nb; i++)
            if (mv[i])
                blend1(&fa[i], &fb[i], k, &out[i]);
        return;
    }
    const PackedBone *a = m->anims[anim].data + i0 * m->hdr->bones, *b = m->anims[anim].data + i1 * m->hdr->bones;
    if (k <= 0 || i1 == i0) {  /* on a keyframe: no blend */
        for (unsigned i = 0; i < m->hdr->bones; i++)
            unpack(&a[i], &out[i]);
        return;
    }
    for (unsigned i = 0; i < m->hdr->bones; i++) {
        unpack(&a[i], &fa[i]);
        unpack(&b[i], &fb[i]);
        blend1(&fa[i], &fb[i], k, &out[i]);
    }
}

/* model -> world: rotate by yaw + pi (model faces -Z), then translate */
static M34 placement(V3 pos, float yaw)
{
    float c = cosf(yaw + 3.14159265f), s = sinf(yaw + 3.14159265f);
    M34 place = {{c, 0, s, 0, 1, 0, -s, 0, c, pos.x, pos.y, pos.z}};
    return place;
}

/* The bone chain under root transform `root`: one 3x4 multiply per bone */
static void chain(const Model *m, const BoneLocal *pose, const M34 *root, M34 *w)
{
    for (unsigned i = 0; i < m->hdr->bones; i++) {
        M34 local = m34_from_qt(pose[i].q, pose[i].t);
        w[i] = m->parents[i] < 0 ? m34_mul(root, &local) : m34_mul(&w[m->parents[i]], &local);
    }
}

void mdl_world(const Model *m, const BoneLocal *pose, V3 pos, float yaw, float (*world)[12])
{
    M34 place = placement(pos, yaw);
    chain(m, pose, &place, (M34 *)world);
}

V3 mdl_point(const float *w, const float *l)
{
    return v3(w[0] * l[0] + w[1] * l[1] + w[2] * l[2] + w[9], w[3] * l[0] + w[4] * l[1] + w[5] * l[2] + w[10],
              w[6] * l[0] + w[7] * l[1] + w[8] * l[2] + w[11]);
}

/* World -> view of the current camera */
static M34 view_matrix(void)
{
    V3 eye, right, up, fwd;
    float focal, focal_y, zscale;
    r_camera_basis(&eye, &right, &up, &fwd, &focal, &focal_y, &zscale);
    M34 view = {{right.x, right.y, right.z, up.x, up.y, up.z, fwd.x, fwd.y, fwd.z,
                 -(right.x * eye.x + right.y * eye.y + right.z * eye.z),
                 -(up.x * eye.x + up.y * eye.y + up.z * eye.z),
                 -(fwd.x * eye.x + fwd.y * eye.y + fwd.z * eye.z)}};
    return view;
}

/* VU1 skin palette (C44, vmath.h): per bone the columns of view * world, and the light direction in bone space in
 * the rotation columns' .w (the light direction in bone space is R^T L for world rotation R; with V R in the palette,
 * V orthonormal, the camera basis, that is (V R)^T (V L)). Built on VU0 by c44_mul_skin (refactor target 5). */
static float palette[MDL_MAX_BONES][4][4] __attribute__((aligned(16)));

/* The area light for a character at pos (sent to the skin program) and its direction in view space -> Lv */
static void skin_light(const M34 *view, V3 pos, float *Lv)
{
    /* DSR-fitted light of the area (map.c map_char_light: area LightBank row, tone map, bonfire) */
    float amb[3], sun[3], L[3];
    map_char_light(pos, amb, sun, L);
    vu1_skin_light(amb, sun);
    {   /* specular terms of the area (map.c map_char_spec: GI_EnvSpc probe x envSpc, LightBank specular light) */
        float env_up[3], env_down[3], spec_i[3], spec_dir[3];
        map_char_spec(env_up, env_down, spec_i, spec_dir);
        vu1_skin_spec(env_up, env_down, spec_i, spec_dir);
    }
    for (int j = 0; j < 3; j++)
        Lv[j] = view->m[j * 3] * L[0] + view->m[j * 3 + 1] * L[1] + view->m[j * 3 + 2] * L[2];
    Lv[3] = 0;
}

/* Skin chains of the palette's pose */
static void skin_draw(const Model *m, int tint)
{
    /* light: amb + sun max(n . L, 0) for texture modulation, vertex colour * 0x80 (0.5 + 0.75 max(n . L, 0)) when
     * untextured; i-frames add a warm gold */
    static const float tint_tex[3] = {70, 45, 0}, tint_flat[3] = {90, 60, 0}, none[3] = {0, 0, 0};
    int win = m->nwin ? 0 : -1;
    vu1_skin_begin(palette, m->hdr->bones, m->nwin ? m->win_ids : NULL, m->nwin ? m->win_n[0] : 0);
    for (int b = 0; b < m->nsk; b++) {
        const struct SkBatch *sb = &m->sk_batch[b];
        const Group *g = &m->groups[sb->group];
        /* the next bone windows (char-7), in order even past hidden batches: each builds on the slots before it */
        while (win >= 0 && win < m->sk_batch[b].win) {
            win++;
            vu1_skin_window(palette, m->win_ids + 2 * m->win_first[win], m->win_n[win]);
        }
        if (g->flags & m->hide)
            continue;
        GSTEXTURE *gt = g->tex >= 0 && g->tex < m->ntex ? (GSTEXTURE *)&m->tex[g->tex] : NULL;
        int textured = g->tex >= 0 && g->tex < m->nte;  /* the packet's st kind (skin_step) */
#ifdef VU1_CHECK
        if (!gt != !textured) {
            static int nrep;
            if (nrep++ < 20)
                printf("DSRPROTO VU1_CHECK model %p batch %d: group %d tex %d, ntex %d, hdr textures %d, tris %d: %s\n",
                       (void *)m, b, sb->group, g->tex, m->ntex, m->nte, sb->tris,
                       gt ? "texture without st: drawn flat" : "st without texture: drawn grey");
        }
#endif
        vu1_skin_add_pkt(gt, textured, g->flags & 1, sb->tris, !tint ? none : gt && textured ? tint_tex : tint_flat,
                         sb->pa, sb->pb, sb->n, sb->st, g->flags >> 28 ? g->flags >> 28 : 1 /* Blinn 2^k from the _s roughness (export_model) */,
                         sb->ms && sb->spec ? sb->ms : NULL, sb->pkt, sb->bytes);
    }
}

void mdl_draw(const Model *m, const BoneLocal *pose, V3 pos, float yaw, int tint)
{
    /* the chain is built in view space (view folded into the root placement), straight into the palette: one VU0
     * multiply per bone, a parent's palette entry is its child's left factor */
    static float root[4][4] __attribute__((aligned(16))), local[4][4] __attribute__((aligned(16)));
    static float Lv[4] __attribute__((aligned(16)));
    M34 view = view_matrix(), place = placement(pos, yaw), vp = m34_mul(&view, &place);
    skin_light(&view, pos, Lv);
    c44_from_m34(&vp, root);
    for (unsigned i = 0; i < m->hdr->bones; i++) {
        const float (*parent)[4] = m->parents[i] < 0 ? root : palette[m->parents[i]];
        c44_from_qt(pose[i].q, pose[i].t, local);
        c44_mul_skin(palette[i], parent, (const float (*)[4])local, Lv);
    }
    skin_draw(m, tint);
}

void mdl_draw_world(const Model *m, const float (*world)[12], V3 pos, int tint)
{
    static float vc[4][4] __attribute__((aligned(16))), wc[4][4] __attribute__((aligned(16)));
    static float Lv[4] __attribute__((aligned(16)));
    M34 view = view_matrix();
    skin_light(&view, pos, Lv);
    c44_from_m34(&view, vc);
    for (unsigned i = 0; i < m->hdr->bones; i++) {
        c44_from_m34((const M34 *)world[i], wc);
        c44_mul_skin(palette[i], (const float (*)[4])vc, (const float (*)[4])wc, Lv);
    }
    skin_draw(m, tint);
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
