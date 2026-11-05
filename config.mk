# Build configuration shared by every Makefile: where the user's Dark Souls Remastered is, which Python runs the
# exporters, and where generated files go. Nothing here names a machine; per-machine values go in config.local.mk
# (git-ignored), e.g.
#     DSR_GAME = /path/to/steamapps/common/DARK SOULS REMASTERED
#     PY = /path/to/venv/bin/python
# or come from the environment / the make command line (make DSR_GAME=...). GAME=... is accepted as an alias.
DSR_ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
# DSR_LOCAL_CONFIG=0 skips it: tools/build-iso.sh's container mounts this checkout, and the host paths in it (PY, a venv
# under the host's home) don't exist in there
ifneq ($(DSR_LOCAL_CONFIG),0)
-include $(DSR_ROOT)/config.local.mk
endif
ifdef GAME
DSR_GAME := $(GAME)
endif
# Unset: tools/gamepath.py looks for a Steam install (Linux native / flatpak, Windows; every Steam library)
ifeq ($(strip $(DSR_GAME)),)
DSR_GAME := $(shell python3 -I $(DSR_ROOT)/tools/gamepath.py --quiet)
endif
GAME := $(DSR_GAME)
# Exporters need Pillow + numpy (tools/requirements.txt); a venv in the repo root (.venv) is used when present
PY ?= $(if $(wildcard $(DSR_ROOT)/.venv/bin/python),$(DSR_ROOT)/.venv/bin/python,python3)
# Everything generated from the game (exported .bin, generated headers, packs, objects, ELFs) goes here, never into
# the source tree. host: under PCSX2 = the ELF's folder, so packs sit in $(BUILD)/data next to the ELF.
BUILD ?= $(DSR_ROOT)/build

# Every goal except clean / distclean / help / elf (ps2/proto: the game-free ELF) needs the game: stop with a clear message when it is missing or an
# unsupported version (tools/verify_game.py; DSR_ALLOW_UNSUPPORTED=1 builds anyway).
ifneq ($(filter-out clean distclean help elf,$(or $(MAKECMDGOALS),all)),)
ifeq ($(strip $(DSR_GAME)),)
$(shell python3 -I $(DSR_ROOT)/tools/gamepath.py >&2)
$(error Dark Souls Remastered not found; set DSR_GAME (see above and README.md))
endif
ifneq ($(shell python3 -I $(DSR_ROOT)/tools/verify_game.py "$(DSR_GAME)" >&2; echo $$?),0)
$(error the game copy at "$(DSR_GAME)" failed tools/verify_game.py (see above))
endif
endif
