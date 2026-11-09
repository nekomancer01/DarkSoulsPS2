/* Logic pass breakdown: lpass.h */
#include <stdio.h>
#include <string.h>
#include "lpass.h"

#define LPASS_LOG 120  /* frames per "logic: pass ms" line (4 s at 30 fps) */

unsigned lpass_t[LG_N];

static const char *const lg_name[LG_N] = {"player", "stream", "enemy", "ai", "nav", "esd", "move", "ragdoll", "pose",
                                          "hits", "gc", "objects", "events", "bonfire"};

void lpass_frame(unsigned total)
{
#if LOGIC_PASS_MS
    static unsigned fr, acc[LG_N + 1], wst[LG_N + 1], mx[LG_N + 1], wtot;
    unsigned v[LG_N + 1], sub = 0;
    for (int i = 0; i < LG_N; i++)
        v[i] = lpass_t[i], sub += lpass_t[i];
    v[LG_N] = total > sub ? total - sub : 0;  /* other */
    for (int i = 0; i <= LG_N; i++) {
        acc[i] += v[i];
        if (v[i] > mx[i])
            mx[i] = v[i];
    }
    if (total > wtot)
        memcpy(wst, v, sizeof wst), wtot = total;
    if (++fr == LPASS_LOG) {
        const float k = 1.0f / 294912.0f, ka = k / (float)LPASS_LOG;
        unsigned at = 0;
        for (int i = 0; i <= LG_N; i++)
            at += acc[i];
        char line[640];
        int n = snprintf(line, sizeof line, "logic: pass ms");
        for (int i = 0; i < LG_N && n < (int)sizeof line; i++)
            n += snprintf(line + n, sizeof line - n, " %s %.2f", lg_name[i], (double)(acc[i] * ka));
        if (n < (int)sizeof line)
            n += snprintf(line + n, sizeof line - n, " other %.2f total %.2f | worst", (double)(acc[LG_N] * ka),
                          (double)(at * ka));
        for (int i = 0; i < LG_N && n < (int)sizeof line; i++)
            n += snprintf(line + n, sizeof line - n, " %s %.2f", lg_name[i], (double)(wst[i] * k));
        if (n < (int)sizeof line)
            n += snprintf(line + n, sizeof line - n, " other %.2f total %.2f | max", (double)(wst[LG_N] * k),
                          (double)(wtot * k));
        for (int i = 0; i < LG_N && n < (int)sizeof line; i++)
            n += snprintf(line + n, sizeof line - n, " %s %.2f", lg_name[i], (double)(mx[i] * k));
        if (n < (int)sizeof line)
            snprintf(line + n, sizeof line - n, " other %.2f (%u frames)", (double)(mx[LG_N] * k), fr);
        printf("%s\n", line);
        fr = wtot = 0;
        memset(acc, 0, sizeof acc), memset(mx, 0, sizeof mx), memset(wst, 0, sizeof wst);
    }
#else
    (void)total;
#endif
    memset(lpass_t, 0, sizeof lpass_t);
}
