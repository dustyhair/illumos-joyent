#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

# Copyright 2026 Jonathan Wagner.

UTSBASE = ../..

MODULE = lx_input
OBJECTS = $(OBJS_DIR)/lx_input.o
ROOTMODULE = $(USR_DRV_DIR)/$(MODULE)
CONF_SRCDIR = $(UTSBASE)/common/brand/lx/io

include $(UTSBASE)/intel/Makefile.intel

ALL_TARGET = $(BINARY) $(SRC_CONFILE)
INSTALL_TARGET = $(BINARY) $(ROOTMODULE) $(ROOT_CONFFILE)

CPPFLAGS += -I$(UTSBASE)/common/brand/lx
LDFLAGS += -Nbrand/lx_brand

.KEEP_STATE:

def:		$(DEF_DEPS)

all:		$(ALL_DEPS)

clean:		$(CLEAN_DEPS)

clobber:	$(CLOBBER_DEPS)

install:	$(INSTALL_DEPS)

include $(UTSBASE)/intel/Makefile.targ

$(OBJS_DIR)/%.o: $(UTSBASE)/common/brand/lx/io/%.c
	$(COMPILE.c) -o $@ $<
	$(CTFCONVERT_O)
