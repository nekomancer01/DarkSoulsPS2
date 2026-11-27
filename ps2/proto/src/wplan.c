/* What the world wants (world_i.h): the blocks the triangle under the player loads and keeps (world.bin links, the
 * reach table), the structural pool's size, slots, and the order of the one background read each frame. Split from
 * world.c (Polaris 4.1, code moved as it was). */
#include "world_i.h"
#include "texcache.h"
_Static_assert(WORLD_SLOTS <= TC_BLOCKS, "a block on without a texture slot is not drawn (mapblock.c)");
unsigned w_reach_m;  /* blocks with a distance from the triangle underfoot */
float w_clock_s;  /* seconds of world_update (ticks() wraps every 14.6 s) */
float w_want_s[WORLD_MAX], w_on_s[WORLD_MAX];  /* clock_s when a block was first wanted while off / turned on */
float w_feet_now[3];  /* for start(): mapstream_alloc evicts the cells farthest from here */
const unsigned *w_lrange;  /* per block: first link, links */
const WorldLink *w_links;
/* wanted by the reach alone (not by the link's load mask): they start after the link-wanted ones, never take a slot or
 * pool room from them (room_for) and never spill into the item pages (lite_alloc); a link-wanted block may turn them
 * off, so the structural pool's size (world_struct_bytes, from the link masks) still holds */
unsigned w_ronly_m;
unsigned w_want_m, w_keep_m;  /* blocks the triangle under the player loads / keeps */
static float unkept_s[WORLD_MAX];  /* seconds a loaded block has not been in keep_m */
/* WORLD_BOOT_LEAN: the last spots the boot block was underfoot or kept from (one per seam: spots WORLD_BOOT_SPOT_D
 * apart; the nearest one follows the player while the block is underfoot or kept); the boot block is wanted back */
static float boot_feet[WORLD_BOOT_SPOTS][3];
int w_room_stuck;  /* this step a wanted block found no room and no kept block could make it some (room_for) */

/* WORLD_LITE_DIET: what a lean block of block i lets go (LEAN_ENT: map tables, visibility) */
static unsigned tab_bytes(const BlockRt *r)
{
    unsigned n = 0;
    for (int e = E_TAB; e <= E_PVS; e++)
        if (LEAN_ENT(e))
            n += (r->size[e] + 127) & ~127u;
    return n;
}

/* the structural pool's size: over every link record of every block (the triangle under the player), the lite sets
 * on at once: the blocks it loads (and the block itself) whole, those it only keeps lean (WORLD_LITE_DIET) or whole,
 * the WORLD_POOL_SLOTS largest (a fourth at a spot that keeps four spills into the item pages: WORLD_SLOTS); the boot
 * block's tables are in the heap (world_boot_map) */
/* WORLD_AI_UNLOAD: the Lua heap the blocks a link keeps can hold at once, by their ai.lua0 sizes (a block's chunks
 * took 77-97 KB of VM heap and malloc for an 89 KB file at boot, 2026-11-25; shared chunks count in every block: an upper
 * bound). Reserved beside the arena (streamer thread, coord.md: max over links of the summed Lua of its keep | load). */
unsigned world_lua_bytes(void)
{
    unsigned best = 0;
    for (int b = 0; b < w_nblk; b++)
        for (unsigned l = 0; l < w_lrange[2 * b + 1]; l++) {
            const WorldLink *k = w_links + w_lrange[2 * b] + l;
            unsigned keep = k->keep | k->load | (b ? 1u << b : 0), sum = 0;
            for (int i = 1; i < w_nblk; i++)
                if (w_rt[i].open && (keep >> i & 1))
                    sum += (w_rt[i].size[E_AI] + 127) & ~127u;
            best = sum > best ? sum : best;
        }
    return best;
}

