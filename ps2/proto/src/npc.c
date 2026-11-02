/* c0000 NPC stand-ins (npc.h) */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "combat.h"
#include "ev_chr.h"
#include "model.h"
#include "npc.h"
#include "render.h"
#include "vu1.h"

#define NPC_HP 500          /* NpcParam of c0000 NPCs is not exported: ASSUMED placeholder HP */
#define NPC_RADIUS 0.4f
#define NPC_HEIGHT 1.8f
#define NPC_HIT_DIST 10.0f  /* combat slots only near the player */
#define NPC_DRAW_DIST 30.0f
#define NPC_DRAW_MAX 4      /* nearest drawn (~0.7 ms of EE each, as enemies) */

typedef struct {
    int block, entity, on;  /* on: its block is on */
    unsigned char off;      /* event bits as enemy.c ev_off: 1 disabled, 2 backread removed */
    V3 pos;
    float yaw;
    CombatChr cc;
    int registered, hits_seen;
    struct EvChr *ev;
} Npc;

static Npc npcs[NPC_MAX];
int npc_count, npc_drawn;
static float idle_t;

static void place(Npc *n)
{
    n->on = 1;
    n->off = 0, n->registered = 0;
    memset(&n->cc, 0, sizeof n->cc);
    n->cc.radius = NPC_RADIUS, n->cc.height = NPC_HEIGHT, n->cc.team = TEAM_ENEMY;
    n->cc.hp = n->cc.hp_max = NPC_HP;
    n->hits_seen = 0;
    n->ev = ev_chr_get(n->entity, 0);  /* what events did to it while its block was off (event-12) */
    if (n->ev) {
        n->off = n->ev->off;
        if (n->ev->dead)
            n->cc.dead = 1, n->cc.hp = -1;
    }
}

void npc_keep(int block, const unsigned char *r)
{
    if (npc_count == NPC_MAX) {
        printf("npc: more than %d c0000 parts, the rest are not placed\n", NPC_MAX);
        return;
    }
    Npc *n = &npcs[npc_count++];
    memset(n, 0, sizeof *n);
    float f[4];
    n->block = block;
    memcpy(&n->entity, r + 16, 4);
    memcpy(f, r + 20, 16);
    n->pos = v3(f[0], f[1], f[2]), n->yaw = f[3];
    if (block == 0)
        place(n);
}

void npc_block(int block, int on)
{
    int k = 0;
    for (int i = 0; i < npc_count; i++) {
        Npc *n = &npcs[i];
        if (n->block != block || n->on == on)
            continue;
        if (on)
            place(n);
        else {
            if (n->registered)
                combat_unregister(&n->cc), n->registered = 0;
            n->on = 0, n->ev = NULL;
        }
        k++;
    }
    if (k)
        printf("npc: block %d %s: %d c0000 stand-ins\n", block, on ? "on" : "off", k);
}

static int shown(const Npc *n) { return n->on && !n->off && !n->cc.dead; }

void npc_update(const Player *pl, float dt)
{
    idle_t += dt;
    for (int i = 0; i < npc_count; i++) {
        Npc *n = &npcs[i];
        if (!n->on)
            continue;
        if (n->ev) {  /* immortal / invincible: as enemy.c */
            if (n->ev->immortal && n->cc.hp < 1)
                n->cc.hp = 1, n->cc.dead = 0;
            n->cc.invincible = n->ev->invincible;  /* no AI: home / AI ops are kept for the real NPC (enemy-14) */
        }
        float dx = n->pos.x - pl->pos.x, dz = n->pos.z - pl->pos.z;
        int want = shown(n) && dx * dx + dz * dz < NPC_HIT_DIST * NPC_HIT_DIST;
        if (want && !n->registered)
            n->registered = combat_register(&n->cc) >= 0;
        else if (!want && n->registered)
            combat_unregister(&n->cc), n->registered = 0;
        memcpy(n->cc.pos, &n->pos, 12);
        n->cc.yaw = n->yaw;
        if (n->cc.hit_count != n->hits_seen) {
            n->hits_seen = n->cc.hit_count;
            ev_chr_hit(n->entity);
        }
    }
}

