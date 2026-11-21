# DSR -> PS2 character prototype. `make` exports everything it needs from the user's own copy of the game (DSR_GAME,
# see ../../config.mk and README.md) into $(BUILD) (default ../../build), then builds $(BUILD)/proto.elf.
# Nothing generated is written to the source tree; ./proto.elf and ./data are convenience links into $(BUILD).
include ../../config.mk
# Export knobs, per-area map tuning, per-character tables (tools/recipe.toml, generated: python3 tools/recipe.py make)
include export_vars.mk
# Parallel by default: a plain make runs JOBS recipes side by side (default: every core); make -jN or JOBS=N overrides
# it. RAM stays within tools/q's make reservation: the only big exporter is export_map, and a build runs at most one per
# area (boot area + world block side by side, as with -j2 before); a map re-export peaked at ~6 GB either way (2026-11-01).
JOBS ?= $(shell nproc)
ifeq ($(filter -j% --jobserver%,$(MAKEFLAGS)),)
MAKEFLAGS += -j$(JOBS)
endif
# exporters run through the job queue (tools/q): one heavy job at a time, 12 GB address-space cap (OOM kills 2026-10-31)
PY := $(abspath ../../tools/q) -m 12 $(PY)
# Area to build and load. Firelink (the default) builds proto.elf from $(BUILD)/gen/*.bin and $(BUILD)/data/*.pak;
# any other area (make MAP=m10_01_00_00) keeps its own outputs: gen/<MAP>/ (area bins, enemy chr.bin), data/<MAP>/
# (enemy + common packs), data/<MAP>.pak, proto_<MAP>.elf. Only main.o / enemy.o differ per area (AREA = the area pack,
# CHR_DIR = where its enemy / common packs live); they go to obj/<MAP>/. What part of the MSB an area holds (spawn,
# region, budgets) is in tools/area.py.
MAP ?= $(DEFAULT_MAP)
# World block (src/world.c, tools/area.py BLOCKS): `make MAP=<msb> BLOCK=<block> blockpacks` exports a cropped part of
# an MSB like an area into gen/<BLOCK>/ and data/<BLOCK>.pak; the exporters read AREA_BLOCK. OUT = the output name.
BLOCK ?=
OUT = $(or $(BLOCK),$(MAP))
ifneq ($(BLOCK),)
export AREA_BLOCK := $(BLOCK)
endif
# Tool dependencies by meaning (TS = $(BUILD)/stamp): exports depend on $(TS)/<tool>.sum, a stamp holding the hash of
# the tool's syntax tree without comments and docstrings (tools/srcsum.py; C sources: their bytes). It is rewritten only
# when that changes, so a touch, a checkout, a save without changes or a comment-only edit re-exports nothing (a new
# stamp takes the tool's own mtime, so adding the stamps rebuilds nothing). Every make re-checks the tools (FORCE), like
# $(O)/.cflags (an unchanged file is not parsed again); stamps are replaced by rename, so parallel sub-makes never read
# half a one.
# Narrow dependencies (2026-11-02, a map.bin re-export is ~4 min): an export depends on what it takes from another
# exporter, not on that exporter's file: map.bin on objects.bin.static / .anim (rewritten only when they change),
# lights.json (export_effects.py --static-lights) and emevd.py's switched_entities; every per-area export on
# $(A)/area.sig (tools/area.py --sig: that area's config + area.py's code), so a new block re-exports only that block.
TS = $(BUILD)/stamp
# A tool not modified since its stamp's last check (<stamp>.key, written by srcsum.py whenever the tool's size / mtime
# change) is not handed to Python at all: each sub-make re-checks every stamp, ~500 Python starts a world build (find
# -newer compares to the nanosecond)
$(TS)/%.sum: FORCE
	@if [ -f $@ ] && [ -f $@.key ] && [ -z "$$(find ../../tools/$* -newer $@.key)" ]; then :; else \
		mkdir -p $(@D) && python3 -I ../../tools/srcsum.py ../../tools/$* $@; fi
.PRECIOUS: $(TS)/%.sum
# G = generated files shared by every area (player model / ESD / data headers, menu), O = objects, D = packs
G = $(BUILD)/gen
O = $(BUILD)/obj
D = $(BUILD)/data
ifeq ($(OUT),$(DEFAULT_MAP))
AREA_SRC = $(G)
AREA_DATA = $(D)
EE_BIN = $(BUILD)/proto.elf
AREA_OBJ = $(O)
AREA_OBJS = $(O)/main.o $(O)/enemy.o
else
AREA_SRC = $(G)/$(OUT)
AREA_DATA = $(D)/$(OUT)
EE_BIN = $(BUILD)/proto_$(OUT).elf
AREA_OBJ = $(O)/$(OUT)
AREA_OBJS = $(O)/$(MAP)/main.o $(O)/$(MAP)/enemy.o
endif
A = $(AREA_SRC)

