/* DSR's item database (items.h; file formats at the top of tools/export_items.py) */
#include <kernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "progress.h"
#include "mem.h"
#include "items.h"
#include "pack.h"
#include "chunk.h"

typedef struct { u32 row; u16 max; u8 type, flags; } Goods;
typedef struct { u32 row; u16 max, pad; } Ammo;

static Pack mp;               /* menu.pak, kept open for the raw entries (desc.bin, icons.bin) */
static int mp_open;
static unsigned char *core;   /* items.bin */
static const Goods *goods;
static const Ammo *ammo;
static int ngoods, nammo;
static unsigned desc_off, icons_off, icons_size;
static unsigned char *icon_head; /* icons.bin's first bytes: its header and id table (icon_ids points into it) */
static const u16 *icon_ids;   /* icons.bin's IDS chunk */
static const u32 *icon_map;   /* items.bin: (u32 item id, u16 icon, u16 0) sorted by id */
static int nicon_map;
static int nicons, icon_bytes, icon_size, icons_data;

/* itemx.bin while a menu is open */
static PackJob xjob;
static int xstate;            /* 0 none, 1 loading, 2 loaded, -1 failed */
static unsigned char *xbuf;
static unsigned xalloc;
static void (*xrelease)(void *, unsigned);
static const ItemRow *xrows;
static const ItemReinforce *xrein;
static const char *xnames;
static int nx, nrein;

static int reading;           /* the shared async slot (pack.c) carries this file's read */

/* bytes [0, n) of a raw entry at offset off into dst (64-byte aligned, n a multiple of 64) */
static int read_head(unsigned off, void *dst, unsigned n)
{
    int r = -1;
    pack_async_wait();
    if (pack_read_async(&mp, off, dst, n)) {
        pack_async_wait();
        pack_async_done(&r);
        InvalidDCache(dst, (char *)dst + n - 1);
    }
    return r == (int)n;
}

int items_init(void)
{
    if (!pack_open(&mp, PACK_ROOT "menu.pak")) {
        printf("items: no menu.pak\n");
        return 0;
    }
    mp_open = 1;
    unsigned size, n1, n2, n3;
    core = pack_load_static(&mp, "items.bin", &size);  /* boot lifetime (src/mem.h) */
    ChunkFile f;  /* items.bin: chunk file 'ITEM' (tools/export_items.py), read in place */
    if (chunk_open(&f, core, size, CHUNK_ID('I', 'T', 'E', 'M'), "items.bin")) {
        goods = chunk_get(&f, CHUNK_ID('G', 'O', 'O', 'D'), 1, &n1);
        ammo = chunk_get(&f, CHUNK_ID('A', 'M', 'M', 'O'), 1, &n2);
        icon_map = chunk_get(&f, CHUNK_ID('I', 'C', 'M', 'P'), 1, &n3);
    }
    if (!core || !goods || !ammo || !icon_map) {
        printf("items: items.bin missing or bad\n");
        mem_release(core), core = 0;
        goods = 0, ammo = 0, icon_map = 0;
        return 0;
    }
    ngoods = n1 / sizeof(Goods), nammo = n2 / sizeof(Ammo), nicon_map = n3 / 8;
    unsigned dsize;
    static u32 dh[16] __attribute__((aligned(64)));
    if (!pack_entry_raw(&mp, "desc.bin", &desc_off, &dsize))
        printf("items: desc.bin not stored raw\n");
    else if (read_head(desc_off, dh, 64)) {  /* desc.bin 'DESC': the TEXT payload's offset; texts are read from it */
        const void *t = chunk_open_part(&f, dh, 64, dsize, CHUNK_ID('D', 'E', 'S', 'C'), "desc.bin") ?
                        chunk_get(&f, CHUNK_ID('T', 'E', 'X', 'T'), 1, NULL) : NULL;
        desc_off = t ? desc_off + chunk_offset(&f, t) : 0;
    } else
        desc_off = 0;
    if (pack_entry_raw(&mp, "icons.bin", &icons_off, &icons_size)) {
        /* icons.bin 'ICON': the header and IHDR first, then the head again with the IDS table and CELL's header */
        static u32 ih[16] __attribute__((aligned(64)));
        const u32 *ihdr = NULL;
        if (read_head(icons_off, ih, 64) && chunk_open_part(&f, ih, 64, icons_size, CHUNK_ID('I', 'C', 'O', 'N'), "icons.bin"))
            ihdr = chunk_get(&f, CHUNK_ID('I', 'H', 'D', 'R'), 1, NULL);
        unsigned hb = ihdr ? (64 + ihdr[0] * 2 + 64 + 63) & ~63u : 0, idb;
        if (hb > icons_size)
            hb = (icons_size + 63) & ~63u;
        icon_head = hb ? memalign(64, hb) : NULL;
        const void *cell;
        if (icon_head && read_head(icons_off, icon_head, hb) &&
            chunk_open_part(&f, icon_head, hb, icons_size, CHUNK_ID('I', 'C', 'O', 'N'), "icons.bin") &&
            (icon_ids = chunk_get(&f, CHUNK_ID('I', 'D', 'S', ' '), 1, &idb)) &&
            (cell = chunk_get(&f, CHUNK_ID('C', 'E', 'L', 'L'), 1, NULL)) && idb == 2 * ihdr[0]) {
            nicons = ihdr[0], icon_bytes = ihdr[1], icon_size = ihdr[2];
            icons_data = (int)chunk_offset(&f, cell);
        } else {  /* (a silent 0 here hid every item icon for a day: say why) */
            printf("items: icons.bin unreadable: no item icons\n");
            free(icon_head), icon_head = NULL, icon_ids = NULL, nicons = 0;
        }
    }
    printf("items: %d goods, %d ammo rows, %d icons (%d B each), resident %d B\n", ngoods, nammo, nicons, icon_bytes,
           (int)size + (nicons * 2 + 63) / 64 * 64);
    return 1;
}

