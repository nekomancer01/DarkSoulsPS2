# dsrsnd.irx: the IOP side of the sound runtime (dsrsnd.c + disc.c + snd.c, protocol in ../../src/dsrsnd_rpc.h). Built by
# ps2/proto/Makefile: make -C iop/dsrsnd IOP_OBJS_DIR=<obj dir>/ IOP_BIN=<obj dir>/dsrsnd.irx
IOP_BIN ?= dsrsnd.irx
all: $(IOP_BIN)
IOP_OBJS = dsrsnd.o disc.o snd.o imports.o
IOP_INCS += -I../../src
IOP_OPTFLAGS = -O2
IOP_WARNFLAGS = -Wall -Werror
IOP_CFLAGS += $(DSRSND_DEFS)
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
# the ps2sdk rule is %.o: %.c only: rebuild every C object when a shared header changes (after the includes, so this
# names the objects as Makefile.iopglobal does, with or without the obj dir prefix)
$(filter-out %imports.o,$(addprefix $(IOP_OBJS_DIR),$(notdir $(IOP_OBJS)))): dsrsnd_i.h ../../src/dsrsnd_rpc.h
