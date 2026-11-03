/* DSR menu screens (drb.h; scr_*.bin format at the top of tools/export_screens.py) */
#include <dmaKit.h>
#include <kernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drb.h"
#include "hud.h"
#include "items.h"
#include "mapstream.h"
#include "pack.h"
#include "render.h"
#include "texcache.h"

#define DRB_Z 0xFFFF
#define ICON_COLS 10  /* icon sheet: ICON_COLS cells per row of a 512-wide PSMT8 texture */

typedef struct {
    char magic[4];
    u32 version, nodes, node_off, roots, root_off, strings, string_off;
    u16 aw, ah;
    u32 clut_off, pix_off, sprites, sprite_off;
} ScrHeader;
typedef struct { u16 u, v, w, h; } ScrSprite;

DrbStats drb_stats;

static float ui_pos[3];
static char name_open[8];
static int state;  /* 0 closed, 1 loading, 2 pin + upload, 3 ready, -1 failed */
static PackJob job;
static unsigned char *file;  /* the whole scr file while loading / uploading */
static unsigned file_alloc;
static unsigned char *tree;  /* header .. sprite table, kept while open */
static unsigned tree_bytes;
static const ScrHeader *hdr;
static const DrbNode *nodes;
static const u32 *roots;
static const ScrSprite *sprites;
static s8 *vis;              /* per node: 1 shown, 0 hidden, -1 default */
static GSTEXTURE atlas;
static int want_icons;

/* icon cache */
typedef struct { int icon, state; unsigned last; } IconCell;  /* state 0 empty, 1 reading, 2 ready */
static IconCell *cells;
static int ncells, icon_px, icon_cell_bytes;
static unsigned char *sheet, *cluts, *stage;  /* EE copies: the sheet (512 x rows), the CLUTs, one read's cell */
static unsigned sheet_bytes;
static GSTEXTURE sheet_t;
static u32 clut_vram;
static int sheet_dirty, reading_cell = -1;
static unsigned clut_dirty_lo, clut_dirty_hi;  /* CLUT cells to upload: bit per cell (up to 64) */
static int want_icon[8], nwant;
static unsigned frame;

static unsigned fnv(const char *s, int n)
{
    unsigned h = 0x811C9DC5u;
    for (int i = 0; i < n && s[i]; i++)
        h = (h ^ (unsigned char)s[i]) * 0x01000193u;
    return h;
}

void ui_set_pos(const float pos[3]) { memcpy(ui_pos, pos, sizeof ui_pos); }

void *ui_alloc(unsigned n)
{
    void *p = mapstream_ready() ? mapstream_alloc(n, ui_pos) : NULL;
    if (!p)
        p = memalign(128, n);
    if (p)
        drb_stats.ram_kb += (n + 1023) >> 10;
    else
        printf("drb: OUT OF RAM for %u B\n", n);
    return p;
}

void ui_free(void *p, unsigned n)
{
    if (!p)
        return;
    drb_stats.ram_kb -= (n + 1023) >> 10;
    if (mapstream_owns(p))
        mapstream_free(p, n);
    else
        free(p);
}

static void *job_get(unsigned n)
{
    void *p = ui_alloc(n);
    if (p)
        file_alloc = n;
    return p;
}

static void release_all(void)
{
    if (state == 1)
        pack_job_cancel(&job);
    if (file)
        ui_free(file, file_alloc), file = 0;
    if (tree)
        ui_free(tree, tree_bytes), tree = 0;
    if (reading_cell >= 0) {  /* a read into stage is in flight: let it land before the buffer goes */
        while (!item_icon_done())
            pack_async_wait();
        reading_cell = -1;
    }
    if (sheet)
        ui_free(sheet, sheet_bytes + ncells * 1024 + ncells * sizeof(IconCell) + icon_cell_bytes), sheet = 0;
    cells = 0, ncells = 0, nodes = 0, hdr = 0, vis = 0;
    tc_unpin();
    drb_stats.vram_kb = 0;
}

void drb_open(const char *name, int icons)
{
    drb_close();
    snprintf(name_open, sizeof name_open, "%s", name);
    char entry[24];
    snprintf(entry, sizeof entry, "scr_%s.bin", name);
    static char entry_keep[24];  /* the job keeps the name pointer */
    memcpy(entry_keep, entry, sizeof entry_keep);
    want_icons = icons;
    pack_job_start(&job, PACK_ROOT "menu.pak", entry_keep, job_get, ui_free);
    state = 1;
}

