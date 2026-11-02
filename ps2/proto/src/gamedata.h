/* Game values loaded at boot instead of compiled in (tools/gamedata.py): player.gd, combat.gd and progress.gd from
 * c0000.pak fill player_data / combat_data / progress_data (src/player_data.h, combat_data.h, progress_data.h), so
 * the ELF holds no game data and one build serves every player's own packs. */
#ifndef GAMEDATA_H
#define GAMEDATA_H

/* Load the three records; 0 (and a DSRPROTO line naming the file) when one is missing or was written for another
 * layout (packs from an older or newer build than the ELF). Call before save_boot (progress_new_game reads them). */
int gamedata_init(void);

#endif
