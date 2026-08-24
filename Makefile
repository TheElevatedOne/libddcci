# libddcci — DDC/CI over I2C
PREFIX  ?= /usr/local
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -fPIC
CPPFLAGS = -Iinclude -Isrc
LDFLAGS ?=
LIBS    =

SRC = src/ddcci.c src/ddcci_parse.c src/ddcci_i2c.c src/ddcci_enum.c
OBJ = $(SRC:src/%.c=build/%.o)

.PHONY: all clean test probe install shared

all: build/libddcci.a build/libddcci.so build/ddcci-probe build/test_ddcci

build:
	mkdir -p build

build/%.o: src/%.c src/ddcci_priv.h include/ddcci.h | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

build/libddcci.a: $(OBJ)
	ar rcs $@ $(OBJ)

build/libddcci.so: $(OBJ)
	$(CC) -shared -fPIC -Wl,-soname,libddcci.so.1 -o $@ $(OBJ) $(LDFLAGS) $(LIBS)

build/ddcci-probe: examples/ddcci-probe.c build/libddcci.a
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ examples/ddcci-probe.c build/libddcci.a $(LDFLAGS) $(LIBS)

build/test_ddcci: tests/test_ddcci.c $(OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_ddcci.c $(OBJ) $(LDFLAGS) $(LIBS)

test: build/test_ddcci
	./build/test_ddcci

probe: build/ddcci-probe
	./build/ddcci-probe

install: all
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/bin
	install -m 644 include/ddcci.h $(DESTDIR)$(PREFIX)/include/ddcci.h
	install -m 644 build/libddcci.a $(DESTDIR)$(PREFIX)/lib/libddcci.a
	install -m 755 build/libddcci.so $(DESTDIR)$(PREFIX)/lib/libddcci.so.1.0.0
	ln -sf libddcci.so.1.0.0 $(DESTDIR)$(PREFIX)/lib/libddcci.so.1
	ln -sf libddcci.so.1 $(DESTDIR)$(PREFIX)/lib/libddcci.so
	install -m 755 build/ddcci-probe $(DESTDIR)$(PREFIX)/bin/ddcci-probe

clean:
	rm -rf build
