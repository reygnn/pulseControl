# pulseControl - minimal volume control for PulseAudio on Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = libpulse wayland-client wayland-cursor xkbcommon cairo pangocairo

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
XDG_SHELL = $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml

PROTO_H = build/xdg-shell-client-protocol.h
PROTO_C = build/xdg-shell-protocol.c

# CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the user's, also on the command line:
# `make CFLAGS=-O0` must not lose the include paths.
CFLAGS ?= -O2 -g
SC_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))
SC_LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

all: pulseControl

pulseControl: sc.c version.h $(PROTO_H) $(PROTO_C)
	$(CC) $(SC_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ sc.c $(PROTO_C) \
		$(SC_LIBS) $(LDLIBS)

build:
	mkdir -p build

build/xdg-shell-client-protocol.h: $(XDG_SHELL) | build
	$(WAYLAND_SCANNER) client-header $(XDG_SHELL) $@

build/xdg-shell-protocol.c: $(XDG_SHELL) | build
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL) $@

install: pulseControl
	install -Dm755 pulseControl $(DESTDIR)$(PREFIX)/bin/pulseControl
	install -d $(DESTDIR)$(PREFIX)/share/applications
	sed 's|^Exec=pulseControl$$|Exec=$(PREFIX)/bin/pulseControl|' data/pulseControl.desktop \
		>$(DESTDIR)$(PREFIX)/share/applications/pulseControl.desktop
	chmod 644 $(DESTDIR)$(PREFIX)/share/applications/pulseControl.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/pulseControl \
		$(DESTDIR)$(PREFIX)/share/applications/pulseControl.desktop

# Needs a built mwc next to this directory, or in MWC_DIR, plus grim, pactl and
# pulseaudio. It runs its own server with null sinks and leaves real volumes alone.
test: pulseControl
	tests/ui.sh

clean:
	rm -rf pulseControl build

.PHONY: all install uninstall test clean
