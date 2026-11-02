/* Effects: DSR FFX particle systems and effect lights of the area (fx.bin, tools/export_effects.py).
 * Billboard particles on the EE, drawn through gsKit after the opaque passes (additive or alpha blended, Z tested,
 * no Z write); effect lights (bonfires) add to the character light. The map's static share of the same lights is
 * baked into map.bin by export_map.py. */
#ifndef FX_H
#define FX_H

#include <gsKit.h>
#include "render.h"

/* fx.bin from the area pack: textures to VRAM. 0 = no effects (blob missing, bad, or VRAM full). */
int fx_init(GSGLOBAL *gs, const void *blob);
/* simulate: emitters within FX_RANGE of the eye emit (prewarmed on activation), particles move and age */
void fx_update(float dt, V3 eye);
/* queue this frame's particles as gsKit packets (call after the map / character VU1 chains are kicked) */
void fx_draw(void);
/* add the effect lights near pos (bonfire flicker) to a character's ambient light (0x80 = 1.0 units) */
void fx_char_light(V3 pos, float amb[3]);
/* World blocks (src/world.c): at boot, block b's fx.bin tables (kept: the blob can be freed); fx_block_on(b, 1, blob)
 * when the block turns on uploads its textures from blob (its fx.bin again, read with the block), (b, 0, NULL) when
 * it turns off gives their VRAM back (its particles go). Only the blocks on hold VRAM (TECH_DEBT world-4). */
#define FX_SETS 5
int fx_block_load(GSGLOBAL *gs, int block, const void *blob, unsigned size);
void fx_block_on(int block, int on, const void *blob);
/* Event-owned map SFX (MSB SFX with an entity id: fog walls): an instance stops emitting while its entity is on
 * drawgrp.c's hidden list (Delete Map SFX, src/event.c) and starts again (prewarmed) when it comes off (Spawn Map SFX).
 * fx_entity_particles: alive particles of entity id's instances (-1: no loaded instance has it); kill = 1 removes
 * them (Delete Map SFX with "root only" 0). */
int fx_entity_particles(int id, int kill);

typedef struct {
    int particles;   /* alive */
    int active;      /* instances within range */
    int drawn;       /* particles drawn this frame */
    int batches;     /* GS packets */
    float ms;        /* EE time of update + draw (smoothed) */
} FxStats;
extern FxStats fx_stats;

#endif
