# ELFWD - Generic PS2 ELF forwarder with argv spoofing and RAM patch support
# Copyright (c) 2026 Okeanos
#
# Licensed under the Academic Free License version 2.0
# See LICENSE for the full license text.
#
# Build rules structure adapted from the PS2SDK sample Makefiles
# (Copyright 2001-2004, ps2dev - http://www.ps2dev.org, AFL-2.0).

EE_BIN = ELFWD.ELF
EE_OBJS = main.o
EE_CFLAGS = -Os -ffunction-sections -fdata-sections
EE_LDFLAGS = -L$(PS2SDK)/ee/lib -Wl,--section-start,.text=0x01800000 -Wl,--gc-sections -s
EE_INCS = -I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include
EE_LIBS = -lkernel -lpatches

all: $(EE_BIN)

clean:
	rm -f $(EE_OBJS) $(EE_BIN)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
