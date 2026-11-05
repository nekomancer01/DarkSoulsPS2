# DSR -> PS2 character prototype. `make` exports everything it needs from the user's own copy of the game (DSR_GAME,
# see ../../config.mk and README.md) into $(BUILD) (default ../../build), then builds $(BUILD)/proto.elf.
# Nothing generated is written to the source tree; ./proto.elf and ./data are convenience links into $(BUILD).
include ../../config.mk
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
MAP ?= m10_02_00_00
DEFAULT_MAP = m10_02_00_00
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
$(TS)/%.sum: FORCE
	@mkdir -p $(@D)
	@python3 -I ../../tools/srcsum.py ../../tools/$* $@
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

EE_OBJS = $(AREA_OBJS) $(addprefix $(O)/,render.o player.o model.o map.o mapstream.o vu1.o vu1map.o vu1skin2.o vu1clip.o \
	vu1map2.o vu1skinclip.o collision.o esd.o esd_env_move.o combat.o pack.o hud.o menu.o lua0.o ai.o objects.o physics.o \
	texcache.o testscript.o bump.o fx.o drawgrp.o world.o flags.o progress.o save.o event.o ev_chr.o ev_obj.o ev_misc.o bonfire.o npc.o \
	gamedata.o ragdoll.o nav.o items.o drb.o loading.o sound.o vmath.o)
EE_INCS += -I$(GSKIT)/include -I$(G) -Isrc
EE_LDFLAGS += -L$(GSKIT)/lib
EE_LDFLAGS += -Wl,--wrap=_write  # src/pack.c: writes wait for a pending async read
EE_LIBS = -lgskit -ldmakit -lpacket2 -ldma -lpad -lmc -lpatches -lm
EE_DVP = dvp-as
# Float math (refactor target 3, 2026-11-05): the game code does no double arithmetic (tools/ci/compile.sh fails on
# any), and sqrtf is already an inline sqrt.s. -fno-math-errno lets gcc treat sinf/cosf/... as pure (merged repeat
# calls); nothing reads errno. -ffinite-math-only and -ffast-math stay off: collision and the ragdoll rely on exact
# compares. MATHFLAGS= builds without, for A/B profiling.
MATHFLAGS ?= -fno-math-errno
EE_CFLAGS += -O2 -std=gnu99 -Wall $(MATHFLAGS) $(DEFS)

