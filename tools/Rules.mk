#  -*- mode: Makefile; -*-

# `all' is the default target
all:

include $(XEN_ROOT)/Config.mk

XEN_INCLUDE        = $(XEN_ROOT)/tools/include
XEN_XC             = $(XEN_ROOT)/tools/python/xen/lowlevel/xc
XEN_LIBXC          = $(XEN_ROOT)/tools/libxc
XEN_XENSTORE       = $(XEN_ROOT)/tools/xenstore
XEN_LIBXENSTAT     = $(XEN_ROOT)/tools/xenstat/libxenstat/src

X11_LDPATH = -L/usr/X11R6/$(LIBDIR)

CFLAGS += -D__XEN_TOOLS__

# Enable implicit LFS support *and* explicit LFS names.
CFLAGS  += $(shell getconf LFS_CFLAGS)
CFLAGS  += -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
LDFLAGS += $(shell getconf LFS_LDFLAGS)

# 32-bit x86 does not perform well with -ve segment accesses on Xen.
CFLAGS-$(CONFIG_X86_32) += $(call cc-option,$(CC),-mno-tls-direct-seg-refs)
CFLAGS += $(CFLAGS-y)

# Require GCC v3.4+ (to avoid issues with alignment constraints in Xen headers)
check-$(CONFIG_X86) = $(call cc-ver-check,CC,0x030400,\
                        "Xen requires at least gcc-3.4")
$(eval $(check-y))

%.opic: %.c
	$(CC) $(CPPFLAGS) -DPIC $(CFLAGS) -fPIC -c -o $@ $<

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.o: %.cc
	$(CC) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

