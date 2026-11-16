#include <kernel.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "enemy.h"
#include "hud.h"
#include "bonfire.h"
#include "render.h"
#include "progress.h"
#include "event.h"
#include "menu.h"

typedef struct {
    char magic[4];
    unsigned version, atlases, sprites, glyphs, layout, strings;
    unsigned atlas_off, sprite_off, glyph_off, layout_off, string_off;
    unsigned short font_h, icon0;
    unsigned grads, grad_off;  /* version 2: GouraudRect corners (layout sprite -2 - k) */
} MenuHeader;
typedef struct { unsigned w, h, clut, pix; } AtlasEntry;
typedef struct { unsigned short atlas, u, v, w, h, pad; } Sprite;
typedef struct { unsigned short u, v, w, h; short pre, adv; } Glyph;
typedef struct { short sprite, x, y, w, h; unsigned char r, g, b, a; } LayoutEntry;

/* Bar length grows with the maximum like DSR's; the DRB frame is the length at these caps (DS1 limits) */
#define HP_CAP 1900.0f
#define SP_CAP 160.0f
/* HUD sprites write the nearest Z (Z test off): main.c's far depth of field blurs pixels by their Z and must leave
 * the HUD sharp wherever it lies over the distance */
#define HUD_Z 0xFFFF

static GSGLOBAL *gs;
static const unsigned char *mb;
static const MenuHeader *hdr;
static const Sprite *spr;
static const Glyph *gly;
static const unsigned char *grad;  /* 16 B per gradient: r, g, b, a of the top left, top right, bottom right, bottom left */
static HudRect lay[HUD_LAYOUT_COUNT];
static GSTEXTURE atlas[2];
static int ok;
static const char *fail = "MENU.PAK MISSING";  /* why hud_init failed, shown instead of the HUD */
static float died_t;

int hud_init(GSGLOBAL *g, const void *menu_bin)
{
    gs = g;
    if (!menu_bin)
        return -1;
    mb = menu_bin;
    hdr = menu_bin;
    fail = "MENU.BIN BAD";
    if (memcmp(hdr->magic, "DSMN", 4) || hdr->version != 2 || hdr->atlases != 2 || hdr->layout != HUD_LAYOUT_COUNT ||
        hdr->strings != STR_COUNT)
        return -1;
    fail = "HUD VRAM FULL";
    spr = (const Sprite *)(mb + hdr->sprite_off);
    gly = (const Glyph *)(mb + hdr->glyph_off);
    grad = mb + hdr->grad_off;
    const LayoutEntry *le = (const LayoutEntry *)(mb + hdr->layout_off);
    for (int i = 0; i < HUD_LAYOUT_COUNT; i++)
        lay[i] = (HudRect){le[i].sprite, le[i].x, le[i].y, le[i].w, le[i].h, le[i].r, le[i].g, le[i].b, le[i].a};
    const AtlasEntry *ae = (const AtlasEntry *)(mb + hdr->atlas_off);
    FlushCache(0);
    for (int i = 0; i < 2; i++) {
        GSTEXTURE *t = &atlas[i];
        memset(t, 0, sizeof *t);
        t->Width = ae[i].w;
        t->Height = ae[i].h;
        t->PSM = GS_PSM_T8;
        t->ClutPSM = GS_PSM_CT32;
        t->ClutStorageMode = GS_CLUT_STORAGE_CSM1;
        t->Mem = (u32 *)(mb + ae[i].pix);
        t->Clut = (u32 *)(mb + ae[i].clut);
        t->Filter = GS_FILTER_LINEAR;
        /* V6 block allocator; the CLUTs stay CT32 (V5 does not apply: HUD sprites and glyphs blend with soft alpha) */
        t->Vram = r_vram_tex(gs, t->Width, t->Height, t->PSM);
        t->VramClut = r_vram_clut(gs, t->ClutPSM);
        if (t->Vram == GSKIT_ALLOC_ERROR || t->VramClut == GSKIT_ALLOC_ERROR)
            return -1;
        gsKit_texture_upload(gs, t);
    }
    ok = 1;
    return 0;
}