void drb_close(void)
{
    release_all();
    state = 0;
    name_open[0] = 0;
}

int drb_ready(void) { return state == 3; }
const char *drb_screen(void) { return state ? name_open : ""; }

static int fail(const char *why)
{
    printf("drb: %s: %s\n", name_open, why);
    release_all();
    state = -1;
    return -1;
}

/* screen file loaded: borrow VRAM, upload the atlas, keep the tree, set up the icon cache */
static int screen_setup(void)
{
    const ScrHeader *h = (const ScrHeader *)file;
    if (memcmp(h->magic, "DSCR", 4) || h->version != 1 || h->pix_off + h->aw * h->ah > job.raw ||
        h->node_off + h->nodes * sizeof(DrbNode) > h->clut_off)
        return fail("bad screen file");
    icon_px = item_icon_size();
    icon_cell_bytes = item_icon_bytes();
    ncells = icon_px > 0 && want_icons > 0 ? (want_icons < 64 ? want_icons : 64) : 0;
    int rows = (ncells + ICON_COLS - 1) / ICON_COLS;
    sheet_bytes = 512 * rows * icon_px;
    int atlas_bytes = h->aw * h->ah;
    int vram = atlas_bytes + 1024 + sheet_bytes + ncells * 1024;
    u32 base = tc_pin(vram);
    if (!base)
        return 0;  /* next frame */
    drb_stats.vram_kb = (vram + 1023) >> 10;
    memset(&atlas, 0, sizeof atlas);
    atlas.Width = h->aw, atlas.Height = h->ah;
    atlas.PSM = GS_PSM_T8, atlas.ClutPSM = GS_PSM_CT32, atlas.ClutStorageMode = GS_CLUT_STORAGE_CSM1;
    atlas.Filter = GS_FILTER_LINEAR;
    atlas.TBW = h->aw / 64;
    atlas.Vram = base, atlas.VramClut = base + atlas_bytes;
    FlushCache(0);
    gsKit_texture_send((u32 *)(file + h->pix_off), h->aw, h->ah, atlas.Vram, GS_PSM_T8, atlas.TBW, GS_CLUT_TEXTURE);
    gsKit_texture_send((u32 *)(file + h->clut_off), 16, 16, atlas.VramClut, GS_PSM_CT32, 1, GS_CLUT_PALLETE);
    dmaKit_wait_fast();
    /* keep header .. sprites (+ per-node visibility) */
    tree_bytes = ((h->clut_off + 127) & ~127) + ((h->nodes + 127) & ~127);
    tree = ui_alloc(tree_bytes);
    if (!tree)
        return fail("no RAM for the tree");
    memcpy(tree, file, h->clut_off);
    ui_free(file, file_alloc), file = 0;
    hdr = (const ScrHeader *)tree;
    nodes = (const DrbNode *)(tree + hdr->node_off);
    roots = (const u32 *)(tree + hdr->root_off);
    sprites = (const ScrSprite *)(tree + hdr->sprite_off);
    vis = (s8 *)(tree + ((hdr->clut_off + 127) & ~127));
    memset(vis, -1, hdr->nodes);
    if (ncells) {
        unsigned n = sheet_bytes + ncells * 1024 + ncells * sizeof(IconCell) + icon_cell_bytes;
        sheet = ui_alloc(n);
        if (!sheet)
            ncells = 0;
        else {
            memset(sheet, 0, sheet_bytes);
            cluts = sheet + sheet_bytes;
            cells = (IconCell *)(cluts + ncells * 1024);
            stage = (unsigned char *)(cells + ncells);
            stage = (unsigned char *)(((unsigned)stage + 63) & ~63u);
            memset(cells, 0, ncells * sizeof(IconCell));
            for (int i = 0; i < ncells; i++)
                cells[i].icon = -1;
            memset(&sheet_t, 0, sizeof sheet_t);
            sheet_t.Width = 512, sheet_t.Height = rows * icon_px;
            sheet_t.PSM = GS_PSM_T8, sheet_t.ClutPSM = GS_PSM_CT32, sheet_t.ClutStorageMode = GS_CLUT_STORAGE_CSM1;
            sheet_t.Filter = GS_FILTER_LINEAR, sheet_t.TBW = 8;
            sheet_t.Vram = base + atlas_bytes + 1024;
            clut_vram = sheet_t.Vram + sheet_bytes;
            sheet_dirty = 1;
        }
    }
    drb_stats.icons_loaded = drb_stats.icon_misses = 0;
    printf("drb: %s open: %u nodes, %u sprites, VRAM %d KB lent, RAM %d KB (tree %u, icons %u)\n", name_open,
           (unsigned)hdr->nodes, (unsigned)hdr->sprites, drb_stats.vram_kb, drb_stats.ram_kb, tree_bytes,
           ncells ? sheet_bytes + ncells * 1024 : 0);
    return 1;
}

