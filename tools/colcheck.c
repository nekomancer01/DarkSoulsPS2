/* colcheck: two collision.bin files of one block (DCOL v1 and v2, tools/export_collision.py) give the same answers.
 *
 *   gcc -O2 -std=gnu99 -I ps2/proto/src tools/colcheck.c ps2/proto/src/collision.c -lm -o build/host/colcheck
 *   build/host/colcheck <a/collision.bin> <b/collision.bin>
 *   tools/export_collision.py --repack <v1 collision.bin> <v2 out>   (a v2 file from a v1 one, without the game)
 *
 * Runs the host build of src/collision.c on both: col_floor and col_ray at 200k random points in the block's box
 * (exact: same triangle number and height / fraction), col_push_capsule standing on those floors, and 4000 random
 * walks of 150 col_walk steps from free standing spots. Wall pushes depend on the order the triangles are visited in
 * (collision.c push2); reference: moving the walks' starts by 0.1 mm on one file changes ~50 of 4000 ends. Exit 1
 * when a floor or ray differs. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "collision.h"

static void *slurp(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) {
        perror(p);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *b = malloc(n);
    if (!b || fread(b, 1, n, f) != (size_t)n) {
        perror(p);
        exit(2);
    }
    fclose(f);
    return b;
}
static unsigned rs = 12345;
static float rnd(void)
{
    rs = rs * 1103515245u + 12345u;
    return (rs >> 8) / 16777216.0f;
}

#define N 200000
#define W 4000
#define STEPS 150
typedef struct { int ok, tri; float y; } FR;
typedef struct { int ok, tri; float t; } RR;
static float q[N][6], push_out[2][N][3], walk_st[W][3], walk_dir[W][STEPS], walk_out[2][W][3];
static FR fr[2][N];
static RR rr[2][N];

/* the block's vertex height range (both versions: from the vertices) and x / z box (header grid) */
static void box(const unsigned char *b, float lo[3], float hi[3])
{
    const unsigned *h = (const unsigned *)b;
    const float *hf = (const float *)b;
    lo[0] = hf[8], lo[2] = hf[9];
    if (h[1] == 2) {
        hi[0] = hf[8] + h[11] * hf[10], hi[2] = hf[9] + h[12] * hf[10], lo[1] = hf[21], hi[1] = hf[22];
        return;
    }
    hi[0] = hf[8] + h[11] * hf[10], hi[2] = hf[9] + h[12] * hf[10];
    const short *v = (const short *)(b + h[13]);
    lo[1] = 1e9f, hi[1] = -1e9f;
    for (unsigned i = 0; i < h[2]; i++) {
        float y = hf[5] + v[i * 3 + 1] * hf[7];
        lo[1] = y < lo[1] ? y : lo[1], hi[1] = y > hi[1] ? y : hi[1];
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: colcheck a/collision.bin b/collision.bin\n");
        return 2;
    }
    void *blob[2] = {slurp(argv[1]), slurp(argv[2])};
    float lo[3], hi[3];
    box(blob[0], lo, hi);
    for (int i = 0; i < N; i++) {
        for (int k = 0; k < 3; k++)
            q[i][k] = lo[k] + rnd() * (hi[k] - lo[k]);
        for (int k = 3; k < 6; k++)
            q[i][k] = (rnd() - 0.5f) * 16;
    }
    for (int v = 0; v < 2; v++) {
        if (!col_load(0, blob[v])) {
            fprintf(stderr, "colcheck: %s: not a collision.bin\n", argv[1 + v]);
            return 2;
        }
        for (int i = 0; i < N; i++) {
            ColHit h;
            fr[v][i].ok = col_floor(q[i][0], q[i][2], q[i][1] + 3, q[i][1] - 10, COL_SOLID, &h);
            if (fr[v][i].ok)
                fr[v][i].tri = h.tri, fr[v][i].y = h.y;
            float to[3] = {q[i][0] + q[i][3], q[i][1] + q[i][4], q[i][2] + q[i][5]};
            rr[v][i].ok = col_ray(q[i], to, COL_CAMERA, &h);
            if (rr[v][i].ok)
                rr[v][i].tri = h.tri, rr[v][i].t = h.t;
            float p[3] = {q[i][0], fr[0][i].ok ? fr[0][i].y : q[i][1], q[i][2]};
            if (fr[0][i].ok)
                col_push_capsule(p, COL_PLAYER_R, COL_PLAYER_H, COL_STEP_UP, COL_SOLID);
            memcpy(push_out[v][i], p, 12);
        }
        if (!v)  /* walk starts: free standing on a floor of the first file */
            for (int n = 0, tries = 0; n < W && tries < 50 * N; tries++) {
                float x = lo[0] + rnd() * (hi[0] - lo[0]), z = lo[2] + rnd() * (hi[2] - lo[2]);
                float y = lo[1] + rnd() * (hi[1] - lo[1]);
                ColHit h;
                if (!col_floor(x, z, y + 3, y - 10, COL_SOLID, &h))
                    continue;
                float p[3] = {x, h.y, z};
                if (col_push_capsule(p, COL_PLAYER_R, COL_PLAYER_H, COL_STEP_UP, COL_SOLID))
                    continue;
                memcpy(walk_st[n], p, 12);
                float a = rnd() * 6.2832f;
                for (int s = 0; s < STEPS; s++)
                    walk_dir[n][s] = a += (rnd() - 0.5f) * 0.6f;
                n++;
            }
        for (int i = 0; i < W; i++) {
            float p[3];
            memcpy(p, walk_st[i], 12);
            for (int s = 0; s < STEPS; s++)
                if (!col_walk(p, cosf(walk_dir[i][s]) * 0.2f, sinf(walk_dir[i][s]) * 0.2f, COL_PLAYER_R, COL_PLAYER_H,
                              COL_STEP_UP, COL_STEP_DOWN, COL_SOLID, 0))
                    break;
            memcpy(walk_out[v][i], p, 12);
        }
        col_unload(0);
    }
    int fd = 0, rd = 0, pd = 0, wd = 0, fh = 0, rh = 0;
    for (int i = 0; i < N; i++) {
        fh += fr[0][i].ok, rh += rr[0][i].ok;
        fd += fr[0][i].ok != fr[1][i].ok || (fr[0][i].ok && (fr[0][i].tri != fr[1][i].tri || fr[0][i].y != fr[1][i].y));
        rd += rr[0][i].ok != rr[1][i].ok || (rr[0][i].ok && (rr[0][i].tri != rr[1][i].tri || rr[0][i].t != rr[1][i].t));
        for (int k = 0; k < 3; k++)
            if (fabsf(push_out[0][i][k] - push_out[1][i][k]) > 1e-4f) {
                pd++;
                break;
            }
    }
    for (int i = 0; i < W; i++)
        for (int k = 0; k < 3; k++)
            if (fabsf(walk_out[0][i][k] - walk_out[1][i][k]) > 1e-3f) {
                wd++;
                break;
            }
    printf("colcheck %s vs %s: floors %d hit, %d differ; rays %d hit, %d differ; pushes on floors %d differ; walks %d of %d "
           "end elsewhere\n", argv[1], argv[2], fh, fd, rh, rd, pd, wd, W);
    return fd || rd > N / 10000;
}
