/* Runtime switches: fallback switches a test script can flip at boot ("option set NAME VALUE", src/testscript.c), so
 * one ELF / ISO serves both sides of an A/B (tools/test-run.py --set NAME=VALUE). A switch on this list reads its
 * variable cv_NAME instead of a constant; make DEFS=-DNAME=value still pins it at compile time (the variable then
 * goes unused), which is what tools/ci/switches.sh builds. Only switches tested by plain C `if`s can be here: one
 * that a #if reads, or that sizes an array, stays compile-time. Values hold for the whole run: set them before the
 * script's first spawn (warm starts carry the option lines in their park script, tools/warmstate.py).
 *
 * The list: name, default (the value a plain build runs with; the switch's row in switches.h says what it does). */
#ifndef CVAR_H
#define CVAR_H

#define CVAR_LIST(X)          \
    X(CHR_OMIT, 1)            \
    X(SEP_CARRY, 1)           \
    X(ENEMY_WALLS, 1)         \
    X(PACK_JOB_ABANDON, 1)    \
    X(MIP_TRILINEAR, 1)       \
    X(TC_PRESS, 1)            \
    X(LUA_POOL_GROW, 1)       \
    X(PUSH2_CELLS, 1)

#define CVAR_EXTERN(name, def) extern int cv_##name;
CVAR_LIST(CVAR_EXTERN)
#undef CVAR_EXTERN

/* 1 when NAME is a runtime switch (then set to value), 0 otherwise (logged by the caller) */
int cvar_set(const char *name, int value);
/* "cvar: NAME = v (default d)" for every switch that differs from its default; none: one "cvar: defaults" line */
void cvar_log(void);

#endif
