/* DSR event flags (EMEVD, ItemLotParam.getItemFlagId, ShopLineupParam.eventFlag, ObjActParam, NPC / boss state).
 *
 * Id layout = the game's own (8 decimal digits G AAA S NNN):
 *   G   group digit: 0, 1, 5, 6, 7 (group bases 0x0, 0x500, 0x5F00, 0xB900, 0x11300 in DSR's flag array)
 *   AAA area code: 000, 100, 101, 102, 110, 120, 121, 130, 131, 132, 140, 141, 150, 151, 160, 170, 180, 181
 *       (block index 0..17, 0x500 bytes per block; group 0 has only area 000)
 *   S   section 0..9 (128 bytes = 1024 bits each in a block), NNN flag 0..999: word NNN / 32, mask 0x80000000 >> NNN % 32
 * Checked against the game data (tools/flag_scan.py, 2026-11-01): 4002 of 4077 distinct flag ids used by all 32
 * EMEVDs (event ids, every "Event Flag" arg, InitializeEvent args) and ItemLot / ObjAct / ShopLineup params fit it; the
 * 75 others are m99 test maps (areas 990 / 999) and three strays. Ids outside the layout read 0 and ignore sets.
 * The byte offsets are the layout DSR-Gadget (community memory tool) uses; the exe's lookup function is not in the
 * decompiled tiers of fndb (not located). The digit split itself is what the game data above follows.
 *
 * Storage (TECH_DEBT save-1): DSR keeps the whole 93 KB array; here one 128-byte page per (group, area, section) is
 * taken from a fixed pool the first time a flag in it is set (game data touches 108 of the 730 sections).
 * Reads of a section without a page are 0. */
#ifndef FLAGS_H
#define FLAGS_H

#include <tamtypes.h>

#define FLAG_PAGES 256         /* pool: 256 x 128 B = 32 KB */
#define FLAG_SECTIONS 730      /* 1 block (group 0) + 4 groups x 18 areas, 10 sections each */

void flags_init(void);         /* all flags off, pool empty */
int flag_get(u32 id);          /* 1 = on */
void flag_set(u32 id, int on);
void flag_range_set(u32 first, u32 last, int on);  /* inclusive; ids outside the layout are skipped */
int flag_range_count(u32 first, u32 last);           /* flags on in [first, last] (EMEVD "all / any in range") */

/* EventValue: a number stored in nbits consecutive flags from `first` (EMEVD IncrementEventValue, Lua
 * Get/SetEventFlagValue). first = most significant bit (ASSUMED, TECH_DEBT save-2). */
u32 flag_value_get(u32 first, int nbits);
void flag_value_set(u32 first, int nbits, u32 value);

/* Change notification for the EMEVD runtime: flags_changes() counts every real change (set of an off flag or clear of
 * an on one); poll it, or register one callback that runs on each change (id, new state). */
unsigned flags_changes(void);
typedef void (*FlagChangeFn)(u32 id, int on);
void flags_set_callback(FlagChangeFn fn);

/* Save support (save.c): pages in use, serialized as u16 count, then per page u16 section slot, u16 0, 32 x u32 */
int flags_pages_used(void);
int flags_save(void *dst, int cap);          /* bytes written, -1 if cap is too small */
int flags_load(const void *src, int len);    /* 0 ok, -1 malformed (store left empty) */
int flags_resident_bytes(void);

#endif
