# dsrsnd.irx: the IOP side of the sound runtime (dsrsnd.c, protocol in ../../src/dsrsnd_rpc.h). Built by
# ps2/proto/Makefile: make -C iop/dsrsnd IOP_OBJS_DIR=<obj dir>/ IOP_BIN=<obj dir>/dsrsnd.irx
IOP_BIN ?= dsrsnd.irx
all: $(IOP_BIN)
IOP_OBJS = dsrsnd.o imports.o
IOP_INCS += -I../../src
IOP_OPTFLAGS = -O2
IOP_WARNFLAGS = -Wall -Werror
IOP_CFLAGS += $(DSRSND_DEFS)
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