GSGLOBAL *hud_gs(void) { return gs; }

const char *hud_string(int s)
{
    if (!ok || s < 0 || s >= STR_COUNT)
        return "";
    return (const char *)(mb + ((const unsigned *)(mb + hdr->string_off))[s]);
}

int hud_icon(int kit) { return hdr->icon0 + kit; }
int hud_line_height(void) { return ok ? hdr->font_h : 8; }

static u64 rgba(int r, int g, int b, int a) { return GS_SETREG_RGBAQ(r, g, b, a, 0); }

void hud_sprite(int s, float x, float y, float w, float h, int r, int g, int b, int a)
{
    if (!ok || s < 0 || (unsigned)s >= hdr->sprites)
        return;
    const Sprite *p = &spr[s];
    /* half-texel inset: bilinear filtering must not pull in the 1-texel gap around each atlas piece */
    gsKit_prim_sprite_texture(gs, &atlas[p->atlas], R_HX(x), y, p->u + 0.5f, p->v + 0.5f, R_HX(x + w), y + h,
                              p->u + p->w - 0.5f, p->v + p->h - 0.5f, HUD_Z, rgba(r, g, b, a));
}

void hud_fill(float x, float y, float w, float h, int r, int g, int b, int a)
{
    gsKit_prim_sprite(gs, R_HX(x), y, R_HX(x + w), y + h, HUD_Z, rgba(r, g, b, a));
}

void hud_gradient(float x, float y, float w, float h, const unsigned char c[16], int a)
{
    /* DRB GouraudRect: corners TL, TR, BR, BL; the strip runs TL, TR, BL, BR */
    u64 k[4];
    for (int i = 0; i < 4; i++)
        k[i] = rgba(c[4 * i], c[4 * i + 1], c[4 * i + 2], c[4 * i + 3] * a >> 7);
    gsKit_prim_quad_gouraud(gs, R_HX(x), y, R_HX(x + w), y, R_HX(x), y + h, R_HX(x + w), y + h, HUD_Z,
                            k[0], k[1], k[3], k[2]);
}

static void draw_layout(int i, int a)
{
    const HudRect *e = &lay[i];
    if (e->sprite >= 0)
        hud_sprite(e->sprite, e->x, e->y, e->w, e->h, 0x80, 0x80, 0x80, a);
    else if (e->sprite <= -2 && (unsigned)(-2 - e->sprite) < hdr->grads)
        hud_gradient(e->x, e->y, e->w, e->h, grad + 16 * (-2 - e->sprite), a);
    else
        hud_fill(e->x, e->y, e->w, e->h, e->r, e->g, e->b, e->a * a / 0x80);
}

void hud_layout(int i, int a) { draw_layout(i, a); }

/* PS2 face buttons for prompts and key guides (hud.h HUD_BTN_*): DSR's PC files carry keyboard / pad prompts in
 * neither menu.tpf nor the fonts, so the symbols are drawn: a dark round plate with the symbol in its usual colour
 * (Triangle green, Circle red, Cross blue, Square pink). Size = the text line height, a small gap after. */
#define BTN_GAP 4.0f
static float btn_size(void) { return ok ? gly['A' - 32].h : 20; }

static void thick_line(float x0, float y0, float x1, float y1, float t, u64 c)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l <= 0)
        return;
    float nx = -dy / l * t * 0.5f, ny = dx / l * t * 0.5f;
    gsKit_prim_quad(gs, R_HX(x0 + nx), y0 + ny, R_HX(x1 + nx), y1 + ny, R_HX(x0 - nx), y0 - ny, R_HX(x1 - nx), y1 - ny,
                    HUD_Z, c);
}

