# Exe findings: player movement / stamina constants and ESD names (2026-10-30)

**What this is:** values and rules found by studying the game's executable, such as stamina regen, turn rates and the
names of ESD script functions, so the port can match the original exactly. Each row gives the value, where in the exe
it lives, how it was found and how confident the finding is. It contains facts only (addresses, offsets, constants,
formulas), no decompiled code. Terms: VA = virtual address in the exe; `FUN_1403...` = Ghidra's name for the function
at that address; ESD = the game's state-machine scripts for characters; TAE = per-animation event timelines.

Source: DarkSoulsRemastered.exe (local Ghidra project, read-only). Every address is a VA.
Object chain used below: `chr` = ChrIns/PlayerIns, `ChrCtrl` = chr+0x68 (PlayerCtrl for the player),
`ctx` = ESD/EzState context = ChrCtrl+0x48 (ctx+0x10 = ChrCtrl), WalkAnim = ChrCtrl+0x1A8 (player: `WalkAnim_Twist`),
TurnAnim = PlayerCtrl+0x308. Frame time `dt` = chr+0x458.

## Values

| Value | Unit | Where (function / constant) | How found | Confidence |
|---|---|---|---|---|
| **Stamina regen, player** = 45 + Σ SpEffectParam.staminaRecoverChangeSpeed | stamina/s | PlayerIns vtable slot +0x1F0 = `FUN_14035ed00`; 45.0 at **0x141A2BE40**; sum `FUN_1404034b0` (SpEffectParam +0xB8 over active effects, skipping entry flags 0x800C0003) | vtable xref (PlayerIns::vftable 0x1413251F0 +0x1F0, also ReplayGhostIns); ChrIns base slot uses NpcParam staminaRecoverBaseVel (`FUN_14034af40`), the player override ignores the param | high |
| Regen formula per frame | — | `FUN_14031fca0` (ChrIns update) at 0x14031FDE0: `rate = slot1F0() * (ctx byte +0x9C / 100.0 [0x1412B2334]) * chr float +0x1E4`; `acc(chr+0x1E0) += rate*dt`, integer part added via `FUN_140339680` | disassembly | high |
| Regen % window (the "regen delay") | % of rate | ctx+0x9C byte, written by TAE event **225** SetSPRegenRatePercent: TAE handler `FUN_14033fd90` case 0xE1 | decompile of the TAE event switch | high |
| Regen % in c0000 TAEs (a00) | % | roll 700-703 0% for 0–1.10 s, 710-713 0% 0–1.37 s, 720-723 0–1.67 s, 730-733 0–2.17 s; backstep 680 0% 0–0.80 s; dash 535/536 and run-stop 510-519 0% whole anim; a00 1/3/5, 100-138, 210-214 and the tail of 140-152 20%; R1 a23 3000-3002 0% 0–1.0..1.1 s. No event → presumably 100% (reset not located) | `fndb/work/tae_dump.py`, full list in `fndb/work/tae_events.txt` | high (100% default: medium) |
| Regen off | — | ctx+0x1DC bit 3 (ESD command c1_110) or WalkAnim dash active → no regen that frame | `FUN_14031fca0` | high |
| **Dash drain** = 1 stamina per 0.1 s → **10 stamina/s** | stamina/s | `FUN_14031fca0` 0x14031FD40: while WalkAnim vt+0x40 (`FUN_1403a5d40` = current loco anim is the dash anim WalkAnim+0xA0), `chr+0x1F0 += dt`; when ≥ 0.1 (**0x141A2BC28**) add −1 (`FUN_140339680(chr,-1)`) and reset | disassembly | high |
| Dash lockout after running dry | 3.0 s | same place: if stamina ≤ 0 after the tick, chr+0x1F4 = 3.0 (**0x141A2BC2C**), counts down by dt | disassembly | high |
| Dash allowed when | — | `FUN_140378d00` (ChrCtrl update): WalkAnim+0x1B0 (dash) = dash request (manipulator +0x1E4) && stamina ≥ 1 && chr+0x1F4 ≤ 0 (or chr vt+0x148) | decompile | high |
| Dash request = roll button held | > 0.4 s | `FUN_140398920` (PadManipulator input): hold time +0x260 > **0x141A2C0A0** (0.4) | decompile + constant | high |
| Roll on release / backstep without stick | stamina > 9 at release; stick dead zone 0.45 (**0x141A2C09C**); backstep re-press window 0.12 s (**0x141A2C0B4**) | — | `FUN_140398920` (requests: +0x93..+0x96 rolls, +0x8E backstep, +0xAE jump) | high (0.12 meaning medium) |
| **Roll stamina cost** = 30 (all weight classes) | stamina | ESD `c1_1001(-30)` in group 1 states 32-36 / 94-98 (`c1_3(2, f110(dir))`); c1_1001 = `FUN_14038dae0` case 0x3E9 → `FUN_140322cf0(chr, stamina + n)` | ESD + command decompile; roll anim set chosen by f110 (ChrCtrl+0x210 movement set), cost is the same for every set | high |
| **Backstep cost** = 20 | stamina | ESD states 37-40 / 99-102: `c1_3(2, 680/682/683 or f110(4))` + `c1_1001(-20)` | ESD | high |
| Other ESD costs | stamina | jump/roll-from-dash f110(5) −30 (state 36); states 245/246 anims 7031/7033+10·f293 −30/−40; state 29 anim 7504 −100; group 0 states 2-7 and group 1 50/54/120 (anims 3150/3200/4150/4200/5150/5200) −20 | ESD | high (what the anims are: unknown) |
| R1 attack cost | 20 | BehaviorParam_PC 1023000xx `stamina` (already in export_player.py) | — | high |
| Roll gate in ESD | stamina > 10 | `f1001() > 10` in every roll/backstep transition | ESD | high |
| Generic action gate | stamina ≥ 1 | `FUN_140323c60` (called by every input test f100/f101/f102/f108/f241-243): returns false if chr+0x3F8 < 1 | decompile | high |
| Minimum stamina | −50 | setter `FUN_140322cf0`: lowering is ignored with chr+0x525 bit 2 or DAT_141C77E5B (infinite stamina); < −50 → −50; > max → max. Max-change paths clamp to [−100, max] (`FUN_14035c090`) | decompile | high |
| Max stamina | 80..160 from END | `FUN_1402dfcd0` (see PORTING.md) | earlier session | high |
| **Turn rate, player, walking** | **180 °/s** | `FUN_140381fe0` (PlayerCtrl vtable +0xC8) end: TurnAnim+0x3C = 180.0 (0x43340000) when WalkAnim_Twist+0x140 (gait) == 0 | decompile | high |
| **Turn rate, player, idle / run / dash** | **720 °/s** | same: 720.0 (0x44340000) otherwise (gait −1 idle or 1 run); TurnAnim ctor `FUN_14039ee90` default 720 | decompile | high |
| Turn step | rad/frame | TurnAnim step `FUN_14039fcc0` (also `FUN_1403a0440`, `FUN_1403a0900`): `max = TurnAnim+0x3C * dt * π/180` (0x141615C54 = 0.017453292); yaw delta scaled down to max. Network-driven (ChrCtrl+0x21D) uses 10000 °/s | decompile | high |
| Turn override (attacks etc.) | °/s | ctx float +0x98 ≥ 0 replaces +0x3C (multiplied by π/180 twice in the code); written by TAE event **224** SetTurnSpeed (`FUN_14033fd90` case 0xE0). **No c0000 TAE has event 224**, so the player never uses it; attack/roll turning is limited by TAE ChrActionFlag 7 (DISABLE_TURNING) windows | decompile + TAE scan | high |
| NpcParam turnVellocity (+0x0C) for the player | not used | only EnemyCtrl (`FUN_14037fb90`, vtable +0xC8) reads NpcParam+0x0C (via EnemyCtrl+0x300); `ParamFieldReaders.java` finds no +0x0C read through the ChrIns param getter | decompile + scan | high |
| Walk→run threshold | stick 0.97 ± 0.01 | WalkAnim_Twist +0x21C (0.97) / +0x220 (0.01), ctor `FUN_1403a4bc0`, used in `FUN_1403a5030` | decompile | high |
| Quick-turn anim trigger | 105° | `FUN_14039f980` with DAT_141A2C154/158 = 105.0 (TurnAnim anims 602-605/620) | decompile | medium |

