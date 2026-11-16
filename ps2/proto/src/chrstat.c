/* Player derived values (chrstat.h). Sources in DarkSoulsRemastered.exe:
 * - max HP: CalcCorrectGraph row 100 at VIT (FUN_1402df9a0 -> lookup FUN_1402dfb70)
 * - max stamina: FUN_1402dfcd0 at END (player.c had it first)
 * - base defence: FUN_1402dc850 sums the stats with the weights at 0x141A2B6A0..0x141A2B73C, one sum per element;
 *   FUN_1402dbda0 maps each through CalcCorrectGraph row 102 (+0.5, truncated) and adds the armour: per piece
 *   EquipParamProtector defense<element>, and for physical defence against a typed attack (param 1 / 2 / 3)
 *   x (defenseSlash / Blow / Thrust + 100) x 0.01 (0x1412B2334, 0x1412DC48C)
 * - weapon attack rating: FUN_1402d9cb0: per element base x stat factor; physical scales with STR / DEX, magic, fire
 *   and lightning with INT / FTH (correctMagic / correctFaith); the two factors combine as (a - 1) + (b - 1) + 1, or
 *   the smaller one when either is under 1 (a requirement not met). Factor FUN_1402dacd0: stat >= requirement ->
 *   1 + graph(stat) / 100 x correct / 100 (graph = the weapon's correctType row); stat < requirement -> a penalty
 *   curve of t = (1 - stat / requirement) x 100 capped at 20 (0x141A2B5D4 / 0x141A2B5D8): ((t - 20)^2 k + 100 -
 *   400 k) / 100 with k = (20 - 100) / (1 - 20^2).
 * PlayerGameData stat offsets used by those functions: +0x40 VIT, +0x48 ATT, +0x50 END (PORTING.md stamina recalc),
 * +0x58 STR, +0x60 DEX (FUN_1402d9cb0 physical pair), +0x68 INT, +0x70 FTH (its magic pair, correctMagic /
 * correctFaith), +0x84 humanity (FUN_1402dddd0 curse resist: min(x, 99) into graph 113), +0x88 RES (ASSUMED: the
 * stat after humanity in CharaInitParam's base* order, baseDurability). */
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "chrstat.h"
#include "combat.h"
#include "combat_data.h"
#include "items.h"
#include "progress.h"

ChrStats chr_stats;

const float *chrstat_graph(int id)
{
    switch (id) {
    case 100: return graph_100;
    case 102: return graph_102;
    case 113: return graph_113;
    case 110: return graph_110;
    case 111: return graph_111;
    case 112: return graph_112;
    case 150: return graph_150;
    }
    for (int i = 0; i < NGRAPHS; i++)
        if (graphs[i].id == id)
            return graphs[i].g;
    return 0;
}

/* FUN_1402dacd0 (see the top) */
static float stat_factor(int stat, int req, float correct, const float *graph)
{
    if (req > stat && req > 0) {
        const float cap = 20, k = (20.0f - 100.0f) / (1 - cap * cap);
        float t = (1 - (float)stat / req) * 100;
        if (t > cap)
            t = cap;
        return ((t - cap) * k * (t - cap) + (100 - cap * k * cap)) / 100;
    }
    if (correct <= 0 || !graph)
        return 1;
    return combat_graph(graph, stat) / 100 * (correct / 100) + 1;
}

static float combine(float a, float b)
{
    if (a < 1 || b < 1)
        return a < b ? a : b;
    return (a - 1) + (b - 1) + 1;
}

int chrstat_weapon_ar(unsigned id, float ar[EL_COUNT])
{
    memset(ar, 0, EL_COUNT * sizeof *ar);
    const ItemRow *r = id ? item_row(id) : 0;
    if (!r || ITEM_CAT(id) != ITEM_WEAPON)
        return 0;
    const ItemReinforce *rf = item_reinforce(r->x2, ITEM_LEVEL(id));
    const float *g = chrstat_graph(r->x3);
    const u8 *s = prog.stats;
    float c[4];
    for (int k = 0; k < 4; k++)  /* correctStrength / Agility / Magic / Faith x the level's correct rate */
        c[k] = r->st[4 + k] * (rf ? rf->cor[k] / 1000.0f : 1);
    float phys = combine(stat_factor(s[ST_STR], r->st[8], c[0], g), stat_factor(s[ST_DEX], r->st[9], c[1], g));
    float mag = combine(stat_factor(s[ST_INT], r->st[10], c[2], g), stat_factor(s[ST_FAI], r->st[11], c[3], g));
    for (int k = 0; k < EL_COUNT; k++)
        ar[k] = r->st[k] * (rf ? rf->atk[k] / 1000.0f : 1) * (k == EL_PHYS ? phys : mag);
    return 1;
}

/* FUN_1402dc850 weights (0x141A2B6A0..73C) over VIT ATT END STR DEX INT FTH, min(humanity, 99), RES per element */
static const float DEF_W[EL_COUNT][9] = {
    {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 1.8f},
    {0.85f, 0.85f, 0.85f, 0.85f, 0.85f, 0.85f, 2.2f, 0.85f, 0.85f},
    {0.85f, 0.85f, 0.85f, 0.85f, 0.85f, 0.85f, 0.85f, 0.85f, 2.2f},
    {1, 1, 1, 1, 1, 1, 1, 1, 1},
};

