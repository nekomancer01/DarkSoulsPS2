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
#define MC_DIR "mc0:/DSRPS2"  /* ASSUMED (TECH_DEBT save-7): not a product-code folder name */

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

/* The card browser's view of the save folder: icon.sys (title, background, lights) and a 3D icon (one textured quad).
 * Both made here, nothing from the game: a dark square with an ember-coloured flame. Layouts are the BIOS browser's
 * documented ones; the icon's model scale and its one-frame animation block are ASSUMED (TECH_DEBT save-7). */
#define ICON_TEX 128
_Static_assert(SAVE_BUF >= 20 + 6 * 24 + 44 + ICON_TEX * ICON_TEX * 2, "icon.ico is built in buf");
static int open_write(const char *path);
static int sjis_put(u8 *d, const char *s)  /* ASCII -> full-width Shift-JIS (the browser shows titles in SJIS) */
{
    int n = 0;
    for (; *s; s++) {
        unsigned c = (u8)*s, w;
        if (c >= 'A' && c <= 'Z') w = 0x8260 + c - 'A';
        else if (c >= 'a' && c <= 'z') w = 0x8281 + c - 'a';
        else if (c >= '0' && c <= '9') w = 0x824F + c - '0';
        else if (c == ':') w = 0x8146;
        else if (c == '-') w = 0x817C;
        else w = 0x8140;  /* space and the rest */
        d[n++] = w >> 8, d[n++] = w & 0xFF;
    }
    return n;
}

static int write_file(const char *path, const void *p, int n)
{
    int fd = open_write(path);
    int w = fd >= 0 ? fioWrite(fd, p, n) : -1;
    if (fd >= 0)
        fioClose(fd);
    return w == n;
}

static void mc_icon(void)
{
    static int done;
    if (done)
        return;
    int fd = fioOpen(MC_DIR "/icon.sys", FIO_O_RDONLY);
    if (fd >= 0) {
        fioClose(fd);
        done = 1;
        return;
    }
    /* icon.sys, 964 bytes */
    u8 *d = buf;
    memset(d, 0, 964);
    memcpy(d, "PS2D", 4);
    u32 u;
    float f;
    int l1 = sjis_put(d + 192, "DARK SOULS");
    sjis_put(d + 192 + l1, "PS2 prototype");
    d[6] = l1;  /* second title line: byte offset in the title */
    u = 0x80, memcpy(d + 12, &u, 4);  /* background opacity */
    static const u32 bg[4][4] = {{8, 6, 6, 0}, {8, 6, 6, 0}, {40, 18, 6, 0}, {40, 18, 6, 0}};
    memcpy(d + 16, bg, sizeof bg);    /* corner colours: top left, top right, bottom left, bottom right */
    static const float light[3][4] = {{0, 0, 1, 0}, {0.5f, -0.5f, 0.5f, 0}, {-0.5f, -0.5f, 0.5f, 0}};
    static const float lcol[3][4] = {{0.5f, 0.5f, 0.5f, 0}, {0.2f, 0.2f, 0.2f, 0}, {0.2f, 0.2f, 0.2f, 0}};
    memcpy(d + 80, light, sizeof light), memcpy(d + 128, lcol, sizeof lcol);
    f = 0.7f;
    for (int k = 0; k < 3; k++)
        memcpy(d + 176 + k * 4, &f, 4);  /* ambient */
    for (int k = 0; k < 3; k++)
        strcpy((char *)d + 260 + k * 64, "icon.ico");  /* list / copy / delete icons */
    pack_async_wait();
    int ok = write_file(MC_DIR "/icon.sys", d, 964);
    /* icon.ico: header, 6 vertices (one shape), animation, 128x128 RGB5A1 texture */
    int nv = 6, off = 0;
    u32 hdr[5] = {0x00010000, 1, 0x07, 0x3F800000, nv};  /* version, shapes, texture uncompressed, ?, vertices */
    memcpy(d, hdr, 20), off = 20;
    static const s16 xy[6][2] = {{-1, -2}, {1, -2}, {-1, 0}, {1, -2}, {1, 0}, {-1, 0}};  /* quad, y up is negative */
    for (int v = 0; v < nv; v++) {
        s16 vert[12] = {xy[v][0] * 4096, (s16)(xy[v][1] * 4096), 0, 0,  /* position, 4.12 fixed */
                        0, 0, -4096, 0,                                 /* normal, towards the viewer */
                        (xy[v][0] + 1) * 2048, (s16)((xy[v][1] + 2) * 2048), 0x8080, 0x8080};  /* uv 4.12, rgba */
        memcpy(d + off, vert, 24), off += 24;
    }
    u32 anim[5] = {0x01, 1, 0x3F800000, 0, 1};  /* id, frame length, speed 1.0, offset, frames */
    memcpy(d + off, anim, 20), off += 20;
    u32 frame[4] = {0, 1, 1, 0};                /* shape 0, one key */
    memcpy(d + off, frame, 16), off += 16;
    float key[2] = {1, 1};
    memcpy(d + off, key, 8), off += 8;
    u16 *t = (u16 *)(d + off);
    for (int y = 0; y < ICON_TEX; y++)
        for (int x = 0; x < ICON_TEX; x++) {
            /* flame: a teardrop over a glowing base; distance field in texels */
            float fx = (x - 64) / 64.0f, fy = (y - 64) / 64.0f;
            float w = 0.55f * (fy + 1) * (fy < 0.4f ? 1 : (1 - fy) / 0.6f);  /* half width at this height */
            float in = w > 0 ? 1 - (fx < 0 ? -fx : fx) / w : -1;
            int r = 10, g = 8, b = 8;
            if (in > 0) {
                float h = in * (0.6f + 0.4f * fy);
                r = 120 + (int)(135 * h), g = 40 + (int)(170 * h * h), b = (int)(60 * h * h * h);
            }
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            t[y * ICON_TEX + x] = r >> 3 | (g >> 3) << 5 | (b >> 3) << 10 | 0x8000;
        }
    off += ICON_TEX * ICON_TEX * 2;
    ok &= write_file(MC_DIR "/icon.ico", d, off);
    printf("save: wrote icon.sys + icon.ico (%d B)%s\n", off, ok ? "" : " FAILED");
    done = ok;
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
    if (device == SAVE_MC) {  /* the folder and its browser icon first: mc_icon uses buf as scratch */
        pack_async_wait();
        fioMkdir(MC_DIR);  /* fails harmlessly when it exists */
        mc_icon();
    }
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
        since = 0;  /* also after a failed write (card full or pulled): retry in SAVE_INTERVAL, not every frame */
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
        if (!strcmp(what, "deaths") && pl && sscanf(r, "%lu", &a) == 1)
            return check("deaths", pl->deaths, a);
        if (!strcmp(what, "seq") && sscanf(r, "%lu", &a) == 1)  /* sequence of the last save written or loaded, 0 after wipe */
            return check("seq", seq, a);
        printf("TEST error check: bad '%s'\n", line);
        return -1;
    } else {
        printf("TEST error bad command '%s'\n", line);
        return -1;
    }
    return 0;
}