unsigned world_struct_bytes(void)
{
    unsigned best = 0;
    for (int b = 0; b < w_nblk; b++)
        for (unsigned l = 0; l < w_lrange[2 * b + 1]; l++) {
            const WorldLink *k = w_links + w_lrange[2 * b] + l;
            unsigned load = k->load | (b ? 1u << b : 0), keep = k->keep | load, v[WORLD_MAX], n = 0, sum = 0;
            unsigned lean_v[WORLD_MAX], whole = 0;
            for (int i = 1; i < w_nblk; i++)
                if (w_rt[i].open && (keep >> i & 1)) {
                    lean_v[n] = WORLD_LITE_DIET ? w_rt[i].total - tab_bytes(&w_rt[i]) : w_rt[i].total;
                    v[n] = (load >> i & 1) || !WORLD_LITE_DIET ? w_rt[i].total : lean_v[n];
                    n++;
                }
            for (unsigned a = 0; a < n; a++)  /* wanted beyond WORLD_STRUCT_WHOLE (smallest first) count lean */
                whole += v[a] != lean_v[a];
            while (whole > WORLD_STRUCT_WHOLE) {
                unsigned m = ~0u;
                for (unsigned a = 0; a < n; a++)
                    if (v[a] != lean_v[a] && (m == ~0u || v[a] < v[m]))
                        m = a;
                v[m] = lean_v[m], whole--;
            }
            for (int i = 1; i < w_nblk; i++)  /* one maptex.bin per map of the blocks on */
                if (w_rt[i].open && w_rt[i].mtx_full && (keep >> i & 1) && map_lead(i) == i) {
                    unsigned most = 0;
                    for (int j = i; j < w_nblk; j++)
                        if (w_rt[j].open && (keep >> j & 1) && map_lead(j) == i)
                            most = w_rt[j].mtx_full > most ? w_rt[j].mtx_full : most;
                    sum += (most + 127) & ~127u;
                }
            for (int i = 1; i < w_nblk; i++)  /* WORLD_EV_SHARED: one events.bin per map of the blocks on */
                if (w_rt[i].open && w_rt[i].ev_split && (keep >> i & 1) && map_lead(i) == i) {
                    unsigned most = 0;
                    for (int j = i; j < w_nblk; j++)
                        if (w_rt[j].open && w_rt[j].ev_split && (keep >> j & 1) && map_lead(j) == i)
                            most = w_rt[j].ev_full > most ? w_rt[j].ev_full : most;
                    sum += (most + 127) & ~127u;
                }
            for (unsigned s = 0; s < WORLD_POOL_SLOTS && s < n; s++) {  /* the WORLD_POOL_SLOTS largest */
                unsigned m = s;
                for (unsigned j = s + 1; j < n; j++)
                    m = v[j] > v[m] ? j : m;
                unsigned t = v[s];
                v[s] = v[m], v[m] = t;
                sum += v[s];
            }
            best = sum > best ? sum : best;
        }
    return best;
}

/* a wanted block's lite set is being read or could start now (start()'s conditions, room aside) */
static int lite_pending(void)
{
    int free_s = 0;
    for (int s = 0; s < WORLD_SLOTS; s++)
        free_s |= w_slot_of[s] < 0;
    for (int i = 1; i < w_nblk; i++)
        if (w_rt[i].state == W_READING || (w_rt[i].state == W_ON && w_rt[i].stage == 1) ||
            (free_s && w_rt[i].open && w_rt[i].state == W_OFF && (w_want_m >> i & 1) && (w_rt[i].retry_s <= 0 || w_warping)) ||
            (w_rt[i].state == W_ON && w_rt[i].lean == 2) ||
            (w_rt[i].state == W_ON && w_rt[i].lean == 1 && (w_want_m >> i & 1 || i == w_cur) && (w_rt[i].retry_s <= 0 || w_warping)))
            return 1;
    return 0;
}

static unsigned src_only_m;  /* blocks wanted by the other sources alone (not by the player): this step's from where
                               * w_step computes it (its start reads the last step's) */
int w_src_only(int i) { return i > 0 && (src_only_m >> i & 1); }
/* blocks the player needs a floor from now: held for (world_hold), or holding the feet while there is no floor (a
 * warp's spot). Such a block always gets a slot: room_pick turns off the farthest other one, margin or not, and it
 * is never deferred (Anor Londo, 2026-11-23: a warp onto block 22 with every slot kept for nearer wanted blocks left
 * it deferred and the player fell through, 3 of 3 runs) */
static unsigned under_m;
int w_under(int i) { return i >= 0 && (under_m >> i & 1); }

/* WORLD_YIELD (world_i.h): a temporary that found no window asks; the next step turns the spilled blocks off (not
 * here: the caller may be inside enemy.c's job, and turning a block off removes its enemies) */
