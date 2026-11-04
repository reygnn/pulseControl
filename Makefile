# sc - minimal volume control for PulseAudio on Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = libpulse wayland-client wayland-cursor xkbcommon cairo pangocairo

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
XDG_SHELL = $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
XDG_DECORATION = $(WAYLAND_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml

PROTO_H = build/xdg-shell-client-protocol.h \
	build/xdg-decoration-unstable-v1-client-protocol.h
PROTO_C = build/xdg-shell-protocol.c build/xdg-decoration-unstable-v1-protocol.c

# CFLAGS and LDLIBS stay the user's, also on the command line: `make CFLAGS=-O0`
# must not lose the include paths.
CFLAGS ?= -O2 -g
SC_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))
SC_LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS))

all: sc

sc: sc.c $(PROTO_H) $(PROTO_C)
	$(CC) $(SC_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ sc.c $(PROTO_C) $(SC_LIBS) $(LDLIBS)

build:
	mkdir -p build

build/xdg-shell-client-protocol.h: | build
	$(WAYLAND_SCANNER) client-header $(XDG_SHELL) $@

build/xdg-shell-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL) $@

build/xdg-decoration-unstable-v1-client-protocol.h: | build
	$(WAYLAND_SCANNER) client-header $(XDG_DECORATION) $@

build/xdg-decoration-unstable-v1-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(XDG_DECORATION) $@

install: sc
	install -Dm755 sc $(DESTDIR)$(PREFIX)/bin/sc
	mkdir -p $(DESTDIR)$(PREFIX)/share/applications
	sed 's|^Exec=sc$$|Exec=$(PREFIX)/bin/sc|' data/sc.desktop \
		>$(DESTDIR)$(PREFIX)/share/applications/sc.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/sc \
		$(DESTDIR)$(PREFIX)/share/applications/sc.desktop

# Needs a built mwc next to this directory, or in MWC_DIR, plus grim, pactl and
# pulseaudio. It runs its own server with null sinks and leaves real volumes alone.
test: sc
	tests/ui.sh

clean:
	rm -rf sc build

.PHONY: all install uninstall test clean
