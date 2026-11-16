/* Runtime switches (see cvar.h) */
#include <stdio.h>
#include <string.h>
#include "cvar.h"

#define CVAR_DEFINE(name, def) int cv_##name = def;
CVAR_LIST(CVAR_DEFINE)
#undef CVAR_DEFINE

typedef struct {
    const char *name;
    int *v;
    int def;
} Cvar;

#define CVAR_ROW(name, def) {#name, &cv_##name, def},
static const Cvar table[] = {CVAR_LIST(CVAR_ROW)};
#undef CVAR_ROW

int cvar_set(const char *name, int value)
{
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(table[i].name, name)) {
            *table[i].v = value;
            return 1;
        }
    return 0;
}

void cvar_log(void)
{
    int n = 0;
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; i++)
        if (*table[i].v != table[i].def)
            printf("cvar: %s = %d (default %d)\n", table[i].name, *table[i].v, table[i].def), n++;
    if (!n)
        printf("cvar: defaults\n");
}