void hud_button(float x, float y, float h, int btn, int a)
{
    if (!gs)
        return;
    float r = h * 0.5f, cx = x + r, cy = y + r, t = h * 0.11f;
    u64 plate = rgba(0x10, 0x10, 0x10, a * 3 / 4), ring = rgba(0x60, 0x60, 0x60, a);
    enum { SEG = 16 };
    for (int i = 0; i < SEG; i++) {  /* plate: a 16-sided disc and a thin rim */
        float a0 = i * 6.2831853f / SEG, a1 = (i + 1) * 6.2831853f / SEG;
        gsKit_prim_triangle(gs, R_HX(cx), cy, R_HX(cx + r * cosf(a0)), cy + r * sinf(a0), R_HX(cx + r * cosf(a1)),
                            cy + r * sinf(a1), HUD_Z, plate);
        thick_line(cx + r * cosf(a0), cy + r * sinf(a0), cx + r * cosf(a1), cy + r * sinf(a1), 1.0f, ring);
    }
    float k = r * 0.52f;  /* symbol half size */
    switch (btn) {
    case HUD_BTN_CROSS: {
        u64 c = rgba(0x48, 0x70, 0x80, a);
        thick_line(cx - k, cy - k, cx + k, cy + k, t, c);
        thick_line(cx - k, cy + k, cx + k, cy - k, t, c);
        break;
    }
    case HUD_BTN_CIRCLE: {
        u64 c = rgba(0x80, 0x38, 0x38, a);
        for (int i = 0; i < SEG; i++) {
            float a0 = i * 6.2831853f / SEG, a1 = (i + 1) * 6.2831853f / SEG;
            thick_line(cx + k * cosf(a0), cy + k * sinf(a0), cx + k * cosf(a1), cy + k * sinf(a1), t, c);
        }
        break;
    }
    case HUD_BTN_TRIANGLE: {
        u64 c = rgba(0x38, 0x78, 0x58, a);
        float y0 = cy - k * 1.05f, y1 = cy + k * 0.75f, hx = k * 1.05f;
        thick_line(cx, y0, cx + hx, y1, t, c);
        thick_line(cx + hx, y1, cx - hx, y1, t, c);
        thick_line(cx - hx, y1, cx, y0, t, c);
        break;
    }
    case HUD_BTN_SQUARE: {
        u64 c = rgba(0x80, 0x50, 0x70, a);
        float q = k * 0.85f;
        thick_line(cx - q, cy - q, cx + q, cy - q, t, c);
        thick_line(cx + q, cy - q, cx + q, cy + q, t, c);
        thick_line(cx + q, cy + q, cx - q, cy + q, t, c);
        thick_line(cx - q, cy + q, cx - q, cy - q, t, c);
        break;
    }
    }
}

float hud_text_width(const char *s)
{
    float w = 0;
    for (; ok && *s; s++)
        if (*s >= 32 && *s < 127)
            w += gly[*s - 32].adv;
        else if (*s >= HUD_BTN_CROSS && *s <= HUD_BTN_SQUARE)
            w += btn_size() + BTN_GAP;
    return w;
}

float hud_text_ex(float x, float y, const char *s, int r, int g, int b, int a, int align, float sx, float sy)
{
    if (!ok)
        return 0;
    float w = hud_text_width(s) * sx;
    if (align)
        x -= w;
    for (; *s; s++) {
        if (*s >= HUD_BTN_CROSS && *s <= HUD_BTN_SQUARE) {  /* a button symbol in the text (hud.h) */
            float bh = btn_size() * sy;
            hud_button(x, y + (gly['A' - 32].h * sy - bh) * 0.5f, bh, *s, a);
            x += (btn_size() + BTN_GAP) * sx;
            continue;
        }
        if (*s < 32 || *s >= 127)
            continue;
        const Glyph *gl = &gly[*s - 32];
        if (*s != ' ')
            gsKit_prim_sprite_texture(gs, &atlas[0], R_HX(x + gl->pre * sx), y, gl->u, gl->v,
                                      R_HX(x + (gl->pre + gl->w) * sx), y + gl->h * sy, gl->u + gl->w, gl->v + gl->h, HUD_Z,
                                      rgba(r, g, b, a));
        x += gl->adv * sx;
    }
    return w;
}