EE_OBJS = $(AREA_OBJS) $(addprefix $(O)/,render.o player.o model.o map.o mapvis.o maplight.o mapchunk.o mappass.o maplog.o mapblock.o mapstream.o msarena.o vu1.o vu1skin2.o vu1clip.o \
	vu1map2.o vu1mapi.o vu1mapspec.o vu1skinclip.o vu1fx.o collision.o coltile.o esd.o esd_env_move.o combat.o trail.o chrstat.o omit.o cvar.o status.o pack.o hud.o menu.o lua0.o ai.o objects.o physics.o \
	texcache.o gs.o material.o mem.o frame.o cam_input.o camera.o ot.o gstrace.o g2d.o testscript.o debug_hud.o bump.o fx.o chrlight.o shadow.o drawgrp.o pvs.o hitch.o lpass.o world.o flags.o progress.o save.o event.o ev_chr.o ev_obj.o ev_misc.o bonfire.o npc.o \
	gamedata.o chunk.o ragdoll.o nav.o items.o drb.o loading.o sound.o vmath.o pickup.o)
EE_INCS += -I$(GSKIT)/include -I$(G) -Isrc
EE_LDFLAGS += -L$(GSKIT)/lib
EE_LDFLAGS += -Wl,--wrap=_write  # src/pack.c: writes wait for a pending async read
# src/hitch.c: allocation failures in the EE log ("alloc:" lines, counted in "hitch:" lines)
EE_LDFLAGS += -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=memalign,--wrap=free
# make HEAPMAP=1: heap census per caller in the EE log ("heapmap:" lines after each hitch line; a measuring build)
ifeq ($(HEAPMAP),1)
EE_CFLAGS += -DHEAPMAP
endif
EE_LIBS = -lgskit -ldmakit -lpacket2 -ldma -lpad -lmc -lpatches -lm
EE_DVP = dvp-as
# Float math (refactor target 3, 2026-11-05): the game code does no double arithmetic (tools/ci/compile.sh fails on
# any), and sqrtf is already an inline sqrt.s. -fno-math-errno lets gcc treat sinf/cosf/... as pure (merged repeat
# calls); nothing reads errno. -ffinite-math-only and -ffast-math stay off: collision and the ragdoll rely on exact
# compares. MATHFLAGS= builds without, for A/B profiling.
MATHFLAGS ?= -fno-math-errno
EE_CFLAGS += -O2 -std=gnu99 -Wall $(MATHFLAGS) $(DEFS)
# CHR_BONES: skeleton bones kept per character (TECH_DEBT char-7). 255 (default): every skeleton whole, drawn through
# per-batch bone windows (model.c); CHR_BONES=61 folds bigger skeletons to 61 bones at export.
# model.h MDL_MAX_BONES sizes the pose / palette arrays for it
EE_CFLAGS += -DMDL_MAX_BONES=$(shell [ $(CHR_BONES) -gt 64 ] && echo $(CHR_BONES) || echo 64)
# VU1_BONES: bones in the VU1 skin palette window (tools/vu1/layout.py BONE_MAX). 40 (default, Polaris 2.2 PR 2): every
# skeleton over 40 paged through bone windows, 27-triangle batches. VU1_BONES=61 is the old path: one 61-bone window
# (windows only above 61), 23-triangle batches (24 before the light block); vu1_layout.h and the two skin programs are generated into $(O)/vu1gen
VU1_BONES ?= 40
# Occlusion (src/pvs.c, tools/pvs_bake.c, 2026-11-06) is the default: map.c draws a chunk only when the camera's cell
# sees its part, and mapstream.c reads only what cells near the camera see. PVS=0: no pvs.bin is used (A/B fallback)
PVS ?= 1
ifeq ($(PVS),0)
EE_CFLAGS += -DNO_PVS
endif
# Disc (budget doc proposals 2 and 4, 2026-11-08): the streaming budgets follow the media in the drive (dsrsnd reads the
# disc type at its first cdrom0: open: the DVD-9 column on a DVD, the CD column on a CD; host: runs use the DVD one).
# MEDIA=cd forces the CD column (fallback). DISC_CACHE=0 builds dsrsnd without its 512 KB IOP page cache (in its own
# BUILD dir: the switch does not rebuild an existing dsrsnd.irx).
MEDIA ?= auto
ifeq ($(MEDIA),cd)
EE_CFLAGS += -DMEDIA_CD
endif
DISC_CACHE ?= 1
DSRSND_DEFS = $(if $(filter 0,$(DISC_CACHE)),-DDSRSND_NO_CACHE)

