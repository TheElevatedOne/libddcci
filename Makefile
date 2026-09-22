# libddcci — DDC/CI over Linux I2C
#
#   make            build the library, the probe tool, and the pkg-config file
#   make test       unit tests and a link check (no monitor, no root)
#   make install    install under PREFIX (default /usr/local)
#   make uninstall
#
#   make PREFIX=/usr install
#   make DESTDIR=/tmp/stage install
#   make CFLAGS='-g -O0' test
#   make WERROR=1
#
# There is nothing to link against besides libc. linux/i2c.h and
# linux/i2c-dev.h come from the kernel UAPI headers (linux-libc-dev,
# kernel-headers, or linux-api-headers). i2c-tools is not required to build;
# install it for the i2c group and the /dev/i2c-* udev rule.

PREFIX       ?= /usr/local
LIBDIR       ?= $(PREFIX)/lib
INCLUDEDIR   ?= $(PREFIX)/include
BINDIR       ?= $(PREFIX)/bin
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig
DESTDIR      ?=

VERSION := 0.2.0
SOMAJOR := 1

CC      ?= gcc
AR      ?= ar
INSTALL ?= install

ifeq ($(WERROR),1)
  WERROR_FLAG := -Werror
endif

ALL_CFLAGS := -std=c11 -fPIC -fstack-protector-strong \
	-Wall -Wextra -Wpedantic -Wshadow -Wmissing-prototypes \
	-Wstrict-prototypes -Wwrite-strings -O2 $(WERROR_FLAG) $(CFLAGS)
LIB_CFLAGS := $(ALL_CFLAGS) -fvisibility=hidden -DDDCCI_BUILD
APP_CFLAGS := $(ALL_CFLAGS)
ALL_LDFLAGS := -Wl,-z,relro -Wl,-z,now $(LDFLAGS)
CPPFLAGS += -Iinclude -Isrc
DEPFLAGS := -MMD -MP

SRC := src/ddcci.c src/ddcci_enum.c src/ddcci_i2c.c src/ddcci_parse.c
OBJ := $(SRC:src/%.c=build/%.o)
DEP := $(OBJ:.o=.d)

.DELETE_ON_ERROR:

.PHONY: all test check probe install uninstall clean help

all: build/libddcci.a build/libddcci.so.$(VERSION) build/ddcci-probe build/libddcci.pc

help:
	@echo "targets:   all test check probe install uninstall clean"
	@echo "variables: PREFIX DESTDIR CC CFLAGS LDFLAGS WERROR=1"
	@echo "version:   $(VERSION)"

build:
	mkdir -p build

build/%.o: src/%.c include/ddcci.h src/ddcci_priv.h | build
	$(CC) $(LIB_CFLAGS) $(CPPFLAGS) $(DEPFLAGS) -c $< -o $@

build/libddcci.a: $(OBJ)
	$(AR) rcs $@ $(OBJ)

build/libddcci.so.$(VERSION): $(OBJ) src/libddcci.map
	$(CC) -shared $(ALL_LDFLAGS) \
		-Wl,-soname,libddcci.so.$(SOMAJOR) \
		-Wl,--version-script,src/libddcci.map \
		-Wl,-z,defs \
		-o $@ $(OBJ)
	ln -sfn libddcci.so.$(VERSION) build/libddcci.so.$(SOMAJOR)
	ln -sfn libddcci.so.$(SOMAJOR) build/libddcci.so

build/libddcci.pc: Makefile | build
	printf '%s\n' \
		'prefix=$(PREFIX)' \
		'exec_prefix=$${prefix}' \
		'libdir=$(LIBDIR)' \
		'includedir=$(INCLUDEDIR)' \
		'' \
		'Name: libddcci' \
		'Description: DDC/CI (VESA MCCS) access over Linux I2C' \
		'Version: $(VERSION)' \
		'Libs: -L$${libdir} -lddcci' \
		'Cflags: -I$${includedir}' \
		> $@

# The probe tool is linked from the object files so ./build/ddcci-probe runs
# without an rpath or ldconfig. Applications should use pkg-config and the
# shared library.
build/ddcci-probe: examples/ddcci-probe.c $(OBJ)
	$(CC) $(APP_CFLAGS) $(CPPFLAGS) -o $@ examples/ddcci-probe.c $(OBJ) $(ALL_LDFLAGS)

build/test_ddcci: tests/test_ddcci.c $(OBJ)
	$(CC) $(APP_CFLAGS) $(CPPFLAGS) -o $@ tests/test_ddcci.c $(OBJ) $(ALL_LDFLAGS)

# Name the archive directly. -lddcci would prefer libddcci.so when both exist.
build/link_static: tests/link_main.c build/libddcci.a
	$(CC) $(APP_CFLAGS) $(CPPFLAGS) -o $@ tests/link_main.c build/libddcci.a $(ALL_LDFLAGS)

build/link_shared: tests/link_main.c build/libddcci.so.$(VERSION)
	$(CC) $(APP_CFLAGS) $(CPPFLAGS) -o $@ tests/link_main.c -Lbuild -lddcci \
		-Wl,-rpath,'$$ORIGIN' $(ALL_LDFLAGS)

test: build/test_ddcci build/link_static build/link_shared
	grep -q '"$(VERSION)"' include/ddcci.h
	./build/test_ddcci
	./build/link_static
	./build/link_shared
	nm -D build/libddcci.so.$(VERSION) | grep -q ' T ddcci_open@@DDCCI_1.0'
	nm -D build/libddcci.so.$(VERSION) | grep -q ' T ddcci_save_settings@@DDCCI_1.1'
	@if nm -D build/libddcci.so.$(VERSION) | grep -q ' ddcci_pack_getvcp'; then \
		echo "internal symbol ddcci_pack_getvcp was exported"; exit 1; \
	fi

check: test

probe: build/ddcci-probe
	./build/ddcci-probe

install: all
	$(INSTALL) -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCLUDEDIR) \
		$(DESTDIR)$(BINDIR) $(DESTDIR)$(PKGCONFIGDIR)
	$(INSTALL) -m 644 include/ddcci.h $(DESTDIR)$(INCLUDEDIR)/ddcci.h
	$(INSTALL) -m 644 build/libddcci.a $(DESTDIR)$(LIBDIR)/libddcci.a
	$(INSTALL) -m 755 build/libddcci.so.$(VERSION) \
		$(DESTDIR)$(LIBDIR)/libddcci.so.$(VERSION)
	ln -sfn libddcci.so.$(VERSION) $(DESTDIR)$(LIBDIR)/libddcci.so.$(SOMAJOR)
	ln -sfn libddcci.so.$(SOMAJOR) $(DESTDIR)$(LIBDIR)/libddcci.so
	$(INSTALL) -m 755 build/ddcci-probe $(DESTDIR)$(BINDIR)/ddcci-probe
	$(INSTALL) -m 644 build/libddcci.pc $(DESTDIR)$(PKGCONFIGDIR)/libddcci.pc

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libddcci.so.$(VERSION) \
		$(DESTDIR)$(LIBDIR)/libddcci.so.$(SOMAJOR) \
		$(DESTDIR)$(LIBDIR)/libddcci.so \
		$(DESTDIR)$(LIBDIR)/libddcci.a \
		$(DESTDIR)$(INCLUDEDIR)/ddcci.h \
		$(DESTDIR)$(BINDIR)/ddcci-probe \
		$(DESTDIR)$(PKGCONFIGDIR)/libddcci.pc

clean:
	rm -rf build

-include $(DEP)