# Profiling builds (tools-hwperf: src/prof.c, notes/threads/tools-hwperf.md); the shared objects and proto.elf stay as
# they are. make PROFILE=1 -> proto_prof.elf: main.c with -DPROFILE (per-pass EE cycles + I$ / D$ misses from the COP0
# performance counters, on the HUD and in the log). make GPROF=1 -> proto_gprof.elf: every C file with -pg in
# $(O)/gprof/, ps2sdk libprofglue sampling, host:gmon.out after PROF_GPROF_FRAMES frames (tools/ps2-gprof.sh).
AREA_DEFS = $(if $(filter-out $(DEFAULT_MAP),$(MAP)),-DAREA='"$(MAP)"' -DCHR_DIR='"$(MAP)/"')
ifeq ($(PROFILE),1)
EE_BIN := $(EE_BIN:.elf=_prof.elf)
EE_OBJS := $(filter-out $(AREA_OBJ)/main.o,$(EE_OBJS)) $(AREA_OBJ)/prof/main.o $(O)/prof.o
endif
ifeq ($(GPROF),1)
EE_BIN := $(EE_BIN:.elf=_gprof.elf)
VSM_OBJS = $(patsubst src/%.vsm,$(O)/%.o,$(wildcard src/*.vsm))
EE_OBJS := $(patsubst $(O)/%,$(O)/gprof/%,$(filter-out $(VSM_OBJS),$(EE_OBJS)) $(O)/prof.o) $(filter $(VSM_OBJS),$(EE_OBJS))
EE_LIBS += -lprofglue
endif
# (the rules below come before `all:`; keep it the default goal)
.DEFAULT_GOAL := all
# Sound (src/sound.c): the IOP module iop/dsrsnd (dsrsnd.irx) and ps2sdk's libsd.irx, linked in as C arrays (bin2c).
# After the PROFILE / GPROF lists: these objects have no source in src/.
IRX_OBJS = $(O)/irx/dsrsnd_irx.o $(O)/irx/libsd_irx.o
EE_OBJS += $(IRX_OBJS)
$(O)/irx/dsrsnd.irx: iop/dsrsnd/dsrsnd.c iop/dsrsnd/imports.lst iop/dsrsnd/irx_imports.h iop/dsrsnd/Makefile src/dsrsnd_rpc.h
	@mkdir -p $(@D)
	$(MAKE) --no-print-directory -C iop/dsrsnd IOP_OBJS_DIR=$(abspath $(@D))/ IOP_BIN=$(abspath $@)
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
# FLVER LOD per character: LOD1 for the two most common melee types, LOD2 for the rest (EE RAM: skin streams)
LOD_c1201 = 2
LOD_c2510 = 2
LOD_c2550 = 2
LOD_c2750 = 2
LOD_c2910 = 2
LOD_c3510 = 2
LOD_c5330 = 2
# Texture height cap (px; width stays >= 128 for PSMT8): every type's textures stay in VRAM next to the map's
TEX_c2540 = 128
TEX_c2900 = 128
PACKS = $(D)/c0000.pak $(D)/$(OUT).pak $(D)/menu.pak $(ENEMIES:%=$(AREA_DATA)/%.pak) $(AREA_DATA)/common.pak \
	$(if $(BLOCK),,$(A)/snd.stamp)
# World streaming (src/world.c): the other blocks of this boot area's world (tools/area.py WORLDS, "<msb>:<block>"),
# each built by a sub-make, and data/world.pak with their table (tools/export_world.py). Only for the default map.
WORLD_BLOCKS := $(if $(GAMEFREE)$(BLOCK)$(filter-out $(DEFAULT_MAP),$(MAP)),,$(shell python3 -I ../../tools/area.py --world $(MAP)))
ifneq ($(WORLD_BLOCKS),)
PACKS += worldpacks $(D)/world.pak
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
worldpacks:
	@$(MAKE) --no-print-directory $(if $(WORLD_J),-j$(WORLD_J)) $(addprefix worldblock@,$(subst :,@,$(WORLD_BLOCKS)))
worldblock@%:
	@b='$*'; $(MAKE) --no-print-directory MAP=$${b%%@*} BLOCK=$${b#*@} blockpacks
.PHONY: worldpacks
$(D)/world.pak: worldpacks $(TS)/export_world.py.sum $(AREA_PY) $(A)/collision.bin
	@mkdir -p $(@D)
	$(PY) -I ../../tools/export_world.py "$(GAME)" $(G)/world.bin $(MAP) $(G)
	python3 -I ../../tools/pack.py $@ world.bin=$(G)/world.bin
.PHONY: $(notdir $(EE_BIN))

# Sound packs (tools/export_audio.py, src/sound.c): data/snd/main.pak (player + global sounds), mAA.pak per area
# (music, ambience, MSB emitters), cXXXX.pak per enemy type, for the boot map and every map of its world blocks. One
# export writes them all (the SPU2 sample rate is chosen over the whole set); $(A)/snd.stamp records it.
SND_MAPS = $(sort $(MAP) $(foreach b,$(WORLD_BLOCKS),$(firstword $(subst :, ,$(b)))))
# progress goes to the terminal and the log (tee); pipefail keeps the exporter's exit status
$(A)/snd.stamp: SHELL := /bin/bash
$(A)/snd.stamp: .SHELLFLAGS := -o pipefail -c
$(A)/snd.stamp: $(TS)/export_audio.py.sum $(TS)/fev.py.sum $(TS)/fsb4.py.sum $(TS)/souls.py.sum $(TS)/pack.py.sum \
		$(TS)/adpcm/adpcm.py.sum $(TS)/adpcm/vagenc.c.sum $(TS)/native/lz4pack.c.sum $(TS)/area.py.sum
	@mkdir -p $(@D) $(D)/snd
	@echo "sound: exporting every bank of $(SND_MAPS) (a few minutes; log in $(A)/snd.log)"
	$(PY) -I ../../tools/export_audio.py $(D)/snd $(A)/snd $(SND_MAPS) | tee $(A)/snd.log
	touch $@

# player.gd / combat.gd / progress.gd: the game values the engine loads at boot (src/gamedata.c, tools/gamedata.py)
GAMEDATA = $(G)/player.gd $(G)/combat.gd $(G)/progress.gd
$(D)/c0000.pak: $(G)/model.bin $(G)/esd.bin $(GAMEDATA) $(TS)/pack.py.sum $(TS)/native/lz4pack.c.sum
	@mkdir -p $(@D)
	python3 -I ../../tools/pack.py $@ model.bin=$(G)/model.bin esd.bin=$(G)/esd.bin $(foreach f,$(GAMEDATA),$(notdir $(f))=$(f))

# The player's action logic: c0000.esd (state groups, conditions, bytecode) packed for src/esd.c
$(G)/esd.bin: $(TS)/export_esd.py.sum $(TS)/esd.py.sum $(TS)/souls.py.sum
	@mkdir -p $(@D)
	python3 -I ../../tools/export_esd.py "$(GAME)" $@

$(D)/$(OUT).pak: $(A)/map.bin $(if $(WORLD_SPLIT),$(A)/maptab.bin) $(A)/bump.bin $(A)/dg.bin $(A)/collision.bin $(A)/enemies.bin $(A)/objects.bin $(A)/ai_map.lua0 $(A)/events.bin $(TS)/pack.py.sum \
		$(TS)/native/lz4pack.c.sum $(A)/fx.bin
	@mkdir -p $(@D)
	python3 -I ../../tools/pack.py $@ map.bin=$(A)/map.bin collision.bin=$(if $(BLOCK),raw:)$(A)/collision.bin enemies.bin=$(if $(BLOCK),raw:)$(A)/enemies.bin \
		objects.bin=$(if $(BLOCK),raw:)$(A)/objects.bin ai.lua0=$(if $(BLOCK),raw:)$(A)/ai_map.lua0 evcommon.bin=$(A)/evcommon.bin bump.bin=$(if $(BLOCK),raw:)$(A)/bump.bin fx.bin=$(if $(BLOCK),raw:)$(A)/fx.bin dg.bin=$(A)/dg.bin \
		$(if $(wildcard $(A)/cells.bin),cells.bin=raw:$(A)/cells.bin) $(if $(wildcard $(A)/pano.bin),pano.bin=$(A)/pano.bin) \
		$(if $(WORLD_SPLIT),maptab.bin=raw:$(A)/maptab.bin mapres.bin=raw:$(A)/mapres.bin) \
		$(if $(wildcard $(A)/events.bin),events.bin=$(if $(BLOCK),raw:)$(A)/events.bin)
# world builds: map.bin split for lite / full blocks (tools/mapsplit.py, src/world.c), read raw in the background
$(A)/maptab.bin $(A)/mapres.bin: $(A)/map.bin $(TS)/mapsplit.py.sum
	python3 -I ../../tools/mapsplit.py $< $(A)/maptab.bin $(A)/mapres.bin

# Objects: src/*.c -> $(O)/*.o; area-dependent objects of a non-default area go to $(O)/<MAP>/
$(O)/%.o: src/%.c $(O)/.cflags
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@
ifneq ($(MAP),$(DEFAULT_MAP))
$(O)/$(MAP)/%.o: src/%.c $(O)/.cflags
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -DAREA='"$(MAP)"' -DCHR_DIR='"$(MAP)/"' -c $< -o $@
endif

# Breakable objects (pots) of the area: intact + pre-fractured models, piece bodies, placements
# (types over the RAM budget are listed in objects.bin.static and drawn by the map as static objects)
# AREA_PY: the whole of area.py (world.pak reads every block); per-area exports depend on $(A)/area.sig instead
AREA_PY = $(TS)/area.py.sum
AREA_SIG = $(A)/area.sig
$(A)/area.sig: FORCE
	@mkdir -p $(@D)
	@python3 -I ../../tools/area.py --sig $(MAP) $@
# + animated objects (doors, levers, gates, chests: objects.bin v3); their part names in objects.bin.anim, which the
# map leaves out of its static bake and the events out of their static colliders
$(A)/objects.bin: $(TS)/export_objects.py.sum $(TS)/resample.py.sum $(TS)/export_model.py.sum $(TS)/souls.py.sum $(TS)/export_chr.py.sum \
		$(TS)/emevd.py.sum $(TS)/havok_anim.py.sum $(AREA_SIG)
	@mkdir -p $(@D)
	$(PY) -I ../../tools/export_objects.py "$(GAME)" $@ $(MAP)
# written by the objects.bin export, only when their content changes (map.bin depends on these, not objects.bin)
$(A)/objects.bin.static $(A)/objects.bin.anim: $(A)/objects.bin
	@test -f $@ || { echo "$@ missing: re-export objects.bin"; rm -f $<; exit 1; }

# MSB enemy parts of the area (model, NpcParam / ThinkParam ids, position, yaw)
# EMEVD bytecode (src/event.c): the map's events + entity table (hit ranges of this area / block's collision.bin),
# common.emevd as evcommon.bin; coverage per map in events.txt
$(A)/events.bin: $(TS)/export_emevd.py.sum $(TS)/emevd.py.sum $(TS)/export_collision.py.sum $(TS)/havok_collision.py.sum \
		$(TS)/souls.py.sum $(AREA_SIG) $(A)/objects.bin
	@mkdir -p $(@D)
	ANIM_OBJ=$(A)/objects.bin.anim $(PY) -I ../../tools/export_emevd.py "$(GAME)" $@ $(MAP) --common $(A)/evcommon.bin --report $(A)/events.txt
$(A)/evcommon.bin: $(A)/events.bin

# (export_progress.py: the humanity zone keys; params.py: NpcParam getSoul / humanityLotId, ItemLotParam)
$(A)/enemies.bin: $(TS)/export_enemies.py.sum $(TS)/souls.py.sum $(TS)/havok_anim.py.sum $(TS)/export_progress.py.sum \
		$(TS)/gamedata.py.sum $(TS)/params.py.sum $(AREA_SIG)
	@mkdir -p $(@D)
	python3 -I ../../tools/export_enemies.py "$(GAME)" $@ $(MAP)

# Enemy / NPC character: FLVER, skeleton, anims, TAE windows, one variant per placed NpcParam / ThinkParam pair
# (enemy_parts lives in area.py: an export_enemies.py edit re-exports enemies.bin only)
CHR_DEPS = $(TS)/export_chr.py.sum $(TS)/resample.py.sum $(TS)/export_model.py.sum $(TS)/havok_anim.py.sum \
	$(TS)/havok_ragdoll.py.sum \
	$(TS)/lua50.py.sum $(TS)/souls.py.sum $(AREA_SIG)
$(A)/c%/chr.bin: $(CHR_DEPS)
	@mkdir -p $(@D)
	$(PY) -I ../../tools/export_chr.py "$(GAME)" c$* $(MAP) $(A)/c$* $(or $(LOD_c$*),1) $(or $(TEX_c$*),64)

$(AREA_DATA)/c%.pak: $(A)/c%/chr.bin $(TS)/pack.py.sum $(TS)/native/lz4pack.c.sum
	@mkdir -p $(@D)
	python3 -I ../../tools/pack.py $@ model.bin=$(A)/c$*/model.bin chr.bin=$(A)/c$*/chr.bin

.PRECIOUS: $(A)/c%/chr.bin

# DSR's AI scripts (Lua 5.0 bytecode) re-packed for the lua0 VM
$(A)/ai_common.lua0: $(TS)/lua50.py.sum $(TS)/souls.py.sum $(AREA_SIG)
	@mkdir -p $(@D)
	$(PY) -I ../../tools/lua50.py --pack --msb "$(GAME)" $(MAP) $@ "$(GAME)/script/aiCommon.luabnd.dcx"
$(A)/ai_map.lua0: $(TS)/lua50.py.sum $(TS)/souls.py.sum $(AREA_SIG)
	@mkdir -p $(@D)
	$(PY) -I ../../tools/lua50.py --pack --msb "$(GAME)" $(MAP) $@ "$(GAME)/script/$(MAP).luabnd.dcx"

# Shared enemy behaviour: enemyCommon.esd (every enemy's ESD in DS1)
$(G)/enemyCommon.esd.bin: $(TS)/export_esd.py.sum $(TS)/esd.py.sum $(TS)/souls.py.sum
	@mkdir -p $(@D)
	python3 -I ../../tools/export_esd.py "$(GAME)" $@ enemyCommon

$(AREA_DATA)/common.pak: $(G)/enemyCommon.esd.bin $(A)/ai_common.lua0 $(TS)/pack.py.sum $(TS)/native/lz4pack.c.sum
	@mkdir -p $(@D)
	python3 -I ../../tools/pack.py $@ esd.bin=$(G)/enemyCommon.esd.bin ai.lua0=$(A)/ai_common.lua0

# The layouts of these records are the committed src/*_data.h (tools/gamedata.py header <name>); the ELF needs no game
$(G)/player.gd: $(TS)/export_player.py.sum $(TS)/gamedata.py.sum $(TS)/souls.py.sum
	@mkdir -p $(@D)
	python3 -I ../../tools/export_player.py "$(GAME)" $@

# New-game progression (src/progress.c): CharaInitParam of the starting class
$(G)/progress.gd: $(TS)/export_progress.py.sum $(TS)/gamedata.py.sum $(TS)/params.py.sum $(TS)/souls.py.sum
	@mkdir -p $(@D)
	python3 -I ../../tools/export_progress.py "$(GAME)" $@

# HP / damage core: stats, CalcCorrectGraph rows, weapon dummy polys, AtkParam windows, the debug dummy's NpcParam
$(G)/combat.gd: $(TS)/export_combat.py.sum $(TS)/gamedata.py.sum $(TS)/havok_anim.py.sum $(TS)/souls.py.sum
	@mkdir -p $(@D)
	python3 -I ../../tools/export_combat.py "$(GAME)" $@

$(O)/combat.o: src/combat.h
$(O)/player.o $(O)/main.o $(O)/enemy.o: src/combat.h
$(O)/enemy.o: src/enemy.h src/ai.h src/model.h src/esd.h src/ragdoll.h src/nav.h
$(O)/ragdoll.o: src/ragdoll.h src/model.h src/collision.h
$(O)/nav.o: src/nav.h
$(O)/ai.o: src/ai.h src/lua0.h
$(O)/lua0.o: src/lua0.h
$(O)/main.o $(O)/player.o: src/model.h

# Real mesh, textures and Havok animations
$(G)/model.bin: $(TS)/export_model.py.sum $(TS)/resample.py.sum $(TS)/havok_anim.py.sum $(TS)/souls.py.sum
	@mkdir -p $(@D)
	$(PY) -I ../../tools/export_model.py "$(GAME)" $@


# HUD and menus: DSR's menu sprites (menu.drb layout), DSFont24, item icons, English FMG text (tools/export_menu.py)
$(G)/menu.bin: $(TS)/export_menu.py.sum $(TS)/resample.py.sum $(TS)/octree.py.sum $(TS)/souls.py.sum
	@mkdir -p $(@D)
	$(PY) -I ../../tools/export_menu.py "$(GAME)" $@

# Item database (src/items.c): stack sizes / kinds (resident), names, stats, descriptions and icons (read while a menu
# is open; desc.bin and icons.bin stored raw for reads in place) (tools/export_items.py)
ITEM_FILES = $(addprefix $(G)/items/,items.bin itemx.bin desc.bin icons.bin)
$(G)/items/.stamp: $(TS)/export_items.py.sum $(TS)/resample.py.sum $(TS)/souls.py.sum
	@mkdir -p $(@D)
	$(PY) -I ../../tools/export_items.py "$(GAME)" $(@D)
	@touch $@
$(ITEM_FILES): $(G)/items/.stamp ;
# Menu screens (src/drb.c): DSR's menu.drb dialog trees at their own layout with their sprite atlases
# (tools/export_screens.py SCREENS)
SCREEN_NAMES = R00 I02 I01 R02 I06
SCREEN_FILES = $(SCREEN_NAMES:%=$(G)/screens/scr_%.bin)
$(G)/screens/.stamp: $(TS)/export_screens.py.sum $(TS)/export_menu.py.sum $(TS)/export_items.py.sum $(TS)/resample.py.sum \
		$(TS)/octree.py.sum $(TS)/souls.py.sum
	@mkdir -p $(@D)
	$(PY) -I ../../tools/export_screens.py "$(GAME)" $(@D)
	@touch $@
$(SCREEN_FILES): $(G)/screens/.stamp ;

# Loading screen (src/loading.c): menu.drb E00_00, the nowloading.tpf bonfire cells, DSFont24, the exe's hint table with
# the items' names and long descriptions (tools/export_loading.py; the icons are icons.bin's)
$(G)/loading.bin: $(TS)/export_loading.py.sum $(TS)/export_screens.py.sum $(TS)/export_menu.py.sum \
		$(TS)/export_items.py.sum $(TS)/resample.py.sum $(TS)/octree.py.sum $(TS)/souls.py.sum
	@mkdir -p $(@D)
	$(PY) -I ../../tools/export_loading.py "$(GAME)" $@

$(D)/menu.pak: $(G)/menu.bin $(ITEM_FILES) $(SCREEN_FILES) $(G)/loading.bin $(TS)/pack.py.sum $(TS)/native/lz4pack.c.sum
	@mkdir -p $(@D)
	python3 -I ../../tools/pack.py $@ menu.bin=$(G)/menu.bin items.bin=$(G)/items/items.bin itemx.bin=$(G)/items/itemx.bin \
		desc.bin=raw:$(G)/items/desc.bin icons.bin=raw:$(G)/items/icons.bin $(foreach n,$(SCREEN_NAMES),scr_$(n).bin=$(G)/screens/scr_$(n).bin) \
		loading.bin=$(G)/loading.bin

# Firelink Shrine: map pieces, textures, navmesh, area fog. Tuning: BUDGET (triangles after quadric simplification,
# foliage not counted), SPAWN_K (detail falloff with distance from the spawn), GRASS_KEEP (1 foliage tuft in N kept,
# ivy 1 in N / 2), FOL_XZ / FOL_Y / FOL_IVY (kept tufts widened to make up for the rest); see tools/export_map.py.
# The fog section numbers collision triangles like export_collision.py, hence its dependencies.
# Per-area overrides of these knobs: MAPENV_<area> (appended, later assignments win).
MAPENV = BUDGET=76000 SPAWN_K=0.6 GRASS_KEEP=4 FOL_XZ=1.3 FOL_Y=1.1 FOL_IVY=1.2
# Per-area cap of normal-map relief batches a frame, nearest first: MAPENV_<area> += BUMP_CAP=n (tools/export_normal.py;
# none since the 30 fps target)
# Firelink (render-distance, 2026-10-31): two levels like Undead Burg below (STREAM=1): near 3 px judged at max(4 m,
# distance to the navmesh) streamed per 32 m cell, coarse 12 px judged at >= 32 m resident
MAPENV_m10_02_00_00 += STREAM=1 SPAWN_K=0 EPS=0.0000279 EPS_FAR=0.000198 STREAM_D=24
# Undead Burg (render-distance): two levels (STREAM=1, map.bin DSM5 + cells.bin): coarse 12 px judged at >= 32 m
# resident (140k tris, map.bin 10.1 MB), near 6 px (3 px was 150k tris within 24 m at the densest smoke point, 15 fps)
# streamed per 32 m cell from cells.bin (src/mapstream.c, 8 MB pool nearest first, 4 MB heap kept free)
MAPENV_m10_01_00_00 += STREAM=1 SPAWN_K=0 EPS=0.000112 EPS_FAR=0.000446
# Undead Asylum (world block m18_01_00_00_a): two levels like Firelink (STREAM=1, which world blocks need), with
# Firelink's errors and near radius (ASSUMED: not tuned on the Asylum yet, TECH_DEBT world-7)
MAPENV_m18_01_00_00 += STREAM=1 SPAWN_K=0 EPS=0.0000279 EPS_FAR=0.000198 STREAM_D=24
# libmapx (tools/mapx: the map exporter's C rewrite, tools/mapx_native.py): every C source and header
MAPX_SUMS = $(TS)/mapx_native.py.sum $(patsubst ../../tools/%,$(TS)/%.sum,$(wildcard ../../tools/mapx/*.c ../../tools/mapx/*.h))
$(A)/map.bin: $(TS)/export_map.py.sum $(MAPX_SUMS) $(TS)/resample.py.sum $(TS)/texmap.py.sum $(TS)/octree.py.sum $(TS)/simplify.py.sum $(TS)/souls.py.sum $(TS)/export_collision.py.sum \
		$(TS)/export_normal.py.sum $(TS)/dsr_light.py.sum \
		$(TS)/drawgroups.py.sum $(TS)/pose_obj.py.sum $(TS)/simplify_pool.py.sum $(TS)/simplify_native.py.sum \
		$(TS)/native/simplify.c.sum $(TS)/native/geom.c.sum $(TS)/geom_native.py.sum $(TS)/emevd.py.sum \
		$(TS)/havok_collision.py.sum $(AREA_SIG) $(A)/objects.bin.static $(A)/objects.bin.anim $(A)/lights.json
	@mkdir -p $(@D)
	env $(MAPENV) $(MAPENV_$(MAP)) STATIC_OBJ=$(A)/objects.bin.static ANIM_OBJ=$(A)/objects.bin.anim FX_LIGHTS_FILE=$(A)/lights.json \
		$(PY) -I ../../tools/export_map.py "$(GAME)" $@ $(MAP)
# the map bake's static point lights (export_effects.static_lights), rewritten only when they change
$(A)/lights.json: $(TS)/export_effects.py.sum $(TS)/ffx.py.sum $(TS)/dsr_light.py.sum $(TS)/souls.py.sum $(AREA_SIG)
	@mkdir -p $(@D)
	@# a first lights.json takes the export_effects.py stamp's mtime: adding it to a build re-exports no map
	@test -e $@ || new=1; $(PY) -I ../../tools/export_effects.py "$(GAME)" --static-lights $@ $(MAP) && \
		if [ -n "$$new" ]; then touch -r $(TS)/export_effects.py.sum $@; fi
# normal-map relief (tools/export_normal.py, src/bump.c): written by the map.bin export, same chunk order
$(A)/bump.bin: $(A)/map.bin
	@test -f $@ || { echo "$@ missing: re-export map.bin"; rm -f $<; exit 1; }
# MSB draw groups per chunk + collision triangle (tools/drawgroups.py, src/drawgrp.c): written by the map.bin export
$(A)/dg.bin: $(A)/map.bin
	@test -f $@ || { echo "$@ missing: re-export map.bin"; rm -f $<; exit 1; }
# effects (tools/export_effects.py, src/fx.c): particle systems, textures, effect lights; colours use the map bake's
# tone map (fx_tone.json, written by the map.bin export)
$(A)/fx.bin: $(TS)/export_effects.py.sum $(TS)/resample.py.sum $(TS)/octree.py.sum $(TS)/ffx.py.sum $(TS)/dsr_light.py.sum $(TS)/souls.py.sum $(AREA_SIG) $(A)/map.bin
	@mkdir -p $(@D)
	$(PY) -I ../../tools/export_effects.py "$(GAME)" $@ $(MAP) --tone $(A)/fx_tone.json


# Map collision from the Havok hkx files (h = high detail; l = low detail, 0.24 MB smaller): tools/export_collision.py
# (PY: havok_collision.py gathers the vertices with numpy)
$(A)/collision.bin: $(TS)/export_collision.py.sum $(TS)/havok_collision.py.sum $(TS)/souls.py.sum $(AREA_SIG)
	@mkdir -p $(@D)
	$(PY) -I ../../tools/export_collision.py "$(GAME)" $@ $(MAP) h


# VU1 microprograms (hand-scheduled VU assembly)
$(O)/%.o: src/%.vsm
	@mkdir -p $(@D)
	$(EE_DVP) $< -o $@

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
$(O)/gprof/%.o: src/%.c $(O)/.cflags
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -pg -DPROFILE -DGPROF $(AREA_DEFS) -c $< -o $@
ifneq ($(MAP),$(DEFAULT_MAP))
$(O)/gprof/$(MAP)/%.o: src/%.c $(O)/.cflags
	@mkdir -p $(@D)
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -pg -DPROFILE -DGPROF $(AREA_DEFS) -c $< -o $@
endif
# <elf>.sym.elf: the same link unstripped (.symtab for the PCSX2 debugger, addr2line, gprof; tools/ps2-pcsx2.sh)
SYM_ELF = $(EE_BIN:.elf=.sym.elf)
$(SYM_ELF): $(EE_OBJS)
	$(EE_CC) -T$(EE_LINKFILE) $(EE_OPTFLAGS) -o $@ $(EE_OBJS) $(EE_LDFLAGS) $(EXTRA_LDFLAGS) $(EE_LIBS)
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
