/* Bonfire rest and the death loop (bonfire.h) */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <libpad.h>

#include "bonfire.h"
#include "enemy.h"
#include "ev_misc.h"
#include "event.h"
#include "flags.h"
#include "fx.h"
#include "hud.h"
#include "progress.h"
#include "save.h"

/* The respawn point a rest sets: the bonfire object's entity id + 1000 (exe FUN_1404ccf20 -> FUN_1402c9bf0(entity +
 * 1000)), its MSB SpawnPoint event 復活イベントNN (Firelink 1021960 -> 1022960, Burg 1011961 -> 1012961;
 * tools/export_emevd.py exports the spawn points) */
#define SPAWN_OFFSET 1000
/* Walking within this of the bloodstain (feet, horizontal) gives its souls back. ASSUMED (TECH_DEBT a4-2): DSR's
 * pickup is the bloodstain SFX's touch check */
#define STAIN_R 1.0f
#define STAIN_DY 2.0f

static u32 area_id;
static V3 ground;      /* last spot the player stood on (alive): the bloodstain's place on a death */
static int ground_ok;
static Player *pl;     /* bonfire_update's player, for a rest from the action button */
static int armed;      /* the player has been away from the stain since it was dropped (one dropped at the respawn
                        * point is not picked up on the spawn) */

/* ---- bonfire use: eventCommon global_event.lua + the bonfire talk ESD (t1xx1xx / t1xx2xx) + exe FUN_1404ccf20 ----
 * Light: Lua_BonfireFirstInjectAnim plays a00 7698; its TAE 231 message 30 (OnEvent_BonfireFirstLvUp) lights the
 *   bonfire (Util_RequestLevelUpFirst: 10 kindle points) and 0.6 s later SetTextEffect(TEXT_TYPE_Bonfire) shows BONFIRE
 *   LIT. Lighting does not sit the player (the talk ESD goes back to its prompt state).
 * Rest: the talk ESD's StartBonfireAnimLoop -> Lua_BonfireLoopAnimBegin: a00 7700 + 10 x GetRandom(0, 2) (sit down),
 *   its message 20 (OnEvent_BonfireRespawn) = Util_RequestRegene + Util_RequestRespawn: HP and Estus (to the bonfire's
 *   CalcCorrectGraph 150 count) now, SFX 90010 / 90011, and 1.333 s later (exe timer 0x3faaaaab) the world respawn and
 *   respawn point = bonfire + 1000 (FUN_1402c9bf0). Then the sit loop (+1) with the menu; leaving plays the stand-up
 *   (+2: Lua_BonfireLoopAnimEnd).
 * Kindle (menu): Lua_BonfireInjectAnim a00 7699, message 10 (OnEvent_BonfireLvUp): Util_RequestLevelUp = 1 humanity
 *   -> 10 points (FUN_1404ccf20) and Regene (the Estus of the new count), then the sit loop again. */
enum { BF_IDLE, BF_LIGHT, BF_SIT, BF_LOOP, BF_KINDLE, BF_STAND };
#define ANIM_LIGHT 7698
#define ANIM_KINDLE 7699
#define ANIM_SIT 7700      /* + 10 x variant; loop + 1, stand + 2 */
#define MSG_LIGHT 30       /* OnCheckEzStateMessage(4080, ..., 30) */
#define MSG_KINDLE 10      /* (4081, 10) */
#define MSG_REST 20        /* (4082, 20) */
#define LIT_TEXT_DELAY 0.6f   /* OnEvent_BonfireFirstLvUp: OnKeyTime2(..., 0.6) */
#define RESPAWN_DELAY 1.3333334f  /* FUN_1404ccf20: param_1 + 0x88 = 0x3faaaaab after the respawn request */
/* BONFIRE LIT on screen: fade in, hold, fade out. ASSUMED (TECH_DEBT a4-7): the text effect's own timing is not
 * decoded */
#define LIT_IN 0.5f
#define LIT_HOLD 3.0f
#define LIT_OUT 1.0f
#define FLAG_RITE 257      /* the talk ESD's Kindle gate past stage 2 (Rite of Kindling) */