/* icon reads: one at a time through the shared read slot; arrived cells go into the sheet / CLUT copies */
static void icons_step(void)
{
    if (!ncells)
        return;
    if (reading_cell >= 0) {
        int r = item_icon_done();
        if (r) {
            IconCell *c = &cells[reading_cell];
            if (r > 0) {
                memcpy(cluts + reading_cell * 1024, stage, 1024);
                int cx = (reading_cell % ICON_COLS) * icon_px, cy = (reading_cell / ICON_COLS) * icon_px;
                for (int y = 0; y < icon_px; y++)
                    memcpy(sheet + (cy + y) * 512 + cx, stage + 1024 + y * icon_px, icon_px);
                c->state = 2;
                sheet_dirty = 1;
                if (reading_cell < 32)
                    clut_dirty_lo |= 1u << reading_cell;
                else
                    clut_dirty_hi |= 1u << (reading_cell - 32);
                drb_stats.icons_loaded++;
            } else
                c->state = 0, c->icon = -1;
            reading_cell = -1;
        }
    }
    if (reading_cell < 0 && nwant) {
        int icon = want_icon[--nwant];
        int k = item_icon_index(icon);
        int dup = 0;
        for (int i = 0; i < ncells; i++)
            dup |= cells[i].icon == icon;
        if (k >= 0 && !dup) {
            int best = 0;  /* empty, else least recently drawn (not this frame) */
            for (int i = 1; i < ncells; i++)
                if ((cells[i].icon < 0 && cells[best].icon >= 0) ||
                    ((cells[i].icon < 0) == (cells[best].icon < 0) && cells[i].last < cells[best].last))
                    best = i;
            if (cells[best].last != frame && item_icon_read(k, stage)) {
                cells[best].icon = icon, cells[best].state = 1, cells[best].last = frame;
                reading_cell = best;
            } else
                want_icon[nwant++] = icon;  /* slot busy: next frame */
        }
    }
    if (sheet_dirty || clut_dirty_lo || clut_dirty_hi) {
        FlushCache(0);
        if (sheet_dirty)
            gsKit_texture_send((u32 *)sheet, 512, sheet_t.Height, sheet_t.Vram, GS_PSM_T8, 8, GS_CLUT_TEXTURE);
        for (int i = 0; i < ncells; i++)
            if (i < 32 ? clut_dirty_lo >> i & 1 : clut_dirty_hi >> (i - 32) & 1)
                gsKit_texture_send((u32 *)(cluts + i * 1024), 16, 16, clut_vram + i * 1024, GS_PSM_CT32, 1,
                                   GS_CLUT_PALLETE);
        dmaKit_wait_fast();
        sheet_dirty = 0, clut_dirty_lo = clut_dirty_hi = 0;
    }
}

int drb_step(void)
{
    frame++;
    if (state == 1) {
        int r = pack_job_step(&job);
        if (r < 0)
            return fail("load failed");
        if (!r)
            return 0;
        file = job.out;
        state = 2;
    }
    if (state == 2) {
        int r = screen_setup();
        if (r <= 0)
            return r;
        state = 3;
    }
    if (state == 3)
        icons_step();
    return state == 3 ? 1 : state < 0 ? -1 : 0;
}

const DrbNode *drb_node(int i) { return state == 3 && i >= 0 && i < (int)hdr->nodes ? &nodes[i] : 0; }