void npc_draw(void)
{
    V3 eye, right, up, fwd;
    float f, fy, zs;
    r_camera_basis(&eye, &right, &up, &fwd, &f, &fy, &zs);
    int pick[NPC_DRAW_MAX], np = 0;
    float pd[NPC_DRAW_MAX];
    for (int i = 0; i < npc_count; i++) {
        const Npc *n = &npcs[i];
        float dx = n->pos.x - eye.x, dz = n->pos.z - eye.z, d = dx * dx + dz * dz;
        const float mn[3] = {n->pos.x - 1, n->pos.y - 0.5f, n->pos.z - 1},
                    mx[3] = {n->pos.x + 1, n->pos.y + NPC_HEIGHT + 0.5f, n->pos.z + 1};
        if (!shown(n) || d > NPC_DRAW_DIST * NPC_DRAW_DIST || !r_box_classify(mn, mx))
            continue;
        int k;  /* insertion into the nearest NPC_DRAW_MAX */
        if (np < NPC_DRAW_MAX)
            k = np++;
        else if (d >= pd[NPC_DRAW_MAX - 1])
            continue;
        else
            k = NPC_DRAW_MAX - 1;
        for (; k > 0 && pd[k - 1] > d; k--)
            pd[k] = pd[k - 1], pick[k] = pick[k - 1];
        pd[k] = d, pick[k] = i;
    }
    npc_drawn = 0;
    if (!np)
        return;
    static BoneLocal pose[MAX_BONES];  /* one idle pose for all of them */
    int idle = model_anim(0);
    if (idle < 0)
        return;
    model_sample(idle, idle_t, 1, pose);
    vu1_skin_fade(255);
    for (int k = 0; k < np; k++) {
        model_draw(pose, npcs[pick[k]].pos, npcs[pick[k]].yaw, 0);
        npc_drawn++;
    }
}

static Npc *find(int entity, int from)
{
    for (int i = from; i < npc_count; i++)
        if (npcs[i].on && npcs[i].entity == entity && entity > 0)
            return &npcs[i];
    return NULL;
}

int npc_entity(int entity, V3 *pos, int *dead, float *hp_ratio, int *active)
{
    Npc *n = find(entity, 0);
    if (!n)
        return -1;
    if (pos) *pos = n->pos;
    if (dead) *dead = n->cc.dead;
    if (hp_ratio) *hp_ratio = (float)n->cc.hp / n->cc.hp_max;
    if (active) *active = shown(n);
    return (int)(n - npcs);
}

int npc_entity_enable(int entity, int on, int bit)
{
    int k = 0;
    for (Npc *n = find(entity, 0); n; n = find(entity, (int)(n - npcs) + 1), k++) {
        int was = n->off != 0;
        n->off = on ? n->off & ~bit : n->off | bit;
        if (was != (n->off != 0))
            printf("npc: %d (c0000 stand-in) %s by an event\n", entity, n->off ? "off" : "on");
    }
    return k;
}

int npc_entity_kill(int entity)
{
    Npc *n = find(entity, 0);
    if (!n || n->cc.dead)
        return 0;
    n->cc.hp = -1, n->cc.dead = 1;
    return 1;
}

int npc_entity_damage(int entity, int hp)
{
    Npc *n = find(entity, 0);
    if (!n)
        return 0;
    if (n->ev && n->ev->invincible)
        return 1;
    n->cc.hp -= hp;
    n->cc.hit_count++;
    if (n->cc.hp <= 0)
        n->cc.dead = 1;
    if (n->ev && n->ev->immortal && n->cc.hp < 1)
        n->cc.hp = 1, n->cc.dead = 0;
    return 1;
}

void npc_entity_bind(int entity, struct EvChr *c)
{
    for (Npc *n = find(entity, 0); n; n = find(entity, (int)(n - npcs) + 1))
        n->ev = c;
}
