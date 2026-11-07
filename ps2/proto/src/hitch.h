/* Hitch monitor (2026-11-07): what a long play session sees and the averages of the scripted tests hid. Every frame
 * main.c times its sections (hitch_begin / hitch_end around each); at the end of the frame hitch_frame() keeps the
 * worst frame of the interval with its section split, counts frames over 50 / 100 / 200 ms, and every HITCH_PERIOD_S
 * of game time prints one "hitch:" line to the EE log with them, free heap, the largest block malloc still finds,
 * the streaming arena's free pages and its largest free run, mapstream churn (items read again after being dropped)
 * and map.c's level-of-detail flips (a cell in view changing between near / coarse / horizon / hole, and flip-flops:
 * back to the level it left within HITCH_FLOP_FRAMES). tools/hitchsum.py summarises the lines over a run (degradation
 * over time: the first against the last 10 minutes); tools/test-run.py gates on them (--hitch-max ...).
 * The line costs one printf per period. Also on by default, for play-test reports (play-report.sh at the repo root):
 * a "spike:" line for a single frame over HITCH_SPIKE_MS outside a loading screen (its section split and where the
 * player stood; at most HITCH_SPIKES_MAX a period, the rest counted in the hitch line), and an "alloc:" line for a
 * malloc / calloc / realloc / memalign that returned NULL (the linker wraps them: the first failure from each caller
 * with its return address for addr2line on proto.sym.elf, then only counted). */
#ifndef HITCH_H
#define HITCH_H

/* a flip back to the level the cell left within this many map draws (~2 s at 30 fps) is a flip-flop */
#define HITCH_FLOP_FRAMES 60

enum {
    HS_PRE,    /* player, combat, enemy type loads (enemy_stream) */
    HS_WORLD,  /* world_update: block reads, settles (mapstream_update inside) */
    HS_SND,    /* snd_map + snd_update (one RPC) */
    HS_MAP,    /* map_draw + vu1_kick */
    HS_LOGIC,  /* enemies, objects, events, bonfire */
    HS_CHR,    /* characters, effects, HUD */
    HS_WAIT,   /* vu1_wait */
    HS_FLIP,   /* gsKit queue + flip (vsync) */
    HS_SAVE,   /* save_tick, logs */
    HS_MSPLAN, /* inside HS_WORLD: mapstream plan() */
    HS_MSWIN,  /* inside: make_window (eviction scans) */
    HS_MSIO,   /* inside: issue / collect */
    HS_MSVIS,  /* inside: the visibility job (vis_step) */
    HS_LOG,    /* printf (pack.c __wrap__write), anywhere */
    HS_N
};

extern unsigned hitch_acc[HS_N];  /* EE ticks this frame */
static inline unsigned hitch_now(void)
{
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}
#define HITCH_BEGIN(s) unsigned hitch_t0_##s = hitch_now()
#define HITCH_END(s, sec) (hitch_acc[sec] += hitch_now() - hitch_t0_##s)

/* loading.c: a loading screen opened in this frame (DSR's own loads: warps, deaths). Such a frame counts apart ("loads
 * N longest ms"), not as a hitch */
void hitch_loadscreen(void);
/* end of a frame: dt_real = the whole frame (s) */
void hitch_frame(float dt_real);
/* mapstream.c: an item read (again: it was loaded before in this run) */
void hitch_load(int again, int thrash, unsigned bytes);
/* mapstream.c: an item dropped; wanted = it still had a class that wanted it (evicted for something else) */
void hitch_drop(int wanted, unsigned bytes);
/* map.c: cell (instance k, index ci) changed its drawn level while in view; flop = back to the level it had before */
void hitch_flip(int k, unsigned ci, int flop);
/* world.c: a block's lite set / relief read started (bytes) */
void hitch_block_read(unsigned bytes);
/* main.c, every frame: the player's position and world block (world_current) for the hitch / spike lines */
void hitch_where(const float *pos, int block);

/* the arena's free granules (256 B) and its largest run of free 64 KB pages (mapstream.c) */
void mapstream_free_info(unsigned *free_kb, unsigned *run_kb, unsigned *other_kb);

/* totals over the run (test gates) */
typedef struct {
    unsigned frames, over50, over100, over200, max_ms, flips, flops, rereads, thrash, evicts, loads, load_max_ms, allocfail;
} HitchTotals;
extern HitchTotals hitch_total;

#endif