static int bf_state;
static s32 bf_entity;
static int bf_variant;
static float bf_t;          /* time in the current anim */
static int bf_msg_done;     /* this anim's message handled */
static float respawn_t;     /* > 0: world respawn pending */
static float lit_t;         /* BONFIRE LIT banner clock (starts at -LIT_TEXT_DELAY) */
static int lit_on;
static int lit_text = HUD_LIT_TEXT;  /* the banner's text: BONFIRE LIT, or HUMANITY RESTORED (HUD_REV_TEXT) */
static int last_level = -2; /* bf_entity's kindle stage last frame (an increase into 1..4 plays the ignite SFX) */
static unsigned short test_pressed;  /* "ev bf pad ..." */

/* rest menu: AddTalkListData(menu index, text, required flag) of the talk ESD's state 4, in call order. Shown order
 * ASSUMED (TECH_DEBT a4-7): call order with Leave (added first, index 9) last */
static const struct { int index, str; s32 flag; } MENU[] = {
    {1, STR_BF_LEVEL_UP, 11810000}, {2, STR_BF_REINFORCE_WEP, 250}, {3, STR_BF_REINFORCE_ARMOR, 251},
    {4, STR_BF_REPAIR, 252}, {5, STR_BF_ATTUNE, 719}, {10, STR_BF_BOX, 258}, {7, STR_BF_WARP, 710},
    {12, STR_BF_WARP, 717}, {6, STR_BF_COVENANT, 69696969}, {8, STR_BF_KINDLE, 11810000},
    {11, STR_BF_REVERSE_HOLLOWING, 11810000}, {9, STR_BF_LEAVE, -1}};
#define MENU_N ((int)(sizeof MENU / sizeof MENU[0]))
static int menu_open, menu_rows[MENU_N], menu_n, menu_cur;
/* dialog: OpenGenericDialog 7 (one button) / 8 (yes / no) */
static int dlg_str = -1, dlg_yesno, dlg_cur, dlg_action;
enum { DLG_NONE, DLG_KINDLE, DLG_REVERSE };
static float stub_t;        /* a menu entry the port does not have yet: its name blinks (A11) */
static int stub_str;

void bonfire_init(const char *area)
{
    area_id = progress_map_id(area);
}

/* Util_RequestRegene: HP and the Estus count of the bonfire (thunk_FUN_14287b723 with FUN_1407a41b0's count) */
static void regene(s32 entity)
{
    prog.estus = prog.estus_max = ev_bonfire_estus(entity);
    if (pl)
        pl->hp = pl->hp_max;
    progress_touch();
}

/* the world part of a rest, 1.333 s after the request: respawn point, enemies, events, autosave */
static void rest_world(s32 entity)
{
    float p[3], yaw;
    prog.bonfire = entity;
    if (event_entity(entity + SPAWN_OFFSET, p, &yaw))
        prog.respawn = entity + SPAWN_OFFSET;
    else
        printf("bonfire %d: no spawn point %d, respawn point stays %d\n", (int)entity, (int)(entity + SPAWN_OFFSET),
               (int)prog.respawn);
    progress_touch();
    enemy_respawn();
    event_rest();  /* temporary flags, Restart / End on rest */
    save_request();
    printf("bonfire %d rest: respawn %d, HP %d, Estus %d\n", (int)entity, (int)prog.respawn, pl ? pl->hp : -1,
           (int)prog.estus);
}

void bonfire_rest(s32 entity)
{
    regene(entity);
    rest_world(entity);
}

void bonfire_death(const Player *p)
{
    V3 at = ground_ok ? ground : p->pos;
    /* DSR: the souls and humanity held go into a bloodstain where the player last stood; an older stain is lost */
    prog.stain_map = 0;
    if (prog.souls > 0 || prog.humanity > 0) {
        prog.stain_map = area_id;
        prog.stain_pos[0] = at.x, prog.stain_pos[1] = at.y, prog.stain_pos[2] = at.z;
        prog.stain_souls = prog.souls, prog.stain_humanity = prog.humanity;
    }
    armed = 0;
    printf("bonfire: death, bloodstain %d souls %d humanity at (%.2f %.2f %.2f)\n", (int)prog.souls,
           (int)prog.humanity, at.x, at.y, at.z);
    prog.souls = prog.humanity = 0;
    prog.chr_type = CHR_HOLLOW;  /* DSR: a death in human form makes the player Hollow */
    progress_touch();
}

