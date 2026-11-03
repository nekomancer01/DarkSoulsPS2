/* DSR's loading screen (FrpgMenuDlgNowLoading, menu.drb dialog E00_00; notes/PORTING.md "Loading screen"): black,
 * a loading hint (an item's icon, name and long description from the exe's 62-item table, the next one every 10 s)
 * and the bonfire animation in the bottom-left corner. Drawn while a load blocks the game: boot, warps (world.c
 * settle), death respawns. The loads keep the frame waiting, so the screen draws itself from inside them (pack.c's
 * wait hook, world.c's settle loop) at up to 30 fps. Its data is menu.pak loading.bin (tools/export_loading.py),
 * read at each open and given back at the close; VRAM is the top of the map texture pool (texcache.c tc_pin), at
 * boot before that pool exists the unallocated top of VRAM. */
#ifndef LOADING_H
#define LOADING_H

#include <gsKit.h>

/* gs, the frame present (main.c flip) and the display buffer (the last shown frame, faded out under the screen) */
void loading_init(GSGLOBAL *gs, void (*present)(GSGLOBAL *), unsigned disp_vram);
/* Open the screen (nested: only the outermost begin / end pair opens and closes it). end waits for the screen's
 * fade-in (DSR's 0.5 s) and plays its fade-out before it returns. */
void loading_begin(void);
void loading_end(void);
/* A begin whose end is the next loading_frame() (main loop, after world_update): boot, a death respawn, so a warp
 * the same frame (world.c settle) runs under the same screen */
void loading_hold(void);
void loading_frame(void);
/* Draw a frame when one is due (>= 1/30 s since the last). can_read: no fio request is in flight (the hint icon
 * reads through pack.c's async slot); 0 from inside a pack_load */
void loading_tick(int can_read);
int loading_active(void);
/* Tests (testscript "loadscreen"): the screen held open for s seconds */
void loading_show(float s);
/* After a load, in the HUD phase: DSR's fade-in of the game from black (0.1 s, then 0.5 s) */
void loading_overlay(float dt);

typedef struct { unsigned frames, shown_ms, vram_kb, ram_kb, opens; int hint; } LoadingStats;
extern LoadingStats loading_stats;

#endif
