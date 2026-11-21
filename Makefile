# pulseControl - minimal volume control for PulseAudio on Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
MTK_DIR = mtk

all: pulseControl

include $(MTK_DIR)/mtk.mk

# CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the user's, also on the command line:
# `make CFLAGS=-O0` must not lose the include paths.
CFLAGS ?= -O2 -g
SC_CFLAGS = $(MTK_CFLAGS) $(shell $(PKG_CONFIG) --cflags libpulse)
SC_LIBS = $(shell $(PKG_CONFIG) --libs libpulse) $(MTK_LIBS)

MODS = theme.c
MODS_H = theme.h

pulseControl: sc.c $(MODS) $(MODS_H) version.h build/gitversion.h $(MTK_DEPS)
	$(CC) $(SC_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ sc.c $(MODS) \
		$(MTK_SRC) $(SC_LIBS) $(LDLIBS)

# The module without Wayland under ASan and UBSan, with mtk's that it uses.
UNIT_CFLAGS = -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	-fno-sanitize-recover=all
UNITS = build/test_theme

build/test_theme: tests/test_theme.c theme.c theme.h $(MTK_HEADLESS) $(MTK_DIR)/mtk.h | build
	$(CC) $(UNIT_CFLAGS) -I. -I$(MTK_DIR) \
		$(shell $(PKG_CONFIG) --cflags $(MTK_HEADLESS_PKGS)) -o $@ \
		tests/test_theme.c theme.c $(MTK_HEADLESS) \
		$(shell $(PKG_CONFIG) --libs $(MTK_HEADLESS_PKGS)) -lm

check: $(UNITS)
	@for t in $(UNITS); do echo "$$t"; ./$$t || exit 1; done

build:
	mkdir -p build

# The commit a build is from, after the version: +DATE.TIME.HASH, its commit time in
# UTC, and .dirty for changes not committed. Rewritten only when it changes, so
# pulseControl is linked again only then; empty without git.
build/gitversion.h: FORCE | build
	@v=$$(TZ=UTC0 git log -1 --format=%cd.%h --date=format-local:%Y%m%d.%H%M 2>/dev/null); \
	if [ -n "$$v" ] && ! git diff --quiet HEAD 2>/dev/null; then v=$$v.dirty; fi; \
	line="#define PULSECONTROL_GIT \"$${v:++$$v}\""; \
	[ "$$(cat $@ 2>/dev/null)" = "$$line" ] || echo "$$line" >$@

FORCE:

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

.PHONY: all install uninstall check test clean FORCE
