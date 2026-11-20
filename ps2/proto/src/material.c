/* material.c: the material class table (material.h, Polaris 2.1). */
#include <stdio.h>
#include "material.h"
#include "vu1_layout.h"

#define MAT_ROW(name, prog, alpha, test, zmsk, passes) {#name, VU1L_CODE_##prog, alpha, test, zmsk, passes},
#define VU1L_CODE_SKIN VU1L_CODE_SKIN2  /* the SKIN row: vu1skin2 (vu1skinclip for the batches it clips) */
const MatRow mat_rows[MAT_COUNT] = {MAT_CLASSES(MAT_ROW)};
/* the batch TEST word vu1.c writes follows the blend bit (VU1_BLEND: GS_TEST_ZGE, else GS_TEST_CUTOUT): every row's TEST
 * must be that word for its Z mask */
#define MAT_TEST_OK(name, prog, alpha, test, zmsk, passes) && (test) == ((zmsk) ? GS_TEST_ZGE : GS_TEST_CUTOUT)
_Static_assert(1 MAT_CLASSES(MAT_TEST_OK), "material_table.h: a row's TEST differs from what vu1.c writes for its blend bit");

/* chunk flag bits the check reads (map_i.h CHUNK_BLEND / CHUNK_ADD) */
#define F_BLEND 2
#define F_ADD 4

int mat_census(int block, const unsigned char *tex_tab, int ntex, const void *chunks, int n, int stride, int has_cls)
{
    static unsigned noted;
    if (!has_cls) {
        if (!(noted >> (block & 31) & 1))
            noted |= 1u << (block & 31), printf("map: block %d: no material classes (exported before Polaris 2.1)\n", block);
        return 0;
    }
    int count[MAT_COUNT] = {0}, bad = 0;
    for (int i = 0; i < n; i++) {
        const unsigned char *c = (const unsigned char *)chunks + (unsigned)i * stride;
        int tex = *(const int *)c;
        unsigned fl = *(const unsigned *)(c + 4);
        int k = mat_chunk_class(tex_tab, tex < ntex ? tex : -1, fl);
        count[k]++;
        const MatRow *r = &mat_rows[k];
        /* the streaming / culling code still routes by CHUNK_BLEND; CHUNK_ADD = the additive row (the sky role aside) */
        if (r->zmsk != !!(fl & F_BLEND) || (r->alpha == GS_ALPHA_ADD) != !!(fl & F_ADD)) {
            if (bad++ < 4)
                printf("TEST error: material: block %d chunk %d: class %s, flags %x\n", block, i, r->name, fl);
        }
    }
    printf("map: block %d materials:", block);
    for (int k = 0; k < MAT_COUNT; k++)
        if (count[k])
            printf(" %s %d", mat_rows[k].name, count[k]);
    printf("%s\n", bad ? " (flag mismatches)" : "");
    return bad;
}