static const u32 *help;
void hud_help_init(const void *blob)
{
    help = blob && !memcmp(blob, "DSHL", 4) ? blob : NULL;
}

const char *hud_help(int id)
{
    if (!help)
        return NULL;
    int lo = 0, hi = (int)help[1] - 1;  /* rows sorted by id */
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        u32 k = help[2 + 2 * m];
        if (k == (u32)id)
            return (const char *)help + help[3 + 2 * m];
        if (k < (u32)id)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return NULL;
}

void hud_prompt(float cx, float y, int btn, const char *text)
{
    char s[96];
    snprintf(s, sizeof s, "%c %s", btn, text);
    int lh = hud_line_height();
    float w = hud_text_width(s) + 40;
    hud_fill(cx - w / 2, y, w, lh + 10, 0, 0, 0, 0x60);  /* bonfire.c's prompt box */
    hud_text(cx - w / 2 + 20, y + 5, s, 0x80, 0x80, 0x80, 0);
}

float hud_text(float x, float y, const char *s, int r, int g, int b, int align)
{
    return hud_text_ex(x, y, s, r, g, b, 0x80, align, 1, 1);
}

void hud_begin(void)
{
    gs->PrimAlphaEnable = GS_SETTING_ON;
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
    gsKit_set_test(gs, GS_ATEST_OFF);
    gsKit_set_test(gs, GS_ZTEST_OFF);
}

void hud_end(void)
{
    gs->PrimAlphaEnable = GS_SETTING_OFF;
    gsKit_set_test(gs, GS_ZTEST_ON);
    gsKit_set_test(gs, GS_ATEST_ON);
}

/* One bar: frame and plate sized by max / cap, fill by cur / max, tinted */
static void bar(int bg, int plate, int fill, int frame, int end, int cur, int max, float cap, int r, int g, int b)
{
    const HudRect *fr = &lay[frame];
    float k = max / cap;
    k = k < 0.25f ? 0.25f : k > 1 ? 1 : k;
    float len = fr->w * k;
    draw_layout(bg, 0x80);
    const HudRect *pl = &lay[plate];
    hud_fill(pl->x, pl->y, len, pl->h, pl->r, pl->g, pl->b, pl->a);
    const HudRect *fi = &lay[fill];
    float f = max > 0 ? (cur > 0 ? cur : 0) / (float)max : 0;
    hud_sprite(fi->sprite, fi->x, fi->y, len * (f > 1 ? 1 : f), fi->h, r, g, b, 0x80);
    hud_sprite(fr->sprite, fr->x, fr->y, len, fr->h, 0x80, 0x80, 0x80, 0x80);
    const HudRect *en = &lay[end];
    hud_sprite(en->sprite, fr->x + len, en->y, en->w, en->h, 0x80, 0x80, 0x80, 0x80);
}

static void slot_icon(int layout, int kit)
{
    const HudRect *e = &lay[layout];
    /* the slot's icon box is portrait (DSR's 160x180 cells): square icon centred at the box's width */
    float s = e->w < e->h ? e->w : e->h;
    hud_sprite(hud_icon(kit), e->x + (e->w - s) / 2, e->y + (e->h - s) / 2, s, s, 0x80, 0x80, 0x80, 0x80);
}

/* Humanity counter (F20-02): FUN_1406753f0 updates it every frame. The number is min(humanity, 99) as two digits, the
 * tens always shown ("07"; FUN_140675760 shows humanNum_1<n / 10> and humanNum_0<n % 10>); while Hollow (chrType 8:
 * the exe's table 0x141a2b1e0 is 1 only there) the digits take MenuColorTableParam 60's grey (the TINT export_menu.py
 * stored in the digit entries). Not drawn (TECH_DEBT hud-3): the flame (Null elements icon_fire_0..9 swapped by the
 * F20_icon_fire_<min(humanity, 9)> animation, FUN_140675530) and the curse skull (icon_stone, shown while PlayerIns
 * +0x117 is set: the port has no curse). */
