/* In-ELF test runner (tools/test-*.sh, tools/pine.py). A plain-text script loaded at boot drives the pad, camera,
 * teleports, waits and screenshot markers, so a test needs no rebuild. The script path comes from the ELF's argv
 * (PCSX2 -gameargs 'test=tests/x.txt', ps2client execee host:proto.elf test=...), relative to host: (the ELF's folder).
 * Without one the built-in demo script plays after 5 s without pad input, as before.
 *
 * Script: one command per line, '#' comments, times in game seconds (the frame dt main.c uses):
 *   option noenemies|hud|holes|nodemo|skipmiss   boot options (top of the file): no enemy roster / debug HUD /
 *                           magenta colour clear so uncovered pixels show / no idle demo / a spawn without collision
 *                           floor skips to the next spawn
 *   spawn x y z [yaw]       teleport the player onto the collision floor under (x, y, z) (y + 3 .. y - 10 m)
 *   cam yaw pitch           camera yaw / pitch in rad (absolute), held until "cam off"; "cam off" = pad/script stick
 *   pitch p                 camera pitch only (yaw as the last cam / approach set it)
 *   stick mx my             left stick held (-1..1, my > 0 forward); rstick cx cy: right stick
 *   circle 0|1              Circle held / released; r1: one R1 press
 *   wait s                  wait s game seconds (frames n: wait n frames)
 *   approach obj|x,z d speed  turn the camera at the nearest breakable (obj) or point and walk at `speed` until
 *                           within d m (gives up after 10 s)
 *   freeze 0|1              1: player / enemies / objects not updated (render-only; autotest uses it)
 *   stats                   reset the per-segment stats (each shot also resets them)
 *   shot name               marker: world frozen, mailbox shot_seq++ with the name; waits for the host's shot_ack
 *                           (PINE) or 3 s; prints "TEST shot <name> ..." with the segment's stats
 *   log text                print "TEST log text"
 *   flag id 0|1 / flags first last 0|1   set event flags (flags.h; ids as integers, e.g. 11010000)
 *   souls n / item id n     set souls / add n of an item (id with category bits, progress.h ITEM_*, e.g. 0x400000C8)
 *   check flag id v | flags first last count | souls n | item id n | loaded 0|1
 *                           "TEST check ... ok", or "TEST error check ..." (fails the run)
 *   save / load / wipe      write the save now / reload it / delete both slots (save.h); saves are off in a test
 *                           run unless "option save mc" or "option save host [name]" (host:<name>0.bin / 1.bin)
 *   loop / end              restart the script / stop (prints "TEST DONE", input idle afterwards)
 * Results: EE log lines "TEST ..." (emulog.txt) and the mailbox test_mb (pine.py reads it by symbol, or by the address
 * printed as "TEST mailbox 0x..." when the ELF is stripped). */
#ifndef TESTSCRIPT_H
#define TESTSCRIPT_H

#include "player.h"

#define TEST_NOENEMIES 1
#define TEST_HUD 2
#define TEST_HOLES 4
#define TEST_NODEMO 8

/* Shared with the host (tools/pine.py MAILBOX layout): keep field order and sizes, bump version on change */
typedef struct {
    unsigned magic;        /* 'TSTM' */
    unsigned version;      /* 1 */
    unsigned frame;        /* frames since boot (heartbeat) */
    unsigned state;        /* 0 no script, 1 running, 2 done, 3 waiting for a shot ack */
    unsigned shot_seq;     /* shot markers so far */
    unsigned shot_ack;     /* host: shot_seq | 0x80000000 while capturing (wait up to 60 s), shot_seq when done */
    char shot_name[32];
    int line;              /* script line running (1-based) */
    float pos[3], yaw;     /* player */
    float fps, frame_ms, map_ms, chr_ms, vuw_ms;  /* smoothed (HUD values) */
    int ram_free, vram_free;
    int chain_map, chain_map_peak, chain_chr, chain_chr_peak, segments;
    int tris, vu1_batches, ee_tris, enemies_active, enemies_drawn;
    int hp, deaths;
    /* current segment (since the last shot / stats): frames, frame time avg / max (ms), map ms max, lowest RAM free */
    unsigned seg_frames;
    float seg_ms_avg, seg_ms_max, seg_map_max;
    int seg_ram_min, seg_chain_map_max, seg_chain_chr_max;
} TestMailbox;
extern volatile TestMailbox test_mb;

/* Loads the script named by argv ("test=<path>"); returns TEST_* option bits */
int test_init(int argc, char **argv);
int test_active(void);  /* a script is loaded and not finished */
int test_frozen(void);  /* world updates paused (shot marker / freeze 1) */
/* Fills *in for this frame from the script (or the built-in demo when demo != 0 and no script); may set the camera
 * and teleport the player. Returns 1 if it produced the input. */
int test_input(Input *in, Player *pl, float *cam_yaw, float *cam_pitch, float dt, int demo);
/* Per-frame numbers for the mailbox / segment stats (call once per frame after the frame is drawn) */
void test_frame_end(const Player *pl, float avg_dt, float dt_real, float map_ms, float chr_ms, float vuw_ms, int tris,
                    int vram_free);

#endif