# Profiling builds (tools-hwperf: src/prof.c, notes/threads/tools-hwperf.md); the shared objects and proto.elf stay as
# they are. make PROFILE=1 -> proto_prof.elf: main.c with -DPROFILE (per-pass EE cycles + I$ / D$ misses from the COP0
# performance counters, on the HUD and in the log). make GPROF=1 -> proto_gprof.elf: every C file with -pg in
# $(O)/gprof/, ps2sdk libprofglue sampling, host:gmon.out after PROF_GPROF_FRAMES frames (tools/ps2-gprof.sh).
AREA_DEFS = $(if $(filter-out $(DEFAULT_MAP),$(MAP)),-DAREA='"$(MAP)"' -DCHR_DIR='"$(MAP)/"')
ifeq ($(PROFILE),1)
EE_BIN := $(EE_BIN:.elf=_prof.elf)
# (ai.c too: its per-goal-script lua0 instruction counts, Polaris census; frame.c: the loop's PROF_MARK / PROF_FRAME /
# PROF_GATE hooks moved there in Polaris 1.3)
EE_OBJS := $(filter-out $(AREA_OBJ)/main.o $(O)/ai.o $(O)/frame.o,$(EE_OBJS)) $(AREA_OBJ)/prof/main.o $(O)/prof/ai.o \
	$(O)/prof/frame.o $(O)/prof.o
