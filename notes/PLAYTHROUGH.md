# Path to a full playthrough (updated 2026-11-01, afternoon)

**What this is:** the checklist of everything the port still needs before Dark Souls plays start to finish, and the
order to build it in. Each system has a status (done / partial / missing) and what's still missing. IDs in brackets
like `[enemy-14]` point to rows in `notes/TECH_DEBT.md`.

The plan: first make every engine system generic (driven by the game's data, with no per-area code) in Phase A and
the exporters safe for every area in Phase B. Then prove it on the game's opening stretch (Phase C). Only then export
and test the whole game in one scripted pass (Phase D).

Terms: EMEVD = the game's event scripts (doors, fog walls, pickups, boss triggers); ESD = character state-machine
scripts; MSB = an area's layout file; a block = one area loaded as a unit; EE / VU1 / GS / IOP = the PS2's main CPU,
vector unit, graphics chip and I/O processor.

## Phase A: engine systems every area needs (build into the game first)

| # | System | Status | Missing for a playthrough |
|---|---|---|---|
| A1 | World streaming between areas | partial: Firelink <-> Undead Burg west walk with no reboot or loading screen; that block's enemies, objects, effects and events load and unload with it. Every block is a full citizen (S2): its own draw groups, relief and near textures, enemies filtered by draw group and view. Far background drawn as backdrop cells. Heap drift checked (no leak over a 30-minute soak); disc packs ordered by world adjacency. S1 block graph (blocks on 32 m cells, switch rule from DSR's ConnectCollision parts, Burg bonfire + Parish courtyard blocks) and S4 heap fragmentation fix (soak OUT OF RAM 40 -> 1). In review: textures per block (enemy textures follow their models, a block's effect / breakable textures and map CLUTs only while it is on: the default world is all 4 blocks with enemies on, >= 156 KB VRAM free). Undead Asylum as a fifth block of the same world: streams in by warp or Firelink's cliff, its crow event carries the player back to Firelink [world-1..7] | A far level for lite neighbours so geometry doesn't pop in at the swap (S3) [world-2]; free EE RAM >= 1.5 MB in the soak [world-5]; the remaining areas; LZ4 block entries [world-6]; fixed RAM / VRAM pools per block [build-5, build-4, area-1] |
| A2 | EMEVD runtime | partial: real event scripts run in Firelink and Burg (chests, item pickups, fog walls, hit-switched objects). Done since: E3 characters by event (AI on / off and commands, team, immortality, special waits, NPC enable / disable / kill), E4 what events switch off disappears (objects, map parts, SFX such as the Burg fog wall), E6 doors / levers / gates / chests animate and block, E2 rules checked in the exe (temporary flag reset, End / SetEventState flags, item-lot draw, regions, EventValue bits) plus save requests, bonfire light / rest, status messages, E5 boss triggers (fog-wall walk 7410, boss bar, defeat: Taurus Demon test), E7 event warps across blocks and maps (crow carry Asylum <-> Firelink, a map's events move between its blocks, warp loads finish the new events). Stub uses down to Firelink 51, Burg 178 (from 127 / 385) [event-1..18] | E2 leftovers [event-2, -3, -4, -6, -9]; boss gauge art / names, other event anims [event-18]; cutscenes [event-13]; the other stubs: sounds, dialogs, bonfire registration, ladders, NPC humanity, ops on the player [event-8, event-15, collision-6] |
| A3 | Save / flags / progression | partial: event flags in the game's own id layout, progression state, saves to the memory card (round trip verified); souls from kills (NpcParam getSoul) and the humanity system: humanity from kills (per-zone hidden counter, GameAreaParam drop points), humanity items by their SpEffect (Humanity +1, Twin Humanities +2), HUD humanity counter (grey while Hollow), curse resist from humanity [save-1..11, hum-1, hum-2] | Background save writes, soul rate rings [hum-2], stats used by combat; full list: event flags, inventory, stats, bonfire state, killed bosses, NPC states; memory card (tools/ps2-mc.sh) [ROADMAP 17] |
| A4 | Bonfire / death loop | partial: the action button at a bonfire lights it (DSR's 7698 anim, BONFIRE LIT banner) or sits down (7700 / 7710 / 7720, stand-up after): HP and Estus to the bonfire's kindle count (5 / 10 / 15 / 20, kindle points saved), enemies respawned, its spawn point the respawn point; the fire follows the kindle level (unlit smoulder, 90001-90004, ignite and rest SFX); rest menu with DSR's entries and flag gates: Kindle (humanity, human form, Rite past 10 Estus) and Reverse Hollowing (HUMANITY RESTORED banner) work, Leave stands up. A death (fall or on the ground) drops a bloodstain with the souls and humanity held, makes the player Hollow, returns them to the respawn point with the bonfire's Estus and resets the world as a rest does; walking onto the stain gives them back; the stain is saved [a4-2, a4-4..7, fx-3] | Level up, attune, warp and the other bonfire menus (A11), YOU DIED hold and a full map reload on death, respawn in another area, bloodstain SFX, Hollow look [a4-2, a4-4..7, enemy-4] |
| A5 | Player action logic (ESD) | partial: movement, R1, roll, falls | Lock-on, guard / parry / riposte / backstab, two-handing, items, magic / pyromancy / miracles, bows, ladders, gestures, throws [esd-1, esd-2, esd-7, esd-8] |
| A6 | Combat + stats + equipment | partial: physical damage, enemy hits | Damage types and defences, scaling, poise, SpEffects (rings, buffs, poison / bleed / curse), level-up, equip load tiers, weapon upgrades [combat-2..6, gameplay-13] |
| A7 | Enemy runtime | partial: Lua AI, ESD, streaming types, active / drawn by draw group (cap 16 active at the Burg entry), event AI commands; navmesh routes for approaches and returns home, ledge stops, falling; hits on the hkx ragdoll capsules, ragdoll deaths [enemy-2, enemy-17] | Line of sight, all native goals, guard, item drops, every chr type in the game, enemy budget per block, NVM flags (gates, ladders) [enemy-2, enemy-6, enemy-10..13, enemy-15, enemy-17] |
| A8 | Bosses | missing | Boss arenas (fog walls, health bars, music cues, phase changes), boss-specific ESD / AI, multi-part bosses, cutscene intros |
| A9 | Objects | partial: breakable pots; doors, levers, gates and chests animate by event and ObjAct, need their key, block the player while shut and stay open across save / load; every MSB object with Havok bodies collides (as boxes) [event-11, collision-1] | Elevator platforms (call / arrive timing is in, the platform doesn't move), ladders, item corpses (o0500 posed, animated objects), exact collision shapes [map-4, collision-1] |
| A10 | NPCs, dialogue, shops | partial: human NPCs (c0000) placed as stand-ins (player model, idle pose), switched on / off / killed by events [enemy-14] | Equipment look, NPC AI, talk ESDs, FMG text, shops / trading, covenants, NPC questlines through flags [enemy-14] |
| A11 | Menus / HUD | partial: HUD bars, Estus slot; the Start menu is DSR's own screens from menu.drb (icon bar, Equipment with pick lists, Inventory with tabs and Use / Discard, Status, System with Quit Game saving) with DSR's item names, stats, help text and icons [menu-1..5, inv-1..3]; DSR's loading screen (item hint, bonfire animation) over the boot load, warps and death respawns [load-1..5] | Level-up, attune, bonfire menu on DSR's screens, item pickup prompts, boss bar, death screen, title screen |
| A12 | Audio | partial: SPU2 runtime (own IOP module, src/sound.c): player and enemy TAE sounds (footsteps by floor material, swings, armor), MSB emitters (Firelink / Burg music regions, wind, water), boss music by EMEVD map sound state, Play SE, area 5.1 ambience streamed from disc; samples at 8 kHz to fit SPU2 RAM [audio-2, -4, -5, -7..-13] | Higher sample rate (residency by use), reverb, FMOD rolloff curves, spell / voice / cutscene sounds, volume options [audio-2, -4, -7, -8, -11, -13] |
| A13 | Cutscenes / movies | missing | Remo cutscenes (or skip with state changes applied), intro / ending movies via IPU |
| A14 | Rendering per area | mostly done: VU1 map, LOD + streaming, fog, lighting bake, normal relief (back on in every build), FFX fire, far background without sky holes, far ring wall no longer distorted [render-12] | Sky per area, water, special materials (lava, Ash Lake, Tomb darkness, Anor Londo lighting change), all FFX types, characters lit by area lights, near / coarse swap pop, fog wall SFX too white, no anti-aliasing [map-13..19, map-37, char-4, fx-2, fx-12, render-12] |
| A15 | Disc / IOP | partial: ISO builds and boots in PCSX2; packs ordered by world adjacency (cdrom0: boot 4169 -> 3949 ms) | Async cdrom0 streaming tuned for seek times; IOP modules for audio / pad / memcard; real-hardware run [build-3, hw-*] |

## Phase B: make the exporters whole-game safe

| # | Item | Status |
|---|---|---|
| B1 | Every exporter takes area / chr id and reads only game data (no Firelink constants) | mostly done [build-6] |
| B2 | Per-area budget fitting automatic: simplify error, foliage, texture pool, cell sizes, enemy types per block, fail loudly when over RAM / VRAM | partial: per-area MAPENV by hand |
| B3 | Format coverage: every FLVER / MTD / TAE / HKX / FFX variant used anywhere (scan all files once, list unknowns) | partial: Firelink + Burg only |
| B4 | Every chr / obj model exports and draws (skeleton > 61 bones, cloth, multi-part bosses) | partial [enemy-15] |
| B5 | One manifest of areas, blocks, connections and progression order from the game's data (worldgraph.py) | reader exists |

## Phase C: vertical slice (the gate before the scripted pass)

Play through the game's first stretch end to end on the port, with only generic code:
Undead Asylum (tutorial, Asylum Demon) -> Firelink -> Undead Burg -> Taurus Demon -> Undead Parish -> Bell Gargoyles.

It covers: area transitions, fog walls, two boss types, a bell / flag event, shortcuts and elevators, NPCs (Oscar,
Solaire, Andre), a shop (Undead Merchant), levelling at the Firelink keeper, deaths and bonfire respawns, saves.
Gate: completes without crashes on PCSX2 at >= 30 fps, within RAM / VRAM, in an automated playthrough test
(tools/test-run.py scripts with warps + flag checks), plus one manual run.

## Phase D: the automated whole-game pass

1. Scan: run every reader over all game files and list unknown formats / features (B3), then fix those.
2. Export every block with automatic budget fitting (B2); a report per block: tris, MB, RAM / VRAM fit, warnings.
3. Smoke every block: FromSoft autotest points (tools/test-smoke.py), render diff vs DSR reference captures.
4. Progression test: warp-and-flag script through the boss order; each boss killable (debug damage) and its event flags fire.
5. Triage the per-block failures into TECH_DEBT, fix the generic code, rerun.

Tooling since the last update: `make` runs in parallel by default, a simplify cache and C simplifier, a RAM-budget job
queue that can run jobs on another machine (`tools/q`, `tools/qd`), and `tools/memwatch.py` acting on real memory
pressure. See `notes/TOOLS.md`.

## Order

A1 + A2 + A3 first (everything else hangs off streaming, events and flags), then A4 / A9 / A11, then A5 / A6 / A7 / A8
for the slice's fights, then A10 / A12, then the Phase C gate, then B and D. A13 / A14 specials can follow the pass.

Now: A1 S1 and S4 are in review; next are per-block type loading (so the Burg bonfire and Parish blocks join the
default world with enemies on) and S3 (far level for lite neighbours). In A2, E5 boss triggers and E7 warps are what
the slice still needs from events.
