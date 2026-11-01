/* EzState runtime: state groups, conditions and the expression bytecode of a packed ESD (tools/export_esd.py).
 * Semantics and the hook order are described in esd.h. */
#include <string.h>
#include "esd.h"

typedef struct {
    char magic[4];
    unsigned groups, states, conds, cmds, args, idx, code_len;
    unsigned groups_off, states_off, conds_off, cmds_off, args_off, idx_off, code_off;
} Header;
typedef struct EsdGroup { int id; unsigned short first, count; } Group;
typedef struct EsdState { int id; unsigned short entry, n_entry, exit, n_exit, whl, n_while, conds, n_conds; } State;
typedef struct EsdCond { short target; unsigned short pass, n_pass, subs, n_subs, code_len; unsigned code; } Cond;
typedef struct EsdCmd { short bank, id; unsigned short args, n_args; } Cmd;
typedef struct EsdArg { unsigned code; unsigned short len, pad; } Arg;

__attribute__((weak)) int esd_env_move(EsdCtx *c, int id, const float *a, int n, float *out)
{
    (void)c, (void)id, (void)a, (void)n, (void)out;
    return 0;
}
__attribute__((weak)) int esd_cmd_move(EsdCtx *c, int bank, int id, const float *a, int n)
{
    (void)c, (void)bank, (void)id, (void)a, (void)n;
    return 0;
}

int esd_load(EsdProgram *g, const void *blob)
{
    const unsigned char *b = blob;
    memset(g, 0, sizeof *g);
    if (!b || memcmp(b, "ESD1", 4))
        return 0;
    const Header *h = (const Header *)b;
    if (h->groups > ESD_MAX_GROUPS)
        return 0;
    g->blob = blob;
    g->ngroups = h->groups;
    g->groups = (const Group *)(b + h->groups_off);
    g->states = (const State *)(b + h->states_off);
    g->conds = (const Cond *)(b + h->conds_off);
    g->cmds = (const Cmd *)(b + h->cmds_off);
    g->args = (const Arg *)(b + h->args_off);
    g->idx = (const unsigned short *)(b + h->idx_off);
    g->code = b + h->code_off;
    return 1;
}

void esd_ctx_init(EsdCtx *c, const EsdProgram *prog, int kind, void *chr, EsdEnvFn env, EsdCmdFn cmd)
{
    memset(c, 0, sizeof *c);
    c->prog = prog && prog->blob ? prog : 0;
    c->kind = kind;
    c->chr = chr;
    c->p = kind == ESD_PLAYER ? chr : 0;
    c->env = env;
    c->cmd = cmd;
}

#ifdef ESD_TRACE  /* native test harness only: called for every env function / command */
void esd_trace(EsdCtx *c, int cmd, int bank, int id, const float *a, int n);
#endif

static float call_env(EsdCtx *c, int id, const float *a, int n)
{
#ifdef ESD_TRACE
    esd_trace(c, 0, 0, id, a, n);
#endif
    float v = 0;
    if (esd_env_move(c, id, a, n, &v) || (c->env && c->env(c, id, a, n, &v)))
        return v;
    return 0;  /* not implemented: safe default (TECH_DEBT esd-1) */
}

/* Evaluate one expression. Returns the value on top of the stack; *aborted = 1 when an AbortIfFalse (0xB7) fired. */
static float eval(EsdCtx *c, const unsigned char *p, int len, int *aborted)
{
    float st[32];
    int sp = 0;
    const unsigned char *end = p + len;
    *aborted = 0;
    while (p < end && sp < 30) {
        unsigned o = *p++;
        float f;
        int i;
        if (o <= 0x7F) {
            st[sp++] = (float)((int)o - 64);
            continue;
        }
        switch (o) {
        case 0x80: memcpy(&f, p, 4), p += 4, st[sp++] = f; break;
        case 0x81: { double d; memcpy(&d, p, 8), p += 8, st[sp++] = (float)d; break; }
        case 0x82: memcpy(&i, p, 4), p += 4, st[sp++] = (float)i; break;
        case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: {
            int n = o - 0x84;
            if (sp < n + 1)
                return 0;
            sp -= n;
            float r = call_env(c, (int)st[sp - 1], st + sp, n);
            st[sp - 1] = r;
            break;
        }
        case 0x8D: if (sp) st[sp - 1] = -st[sp - 1]; break;
        case 0x9A: if (sp) st[sp - 1] = st[sp - 1] == 0; break;
        case 0xA1: break;  /* end */
        case 0xA5:  /* UTF-16 string literal (only debug prints): skip, push 0 */
            while (p + 1 < end && (p[0] || p[1]))
                p += 2;
            p += 2;
            st[sp++] = 0;
            break;
        case 0xA6: break;  /* ContinueIfFalse: no effect on the result */
        case 0xB7:         /* AbortIfFalse: the whole expression is false */
            if (sp && st[sp - 1] == 0) {
                *aborted = 1;
                return 0;
            }
            break;
        case 0xB8: case 0xB9: case 0xBA: st[sp++] = 0; break;  /* Arg / CallResult / CallOngoing: unused by c0000 */
        default:
            if (o >= 0xA7 && o <= 0xAE) {
                if (sp) c->reg[o - 0xA7] = st[sp - 1];
            } else if (o >= 0xAF && o <= 0xB6) {
                st[sp++] = c->reg[o - 0xAF];
            } else if (o >= 0x8C && o <= 0x99 && sp >= 2) {
                float r = st[--sp], l = st[sp - 1], v = 0;
                switch (o) {
                case 0x8C: v = l + r; break;
                case 0x8E: v = l - r; break;
                case 0x8F: v = l * r; break;
                case 0x90: v = r != 0 ? l / r : 0; break;
                case 0x91: v = l <= r; break;
                case 0x92: v = l >= r; break;
                case 0x93: v = l < r; break;
                case 0x94: v = l > r; break;
                case 0x95: v = l == r; break;
                case 0x96: v = l != r; break;
                case 0x98: v = l != 0 && r != 0; break;
                case 0x99: v = l != 0 || r != 0; break;
                }
                st[sp - 1] = v;
            }
        }
    }
    return sp ? st[sp - 1] : 0;
}

