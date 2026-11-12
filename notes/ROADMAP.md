# DSR -> PS2 port: what still needs implementing, by priority

**What this is:** the original priority list for the port (P0 = foundations, P3 = polish), with a starting point for
each item. It was written on 2026-10-30 and most of it is done since: the status table below (checked against the code
on 2026-11-12) says where each item stands and which TECH_DEBT rows hold what is left. The tables after it are the
original plan, kept as written. Terms: EE = the PS2's main CPU, VU1 = its vector unit (draws the 3D), GS = its graphics chip.

Status (2026-10-30): the Warrior (real mesh, textures, Havok animations, TAE timing, R1 combo, roll/dash) walks
Firelink Shrine (map pieces, textures, navmesh, back-face culling) in PCSX2 at 30 fps with the VU1 renderer. Tooling reads DCX, BND3/BXF3, FMG, PARAMDEF/PARAM,
TAE, ESD, FLVER2, TPF/DDS (BC1-3, BC7), MSB1, NVM, Havok 2015 tagfiles (skeleton, spline animation, root motion).

Priority: **P0** = blocks most other work or makes the port unplayable; **P1** = core Dark Souls gameplay;
**P2** = content and features after the core works; **P3** = polish / optional.

## Status (2026-11-12)

Done = the system exists in the port and its remaining differences from DSR are TECH_DEBT rows. Partial = part of the
item is still missing (named in the Left column).

| # | Item | Status | Left / where |
|---|---|---|---|
| 1 | VU1 renderer | Done | VU1 skinning, clipping and indexed map batches are in (vu1skin2 / vu1clip / vu1mapi). The frame target is 30 fps (tools/budget.py), not 60: render-5, vu1-1..5, hw-7 |
| 2 | Better mesh simplification | Done | QEM with structure weight, shape envelope and triangle guard (tools/native/simplify.c, simplify.py): map-36..44 |
| 3 | Memory and streaming plan | Done | World blocks (src/world.c) and the map streamer (src/mapstream.c), texture pool (texcache.c). Detail vs memory is still short in the Burg: world-10, world-14 (H) |
| 4 | Real collision | Done | Havok collision decoded, capsule controller, falls, 16 m collision tiles: collision-1..10, world-15 |
| 5 | ESD runtime | Done | c0000.esd interpreted (src/esd.c, player.c, esd_env_move.c): esd-1..9 |
| 6 | Real-hardware test loop | Open | No run on a PS2 yet; tools/ps2-run.sh untested: hw-1 |
| 7 | Exact player numbers | Partial | Several found in the exe (notes/EXE_FINDINGS.md); movement-set tiers not decoded: gameplay-13 |
| 8 | Full movement set | Partial | Rolls, backstep, jump, falls in; no lock-on (esd-7), no player ladders |
| 9 | Animation system | Partial | Fixed 0.15 s crossfade, layer args ignored: esd-6, esd-9 |
| 10 | Combat | Partial | Physical damage only, no player poise / defence, no parry / backstab: combat-2..6 |
| 11 | Character stats & equipment | Partial | Stats saved and shown in menus, but combat still uses the compile-time Warrior numbers: save-8 |
| 12 | Enemies | Done | Lua AI on lua0 (ai.c), enemy ESD, navmesh moves: enemy-2..21 |
| 13 | Event scripts (EMEVD) | Done | src/event.c with character / object / misc ops; remaining stubs: event-8 (H) |
| 14 | Bonfire / Estus / death loop | Done | src/bonfire.c (rest, kindle, respawn, bloodstain): a4-2..7 |
| 15 | HUD & menus | Done | DSR's HUD and Start menu screens from menu.drb: hud-2..7, menu-1..8; no title screen (menu-5) |
| 16 | Audio | Done | SPU2 banks + disc streams through dsrsnd.irx: audio-2 (H, 8 kHz resident rate), audio-3..13 |
| 17 | Save system | Done | Memory card saves (src/save.c): save-1..12 |
| 18 | More areas + area streaming | Partial | Firelink, Undead Burg (three blocks), Undead Asylum stream as one world; rest of Lower Burg, the Parish church and the Depths are not blocks yet: world-1 |
| 19 | Map objects | Partial | Breakables (physics-3), doors and levers (ObjAct, event-11) in; elevators stay static, ladders not usable |
| 20 | Lighting & atmosphere | Done | Baked DSR light, fog, scattering, sky (tools/dsr_light.py): map-15..17, map-22, map-26..28; map specular / AO still missing: map-23 (H) |
| 21 | Effects | Partial | FFX billboards on VU1; models, tracers, distortion skipped: fx-7 |
| 22 | NPCs & dialogue | Partial | Human NPCs are stand-ins with the player's model; only the bonfire's talk ESD runs: enemy-14 |
| 23 | Cutscenes & movies | Open | Cutscenes are skipped and their warps run: event-13 |
| 24 | Disc build | Done | ISO from tools/ps2-iso.sh / tools/iso9660.py, packs read from cdrom0: by the disc server: world-6, world-8 |
| 25 | Perspective-correct texturing | Done | Every VU1 program draws with STQ (src/vu1.c PRIM setup) |
| 26 | Wall sliding on the navmesh | Done | Replaced by real collision (item 4): the capsule slides along walls |
| 26b | Reverse Z precision | Partial | Still a 16-bit Z buffer; two depth ranges split at 50 m since 2026-11-11: render-1, render-15 |
| 27 | Texture quality | Partial | Two mip levels, PSMT4 where it holds colour, map textures capped at 128 px: map-2, texstream-1..5 |
| 28 | Character skin texture | Done | BD_M_body used: char-10 |
| 29 | Multiplayer | Dropped | |

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