int bonfire_spawn(V3 *pos, float *yaw)
{
    float p[3];
    if (!prog.respawn || !event_entity(prog.respawn, p, yaw))
        return 0;
    *pos = v3(p[0], p[1], p[2]);  /* the spawn point regions sit on the floor (Firelink 復活ポイント00 = c0000_0000) */
    return 1;
}

void bonfire_revive(Player *p)
{
    p->hp = p->hp_max;
    /* a respawn at a lit bonfire refills Estus to its count (exe FUN_1404de720); else the last maximum */
    prog.estus = prog.bonfire && ev_bonfire_points(prog.bonfire) ? ev_bonfire_estus(prog.bonfire) : prog.estus_max;
    progress_touch();
    enemy_respawn();
    event_rest();  /* temporary flags, Restart / End on rest (a death reloads the map in DSR: TECH_DEBT a4-4) */
    ground = p->pos, ground_ok = 1;
    bf_state = BF_IDLE, menu_open = 0, dlg_str = -1, respawn_t = 0;
}

static int anim_sit(int k) { return ANIM_SIT + 10 * bf_variant + k; }

static void face_bonfire(s32 entity)
{
    float b[3];
    if (pl && ev_bonfire_pos(entity, b))  /* NoAnimeTurnCharactor: turned at once */
        pl->yaw = atan2f(b[0] - pl->pos.x, b[2] - pl->pos.z);
}

static void play(int id)
{
    bf_t = 0, bf_msg_done = 0;
    if (pl && player_event_anim(pl, id) <= 0)
        printf("bonfire: anim %d not in this build\n", id);
}

int bonfire_busy(void) { return bf_state != BF_IDLE; }

int bonfire_use(s32 entity)
{
    if (!pl || bf_state != BF_IDLE || pl->hp <= 0 || pl->airborne || (pl->state != ST_IDLE && pl->state != ST_MOVE))
        return 0;
    bf_entity = entity;
    last_level = ev_bonfire_level(entity);
    face_bonfire(entity);
    if (last_level == 0) {  /* the talk ESD's state 25: unlit -> light it */
        bf_state = BF_LIGHT;
        play(ANIM_LIGHT);
    } else {
        bf_variant = rand() % 3;  /* GetRandom(0, 2) */
        bf_state = BF_SIT;
        play(anim_sit(0));
    }
    printf("bonfire %d: %s\n", (int)entity, bf_state == BF_LIGHT ? "light" : "sit");
    return 1;
}

static void menu_build(void)
{
    int warp = 0;
    menu_n = 0;
    for (int i = 0; i < MENU_N; i++) {
        if (MENU[i].index == 9 || (MENU[i].flag >= 0 && !flag_get(MENU[i].flag)))
            continue;
        if (MENU[i].str == STR_BF_WARP && warp++)  /* entries 7 / 12 share the text: one Warp */
            continue;
        menu_rows[menu_n++] = i;
    }
    for (int i = 0; i < MENU_N; i++)
        if (MENU[i].index == 9)
            menu_rows[menu_n++] = i;
    menu_cur = menu_cur < menu_n ? menu_cur : 0;
}

static void stand_up(void)
{
    menu_open = 0, dlg_str = -1;
    bf_state = BF_STAND;
    play(anim_sit(2));
}

static void dialog(int str, int yesno, int action)
{
    dlg_str = str, dlg_yesno = yesno, dlg_cur = 0, dlg_action = action;
}

