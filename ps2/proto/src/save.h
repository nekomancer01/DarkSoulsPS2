/* Save / load of the game state: progression (progress.h) + event flags (flags.h) in one versioned file, written to
 * the memory card (mc0:/DSRPS2/SAVE0.BIN / SAVE1.BIN, rom0:MCMAN through fio) or to host:save/<name>0.bin / 1.bin
 * (PCSX2 HostFs: build/save/, the one folder tools/pcsx2-run.sh mounts writable; tests).
 *
 * File: 32-byte header (u32 'DSRS', u16 SAVE_VERSION, u16 header size, u32 total size, u32 CRC-32 of the bytes after
 * the header, u32 sequence, u32 Progress bytes, u32 flag bytes, u32 0), then Progress as in progress.h, then the flag
 * pages (flags_save). Two slots are written in turn; boot loads the valid one with the higher sequence, so a write cut
 * short (power off, card pulled) leaves the previous save.
 *
 * When: boot loads; save_request() (rest at a bonfire, EMEVD Save Request, death; area change not yet: save-6) writes
 * on the next save_tick; save_tick also writes every SAVE_INTERVAL s when flags or progression changed. Card writes
 * from save_tick run in the background (libmc, polled once a frame; TECH_DEBT save-3); save_write and host: writes are
 * synchronous.
 * Device: argv save=mc|host|off (default: the card if one is formatted in slot 1, else host:); a test script runs with
 * saves off (new game every boot) unless it says "option save mc|host [name]". */
#ifndef SAVE_H
#define SAVE_H

#include "player.h"

#define SAVE_VERSION 1
#define SAVE_INTERVAL 60.0f  /* ASSUMED (TECH_DEBT save-6): DSR autosaves on most state changes */

enum { SAVE_OFF, SAVE_HOST, SAVE_MC };

void save_select(const char *dev);  /* "mc" / "host [name]" / "off" (testscript option / argv), before save_boot */
/* Picks the device, loads the newest valid save into prog + flags or starts a new game. Returns 1 if a save was
 * loaded. Call once after pad_setup / test_init; area = the booted area ("m10_02_00_00") for the saved position. */
int save_boot(int argc, char **argv, const char *area);
/* After player_init + map spawn: put the saved position (same area only), HP and deaths onto the player */
void save_apply_player(Player *pl);
void save_request(void);
void save_tick(const Player *pl, float dt);  /* once per frame */
int save_busy(void);  /* a background card write is in progress (for a saving indicator) */
int save_write(const Player *pl);  /* now; 0 ok */
int save_load(void);  /* reload prog + flags from the device; 1 ok */
int save_device(void);
int save_resident_bytes(void);
/* Test-script commands (testscript.c "flag / flags / souls / item / check / save / load / wipe"); 0 ok, -1 error
 * (prints "TEST error ...") */
int save_test_cmd(const char *line, const Player *pl);

#endif