float w_yield_s;
static int yield_req;
static unsigned yield_m;  /* the blocks it turned off: no block goes to make room for them meanwhile */
static int spilled(int i)
{
    for (int e = 0; e < NENT; e++)
        if (w_rt[i].blob[e] && mapstream_in_items(w_rt[i].blob[e]))
            return 1;
    return 0;
}
int world_yield_spill(unsigned bytes)
{
    if (!WORLD_YIELD || !mapstream_pools_on())
        return 0;
    int n = 0;
    for (int i = 1; i < w_nblk; i++)
        n += w_rt[i].state == W_ON && i != w_cur && !(under_m >> i & 1) && spilled(i);
    if (!yield_req && w_yield_s <= 0)
        printf("world: %u KB found no window: %d spilled block%s off for %.0f s\n", bytes >> 10, n, n == 1 ? "" : "s",
               (double)WORLD_YIELD_S);
    w_yield_s = WORLD_YIELD_S;
    yield_req = n > 0;
    return n;
}
static void yield_step(void)
{
    if (!yield_req)
        return;
    yield_req = 0;
    for (int i = 1; i < w_nblk; i++)
        if (w_rt[i].state == W_ON && i != w_cur && !(under_m >> i & 1) && world_stats.reading != i && spilled(i)) {
            w_turn_off(i);
            w_rt[i].retry_s = WORLD_YIELD_S;
            yield_m |= 1u << i;
            world_stats.yields++;
        }
}
/* the block room_for would turn off for block want, -1 none */
static int room_pick(int want, const float *feet)
{
    int b = -1;
    float bd = -1e30f;
    int ronly = w_ronly_m >> want & 1;
    if (yield_m >> want & 1)
        return -1;  /* (WORLD_YIELD turned it off: its retry wait is no failure for room; 16 and 23 went for it) */
    /* blocks not wanted; for a block the links want, also those wanted by the reach alone (WORLD_REACH). For a block
     * only another source wants (a respawn point), never a kept one (w_keep_m: what the player's floor and reach keep,
     * and the sources' own): the preload must not take the player's surroundings away */
    unsigned firm = ronly ? w_want_m : w_want_m & ~w_ronly_m;
    if (src_only_m >> want & 1)
        firm |= w_keep_m;
    for (int i = 1; i < w_nblk; i++)
        if (i != want && i != w_cur && w_rt[i].state == W_ON && !(firm >> i & 1) && world_stats.reading != i) {
            float d = box_d(&w_tab[i], feet) - ((w_want_m >> i & 1) ? 1e6f : 0);  /* (unwanted ones go first) */
            if (d > bd)
                b = i, bd = d;
        }
    int draining = 0;  /* (a block draining frees its slot within WORLD_DRAIN steps: no second one goes for it) */
    for (int s = 0; s < WORLD_SLOTS; s++)
        draining |= w_slot_of[s] >= 0 && w_rt[w_slot_of[s]].state == W_DRAIN;
    if (b < 0 && want > 0 && (under_m >> want & 1) && slots_full() && !draining) {  /* the player's floor: the farthest other goes */
        for (int i = 1; i < w_nblk; i++)
            if (i != want && i != w_cur && !(under_m >> i & 1) && w_rt[i].state == W_ON && world_stats.reading != i) {
                float d = box_d(&w_tab[i], feet);
                if (d > bd)
                    b = i, bd = d;
            }
    }
    if (b < 0 && WORLD_SLOT_NEAREST && want > 0 && !ronly && slots_full()) {
        /* every slot held by a wanted block: the farthest of them gives its slot to a nearer one. On the Burg bonfire
         * block's floor by the Parish steps, the triangles want Burg west, the Parish and the Parish rest at once
         * with three slots: the Parish (where the player was going) waited 9 s for a slot and the player was held
         * at its stairs 0.8 s (ISO world_route 2026-11-16). WORLD_SLOT_MARGIN m of hysteresis, so the two never
         * trade slots back and forth (ASSUMED) */
        float dw = box_d(&w_tab[want], feet);
        for (int i = 1; i < w_nblk; i++)
            if (i != want && i != w_cur && w_rt[i].state == W_ON && world_stats.reading != i) {
                float d = box_d(&w_tab[i], feet);
                if (d > dw + WORLD_SLOT_MARGIN && d > bd)
                    b = i, bd = d;
            }
    }
    return b;
}
static int room_for(int want, const float *feet)
{
    int b = room_pick(want, feet);
    if (b < 0)
        return 0;
    printf("world: block %d off to make room for block %d\n", b, want);
    w_turn_off(b);
    return 1;
}