#define HUM_RING_FADE 70  /* FUN_1406753f0: icon_ring_0's FrpgMenuDlgObjFadePlate value 0x1bee = 70 by default, taken as
                           * alpha percent (ASSUMED, TECH_DEBT hud-3) */
static void humanity_counter(void)
{
    int n = prog.humanity < 0 ? 0 : prog.humanity > 99 ? 99 : prog.humanity;
    draw_layout(HUD_HUM_BG, 0x80);
    draw_layout(HUD_HUM_RING1, 0x80);
    draw_layout(HUD_HUM_RING0, 0x80 * HUM_RING_FADE / 100);
    for (int k = 0; k < 2; k++) {
        const HudRect *e = &lay[HUD_HUM_NUM + (k ? 10 + n / 10 : n % 10)];
        int c = prog.chr_type == CHR_HOLLOW ? 0x80 * e->r / 255 : 0x80;  /* r = g = b in row 60 */
        hud_sprite(e->sprite, e->x, e->y, e->w, e->h, c, c, c, 0x80);
    }
}

/* ---- boss bars (EMEVD 2003[11]) ---- */
static int boss_ent[2] = {-1, -1}, boss_name[2];

void hud_boss_bar(int slot, int entity, int name)
{
    for (int i = 0; i < 2; i++)
        if (slot < 0 || slot == i)
            boss_ent[i] = entity, boss_name[i] = name;
}

int hud_boss_bar_entity(int slot) { return slot >= 0 && slot < 2 ? boss_ent[slot] : -1; }

/* ASSUMED layout (TECH_DEBT event-18): DSR's FrpgMenuDlgBossGauge sprites are not exported, so a plain bar centred
 * near the bottom, slot 1 above slot 0, the name id over it */
static void boss_bars(void)
{
    for (int i = 0; i < 2; i++) {
        float r = 0;
        int dead = 0;
        if (boss_ent[i] < 0 || enemy_entity(boss_ent[i], NULL, &dead, &r, NULL) < 0)
            continue;
        float x = 140, y = 392 - i * 30, w = 360, h = 6;
        hud_fill(x - 1, y - 1, w + 2, h + 2, 0x10, 0x10, 0x10, 0x60);
        hud_fill(x, y, w * (r < 0 ? 0 : r > 1 ? 1 : r), h, 0xA0, 0x20, 0x18, 0x80);
        char buf[16];
        snprintf(buf, sizeof buf, "%d", boss_name[i]);
        hud_text(x, y - 22, buf, 0x80, 0x80, 0x80, 0);
    }
}

