/* The player's status effects (poison, toxic, bleed, curse): buildup meters filled by hits that carry a status
 * SpEffect (combat_status_hook), their resistances (chrstat.c resist), and the effect once a meter fills. */
#ifndef STATUS_H
#define STATUS_H

enum { STATUS_POISON, STATUS_TOXIC, STATUS_BLEED, STATUS_CURSE, STATUS_COUNT };

typedef struct {
    float meter[STATUS_COUNT];   /* buildup so far; the effect starts when it reaches the resistance */
    int active[STATUS_COUNT];    /* poison / toxic running (curse: cursed) */
    float left[STATUS_COUNT];    /* s left of a running poison / toxic */
    float tick_t[STATUS_COUNT];  /* s to its next tick */
    float hp[STATUS_COUNT], tick[STATUS_COUNT];  /* hp per tick and tick length of the running effect */
    int procs[STATUS_COUNT];     /* times each one filled (tests / HUD) */
} StatusState;

extern StatusState status_pl;

struct Player;
void status_init(void);                       /* sets combat_status_hook */
void status_update(struct Player *p, float dt);  /* decay, ticks */
void status_clear(void);                      /* rest at a bonfire / respawn / cure items */
/* Test scripts: add buildup of type (0-3) as a hit would (power, effect hp / tick / time / rate) */
void status_test_add(int type, int power, float hp, float tick, float time, float rate);

#endif