static const Goods *find_goods(u32 row)
{
    int lo = 0, hi = ngoods - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (goods[m].row == row)
            return &goods[m];
        if (goods[m].row < row)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return 0;
}

static const Ammo *find_ammo(u32 row)
{
    int lo = 0, hi = nammo - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (ammo[m].row == row)
            return &ammo[m];
        if (ammo[m].row < row)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return 0;
}

int item_max(u32 id)
{
    if (!core)
        return 99;
    if (ITEM_CAT(id) == ITEM_GOODS) {
        const Goods *g = find_goods(ITEM_ROW(id));
        return g ? (g->max ? g->max : 1) : 1;
    }
    if (ITEM_CAT(id) == ITEM_WEAPON && find_ammo(ITEM_ROW(ITEM_BASE(id))))
        return 99;
    return 1;
}

int item_goods_type(u32 id)
{
    const Goods *g = ITEM_CAT(id) == ITEM_GOODS && core ? find_goods(ITEM_ROW(id)) : 0;
    return g ? g->type : -1;
}

int item_goods_flags(u32 id)
{
    const Goods *g = ITEM_CAT(id) == ITEM_GOODS && core ? find_goods(ITEM_ROW(id)) : 0;
    return g ? g->flags : 0;
}

int item_tab(u32 id)
{
    switch (ITEM_CAT(id)) {
    case ITEM_GOODS:
        switch (item_goods_type(id)) {
        case GOODS_KEY: return TAB_KEY;
        case GOODS_MATERIAL: return TAB_MATERIAL;
        case GOODS_SPELL: return TAB_SPELL;
        default: return TAB_CONSUME;
        }
    case ITEM_WEAPON:
        return core && find_ammo(ITEM_ROW(ITEM_BASE(id))) ? TAB_AMMO : TAB_WEAPON;
    case ITEM_PROTECTOR:
        return TAB_ARMOR;
    default:
        return TAB_RING;
    }
}

static void *(*xget_fn)(unsigned);
static void *xget(unsigned n)
{
    void *p = xget_fn(n);
    if (p)
        xalloc = n;
    return p;
}

int items_detail_step(void *(*alloc)(unsigned), void (*release)(void *, unsigned))
{
    if (xstate == 2 || xstate == -1)
        return xstate == 2 ? 1 : -1;
    if (!mp_open)
        return xstate = -1;
    if (!xstate) {
        xget_fn = alloc, xrelease = release;
        pack_job_start(&xjob, PACK_ROOT "menu.pak", "itemx.bin", xget, release);
        xstate = 1;
    }
    int r = pack_job_step(&xjob);
    if (!r)
        return 0;
    if (r < 0)
        return xstate = -1;
    xbuf = xjob.out;
    ChunkFile f;  /* itemx.bin: chunk file 'ITMX', read in place */
    unsigned n1, n2, n3;
    if (chunk_open(&f, xbuf, xjob.raw, CHUNK_ID('I', 'T', 'M', 'X'), "itemx.bin")) {
        xrows = chunk_get(&f, CHUNK_ID('R', 'O', 'W', 'S'), 1, &n1);
        xrein = chunk_get(&f, CHUNK_ID('R', 'E', 'I', 'N'), 1, &n2);
        xnames = chunk_get(&f, CHUNK_ID('N', 'A', 'M', 'E'), 1, &n3);
    }
    if (!xrows || !xrein || !xnames || n1 % sizeof(ItemRow)) {
        printf("items: itemx.bin bad\n");
        release(xbuf, xalloc), xbuf = 0, xrows = 0;
        return xstate = -1;
    }
    nx = n1 / sizeof(ItemRow), nrein = n2 / sizeof(ItemReinforce);
    xstate = 2;
    return 1;
}