/* talk ESD state 32 (Kindle) / 51 (Reverse Hollowing) */
static void menu_pick(int index)
{
    int lv = ev_bonfire_level(bf_entity);
    switch (index) {
    case 9: stand_up(); break;
    case 8:
        if (prog.chr_type == CHR_HOLLOW)
            dialog(STR_BF_KINDLE_HOLLOW, 0, DLG_NONE);
        else if (prog.humanity < 1)  /* ESD: GetStatus(10) <= 1 -> "No humanity"; DSR kindles with 1 (TECH_DEBT a4-7) */
            dialog(STR_BF_NO_HUMANITY, 0, DLG_NONE);
        else if (lv >= 4)
            dialog(STR_BF_KINDLE_MAX, 0, DLG_NONE);
        else if (lv >= 2 && !flag_get(FLAG_RITE))
            dialog(STR_BF_KINDLE_RITE, 0, DLG_NONE);
        else
            dialog(STR_BF_ASK_KINDLE, 1, DLG_KINDLE);
        break;
    case 11:
        if (prog.chr_type == CHR_HUMAN)
            dialog(STR_BF_NOT_HOLLOW, 0, DLG_NONE);
        else if (prog.humanity < 1)
            dialog(STR_BF_NO_HUMANITY2, 0, DLG_NONE);
        else
            dialog(STR_BF_ASK_REVERSE, 1, DLG_REVERSE);
        break;
    default:  /* Level Up, Attune, Reinforce, Repair, Box, Warp, Covenant: menus the port does not have (A11) */
        for (int i = 0; i < MENU_N; i++)
            if (MENU[i].index == index)
                stub_str = MENU[i].str, stub_t = 1.0f;
        printf("bonfire: menu %d not in this build\n", index);
    }
}

static void dialog_done(int yes)
{
    int act = dlg_action;
    dlg_str = -1;
    if (!yes || act == DLG_NONE)
        return;
    if (act == DLG_KINDLE) {  /* OfferHumanity -> Lua_BonfireInjectAnim */
        menu_open = 0;
        bf_state = BF_KINDLE;
        play(ANIM_KINDLE);
    } else if (act == DLG_REVERSE) {  /* PlayerRespawn, DisplayBanner(3), SpEffect 25, humanity - 1 */
        prog.humanity--;
        prog.chr_type = CHR_HUMAN;
        progress_touch();
        /* DisplayBanner(3) = F23_YouRevived (FUN_140709d40's table 0x141a32a60); shown at once with BONFIRE LIT's
         * fades (ASSUMED, TECH_DEBT a4-6: the exe's banner time 0x141c88d98 + 0xf6c is a runtime value) */
        lit_t = 0, lit_on = 1, lit_text = HUD_REV_TEXT;
        printf("bonfire: hollowing reversed, humanity %d\n", (int)prog.humanity);
    }
}

int bonfire_input(unsigned short pressed)
{
    pressed |= test_pressed;
    test_pressed = 0;
    if (bf_state == BF_IDLE)
        return 0;
    if (dlg_str >= 0) {
        if (dlg_yesno && (pressed & (PAD_LEFT | PAD_RIGHT | PAD_UP | PAD_DOWN)))
            dlg_cur ^= 1;
        if (pressed & PAD_CROSS)
            dialog_done(dlg_yesno && dlg_cur == 0);
        else if (pressed & PAD_CIRCLE)
            dialog_done(0);
    } else if (menu_open) {
        if (pressed & PAD_UP)
            menu_cur = (menu_cur + menu_n - 1) % menu_n;
        if (pressed & PAD_DOWN)
            menu_cur = (menu_cur + 1) % menu_n;
        if (pressed & PAD_CROSS)
            menu_pick(MENU[menu_rows[menu_cur]].index);
        else if (pressed & PAD_CIRCLE)
            stand_up();
    }
    return 1;
}

