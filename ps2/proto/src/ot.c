/* ot.c: the order table (ot.h). */
#include <stdio.h>
#include "ot.h"
#include <dmaKit.h>
#include "render.h"
#include "vu1.h"

#define OT_ENTRIES 16

/* the VU1 chain buffer each bucket's segments are built in (vu1_begin's buffer numbers) */
static const signed char bucket_buf[OT_N] = {VU1L_BUF_MAP, VU1L_BUF_MAP, VU1L_BUF_MAP, VU1L_BUF_MAP, VU1L_BUF_SKIN,
                                             VU1L_BUF_SKIN, VU1L_BUF_SKIN, VU1L_BUF_MAP, VU1L_BUF_FX, -1, -1};
static const char *const bucket_name[OT_N] = {"sky", "pano", "horizon", "world", "chr", "obj", "alpha_test", "blend",
                                             "fx", "post", "ui"};

typedef struct {
    unsigned char first, last, closed, sent;
    signed char buf;  /* -1: deferred */
    void (*run)(void *);
    void *arg;
} Entry;
static Entry e[OT_ENTRIES];
static int n, next;     /* entries this frame, the first not yet sent */
static int open_buf = -1;  /* the VU1 buffer being built (vu1_begin'd, not kicked yet) */
static int open_seg = -1;  /* the entry of the open segment */
static unsigned out_of_order, inherits[OT_N], overflow, frames;

void ot_frame_begin(void)
{
    if (next < n)
        printf("TEST error: ot: %d entries of the last frame never sent\n", n - next);
    n = next = 0;
    frames++;
}

static Entry *add(OtBucket first, OtBucket last)
{
    if (n == OT_ENTRIES) {
        if (!overflow++)
            printf("TEST error: ot: more than %d entries in a frame\n", OT_ENTRIES);
        return NULL;
    }
    if (n && first < e[n - 1].first)
        out_of_order++;  /* today's order and the bucket order disagree here (ot.h) */
    Entry *x = &e[n++];
    x->first = first, x->last = last, x->closed = x->sent = 0, x->run = 0, x->arg = 0;
    return x;
}

void ot_open(OtBucket first, OtBucket last)
{
    int buf = bucket_buf[first];
    if (open_seg >= 0)
        printf("TEST error: ot: %s opened while %s is open\n", bucket_name[first], bucket_name[e[open_seg].first]);
    Entry *x = add(first, last);
    if (!x)
        return;
    x->buf = (signed char)buf;
    if (open_buf == buf)
        inherits[first]++;  /* no preamble of its own: carries the state of the segment before it (ot.h rule 2) */
    else {
        if (open_buf >= 0)
            printf("TEST error: ot: buffer %d opened while buffer %d is not sent\n", buf, open_buf);
        vu1_begin(buf);
        open_buf = buf;
    }
    open_seg = n - 1;
}

void ot_close(void)
{
    if (open_seg < 0) {
        printf("TEST error: ot: close without an open segment\n");
        return;
    }
    e[open_seg].closed = 1;
    open_seg = -1;
}

void ot_deferred(OtBucket first, OtBucket last, void (*run)(void *), void *arg)
{
    Entry *x = add(first, last);
    if (!x)
        return;
    x->buf = -1, x->run = run, x->arg = arg, x->closed = 1;
}

static void run_queue(void *gs) { r_queue_exec(gs); }
static void run_queue_wait(void *gs)
{
    r_queue_exec(gs);
    dmaKit_wait_fast();
}
void ot_queue(OtBucket first, OtBucket last, void *gs, int wait_after)
{
    ot_deferred(first, last, wait_after ? run_queue_wait : run_queue, gs);
}

void ot_flush(void)
{
    while (next < n && e[next].closed) {
        Entry *x = &e[next];
        if (x->buf >= 0) {
            /* the run: this segment and the closed ones after it in the same buffer, contiguous in it: one kick */
            int k = next;
            while (k + 1 < n && e[k + 1].closed && e[k + 1].buf == x->buf)
                k++;
            if (k + 1 < n && e[k + 1].buf == x->buf)
                break;  /* a later segment of this buffer is still open: the run goes with it */
            vu1_kick();
            open_buf = -1;
            for (; next <= k; next++)
                e[next].sent = 1;
        } else {
            vu1_wait();  /* deferred entries draw over what VU1 / PATH1 drew */
            x->run(x->arg);
            x->sent = 1;
            next++;
        }
    }
}

void ot_wait(void) { vu1_wait(); }

#ifdef OT_HASH
int ot_ab_old;
void ot_ab_frame(int frozen) { ot_ab_old = frozen ? !ot_ab_old : 0; }
void ot_old_begin(int buffer) { vu1_begin(buffer); }
void ot_old_kick(void) { vu1_kick(); }
void ot_old_wait(void) { vu1_wait(); }
void ot_old_queue(void *gs) { r_queue_exec(gs); }
#endif

void ot_log(void)
{
    char buf[200];
    int k = 0;
    for (int b = 0; b < OT_N && k < (int)sizeof buf - 24; b++)
        if (inherits[b])
            k += snprintf(buf + k, sizeof buf - k, " %s %u", bucket_name[b], inherits[b]);
    buf[k] = 0;
    printf("ot: %u frames, entries out of bucket order %u, segments inheriting state:%s%s, overflow %u\n", frames,
           out_of_order, k ? "" : " none", buf, overflow);
}
