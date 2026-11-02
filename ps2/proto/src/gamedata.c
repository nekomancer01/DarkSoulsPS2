/* Runtime game values (gamedata.h; blob format at the top of tools/gamedata.py) */
#include <stdio.h>
#include <stdlib.h>
#include "gamedata.h"
#include "pack.h"
#include "player_data.h"
#include "combat_data.h"
#include "progress_data.h"

const PlayerData *player_data;
const CombatData *combat_data;
const ProgressData *progress_data;

#define GD_MAGIC 0x54414447u  /* "GDAT" */
#define GD_HDR 0x20

/* Load one blob, check its layout id against the header the ELF was built with, turn its offsets into pointers */
static const void *load(Pack *pk, const char *name, unsigned layout, unsigned record_size)
{
    unsigned size;
    unsigned char *b = pack_load(pk, name, &size);
    if (!b) {
        printf("DSRPROTO gamedata: %s missing from c0000.pak\n", name);
        return 0;
    }
    const unsigned *h = (const unsigned *)b;
    if (size < GD_HDR + record_size || h[0] != GD_MAGIC || h[1] != layout || h[2] != size ||
        h[4] + h[3] * 4 > size) {
        printf("DSRPROTO gamedata: %s has layout %08x, this ELF expects %08x (rebuild the packs or the ELF)\n", name,
               size >= 8 ? h[1] : 0, layout);
        free(b);
        return 0;
    }
    const unsigned *rel = (const unsigned *)(b + h[4]);
    for (unsigned i = 0; i < h[3]; i++)
        *(unsigned *)(b + rel[i]) += (unsigned)b;
    return b + GD_HDR;
}

int gamedata_init(void)
{
    Pack pk;
    if (!pack_open(&pk, PACK_ROOT "c0000.pak")) {
        printf("DSRPROTO gamedata: no c0000.pak\n");
        return 0;
    }
    player_data = load(&pk, "player.gd", PLAYER_LAYOUT, sizeof(PlayerData));
    combat_data = load(&pk, "combat.gd", COMBAT_LAYOUT, sizeof(CombatData));
    progress_data = load(&pk, "progress.gd", PROGRESS_LAYOUT, sizeof(ProgressData));
    pack_close(&pk);
    if (combat_data && (combat_data->sk_bones > SK_BONES_MAX || combat_data->sk_r_weapon >= combat_data->sk_bones)) {
        printf("DSRPROTO gamedata: %d bones, SK_BONES_MAX %d\n", combat_data->sk_bones, SK_BONES_MAX);
        combat_data = 0;
    }
    return player_data && combat_data && progress_data;
}
