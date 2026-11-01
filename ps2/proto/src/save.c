/* Save / load (save.h) */
#define NEWLIB_PORT_AWARE  /* fio calls, as pack.c */
#include <fileio.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "save.h"
#include "flags.h"
#include "pack.h"
#include "progress.h"

#define SAVE_MAGIC 0x53525344u  /* 'DSRS' */
#define HDR 32
#define SAVE_BUF (HDR + sizeof(Progress) + 4 + FLAG_PAGES * (4 + 128))
#define MC_DIR "mc0:/DSRPS2"  /* ASSUMED (TECH_DEBT save-7): no icon.sys / icon yet, the BIOS browser shows it as corrupt */

static u8 buf[SAVE_BUF] __attribute__((aligned(64)));
static int device = -1, mc_loaded, loaded, request;
static char host_name[32] = "dsrsave";
static u32 seq;
static const char *area;
static unsigned seen_flags, seen_prog;
static float since, play_acc;

static inline unsigned ticks(void)
{
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

static u32 crc32(const u8 *p, int n)
{
    u32 c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = c >> 1 ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

static void slot_path(char *out, int n, int slot)
{
    if (device == SAVE_MC)
        snprintf(out, n, MC_DIR "/SAVE%d.BIN", slot);
    else
        snprintf(out, n, "host:save/%s%d.bin", host_name, slot);  /* build/save/: writable (pcsx2-run.sh) */
}

void save_select(const char *dev)
{
    if (!strncmp(dev, "mc", 2))
        device = SAVE_MC;
    else if (!strncmp(dev, "host", 4)) {
        device = SAVE_HOST;
        if (dev[4] == ' ' && dev[5])
            snprintf(host_name, sizeof host_name, "%s", dev + 5);
    } else
        device = SAVE_OFF;
}

/* rom0:MCMAN gives fio the mc0: device; 1 if a formatted card is in slot 1 */
static int mc_ready(void)
{
    if (!mc_loaded) {
        mc_loaded = SifLoadModule("rom0:MCMAN", 0, NULL) >= 0 && SifLoadModule("rom0:MCSERV", 0, NULL) >= 0 ? 1 : -1;
        if (mc_loaded < 0)
            printf("save: rom0:MCMAN / MCSERV failed to load\n");
    }
    if (mc_loaded < 0)
        return 0;
    pack_async_wait();
    int d = fioDopen("mc0:/");
    if (d < 0)
        return 0;
    fioDclose(d);
    return 1;
}

/* No fioRemove: the boot ROM's FILEIO (rom0, which this build talks to) creates a directory of that name instead,
 * on host: and mc0: alike (seen 2026-11-01). Saves are overwritten in place; a longer old file keeps its tail, which
 * read_slot ignores (the header has the size). */
static int open_write(const char *path)
{
    int fd = fioOpen(path, FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC);
    return fd >= 0 ? fd : fioOpen(path, FIO_O_WRONLY | FIO_O_CREAT);
}

/* Reads one slot into buf; returns its size if the header, size and CRC are valid, else 0 */
static int read_slot(int slot)
{
    char path[64];
    slot_path(path, sizeof path, slot);
    pack_async_wait();
    int fd = fioOpen(path, FIO_O_RDONLY);
    if (fd < 0)
        return 0;
    int n = fioRead(fd, buf, SAVE_BUF);
    fioClose(fd);
    u32 h[8];
    if (n < HDR)
        return 0;
    memcpy(h, buf, HDR);
    if (h[0] != SAVE_MAGIC || (h[1] & 0xFFFF) > SAVE_VERSION || h[2] > (u32)n || h[2] < HDR ||
        h[3] != crc32(buf + HDR, h[2] - HDR) || HDR + h[5] + h[6] != h[2]) {
        if (h[0])
            printf("save: %s invalid (size %d)\n", path, n);
        return 0;
    }
    return h[2];
}

int save_load(void)
{
    if (device <= SAVE_OFF)
        return 0;
    u32 best = 0;
    int best_slot = -1;
    for (int s = 0; s < 2; s++)
        if (read_slot(s)) {
            u32 q;
            memcpy(&q, buf + 16, 4);
            if (best_slot < 0 || q > best)
                best = q, best_slot = s;
        }
    if (best_slot < 0)
        return 0;
    int n = read_slot(best_slot);
    u32 h[8];
    memcpy(h, buf, HDR);
    /* older / newer Progress layouts: fields present in both are kept, the rest stay at their new-game values */
    progress_new_game();
    memcpy(&prog, buf + HDR, h[5] < sizeof prog ? h[5] : sizeof prog);
    if (prog.inv_count < 0 || prog.inv_count > INV_MAX)
        prog.inv_count = 0;
    if (flags_load(buf + HDR + h[5], h[6]) < 0)
        printf("save: flag pages invalid\n");
    seq = h[4];
    seen_flags = flags_changes(), seen_prog = progress_changes();
    char path[64];
    slot_path(path, sizeof path, best_slot);
    printf("save: loaded %s (%d B, seq %u, v%u): souls %d, level %d, %d items, %d flag pages\n", path, n,
           (unsigned)seq, (unsigned)(h[1] & 0xFFFF), (int)prog.souls, (int)prog.level, (int)prog.inv_count,
           flags_pages_used());
    return 1;
}

int save_boot(int argc, char **argv, const char *boot_area)
{
    area = boot_area;
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "save=", 5))
            save_select(argv[i] + 5);
    if (device < 0) {
        int test = 0;
        for (int i = 1; i < argc; i++)
            test |= !strncmp(argv[i], "test=", 5);
        device = test ? SAVE_OFF : mc_ready() ? SAVE_MC : SAVE_HOST;
    } else if (device == SAVE_MC && !mc_ready()) {
        printf("save: no formatted memory card in slot 1, saves off\n");
        device = SAVE_OFF;
    }
    flags_init();
    progress_new_game();
    loaded = save_load();
    if (!loaded)
        seen_flags = flags_changes(), seen_prog = progress_changes();
    printf("save: device %s, %s; resident %d B (flags %d, progress %d, buffer %d)\n",
           device == SAVE_MC ? "mc0:" : device == SAVE_HOST ? "host:" : "off", loaded ? "loaded" : "new game",
           save_resident_bytes(), flags_resident_bytes(), (int)sizeof prog, (int)sizeof buf);
    return loaded;
}

