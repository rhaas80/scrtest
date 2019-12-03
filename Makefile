# convert a strin to lower case
define lc
$(shell echo $(1) | tr A-Z a-z)
endef
# needed to have comma itself show up in Make commands
comma=,

# where to find SCR
SCR_HOME = $(PWD)/..

# construct options to link and compile in SCR and all components
SCR_COMPONENTS = scr spath kvtree rankstr AXL filo shuffile redset er

INCDIRS := $(patsubst %,-I$(SCR_HOME)/%/install/include,$(SCR_COMPONENTS))
LIBDIRS := $(patsubst %,-Wl$(comma)--rpath$(comma)$(SCR_HOME)/%/install/lib,$(SCR_COMPONENTS)) $(patsubst %,-L$(SCR_HOME)/%/install/lib,$(SCR_COMPONENTS))
LIBS = $(patsubst %,-l%,$(call lc,$(SCR_COMPONENTS)))

CC = mpicc
CFLAGS += -std=gnu99 -Wall -g $(INCDIRS)
# --disable-new-dtags is required to avoid RUNPATH issues messing up RPATH
LDFLAGS += $(LIBDIRS) -Wl,--disable-new-dtags
LDLIBS = $(LIBS)

.PHONY: clean

scrtest: scrtest.c Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f scrtest ckpt.*.txt