endif
ifeq ($(GPROF),1)
EE_BIN := $(EE_BIN:.elf=_gprof.elf)
VSM_OBJS = $(patsubst src/%.vsm,$(O)/%.o,$(wildcard src/*.vsm))
EE_OBJS := $(patsubst $(O)/%,$(O)/gprof/%,$(filter-out $(VSM_OBJS),$(EE_OBJS)) $(O)/prof.o) $(filter $(VSM_OBJS),$(EE_OBJS))
EE_LIBS += -lprofglue
endif
# Alignment check build (hw-6): make ALIGNCHECK=1 -> proto_align.elf, every C file with -fsanitize=alignment in
# $(O)/align/. The EE raises an address error on a lh / lw / ld / sw ... at a misaligned address (the game freezes on a
# PS2); PCSX2 does the access anyway. src/aligncheck.c logs each misaligned site once ("ALIGN file:line type ptr") and
# lets the access go on, so a PCSX2 run of this ELF lists every access that would crash a console.
ifeq ($(ALIGNCHECK),1)
EE_BIN := $(EE_BIN:.elf=_align.elf)
VSM_OBJS = $(patsubst src/%.vsm,$(O)/%.o,$(wildcard src/*.vsm))
EE_OBJS := $(patsubst $(O)/%,$(O)/align/%,$(filter-out $(VSM_OBJS),$(EE_OBJS))) $(filter $(VSM_OBJS),$(EE_OBJS)) \
	$(O)/aligncheck.o
endif
# (the rules below come before `all:`; keep it the default goal)
.DEFAULT_GOAL := all
# Sound (src/sound.c): the IOP module iop/dsrsnd (dsrsnd.irx) and ps2sdk's libsd.irx, linked in as C arrays (bin2c).
# After the PROFILE / GPROF lists: these objects have no source in src/.
IRX_OBJS = $(O)/irx/dsrsnd_irx.o $(O)/irx/libsd_irx.o
EE_OBJS += $(IRX_OBJS)
$(O)/irx/dsrsnd.irx: iop/dsrsnd/dsrsnd.c iop/dsrsnd/imports.lst iop/dsrsnd/irx_imports.h iop/dsrsnd/Makefile src/dsrsnd_rpc.h
	@mkdir -p $(@D)
	$(MAKE) --no-print-directory -C iop/dsrsnd IOP_OBJS_DIR=$(abspath $(@D))/ IOP_BIN=$(abspath $@) DSRSND_DEFS='$(DSRSND_DEFS)'
$(O)/irx/libsd.irx: $(PS2SDK)/iop/irx/libsd.irx
	@mkdir -p $(@D)
	cp $< $@
$(O)/irx/%_irx.c: $(O)/irx/%.irx
	bin2c $< $@ $*_irx
$(O)/irx/%_irx.o: $(O)/irx/%_irx.c
	$(EE_CC) -c $< -o $@

# Assets are not linked in: they load at runtime from LZ4 packs in $(BUILD)/data/ (host: = the ELF's folder under PCSX2 with
# HostFs on; cdrom0: on a disc later). One pack per area (map + collision) and per character (model).
# Enemies / NPCs: one pack per character (model.bin + chr.bin, tools/export_chr.py) for every MSB enemy part of the
# area; enemyCommon.esd + aiCommon's Lua (tools/lua50.py --pack) in common.pak, the area's Lua in the area pack.
# The area's enemy types come from its MSB (inside the area's region, tools/area.py); Firelink: c1000 c1201 c2510 c2540
# c2550 c2750 c2900 c2910 c3510 c5330
# (make elf needs neither the game nor the packs: no enemy list, no world blocks)
GAMEFREE := $(filter elf,$(MAKECMDGOALS))
ENEMIES ?= $(if $(GAMEFREE),,$(shell AREA_BLOCK=$(BLOCK) python3 -I ../../tools/export_enemies.py --models "$(GAME)" $(MAP)))
PACKS = $(D)/c0000.pak $(D)/$(OUT).pak $(D)/menu.pak $(ENEMIES:%=$(AREA_DATA)/%.pak) $(AREA_DATA)/common.pak \
	$(if $(BLOCK),,$(A)/snd.stamp)
# World streaming (src/world.c): the other blocks of this boot area's world (tools/area.py WORLDS, "<msb>:<block>"),
# each built by a sub-make, and data/world.pak with their table (tools/export_world.py). Only for the default map.
WORLD_BLOCKS := $(if $(GAMEFREE)$(BLOCK)$(filter-out $(DEFAULT_MAP),$(MAP)),,$(shell python3 -I ../../tools/area.py --world $(MAP)))
ifneq ($(WORLD_BLOCKS),)
PACKS += worldpacks $(D)/world.pak $(G)/world_manifest.json $(G)/world_budget.json $(BUILD)/scan/files.json $(G)/blockreport.json
endif
WORLD_SPLIT = $(or $(BLOCK),$(WORLD_BLOCKS))

all: $(PACKS) $(EE_BIN)
	$(EE_STRIP) --strip-all $(EE_BIN)
	@# the links (the user's launch path) follow the default build only: scratch / variant BUILD= dirs leave them alone
	@$(if $(filter $(abspath $(DSR_ROOT)/build),$(abspath $(BUILD))),ln -sfn $(EE_BIN) $(notdir $(EE_BIN)); [ -e data ] && [ ! -L data ] || ln -sfn $(D) data,true)
# make elf: only the ELF, without the game (CI release, tools/pages): it holds no game data, the values it needs come
# from the packs at boot (src/gamedata.c), so one build runs with every player's own packs of the same layout
elf: $(EE_BIN)
	$(EE_STRIP) --strip-all $(EE_BIN)
.PHONY: elf
# The ELF's old in-tree name (make proto.elf, make proto_<MAP>.elf) = all; ./proto.elf is the link `all` refreshes
$(notdir $(EE_BIN)): all
# one world block's packs (no ELF): make MAP=<msb> BLOCK=<block> blockpacks
blockpacks: $(D)/$(OUT).pak $(ENEMIES:%=$(AREA_DATA)/%.pak)  # chr packs with this block's variants (enemy.c)
.PHONY: blockpacks
# WORLD_J: jobs for the block sub-makes (default: share this make's jobs). Their exporters run inside this make's queue
# job and reserve no RAM of their own (each block adds one export_map, up to ~5 GB). WORLD_J=1: one block recipe at a
# time, as before.
WORLD_J ?=
# Occlusion bakes see every block (tools/pvs_bake.c --with: a neighbour's floors place cameras, its walls occlude), so
# every block's map export (pvsin.bin, dg.bin, collision.bin) comes first: worldexports, then the packs.
PVS_PEERS ?= $(if $(WORLD_BLOCKS),$(G) $(foreach b,$(WORLD_BLOCKS),$(G)/$(lastword $(subst :, ,$(b)))))
worldexports:
	@$(MAKE) --no-print-directory $(if $(WORLD_J),-j$(WORLD_J)) $(addprefix worldexport@,$(subst :,@,$(WORLD_BLOCKS)))
worldexport@%:
	@b='$*'; $(MAKE) --no-print-directory MAP=$${b%%@*} BLOCK=$${b#*@} blockexport
# The far panorama of a block (tools/farpano.py) is baked from every other block's horizon, so the horizons come before
# the packs too: worldhzn (each block's sub-make and this make's own), then the packs.
worldhzn: worldexports $(if $(WORLD_BLOCKS),$(A)/hzn.bin)
	@$(MAKE) --no-print-directory $(if $(WORLD_J),-j$(WORLD_J)) PVS_PEERS='$(PVS_PEERS)' $(addprefix worldblockhzn@,$(subst :,@,$(WORLD_BLOCKS)))
worldblockhzn@%:
	@b='$*'; $(MAKE) --no-print-directory MAP=$${b%%@*} BLOCK=$${b#*@} PVS_PEERS='$(PVS_PEERS)' blockhzn
blockhzn: $(A)/hzn.bin
.PHONY: worldhzn blockhzn
# The two long bakes of every block start as soon as their inputs are there, ahead of the packs' smaller exports
# (2026-11-17: in worldpacks the bakes queued behind each block's chr exports for up to 2 min, and the occlusion bakes
# waited for every horizon though they need none; the bakes, the GPU's turn per block, are the build's critical path):
# worldpvs right after worldexports (alongside worldhzn), worldfar right after worldhzn. WORLD_EARLY_BAKES=0: as before
WORLD_EARLY_BAKES ?= 1
worldpvs: worldexports $(if $(WORLD_BLOCKS),$(A)/pvsin.bin $(A)/dg.bin $(A)/collision.bin)
	@$(MAKE) --no-print-directory $(if $(WORLD_J),-j$(WORLD_J)) PVS_PEERS='$(PVS_PEERS)' $(addprefix worldblockpvs@,$(subst :,@,$(WORLD_BLOCKS)))
worldblockpvs@%:
	@b='$*'; $(MAKE) --no-print-directory MAP=$${b%%@*} BLOCK=$${b#*@} PVS_PEERS='$(PVS_PEERS)' blockpvs
blockpvs: $(A)/pvs.bin
worldfar: worldhzn $(if $(WORLD_BLOCKS),$(A)/pano.bin)  # (far.bin reads the boot map's pano.bin: recipe.toml)
	@$(MAKE) --no-print-directory $(if $(WORLD_J),-j$(WORLD_J)) PVS_PEERS='$(PVS_PEERS)' $(addprefix worldblockfar@,$(subst :,@,$(WORLD_BLOCKS)))
worldblockfar@%:
	@b='$*'; $(MAKE) --no-print-directory MAP=$${b%%@*} BLOCK=$${b#*@} PVS_PEERS='$(PVS_PEERS)' blockfar
blockfar: $(A)/far.bin
.PHONY: worldpvs blockpvs worldfar blockfar
worldpacks: worldhzn $(if $(filter 1,$(WORLD_EARLY_BAKES)),worldpvs worldfar $(if $(WORLD_BLOCKS),$(A)/pvs.bin $(A)/far.bin)) $(if $(WORLD_BLOCKS),$(A)/pvsin.bin $(A)/dg.bin $(A)/collision.bin)
	@$(MAKE) --no-print-directory $(if $(WORLD_J),-j$(WORLD_J)) PVS_PEERS='$(PVS_PEERS)' $(addprefix worldblock@,$(subst :,@,$(WORLD_BLOCKS)))
worldblock@%:
	@b='$*'; $(MAKE) --no-print-directory MAP=$${b%%@*} BLOCK=$${b#*@} PVS_PEERS='$(PVS_PEERS)' blockpacks
blockexport: $(A)/pvsin.bin $(A)/dg.bin $(A)/collision.bin
.PHONY: worldpacks worldexports blockexport
.PHONY: $(notdir $(EE_BIN))

# Sound packs (tools/recipe.toml "sound"): the boot map and every map of its world blocks in one export
SND_MAPS = $(sort $(MAP) $(foreach b,$(WORLD_BLOCKS),$(firstword $(subst :, ,$(b)))))


# WORLD_SEAM_BUNDLE (knob, tools/recipe.toml): world block packs list what world.c reads to turn a block on first
EE_CFLAGS += -DWORLD_SEAM_BUNDLE=$(WORLD_SEAM_BUNDLE)
LITE_RUN = $(and $(filter 1,$(WORLD_SEAM_BUNDLE)),$(BLOCK),$(WORLD_SPLIT))
# (a changed order re-packs: $(A)/pakenv; a tree without it that packs in the old order keeps its packs)
$(A)/pakenv: FORCE
	@mkdir -p $(@D)
	@v='LITE_RUN=$(if $(LITE_RUN),1,0)'; if [ ! -e $@ ]; then echo "$$v" > $@; [ -n "$(LITE_RUN)" ] || touch -d @0 $@; \
	else echo "$$v" | cmp -s - $@ || echo "$$v" > $@; fi
# world builds: map.bin split for lite / full blocks (tools/mapsplit.py, src/world.c), read raw in the background
# + the resident far backdrop (tools/horizon.py: a very coarse untextured level of every cell, appended to maptab.bin so
# it loads with the block; map.c draws it beyond HZN_R and in place of coarse cells not in yet). HZN=0: none (old path)
# WORLD_LITE_DIET (the Burg plan's step B1, default 1): mip 1 of textures with a mip chain streams from mapres.bin
# (mapsplit.py MIP1_OUT) and a block that is only kept holds its collision and events, not its map tables (src/world.c).
# WORLD_LITE_DIET=0: today's lite sets (a changed value re-splits every block: $(A)/splitenv). WORLD_MIP1_OUT
# (default WORLD_LITE_DIET) is the mip 1 half alone: on taurus it halved the cells drawn coarse while due near
# (7 -> 3) with near refusals level (ISO A/B 2026-11-08, two runs each).
EE_CFLAGS += -DWORLD_LITE_DIET=$(WORLD_LITE_DIET)
# COL_TILES (the cube-grid plan's step 1, default 1): world blocks read their collision.bin's index with the lite set
# and its 16 m tiles (tools/export_collision.py COL_TILE) by distance into the structural pool (src/coltile.c).
# COL_TILES=0: collision.bin read whole with the lite set (the export stays paged; COL_TILE=0 exports unpaged files).
COL_TILES ?= 1
# BUMP_BAKE (default 1): normal-map relief baked into the diffuse texels at export (tools/export_normal.py bake_rgba);
# bump.bin stays empty and src/bump.c reserves no relief VRAM pool / CLUT area. 0: the runtime CLUT relief pass (needs a
# re-export: make distclean of the map exports, the switch is not in the export stamps).
EE_CFLAGS += -DBUMP_BAKED=$(BUMP_BAKE)
EE_CFLAGS += -DCOL_TILES=$(COL_TILES)
$(A)/splitenv: FORCE
	@mkdir -p $(@D)
	@if [ ! -e $@ ]; then echo 'MIP1_OUT=$(WORLD_MIP1_OUT) TEX=$(WORLD_TEX_SHARED)' > $@ && touch -d @0 $@; \
	else echo 'MIP1_OUT=$(WORLD_MIP1_OUT) TEX=$(WORLD_TEX_SHARED)' | cmp -s - $@ || echo 'MIP1_OUT=$(WORLD_MIP1_OUT) TEX=$(WORLD_TEX_SHARED)' > $@; fi
# Shared textures (the cube-grid plan): the other blocks of this MSB in the world (PVS_PEERS whose directory is a block
# of $(MAP)); textures two or more of them have go to maptex.bin, read once per map (src/world.c). WORLD_TEX_SHARED=0:
# none (every block's maptab holds its own copy, as before).
TEX_PEERS = $(if $(filter 1,$(WORLD_TEX_SHARED)),$(if $(BLOCK),$(foreach d,$(PVS_PEERS),$(if $(filter $(MAP)_%,$(notdir $(d))),$(d)/map.bin))))
HZN_PANO = $(if $(and $(WORLD_BLOCKS),$(filter 1,$(PANO))),1,0)

# Objects: src/*.c -> $(O)/*.o; area-dependent objects of a non-default area go to $(O)/<MAP>/
$(O)/%.o: src/%.c $(O)/.cflags
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@
ifneq ($(MAP),$(DEFAULT_MAP))
$(O)/$(MAP)/%.o: src/%.c $(O)/.cflags
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DAREA='"$(MAP)"' -DCHR_DIR='"$(MAP)/"' -c $< -o $@
endif

AREA_SIG = $(A)/area.sig
$(A)/area.sig: FORCE
	@mkdir -p $(@D)
	@python3 -I ../../tools/area.py --sig $(MAP) $@
$(A)/.chr_bones: FORCE
	@mkdir -p $(@D); echo $(CHR_BONES) | cmp -s - $@ || echo $(CHR_BONES) > $@


$(O)/combat.o: src/combat.h
$(O)/player.o $(O)/main.o $(O)/enemy.o: src/combat.h
$(O)/enemy.o: src/enemy.h src/ai.h src/model.h src/esd.h src/ragdoll.h src/nav.h
$(O)/ragdoll.o: src/ragdoll.h src/model.h src/collision.h
$(O)/nav.o: src/nav.h
$(O)/ai.o: src/ai.h src/lua0.h
$(O)/lua0.o: src/lua0.h
$(O)/main.o $(O)/player.o: src/model.h


# The knobs above as passed to this area's export (MAPENV + MAPENV_<area>, also as set in config.local.mk or on the
# command line), rewritten only when they change like $(O)/.cflags: a knob edit re-exports that area's map.bin. A first
# stamp takes export_map.py's stamp mtime, so adding it re-exports no map.
MAPENV_LINE = $(strip $(MAPENV) $(MAPENV_$(MAP)))
$(A)/mapenv: FORCE | $(TS)/export_map.py.sum
	@mkdir -p $(@D)
	@if [ ! -e $@ ]; then echo '$(MAPENV_LINE)' > $@ && touch -r $(TS)/export_map.py.sum $@; \
	else echo '$(MAPENV_LINE)' | cmp -s - $@ || echo '$(MAPENV_LINE)' > $@; fi
# host executables are named per C library: the container build (tools/build-iso.sh, Alpine musl) and a host build
# (glibc) may share one build dir, and the other one's binary does not run here but looks up to date (tools/native_so.py)
HOST_TAG := $(shell ldd --version 2>&1 | grep -qi musl && echo -musl)
worldpvs: $(PVS_BAKE)  # (built once here, before the block sub-makes would race to build it)
PVS_WITH = $(filter-out $(abspath $(A)),$(abspath $(PVS_PEERS)))
# the other blocks' exports have no rule in this make: worldexports (the top make) or the parent make's worldexports
# (a block's sub-make) writes them; order-only, so an existing file is judged by its time alone
PVS_WITH_FILES = $(foreach d,$(PVS_WITH),$(d)/pvsin.bin $(d)/dg.bin $(d)/collision.bin)
$(PVS_WITH_FILES): | $(if $(WORLD_BLOCKS),worldexports)
	@test -f $@ || { echo "$@ missing: the world block exports (worldexports) did not write it"; exit 1; }
WB_WITH = $(foreach d,$(PVS_WITH),$(d)/map.bin.cells.json)
$(WB_WITH): | $(if $(WORLD_BLOCKS),worldexports)
	@test -f $@ || { echo "$@ missing: the world block exports (worldexports) did not write it"; exit 1; }
# the other blocks' maptab.bin come from their blockpacks sub-makes (worldpacks): no rule here, as with WB_WITH, so a
# fresh build dir (the container's /build) stopped on "No rule to make target .../maptab.bin" (2026-11-09)
WB_TABS = $(foreach d,$(PVS_WITH),$(d)/maptab.bin)
$(WB_TABS): | $(if $(WORLD_BLOCKS),worldpacks)
	@test -f $@ || { echo "$@ missing: the world block packs (worldpacks) did not write it"; exit 1; }
# the other blocks of this MSB whose textures the map split shares (TEX_PEERS) come from worldexports too
TEX_WITH = $(filter-out $(A)/map.bin,$(TEX_PEERS))
$(TEX_WITH): | $(if $(WORLD_BLOCKS),worldexports)
	@test -f $@ || { echo "$@ missing: the world block exports (worldexports) did not write it"; exit 1; }
# the other blocks' horizons (far.bin's peers) come from worldhzn. (These check rules follow PVS_WITH: a rule's targets
# expand where it is read; until 2026-11-20 this one came first, its target list was empty and far.bin did not list its
# peers' horizons and draw groups as prerequisites, so a peer's re-export left its panorama stale.)
FAR_WITH = $(foreach d,$(PVS_WITH),$(d)/hzn.bin)
$(FAR_WITH): | $(if $(WORLD_BLOCKS),worldhzn)
	@test -f $@ || { echo "$@ missing: the world block horizons (worldhzn) did not write it"; exit 1; }
# the other blocks' files the block report reads (recipe.toml "block report") come from their sub-makes (worldpacks):
# without these rules a fresh build dir stopped on "No rule to make target .../map.bin, needed by blockreport.json"
BR_WITH = $(filter-out $(TEX_WITH),$(foreach d,$(PVS_WITH),$(d)/map.bin)) \
	$(foreach d,$(PVS_WITH),$(d)/far.bin $(d)/fx.bin $(d)/objects.bin $(d)/events.bin $(d)/ai_map.lua0)
$(BR_WITH): | $(if $(WORLD_BLOCKS),worldpacks)
	@test -f $@ || { echo "$@ missing: the world block packs (worldpacks) did not write it"; exit 1; }

# the boot map's backdrop pictures, which every block's far panorama reads, come from the top make (worldfar)
ifneq ($(BLOCK),)
$(G)/pano.bin:
	@test -f $@ || { echo "$@ missing: the boot map's backdrop pictures (worldfar) did not write it"; exit 1; }
endif

# The export rules: one per step of tools/recipe.toml (generated: python3 tools/recipe.py make)
include export_rules.mk




# VU1 microprograms (hand-scheduled VU assembly)
$(O)/%.o: src/%.vsm
	@mkdir -p $(@D)
	$(EE_DVP) $< -o $@
# make VU1_BONES=61 (above): the old skin layout's header and programs, generated into the build dir
ifneq ($(VU1_BONES),40)
VU1G = $(O)/vu1gen
EE_CFLAGS += -DVU1_LAYOUT_FILE='"$(abspath $(VU1G))/vu1_layout.h"'
$(VU1G)/vu1_layout.h $(VU1G)/vu1skin2.vsm $(VU1G)/vu1skinclip.vsm &: ../../tools/vu1/layout.py ../../tools/vu1/gen.py $(O)/.cflags
	@mkdir -p $(@D)
	cd ../../tools/vu1 && VU1_BONE_MAX=$(VU1_BONES) python3 gen.py layout > $(abspath $(VU1G))/vu1_layout.h
	cd ../../tools/vu1 && VU1_BONE_MAX=$(VU1_BONES) python3 gen.py skin > $(abspath $(VU1G))/vu1skin2.vsm
	cd ../../tools/vu1 && VU1_BONE_MAX=$(VU1_BONES) python3 gen.py skinclip > $(abspath $(VU1G))/vu1skinclip.vsm
$(O)/vu1skin2.o: $(VU1G)/vu1skin2.vsm
	@mkdir -p $(@D)
	$(EE_DVP) $< -o $@
$(O)/vu1skinclip.o: $(VU1G)/vu1skinclip.vsm
	@mkdir -p $(@D)
	$(EE_DVP) $< -o $@
$(filter-out %vu1skin2.o %vu1skinclip.o,$(EE_OBJS)): $(VU1G)/vu1_layout.h
endif

# Stale objects: every object depends on $(O)/.cflags, rewritten only when the compile flags change (a plain make after
# make EE_DBGINFOFLAGS=-DCOL_TEST=1 recompiles), and on the headers it includes (-MMD writes $(O)/*.d)
EE_CFLAGS += -MMD -MP
$(O)/.cflags: FORCE
	@mkdir -p $(@D)
	@echo '$(EE_CFLAGS) $(EE_DBGINFOFLAGS)' | cmp -s - $@ || echo '$(EE_CFLAGS) $(EE_DBGINFOFLAGS)' > $@
$(EE_OBJS): $(O)/.cflags
-include $(EE_OBJS:.o=.d)
FORCE:

# clean: code only (objects, ELF); distclean: everything exported from the game as well
clean:
	rm -f $(EE_BIN) $(EE_OBJS) $(EE_OBJS:.o=.d) $(O)/.cflags
distclean:
	rm -rf $(G) $(D) $(O) $(BUILD)/proto*.elf $(BUILD)/proto*.iso
	rm -f proto.elf proto_*.elf data

# --- Real hardware / ISO / profiling (tools-hwperf, notes/threads/tools-hwperf.md) ---
$(AREA_OBJ)/prof/main.o: src/main.c $(O)/.cflags
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DPROFILE $(AREA_DEFS) -c $< -o $@
$(O)/prof/ai.o: src/ai.c $(O)/.cflags
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DPROFILE -c $< -o $@
$(O)/prof/frame.o: src/frame.c $(O)/.cflags
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DPROFILE -c $< -o $@
$(O)/gprof/%.o: src/%.c $(O)/.cflags
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -pg -DPROFILE -DGPROF $(AREA_DEFS) -c $< -o $@
ifneq ($(MAP),$(DEFAULT_MAP))
$(O)/gprof/$(MAP)/%.o: src/%.c $(O)/.cflags
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -pg -DPROFILE -DGPROF $(AREA_DEFS) -c $< -o $@
endif
$(O)/align/%.o: src/%.c $(O)/.cflags
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -fsanitize=alignment $(AREA_DEFS) -c $< -o $@
ifneq ($(MAP),$(DEFAULT_MAP))
$(O)/align/$(MAP)/%.o: src/%.c $(O)/.cflags
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -fsanitize=alignment $(AREA_DEFS) -c $< -o $@
endif
# <elf>.sym.elf: the same link unstripped (.symtab for the PCSX2 debugger, addr2line, gprof; tools/ps2-pcsx2.sh)
SYM_ELF = $(EE_BIN:.elf=.sym.elf)
$(SYM_ELF): $(EE_OBJS)
	$(EE_CC) -T$(EE_LINKFILE) $(EE_OPTFLAGS) -o $@ $(EE_OBJS) $(EE_LDFLAGS) $(EXTRA_LDFLAGS) $(EE_LIBS)
# The Lab (Polaris 1.7b, lab/README.md): the player's simulate phase as a 32-bit host program driven over a pipe
# (tools/lab.py). 32-bit because the loaders keep pointers in 32-bit blob fields (TECH_DEBT core-1); needs a host gcc
# with -m32 (multilib): on the distrobox, `distrobox-host-exec make -C ps2/proto lab`.
LAB_CC ?= gcc -m32
LAB_CFLAGS ?= -std=gnu99 -O2 -g -msse2 -mfpmath=sse -ffp-contract=off -fno-fast-math -w -DCOL_TILES=0
LAB_SRCS = lab/lab_main.c lab/lab_pack.c lab/lab_stubs.c lab/lab_fio.c lab/lab_world.c $(addprefix src/,gamedata.c chunk.c flags.c progress.c chrstat.c \
	items.c player.c esd.c esd_env_move.c combat.c physics.c collision.c coltile.c nav.c status.c cvar.c vmath.c \
	model.c omit.c testscript.c save.c cam_input.c objects.c event.c ev_chr.c ev_obj.c ev_misc.c bonfire.c pickup.c world.c enemy.c ai.c lua0.c npc.c camera.c ragdoll.c drawgrp.c)
LAB_BIN = $(BUILD)/host/lab
lab: $(LAB_BIN)
$(LAB_BIN): $(LAB_SRCS) $(wildcard lab/*.h lab/stub/*.h src/*.h)
	@mkdir -p $(@D)
	$(LAB_CC) $(LAB_CFLAGS) -DLAB -Ilab/stub -Ilab -I$(GSKIT)/include -I$(PS2SDK)/common/include -Isrc $(LAB_SRCS) -o $@ -lm -Wl,--wrap=world_hold
.PHONY: lab

sym: $(SYM_ELF)
# make iso: bootable disc image <elf>.iso (SYSTEM.CNF, the ELF as DSRP_000.01, every pack in data/ under DATA\, see
# tools/ps2-iso.sh); booted from it, pack.c reads cdrom0:. ISO_SYMS=1 keeps the .symtab on the disc (PCSX2 debugger).
# It packages the packs already in data/ (run make first; iso itself never re-runs the exporters).
ISO = $(EE_BIN:.elf=.iso)
iso: $(SYM_ELF)
	NOSTRIP=$(ISO_SYMS) ../../tools/ps2-iso.sh $(SYM_ELF) $(D) $(ISO)
.PHONY: iso sym

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
