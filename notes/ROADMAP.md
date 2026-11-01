# DSR -> PS2 port: what still needs implementing, by priority

Status (2026-10-30): the Warrior (real mesh, textures, Havok animations, TAE timing, R1 combo, roll/dash) walks
Firelink Shrine (map pieces, textures, navmesh, back-face culling) in PCSX2 at 30 fps with the VU1 renderer. Tooling reads DCX, BND3/BXF3, FMG, PARAMDEF/PARAM,
TAE, ESD, FLVER2, TPF/DDS (BC1-3, BC7), MSB1, NVM, Havok 2015 tagfiles (skeleton, spline animation, root motion).

Priority: **P0** = blocks most other work or makes the port unplayable; **P1** = core Dark Souls gameplay;
**P2** = content and features after the core works; **P3** = polish / optional.

## P0: foundations

| # | Item | Why now | Notes / starting point |
|---|---|---|---|
| 1 | **VU1 renderer** (first version done: 6 -> 30 fps) | Remaining: 60 fps needs the frame under 16.7 ms | Next: VU1 skinning for characters (EE skinning is 7 ms), a coarse chunk grid so culling doesn't touch all 5.8k chunks (map pass 12 ms), indexed/compressed streams (map.bin is 7.9 MB), VU1 clipping instead of EE fallback, per-vertex lighting on VU1 |
| 2 | **Better mesh simplification** | Vertex clustering shatters architecture at budgets the PS2 can draw | Quadric edge-collapse in the exporter with UV/seam preservation, per-mesh budgets by distance from the playable area; reuse the game's LOD1/2 where they exist |
| 3 | **Memory and streaming plan** | 32 MB RAM, 4 MB VRAM (~370 KB free now), 2 MB IOP RAM for sound | Per-area packs; per-frame texture upload of visible textures instead of everything resident; ELF size limits (a 15 MB ELF already crashes) |
| 4 | **Real collision** | Navmesh only covers walkable floor: no walls, no falling, no camera collision | Decode the map's Havok collision (`h*.hkxbhd/bdt`, hkpCompressedMeshShape / hkp*MeshShape) into a simple triangle soup + grid; capsule vs. triangles; gravity and fall damage |
| 5 | **ESD runtime + named env functions** | c0000.esd *is* DS1's player action logic; today the controller hand-codes a few of its transitions | Name the `f###`/`c1_###` ids by finding the exe's env-function dispatch in the decompile (fndb code search); then interpret the ESD on the PS2 instead of hand-written states |
| 6 | **Real-hardware test loop** | PCSX2 speed ≠ PS2 speed (EE/VU/GS timing, DMA, cache) | ps2link over network or USB, ps2client; profile with the EE performance counters |

## P1: core gameplay

| # | Item | Notes |
|---|---|---|
| 7 | Exact player numbers | Max stamina by END, stamina regen, roll/backstep costs, dash drain, turn speed, equip-load roll tiers (fast/mid/fat = a00 735/700/720): find in exe code |
| 8 | Full movement set | Lock-on (+ strafe anims 300-313), 8-way rolls (700-703 and variants), backstep (760), turn-in-place, jump, ladders, falling/landing |
| 9 | Animation system | Upper/lower body layers (c1_3 layer argument), per-transition blend times from the ESD, TAE events (sounds, effects, speed, turn rate) |
| 10 | Combat | Weapon hitboxes from FLVER dummy polys + AtkParam, damage formula (CalcCorrectGraph scaling, defences), hit reactions, poise/super armour, guard/parry/riposte/backstab |
| 11 | Character stats & equipment | CharaInitParam classes, level-up, equip load, armour defence, ring effects (SpEffectParam) |
| 12 | Enemies | Generic chr loading (c-models, anibnd, TAE, NpcParam), enemy ESDs; AI is Lua (script/*.luabnd) -> needs a Lua 5.0-era VM or offline translation |
| 13 | Event scripts (EMEVD) | Bonfires, doors, fog walls, item pickups, flags; DarkScript3's instruction tables as reference |
| 14 | Bonfire / Estus / death loop | Rest, respawn enemies, bloodstain, souls |
| 15 | HUD & menus | HP/stamina bars, equipment/inventory menus; text from FMG (English msg), font atlas from menu TPFs |
| 16 | Audio | FMOD Ex `.fsb/.fev` -> PS2 SPU2 ADPCM; audsrv/libsd on the IOP; music streaming from disc |
| 17 | Save system | Memory card (libmc), DS1 save data subset |

## P2: content

| # | Item | Notes |
|---|---|---|
| 18 | More areas + area streaming | Undead Asylum (tutorial) first, then Undead Burg; MSB load lists and area transitions |
| 19 | Map objects | Doors, breakables, ladders, elevators, chests (MSB object parts + ObjActParam) |
| 20 | Lighting & atmosphere | Vertex lighting from the map's light banks, GS fog, sky dome (the sky/background strips are wrong today), DOF/tone map approximations |
| 21 | Effects | FFX particles (TAE events 96-120), blood, sparks |
| 22 | NPCs & dialogue | Talk ESDs, shops, covenants; subtitles from FMG |
| 23 | Cutscenes & movies | Remo cutscenes; Bink movies re-encoded to PS2 MPEG (IPU) |
| 24 | Disc build | ISO/CD-DVD layout, IOP module set, boot ELF, asset packing |

## P3: polish / optional

| # | Item | Notes |
|---|---|---|
| 25 | Perspective-correct texturing | gsKit's textured triangles are affine; switch to STQ in the VU1 path |
| 26 | Wall sliding on the navmesh | Today moves that leave it are just refused |
| 26b | Reverse Z precision (user request, later) | Depth is already written as near/z (larger = nearer); with CT16 the Z buffer must be 16-bit. Options: 32-bit colour + Z24/32 when VRAM allows, or a float-like Z encoding; then push the near plane back to 0.1 m |
| 27 | Texture quality | Per-texture size budgets, 4-bit CLUTs where possible, mipmaps (MIPTBP) |
| 28 | Character skin texture | `BD_M_body` texture lives outside the armour archives |
| 29 | Multiplayer | Dropped: no equivalent servers; possibly local co-op later |

## Order of work suggested by dependencies
1 -> 2 -> 3 (rendering budget known) -> 4 (collision) -> 5 (ESD runtime) -> 7-10 (feel of combat) -> 12-14 (first
enemy + bonfire loop) -> 18 (Undead Asylum as the first complete playable area).
