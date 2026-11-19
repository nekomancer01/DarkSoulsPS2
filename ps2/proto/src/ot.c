/* ot.c: the order table (ot.h). */
#include <stdio.h>
#include "ot.h"
#include <dmaKit.h>
#include "render.h"
#include "vu1.h"
#include "g2d.h"
#ifdef OT_HASH
#include "gstrace.h"
#endif

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

static void run_g2d(void *stream)
{
    void *chain = g2d_close((G2dStream)(int)stream);
    dmaKit_wait_fast();  /* the GIF channel idle (texture uploads, the last 2D segment) before this one starts */
    if (chain) {
#ifdef OT_HASH
        const u64 *t = chain;
        gstrace_chain(chain, t + 2 + 2 * (t[0] & 0xFFFF));
#endif
        dmaKit_send_chain(DMA_CHANNEL_GIF, chain, 0);
    }
}
void ot_g2d(OtBucket first, OtBucket last, int stream) { ot_deferred(first, last, run_g2d, (void *)stream); }

/* gsKit's own queue after Polaris 1.6: nothing should write to it any more (the 2D goes through g2d); if something
 * still does, run it here (its 16 KB have no bounds check) and say so once */
void ot_gskit_leftover(void *gs_)
{
    GSGLOBAL *gs = gs_;
    GSQUEUE *q = gs->Os_Queue;
    if ((char *)q->pool_cur - (char *)q->pool[q->dbuf] > 16) {  /* (16: the DMA tag gsKit keeps at the start) */
        static int said;
        if (!said++)
            printf("TEST warn: ot: gsKit's draw queue was used (%u bytes) after Polaris 1.6\n",
                   (unsigned)((char *)q->pool_cur - (char *)q->pool[q->dbuf]));
        r_queue_exec(gs);
    }
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
void ot_hash_begin(int frozen)
{
    static int run;
    run = frozen ? run + 1 : 0;
    gstrace_begin(run == 2 || run == 3 ? 60 : 0);  /* a shot's second and third frozen frames: their 2D draws in the log */
}
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