void items_detail_free(void)
{
    if (xstate == 1)
        pack_job_cancel(&xjob);
    if (xbuf && xrelease)
        xrelease(xbuf, xalloc);
    xbuf = 0, xrows = 0, nx = 0, xstate = 0;
}

int items_detail_ready(void) { return xstate == 2; }

const ItemRow *item_row(u32 id)
{
    if (xstate != 2)
        return 0;
    u32 key = ITEM_BASE(id);
    int lo = 0, hi = nx - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (xrows[m].id == key)
            return &xrows[m];
        if (xrows[m].id < key)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return 0;
}

const ItemReinforce *item_reinforce(int type, int level)
{
    u32 key = type + level;
    int lo = 0, hi = nrein - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (xrein[m].id == key)
            return &xrein[m];
        if (xrein[m].id < key)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return 0;
}

const char *item_name(u32 id, char *buf, int n)
{
    const ItemRow *r = item_row(id);
    if (!r) {
        snprintf(buf, n, "%s", "");
        return buf;
    }
    int lv = ITEM_LEVEL(id);
    if (lv)
        snprintf(buf, n, "%s+%d", xnames + r->name, lv);
    else
        snprintf(buf, n, "%s", xnames + r->name);
    return buf;
}

/* desc.bin text of one row: a background read of the help line + description (both NUL-terminated) */
static char *text_dst;
static int text_bytes, text_help, text_state;  /* state 0 idle, 1 reading, 2 done, -1 failed */

int item_text_request(u32 id, char *buf, int n)
{
    const ItemRow *r = item_row(id);
    if (!r || !desc_off)
        return -1;
    int bytes = r->help_len + r->desc_len;
    if (bytes > n || bytes <= 0)
        return -1;
    if (reading || !pack_read_async(&mp, desc_off + r->text, buf, bytes))
        return 0;
    reading = 1;
    text_dst = buf, text_bytes = bytes, text_help = r->help_len, text_state = 1;
    return 1;
}

int item_text_done(int *desc)
{
    if (text_state != 1)
        return text_state == 2 ? 1 : text_state;
    int r;
    if (reading != 1 || !pack_async_done(&r))
        return 0;
    reading = 0;
    InvalidDCache(text_dst, text_dst + text_bytes - 1);
    text_state = r == text_bytes ? 2 : -1;
    text_dst[text_bytes - 1] = 0;
    *desc = text_help;
    return text_state == 2 ? 1 : -1;
}

int item_icon_of(u32 id)
{
    id = ITEM_BASE(id);  /* a weapon's reinforcement level shares the base row's icon */
    int lo = 0, hi = nicon_map - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (icon_map[2 * m] == id)
            return (int)(icon_map[2 * m + 1] & 0xFFFF);
        if (icon_map[2 * m] < id)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return -1;
}

int item_icon_index(int icon)
{
    int lo = 0, hi = nicons - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (icon_ids[m] == icon)
            return m;
        if (icon_ids[m] < icon)
            lo = m + 1;
        else
            hi = m - 1;
    }
    return -1;
}

int item_icon_bytes(void) { return icon_bytes; }
int item_icon_size(void) { return icon_size; }

static void *icon_dst;
int item_icon_read(int k, void *dst)
{
    if (k < 0 || k >= nicons || reading)
        return 0;
    if (!pack_read_async(&mp, icons_off + icons_data + k * icon_bytes, dst, icon_bytes))
        return 0;
    reading = 2;
    icon_dst = dst;
    return 1;
}

int item_icon_done(void)
{
    int r;
    if (reading != 2 || !pack_async_done(&r))
        return 0;
    reading = 0;
    InvalidDCache(icon_dst, (char *)icon_dst + icon_bytes - 1);
    return r == icon_bytes ? 1 : -1;
}
