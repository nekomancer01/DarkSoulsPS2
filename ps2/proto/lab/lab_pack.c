/* lab_pack.c: the Lab's packs (Polaris 1.7b). src/pack.c reads through the IOP's disc server; the Lab reads the same
 * files (tools/pack.py: "DSRP" v1, a table of 64-byte entries, each entry a table of block sizes then LZ4 blocks of
 * block_size bytes, a set top bit = stored) straight from the build's data/ with stdio. PACK_ROOT's "host:" maps to
 * lab_root. */
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include "pack.h"
#include "lab.h"

struct PackEntry {
    char name[32];
    unsigned offset, raw, packed, blocks, block_size, pad[3];
};
#define RAW_FLAG 0x80000000u

static FILE *files[64];  /* Pack.fd indexes this */

const char *lab_path(const char *path, char *out, unsigned cap)
{
    const char *p = strncmp(path, "host:", 5) ? path : path + 5;
    snprintf(out, cap, "%s/%s", lab_root, p);
    return out;
}

int pack_open(Pack *p, const char *path)
{
    char full[512];
    memset(p, 0, sizeof *p);
    p->fd = -1;
    int k;
    for (k = 0; k < 64 && files[k]; k++)
        ;
    FILE *f = k < 64 ? fopen(lab_path(path, full, sizeof full), "rb") : NULL;
    unsigned head[4];
    if (!f || fread(head, 4, 4, f) != 4 || memcmp(head, "DSRP", 4) || head[1] != 1) {
        if (f)
            fclose(f);
        LAB_READBACK("pack_open failed");
        return 0;
    }
    p->count = head[2];
    p->entries = malloc(head[2] * sizeof *p->entries);
    if (fread(p->entries, sizeof *p->entries, head[2], f) != head[2]) {
        fclose(f), free(p->entries);
        p->entries = NULL, p->count = 0;
        return 0;
    }
    files[k] = f, p->fd = k;
    return 1;
}

void pack_close(Pack *p)
{
    if (p->fd >= 0 && files[p->fd])
        fclose(files[p->fd]), files[p->fd] = NULL;
    free(p->entries);
    p->entries = NULL, p->fd = -1;
}

static int lz4_decode(const unsigned char *s, unsigned n, unsigned char *dst, unsigned cap)
{
    const unsigned char *se = s + n;
    unsigned char *o = dst, *oe = dst + cap;
    while (s < se) {
        unsigned tok = *s++, l = tok >> 4;
        if (l == 15) {
            unsigned b;
            do
                l += b = *s++;
            while (b == 255 && s < se);
        }
        if (o + l > oe || s + l > se)
            return -1;
        memcpy(o, s, l), o += l, s += l;
        if (s >= se)
            break;
        unsigned off = s[0] | s[1] << 8, m = (tok & 15) + 4;
        s += 2;
        if ((tok & 15) == 15) {
            unsigned b;
            do
                m += b = *s++;
            while (b == 255 && s < se);
        }
        if (!off || off > (unsigned)(o - dst) || o + m > oe)
            return -1;
        for (unsigned i = 0; i < m; i++, o++)
            *o = o[-(int)off];
    }
    return (int)(o - dst);
}

static struct PackEntry *find(const Pack *p, const char *name)
{
    for (unsigned i = 0; i < p->count; i++)
        if (!strncmp(p->entries[i].name, name, sizeof p->entries[i].name))
            return &p->entries[i];
    return NULL;
}

void *pack_load_with(Pack *p, const char *name, unsigned *size, void *(*get)(unsigned), void (*put)(void *, unsigned))
{
    (void)put;
    struct PackEntry *e = find(p, name);
    if (!e || p->fd < 0)
        return NULL;
    FILE *f = files[p->fd];
    unsigned nb = e->blocks, *cs = malloc(nb * 4 + 4);
    unsigned char *out = get((e->raw + 127) & ~127u), *src = malloc(e->packed);
    fseek(f, e->offset, SEEK_SET);
    int ok = fread(cs, 4, nb, f) == nb && fread(src, 1, e->packed - nb * 4, f) == e->packed - nb * 4;
    unsigned pos = 0;
    const unsigned char *s = src;
    for (unsigned b = 0; ok && b < nb; b++) {
        unsigned c = cs[b] & ~RAW_FLAG, want = e->raw - pos < e->block_size ? e->raw - pos : e->block_size;
        if (cs[b] & RAW_FLAG)
            ok = c == want, memcpy(out + pos, s, ok ? c : 0);
        else
            ok = lz4_decode(s, c, out + pos, want) == (int)want;
        s += c, pos += want;
    }
    free(cs), free(src);
    if (!ok) {
        LAB_READBACK("pack_load failed");
        return NULL;
    }
    if (size)
        *size = e->raw;
    return out;
}

/* model.c's paged reads (lab_stubs.c pack_req_read): bytes at a pack offset, synchronously */
int lab_pack_read(Pack *p, unsigned offset, void *dst, unsigned bytes)
{
    if (p->fd < 0 || !files[p->fd])
        return -1;
    fseek(files[p->fd], offset, SEEK_SET);
    return (int)fread(dst, 1, bytes, files[p->fd]);
}

static void *heap_get(unsigned n) { return memalign(128, n); }
void *pack_load(Pack *p, const char *name, unsigned *size) { return pack_load_with(p, name, size, heap_get, NULL); }
void *pack_load_static(Pack *p, const char *name, unsigned *size) { return pack_load(p, name, size); }
void *pack_load_temp(Pack *p, const char *name, unsigned *size) { return pack_load(p, name, size); }
unsigned pack_entry_size(const Pack *p, const char *name)
{
    struct PackEntry *e = find(p, name);
    return e ? e->raw : 0;
}
int pack_entry_raw(Pack *p, const char *name, unsigned *offset, unsigned *size)
{
    struct PackEntry *e = find(p, name);
    if (!e || e->packed != e->blocks * 4 + e->raw)
        return 0;
    *offset = e->offset + e->blocks * 4, *size = e->raw;
    return 1;
}