/* the bonfire use, once a frame after the player moved */
static void use_update(float dt)
{
    int lv = bf_entity ? ev_bonfire_level(bf_entity) : -1;
    if (bf_entity && lv > last_level && lv >= 1)  /* fire id changed into 90001..90004: ignite SFX 90007 */
        fx_bonfire_burst(bf_entity, 0);
    last_level = lv;
    if (respawn_t > 0 && (respawn_t -= dt) <= 0)
        rest_world(bf_entity);
    if (lit_on)
        lit_t += dt;
    if (stub_t > 0)
        stub_t -= dt;
    if (bf_state == BF_IDLE || !pl)
        return;
    if (pl->hp <= 0) {  /* IsPlayerDead: the talk ends */
        bf_state = BF_IDLE, menu_open = 0, dlg_str = -1;
        return;
    }
    bf_t += dt;
    int id = player_event_anim_id(pl);
    switch (bf_state) {
    case BF_LIGHT: {
        float m = player_anim_message(ANIM_LIGHT, MSG_LIGHT);
        if (!bf_msg_done && bf_t >= m) {
            bf_msg_done = 1;
            ev_bonfire_light(bf_entity);
            lit_t = -LIT_TEXT_DELAY, lit_on = 1, lit_text = HUD_LIT_TEXT;
        }
        if (id != ANIM_LIGHT)
            bf_state = BF_IDLE;
        break;
    }
    case BF_SIT: {
        float m = player_anim_message(anim_sit(0), MSG_REST);
        if (!bf_msg_done && bf_t >= m) {  /* OnEvent_BonfireRespawn */
            bf_msg_done = 1;
            regene(bf_entity);
            fx_bonfire_burst(bf_entity, 1);
            respawn_t = RESPAWN_DELAY;
        }
        if (id != anim_sit(0)) {  /* OnChrAnimEnd -> Lua_BonfireLoopAnimBegin_1: the loop, menu */
            bf_state = BF_LOOP;
            play(anim_sit(1));
            menu_open = 1;
            menu_build();
        }
        break;
    }
    case BF_KINDLE: {
        float m = player_anim_message(ANIM_KINDLE, MSG_KINDLE);
        if (!bf_msg_done && bf_t >= m) {  /* OnEvent_BonfireLvUp: 1 humanity -> 10 points, and Regene */
            bf_msg_done = 1;
            if (prog.humanity > 0) {
                prog.humanity--;
                ev_bonfire_add_points(bf_entity, 10);
            }
            regene(bf_entity);
            printf("bonfire %d kindled: points %d, Estus %d\n", (int)bf_entity, ev_bonfire_points(bf_entity),
                   (int)prog.estus);
        }
        if (id != ANIM_KINDLE) {
            bf_state = BF_LOOP;
            play(anim_sit(1));
            menu_open = 1;
            menu_build();
        }
        break;
    }
    case BF_LOOP:
        if (id != anim_sit(1))  /* something else took the player (a hit): the talk ends */
            bf_state = BF_IDLE, menu_open = 0, dlg_str = -1;
        break;
    case BF_STAND:
        if (id != anim_sit(2))
            bf_state = BF_IDLE;
        break;
    }
}

/* DSR's one-line help / talk list / generic dialog boxes: dark panels with DSFont24 text (layout ASSUMED, TECH_DEBT
 * a4-7: the dialogs' DRB layouts are A11's) */
static void box(float x, float y, float w, float h) { hud_fill(x, y, w, h, 0, 0, 0, 0x60); }

