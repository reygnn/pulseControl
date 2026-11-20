# pulseControl - minimal volume control for PulseAudio on Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = libpulse wayland-client wayland-cursor xkbcommon cairo pangocairo

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols 2>/dev/null)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner 2>/dev/null)
# Said plainly, rather than as a missing /stable/xdg-shell/xdg-shell.xml; but not to
# `make clean`, which needs neither.
ifneq ($(filter-out clean uninstall check build/test_theme build/test_util,$(or $(MAKECMDGOALS),all)),)
ifeq ($(WAYLAND_PROTOCOLS),)
$(error pkg-config finds no wayland-protocols)
endif
ifeq ($(WAYLAND_SCANNER),)
$(error pkg-config finds no wayland-scanner)
endif
endif
XDG_SHELL = $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
VIEWPORTER = $(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml

PROTO_H = build/xdg-shell-client-protocol.h build/viewporter-client-protocol.h
PROTO_C = build/xdg-shell-protocol.c build/viewporter-protocol.c

# CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the user's, also on the command line:
# `make CFLAGS=-O0` must not lose the include paths.
CFLAGS ?= -O2 -g
SC_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))
SC_LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

MODS = theme.c util.c
MODS_H = theme.h util.h

all: pulseControl

pulseControl: sc.c $(MODS) $(MODS_H) version.h $(PROTO_H) $(PROTO_C)
	$(CC) $(SC_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ sc.c $(MODS) \
		$(PROTO_C) $(SC_LIBS) $(LDLIBS)

# The modules without Wayland, each under ASan and UBSan.
UNIT_CFLAGS = -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	-fno-sanitize-recover=all
UNITS = build/test_theme build/test_util

build/test_util: tests/test_util.c util.c util.h | build
	$(CC) $(UNIT_CFLAGS) -I. -o $@ tests/test_util.c util.c

build/test_theme: tests/test_theme.c theme.c theme.h | build
	$(CC) $(UNIT_CFLAGS) -I. $(shell $(PKG_CONFIG) --cflags pango) -o $@ \
		tests/test_theme.c theme.c $(shell $(PKG_CONFIG) --libs pango) -lm

check: $(UNITS)
	@for t in $(UNITS); do echo "$$t"; ./$$t || exit 1; done

build:
	mkdir -p build

build/xdg-shell-client-protocol.h: $(XDG_SHELL) | build
	$(WAYLAND_SCANNER) client-header $(XDG_SHELL) $@

build/xdg-shell-protocol.c: $(XDG_SHELL) | build
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL) $@

build/viewporter-client-protocol.h: $(VIEWPORTER) | build
	$(WAYLAND_SCANNER) client-header $(VIEWPORTER) $@

build/viewporter-protocol.c: $(VIEWPORTER) | build
	$(WAYLAND_SCANNER) private-code $(VIEWPORTER) $@

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

.PHONY: all install uninstall check test clean