void save_apply_player(Player *pl)
{
    if (!loaded)
        return;
    if (prog.map_id && prog.map_id == progress_map_id(area)) {
        pl->pos = v3(prog.pos[0], prog.pos[1], prog.pos[2]);
        pl->yaw = prog.yaw;
        pl->fall_y0 = pl->pos.y;
    }
    if (prog.hp > 0)
        pl->hp = prog.hp < pl->hp_max ? prog.hp : pl->hp_max;
    pl->deaths = prog.deaths;
}

int save_write(const Player *pl)
{
    if (device <= SAVE_OFF)
        return -1;
    unsigned t0 = ticks();
    if (pl) {
        prog.map_id = progress_map_id(area);
        prog.pos[0] = pl->pos.x, prog.pos[1] = pl->pos.y, prog.pos[2] = pl->pos.z, prog.yaw = pl->yaw;
        prog.hp = pl->hp, prog.deaths = pl->deaths;
    }
    int nf = flags_save(buf + HDR + sizeof prog, SAVE_BUF - HDR - sizeof prog);
    if (nf < 0)
        return -1;
    memcpy(buf + HDR, &prog, sizeof prog);
    int n = HDR + sizeof prog + nf;
    u32 h[8] = {SAVE_MAGIC, SAVE_VERSION | HDR << 16, n, 0, ++seq, sizeof prog, nf, 0};
    h[3] = crc32(buf + HDR, n - HDR);
    memcpy(buf, h, HDR);
    char path[64];
    slot_path(path, sizeof path, seq & 1);
    pack_async_wait();
    if (device == SAVE_MC)
        fioMkdir(MC_DIR);  /* fails harmlessly when it exists */
    int fd = open_write(path);
    int w = fd >= 0 ? fioWrite(fd, buf, n) : -1;
    if (fd >= 0)
        fioClose(fd);
    unsigned ms = (ticks() - t0) / 294912;
    if (w != n) {
        printf("save: WRITE ERROR %s (fd %d, %d of %d B)\n", path, fd, w, n);
        seq--;
        return -1;
    }
    seen_flags = flags_changes(), seen_prog = progress_changes();
    since = 0;
    printf("save: wrote %s %d B (seq %u, %d flag pages) in %u ms\n", path, n, (unsigned)seq, flags_pages_used(), ms);
    return 0;
}