## ESD names (full list with evidence: `data/esd_names.json`)
Env functions: `EzStateActionEnv` vtable +0x8 = `FUN_14038b540` (switch on the id). Commands: `EzStateActionEvent`
vtable +0x8 = `FUN_14038dae0`. The id is the case value (f102 = case 0x66).

| Id | Name | Args | What it reads / does |
|---|---|---|---|
| f0 / f1 | IsAnimEnd / IsMoveAnimEnd | – | anim handle ctx+0x80 (c1_3) / ctx+0x78 (c1_4) finished |
| f100 | IsButtonTriggered | slot | pad request cur && !prev |
| f101 | IsButtonReleased | slot | !cur && prev |
| f102 | IsActionRequested | slot | latched request ctx+0x468, stamina ≥ 1 gate, delay timer ctx+0x320 ≤ 0. Slots: 0/7 R1, 5 R2, 10 backstep, 15-18 rolls, 42 jump; 14 → spell states, 2/3 → item states |
| f108 | IsStepRequested | slot | same, only slots 9-12/15-18/42 |
| f110 | GetRollAnimId | dir 0-5 | anim from ChrCtrl movement set (+0x1C0[+0x210]) |
| f207 | GetWeaponHoldStyle | – | chr+0x198[0]: 1 one-hand, 2/3 two-hand |
| f219 | GetMoveState | – | 0 idle, 1 walk, 2 run, 3 dash |
| f220 | IsSlotHeld | slot | ctx+0x2E1[slot] |
| f222 | GetDamageDirection | – | ctx+0x1E8 |
| f225 | GetWeaponMotionCategory | hand | EquipParamWeapon.wepmotionCategory of the hand's weapon (23 straight sword, 44 bow, 46 crossbow …) |
| f227 / f231 | GetSelectedItemMotionType / GetSelectedSpellMotionType | – | goods +0x25 / magic +0x3E, 0xFE/0xFF = unusable |
| f232 / f233 | HasUsableItem / CanCastSpell | – | current goods / magic checks |
| f236 / f237 | GetDamageLevel / GetDamageReactionClass | – | ctx+0x1EC / table 0x141A2B780[lvl][ctx+0x1F0] |
| f241 / f242 / f243 | IsActionRequested19 / 20 / IsSlotHeld21 | – | fixed slots 19, 20, 21 |
| f284 / f285 | GetDamageInfo3 / 4 | – | ctx+0x1F4 / ctx+0x1F8 (meaning open) |
| f300 | IsCtxFlagA0_20 | – | ctx+0xA0 bit 20 (meaning open) |
| f305 | HasSpEffectStateInfo | stateInfo | active SpEffect with SpEffectParam.stateInfo == arg |
| f311 / f312 | GetHandWeaponInventoryValue / IsWeaponUsableInHand | hand | PlayerIns vt+0x3D8 / +0x3E0 (low confidence) |
| f1000 / f1001 | GetHP / GetStamina | – | chr+0x3E8 / chr+0x3F8 |
| f1003 / f1004 | RandInt / RandFloat | min, max | |
| c1_3 | SetAnim | weaponSel, animId, layer, blend, loop | resolves weapon-motion anim (FUN_140325f70), plays, handle → ctx+0x80 |
| c1_4 | SetMoveSyncAnim | + syncMode | handle → ctx+0x78, speed synced to WalkAnim |
| c1_101 | SetLocomotionEnabled | bool | ChrCtrl+0x1B0; 0 during rolls/attacks |
| c1_109 | SetChrMotionMode | −1..12 | chr+0x270 (meaning open) |
| c1_110 | DisableStaminaRegen | bool | ctx+0x1DC bit 3 |
| c1_123 / c1_124 | OnSpellRejected / OnItemRejected | – | obfuscated PlayerIns vt+0x330 / +0x368 (low) |
| c1_134 | SetAnimOverride | animId | ctx+0x440 |
| c1_1001 | AddStamina | delta | FUN_140322cf0(stamina + delta) |
| c1_1100 | (no-op) | arg | case 0x44C only reads its argument |
| c1_9000 | DebugPrint | string | |

