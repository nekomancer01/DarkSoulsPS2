/* Alignment check build (make ALIGNCHECK=1, TECH_DEBT hw-6): the runtime half of gcc's -fsanitize=alignment.
 * The EE takes an address error exception on a halfword / word / doubleword load or store at a misaligned address,
 * and the game stops on a real PS2. PCSX2 performs such an access without complaint, so the bug only shows on a
 * console. In this build every typed access checks its pointer first and calls the handler below when it is
 * misaligned: each site is logged once and the access then goes on as before (PCSX2 tolerates it).
 * lq / sq (128-bit types) ignore the low four address bits on the EE instead of faulting; those are logged too, as
 * they read or write the wrong 16 bytes. This file itself is built without the sanitizer. */
#include <stdio.h>

struct src_loc {
    const char *file;
    unsigned line, column;
};
struct type_desc {
    unsigned short kind, info;
    char name[1];
};
struct mismatch_v1 {
    struct src_loc loc;
    const struct type_desc *type;
    unsigned char log_alignment, check_kind;
};

#define SITES 256
static const void *seen[SITES];
static unsigned nseen, total;

static void report(struct mismatch_v1 *d, unsigned long ptr)
{
    static const char *const kinds[5] = {"load", "store", "ref", "member", "call"};
    unsigned align = 1u << d->log_alignment;
    /* a member access / call / reference checks the whole struct's alignment; a struct declared aligned(64) (gsKit's
     * GSGLOBAL, packet2_t) but malloc'd at 16 trips it although its fields are words. Only what the EE itself needs
     * counts there: 8 bytes at most. Plain loads / stores keep the type's alignment (u128: lq / sq). */
    if (d->check_kind >= 2 && align > 8)
        align = 8;
    if (!ptr || !(ptr & (align - 1)))
        return;  /* -fsanitize=alignment only: a null pointer is not this check's business */
    total++;
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == d)
            return;
    if (nseen < SITES)
        seen[nseen++] = d;
    printf("ALIGN %s:%u:%u %s %s at 0x%08lx (needs %u)%s\n", d->loc.file, d->loc.line, d->loc.column,
           d->check_kind < 5 ? kinds[d->check_kind] : "use", d->type ? d->type->name : "?", ptr, align,
           align >= 16 ? " [16-aligned type: lq / sq would hit the wrong qword]" : "");
}

void __ubsan_handle_type_mismatch_v1(struct mismatch_v1 *d, unsigned long ptr) { report(d, ptr); }
void __ubsan_handle_type_mismatch_v1_abort(struct mismatch_v1 *d, unsigned long ptr) { report(d, ptr); }

/* total misaligned accesses so far and distinct sites (the test harness may print it at exit) */
unsigned aligncheck_count(unsigned *sites)
{
    if (sites)
        *sites = nseen;
    return total;
}
