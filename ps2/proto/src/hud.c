#include <kernel.h>
#include <string.h>
#include <stdio.h>
#include "enemy.h"
#include "hud.h"
#include "bonfire.h"
#include "render.h"
#include "progress.h"

typedef struct {
    char magic[4];
    unsigned version, atlases, sprites, glyphs, layout, strings;
    unsigned atlas_off, sprite_off, glyph_off, layout_off, string_off;
    unsigned short font_h, icon0;
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
    if (memcmp(hdr->magic, "DSMN", 4) || hdr->atlases != 2 || hdr->layout != HUD_LAYOUT_COUNT ||
        hdr->strings != STR_COUNT)
        return -1;
    fail = "HUD VRAM FULL";
    spr = (const Sprite *)(mb + hdr->sprite_off);
    gly = (const Glyph *)(mb + hdr->glyph_off);
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
const HudRect *hud_rect(int i) { return &lay[i]; }

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

static void draw_layout(int i, int a)
{
    const HudRect *e = &lay[i];
    if (e->sprite >= 0)
        hud_sprite(e->sprite, e->x, e->y, e->w, e->h, 0x80, 0x80, 0x80, a);
    else
        hud_fill(e->x, e->y, e->w, e->h, e->r, e->g, e->b, e->a * a / 0x80);
}

void hud_layout(int i, int a) { draw_layout(i, a); }

float hud_text_width(const char *s)
{
    float w = 0;
    for (; ok && *s; s++)
        if (*s >= 32 && *s < 127)
            w += gly[*s - 32].adv;
    return w;
}

float hud_text(float x, float y, const char *s, int r, int g, int b, int align)
{
    if (!ok)
        return 0;
    float w = hud_text_width(s);
    if (align)
        x -= w;
    for (; *s; s++) {
        if (*s < 32 || *s >= 127)
            continue;
        const Glyph *gl = &gly[*s - 32];
        if (*s != ' ')
            gsKit_prim_sprite_texture(gs, &atlas[0], R_HX(x + gl->pre), y, gl->u, gl->v, R_HX(x + gl->pre + gl->w),
                                      y + gl->h,
                                      gl->u + gl->w, gl->v + gl->h, HUD_Z, rgba(r, g, b, 0x80));
        x += gl->adv;
    }
    return w;
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
    /* YOU DIED: fades in over 1.5 s while the player is dead (DSR F23_YouDied), gone after the respawn */
#ifdef DIED_TEST  /* screenshot build (make EE_DBGINFOFLAGS=-DDIED_TEST): banner after 3 s */
    static float tt;
    died_t = (tt += dt) > 3 ? died_t + dt : 0;
#else
    died_t = p->hp < 0 ? died_t + dt : 0;  /* the ESD's death test; hp is back at hp_max after the respawn */
#endif
    if (died_t > 0) {
        int a = died_t > 1.5f ? 0x80 : (int)(0x80 * died_t / 1.5f);
        for (int i = HUD_DIED_BG; i <= HUD_DIED_TEXT; i++)
            draw_layout(i, a);
    }
    bonfire_hud(dt);  /* bonfire prompt, rest menu, dialogs, BONFIRE LIT (src/bonfire.c) */
    hud_end();
}