void bonfire_hud(float dt)
{
    (void)dt;
    int lh = hud_line_height();
    if (lit_on && lit_t > 0) {  /* BONFIRE LIT: F23_Bonfire; HUMANITY RESTORED: F23_YouRevived (same backdrop) */
        float a = lit_t < LIT_IN ? lit_t / LIT_IN : lit_t < LIT_HOLD ? 1 : 1 - (lit_t - LIT_HOLD) / LIT_OUT;
        if (a <= 0)
            lit_on = 0;
        else {
            for (int i = HUD_LIT_BG; i < HUD_LIT_TEXT; i++)
                hud_layout(i, (int)(0x80 * a));
            hud_layout(lit_text, (int)(0x80 * a));
        }
    }
    if (bf_state == BF_IDLE && pl && pl->hp > 0) {  /* DisplayOneLineHelp 10010182 / 10010183 */
        s32 e = ev_bonfire_near((const float[3]){pl->pos.x, pl->pos.y, pl->pos.z}, pl->yaw);
        if (e) {
            const char *t = hud_string(ev_bonfire_level(e) ? STR_BF_REST : STR_BF_LIGHT);
            float w = hud_text_width(t) + 40;
            box(320 - w / 2, 330, w, lh + 10);
            hud_text(320 - w / 2 + 20, 335, t, 0x80, 0x80, 0x80, 0);
        }
    }
    if (menu_open) {
        float w = 220, x = 60, y = 120, rh = lh + 6;
        box(x, y, w, menu_n * rh + 16);
        for (int i = 0; i < menu_n; i++) {
            int str = MENU[menu_rows[i]].str, c = i == menu_cur ? 0x80 : 0x60;
            if (i == menu_cur)
                hud_fill(x + 6, y + 8 + i * rh, w - 12, rh, 0x60, 0x50, 0x30, 0x40);
            if (stub_t > 0 && str == stub_str && i == menu_cur)
                c = 0x40;
            hud_text(x + 16, y + 11 + i * rh, hud_string(str), c, c, c, 0);
        }
    }
    if (dlg_str >= 0) {
        const char *t = hud_string(dlg_str);
        float w = hud_text_width(t) + 60;
        w = w < 260 ? 260 : w;
        float x = 320 - w / 2, y = 190;
        box(x, y, w, 2 * lh + 34);
        hud_text(x + 30, y + 10, t, 0x80, 0x80, 0x80, 0);
        const char *b0 = dlg_yesno ? "Yes" : "OK", *b1 = "No";  /* ASSUMED button labels (A11: the FMG's) */
        for (int k = 0; k < 1 + dlg_yesno; k++) {
            int c = k == dlg_cur ? 0x80 : 0x58;
            hud_text(x + w / 2 - (dlg_yesno ? 60 : 10) + k * 90, y + 20 + lh, k ? b1 : b0, c, c, c, 0);
        }
    }
}

void bonfire_update(Player *p, float dt)
{
    pl = p;
    use_update(dt);
    if (p->hp > 0 && !p->airborne && p->state != ST_DEAD)
        ground = p->pos, ground_ok = 1;
    if (!prog.stain_map || prog.stain_map != area_id || p->hp <= 0)
        return;
    float dx = p->pos.x - prog.stain_pos[0], dz = p->pos.z - prog.stain_pos[2];
    if (dx * dx + dz * dz > STAIN_R * STAIN_R || fabsf(p->pos.y - prog.stain_pos[1]) > STAIN_DY) {
        armed = 1;
        return;
    }
    if (!armed)
        return;
    progress_add_souls(prog.stain_souls);
    prog.humanity += prog.stain_humanity;
    printf("bonfire: bloodstain picked up: %d souls, %d humanity\n", (int)prog.stain_souls, (int)prog.stain_humanity);
    prog.stain_map = 0, prog.stain_souls = prog.stain_humanity = 0;
    progress_touch();
}

void bonfire_draw(void)
{
    if (!prog.stain_map || prog.stain_map != area_id)
        return;
    /* ASSUMED stand-in (TECH_DEBT a4-2): a flat green box for DSR's bloodstain SFX */
    r_box(v3(prog.stain_pos[0], prog.stain_pos[1], prog.stain_pos[2]), m3_rot_y(0), v3(0, 0.1f, 0),
          v3(0.35f, 0.1f, 0.35f), 0x30, 0xC0, 0x50);
}

