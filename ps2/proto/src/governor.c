/* Frame governor: see governor.h */
#include "governor.h"
#include "cvar.h"
#include "frame_budget.h"

#define GOV_RING 8          /* frames the "4 of 8 busy" rule looks back */
#define GOV_RING_BUSY 4     /* of them over FB_GOV_BUSY_MS steps up */
#define GOV_CALM_FRAMES 60  /* frames in a row under FB_GOV_CALM_MS step down (2 s at 30 fps) */
#define GOV_SKIP_MS 100.0f  /* busier frames are settles / loading screens: not fed to the rules */

int gov_level;
static float ring[GOV_RING];
static unsigned ring_n, ring_i, calm;

void gov_update(float busy_ms)
{
    int max = cv_GOV_MAX < 0 ? 0 : cv_GOV_MAX > 3 ? 3 : cv_GOV_MAX;
    if (!cv_GOV) {
        gov_level = 0, ring_n = ring_i = calm = 0;
        return;
    }
    if (cv_GOV_FORCE >= 0) {
        gov_level = cv_GOV_FORCE > 3 ? 3 : cv_GOV_FORCE;
        return;
    }
    if (busy_ms > GOV_SKIP_MS || busy_ms < 0)
        return;
    ring[ring_i] = busy_ms, ring_i = (ring_i + 1) % GOV_RING;
    ring_n += ring_n < GOV_RING;
    unsigned busy = 0;
    for (unsigned i = 0; i < ring_n; i++)
        busy += ring[i] > FB_GOV_BUSY_MS;
    if (busy_ms > FB_GOV_EDGE_MS || busy >= GOV_RING_BUSY) {
        if (gov_level < max)
            gov_level++, ring_n = ring_i = 0;  /* the next step up needs new frames (the ring held the old level's) */
        calm = 0;
    } else if (busy_ms < FB_GOV_CALM_MS) {
        if (++calm >= GOV_CALM_FRAMES && gov_level > 0)
            gov_level--, calm = 0;
    } else {
        calm = 0;
    }
    if (gov_level > max)
        gov_level = max;
}