const char *drb_string(int s)
{
    if (state != 3 || s < 0 || s >= (int)hdr->strings)
        return "";
    return (const char *)(tree + ((const u32 *)(tree + hdr->string_off))[s]);
}

int drb_child(int parent, const char *name)
{
    if (state != 3 || parent < 0)
        return -1;
    unsigned h = fnv(name, 64);
    for (int k = parent + 1; k < nodes[parent].end; k++)
        if (nodes[k].hash == h)
            return k;
    return -1;
}

int drb_find(int root, const char *path)
{
    if (state != 3)
        return -1;
    int cur = root;
    while (*path) {
        const char *e = strchr(path, '/');
        int n = e ? e - path : (int)strlen(path);
        unsigned h = fnv(path, n);
        int found = -1;
        if (cur < 0) {
            for (unsigned r = 0; r < hdr->roots && found < 0; r++)
                if (nodes[roots[r]].hash == h)
                    found = roots[r];
        } else
            for (int k = cur + 1; k < nodes[cur].end && found < 0; k = nodes[k].end)
                if (nodes[k].hash == h)
                    found = k;
        if (found < 0)
            return -1;
        cur = found;
        path += n + (e != 0);
    }
    return cur;
}

void drb_show(int node, int on)
{
    if (state == 3 && node >= 0 && node < (int)hdr->nodes)
        vis[node] = on;
}

static u64 rgba(int r, int g, int b, int a) { return GS_SETREG_RGBAQ(r, g, b, a, 0); }

static void sprite(int s, float x0, float y0, float x1, float y1, int a)
{
    if (s < 0 || s >= (int)hdr->sprites)
        return;
    const ScrSprite *p = &sprites[s];
    gsKit_prim_sprite_texture(hud_gs(), &atlas, R_HX(x0), y0, p->u + 0.5f, p->v + 0.5f, R_HX(x1), y1,
                              p->u + p->w - 0.5f, p->v + p->h - 0.5f, DRB_Z, rgba(0x80, 0x80, 0x80, a));
}

void drb_draw_at(int node, float x, float y, float w, float h, int alpha)
{
    const DrbNode *n = drb_node(node);
    if (!n)
        return;
    if (n->type == DRB_SPRITE)
        sprite(n->spr, x, y, x + w, y + h, alpha);
    else if (n->type == DRB_RECT)
        gsKit_prim_sprite(hud_gs(), R_HX(x), y, R_HX(x + w), y + h, DRB_Z,
                          rgba(n->r >> 1, n->g >> 1, n->b >> 1, n->a * alpha >> 7));
}

const char *drb_button_tag(const char *tag)
{
    /* DS1 on the PS3 confirmed with Cross and went back with Circle; the PC text tags name the actions */
    static const struct { const char *tag, *name; } map[] = {
        {"conclusion", "X"}, {"cancel", "O"}, {"selectUD", "Up/Down"}, {"selectLR", "Left/Right"},
        {"select", "D-pad"}, {"pageUp", "L1"}, {"pageDown", "R1"}, {"pageUp2", "L2"}, {"pageDown2", "R2"},
        {"detail", "Square"}, {"sort", "Triangle"}, {"change", "Triangle"}, {"startAction", "START"},
        {"selectAction", "SELECT"},
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(tag, map[i].tag))
            return map[i].name;
    return NULL;
}

/* s with every <?tag?> replaced (fn first, then button names, else dropped) */
static const char *expand(const char *s, char *out, int n, int node, DrbTextFn fn, void *ctx)
{
    if (!strstr(s, "<?"))
        return s;
    int o = 0;
    while (*s && o < n - 1) {
        if (s[0] == '<' && s[1] == '?') {
            const char *e = strstr(s + 2, "?>");
            if (!e)
                break;
            char tag[40];
            int tn = e - (s + 2) < (int)sizeof tag - 1 ? e - (s + 2) : (int)sizeof tag - 1;
            memcpy(tag, s + 2, tn);
            tag[tn] = 0;
            const char *v = fn ? fn(node, tag, ctx) : NULL;
            if (!v)
                v = drb_button_tag(tag);
            for (; v && *v && o < n - 1; v++)
                out[o++] = *v;
            s = e + 2;
        } else
            out[o++] = *s++;
    }
    out[o] = 0;
    return out;
}