/* WORLD_SLOT_DEFER (default 1; TECH_DEBT world-22): a wanted block that cannot have a slot now: every slot holds a
 * block room_for keeps for it (wanted, and not WORLD_SLOT_MARGIN farther than it). It waits for the player to move:
 * a settle does not wait for it and it is no "no room" (room_stuck). 1.6 % of the 14 blocks' collision triangles
 * (6668 of 407575, most on the Burg bonfire block by the Parish steps) want four blocks besides the boot block with
 * WORLD_SLOTS 3; at tests/burg.txt's spawn the warp's settle waited out WORLD_STUCK_MS for the fourth (2.3 s on
 * PCSX2, 2026-11-18). 0: the settle waits for it as before */
int w_slot_deferred(int i, const float *feet)
{
    if (!WORLD_SLOT_DEFER || i <= 0 || !slots_full() || (under_m >> i & 1))
        return 0;
    for (int s = 0; s < WORLD_SLOTS; s++)  /* a draining block frees its slot at the next step: not deferred */
        if (w_rt[w_slot_of[s]].state == W_DRAIN)
            return 0;
    return room_pick(i, feet) < 0;
}

/* WORLD_REACH: the reach entry of triangle tri of block b, or NULL (binary search as link_at) */
static const WorldReach *reach_at(int b, unsigned tri)
{
    if (!w_rrange || !WORLD_REACH)
        return NULL;
    const unsigned short *f = w_rfirst + w_rrange[2 * b];
    int lo = 0, hi = (int)w_rrange[2 * b + 1] - 1, at = -1;
    while (lo <= hi) {  /* the last run starting at or before tri */
        int m = (lo + hi) >> 1;
        if (f[m] <= tri)
            at = m, lo = m + 1;
        else
            hi = m - 1;
    }
    unsigned e = at < 0 ? 0 : w_rent[w_rrange[2 * b] + at];
    return e && e <= w_reach_n ? w_reach_tab + e - 1 : NULL;
}

/* the link record of triangle tri of block b, or NULL (binary search: links are sorted by first triangle) */
static const WorldLink *link_at(int b, unsigned tri)
{
    const WorldLink *l = w_links + w_lrange[2 * b];
    int lo = 0, hi = (int)w_lrange[2 * b + 1] - 1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        if (tri < l[m].first)
            hi = m - 1;
        else if (tri >= l[m].first + l[m].n)
            lo = m + 1;
        else
            return &l[m];
    }
    return NULL;
}

/* Streaming sources (world.h): their state; the player's is world_update's feet */
WorldSrc w_src[WSRC_N];
void world_source(int kind, const float pos[3])
{
    if (kind < 0 || kind >= WSRC_N)
        return;
    WorldSrc *s = &w_src[kind];
    for (int a = 0; a < 3; a++)
        s->vel[a] = s->on ? pos[a] - s->pos[a] : 0, s->pos[a] = pos[a];
    if (!s->on && kind != WSRC_PLAYER)
        printf("world: source %d on at %.1f %.1f %.1f\n", kind, (double)pos[0], (double)pos[1], (double)pos[2]);
    s->on = 1;
}
void world_source_off(int kind)
{
    if (kind < 0 || kind >= WSRC_N || !w_src[kind].on)
        return;
    w_src[kind].on = 0;
    if (kind != WSRC_PLAYER)
        printf("world: source %d off\n", kind);
}
/* the blocks a source other than the player wants: those its floor's links load / keep (the block it stands in too)
 * when that block is on, else every block whose bounds and height range hold it (as the player without a floor) */
static int src_boot;  /* a source stands in the boot block (its tables come back: boot_want) */
static void src_wants(const float *p, unsigned *wm, unsigned *km)
{
    ColHit h = {.tri = -1};
    *wm = *km = 0;
    if (col_floor(p[0], p[2], p[1] + 1.0f, p[1] - 3.0f, COL_SOLID, &h) && h.block < w_nblk && w_rt[h.block].state == W_ON) {
        src_boot |= h.block == 0;
        const WorldLink *l = link_at(h.block, h.tri);
        unsigned self = h.block ? 1u << h.block : 0;
        *wm = (l ? l->load : 0) | self, *km = (l ? l->keep : 0) | self;
        return;
    }
    for (int i = 1; i < w_nblk; i++)
        if (box_d(&w_tab[i], p) <= 0.0f && p[1] >= w_tab[i].y0 - WORLD_Y_MARGIN && p[1] <= w_tab[i].y1 + WORLD_Y_MARGIN)
            *wm |= 1u << i, *km |= 1u << i;
}