## Implemented in ps2/proto (2026-10-30)
`src/player.c` now uses these values (addresses in its comments): int stamina with the FUN_140322cf0 clamp [-50, max],
max from END (Warrior 95), regen 45/s x TAE 225 % with the fraction carried, dash -1 per 0.1 s without regen,
3.0 s dash lockout, dash only after > 0.3 s in the run gait, ROLL_TAP 0.40, walk/run hysteresis 0.97 +- 0.01 on
tilt / 0.92 (assumed pad mapping), turn 180 deg/s walking / 720 otherwise, roll 30 (> 10 needed, > 9 at release),
backstep a00 680 for 20 on a Circle tap with no stick, R1 needs stamina >= 1. Checked by a native harness
(`fndb/work/ptest`: test.c / test2.c compile player.c with stubs) and in PCSX2. Open: TECH_DEBT gameplay-8, 12-14.

## ESD runtime (2026-10-31)
The names above drive ps2/proto's ESD runtime (src/esd.c, player.c player_esd_env / player_esd_cmd). New from it:
f110(n) = MoveParam row fields escapeF/B/L/R (0x28-0x34), stepMove (0x48), dashEscapeF (0x78) (case 5 found by
map-collision); EzState `~abort` = condition false, `~continue` = no-op (inferred from the bytecode patterns).
See PORTING.md "ESD runtime" and TECH_DEBT esd-1..7.

