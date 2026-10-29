# CDDL version 1.0, http://www.illumos.org/license/CDDL.
UTSBASE = ../..
MODULE = lx_namespace_ktest
OBJECTS = $(LX_NAMESPACE_KTEST_OBJS:%=$(OBJS_DIR)/%)
ROOTMODULE = $(USR_KTEST_DIR)/$(MODULE)

include $(UTSBASE)/intel/Makefile.intel
INC_PATH += -I$(UTSBASE)/common/brand/lx
ALL_TARGET = $(BINARY)
INSTALL_TARGET = $(BINARY) $(ROOTMODULE)
LDFLAGS += -N drv/ktest -N brand/lx_brand

.KEEP_STATE:
def: $(DEF_DEPS)
all: $(ALL_DEPS)
clean: $(CLEAN_DEPS)
clobber: $(CLOBBER_DEPS)
install: $(INSTALL_DEPS)
include $(UTSBASE)/intel/Makefile.targ
