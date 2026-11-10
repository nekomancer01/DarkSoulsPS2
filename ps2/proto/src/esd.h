/* EzState (ESD) runtime: runs a chr's .esd (packed by tools/export_esd.py) for any number of characters.
 *
 * One EsdProgram per loaded .esd (shared), one EsdCtx per character instance (its own state per group, registers and
 * hooks). Semantics follow the exe (notes/PORTING.md "ESD runtime"): each group steps once per frame; first true
 * condition wins (a targetless one descends into its sub-conditions, or re-enters the state when it has none);
 * transition = exit, pass, entry commands; while commands every frame. Expressions are the ESD stack bytecode on
 * floats (ints exact up to 2^24); `~abort` (0xB7, FUN_140e9c6c0) ends the expression with 0 when its value is false,
 * `~continue` (0xA6) is a bare RET in the opcode table at 0x141A8C620.
 *
 * Dispatch of f<id>(...) / c<bank>_<id>(...): esd_env_move / esd_cmd_move (movement + combat side, every kind of
 * character; check c->kind / c->p), then the context's own env / cmd hooks (player.c for the player, the enemy module
 * for enemies). Unhandled env functions return 0, unhandled commands do nothing. Ids: data/esd_names.json. */
#ifndef ESD_H
#define ESD_H

struct Player;
struct Input;

#define ESD_MAX_ARGS 8
#define ESD_MAX_GROUPS 8

enum { ESD_PLAYER, ESD_ENEMY };

/* A loaded program: views into the packed blob (which the caller keeps allocated) */
typedef struct EsdProgram {
    const void *blob;
    int ngroups;
    const struct EsdGroup *groups;
    const struct EsdState *states;
    const struct EsdCond *conds;
    const struct EsdCmd *cmds;
    const struct EsdArg *args;
    const unsigned short *idx;
    const unsigned char *code;
} EsdProgram;

struct EsdCtx;
typedef int (*EsdEnvFn)(struct EsdCtx *c, int id, const float *args, int nargs, float *out);
typedef int (*EsdCmdFn)(struct EsdCtx *c, int bank, int id, const float *args, int nargs);

typedef struct EsdCtx {
    const EsdProgram *prog;
    int kind;                  /* ESD_PLAYER / ESD_ENEMY */
    struct Player *p;          /* the player when kind == ESD_PLAYER, else NULL */
    void *chr;                 /* the owning character (Player * or the enemy's own struct) */
    EsdEnvFn env;              /* character-side hooks (may be NULL) */
    EsdCmdFn cmd;
    void *user;                /* free for the character side */
    const struct Input *in;    /* this frame's pad input (player only, else NULL) */
    float dt;                  /* this frame's step, set by the caller before esd_update */
    float reg[8];              /* REG0..REG7 (shared by the machine's groups) */
    int cur[ESD_MAX_GROUPS];   /* current state index per group */
    int group;                 /* group whose commands / conditions run now (for the hooks) */
} EsdCtx;

/* Load a packed .esd; returns 0 on a bad blob (the blob must stay allocated while contexts use it) */
int esd_load(EsdProgram *prog, const void *blob);
/* Set up a character's context (registers cleared); call esd_start before the first esd_update */
void esd_ctx_init(EsdCtx *c, const EsdProgram *prog, int kind, void *chr, EsdEnvFn env, EsdCmdFn cmd);
/* Every group to its first state, entry commands run */
void esd_start(EsdCtx *c);
/* One frame: evaluate every group (set c->dt / c->in first) */
void esd_update(EsdCtx *c);
/* Current state id of a group (HUD / debugging), -1 if there is no such group */
int esd_state(const EsdCtx *c, int group);

/* Movement / combat side (esd_env_move.c): called first for every context and id; return 1 when handled */
int esd_env_move(struct EsdCtx *c, int id, const float *args, int nargs, float *out);
int esd_cmd_move(struct EsdCtx *c, int bank, int id, const float *args, int nargs);

#endif