void hud_draw(const Player *p, float dt)
{
    if (!ok) {
        r_text_outlined(30, 30, 2, fail);
        return;
    }
#ifdef NO_HUD  /* cost check build: everything loaded and uploaded, nothing drawn */
    return;
#endif
    hud_begin();
    bar(HUD_BAR_BG_HP, HUD_PLATE_HP, HUD_FILL_HP, HUD_FRAME_HP, HUD_END_HP, p->hp, p->hp_max, HP_CAP,
        0xC0, 0x28, 0x20);
    bar(HUD_BAR_BG_SP, HUD_PLATE_SP, HUD_FILL_SP, HUD_FRAME_SP, HUD_END_SP, p->stamina, p->stamina_max, SP_CAP,
        0x48, 0xA8, 0x40);
    draw_layout(HUD_BAR_START1, 0x80);
    draw_layout(HUD_BAR_START, 0x80);
    humanity_counter();
    /* quick slots: left hand (shield), right hand (sword), item (Estus Flask) */
    draw_layout(HUD_SLOT_L_PLATE, 0x80);
    slot_icon(HUD_SLOT_L_ICON, STR_WEP_L - STR_WEP_R);
    draw_layout(HUD_SLOT_R_PLATE, 0x80);
    slot_icon(HUD_SLOT_R_ICON, 0);
    draw_layout(HUD_SLOT_I_FRAME, 0x80);
    draw_layout(HUD_SLOT_I_PLATE, 0x80);
    slot_icon(HUD_SLOT_I_ICON, STR_ESTUS - STR_WEP_R);
    char buf[16];
    const HudRect *c = &lay[HUD_SLOT_I_COUNT];
    snprintf(buf, sizeof buf, "%d", (int)prog.estus);  /* progress.c (A3) */
    hud_text(c->x + c->w / 2.0f + hud_text_width(buf) / 2, c->y, buf, 0x80, 0x80, 0x80, 1);
    const HudRect *n = &lay[HUD_ITEM_NAME];
    hud_text(n->x, n->y, hud_string(STR_ESTUS), 0x80, 0x80, 0x80, 0);
    /* souls */
    draw_layout(HUD_SOUL_PLATE, 0x80);
    draw_layout(HUD_SOUL_LINE, 0x80);
    const HudRect *st = &lay[HUD_SOUL_TEXT];
    snprintf(buf, sizeof buf, "%d", (int)prog.souls);
    hud_text(st->x + st->w, st->y, buf, 0x80, 0x80, 0x80, 1);
    boss_bars();
    /* YOU DIED while the player is dead, gone after the respawn. menu.drb scene F23_open_YouDied (90 frames): BG, the
     * gradients and the band run ANIM F23_fead (alpha key 0 -> 255 at frame 30), main_text_youdied runs
     * F23_Died_main_text (alpha keys 0, 20 at frame 30, 255 at 90; scale 85 % -> 100 % over 90 frames about its SHPR
     * scaling origin 640, 360). DRB frames are 1/60 s: DSR's text grows 0.10 / s (reference capture, 2026-11-03).
     * DSR blends the UI in linear light: its text read 0.32 / 0.59 / 0.72 of full at frames 34 / 42 / 47, which the
     * keys match as key^(1/2.2) (0.41 / 0.54 / 0.61), not as they are (0.14 / 0.26 / 0.34); the PS2 blends the
     * framebuffer values, so the keys are converted (TECH_DEBT hud-7). */
#ifdef DIED_TEST  /* screenshot build (make EE_DBGINFOFLAGS=-DDIED_TEST): banner after 3 s */
    static float tt;
    died_t = (tt += dt) > 3 ? died_t + dt : 0;
#else
    died_t = p->hp < 0 ? died_t + dt : 0;  /* the ESD's death test; hp is back at hp_max after the respawn */
#endif
    if (died_t > 0) {
        float f = died_t * 60;
        int ba = f >= 30 ? 0x80 : (int)(0x80 * powf(f / 30, 1 / 2.2f));
        for (int i = HUD_DIED_BG; i < HUD_DIED_TEXT; i++)
            draw_layout(i, ba);
        float ta = f >= 90 ? 1 : f < 30 ? 20 / 255.0f * f / 30 : (20 + 235 * (f - 30) / 60) / 255;
        float s = f >= 90 ? 1 : 0.85f + 0.15f * f / 90, cx = 640 * 640 / 1280.0f, cy = 360 * 448 / 720.0f;
        const HudRect *e = &lay[HUD_DIED_TEXT];
        hud_sprite(e->sprite, cx + (e->x - cx) * s, cy + (e->y - cy) * s, e->w * s, e->h * s, 0x80, 0x80, 0x80,
                   (int)(0x80 * powf(ta, 1 / 2.2f)));
    }
    bonfire_hud(dt);  /* bonfire prompt, rest menu, dialogs, BONFIRE LIT (src/bonfire.c) */
    /* an event's action button in reach (IfActionButton's help message id: "Open", "Examine" ...), with the PS2
     * action button's symbol (Cross) */
    const char *ht = event_stats.prompt >= 0 && p->hp > 0 && !menu_open() ? hud_help(event_stats.prompt) : NULL;
    if (ht)
        hud_prompt(320, 330, HUD_BTN_CROSS, ht);
    hud_end();
}