int chrstat_recalc(void)
{
    if (!items_detail_ready())
        return 0;
    ChrStats c;
    memset(&c, 0, sizeof c);
    const u8 *s = prog.stats;
    c.hp_max = (int)combat_graph(graph_100, s[ST_VIT]);
    int m = (int)(80.0f + 80.0f * powf(s[ST_END] / 40.0f, 1.4f) + 0.5f);
    c.stamina_max = m < 80 ? 80 : m > 160 ? 160 : m;
    /* equip load: everything in a weapon / ammo / armour / ring slot (as the Status screen); max 40 + END, ASSUMED
     * (TECH_DEBT menu-4) */
    for (int k = 0; k < EQ_QUICK1; k++) {
        const ItemRow *r = item_row(equip_item(k));
        if (r)
            c.equip_weight += r->weight / 100.0f;
    }
    c.equip_max = 40 + s[ST_END];
    /* base defence from stats */
    int hum = prog.humanity < 99 ? prog.humanity : 99;
    float v[9] = {s[ST_VIT], s[ST_ATT], s[ST_END], s[ST_STR], s[ST_DEX], s[ST_INT], s[ST_FAI], hum, s[ST_RES]};
    for (int e = 0; e < EL_COUNT; e++) {
        float x = 0;
        for (int i = 0; i < 9; i++)
            x += v[i] * DEF_W[e][i];
        c.def[e] = (int)(combat_graph(graph_102, x) + 0.5f);
    }
    for (int t = 0; t < 4; t++)
        c.def_typed[t] = c.def[EL_PHYS];
    /* armour: itemx protector st = physics, blow, slash, thrust, magic, fire, thunder, resists, saDurability */
    static const int typed_st[4] = {-1, 2, 1, 3};  /* ATK_SLASH -> defenseSlash, ATK_BLOW -> defenseBlow, ... */
    for (int k = EQ_HELM; k <= EQ_LEG; k++) {
        const ItemRow *r = item_row(equip_item(k));
        if (!r)
            continue;
        c.def[EL_PHYS] += r->st[0], c.def[EL_MAG] += r->st[4], c.def[EL_FIRE] += r->st[5], c.def[EL_THUN] += r->st[6];
        for (int t = 0; t < 4; t++)
            c.def_typed[t] += t ? r->st[0] * (r->st[typed_st[t]] + 100) * 0.01f : r->st[0];
        c.poise += r->st[11];
    }
    /* weapons in the active slots */
    for (int h = 0; h < 2; h++) {
        u32 id = equip_item(h == HAND_R ? EQ_WEP_R1 : EQ_WEP_L1);
        chrstat_weapon_ar(id, c.ar[h]);
        const ItemRow *r = id ? item_row(id) : 0;
        if (r) {
            c.poise_dmg[h] = r->x4 & 0xFFFF;
            c.wep_types[h] = (r->x4 >> 16) & 0xFF;
            c.guard_level[h] = (r->x4 >> 24) & 0xF;
            for (int e = 0; e < EL_COUNT; e++)
                c.guard_cut[h][e] = r->st[12 + e];
            c.guard_stab[h] = r->x0;
        }
    }
    /* resistances (FUN_1402dddd0): poison graph 110 and toxic 111 at RES (PlayerGameData +0x88), bleed 112 at END
     * (+0x50), curse 113 at min(humanity, 99) (+0x84), each + the armour's resist (ASSUMED: plain sum) */
    c.resist[0] = (int)combat_graph(graph_110, s[ST_RES]), c.resist[1] = (int)combat_graph(graph_111, s[ST_RES]);
    c.resist[2] = (int)combat_graph(graph_112, s[ST_END]), c.resist[3] = (int)combat_graph(graph_113, hum);
    for (int k = EQ_HELM; k <= EQ_LEG; k++) {
        const ItemRow *r = item_row(equip_item(k));
        for (int i = 0; r && i < 4; i++)
            c.resist[i] += r->st[7 + i];
    }
    c.valid = 1;
    chr_stats = c;
    return 1;
}

static void *boot_alloc(unsigned n) { return memalign(64, n); }
static void boot_free(void *p, unsigned n) { (void)n; free(p); }

void chrstat_boot(void)
{
    int r;
    while (!(r = items_detail_step(boot_alloc, boot_free)))
        ;
    if (r > 0)
        chrstat_recalc();
    items_detail_free();
    printf("chrstat: resist poison %.0f toxic %.0f bleed %.0f curse %.0f\n", chr_stats.resist[0], chr_stats.resist[1],
           chr_stats.resist[2], chr_stats.resist[3]);
    printf("chrstat: HP %d stamina %d load %.1f/%.0f def %.0f/%.0f/%.0f/%.0f poise %.0f AR R %.0f L %.0f%s\n",
           chr_stats.hp_max, chr_stats.stamina_max, chr_stats.equip_weight, chr_stats.equip_max, chr_stats.def[0],
           chr_stats.def[1], chr_stats.def[2], chr_stats.def[3], chr_stats.poise, chr_stats.ar[0][0],
           chr_stats.ar[1][0], chr_stats.valid ? "" : " (itemx.bin missing: class start values)");
}