int bonfire_test_cmd(const char *cmd, int *pass, int *fail)
{
    char what[16];
    int v, e;
    if (sscanf(cmd, "hurt %d", &v) == 1) {  /* damage to the player (an enemy hit without the sweep) */
        if (pl && pl->hp > 0) {
            pl->hp -= v;
            if (pl->hp <= 0)
                pl->hp = -1;  /* the ESD death branches test f1000() < 0 */
        }
        printf("TEST ev hurt %d: HP %d\n", v, pl ? pl->hp : -999);
        return 1;
    }
    char word[16];
    if (sscanf(cmd, "bf pad %15s", word) == 1) {
        static const struct { const char *n; unsigned short b; } B[] = {{"up", PAD_UP}, {"down", PAD_DOWN},
            {"left", PAD_LEFT}, {"right", PAD_RIGHT}, {"cross", PAD_CROSS}, {"circle", PAD_CIRCLE}};
        for (int i = 0; i < 6; i++)
            if (!strcmp(word, B[i].n))
                test_pressed |= B[i].b;
        printf("TEST ev bf pad %s\n", word);
        return 1;
    }
    if (sscanf(cmd, "bf pick %d", &v) == 1) {
        for (int i = 0; i < menu_n; i++)
            if (MENU[menu_rows[i]].index == v)
                menu_cur = i;
        printf("TEST ev bf pick %d: row %d of %d\n", v, menu_cur, menu_n);
        return 1;
    }
    if (sscanf(cmd, "humanity %d", &v) == 1) {
        prog.humanity = v;
        progress_touch();
        return 1;
    }
    int k3;
    if (sscanf(cmd, "killed %d %d %d", &v, &e, &k3) == 3) {  /* an enemy death's award: souls, humanity points, zone */
        printf("TEST ev killed %d %d %d: +%d humanity\n", v, e, k3, progress_enemy_killed(v, e, k3));
        return 1;
    }
    if (sscanf(cmd, "hollow %d", &v) == 1) {
        prog.chr_type = v ? CHR_HOLLOW : CHR_HUMAN;
        progress_touch();
        return 1;
    }
    if (sscanf(cmd, "expect %15s %d %d", what, &v, &e) == 3 && (!strcmp(what, "points") || !strcmp(what, "level"))) {
        int got = what[0] == 'p' ? ev_bonfire_points(v) : ev_bonfire_level(v), ok = got == e;
        *pass += ok, *fail += !ok;
        printf("TEST ev expect %s %d %d: %d %s\n", what, v, e, got, ok ? "PASS" : "FAIL");
        return 1;
    }
    float off;
    if (sscanf(cmd, "stain go %f", &off) == 1) {  /* onto the bloodstain, or `off` m east of it facing it */
        if (pl && prog.stain_map)
            pl->pos = v3(prog.stain_pos[0] + off, prog.stain_pos[1], prog.stain_pos[2]), pl->yaw = -1.5707963f,
            pl->vel = v3(0, 0, 0), pl->fall_y0 = pl->pos.y;
        printf("TEST ev stain go %.1f: %s\n", off, prog.stain_map ? "ok" : "no stain");
        return 1;
    }
    if (sscanf(cmd, "expect %15s %d", what, &v) != 2)
        return 0;
    int got;
    if (!strcmp(what, "estus"))
        got = prog.estus;
    else if (!strcmp(what, "hp"))  /* -1 dead, else HP */
        got = pl ? pl->hp : -999;
    else if (!strcmp(what, "stain"))  /* souls in the bloodstain of this area, -1 none */
        got = prog.stain_map == area_id ? prog.stain_souls : -1;
    else if (!strcmp(what, "stainx") || !strcmp(what, "stainz"))  /* its position, whole metres */
        got = (int)floorf(prog.stain_pos[what[5] == 'x' ? 0 : 2]);
    else if (!strcmp(what, "souls"))
        got = prog.souls;
    else if (!strcmp(what, "humanity"))
        got = prog.humanity;
    else if (!strcmp(what, "lastbonfire"))
        got = (int)prog.bonfire;
    else if (!strcmp(what, "estusmax"))
        got = prog.estus_max;
    else if (!strcmp(what, "hollow"))
        got = prog.chr_type == CHR_HOLLOW;
    else if (!strcmp(what, "bfstate"))  /* 0 idle 1 light 2 sit 3 loop (menu) 4 kindle 5 stand */
        got = bf_state;
    else if (!strcmp(what, "menu"))  /* rest menu rows shown, 0 closed */
        got = menu_open ? menu_n : 0;
    else if (!strcmp(what, "banner"))  /* on screen: 0 none, 1 BONFIRE LIT, 2 HUMANITY RESTORED */
        got = lit_on && lit_t > 0 ? 1 + (lit_text == HUD_REV_TEXT) : 0;
    else
        return 0;
    int ok = got == v;
    *pass += ok, *fail += !ok;
    printf("TEST ev expect %s %d: %d %s\n", what, v, got, ok ? "PASS" : "FAIL");
    return 1;
}