## Request gate and anim layers (2026-10-31, ESD runtime)
- FUN_140323c60(chr, b): chr+0x3F8 < 1 -> false; b -> true; else !FUN_14032c4f0(chr). FUN_14032c4f0: if
  (chr+0x68)->+0x20 == 0 returns (chr+0x20)->+4 >= 0, else 1 unless the handles at chr+0x16C..+0x178 equal
  DAT_141a2bc90 (0x20FFFFFFFF, invalid handle) -> partner link (throw). Callers: f100 / f102 / f108 pass ctx+0xF2[slot],
  f111 (FUN_140386bb0) ctx+0x49E, FUN_140388020 ctx+0x49F before FUN_140388790.
- FUN_140388020 sets ctx+0x49E = some slot has ctx+0xF2 (Anim Cancel) set, ctx+0x49F = some slot latchable (Key Input
  window or c1_146) with ctx+0x127 set; ctx+0x49D/0x49E cleared each frame.
- FUN_140385550 (c1_3's play): layer < 11 -> ChrCtrl vt+0x38 with slot table 0x14132D000[layer]
  (0 1 2 3 5 6 7 8 9 11 10), layer < 13 -> again with 0x14132D030[layer] (15 16 20 21 17 18 23 24 19 26 25 27 28), the
  second handle stored in ctx+0x80; then FUN_140388670 (latch clear) when animId >= 0. c0000 uses layers 2 / 4 / 6 / 7 / 9
  (group 0: falls 7, plunge 6, damage 9; group 1: actions 6, guard 4).

## EMEVD runtime and event flags (2026-11-02, A2 / E2)

| Value | Where (function / constant) | How found | Confidence |
|---|---|---|---|
| Event flag manager | `DAT_141c7c5f0`; two flag sets (+0x28 and +0x120, active set +0x218), per id digit group (id / 10^7 % 10) a list of area records of 0xA8 bytes (10 x {ptr, size 0x80} per 1000 flags); record counts per group at **0x141369fc0** (set 0: group 0 one record, groups 1 / 5 / 6 / 7 18 records each) | Lua binding SetEventFlagValue (`FUN_140020a70` -> vtable 0x141a2ff00 -> `FUN_1404b12a0` -> `FUN_1404f57c0`), record lookup `FUN_1404f39a0` | high |
| Flag bit | flag n = bit 31 - (n % 1000 % 32) of word (n % 1000) / 32 of its chunk (`FUN_1404f22b0` set, `FUN_1404f23d0` get) | decompile | high |
| EventValue (N flags as a number) | `FUN_1404f2590` get, `FUN_1404f24b0` set: the 64-bit pair of words at the first flag, shift 64 - bit - N: the first flag is the most significant bit; Lua Get/SetEventFlagValue use N = 8 | decompile | high |
| Temporary flags at an area load | `FUN_1404f1fb0` (called from `FUN_1403ce680` in the load task `FUN_14024f530`): clears chunks 4 + 5 of the group-0 record and chunks 5 + 6 of every group-1 record (flags 4000-5999, 1AAB5000-1AAB6999) | decompile | high (when it runs: medium) |
| Temporary flags on the event manager's restart pass | `FUN_1404f20b0`, first call of `FUN_1407cb660` (callers `FUN_1407cbf30`, `FUN_1407cc300`): group-0 chunk 5, group-1 chunks 5 + 6 | decompile | high (that this is the rest: medium) |
| Event instance End / SetEventState | EmkEventIns vtable **0x1413fabf8** +0x28 = `FUN_1407d4230(state)`: 0 ends, else restarts; then sets flag id + slot if id > 98 and slot >= 0. The run loop `FUN_1407d40b0` calls it with 0 at the end; 2003[08] reaches it through `FUN_1407cb090` (instance lists of the event manager, matched by id and slot) | decompile | high |
| Bank 2003 handler | `FUN_1407d0970` (switch on the instruction index: [02] SetEventFlag, [04] AwardItemLot -> `FUN_1404ab360` -> `FUN_1403fc940`, [08] SetEventState, [11] boss health bar `FUN_1404905f0` (entity, name id, slot), [12] HandleBossDefeat `FUN_1404ab4b0(0xFD2, entity)`); bank 2004 handler `FUN_1407ce820` | decompile | high |
| Item lot draw | `FUN_1403f5d90` (`FUN_1403f5360` is the online variant): rows id, id + 1, ... while one exists, each rolls one item: weight = basePoint x 100 (cumulate share by the counter, luck items x item discovery else x 1.0 at **0x141615c6c**), items whose get flag (getItemFlagId0N, else the row's) is on are skipped; roll = tempered random x 2^-23 (**0x14130ca90**) x total; "nothing" = lotItemNum 0 or id / category < 0. `FUN_1403fc940` then awards the results and sets their flags | decompile | high (luck factor value: unknown) |
| Region box / cylinder test | FrpgRegionBox vtable 0x14133d258 +0x18 = `FUN_1403f00e0`: offset turned by -rotY, x in (-w/2, w/2), y in (0, h), z in (-d/2, d/2) (bounds from **0x14133d4e0** / **0x14133d490**, set in `FUN_1403efa00`); FrpgRegionCylinder +0x18 = `FUN_1403f0de0`: y in [0, h], xz distance < r | decompile + constants | high |

## Still unknown
- Slot priority inside the anim player (ChrCtrl vt+0x38) when both ESD groups play anims; what f0 sees then (esd-9).
- Which NpcParam row PlayerIns+0x7E0 points at (PlayerIns ctor is in the obfuscated .text). It no longer matters for
  regen (player override uses 45) or turn rate (not param-driven); HP/MP come from PlayerGameData.
- Where ctx+0x9C (regen %) and ctx+0x98 (turn override) are reset each frame (assumed 100 / −1).
- The anims behind the other ESD stamina costs (3150/3200/4150/4200/5150/5200, 7031/7033, 7504) and what each roll
  movement set (ChrCtrl+0x210 → 700/710/720/730/735/740 families) corresponds to in equip-load terms.
- Pad slot → button mapping beyond rolls/backstep/jump/R1/R2; f284/f285/f300/c1_109 meanings; f311/f312 details.
- WalkAnim_Twist debug labels ("Turn Gain", "UpperBody Rotation Rate", "Forward Limit Angle"...) have no code refs
  (debug draw is in the obfuscated .text), so its +0x208..+0x218 fields (0.35, 60, 5, 60, 5) are unnamed.

## Tools added (ghidra/scripts, fndb/work)
- `Peek.java kind:addr[:n]` – floats/ints/qwords/disassembly/refs (refs name the vftable slot for data refs).
- `ParamFieldReaders.java [disp]` – reads of the NpcParam row behind ChrIns vtable +0x100.
- `FindOperand.java text…` – instructions containing a substring.
- `DecompileList.java` takes an optional 5th arg: max characters per function (default 24000, which truncated
  the ESD evaluator/dispatcher in fndb).
- `fndb/work/gh.sh <script> args` – headless wrapper; `fndb/work/shrink.py` compacts ESD switch decompiles;
  `fndb/work/params.py` param field offsets; `fndb/work/tae_dump.py` TAE 224/225/flag-7 dump;
  `fndb/work/make_esd_names.py` regenerates data/esd_names.json. Full decompiles: `fndb/work/esd_14038b540.c`,
  `esd_14038dae0.c`, `fn_14031fca0.c`, `fn_1403a5030.c`, `fn_14039fcc0.c`.