static void text_box(const DrbNode *t, float dx, float dy, const char *s, int alpha)
{
    if (!*s)
        return;
    int lh = hud_line_height();
    float x0 = t->x0 + dx, x1 = t->x1 + dx, y0 = t->y0 + dy, y1 = t->y1 + dy;
    float y = t->align & DRB_MIDDLE ? (y0 + y1 - lh) / 2 : t->align & DRB_BOTTOM ? y1 - lh : y0;
    int r = t->r >> 1, g = t->g >> 1, b = t->b >> 1;
    (void)alpha;  /* ASSUMED (TECH_DEBT menu-3): HUD text draws opaque */
    /* one line per \n, the box's width wraps nothing (the FMG text has its own line breaks) */
    char line[160];
    while (*s) {
        const char *e = strchr(s, '\n');
        int n = e ? e - s : (int)strlen(s);
        if (n > (int)sizeof line - 1)
            n = sizeof line - 1;
        memcpy(line, s, n);
        line[n] = 0;
        if (t->align & DRB_RIGHT)
            hud_text(x1, y, line, r, g, b, 1);
        else if (t->align & DRB_CENTER)
            hud_text((x0 + x1 - hud_text_width(line)) / 2, y, line, r, g, b, 0);
        else
            hud_text(x0, y, line, r, g, b, 0);
        y += lh;
        if (!e)
            break;
        s = e + 1;
    }
}

void drb_text(int node, float dx, float dy, const char *s, int alpha)
{
    const DrbNode *t = drb_node(node);
    if (t && s)
        text_box(t, dx, dy, s, alpha);
}

void drb_draw(int node, float dx, float dy, int alpha, DrbTextFn fn, void *ctx)
{
    if (state != 3 || node < 0 || node >= (int)hdr->nodes)
        return;
    char buf[256];
    int end = nodes[node].end;
    for (int k = node; k < end;) {
        const DrbNode *n = &nodes[k];
        int shown = k == node || (vis[k] < 0 ? !(n->flags & 1) : vis[k] > 0);
        if (!shown) {
            k = n->end;
            continue;
        }
        switch (n->type) {
        case DRB_SPRITE:
            sprite(n->spr, n->x0 + dx, n->y0 + dy, n->x1 + dx, n->y1 + dy, alpha);
            break;
        case DRB_RECT:
            gsKit_prim_sprite(hud_gs(), R_HX(n->x0 + dx), n->y0 + dy, R_HX(n->x1 + dx), n->y1 + dy, DRB_Z,
                              rgba(n->r >> 1, n->g >> 1, n->b >> 1, n->a * alpha >> 7));
            break;
        case DRB_TEXT:
            if (n->spr >= 0)
                text_box(n, dx, dy, expand(drb_string(n->spr), buf, sizeof buf, k, fn, ctx), alpha);
            else if (fn) {  /* no string of its own: filled by the caller (by node hash) */
                const char *s = fn(k, NULL, ctx);
                if (s)
                    text_box(n, dx, dy, s, alpha);
            }
            break;
        }
        k++;
    }
}

int drb_icon(int icon, float x, float y, float w, float h, int alpha)
{
    if (state != 3 || !ncells || icon < 0)
        return 0;
    for (int i = 0; i < ncells; i++)
        if (cells[i].icon == icon) {
            cells[i].last = frame;
            if (cells[i].state != 2)
                return 0;
            float u = (i % ICON_COLS) * icon_px, v = (i / ICON_COLS) * icon_px;
            sheet_t.VramClut = clut_vram + i * 1024;
            gsKit_prim_sprite_texture(hud_gs(), &sheet_t, R_HX(x), y, u + 0.5f, v + 0.5f, R_HX(x + w), y + h,
                                      u + icon_px - 0.5f, v + icon_px - 0.5f, DRB_Z, rgba(0x80, 0x80, 0x80, alpha));
            return 1;
        }
    drb_stats.icon_misses++;
    for (int i = 0; i < nwant; i++)
        if (want_icon[i] == icon)
            return 0;
    if (nwant < (int)(sizeof want_icon / sizeof want_icon[0]))
        want_icon[nwant++] = icon;
    return 0;
}
