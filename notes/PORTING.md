# DSR → PS2 porting project notes

**What this is:** the project's engineering log, in the order the work happened (newest at the bottom). It records
file formats, measurements, gotchas and the reasons behind engine decisions. It's long and written for people working
on the code; to build and play, read `README.md`, and for what works now, `notes/PLAYTHROUGH.md`. Search it for a
format or system name (for example `FLVER`, `collision`, `VU1`) rather than reading it top to bottom. Some sections
are dated session notes ("Resume after restart") and mention local paths from the maintainer's machine.

No game data or decompiled code is included here: only facts (layouts, offsets, sizes, measurements).

## Target identity
- `DarkSoulsRemastered.exe` SHA-256 `a45aaa36dd2f6cc151670a639ea5547043cf38ea79ff4178b963c6ed71f98d7b` (`exe.sha256`)
- PE32+ x86-64, MSVC 2015+ CRT (VCRUNTIME140/MSVCP140)
- Sections: `.text` 18.6 MB @0x140001000, **second `.text` 17.5 MB @0x142019000** (probably packer/obfuscation;
  check before trusting decompilation of that range), `.rdata` 7.5 MB, `.pdata` 1.3 MB (≈ 90k unwind entries ≈ function count)
- Imports: d3d11/dxgi (render), DINPUT8/XINPUT (input), fmodex64/fmod_event64 (FMOD Ex audio), binkw64 (Bink video),
  steam_api64, WS2_32/IPHLPAPI (online)

## Asset sizes (6.9 GB total)
| Dir | Size | PS2 note |
|---|---|---|
| map | 2.7 GB | MSB layouts + FLVER map pieces + collision (HKX) |
| parts | 1.5 GB | armour/weapon FLVERs |
| chr | 989 MB | characters + animations (HKX/TAE) |
| sound | 609 MB | FMOD FSB/FEV |
| obj | 185 MB | objects |
| param/script/event | ~3 MB | params, Lua AI (luabnd), EMEVD events: **portable as data** |

## Similar projects (reference)
- soulstruct (grimrukh): Python DS1/DSR formats, MSB/EMEVD/params: https://github.com/grimrukh/soulstruct
- Dark-Souls-1-Overhaul (metal-crow): C++ DSR runtime RE/hooks, names many game structs: https://github.com/metal-crow/Dark-Souls-1-Overhaul
- No public DSR decompilation exists.
- PS2 decomps using splat (layout reference for the PS2 side): god-hand-decomp, ssxdvd, OpenRAC rac1-decomp
  (its native PC port ReRAC is the "decomp → reimplementation" pattern applied in reverse here)

## Approach (not a matching decomp)
A matching decomp of an x64 MSVC binary does not help a MIPS target. Plan: **reimplement the engine on PS2**,
reading the decompiled DSR code as the specification, with game data converted offline.

1. RE (REA/Ghidra): file loaders (DCX/BND3/BHD5), param system, EMEVD interpreter, ESD/Lua AI hooks, TAE
   animation events, damage formulas, player state machine.
2. Offline converters (PC, `tools/`): FLVER → PS2 strip meshes (GS packets), TPF/DDS → 4/8-bit CLUT textures,
   HKX anims → reduced keyframes, FSB → VAG/ADPCM, MSB → per-area streamed cells.
3. PS2 runtime (`ps2/`, ps2sdk): VU1 renderer, CDVD streaming, IOP SPU2 audio, game logic ported from step 1.

## Hard limits (do not plan around these)
- 32 MB EE RAM / 4 MB GS VRAM / 2 MB SPU RAM: areas must be split into cells, budget ~8–12 MB per resident cell.
- Havok physics/animation is a licensed middleware library; it needs a replacement, not a port.
- Lua AI scripts need a Lua 5.1 VM on the EE (feasible, ~200 KB).

## Resume after restart (2026-10-30)
- Toolchain: ps2dev v2.0.0 in `~/tools/ps2dev-2.0.0/ps2dev` (tarball sha256 c8e5dedc…c028). `. ps2/env.sh` (from the repo root)
- Built: `ps2/vu1test/vu1.elf` (ps2sdk VU1 cube sample). `ps2/hello` is an ERL sample, does not link as ELF; ignore.
- PCSX2: flatpak `net.pcsx2.PCSX2` (--user) installed.
  BIOS on host: `~/Downloads/scph77001-us.bin`.
- Ghidra: persistent project `$DSR_GHIDRA_PROJECT` (host, default ~/Projects/dsr-ghidra; symlink ghidra/project; Ghidra forbids dot-dirs like .local in project paths).
  Trimmed analyzers via ghidra/scripts/DsrFastAnalysis.java; logs ghidra/logs/. JVM needs JAVA_TOOL_OPTIONS="-XX:-UseContainerSupport -Xlog:os+container=off" inside the Claude app (cgroup warnings break launch.sh).
- Old REA command (temp project, deleted on exit):
  `REA_GHIDRA_STARTUP_TIMEOUT_MS=14400000 npx -y rea-agents@6.8.0 analyze "<exe>" --provider ghidra --snapshot rea/dsr.snapshot --format json > rea/analyze.json`
  (with GHIDRA_INSTALL_DIR/JAVA_HOME from ~/tools)

## Function database (fndb/)
- `fndb/fndb.py`: SQLite `fndb/dsr-functions.sqlite` (tables `functions`, `calls`). Subcommands: import-meta, prioritize,
  select, import-decomp, label, status. `FNDB_DB=` overrides the DB path (tests use `fndb/test/test.sqlite`).
- Tiers from `prioritize`: 1 = file/param/event/AI/anim/map/chr/combat seeds, 2 = up to 2 call hops from a seed,
  3 = rest, 4 = render/sound/net (replaced on PS2), 5 = second `.text`, 9 = thunks/tiny/CRT (not labeled).
- Workflow (after Ghidra analysis finishes):
  1. `fndb/batch.sh export` (once): metadata of all functions → DB → tiers
  2. `fndb/batch.sh 1 500` then `batch.sh 2 1000`, …: decompile the next N in Ghidra, label them, print status
- Labeler: Ollama, 4 parallel slots (`ollama-up.sh`: flash attention, q8 KV cache), labels bottom-up (callees first),
  no throttling by default (`--max-load/--max-gpu/--pause-if/--sleep` to restrain it).
- Synthetic test 2026-10-30, 4 functions, warm, ~400-token prompts:
  qwen2.5-coder:7b 7.9 s per round of 4 (15% CPU / 85% GPU); qwen3:8b 14.5 s (43% CPU, does not fit with RuneLite open).
  qwen2.5-coder mislabeled a DCX header check as a param header; check tier-1 labels by hand.
- RuneLite closed (2.3 GB VRAM used by desktop/Discord), qwen2.5-coder:7b, 4 slots, warm round of 4:
  ctx 8192 5.3 s (27% CPU) · ctx 6144 1.8 s (9% CPU) · **ctx 4096 1.5 s (100% GPU)** → defaults now --ctx 4096 --max-chars 9000.
  qwen3:8b ctx 8192: 16 s (45% CPU). Functions over ~9000 chars of C are truncated; rerun those with a larger --ctx.
- Context is bucketed by code size (num_ctx is fixed per model load, and KV = ctx x slots): batch.sh runs
  ctx 4096 x4 slots → 8192 x2 → 16384 x1 --truncate, restarting Ollama with matching OLLAMA_NUM_PARALLEL (ollama-up.sh N).
  Each pass only takes functions whose C fits (≤ 4.2k / 11.2k / 25.1k chars); all three ran 100% on GPU.
- Measured Ghidra C ≈ 1.8 chars/token (default --chars-per-token 1.7). Ollama silently cuts an over-long prompt to
  ctx/2 tokens from the front (drops the system prompt → junk labels); the labeler now refuses to save those.
- Final labeler design (2026-10-30): `ctx` column set per function at import-decomp (`fndb.py assign-ctx` recomputes).
  `label --tier 2` runs every bucket to completion in order (4096 x4 → 8192 x2 → 16384 x1), restarting Ollama per bucket,
  continuous queue (a slot refills as soon as one finishes), then returns Ollama to 4 slots. A prompt that still
  gets truncated is moved up a bucket automatically. First real run: `fndb/batch.sh` (tiers 1–2).
- Resume: labels are committed per function; Ctrl+C / closing the window finishes the in-flight requests and exits
  (second Ctrl+C quits at once). DecompileList flushes each function; batch.sh imports a leftover work/decomp.jsonl first.
  Rerun the same command (`fndb/batch.sh`) to continue. Tested: stop at 12/24 → resume labeled the other 12.
- Vector lookup: `$FNDB_PY fndb/fndb.py embed` (batch.sh runs it after labeling; only new/relabeled
  functions) → `fndb/dsr-functions-vec.sqlite` (bge-small-en-v1.5 384d via fastembed CUDA, sqlite-vec + FTS5).
  `… fndb.py search "load DCX archive" [-k 10] [--subsystem file] [--tier 2]`: hybrid vector + keyword (RRF).
  Test: 24 labels embedded in 0.3 s; a search takes ~1.2–2.4 s, almost all of it loading the embedding model.
- DecompileList uses ParallelDecompiler (one decompiler process per thread, default all 16 cores;
  `DECOMP_THREADS=8 fndb/batch.sh` to limit). Auto-analysis itself is single-threaded by design (shared DB writes).

## Analysis results and decisions (2026-10-30)
- Ghidra auto-analysis finished 16:11 (~2 h 35 min); project 662 MB. 255,793 functions exported. Tiers:
  1: 13,861 · 2: 13,307 · 3: 68,968 · 4: 921 · 5 (second .text): 87,755 · 9 (skip): 70,981.
- **LLM labeling dropped** (user decision): 7B labels on stripped MSVC code are guesses; not worth 9–14 h GPU.
- Tiers 1–2 decompiled: `batch.sh decompile 2` (decompile only, no labeling), 12 threads, ~25 min for 27,168
  (1 failure, 153 truncated at 24k chars). The default `batch.sh [TIER]` mode still decompiles + labels.

## Code search (fndb.py embed-code / code-search)
- `dsr-functions-code.sqlite`: jina-embeddings-v2-base-code vectors (768-dim, max 2048 tokens) of the decompiled C,
  plus FTS5 over the same text. Header lines add name, referenced strings and English for Japanese strings (from jp/).
- Run with `$FNDB_PY -I fndb.py ...` (a Python with fastembed + sqlite-vec: `FNDB_PY`, else `.venv-fndb/bin/python` in
  the repo root, else python3). `embed-code` is incremental (decomp_at); 27k functions
  took ~6 min on the GPU (unload Ollama first: it fills the 8 GB card).
- `code-search "describe it"` / `code-search --like ENTRY`; `--min-size` (default 120 B) hides stubs, which otherwise
  sit near every query. Good when there are strings / library code (zlib, Havok, ParamMan); weak for string-free
  gameplay math.
- `embed-code` only re-embeds functions whose decomp changed, so new translations are not picked up by a rerun:
  delete dsr-functions-code.sqlite and rerun (~6 min) once jp/translate.py has finished.

## Japanese strings (jp/)
- `extract.py <game dir> paramdef param script event menu other remo mtd map/MapStudio`: UTF-16LE + Shift-JIS,
  NUL-terminated, must contain kana, kanji limited to JIS level 1 (filters x64 code bytes). ~19.9k strings.
  msg/ is skipped: game text already ships in English.
- `translate.py` (qwen3:8b, think off, ctx 2048, batches of 20 with numbered keys in the JSON schema; plain arrays
  made models drop/shift entries). Resumable; Ctrl-C finishes in-flight batches. ~0.1 s/string with the CPU idle,
  ~0.8 s while Ghidra/Steam shader cache use the CPU (21% of layers spill to CPU on the 8 GB card).
- Many exe debug strings already carry FromSoftware English glosses: "(Run)走り".

## Resume after reboot (2026-10-30 evening)
Translation and the code-index rebuild are both resumable; inside the distrobox, from the repo root:
1. `fndb/ollama-up.sh 2`
2. `(cd jp && setsid nohup python3 -I translate.py > translate.out 2>&1 < /dev/null &)`
3. `(cd fndb && setsid nohup ./rebuild-code.sh > work/embed-code.log 2>&1 < /dev/null &)`
   (waits for translation, unloads Ollama, rebuilds dsr-functions-code.sqlite)
4. Watch: `./progress.sh`

## Japanese strings → functions (done 2026-10-30)
- ExportFunctions only sees strings Ghidra typed as string data; it left the Japanese UTF-16/Shift-JIS strings
  untyped, so no function listed them. Fix: `jp/locate.py <exe> fndb/work/jp-addrs.tsv` (string → VA, 1465/1493
  found) → `ghidra/scripts/JpRefs.java` (references, 958) → `fndb.py import-jp-refs fndb/work/jp-refs.tsv`
  (537 functions; it drops them from the code index) → `embed-code` (368 of them are decompiled tiers 1–2).