void save_request(void) { request = 1; }

void save_tick(const Player *pl, float dt)
{
    if ((play_acc += dt) >= 1)
        play_acc -= 1, prog.play_time++;
    if (device <= SAVE_OFF)
        return;
    since += dt;
    int dirty = flags_changes() != seen_flags || progress_changes() != seen_prog;
    if (request || (dirty && since >= SAVE_INTERVAL)) {
        request = 0;
        save_write(pl);
    }
}

int save_device(void) { return device; }
int save_resident_bytes(void) { return (int)(sizeof buf + sizeof prog) + flags_resident_bytes(); }

static int check(const char *what, long long got, long long want)
{
    if (got == want) {
        printf("TEST check %s = %lld ok\n", what, got);
        return 0;
    }
    printf("TEST error check %s = %lld, expected %lld\n", what, got, want);
    return -1;
}

int save_test_cmd(const char *line, const Player *pl)
{
    char w[16], what[16];
    unsigned long a = 0, b = 0, c = 0;
    int k = 0;
    if (sscanf(line, " %15s%n", w, &k) != 1)
        return -1;
    const char *r = line + k;
    if (!strcmp(w, "flag") && sscanf(r, "%lu %lu", &a, &b) == 2)
        flag_set(a, b);
    else if (!strcmp(w, "flags") && sscanf(r, "%lu %lu %lu", &a, &b, &c) == 3)
        flag_range_set(a, b, c);
    else if (!strcmp(w, "souls") && sscanf(r, "%lu", &a) == 1)
        prog.souls = a, progress_touch();
    else if (!strcmp(w, "item") && sscanf(r, "%li %li", (long *)&a, (long *)&b) == 2)
        inv_add(a, (long)b);
    else if (!strcmp(w, "save"))
        return save_write(pl) ? (printf("TEST error save failed (device %d)\n", device), -1) : 0;
    else if (!strcmp(w, "load"))
        return save_load() ? 0 : (printf("TEST error load: no valid save\n"), -1);
    else if (!strcmp(w, "wipe")) {
        for (int s = 0; s < 2 && device > SAVE_OFF; s++) {
            char path[64];
            slot_path(path, sizeof path, s);
            pack_async_wait();
            int fd = open_write(path);  /* a zeroed header reads as no save (no fioRemove, see open_write) */
            if (fd >= 0) {
                memset(buf, 0, HDR);
                fioWrite(fd, buf, HDR);
                fioClose(fd);
            }
        }
        seq = 0;
    } else if (!strcmp(w, "check") && sscanf(r, " %15s%n", what, &k) == 1) {
        r += k;
        if (!strcmp(what, "flag") && sscanf(r, "%lu %lu", &a, &b) == 2) {
            char t[24];
            snprintf(t, sizeof t, "flag %lu", a);
            return check(t, flag_get(a), b);
        }
        if (!strcmp(what, "flags") && sscanf(r, "%lu %lu %lu", &a, &b, &c) == 3) {
            char t[32];
            snprintf(t, sizeof t, "flags %lu-%lu", a, b);
            return check(t, flag_range_count(a, b), c);
        }
        if (!strcmp(what, "souls") && sscanf(r, "%lu", &a) == 1)
            return check("souls", prog.souls, a);
        if (!strcmp(what, "item") && sscanf(r, "%li %lu", (long *)&a, &b) == 2)
            return check("item", inv_count(a), b);
        if (!strcmp(what, "loaded") && sscanf(r, "%lu", &a) == 1)
            return check("loaded", loaded, a);
        printf("TEST error check: bad '%s'\n", line);
        return -1;
    } else {
        printf("TEST error bad command '%s'\n", line);
        return -1;
    }
    return 0;
}