void w_step(const float *feet, float dt)
{
    coltile_step(feet);  /* collision tiles around the feet (also in a settle's loop) */
    /* the block under the player: the one whose collision floor is under the feet; its triangle picks the blocks
     * wanted (world.bin links) */
    ColHit h = {.tri = -1};
    int was = w_cur;
    unsigned wm = w_want_m, km = w_keep_m;
    memcpy(w_feet_now, feet, sizeof w_feet_now);
    w_room_stuck = 0;
    under_m = 0;
    if (col_floor(feet[0], feet[2], feet[1] + 1.0f, feet[1] - 3.0f, COL_SOLID, &h) && h.block < w_nblk &&
        w_rt[h.block].state == W_ON) {
        w_cur = h.block;
        const WorldLink *l = link_at(h.block, h.tri);
        wm = l ? l->load : 0, km = l ? l->keep : 0;
        const WorldReach *rc = reach_at(h.block, h.tri);
        unsigned rm = 0;
        for (int j = 0, k = 0; rc && j < w_nblk && k < 6; j++)
            if (rc->mask >> j & 1) {
                float d = rc->d[k++] * w_reach_step;
                if (!(w_reach_m >> j & 1))  /* the way the distance went last: towards (-1) or away from (+1) */
                    w_reach_v[j] = 0;
                else if (d != w_reach_d[j])
                    w_reach_v[j] = d > w_reach_d[j] ? 1.0f : -1.0f;
                w_reach_d[j] = d, rm |= 1u << j;
            }
        w_reach_m = rm;
        unsigned lm = wm;
        for (int j = 1; j < w_nblk; j++)  /* reachable before it would be on: wanted; within reach: kept */
            if ((rm >> j & 1) && reach_t(j) * 1000.0f < w_on_est[j] + WORLD_REACH_MARGIN_S * 1000.0f)
                wm |= 1u << j;
        w_ronly_m = wm & ~lm;
        km |= rm & ~1u;  /* (the boot block comes back by its own spots: boot_step) */
    } else {  /* no floor: the blocks around the feet as well (a warp settles them, as DSR's loading screen). Their
               * height range too: the Asylum lies over the Burg and Firelink in x / z, and loading all three at once
               * left no room in the near arena (settle looped, 2026-11-02) */
        if (w_warping)  /* a warp: only the blocks around the new spot, not what the old spot's floor wanted (a warp
                       * from Darkroot into the Depths kept two Darkroot blocks wanted and found no slot, 2026-11-15) */
            wm = km = 0, w_reach_m = 0;
        w_ronly_m = 0;
        wm &= ~src_only_m;  /* (last step's source blocks come back below as the sources' again, not the player's) */
        for (int i = 1; i < w_nblk; i++)
            if (box_d(&w_tab[i], feet) <= 0.0f && feet[1] >= w_tab[i].y0 - WORLD_Y_MARGIN &&
                feet[1] <= w_tab[i].y1 + WORLD_Y_MARGIN)
                wm |= 1u << i, km |= 1u << i, under_m |= 1u << i;
    }
    {  /* the other streaming sources (a respawn point while a death plays): their blocks too, and their cells
        * (mapstream_sources) */
        float sp[WSRC_N][3];
        int ns = 0;
        src_boot = 0;
        unsigned pw = wm;  /* the player's wants */
        src_only_m = 0;
        for (int s = WSRC_PLAYER + 1; WORLD_SOURCES && s < WSRC_N; s++)
            if (w_src[s].on) {
                unsigned sw, sk;
                src_wants(w_src[s].pos, &sw, &sk);
                /* wanted as by the reach alone (w_ronly_m): after the player's, never taking a slot or pool room
                 * from a block the player's floor wants, never spilling into the item pages */
                w_ronly_m |= sw & ~wm;
                src_only_m |= sw & ~pw;
                wm |= sw, km |= sk;
                memcpy(sp[ns++], w_src[s].pos, sizeof sp[0]);
            }
        mapstream_sources(ns ? sp[0] : NULL, ns);
    }
    wm |= w_hold_want, km |= w_hold_want;  /* the blocks the player is held for (world_hold) */
    under_m |= w_hold_want;
    yield_step();  /* (after under_m: never the player's floor) */
    w_ronly_m &= ~w_hold_want;
    src_only_m &= ~w_hold_want;  /* (the player's now) */
    if (wm != w_want_m || (km | wm) != w_keep_m)
        printf("world: block %d tri %d: want %x keep %x\n", w_cur, h.tri, wm, km | wm);
    w_want_m = wm, w_keep_m = km | wm;
    world_stats.want = w_want_m;
    if (WORLD_BOOT_LEAN && w_rt[0].size[E_TABB]) {
        float d2 = 1e30f;  /* to the nearest remembered spot (none: a boot straight into another block) */
        int ni = 0;
        for (int i = 0; i < w_boot_spots; i++) {
            float dx = feet[0] - boot_feet[i][0], dy = feet[1] - boot_feet[i][1], dz = feet[2] - boot_feet[i][2];
            if (dx * dx + dy * dy + dz * dz < d2)
                d2 = dx * dx + dy * dy + dz * dz, ni = i;
        }
        if ((h.tri >= 0 && h.block == 0) || (w_keep_m & 1)) {  /* on its floor, or on a part that keeps it */
            if (d2 > WORLD_BOOT_SPOT_D * WORLD_BOOT_SPOT_D) {  /* another seam: a new spot (the oldest goes) */
                if (w_boot_spots == WORLD_BOOT_SPOTS)
                    memmove(boot_feet, boot_feet + 1, (--w_boot_spots) * sizeof *boot_feet);
                ni = w_boot_spots++;
            }
            memcpy(boot_feet[ni], feet, sizeof *boot_feet);  /* (the seam's spot follows the player) */
            w_boot_want = 1;
        } else if (w_cur == 0)
            w_boot_want = 1;  /* (in the air over it, or before the first floor is known) */
        else {
            /* off only while a panorama is drawn (the block underfoot's, or for a moment the one of the block
             * before: it shows the boot block as well); back without one (a block whose far.bin found no heap or
             * that has none), or near the seam again */
            if ((!w_far_cur && !w_far_swap) || d2 < WORLD_BOOT_BACK_D * WORLD_BOOT_BACK_D)
                w_boot_want = 1;
            else if (d2 > WORLD_BOOT_OFF_D * WORLD_BOOT_OFF_D)
                w_boot_want = 0;
        }
        w_boot_want |= src_boot;  /* (a respawn point in the boot block: its tables come back for it) */
        if (!w_boot_want && !w_rt[0].lean && world_stats.reading != 0 && (w_warping || !mapstream_reading(0)))
            w_boot_lean();
    }
    w_far_retry_s = w_far_retry_s > dt ? w_far_retry_s - dt : 0;
    w_yield_s = w_yield_s > dt ? w_yield_s - dt : 0;
    yield_m = w_yield_s > 0 ? yield_m : 0;
    if (w_far_old && (--w_far_old_drain <= 0 || w_warping)) {
        w_far_free(w_far_old);
        w_far_old = NULL;
    }
    for (int i = 0; i < w_nblk; i++)
        unkept_s[i] = w_keep_m >> i & 1 ? 0 : unkept_s[i] + dt;
    for (int i = 0; i < w_nblk; i++)
        w_rt[i].retry_s = w_rt[i].retry_s > dt ? w_rt[i].retry_s - dt : 0;
    w_clock_s += dt;
    for (int i = 1; i < w_nblk; i++) {  /* the hold log: since when a block is wanted while not on */
        int w = (w_want_m >> i & 1) && (w_rt[i].state == W_OFF || w_rt[i].state == W_READING);
        w_rt[i].want_t = w ? (w_rt[i].want_t ? w_rt[i].want_t : ticks() | 1) : 0;
        w_want_s[i] = w ? (w_want_s[i] > 0 ? w_want_s[i] : w_clock_s + 1e-3f) : w_rt[i].state == W_ON ? w_want_s[i] : 0;
    }
    if (w_cur != was && was >= 0 && w_cur > 0 && !w_warping)  /* crossing a seam: how long the block had been on */
        printf("world: into block %d from %d: on %.0f ms before\n", w_cur, was, (double)((w_clock_s - w_on_s[w_cur]) * 1000));
    {  /* MS_LEND: a block one step from wanted (kept by the triangle, still off) gets its pool room cleared of lent
        * items now, so the lite set never waits for them to drain at the seam (the planning thread, 2026-11-09) */
        static float prep_s;
        prep_s -= dt;
        for (int i = 1; i < w_nblk && prep_s <= 0; i++)
            if (w_rt[i].open && w_rt[i].state == W_OFF && (w_keep_m >> i & 1) && mapstream_pools_on() &&
                (reach_t(i) >= 0 ? reach_t(i) * 1000.0f < w_on_est[i] + (WORLD_REACH_MARGIN_S + WORLD_PREP_S) * 1000.0f
                                 : box_d(&w_tab[i], feet) < WORLD_PREP_D) &&
                !mapstream_pool_prepare(MS_POOL_STRUCT, w_rt[i].total))
                prep_s = 0.25f;
    }
    for (int i = 1; i < w_nblk; i++) {  /* WORLD_LITE_DIET: kept-only blocks let go of their map tables */
        BlockRt *r = &w_rt[i];
        r->unwanted_s = r->state == W_ON && !(w_want_m >> i & 1) && i != w_cur ? r->unwanted_s + dt : 0;
        w_lean_step(i);
        if (WORLD_LITE_DIET && r->state == W_ON && r->stage == 2 && !r->lean && i != w_cur && !(w_want_m >> i & 1) &&
            world_stats.reading != i && (r->unwanted_s >= WORLD_LEAN_S || (w_warping && !(w_hold_want >> i & 1))))
            w_lean(i);
    }
    w_in_cur_s = w_cur == was ? w_in_cur_s + dt : 0;
    if (w_cur != was)
        w_relief_at = 0;
    world_stats.current = w_cur;
    for (int i = 0; i < w_nblk; i++)  /* slots of unloaded blocks come free once the chains are done */
        if (w_rt[i].state == W_DRAIN && (--w_rt[i].drain <= 0 || w_warping))
            w_free_slot(i);
    if (w_bump_old && (--w_bump_old_drain <= 0 || w_warping)) {
        w_bump_free(w_bump_old, w_bump_old_n);
        w_bump_old = NULL;
    }
    /* one background read at a time. Wanted blocks' lite sets go first: they hold the collision the player walks onto
     * next, while the resident cells (mapstream.c) and the relief only add drawing detail. With a full swap first, from
     * cdrom0: a Burg swap (4.9 MB, ~5 s at ~1 MB/s) kept the next block's collision from loading and the player walked
     * off the loaded floor (2026-11-04, TECH_DEBT world-8). A relief chunk in flight is collected first; while a lite
     * set waits neither the relief nor mapstream.c issue a new read. */
    w_lite_first = lite_pending();
    mapstream_hold(w_lite_first);
    mapstream_focus(w_cur);
    if (w_bump_reading) {
        w_bump_step();
        if (w_bump_reading)
            goto unload;
    }
    if (w_far_reading) {
        w_far_step();
        if (w_far_reading)
            goto unload;
    }
    w_boot_step(0);  /* (its buffers' drain; a chunk of its tables in flight) */
    if (world_stats.reading == 0)
        goto unload;
    if (world_stats.reading > 0 && !w_bump_reading && w_rt[world_stats.reading].state == W_ON &&
        w_rt[world_stats.reading].lean == 2) {
        w_relite_step(world_stats.reading);  /* a lean block's tables coming back */
        goto unload;
    }
    if (world_stats.reading > 0 && !w_bump_reading && w_rt[world_stats.reading].state == W_ON) {
        w_read_step(world_stats.reading, 0);  /* a texture chunk in flight is collected; the next one waits its turn */
        goto unload;
    }
    for (int i = 1; i < w_nblk; i++)  /* a lean block's tables: one read in flight at a time */
        if (w_rt[i].state == W_ON && w_rt[i].lean == 2 && world_stats.reading < 0) {
            w_relite_step(i);
            goto unload;
        }
    for (int i = 0; i < w_nblk; i++)
        if (w_rt[i].state == W_READING) {
            if (world_stats.reading < 0 || world_stats.reading == i)
                w_read_step(i, 1);
            goto unload;
        }
    {   /* start the nearest wanted block (WORLD_REACH: those the links want before those the reach alone wants) */
        int want = -1;
        float wd = 1e30f;
        for (int i = 1; i < w_nblk; i++)
            if (w_rt[i].open && w_rt[i].state == W_OFF && (w_want_m >> i & 1) &&
                (want < 0 || (w_ronly_m >> want & 1) || !(w_ronly_m >> i & 1))) {
                /* nearest first, the height range counted too (WORLD_WANT_3D, default on; 0 = x / z only): on
                 * Firelink's aqueduct floor, which wants Burg west and the Parish rest since a part loads every block
                 * it touches, the Parish rest (x / z 8 m away, 29 m above) read first and Burg west, where the walk
                 * goes, 3.4 s later (Ame's play report 2026-11-17, t 153: Burg pieces missing at the door) */
                float d = box_d(&w_tab[i], feet);
                if (WORLD_WANT_3D)
                    d += feet[1] < w_tab[i].y0 ? w_tab[i].y0 - feet[1] : feet[1] > w_tab[i].y1 ? feet[1] - w_tab[i].y1 : 0;
                if (reach_t(i) >= 0)  /* WORLD_REACH: by time to its seam, the ones walked away from last */
                    d = (reach_t(i) + (w_reach_v[i] > 0.5f ? WORLD_AWAY_S : 0)) * WORLD_DASH;
                if (src_only_m >> i & 1)
                    d += 1e6f;  /* (a source's block after the player's reach-only ones) */
                if (want >= 0 && (w_ronly_m >> want & 1) && !(w_ronly_m >> i & 1))
                    wd = 1e30f;  /* (a link-wanted block goes before any wanted by the reach alone) */
                if (d < wd)
                    want = i, wd = d;
            }
        if (want >= 0 && w_start(want)) {
            w_read_step(want, 1);
            goto unload;
        }
        if (want < 0) {  /* every wanted block is on: a lean one wanted again (or underfoot) reads its tables back */
            int lw = -1;
            float ld = 1e30f;
            for (int i = 1; i < w_nblk; i++)
                if (w_rt[i].state == W_ON && w_rt[i].lean == 1 && (w_want_m >> i & 1 || i == w_cur)) {
                    float d = box_d(&w_tab[i], feet);
                    if (d < ld)
                        lw = i, ld = d;
                }
            if (lw >= 0 && w_relite_start(lw)) {
                w_relite_step(lw);
                goto unload;
            }
            if (lw >= 0 && w_rt[lw].retry_s >= WORLD_RETRY_S)
                w_room_stuck = !room_for(lw, feet);
        }
        /* a block only a source wants (a respawn point) with nothing it may turn off waits without holding the rest
         * of the chain back: the boot block's tables, the panorama and the relief go on */
        int src_wait = want >= 0 && (src_only_m >> want & 1) && room_pick(want, feet) < 0;
        if (want >= 0 && !src_wait && (w_rt[want].retry_s >= WORLD_RETRY_S || w_warping || slots_full())) {
            int def = w_slot_deferred(want, feet);
            w_room_stuck = !room_for(want, feet) && !def && !(yield_m >> want & 1);  /* a fresh failure for room, or no
                                                                                    * slot (not a deferred or yielded one) */
            static int def_last = -1;
            if (def && def_last != want) {
                printf("world: block %d wanted, no slot: deferred until the player moves (slots", want);
                for (int s = 0; s < WORLD_SLOTS; s++)
                    printf(" %d", w_slot_of[s]);
                printf(")\n");
            }
            def_last = def ? want : -1;
        }
        else if (world_stats.reading < 0) {
            w_boot_step(1);  /* nothing for the lite sets: the boot block's tables when it is wanted back, */
            if (world_stats.reading < 0)
                w_far_step();  /* the far panorama of the block underfoot, */
            if (world_stats.reading < 0)
                w_bump_step();  /* and its relief */
        }
    }
    for (int i = 1; i < w_nblk; i++)  /* then the textures, enemies and events of blocks whose collision is on */
        if (w_rt[i].state == W_ON && w_rt[i].stage == 1 && world_stats.reading < 0) {
            w_read_step(i, 1);
            if (world_stats.reading == i || w_rt[i].stage == 2)
                break;
            w_room_stuck |= !w_rt[i].tex_held;  /* no room for its texture buffer (settle stops on it) */
        }
unload:
    /* unload blocks the triangle under the player has not kept for WORLD_KEEP_S (never the boot block or the one under
     * the player) */
    for (int i = 1; i < w_nblk; i++)
        if (i != w_cur && w_rt[i].state == W_ON && !(w_keep_m >> i & 1) && world_stats.reading != i &&
            (unkept_s[i] >= WORLD_KEEP_S || w_warping))
            w_turn_off(i);
}
