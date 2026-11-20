/* The player's derived character values from the saved stats (prog.stats, prog.humanity) and equipment (prog.equip):
 * max HP / stamina, equip load, defence per damage type, poise and the attack rating of each hand's weapon. Formulas
 * from DarkSoulsRemastered.exe (chrstat.c). Item numbers come from itemx.bin, which is only loaded while a menu is
 * open, so the values are computed into a resident snapshot: at boot (chrstat_boot loads itemx.bin for a moment) and
 * whenever a menu has it loaded (menu.c calls chrstat_recalc every frame then: a level-up or an equipment change shows
 * at once). Combat (combat.c), the player (player.c) and the Status screen (menu.c) read the snapshot. */
#ifndef CHRSTAT_H
#define CHRSTAT_H

enum { EL_PHYS, EL_MAG, EL_FIRE, EL_THUN, EL_COUNT };    /* damage elements, AtkParam / EquipParam order */
enum { ATK_NONE = 0, ATK_SLASH = 1, ATK_BLOW = 2, ATK_THRUST = 3 };  /* AtkParam.atkAttribute (typed physical) */
enum { HAND_R, HAND_L };

typedef struct { unsigned id; float ar[EL_COUNT], ar2h[EL_COUNT], poise_dmg, guard_cut[EL_COUNT]; int types, guard_stab, guard_level; }
    ChrWep;
typedef struct { unsigned id; int anim, heal, hum, souls, consume; } ChrQuick;

typedef struct {
    int valid;                  /* 0 until the first recalc: users fall back to the class start values */
    int hp_max, stamina_max;
    float equip_weight, equip_max;
    float def[EL_COUNT];        /* total defence: base from stats (graph 102) + armour */
    float def_typed[4];         /* physical defence against ATK_* (index 0 = untyped = def[EL_PHYS]) */
    float poise;                /* armour saDurability sum */
    float ar[2][EL_COUNT];      /* attack rating of the active weapon per hand (chr_hand_slot: EQ_WEP_*1 or *2) */
    float poise_dmg[2];         /* EquipParamWeapon.saWeaponDamage per hand */
    int wep_types[2];           /* WT_* bits of each hand's weapon */
    float guard_cut[2][EL_COUNT]; /* EquipParamWeapon phys / mag / fire / thunGuardCutRate (%) */
    int guard_stab[2];          /* staminaGuardDef (stability) */
    int guard_level[2];
    float resist[4];
    /* the selected quick item (chr_quick; Estus = goods 201's row): id, goodsUseAnim (ESD f231), HP healed,
     * humanity (st 11) and souls (st 9 / 10) given, consumed on use */
    unsigned item_id;
    int item_anim, item_heal, item_hum, item_souls, item_consume;
    /* both slots of each hand and all five quick slots, so the d-pad can switch without itemx.bin (recalc) */
    ChrWep wep[2][2];
    ChrQuick quick[5];            /* poison, toxic, bleed, curse: stat graph (FUN_1402dddd0) + armour */         /* guardLevel (0-4): the guard row of the reaction table (ESD f237) */
} ChrStats;

extern ChrStats chr_stats;
/* d-pad selection (DSR: left / right switch the hand's weapon slot, down cycles the quick items): the active
 * weapon slot per hand (0 = slot 1, 1 = slot 2) and the selected quick slot (0-4); runtime only (TECH_DEBT items-1) */
extern int chr_hand_slot[2], chr_quick;
extern int chr_two_hand;         /* the right weapon held in both hands (ESD hold style 3, c1_104) */
void chrstat_select(void);       /* copy the selected slots of the snapshot into the active fields */
int chrstat_switch_hand(int h);  /* toggle hand h's weapon slot; 1 if it changed */
int chrstat_cycle_quick(void);   /* next quick slot holding an item; 1 if it changed */

/* From prog + the loaded itemx rows; returns 0 (snapshot unchanged) when itemx.bin is not loaded */
int chrstat_recalc(void);
/* Boot, after the save loaded: loads itemx.bin from menu.pak into a temporary malloc buffer, recalcs, frees it */
void chrstat_boot(void);
/* CalcCorrectGraph row by id (combat_data graphs + the fixed rows), NULL unknown */
const float *chrstat_graph(int id);
/* Attack rating of a weapon item (id with its reinforcement level) for the current stats, per element; 0 if unknown
 * or itemx.bin is not loaded */
int chrstat_weapon_ar(unsigned id, float ar[EL_COUNT]);

#endif