- Only exe strings can be referenced by code; param/event/map names live in data files, so they are lookup-only
  (jp/jp-strings.sqlite), not in the code index.
- code-search embeds the query on the CPU by default (`--gpu` to override): the GPU is often full with Ollama.
- Translation: 19,918/19,923 (5 NPC dialogue lines keep failing; English exists in msg/). Quality: generic terms good;
  proper nouns unreliable (カアス → Kaath/Caas, 王の器 → "King's Relic", stray Chinese characters in ~4%).
  1,136 rows unchanged, mostly exe strings that already carry an English gloss.
- Glossary pass (jp/glossary.py → `translate.py --redo`): 1,682 terms from msg/*_name_.fmg (JAPANESE/ENGLISH share IDs)
  plus 26 manual (Kaathe, Darkstalker, Lordvessel...). Menu/help FMGs excluded (UI phrasing), and サイズ/回復/テスト.
  Redid 5,668 strings: 594 improved, 84 regressed (whole batches echoed back) and were reverted to prev_en with the
  plain model tag, so another `--redo` retries only those. Rows with leftover Japanese/Chinese: 953.

## PS2 character prototype (ps2/proto, 2026-10-30)
- `make` in ps2/proto regenerates src/player_data.h (tools/export_player.py) and builds proto.elf (gsKit, CPU transform).
  Run: `tools/pcsx2-run.sh ps2/proto/proto.elf [secs shot.png]` (PCSX2 flatpak, BIOS in its config dir; capture via
  tools/winshot.py). Demo script plays when there is no pad input for 5 s.
- From game data: anim lengths, TAE ChrActionFlag windows (cancel/turn/i-frames), AttackBehavior hit windows, SP-regen
  windows, Havok root motion (walk 1.50, run 3.67, dash 6.33 m/s; roll/attack lunges), R1 stamina 20 (BehaviorParam_PC
  1023000xx), Warrior start (CharaInitParam 3000, Longsword 201000), R1 chain 3000->3001->3002->3001 (ESD states 46/48/49).
- Real model + animations (2026-10-30, second pass): tools/export_model.py -> src/model.bin (545 KB): c0000 Havok
  skeleton (61 bones), Hard Leather set HD/BD/AM/LG_M_9360 + Longsword WP_A_0201 at FLVER LOD2 (2,960 tris, 3,375 verts,
  2 bone weights), colour = average of each mesh's DXT1 diffuse; 8 hkaSplineCompressedAnimations (a00 0/200/500/535/700,
  a23 3000-3002) baked at 30 fps. PS2: CPU skinning (3x4 bone matrices), per-vertex light, 0.15 s crossfades.
  tools/havok_anim.py ports SoulsAssetPipeline's spline decoder; souls.HkTag reads TAG0 type bodies (field offsets).
- Right-hand weapon: DS1/DSR flip 180 deg about X and Y (= Z) on R_Weapon (DSAnimStudio NewChrAsm isOldGame rback/rside).
- Floor is drawn at the farthest depth (flat_z): real floor depths intermittently hid the character's legs.
- Textures (2026-10-30, third pass): each mesh's DXT1 diffuse, mip <= 256 px (min width 128), quantized to 256
  colours (Pillow median cut, so model.bin is built with PY = a venv that has Pillow), PSMT8 + CT32 CLUT in
  CSM1 order, uploaded once into the VRAM left after the 640x448 CT24 frame/Z buffers (6 textures, 442 KB still free).
  Drawn with gsKit_prim_triangle_goraud_texture_3d (affine UVs), modulated by per-vertex light (0x80 = 1.0),
  GS_CMODE_REPEAT because DSR UVs run past 0..1; per-triangle UVs are shifted by whole texture sizes to stay >= 0.
  Bare-skin mesh (BD_M_body) has no texture in these archives: flat skin tone (until 2026-10-31: see "Bare-skin texture").
- NOT from game data yet: max stamina/regen (NpcParam row 1 defaults), roll cost 30 (ESD c1_1001(-30) guess), dash drain, turn rate.
- Formats decoded in tools/: souls.py (DCX, BND3, FMG, PARAMDEF/PARAM, TAE, Havok TAG0 root motion), esd.py
  (c0000.esd -> data/c0000.esd.txt; env function ids unnamed). Event/flag names: data/ref/TAE.DS1.xml (DSAnimStudio).
- gsKit gotchas: flat gsKit_prim_triangle_3d draws only dots after the first triangle (use the gouraud call);
  PrimAAEnable on untextured triangles also broke fills.

## Firelink Shrine map (m10_02_00_00, 2026-10-30)
- tools/export_map.py -> ps2/proto/src/map.bin: MSB1 map pieces (67 placements, XZY Euler degrees) with FLVER
  meshes, area textures from map/m10/m10_000N.tpfbhd/bdt (BXF3), navmesh from the MSB navmesh parts + nvmbnd (NVM),
  spawn = MSB player part c0000_0000 (-47.41, -59.82, 53.30, yaw 145).
- Most Firelink pieces have only LOD0 (540k tris; LOD1/2 exist for few). The exporter thins grass cards (1 quad in 16)
  and simplifies the rest by vertex clustering (cell CELL m within RING m of the spawn, doubling with distance;
  merged vertices go to the cell average). Defaults CELL=0.2 RING=12 -> 87.7k tris; CELL=0.4 RING=6 -> 32k but
  visibly shattered; CELL=0.15 RING=20 -> 143k (screenshot notes/firelink-ps2-detail.png).
- Textures: BC1 and BC7 (the remaster's foliage/sky) decoded by tools/native/libbcdecode.so (bcdec.h, MIT; build:
  gcc -O2 -shared -fPIC -o libbcdecode.so bcdecode.c), <= 128 px, 255 colours + transparent index for alpha-test cut-outs.
- PS2: framebuffers now CT16 (dithered) + Z24 to free VRAM; CLUTs packed in one block (separate allocs round to 8 KB);
  70 map textures + 6 character textures fit with ~370 KB left. Frustum + 70 m distance culling per mesh, view-space
  clipping (near plane + 1700 px guard band) only for triangles that need it, alpha test (AREF 0x40) for foliage.
- Navmesh: player height = navmesh triangle under it nearest the current height (step up 0.6 m / down 1.2 m); moves
  that leave the navmesh are refused (no wall sliding yet).
- Performance: 6 fps with ~42k triangles in view (PCSX2 counter and the HUD agree). The EE-only path (CPU transform
  + one gsKit call per triangle) is the limit -> VU1 renderer is roadmap item 1. Character alone: 60 fps.
- Known issues: sky/background strips with wrong colours (skydome meshes), unsimplified map (286k tris, 15 MB ELF)
  crashes PCSX2 with "Jump to unaligned address" (not investigated), demo script barely moves at 6 fps.
- Formats added to tools/souls.py: BXF3, MSB1 (models + parts), NVM, FLVER vertex colours, dds_decode_rgba (BC1-3, BC7).

## Back-face culling + VU1 renderer (2026-10-30)
- "Flipped normals" were back faces: vertex normals point outward, but DS front faces are clockwise on screen
  (Direct3D) and nothing was culled, so the inside of sky/background/walls showed. The FLVER face-set cull flag is now
  exported (map chunks, character groups) and honoured: EE r_backface(), VU1 cross-product sign.
- ps2/proto/src/vu1map.vsm: hand-scheduled VU1 microprogram (dvp-as): per triangle, 3 vertices -> view transform,
  1/z (DIV + WAITQ), screen = v q scale + offset, STQ (perspective-correct texturing), RGBAQ from the vertex colour,
  XYZ2; CLIP of (x F/guard, y F/guard, z - 2 near) vs w = z marks vertices behind the near plane or outside the guard
  band, OPMULA/OPMSUB gives the screen cross product; either sets ADC on the third vertex (GS skips the triangle).
  Constants at VU mem 0-7, double buffers at 8 and 504 (BASE 8, OFFSET 496).
- vu1.c builds the DMA chain by hand (VIF codes in the DMA tag's upper half): per batch CNT header (A+D TEX0/TEX1 + PRIM
  GIF tag) and three REFs straight into map.bin (pos V3-32, st V2-32, colour V4-8 unsigned), MSCAL 0. Two chains:
  map (kicked, drawn while the EE skins the character) and character; gsKit (PATH3) clear runs before, the EE-clipped
  triangles, bare skin and HUD after vu1_wait.
- export_map.py now writes VU1 chunks: <= 40 unindexed triangles per (texture, cull, grid cell), grid 8 m doubling
  with distance from the spawn -> 5,821 chunks for Firelink. Chunk bounding-sphere frustum test; chunks touching the
  near plane/guard band also go to VU1 and only the triangles VU1 skips are clipped on the EE (stricter test there).
- Result in PCSX2 (same scene): 6 fps -> 30 fps; map pass 12-13 ms (cull + ~300-500 EE-clipped triangles),
  character 7 ms (skinning on the EE, triangles via VU1), VU1 never makes the EE wait. 60 fps needs < 16.7 ms total.

## Fixes after the VU1 pass (2026-10-30)
- Back-face cull sign was inverted (EE and VU1): DS front faces give a *negative* (b - a) x (c - a) on our y-down
  screen. The wrong sign removed floors, walls and the outside of the armour (character "floating", map "not all
  there", "flipped normals"); verified with a culling-off A/B capture in PCSX2.
- Depth, real cause: the microprogram did mulq.xyz on the view vector, so z became z * (1/z) = 1 and every VU1
  vertex got the same GS depth (last drawn won -> wrong model order). Now mulq.xy + addq.z: depth = q = 1/z, scaled
  to ZMAX*NEAR/z like the EE path. Also, each VU1 batch now sets TEST_1 itself (A+D: TEX0, TEX1, TEST; header is
  6 qwords, vertices start at TOPS+6), because gsKit's clear can leave ZTST=ALWAYS until its next primitive.
- MSB objects (type 1) are exported: obj/oXXXX.objbnd main FLVER + its TPF (bonfire o0200, pots o1497, ...);
  skinned object meshes (corpses o0500) are skipped until object animation exists. The bonfire's coiled sword is
  not visible yet.
- Map textures capped at 128x128 (objects added 380 KB; the 128x256 ones would not fit VRAM).
- Map simplification now CELL=0.12 RING=16: ~27k triangles in view at 30 fps; ELF 13.5 MB (a 15 MB ELF crashed
  PCSX2 earlier, so the stream format needs shrinking before going finer).
- Depth, the remaining cause: the CT16 colour buffer was paired with a 24-bit Z buffer (32-bit storage). The GS needs
  colour and Z of the same pixel size; with the mismatch, depth tests failed (underground walls drew over the floor).
  Now PSMZ16S + CT16, ZMAX 65535, near plane 0.25 m (Z = 65535 * near / z: ~1 unit per 0.6 cm at 5 m, per 25 cm at
  40 m). Also freed 573 KB VRAM (764 KB free). The bonfire sword shows now; it was hidden by the same bug.

## Cracks, slabs, camera and movement (2026-10-30)
- Non-skinned FLVER meshes are relative to their default bone (souls.flver_bind_world): 29 Firelink map models had
  0% of their raw vertices inside their own bounding box, 89-100% after the bone transform. Applied in export_map.
- Effect materials skipped (is_effect): additive light shafts (A10_BG_shaft[Dn]_Add) and alpha-blended mist/cloud
  ([Dn]_Alp) drew as solid grey slabs. Needs an alpha-blend pass to bring them back.
- Cracks: clustering merged vertices to per-mesh cell averages, so adjacent meshes' seams separated. Merged vertices
  now go to the cell centre (shared by every mesh). A few slivers remain (ring boundaries / original T-junctions).
- Camera from LockCamParam row 0: camDistTarget 4.0 m, chrOrgOffset_Y 1.42 m, camFovY 43 (FOCAL_Y 568.4 px,
  FOCAL_X 609 px for 640x448 shown 4:3), pitch down to rotRangeMinX -40 degrees.
- Locomotion moves by the loop's own Havok root motion at native playback speed (no averaged speed/smoothing);
  walk/run/dash switches keep the stride phase. Navmesh edges slide (x or z part of the move alone).
- Player turn rate is not in params (online phantom NpcParam rows have turnVellocity 0 -> exe); still 900 deg/s.

## Exe findings: player stamina (2026-10-30)
- Current stamina: int at ChrIns +0x3F8, max at +0x3FC. Setter FUN_140322cf0 clamps to [-50, max] (stamina can go
  negative); other paths clamp current to [-100, max] when max changes (FUN_14034b540 / 140349620 / 140762130).
- Action gate: input handler FUN_140398920 only raises roll/attack requests when stamina > 9 (ESD: f1001() > 10).
  ESD env function f1001 (evaluator FUN_14038b540, case 0x3E9) returns the float at chr +0x3F8 path.
- Max stamina from END: FUN_1402dfcd0 = clamp(int(80 + 80 * (END / 40)^1.4 + 0.5), 80, 160); constants at
  0x141A2B58C (min 80), 0x141A2B590 (max 160), 0x141A2B594 (40), 0x141A2B598 (1.4). END 12 (Warrior) = 95.
  Called from the level-up recalc FUN_1406981a0 (PlayerGameData +0x50 = END -> +0x38 base max stamina).
- Param block: vtable +0x100 returns {.., +8 = NpcParam-layout row}: hp +0x20, mp +0x24, stamina u16 +0xF0,
  staminaRecoverBaseVel u16 +0xF2, turnVellocity f32 +0x0C. PlayerIns::+0x100 = FUN_140365a90 returns *(this+0x7E0);
  ChrIns base returns 0; EnemyIns FUN_14034c710.
- Regen: FUN_14034af40 rate = staminaRecoverBaseVel + SpEffect bonus (FUN_1404034b0, decompiles as a stub).
- Open: which NpcParam row the player uses (PlayerIns constructors FUN_142dcffae / FUN_142842674 are in the second
  .text and decompile with unaffected registers; +0x7E0 not seen yet). Rows 0/1 have stamina 100, regen 40,
  turnVellocity 120. Turn rate, roll cost, dash drain still open.
- Tools added: ghidra/scripts/VtableSlots.java (vtable slots by RTTI name), FindDisp.java (instructions with a
  given mnemonic + displacement), RefsTo.java (references to an address). Single-function decompiles:
  `analyzeHeadless ... -postScript DecompileList.java list.txt out.jsonl 60 N`.

## Character skinning on VU1 (2026-10-30)
- `src/vu1skin.vsm` (dvp-as, loaded at micro address 128 after vu1map at 0): per vertex two bone transforms blended by
  weight, light, then vu1map's projection / CLIP near+guard test / screen cross product cull (ADC) and XGKICK. Two entry
  points from one macro: VU1Skin_Tex (base colour 0x80, texture modulation) and VU1Skin_Flat (per-vertex colour, the
  untextured bare-skin group, PRIM without TME), so the old gsKit path for bare skin is gone.
- EE per frame (`model_draw`): 61 world bone matrices as before, then palette[b] = columns of view * world (col3 =
  translation) with the bone-space light vector R^T L / |L| in col0-2 .w, so one 3-op MADD chain on .w gives n . L.
  Colour = min(tint + base (0.5 + 0.75 max(n . L, 0)), 255) with truncation: bit-identical to the old EE light.
- VU memory for the skin chain: 0-7 camera constants, 8..251 palette (4 qwords per bone, <= 61 bones), double buffers
  BASE 252 / OFFSET 386. vu1_skin_begin sends VIF BASE/OFFSET (OFFSET also resets the double buffer) and the palette
  inline in the chain; vu1_begin resets BASE 8 / OFFSET 496 for the map. Batch: 9-qword header (count + cull, tint,
  (0.5, 0.75, 0, 255), base colour, A+D TEX0/TEX1/TEST, PRIM tag with 4 regs STQ, RGBAQ, XYZ2, NOP) + 4 qwords per vertex
  (output overwrites input in place; the 4th qword goes to the GIF NOP register): <= 31 triangles per batch (100 batches).
- Static streams built once in model_init from model.bin's verts/tris/uvs (no exporter change): per batch slot pa, pb
  (s16x4: bone-local position * 4096, palette offset 4 * bone in .w, read with ILW before ITOF12), nrm (s16x4: normal *
  4096, weight of bone A * 4096) via UNPACK V4-16 with STCYCL 4,1, and st f32x2 (V2-32) or colour u8x4 (V4-8 unsigned).
  393 KB BSS, DMA'd by REF straight from memory.
- Result in PCSX2: CHR pass 7 ms -> < 1 ms (HUD "CHR 0"), VUW stays 0, 30 fps -> 30-60 fps (35-60 in the demo), map
  unchanged. A/B with a forced pose and pinned camera (idle, walk, run, roll with i-frame tint, two R1 frames): the
  character's pixel difference is below the capture noise (notes/skin/ab_fixed_poses.png).
- Gotchas: MOVE is a lower instruction in dvp-as (use addx with VF00x); `.macro` and numeric local labels (1: / 1b) work.

## Player stamina / turn / dash from the exe (2026-10-30, exe-decoding)
- player.c follows notes/EXE_FINDINGS.md: max stamina = END formula (95), regen 45/s x TAE 225 %, dash -1/0.1 s and
  no regen, 3 s dash lockout, int stamina clamped to [-50, max], roll 30 / backstep 20 (a00 680, Circle tap without
  stick; added to export_player.py and export_model.py), turn 180 deg/s walking, 720 otherwise, ROLL_TAP 0.40,
  walk/run hysteresis 0.97 +- 0.01 with tilt / 0.92 as an assumed pad mapping. Supersedes the PROVISIONAL list above.
- Native test harness: `fndb/work/ptest` (gcc with stub model/map/collision; `inc/gsKit.h` fakes the gsKit types).


## Map collision (2026-10-30)
- Source: `map/<area>/h<area>.hkxbhd/bdt` (BXF3, one Havok 2015 TAG0 hkx per collision model; `l...` = low-detail
  set). Each: hkpPhysicsData -> hkpRigidBody -> hkpMoppBvTreeShape -> CustomParamStorageExtendedMeshShape
  (hkpStorageExtendedMeshShape): per subpart a TrianglesSubpart (identity hkQsTransform), a MeshSubpartStorage
  (hkVector4 vertices, indices16 = 4 u16 per triangle `a b c 0`) and a CustomMeshParameter whose materialNameData is
  the subpart material id. Vertices are already in map space. The MOPP tree is ignored.
- Placement: MSB part type 5 (52 in Firelink, all at the origin except h0017B2_0002 and h8000B2_0001). Type data
  byte 0 = HitFilterID (soulstruct CollisionHitFilter): 0-8 solid (8 Normal), 9/16 water, 11/19 NPC only,
  13 death camera, 14 lethal fall, 15 kill plane, 17 group switch, 20/23 level exit. Firelink: 51,644 triangles,
  51,015 solid. Unplaced hkx in the binder (h0012, h0013, h0093, h1000, h7010) are skipped.
- `tools/havok_collision.py` (decoder, MSB collision parts), `tools/export_collision.py` -> `src/collision.bin`
  (DCOL: s16 vertices at 1/128 m around a map origin, 8-byte triangles with hit filter + material, 2 m x/z grid of
  u16 triangle lists; 858,892 B for Firelink; ~0.24 MB less with the `l` set).
- `src/collision.c/.h`: `col_floor` (highest surface under a point in a y range), `col_push_capsule` / `col_move`
  (vertical capsule, horizontal push-out from non-floor triangles = wall sliding, substeps of r/2), `col_walk`,
  `col_ray` (2D DDA over the grid + Moller-Trumbore; camera). Masks select hit filters (`COL_SOLID`, ...).
  Host-checked against Python / brute force (side-session notes `map-collision/`: test_collision.c,
  verify_collision.py, renders).
- Wired in: player.c `constrain()` moves the capsule (r 0.4, h 1.5, step 0.6) with `col_move`, then follows the
  floor within +0.6 / -1.2 m; without a floor the player falls (gravity 9.81, max 30 m/s, provisional) and lands on
  the first floor crossed; crossing a lethal-fall / kill / level-exit plane or y < -250 respawns at the MSB spawn.
  The navmesh constrain stays as fallback when collision.bin is missing. main.c: `col_init(0)`, camera boom pulled
  in to the first `col_ray` hit minus 0.3 m and eased back out at 4 m/s; HUD line `COL x y z W<wall pushes this
  frame> C<boom x10>`.
- The navmesh floats ~0.1 m (median) above the collision floor; the spawn snaps down on the first frame.
- Wall-slide check build: `make EE_DBGINFOFLAGS=-DCOL_WALLTEST` (touch main.c first) forces a run from the spawn at
  yaw 0.6 rad; PCSX2 shows the slide along the north wall to (-33, -57, 72), matching the host simulation
  (notes/firelink-collision-slide.png). Normal demo: 60 fps, ELF 10.28 MB (notes/firelink-collision.png).
- Open: object collision (bonfire etc.), real step/slope/gravity, fall damage/animation, water/death-camera
  behaviour (TECH_DEBT collision-1..7, 9).

## Map simplification and the ground smear fix (2026-10-30, map-simplify)
- export_map.py: quadric edge collapse (tools/simplify.py) replaced vertex clustering. Meshes sharing vertex
  positions are welded and simplified together (no cracks), half-edge collapses keep original positions (no shards,
  flat floors), UV seams kept or torn per corner at a cost; error judged at max(4 m, navmesh distance, 0.6 x spawn
  distance), one global threshold fits BUDGET. Makefile: BUDGET=90000 SPAWN_K=0.6 GRASS_KEEP=16. 145k -> 90k tris,
  ELF -4 MB. Details, budgets 60k/90k/120k, offline renders: notes/simplify/README.md. `export_map.py --render
  map.bin prefix` renders a map.bin offline (z-buffer, textures, PS2 culling); EPS=-1 exports unsimplified.
- Ground smear ("looks off periodically", notes/user-ground-glitch.webp at -40 -56 74): not the map data (offline
  renders of the unsimplified, clustered and QEM maps are all correct there) but the EE clip fallback in render.c.
  Triangles crossing the near plane / guard band were drawn with gsKit's UV primitive: affine, and the 14.4
  fixed-point UV register wraps above 1024 texels, so near floor triangles smeared or became flat bands. The QEM
  floors have bigger triangles, so the camera hit this path more often, and it toggled as triangles moved between
  VU1 and the EE path ("periodically"). r_tri_clip now sends STQ (S = u/w/z, T = v/h/z, Q = 1/z) through
  gsKit_prim_list_triangle_goraud_texture_stq_3d. Before/after: notes/ground-fix-compare.png, ground-{before,fixed}-
  {idle,walk1,walk2}.png (spawn patched to the spot in a test map.bin; demo walk). 60 fps, EE tris unchanged.

## Dash lean fixed: DSR turn-lean pairs (2026-10-30)
- User report: the character leaned to the left while running. Cause: the dash (Circle held, HUD "DASH A00_0535")
  played a00_0535 alone, but DSR's locomotion clips come in turn-lean pairs that WalkAnim_Twist blends by turn rate.
  Head over the feet (mean, + = character left) / mean shoulder roll: 0500 run +0.017 m / -3.6 deg (straight);
  0530 / 0531 run leaning left / right +0.12 / -0.15 m; 0535 / 0536 dash leaning left / right +0.20 / -0.24 m,
  -21 / +13 deg. 0535 and 0536 share the leg cycle and root motion (identical foot phase), only root/spine/arms differ.
- Fix: export_model.py TURN_PAIR {535: 536} bakes the dash as the 50/50 per-bone blend (nlerp + lerp), i.e. running
  straight; anim id stays 535, so player.c and player_data.h are unchanged. Turning still shows no lean (TECH_DEBT char-8).
- Ruled out, with numbers: Havok spline decode (matches SoulsAssetPipeline; loops close, run/walk bodies face -Z with the
  head over the feet), FLVER bind vs Havok reference pose (every weighted bone < 0.2 deg / 0 mm apart), VU1 skinning
  (PCSX2 frame matches the offline pose's outline), quantization. Idle stance is turned -18 deg in the data (DS idle).
- tools/pose_check.py: poses a frame from the hkx + original FLVERs (4 weights, float), renders it with the game camera
  at the spawn, prints the lean (head/neck vs feet/pelvis), overlays the outline on a PCSX2 capture; --blend id for a
  pair. Images: notes/skin/lean_offline.png (0535 / 0536 / blend / 0500), notes/skin/lean_pcsx2_before_after.png.

## Assets from packs, memory budget (2026-10-30)
- Nothing is linked into the ELF any more: proto.elf 10.6 MB -> 0.27 MB (code only). `tools/pack.py` writes per-area /
  per-character packs (`ps2/proto/data/m10_02_00_00.pak` = map.bin + collision.bin, `data/c0000.pak` = model.bin):
  TOC + entries at 2048-byte (CD sector) offsets, each entry LZ4-compressed in independent 64 KB blocks (compressor
  `tools/native/lz4pack.c`, built on demand, every block round-trip checked). map.bin 8.2 -> 3.9 MB, model.bin
  1.28 -> 0.61 MB, collision.bin 0.86 -> 0.72 MB (quantized vertex grid barely compresses); 5.3 MB on disk.
- `src/pack.c`: fio directly (NOWAIT mode) so the IOP reads batch n+1 (<= 256 KB of blocks) into the second staging
  buffer while the EE decompresses batch n; output buffers are memalign(128) and written back from the D-cache, so
  map/model/collision use them in place exactly like the old incbin data (`map_init(blob)`, `model_init(blob)`,
  `col_init(blob)`; formats unchanged). Paths: `PACK_ROOT` = `host:data/` (PCSX2 HostFs, root = the ELF's folder).
- PCSX2 needs `[EmuCore] HostFs = true` for `host:` (set in `~/.var/app/net.pcsx2.PCSX2/config/PCSX2/inis/PCSX2.ini` on
  the host, backup next to it). pcsx2-run.sh already exposes the ELF folder read-only to the flatpak.
- Result in PCSX2: load 0.36 s at boot for 5.0 MB read / 10.3 MB raw, IOP wait 0.00 s (host: reads finish before the
  decompression of the previous batch, so the time is EE LZ4 at ~29 MB/s); demo unchanged at 60 fps
  (notes/streaming-packs.png; HUD line "LOAD .. IO .. RAM .."). From DVD (~2.5-5 MB/s) expect ~1-2 s, read-bound.
- The "Jump to unaligned address" crash with a > 13 MB ELF no longer applies (ELF 0.27 MB); not root-caused (most likely
  the boot ELF loader's own buffer + a 13+ MB image + 3.3 MB bss no longer fitting below the kernel/stack).

EE RAM budget (32 MB; HUD shows the free part live):

| Region | MB | Notes |
|---|---|---|
| BIOS / kernel | 1.00 | 0x000000-0x0FFFFF |
| Code + data + rodata | 0.24 | proto.elf .text 214 KB |
| bss | 3.26 | VU1 DMA chains 2.82 (2 x 88k qwords, sized for 8000 batches; ~1000 used, can drop to ~0.5), skin streams 0.39 |
| Area pack data | 9.06 | map.bin 8.20 + collision.bin 0.86 (+0.10 collision stamps) |
| Character pack data | 1.28 | model.bin (mesh, 9 anims, textures in main RAM for upload) |
| Other heap | ~2.0 | gsKit queues/buffers, pad, newlib |
| Stack | 0.13 | crt0 default |
| **Free** | **15.0** | |

Planned use of the free 15 MB: ESD + params 0.5, enemy characters ~1.3 MB each (4 types = 5.2), second area resident for
transitions / streaming double buffer ~4, sound bank staging on the EE 0.5 (sound data itself in the 2 MB SPU2 RAM /
IOP), reserve 2+. Map data is the lever: map.bin stores float positions + float st (20 B per vertex unindexed);
16-bit fixed positions/st (V4-16 / V2-16 unpack, like the skin streams) would roughly halve it.

VRAM (4 MB): 2 x 640x448 CT16 frame buffers 1.12 MB, Z16 0.56 MB, character textures + CLUTs ~0.45 MB, map
textures + CLUTs ~1.17 MB, free 798 KB. Textures are uploaded per area at load (all resident); per-frame upload of the
visible set (VRAM cache with LRU, GIF PATH3 uploads between VU1 batches) is the next step and is what lets map-2's 128 px
cap go up (TECH_DEBT build-4).

Disc layout later (ROADMAP 24): ISO9660 with SYSTEM.CNF (`BOOT2 = cdrom0:\PROTO.ELF;1`), PROTO.ELF, `DATA\` with 8.3
names (e.g. `M100200.PAK`, `C0000.PAK`; packs are already sector-aligned), `PACK_ROOT` = `cdrom0:\DATA\` and `;1`
suffixes; then replace fio with sceCdRead/sceCdStRead streaming (no IOP file system hop) and order the files on disc by
area.

## ESD runtime (2026-10-31, exe-decoding; TECH_DEBT gameplay-5 resolved, gaps esd-1..7)
- `tools/export_esd.py` packs chr/c0000.esd.dcx (via tools/esd.py) into `src/esd.bin` (191 KB, 82 KB LZ4 in
  data/c0000.pak): groups -> states (entry / exit / while command lists, condition list) -> conditions (target, pass
  commands, sub-conditions, bytecode) -> commands (bank, id, argument bytecode). Layout in the script's docstring.
- `src/esd.c` (interface `esd.h`): both state groups run every frame (group 0 = damage / death reactions, group 1 =
  actions); first true condition wins (a targetless one descends into its sub-conditions); transition = pass cmds,
  exit cmds, entry cmds, then the new state is evaluated again (max 8 hops). Bytecode is a float stack machine;
  `~abort` (0xB7) = false -> condition false, `~continue` (0xA6) = no-op (the `(a ~continue || b ~continue) ~abort`
  pattern rules out any other reading; the exe's evaluator sits in the obfuscated .text).
- Dispatch: `esd_env_move` / `esd_cmd_move` (map-collision, esd_env_move.c) first, then `player_esd_env` /
  `player_esd_cmd` (player.c), unknown env functions return 0, unknown commands are ignored.
- player.c: the pad becomes ESD request slots (R1 0, roll 15, backstep 10, running jump 42): `trig` for this frame
  (f100, f111) and `latch` until the next action starts (f102, f108, f112; cancel windows from the TAE flags).
  c1_3 / c1_4 resolve the anim (weapon-motion file for weaponSel 1/0/-1, a00 otherwise; `key` in player_data.h) and
  start it; anim -1 ends the action; c1_101 switches the engine locomotion (walk / run / dash stay engine code, as
  WalkAnim in DSR); c1_1001 is the stamina change. f110 = MoveParam row 0 escape / step anims (move_escape[]).
  Fall, landing and fall death stay engine-driven (map-collision).
- In PCSX2 (notes/esd-ps2.png): idle / walk / run / dash in group-1 state 0, roll 32, backstep 38, R1 46 -> 48 -> 49,
  rolling attack 41 (R1 during a roll), dash / backstep attack 59. The demo script now keeps the stick held 0.1 s
  after one Circle tap so it shows a roll as well as a backstep (released with the stick centred = backstep).
- HUD line: `ESD <group0 state>/<group1 state> <anim name> <anim key>`.
- Native harness: fndb/work/ptest (test3.c ESD trace, test4.c running jump; stubs.c loads esd.bin from disk).
- Death / fall contract with map-collision (2026-10-31): the engine side sets hp = -1 on a lethal fall; with hp < 0
  the ESD's c1_3 plays as ST_DEAD (group 0: 57 = 6065, 58 = 6066 loop until f201 OnGround, 59 = 6067), and f0
  counts ST_DEAD anims. ST_FALL / ST_LAND stay engine overrides; ESD anims are ignored there while hp >= 0.
  `player_esd_reset()` restarts both groups (for respawn). Makefile has no header deps (build-2): touch the .c files
  that include player.h after changing it (a stale esd_env_move.o read the old Player layout once).

## Falling, landing, running jump, stairs (2026-10-30/31)
- Exe: airborne proxy integration FUN_1402bbe40: |vx|,|vz| <= 6 (0x141A2AE54), -50 <= vy <= 1 m/s (0x141A2AE5C/58),
  v += gravity * dt, gravity = hkpWorldCinfo (0, -9.8, 0) (0x14130B1D0, FUN_1402a36b0) with y scaled by the TAE
  gravity multiplier (ChrActionFlag 27 Ignore Gravity; ChrActionGradient event 300 output 27, FUN_1403410f0 ->
  ctx+0x448 -> proxy+0x1E4). Root-motion reduction (TAE 236, FUN_140343660 / FUN_14037c250): delta *= 1 - clamp(v *
  (1 - mobility)); mobility (PlayerIns vt+0x5A0, FUN_1402e1c20 on equip-load ratio): < 0.5 -> 0.9, 0.5 -> 0.75,
  <= 1 -> 0.85, > 1 -> 0.6 (Warrior 21.4/52 -> 0.9).
- Jump: pad handler FUN_140398920: Circle released while dashing opens 0.2 s (+0x340); a new press = request slot 42;
  ESD group 1 state 36 plays f110(5) = MoveParam dashEscapeF (+0x78, FUN_14052dea0 case 5) = a00_0900, -30 stamina.
  a00_0900 TAE: Ignore Gravity 0-0.1 s, gravity ramp 0->1 over 0.1-0.3 s, Disable Fall Motion 0-0.567 s, root
  reduction 0.8->0.7 (0-0.67 s) / 0.5 (0.67-1.0 s). Its root motion is horizontal only (6 m); the visible hop is in
  the skeleton (pelvis up to 1.48 m).
- Fall anims (engine, not ESD): a00_0760 airborne loop (feet held 0.4-0.56 m up; TAE QueueTransitionAnimation -> 761),
  a00_0761 landing, a00_0610 heavy landing (deeper squat). Fall death is ESD: group 0 states 57-59 (HP < 0 &&
  f283()) 6065 / 6066 loop until f201 (on ground, proxy byte +0x32) / 6067.
- Not found: fall-damage heights (PROVISIONAL 1.2 m no anim / 6 m heavy + damage / 16 m lethal; TECH_DEBT
  gameplay-15). Params involved: NpcParam fallDamageDump (reduction %), SpEffectParam fallDamageRate, CharaInitParam
  red_Falldam.
- Port: player.c constrain()/land()/respawn() (airborne state, landing tiers, fall death, respawn), step easing
  (0.04 s, lag <= 0.12 m) and eased camera follow height (main.c, 0.15 s); src/esd_env_move.c answers f201 / f283.
  tools/export_player.py exports Grad (TAE 300/236 ramps) and flags 27/51; export_model.py bakes 900, 760, 761,
  610, 6065-6067.
- Check builds: `make EE_DBGINFOFLAGS=-DCOL_TEST=n` (touch main.c player.c): 1 wall slide, 2 ledge drop, 3 running
  jump, 4 stairs. PCSX2 results: notes/firelink-ledge-drop.png, firelink-running-jump.png, firelink-stairs-smooth.png
  (HUD `Y <cm> J<feet>/<camera>` = largest per-frame height change in the last second, cm).
- Fall death via ESD (2026-10-31): constrain()/land() only set HP = -1 (past 16 m in the air, or damage on landing);
  ESD group 0 (f1000() < 0 && f283()) runs 57 (6065) -> 58 (6066 until f201 = !airborne) -> 59 (6067); the engine
  side respawns when 6067 ends and calls player_esd_reset(). COL_TEST=5 runs off the east cliff (PCSX2: 6065 starts,
  then the kill plane under the cliff respawns); the 57-59 chain was checked with a host build of player.c + esd.c
  + esd_env_move.c + collision.c on the real esd.bin / collision.bin. collision.c col_move refuses moves >= 20 m (and
  NaN) so a bad velocity cannot spin ~2^31 substeps on the EE.

## Effects, sky, fog and map lighting (2026-10-31, map-simplify)
- Data: param/DrawParam/a10_DrawParam.parambnd (LightBank, FogBank, ToneMapBank per block; Firelink = rows 20-29,
  names "B2..."), decoded with paramdef/paramdef.paramdefbnd (FogBank: fogBeginZ/EndZ m, degRotW = intensity %, colA =
  RGB multiplier %). MSB part entity data: s8 LightID, FogID, ScatterID, LensFlareID, ShadowID, DofID, ToneMapID,
  ToneCorrectID after the entity id (export_map.part_draw_ids). Map pieces: LightID 20/21/23/24, FogID mostly 20; the
  sky part m9000B2 uses fog row 23; FogID -1 is the abyss mist m6100. MTD parameters (mtd/Mtd.mtdbnd: name, type name,
  marker 0xC0/0xC5/0xCA, align 4, u32 count, values; export_map.mtd_params): shafts g_DiffuseMapColorPower 3, mist
  0.7, sky 7, clouds 0.3; NormalToAlpha angles and TexScroll speeds (not used yet, TECH_DEBT map-13/14).
- map.bin (same 72-byte header, padding used): f32 fog begin, end, max density, u8 fog rgb + 0x80, u8 sky fog rgb +
  density; chunk flags bit 1 blend, bit 2 additive, bit 3 sky; colour stream alpha byte = vertex alpha for blended
  chunks; blended textures quantized with alpha in the CLUT. Agreed with vu1-skinning (loading owner).
- Lighting is baked per vertex at export (static map): directional + hemisphere + env term per part row, normalized
  so up-facing = 1.1 under the main row; unlit sky/effects x MTD colour x power x tone-map key/max lum (2.25).
- vu1map.vsm now writes XYZF2 with a fog factor per vertex from constants in the .w slots of constants 4-7
  (vu1_map_fog uploads them mid-chain with FLUSH + STCYCL 1,1 + UNPACK; Z scale x16 because XYZF2 keeps Z 4 bits up).
  116 instructions (skin program starts at 128). GS registers between batches (ALPHA, ZBUF ZMSK, FOGCOL) go through
  vu1_gs_regs: FLUSH + VIF DIRECT (PATH2) so they change in draw order. PRIM FGE/ABE per batch in the GIF tag.
- map_draw order: sky (constant sky fog and FOGCOL, opaque then alpha clouds), opaque map (map fog), effects sorted
  by chunk centre far to near: alpha (mist) then additive (shafts), Z tested not written, then ALPHA/ZBUF/FOGCOL back
  to gsKit's values. Sky and blended chunks skip the 90 m distance cull.
- EE clip path (render.c tri_list): one GS packet per clipped polygon with TEX0/TEX1/TEST/ALPHA/ZBUF/FOGCOL, PACKED
  ST/RGBAQ/XYZF2 (fog factor computed on the EE with the same terms), then ALPHA/ZBUF/FOGCOL restored. map.c rejects
  triangles whose three vertices are outside the same side of the screen frustum before clipping: sky chunks always
  straddle the near plane, and without this their triangles behind the camera took the frame to 30 fps.
- Sky chunks are grouped by 8 azimuth sectors x above/below the horizon (805 tiny batches -> 71).
- PCSX2 (same frame, before -> after): spawn view 60 -> 60 fps, map pass 13 -> 14 ms, EE-clipped tris 557 -> 0;
  view toward the shafts 30 -> 30 fps (20-21k tris), 14 -> 15 ms; cliff view 60 -> 60, 11 -> 13 ms. Screenshots
  notes/fx-{before,after}-{A,B,C,E,walk}.png, grid notes/fx-compare-grid.png; offline render of shafts / mist
  notes/fx-offline-shafts-mist.png. Spots were set by patching the spawn in a test map.bin (scripts in the session
  scratchpad); the real map.bin is rebuilt after each run.
- export_map.py --render now draws blended chunks (layered per pixel) and fog like the PS2, for offline checks.

## HP and damage core (2026-10-31)
- Max HP: FUN_1402df9a0 = CalcCorrectGraph row 100 ("最大HP") at VIT via the generic lookup FUN_1402dfb70 (stage by
  stageMaxVal, growth t^e for e >= 0, 1 - (1 - t)^-e for e < 0). Warrior VIT 11 -> 594.
- Weapon AR: FUN_1402dacd0 stat factor 1 + graph(stat)/100 x correct/100 (CalcCorrectGraph row = correctType),
  STR and DEX combined as (a-1)+(b-1)+1 in FUN_1402d9cb0. Longsword 80, STR/DEX 13, 51 %: AR 92.65.
- Defence: FUN_1402d9890: r = atk/def (def 0 -> 8), cut % quadratic through (0.12, 90) (1, 60) (2.5, 30) (8, 10),
  damage = atk x (1 - cut/100) (blend weight DAT_141C74E28 = 0). R1 on NpcParam 206000 (def 71): 47.
- Fall damage (player): PlayerIns vtable+0x2A8 -> FUN_1402df3a0: 0 under 5 m, 1.0 from 20 m, else (h/20)^2 x
  FUN_1402df580 (equip weight w: v = clamp((1 - w/100) x 100, 1, 100), linear (1,1) (10,1) (30,0.95) (50,0.9)
  (100,0.7)); Warrior 21.4 -> 0.786. Applied as max HP x rate. NPCs: FUN_1402df420 = (h/15)^1.8 x (100 -
  NpcParam.fallDamageDump)/100.
- Hit shapes: AtkParam_Pc 2300000/10/20 (Longsword R1 chain, BehaviorParam_PC refId): hit0 capsule dummy polys
  100 -> 110 r 0.4, hit1 sphere 120 (or 110) r 0.2, atkPhysCorrection 100, dmgLevel 1. Dummy polys from the weapon
  FLVER (root bone), flipped like the mesh (-x, -y, z) into R_Weapon space; combat.c rebuilds R_Weapon from
  model_sample at the attack time and places it as model_draw does.
- Code: src/combat.c/.h (registry of CombatChr hurtboxes, combat_sweep / combat_apply, poise, the player glue
  combat_player_frame), generated src/combat_data.h (tools/export_combat.py). Enemies register a CombatChr (API
  sent to the memory/streaming thread); the HUD reads Player.hp / hp_max.
- Checks: host (combat.c alone): HP 594, AR 92.65, R1 47, fall 5/10/15/20 m = 29/116/262/594. PCSX2:
  `make EE_DBGINFOFLAGS="-DCOL_TEST=6 -DCOMBAT_DUMMY"` (R1 x2 on the dummy: 203 -> 156 -> 109,
  notes/combat-r1-dummy.png), COL_TEST=2 (5.3 m drop: heavy landing, 594 -> 562, notes/combat-fall-damage.png),
  COL_TEST=5 (lethal fall: FALL DEATH 6065 at HP -1, respawn with 594, deaths D1,
  notes/combat-fall-death-respawn.png). COL_TEST builds turn the Select debug overlay on.

## HUD and Start menu from DSR's assets (2026-10-31, map-simplify)
- menu/menu.drb = the game's UI layout (1280x720). Chunks {char[4], u32 size, u32 count, pad to 16}: STR (UTF-16),
  TEXI {u32 name, u32 path} 16 B (texture list: Icon00, Menu01, Menu04, Menu08, ...), SHPR (shape parameters),
  SHAP {u32 type, u32 SHPR offset}, DLGO {u32 name, u32 SHAP, u32 CTRL, ...} 32 B, DLG {u32 name, ..., +32 element
  count, +36 DLGO offset} 64 B. Shape parameters: s16 left, top, right, bottom relative to the parent dialog; Sprite
  +16 s16 texel rect, +24 s16 texture index; MonoRect +24 colour a,r,g,b; GouraudRect +20 four corner colours.
  In-game HUD = dialog F20-00 (bar_0/2_hp/sp, HP_/SP_endbar, bar_BG, F20-00_item / arms_l / arms_r slot dialogs,
  soul_param), YOU DIED = F23_YouDied (Menu14 text sprite, backdrop, band, gradients).
- Item icons: IconNN sheets, 160x180 cells on a 165x184 grid, 12 x 11 = 132 per sheet; sheet "Icon<id // 1000>
  <(id % 1000) // 132>", cell (id % 1000) % 132 row-major (rectangles = the DRB sprites on that sheet).
  EquipParamWeapon/Goods iconId, EquipParamProtector iconIdM.
- Font: font/english/DSFont24.ccm: header (u16 line height 58 at +8, texture 1024x1024, u16 code-group count at
  +0x10), code groups {u32 first, last, first glyph} 12 B at 0x20 (later groups repeat codes: first one wins), then
  glyphs {f32 u0, v0, u1, v1; s16 pre-space, width, advance, page} 24 B; page = index into the .tpf's texture order.
- Text: msg/ENGLISH/item.msgbnd (Weapon_/Armor_/Item_name_), menu.msgbnd (Ingame_menu_ 110000 Inventory, 110001
  Equipment, 110003 Status; Menu_general_text_ 101013 Level, 101030 HP, 101032 Stamina, 101041 Souls).
- Warrior kit: CharaInitParam 3000 (Longsword 201000, Heater Shield 1450000, Standard Helm / Hard Leather set
  210000-213000, goods 117 Darksign, 103 Black Separation Crystal, soul level 4, souls 0); Estus Flask added.
- tools/export_menu.py -> src/menu.bin (packed as data/menu.pak): sprites resampled to their exact 640x448 size
  (x 0.5, y 0.622), packed tallest-first with the 95 ASCII glyphs (17 px lines) into one 512x256 PSMT8 atlas
  (256-colour CLUT with alpha), long bars and YOU DIED stored at half size and stretched; 9 icons (64 px) in a
  512x128 atlas; layout table, glyph table, strings. 194 KB VRAM.
- src/hud.c: bars (frame length = max / cap, fill = current / max, tinted), quick slots, item name + count, souls,
  YOU DIED fade while pl.hp < 0; gsKit sprites with alpha blending, alpha / depth test off around the HUD. src/menu.c:
  Start opens a read-only Equipment / Inventory / Status screen (icons, names, live HP / stamina), d-pad cursor,
  Start or Circle closes; movement and the demo are blocked while it is open. Select toggles the old debug overlay
  (fps, VU1, collision, load), off by default. Screenshot switches: -DMENU_TEST, -DDIED_TEST, -DHUD_DEBUG, -DNO_HUD.
- PCSX2 (same tree, same frame): HUD drawn vs compiled out (-DNO_HUD) both 30 fps, map pass 14 ms, 682 VU1
  batches: no measurable cost. (The spawn view was 60 fps earlier on 2026-10-31; the drop came with other changes in
  the tree since, e.g. the enemy / dummy, and shows with NO_HUD too.) Screenshots notes/hud-play.png, hud-walk.png,
  hud-menu.png, hud-youdied.png, hud-fps-hud-{on,off}.png; atlases notes/hud-atlas.png, hud-icons.png.

## ESD runtime, exe-accurate pass (2026-10-31, exe-decoding; TECH_DEBT esd-4 / esd-5 resolved, esd-1 / esd-2 updated, esd-8 new)
- Per-instance API (esd.h): `EsdProgram` per packed .esd (`esd_load`), `EsdCtx` per character (`esd_ctx_init(ctx, prog,
  ESD_PLAYER | ESD_ENEMY, chr, env_hook, cmd_hook)`, `esd_start`, `esd_update`, `esd_state`). esd_env_move /
  esd_cmd_move run first for every context. Enemies all share chr/enemyCommon.esd.dcx (`export_esd.py <game> <out>
  enemyCommon`; the mNN.chresdbnd files only hold dummy.esd).
- Evaluation, from the exe: FUN_140388020 (the chr's ESD update) steps each group machine once per frame
  (FUN_140e5d000 with a step count of 1). A step evaluates the current state's conditions in order (FUN_140ea0ff0 /
  FUN_140ea0d10); on a match: the state's exit commands, the condition's pass commands, the new state's entry
  commands; a true targetless condition without sub-conditions re-enters the same state. The new state is evaluated on
  the next frame. While commands run every frame. Bytecode: handler table at 0x141A8C620; 0xA6 is a bare RET,
  0xB7 (FUN_140e9c6c0) on a false value pushes 0 and ends the expression.
- Requests, from the exe (FUN_140388020 -> FUN_140388790 each frame before the machines; TAE event 0 handler
  FUN_140340580): the playing anim's ChrActionFlags set per-slot "Key Input" windows (ctx+0x1A0) and "Anim Cancel"
  enables (ctx+0xF2) plus bit sets ctx+0xA0 / +0xA4 (player.c apply_flag has the whole table). A press latches
  (ctx+0x468) only inside its Key Input window (or while c1_146 accept-all lasts); latches clear on every new ESD
  anim and whenever stamina < 1. f100 = this frame's press through the stamina gate FUN_140323c60; f102 / f108 = latch
  && enable; f111 = any press this frame; f112 = any slot actionable. export_player.py now exports every
  ChrActionFlag window.
- Consequences visible in play, all as in DSR: an R1 pressed before an attack's Common Key Input window (flag 87,
  0.5 s into a23 3000) is dropped; a latched request nothing consumes (a roll released at 10 stamina) falls through
  group 1 state 0 to the guard states 75 / 76 (f103 returns 1.0 / 0.0, so `f103(21) >= 0` is always true); guard
  anims a00 100 / 101 / 106 / 116 are exported (f278 = guardmotionCategory). (Before 2026-10-31 an R1 at stamina < 1
  did too; f111 is stamina-gated in the exe, see "Air R1 / plunge".)
- Group 0 is the airborne / damage layer: f200 (FUN_140386850, "should fall") enters the fall states 14 / 15 (a00 1550
  / 1500) and the plunging attack 19 / 20; map-collision owns f200 / f201 and the fall.
- Checks: native harness (fndb/work/ptest: test3 full flow, test8 early press + guard, test9 the ESD_TEST script);
  PCSX2: demo (notes/esd-ps2.png: idle, walk, run, dash, roll, backstep, R1 chain, rolling attack) and the
  `make EE_DBGINFOFLAGS="-DESD_TEST -DHUD_DEBUG"` script build (notes/esd-ps2-actions.png: backstep -> backstep attack
  59, dash -> dash attack 59, dash -> running jump 36). Guard (76) is checked natively only (needs stamina < 1).
- Fall through the ESD (2026-10-31): DSR's fall is ESD group 0, not engine anims. f200 (FUN_140386850: proxy not on
  ground, Disable Fall Motion clear) -> state 14 (a00_1550) -> 15 (a00_1500 loop); on f201 the landing is picked by
  f224 (FUN_14037ac30: proxy +0x114 - +0x14 = height fallen) and f263 (PlayerIns vt+0x558: equip load %) in three
  load tiers (> 100 / > 50 / > 0): states 31/32/33 (1750/1755/1760 via c1_12; +f246 variants 1765-1767), 34 (1790),
  97 (6070 -> 6170: f224 >= 20 or HP < 0), 99 (6073 -> 6173: HP < 0 under 10 m). The earlier engine stand-ins
  (760 loop, 761/610 landings) are no longer used; constrain()/land() keep the physics, fall damage and respawn.

## First enemy: graveyard skeletons c2900 (2026-10-31)
- Picked c2900 ("skeleton, curved sword", NpcParam 290000 HP 115, ThinkParam 290003/290004): the hostile MSB enemy
  type of Firelink (11 placements in the graveyard, 55-110 m from the bonfire) with a plain humanoid skeleton (59
  bones, fits the 61-bone VU1 palette), sword + shield and simple melee AI. The c2540/c2550 parts (crows on the roofs)
  and Frampt / Petrus / NPC c1000 are not standard melee enemies.
- Data: tools/export_chr.py -> data/c2900.pak = model.bin (export_model.py's writer, shared via MeshBuilder /
  write_model; c2900.flver LOD1 1744 tris, body texture from c2909.chrbnd = NpcParam normalChangeTexChrId; meshes
  "#NN#" shown by NpcParam modelDispMaskNN: sword #00# + shield #01#, bow / greatsword hidden) + chr.bin (NpcParam /
  ThinkParam values, per anim: length, root motion, TAE windows: ChrActionFlag 5 parry, 7 no turning, 23 AI combo
  window, AttackBehavior -> BehaviorParam 2_<variation>_<judge> -> AtkParam_Npc atkPhys / dmgLevel / hit0 dummy-poly
  capsule in R Weapon space, from the FLVER dummy table). tools/export_enemies.py -> enemies.bin in the area pack
  (MSB part type 2: +0x08 ThinkParam, +0x0C NpcParam). enemyCommon.esd (every DS1 enemy's ESD) -> data/common.pak.
- model.c is multi-instance: Model per character type (mdl_load / mdl_upload / mdl_sample / mdl_draw), any number of
  draws per frame; vu1_skin_begin now starts with FLUSHE so each character's palette waits for the previous one's
  last batch. The player keeps the old model_* calls (wrappers). Also fixed: the anim table was capped at 16, the
  player's model.bin has 18.
- Enemy frame (src/enemy.c): combat sync -> AI (src/ai.c) -> ESD (enemyCommon via the per-instance esd.h API, hooks
  enemy_env / enemy_cmd: c1_0 / c1_3 play anims, c1_101 locomotion) -> anim time, root motion of the action anim or
  walk / run at their own root-motion speed over col_walk -> turning (turnVellocity 180 deg/s, not in TAE flag 7)
  -> pose -> combat_sweep of the active AttackBehavior windows. Capsules pushed apart (enemy vs player / enemies).
- How the ESD is driven (from tools/esd.py dumps of enemyCommon; env semantics from exe-decoding's evaluator
  decompile): the AI writes one request (exe ctx+0x528: attack 3000-4112 or step 700-723, -1 none). f106 returns it
  always (group 1 state 0 -> one state per anim, c1_0(anim, 6, 0.1, 0)); inside an attack f120 returns it only in the
  TAE flag 86 window (combo 3000 -> 3001 -> 3002) and f118 only in flags 11 / 79 (steps); f0(0) = anim ended -> back
  to state 0. A hit stays visible (f206 == 0) until group 0 changes state: with one step per group per frame a
  one-frame flag was missed and a killed skeleton stayed standing. Group 0 handles hits: on
  the hit frame f206() == 0 opens the damage branch, f236 = damage level, f222 direction -> states 19-27 (2000-2014),
  f1000() < 0 -> death states 33 / 35 (2200 / 2205).

### Enemy AI
- DSR AI = compiled Lua 5.0 chunks ("\x1bLuaP", size_t 8, doubles) in script/*.luabnd: aiCommon (goal / logic lists,
  common_func*.lua: Approach_Act, Common_Battle_Activate, ...) and per map (NNNNNN_battle.lua / _logic.lua by
  ThinkParam battleGoalID / logicId). They call the AI object (GetDist, GetRandam_Int, IsLadderAct,
  GetTeamRecordCount, IsInterupt...) and push engine goals (GOAL_COMMON_ApproachTarget / Attack / ComboAttack /
  ComboFinal / ComboRepeat / SpinStep / LeaveTarget / SidewayMove), which the C++ engine runs. tools/lua50.py
  disassembles them (and lists globals / methods per file with --calls).
- Approach chosen: offline translation to C (src/ai.c), not a VM: one battle goal is ~400 lines of Lua but needs the
  engine's goal system either way; a VM would add the interpreter plus ~100 AI functions. 290003_battle.lua:
  Activate odds by distance (>= 8 m: acts 1/2/3/7 = 10/15/5/70; 3.5-8 m: 15/20/15/0 + 30/20; 2.5-3.5 m: 20/35/20/0
  + 0/25; < 2.5 m: 10/30/20/40), Act01 approach 3.1 + 3004, Act02 approach 2.2 + 3000 / combo 3000-3001(-3002),
  Act03 approach 2.2 + 3003, Act04 approach 2.5 + 3005, common act 7 approach 6.5 + 3008, AdjustSpace after acts
  1-3 (spin steps 702 / 703, leave + sideway walk). Approach_Act walks below range + 2 m, runs beyond.
- Result in PCSX2 (test build `make DEFS='-DHUD_DEBUG -DTEST_SPAWN=-41.3f,-61.5f,123.5f,0.0f'`, player 6 m south of
  skeleton c2900_0000): the skeletons idle at their MSB spots, hear the player, approach, attack (3000/3001 combos,
  3008 lunges), take the player from 594 HP to death; with `-DENEMY_HIT_TEST` (a hit every 2 s) the hit reaction
  (state 21, 2002) and death (state 35, 2205) play. Screenshots: notes/enemy/1-6*.png, sequence.png. Game fps on the
  HUD stays 60 with up to 9 skeletons drawn (EE: MAP 2-8 ms, CHR 2-3 ms, VUW 0); PCSX2 itself runs at ~70-85 %
  speed on this host in every scene now (also with no enemy in view), so its window fps is lower.
- Memory: data/c2900.pak 0.72 MB on disk; 1.99 MB model + 24 KB chr data in RAM, ~0.3 MB skin streams; textures
  ~200 KB VRAM (free after: 370 KB with the HUD atlases); EE RAM free 11.9 MB in the test build.

## Map pass back under budget: 60 fps (2026-10-31, map-simplify)
- Symptom: 30 fps in the heavier demo views (spawn 41 fps, toward the shafts and the wide view 30) with a 12-15 ms
  map pass (EE time to build the map's VU1 chain). Profiled with `make EE_DBGINFOFLAGS="-DHUD_DEBUG -DMAP_PROF"` (the
  VU1 / EE overlay numbers become visibility / EE-clip time in 0.01 ms): ~3.9 ms of chunk sphere tests (all ~5700
  chunks every frame) and ~7.4 ms in the EE clip check. The HUD, fog and VU1 were not the cause (HUD on / off same).
- The clip check ran over every triangle of every chunk whose sphere touches the near plane or the guard band:
  three non-inlined view transforms + building 8-float clip vertices before the VU1-safe test. The sky chunks (huge
  spheres, always "touching") and the large mist planes added ~1900 triangles of that per frame.
- Fixes in map.c: sky chunks skip the EE check (at 1-3 km nothing in view needs clipping; what VU1 drops is outside
  the 43 degree view); culling index built on the first draw (non-sky chunks grouped into 32 m cells of their
  centres, one sphere test per cell, chunks only in visible cells; sky chunks in their own list); the clip check
  transforms inline from a per-frame camera basis and builds clip vertices only for triangles that need clipping.
- Same tree, same frames (before -> after): spawn 41 fps / 13 ms -> 60 / 7 (2.8 ms visibility, 2.4 ms clip); toward
  the shafts 30 / 15 -> 60 / 9; cliff 60 / 13 -> 60 / 5; wide view 30 / 12 -> 60 / 7; demo walk 60 fps (6-7 ms).
  Screenshots notes/milestones/perf-20261031-0101-{A,B,E}-{before,after}.png.
- Makefile: src/.cflags stamp + -MMD header dependencies (TECH_DEBT build-2 resolved).

## Air R1 / plunging attack (2026-10-31, ESD runtime; TECH_DEBT esd-10 resolved, esd-9 new)
- Bug (map-collision, COL_TEST=7): R1 on the first airborne frame sent group 1 to 76 GUARD. Cause: f111 was not gated
  while f100 was gated by "no ESD anim playing", so group 1 state 0 saw a request but no R1 and fell through to guard.
- Exe: gate FUN_140323c60(chr, b) = chr+0x3F8 >= 1 && (b || !FUN_14032c4f0(chr)); FUN_14032c4f0 returns 1 only while
  the chr has partner handles (+0x16C..+0x178 != invalid 0x20FFFFFFFF at 0x141A2BC90: throws). f100 passes ctx+0xF2[slot],
  f111 (FUN_140386bb0) passes ctx+0x49E (any slot enabled). With no throws in the port both reduce to stamina >= 1
  (player.c gate()). Behavioural check: c0000 group 0 15 -> 19 tests f100(0) during a00_1500, which has no Anim Cancel
  window, so the gate must be open there.
- c0000 air R1: state 14 -> 19 needs f298 (ChrActionFlag 84, a00_1550 from 0.133 s), 15 -> 19 needs only f100(0);
  both need f207 = 1 and f225(1) not 41/43/44/46. 19 = a23 3800 (layer 6), 20 = 3801 loop, landings 21-23 = a23 3810
  (not exported, esd-2), 31-34 / 93 / 97 / 99 as the fall. An R1 before 0.133 s is dropped by group 0.
- Group 1 sees the same R1 (state 0 -> 46, c1_3(1, 3000, 6)). Layers are anim-player slots (FUN_140385550, tables
  0x14132D000 / 0x14132D030: 0 1 2 3 5 6 7 8 9 11 10 and 15 16 20 21 17 18 23 24 19 26 25 27 28). ASSUMED (esd-9): a
  group cannot replace or stop the other group's ESD anim on a higher or equal layer (EsdCtx.group says which group runs;
  Player.esd_layer / esd_group remember the playing anim's); group 1 waits in 46 until f0, without anim or stamina.
- Checks: native fndb/work/ptest test10 (R1 in 15 -> 19 -> 20), test11 (R1 at 0.3 s: 14 -> 19), test12 (R1 at 0.05 s:
  fall continues, group 1 46 waits); test3 / 8 / 9 ground flows unchanged except test8 R1 at 0 SP (idle, was guard).
  PCSX2 `make EE_DBGINFOFLAGS="-DCOL_TEST=7 -DHUD_DEBUG"` (R1 0.3 s into the west-ledge drop; a HUD line under the ESD
  line lists every ESD state change): 0/0 > 14/0 > 19/46 > 23/0 > 0/0 (notes/milestones/esd-runtime-20261031-plunge-path3.png).
- `player_esd_state(group)` (player.h) for test scripts.

## Bare-skin texture (2026-10-31, char-skin; TECH_DEBT char-1 resolved, char-10 new)
- The body under the armour (material 体, Ps_Body[DSB].mtd, diffuse slot "BD_M_body") is in BD_M_9360 (neck/chest) and
  AM_M_9360 (one mesh each), 72 LOD2 tris. Its textures are in the loose, uncompressed `parts/Common_Body.tpf` (11 MB TPF, not a
  bnd): BD_M_body / BD_F_body 1024 px DXT1 + _n (DX10) / _s / _t, FC_M_face / FC_F_face 512 px, BD_A_underwear*.
  `parts/Common_Body_M.tpf` holds BD_M_body_M for the [M] (multi-texture) MTD variants. Head parts' FC_M_face meshes
  will find their texture there too.
- BD_M_body is already skin-coloured (avg 185/141/111); BD_M_body_t is plain white. Ps_Body MTDs carry g_FaceGenType,
  so DSR probably tints skin by the character-creator FaceGen colour at runtime; the Warrior has no param face
  (CharaInitParam 3000 npcPlayerFaceGenId -1). ASSUMED: texture used as-is (char-10).
- export_model.py: `d = tex.get(diffuse) or common.get(diffuse)` with `common = tpf(parts/Common_Body.tpf)`; the mesh
  becomes a normal textured group (256 px PSMT8, CSM1 CLUT) and draws through VU1Skin_Tex. The Warrior no longer has
  an untextured group; VU1Skin_Flat / SKIN stay for meshes whose diffuse is found nowhere.
- Cost: +1 texture (7 of model.c MAX_TEX 8; CLUTs are preallocated, so only the 64 KB pixel page), VRAM free 370 K ->
  306 K, model.bin 2.09 -> 2.16 MB, EE RAM free 11.6 -> 11.5 MB, 60 fps unchanged.
- Close-up test build: `make EE_DBGINFOFLAGS='-DCHAR_CLOSEUP=yaw,dist,pitch'` (main.c: no demo input, camera dist m in
  front of the player rotated by yaw rad), e.g. 0.35,0.9,0.0 for the neck. Before/after:
  notes/milestones/char-skin-20261031-0115-{close,neck,before,after}.png.

## Player head and face (2026-10-31, char-skin; TECH_DEBT char-11 / 12 / 13 new)
- DSR builds the player's head from three parts, all in `parts/`: FC_M_0000 (back of head + neck, material "#15# 後頭部",
  texture FC_M_face from Common_Body.tpf, 664 tris at every LOD), FG_A_0000 (FaceGen face "#12#" / "#13#" + eyes; same
  file as facegen/FaceGen.fgbnd's face.flver) and the hair HR_M_<hairStyle> (HR_M_0000 = bald, no meshes). Bones: FG
  and HR carry their own Head bone (parent -1, world bind) plus eye bones / nubs; MeshBuilder walks the unknown ones
  up to Head, so the whole head is skinned to c0000's Head on VU1 like the armour.
- facegen/FaceGen.fgbnd (BND3): CustomizerData si.ctl, face.tri / face.egm (geometry modes), face.egt (50 texture modes
  at 128x128: the size fits a 64-byte header + 50 x (f32 + 3 x 128 x 128 bytes); not decoded), eye.tri / eye.egm; GameData face.bmp (128 px
  front-projected FaceGen mean texture), face_m.bmp (the Hollow variant), x64/face.flver, face.flver2tri,
  facegen.tpf (FG_eye 128 DX10, FG_m_face_org_d / FG_f_face_org_d 512 DXT1 detail diffuse, normal / spec maps, a 16 px
  placeholder FG_face_gen). FaceGenParam rows carry faceGeoData00-49 / faceTexData00-49 (bytes) + hair / eye colour;
  row 30 "４末版　戦士_男" is a Warrior-male preset (hair 1, colour 160/130/80). CharaInitParam 3000 has
  npcPlayerFaceGenId -1 (the face comes from the save).
- The face FLVER's vertices hold a UV pair (type 0x16): set 1 = FaceGen projection (face.bmp / the generated
  FG_face_gen), set 2 = FG_m_face_org_d. Shipped: org detail on set 2 only, mean geometry (char-11). souls.flver now
  returns `uv2`. FG_A_0000's eye buffer is 36 bytes but its layout puts the UV at 36 (it lists a tangent the buffer
  lacks): souls.flver reads the last 4 bytes then. The eye-highlight quad (M[D]_Alp) is skipped.
- Hair: grey alpha texture tinted by the preset's hairColor offline (ASSUMED x c / 255), quantized with a transparent
  index (export_map.quantize_rgba, 256x128) so the skin batches' alpha test (GEQUAL 0x40) cuts the cards (char-12).
- **Hide rule** (DSR): a mesh whose material name starts with #NN# is hidden while any worn protector has
  EquipParamProtector invisibleFlagNN. Warrior (CharaInitParam 3000: 210000 / 211000 / 212000 / 213000): helm hides
  0 1 4 5 8 11 31 (hair "#04# 側頭部"), armour 16 18 19 25 26 32 33 37 39 40 42, gauntlets 43 45, leggings 35. So the
  two LG_M_9360 "#42#" meshes were wrongly drawn until now; the exporter drops them. Face, back of head and the other
  hair cards stay drawn under the helm (as in DSR: the face shows through the visor slit).
- model.bin group flags: bit 1 = helmet mesh, bit 2 = hidden while the helm is worn; model.c `mdl_hide(m, bits)` skips
  groups with a shared bit, `model_helmet(on)` (on = hide 4, off = hide 2), on by default. Debug: L3 toggles the helm;
  `make EE_DBGINFOFLAGS='-DHELM_OFF -DCHAR_CLOSEUP=0,0.8,0.05'` starts with it off for face close-ups.
- Cost: 2,960 -> 6,124 player tris (face LOD1 1,038, back of head 664, hair 1,532 at LOD2 / LOD1 where LOD2 is missing),
  11 textures (MAX_TEX 8 -> 12; the CLUT block is now ntex KB), VRAM free 306 K -> 197 K, model.bin 2.16 -> 2.48 MB,
  60 fps (HUD 14k tris in the spawn view); EE RAM -0.7 MB (A/B on one tree: 11.0 -> 10.3 MB free; model.bin +0.32 MB,
  VU1 skin streams ~0.35 MB). Close-ups: notes/milestones/char-skin-20261031-0130-*.png.

## Breakable objects / destruction (2026-10-31, destruction thread; TECH_DEBT physics-1)
- Data: a breakable MSB object's obj/<model>.objbnd carries a broken state next to the intact one: <model>_1.flver
  (pre-fractured mesh, one root bone per piece, rigid bone-local vertices, no weights) and <model>_1.hkx (Havok 2015
  TAG0: one hkpRigidBody per piece, motion type 3 = box inertia, hkpConvexTranslateShape -> hkpBoxShape, convex radius
  ~0.01-0.05, massInv 0.02 = 50 kg, inverse inertia per axis, hkpMaterial friction 0.5 restitution 0.4). The intact
  <model>.hkx is one fixed body (motion type 5) with an hkpCylinderShape (pots: r 0.48 m, 0..1.63-1.67 m). FLVER piece
  bones and Havok bodies share names (_01.._29) and rest transforms (export_objects.py asserts it).
- Firelink: o1495 (mimic pot "擬態の壷", 7 parts, 29 pieces), o1496 (8, 32), o1497 (10, 29); nothing else at m10_02
  has a broken state. ObjectParam 1495-1497: hp 1, defense 0, isBreakByPlayerCollide 0, isChrHit 1, isCamHit 1,
  breakSfxId none / 33554431. The pot diffuse m10_obj_pot3 is in map/m10/m10_9999.tpf.dcx (area-shared TPF).
- Exe: `map:/breakobj/%s.breakobj` (FUN_140263cd0, FUN_1403da550 loads it with the MSB) persists broken objects per
  map; `IsTouchBreakableObject` / `SetObjDisableBreak` / `ReconstructBreak` are event functions. The break simulation is
  Havok's (hkpRigidBody per piece); its spawn impulse and fade timing were not found (ASSUMED values below).
- tools/export_objects.py -> src/objects.bin (area pack): per type one model.bin (bone 0 intact, bones 1..n pieces;
  group bits 8 intact / 16 broken), piece rest transforms, box half extents (+ convex radius), centre, mass / inertia,
  cylinder; per MSB part the placement. export_map.py skips the same objects (load_object: `_1.hkx` present).
- src/objects.c: intact pots drawn with mdl_draw (identity placement, bone pose = world transform; mdl_hide picks
  intact / broken groups), within 40 m in front of the camera. Breaks: every combat_sweep (combat.h
  `combat_sweep_hook`, so enemy weapons too) whose capsule reaches the cylinder with damage >= hp; the player rolling /
  backstepping (ESD group 1 states 32-35, 37-40) or dashing into one (`player_breaks_objects`); landing on one
  (airborne, vy < -2 m/s). Otherwise the player capsule is pushed out of intact cylinders (collision-1 for breakables).
  Pieces: v = 2.0 m/s out from the axis + 2.5 m/s along the hit + 0.75-1.5 m/s up, spin up to 6 rad/s (ASSUMED);
  live 8 s, then sink 0.5 m over 2 s and vanish (stand-in for DSR's fade); 96-body budget (oldest object's pieces go
  first). Broken pots stay broken until the area reloads.
- src/physics.c: boxes vs the map: per body and frame one col_floor (floor plane under the box centre) and one col_ray
  (walls: non-walkable hits only), 2 substeps of gravity / integration and 4 iterations of accumulated, clamped
  corner impulses (normal with restitution above 1 m/s, Coulomb friction including the corner's weight share), push-out
  by the deepest corner, 1 cm contact slop. Body-body: spheres (0.6 x the largest half extent), only while one of the
  two moves faster than 1 m/s, never within one pot for its first 0.3 s. Sleep after 0.4 s touching under 0.3 m/s and
  1 rad/s; another break within 2 m wakes sleepers. Host check (fndb/work/ptest/obj/objtest.c, real collision.bin /
  objects.bin, o1497_0004 broken westwards): 29 pieces, 58 map queries / frame while flying, all asleep after 3.5 s,
  resting 1-7 cm above the floor (lying flat), 3 pieces fell off the ledge to the level below.
- EE RAM budget (coordinator: < 0.3 MB): FLVER LOD2 for intact pots (96-124 tris) and pieces (176-226 per pot), one
  128x128 texture per type (PSMT8 needs width >= 128): objects.bin 151 KB + mdl_load VU1 skin streams 117 KB (3072 B
  per batch of <= 31 tris) + pools ~24 KB (96 bodies 11.9 KB, instances 6.6 KB, phys scratch 3.2 KB, pose 1.8 KB) =
  0.279 MB (+ ~4 KB for the per-body floor caches added since). LOD1 intact pots (292-380 tris) would be 0.384 MB.
  export_objects.py prints the total.
- EE cost (PCSX2, OBJ_TEST=1, HUD `.. US`): first version 9.4 ms with 29 pieces awake (26 map queries at ~180 us each
  + ~160 us solver per piece) = 32-35 fps. Now: one substep, fixed-tangent friction with precomputed effective
  masses, sqrt.s, pieces start with the pot's floor plane (1 query per break), floor re-query after 0.3 m sideways,
  staggered wall rays every 3rd frame, a budget of 8 map queries per frame (round-robin), a floor check before a
  piece sleeps: 2.9 ms with 29 / 29 awake, 0.6 ms with 12, 0.1 ms settled; 60 fps throughout
  (notes/milestones/destruction-20261031-0218-*.png: intact, mid-flight, settled).
- Test build: `make EE_DBGINFOFLAGS='-DOBJ_TEST=n -DHUD_DEBUG'` (1 walk to the nearest pot + R1, 2 roll into it, 3 walk
  into it); OBJ_TEST builds skip the enemy roster. HUD line `OBJ <instances>/<broken> PC <pieces>/<awake> Q<queries>`.

## Foliage density and area fog (2026-10-31, foliage-fog; TECH_DEBT map-12 / map-15 updated)
- **Foliage "looks broken"**: the old thinning kept triangle pairs `(i // 2) % 16 == 0`. Firelink's foliage is tufts
  of 4-32 triangles, many as back-to-back card pairs (two-sided by geometry, cull flag on), so the cut left single
  quads that were invisible from behind, plus isolated ivy leaves: sparse dark strands. Lighting, alpha and cull were
  fine (offline full-density render notes/milestones/foliage-fog-*-offline-full-vs-port.png).
- Now (export_map.thin_foliage): tufts = triangles connected by shared positions, each with a stable hash u. A tuft
  is kept if u < 1/GRASS_KEEP (14; ivy, single leaf quads, 1 in 7), and its rank is u x GRASS_KEEP. Kept tufts are
  widened about their base (FOL_XZ 2.2 sideways, FOL_Y 1.25 up; ivy FOL_IVY 1.6) to make up part of the coverage.
  GRASS_KEEP 4 (57.7k tris, map.bin 11.5 MB) looked close to DSR but broke the area RAM budget (map.bin <= 8.5 MB).
  prune_foliage drops tufts that could never get close enough to be drawn (navmesh distance - 6 m camera reach).
  Foliage no longer counts in BUDGET (76000 = the old 90000 minus the old 14010 grass: the rest of the map is
  unchanged). Firelink: 17.0k foliage tris stored (DSR 226k), map.bin 8.2 -> 8.47 MB RAM (breakable pots now outside it).
- Chunks: foliage grouped per (texture, flags, 2 m cell of the tuft centre, tuft size), sorted by rank; flags bit 4
  CHUNK_GRASS, bits 8-15 tris per tuft, 16-23 / 24-31 lowest / highest rank x 255. map.c grass_tris draws the first n
  triangles: tufts with rank < (GRASS_D0 10 m / box distance)^2. **n must be a multiple of 4 triangles**: VIF only
  skips to the next word after an UNPACK, so a stream that ends inside a qword leaves vertex data that runs as VIF
  codes ("Vif1: Unknown VifCmd! [c2]", hung VU1, black screen / NO MAP: the 01:35-02:08 tree). Full chunks are
  safe because their streams are zero-padded (VIF NOPs).
- Transparent CLUT index colour = mean of the opaque edge texels (was black): bilinear CLUT filtering no longer
  gives cut-outs dark rims.
- **Fog** from DSR's own shader source (embedded HLSL in shader/FRPG_*_DX11.shaderbnd, e.g. FRPG_Deferred_LightProbe.fpo):
  `fogCoef = (pos.w - gFC_FogParam.x) * gFC_FogParam.y; lerp(col, fogCol.rgb, saturate(fogCol.a * saturate(coef)))`:
  linear in view z (clip w), max density = FogBank degRotW % (may exceed 100: saturates earlier; row 26 is 165 %),
  mixed in linear HDR before ToneMap_FS (`col *= gFC_ToneMap.x; col /= (y + col)` or the linear variant). The PS2
  fog colour = sRGB(min(1, linear(colRGB) x colA % x grayKeyValue / maxAdapredLum)): row 20 (114,114,110) -> (149,149,144).
- **Area fog**: DSR uses the draw params of the collision part under the player. map.bin ('DSM3', header 80 B,
  u32 fog section offset at 72) has a fog section: rows {begin, end, max, rgb} (row 0 = main) + one u8 row per
  collision.bin triangle (numbered like export_collision.build: sub-parts in order, degenerate ones after its s16
  quantization dropped; count checked against col_tris() at runtime). main.c calls map_set_player(pl.pos) before
  map_draw: col_floor's hit triangle -> row; parameters ease over ~0.3 s (FOG_RATE 0.06 / frame). Firelink rows: 20
  courtyard (most), 21 elevator shaft h0015-17 / h7015-17, 24 Fire Keeper cell h0019 and the tunnels h0029-31 /
  h0022 / h0090-91, 25 h0118. Untextured EE-clipped map triangles now go through the fogged tri_list too.
- Breakable objects (objbnd with `<model>_1.hkx`: o1495-o1497, 25 pots) are left out of map.bin
  (tools/export_objects.py / objects.c draw them).
- PCSX2 (HUD_DEBUG, same spots before -> after): spawn 13k -> 16k tris, map pass 6 -> 8 ms, 60 fps; courtyard
  21k -> 22k, 9 -> 7 ms; cliff 9k -> 13k; wide 22k -> 19k; all 60 fps except the elevator spot L at 30 fps (11k tris,
  map 7 ms, CHR 5 ms with the new enemy roster there; it was 60 before the roster). In-game free RAM 2.9 MB.
  Images notes/milestones/foliage-fog-20261031-0210-{foliage,fog,after-*}.png.
- Not fogged yet: skinned characters (vu1skin.vsm). With row 20 (begin -150 m) the map 4 m away is ~45 % fogged, so
  the player and enemies stand out; vu1skin needs the same XYZF2 fog factor (skin path owner).

## Enemy AI: Lua 5.0 VM + the full Firelink roster (2026-10-31, chars-streaming thread; TECH_DEBT enemy-1 / enemy-3)
- Route chosen: run DSR's own Lua 5.0 bytecode on a small VM (src/lua0.c, ~900 lines), not a C translation. Why: the
  scripts use only 30 of 35 opcodes (no varargs, TFORLOOP, SETLISTO), no metatables, and a stdlib of math.abs +
  table.getn + one loadstring pattern (if.lua: "return function (arg) NAME(arg.ai, arg.goal, arg.codeNo) end"), while
  aiCommon alone is 800 KB of bytecode: almost every GOAL_COMMON_* goal (ApproachTarget, Attack, ComboAttack, Turn,
  NonBattleAct, TeamCallHelp...) is Lua. A translator would have to turn all of that into C; the VM runs it as is.
  Engine surface the scripts need: 37 globals (REGISTER_GOAL / _NO_UPDATE / _NO_INTERUPT / _UPDATE_TIME /
  REGISTER_LOGIC_FUNC / ENABLE_COMBO_ATK_CANCEL / REGISTER_DBG_GOAL_PARAM, loadstring) and 118 ai:/goal: methods.
- tools/lua50.py --pack [--msb game map]: re-serializes chunks (lua_Number double -> float: the EE has no double
  FPU; debug info dropped; definitions ai_define / goal_list / logic_list first). --msb keeps only the NNNNNN_battle /
  _logic chunks of goals the map's enemies use (aiCommon 421 KB -> 133 KB). aiCommon -> common.pak ai.lua0, the map's
  luabnd -> area pack ai.lua0. tools/lua50_pseudo.py prints readable pseudo-code from the bytecode (used to read the
  engine contract below).
- lua0: values 8 bytes (float numbers), interned strings, tables (array + open-addressing hash), closures with open /
  closed upvalues, C closures with one upvalue, light userdata with a method table per tag (ai = 1, goal = 2; a
  missing method gets a stub: Is* / Has* -> false, else 0, logged once), protected calls via setjmp, mark-and-sweep
  GC run between frames when > 192 KB were allocated (roots: globals, method tables, goal params). Lua heap on the EE:
  360 KB after loading, ~380 KB in play.
- Goal engine (src/ai.c, contract read off top_goal / common_logic_func / approach_target / attack / combo_* /
  team_*_help): logic (REGISTER_LOGIC_FUNC by NpcThinkParam.logicId, e.g. common10000_Logic -> COMMON_EasySetup3)
  runs when the top goal is empty or the perception state changed; goals activate at the front of their parent's
  list; front subgoal first, success pops, failure fails the parent; Continue with all subgoals done = success;
  interrupts deepest goal first, then the logic's interrupt. Native (C++ in the game, only registered in Lua):
  Wait, Stay, MoveToSomewhere, Guard, SidewayMove, KeepDist, BackToHome, SpinStep, LeaveTarget, ApproachStep, Parry,
  CommonAttack (its Lua body is an empty stub). They write AiOut: one action request (exe ctx+0x528, read by
  enemyCommon via f106 / f118 / f120), locomotion, facing, guard pose. Assumptions: TECH_DEBT enemy-9 / enemy-10.
- Perception: eye_dist + eye_angX cone, player noise radii into ear_dist, battle / caution states, forget times,
  GetReplanningGoalAction from goalAction_ToCaution / ToFind; FindEnemy / Damaged / FindAttack / Inside_ObserveArea /
  LoseSightTarget / ForgetTarget interrupts (enemy-11 / enemy-12). The chase ends by the scripts' own back-home rules
  (_COMMON_SetBattleActLogic: maxBackhomeDist 75, backhomeDist 50, backhomeBattleDist 6 for most Firelink rows).
  Call help: TeamHelp_* with callHelp_MyPeerId / CallPeerId within callHelp_CallValidRange (0 for every Firelink
  row, so nobody calls here: game data).
- Native check (notes/enemy/aitest/run.sh <chr.bin> [variant]): ai.c + lua0.c on the host against a scripted
  player and a stand-in ESD. c2900 290003: run in, 3008 lunge at 6.5 m, spin steps 702 / 703, 3000 -> 3001 -> 3002
  combos via ComboAttack / ComboRepeat / ComboFinal, guard 9910 while approaching, SidewayMove / KeepDist /
  LeaveTarget spacing, chase when the player runs; every variant of the roster runs its scripts with 0 errors.
- Roster (export_chr.py v4, one pack per character, Makefile ENEMIES): c1000 bonfire dummies x3 (no mesh, idle),
  c1201 rat, c2510 undead (beggar, teamType 2), c2540 undead soldiers x5 (4 variants: sword+shield, axe with a01_
  anims, firebomb, two think rows), c2550 heavy soldier (a01_), c2750 NPC, c2900 skeletons x11 (290002 greatsword
  variant with a02_ anims), c2910 giant skeletons x2, c3510 giant crow, c5330 Frampt. chr.bin variants carry NpcParam
  (HP, def_phys, capsule, turn, poise, modelDispMask -> #NN# mesh group bits 8+NN hidden per variant with mdl_hide,
  animIdOffset, teamType) and NpcThinkParam values (perception, distances, the 24 GetExcelParam entries).
- Memory: model.bin anims in a compact form (export_chr.py append_compact_anims, model.c reads both kinds): 15 fps,
  base pose + per frame only the bones that move (c2900: 2.0 MB of 30 fps full poses -> 0.7 MB for 35 anims).
  Skeletons over 61 bones reduced to the most-weighted bones (dropped ancestors folded into kept children per
  frame, dropped leaves' vertices on their kept ancestor). LOD / texture caps per type in the Makefile. Totals in the
  test build: packs + roster leave 2.9 MB EE RAM and 59 KB VRAM; 60 fps game time with 11 characters drawn.
- Streaming (coordinator's 3 MB enemy budget): textures of every type upload at boot and stay in VRAM; the model
  data of a type loads when a placement is within 60 m and is freed beyond 75 m (enemy.c stream_types, model.c
  mdl_compact / mdl_strip / mdl_share_textures). Per-type cost when loaded: c2900 643 KB, c2540 600 KB, c2750 600 KB,
  c5330 490 KB, c2910 440 KB, c2550 350 KB, c3510 340 KB, c2510 300 KB, c1201 230 KB; chr.bin (170 KB for all) and the
  Lua VM stay resident. EE log, graveyard: 12.2 MB free after boot (all unloaded), 9.9 MB in play with 5 types.
  pack.c reports failed loads ("OUT OF RAM: <entry> needs ..K, free ..K") in the log and on the enemy HUD line.
  CHR pass: poses only for characters on screen or in an attack window, every other frame beyond 15 m.
- Gotchas: (1) a texture's narrow mip (32x128) is widened by quantize() to 128 keeping the aspect (128x512): the
  exporter now resamples to exactly 128 wide itself. (2) Running out of VRAM makes later gsKit allocations fail and
  the GS output goes black with the game still running: check emulog for "Not enough VRAM". (3) EE printf reaches
  ~/.var/app/net.pcsx2.PCSX2/config/PCSX2/logs/emulog.txt (host home) with [Logging] EnableEEConsole / EnableIOPConsole
  = true in PCSX2.ini (set 2026-10-31, backup PCSX2.ini.bak-before-eelog). (4) make treats src/c####/chr.bin as an
  intermediate: after changing LOD / TEX in the Makefile delete data/c####.pak to re-export.

## Fog on skinned characters (2026-10-31, char-skin; TECH_DEBT map-15 updated)
- vu1skin.vsm now does vu1map.vsm's fog: F = clamp(a * view z + b, min, max) from the bone-blended view z (VF01.w),
  output register XYZF2 instead of XYZ2 (PRIM tag regs 0xF412, FGE when the row fogs), the whole XYZF2 qword stored
  (the old `isw.w VI00` clears of the w word are gone) and on the third vertex ADC | F as in the map program (VI10 =
  0x8000, mtir of the ftoi4'd F).
- Constants 4-7: vu1_skin_begin re-sends them per character with the XYZF2 Z scale (raw, not /16) and the fog terms
  that vu1_map_fog saw last (the map's main row after the sky's; no map = no fog). FOGCOL is still the map's colour
  when chain 1 runs (map.c sets it with vu1_gs_regs, render.c restores it after EE packets).
- 7 m in front of the player (spawn): character now matches the fogged arches behind it, 60 fps, VU1 / EE ms unchanged
  (4 more VU ops per vertex). notes/milestones/char-skin-20261031-0215-fog-*.png.
- **User "gap" (notes/user-gap-0116.webp)**: not a hole. Firelink's two water meshes ([We] water shader: m2022
  pool, m2310 aqueduct) have no diffuse map, so they were exported untextured and drawn on the EE as flat bright
  grey, which reads as sky through a missing floor (reproduced at -40 -56 84, the pool at the foot of the m2020
  stairs). Ruled out first: QEM (offline simplified vs EPS=-1 hole search over all stairs: only known map-8 losses
  and hairline cracks), the 90 m cull, EE/VU1 guard and cull signs, `_Alp` stone textures (opaque). Now
  export_map.water_rgba: one RGBA from g_WaterColor / g_FresnelColor at 60 degrees, blended chunk with a solid
  128x16 texture (TECH_DEBT water row). Image notes/milestones/foliage-fog-20261031-0234-water-gap.png.