static void run_cmds(EsdCtx *c, int first, int n)
{
    const EsdProgram *g = c->prog;
    const Cmd *cmds = g->cmds;
    const Arg *args = g->args;
    const unsigned char *code = g->code;
    for (int k = 0; k < n; k++) {
        const Cmd *m = &cmds[first + k];
        float a[ESD_MAX_ARGS];
        int na = m->n_args < ESD_MAX_ARGS ? m->n_args : ESD_MAX_ARGS, ab;
        for (int j = 0; j < na; j++)
            a[j] = eval(c, code + args[m->args + j].code, args[m->args + j].len, &ab);
#ifdef ESD_TRACE
        esd_trace(c, 1, m->bank, m->id, a, na);
#endif
        if (!esd_cmd_move(c, m->bank, m->id, a, na) && c->cmd)
            c->cmd(c, m->bank, m->id, a, na);
    }
}

/* Conditions idx[first .. first+n): the first one that holds (FUN_140ea0d10): a true condition with a target, or a
 * true targetless one without sub-conditions, is the result; a true targetless one with sub-conditions returns the
 * first matching sub-condition, else the search goes on. Returns the condition index, -1 if none. */
static int check(EsdCtx *c, int first, int n)
{
    const EsdProgram *g = c->prog;
    const Cond *conds = g->conds;
    const unsigned short *idx = g->idx;
    const unsigned char *code = g->code;
    for (int k = 0; k < n; k++) {
        const Cond *d = &conds[idx[first + k]];
        int ab;
        if (eval(c, code + d->code, d->code_len, &ab) == 0)
            continue;
        if (d->target >= 0 || d->n_subs == 0)
            return idx[first + k];
        int t = check(c, d->subs, d->n_subs);
        if (t >= 0)
            return t;
    }
    return -1;
}

void esd_start(EsdCtx *c)
{
    const EsdProgram *g = c->prog;
    if (!g)
        return;
    memset(c->reg, 0, sizeof c->reg);
    for (int k = 0; k < g->ngroups; k++) {
        c->group = k;
        c->cur[k] = g->groups[k].first;
        run_cmds(c, g->states[c->cur[k]].entry, g->states[c->cur[k]].n_entry);
    }
}

/* One machine step per group per frame, as FUN_140388020 calls the EzStateMachine step (FUN_140e5d000) with a step
 * count of 1: evaluate the conditions once (FUN_140ea0ff0); on a match run the state's exit commands, the
 * condition's pass commands, then switch (a targetless condition re-enters the same state) and run the entry
 * commands; finally the current state's while commands. The new state is first evaluated next frame. */
void esd_update(EsdCtx *c)
{
    const EsdProgram *g = c->prog;
    if (!g)
        return;
    for (int k = 0; k < g->ngroups; k++) {
        c->group = k;
        const State *s = &g->states[c->cur[k]];
        int ci = check(c, s->conds, s->n_conds);
        if (ci >= 0) {
            const Cond *d = &g->conds[ci];
            run_cmds(c, s->exit, s->n_exit);
            run_cmds(c, d->pass, d->n_pass);
            if (d->target >= 0)
                c->cur[k] = d->target;
            run_cmds(c, g->states[c->cur[k]].entry, g->states[c->cur[k]].n_entry);
        }
        s = &g->states[c->cur[k]];
        run_cmds(c, s->whl, s->n_while);
    }
}

int esd_state(const EsdCtx *c, int group)
{
    const EsdProgram *g = c->prog;
    return g && group >= 0 && group < g->ngroups ? g->states[c->cur[group]].id : -1;
}

int esd_groups(const EsdCtx *c) { return c->prog ? c->prog->ngroups : 0; }
