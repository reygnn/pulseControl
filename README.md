# mtk

mtk is the Motif toolkit of the clients made for [mwc](https://github.com/reygnn/mwc):
mfm, mrun, deskControl, digitalClock, pulseControl and osDisplay. It holds what each of
them had in a copy of its own: the Wayland window on wl_shm buffers, the seat with
keyboard, compose, key repeat, pointer and cursor, an event loop, mwc's colours from
the environment and the mwmrc, mwm's bevels and borders on Cairo, popups for menus,
and a line of text to edit. C on libwayland, xkbcommon, Cairo and Pango.

It is no widget set. A client draws its window itself and handles its own events;
mtk tells it when, at which scale and with which colours.


## Colours

A client draws on IRIX's client palette: `Mwm*basicBackground` from
`~/.config/mwc/mwmrc` with `Mwm*textForeground`, else mwc's `#c4c4c4` with black, as
`theme.inactive`. A `basicBackground` without `textForeground` gets black or white.
`theme.active` is mwc's focused frame: `MWC_ACTIVE`, else `Mwm*activeBackground`, else
`#2e5a8c`, its text `Mwm*activeForeground`, else Motif's. The text on both has at
least 4.5:1: a colour with less is moved towards black or white as far as that needs.
`MWC_INACTIVE`, `Mwm*background` and `Mwm*foreground` are mwc's frames' alone.
`has_fg` and `has_active_fg` say that `textForeground` and `activeForeground` are set.
mwc's `docs/mwmrc.irix-schemes` has the palette for each of IRIX 6.5's schemes.

The shadows and the default text follow Motif 2.3.8's `CalculateColorsRGB()`, as mwc's
own frames do, so a client and its frame match: the rules are mwc's `src/color.c` and
`color.h`, copied unchanged into `mwc/` and built under names that start with `mtk_`.
`make check` compares them with `MWC_DIR`'s. `mtk_best_contrast()`, `mtk_dimmed()` and
`mtk_readable()` are there for text that must reach WCAG's 4.5:1.

A list is a face of the clients' own, `theme.list`: `inactive` 30 % towards white.
mwc's active colour often lands on its brightness (IRIX Base's `#c6c1aa` on its list
`#c4c4c4` has 1.04:1, Buckingham's 1.00:1), so a row selected on it, `theme.select`,
is that colour moved towards black or white until it has `MTK_SELECT_CONTRAST`, 2:1,
on the list (`mtk_apart()`). The text on both has 4.5:1. mwc draws no list and keeps
its colours; its armed menu item stands out by its bevel, not by colour.

A field to type in is `theme.field`, `#f6f6f6` on every palette, its text as on the
list. `theme.trough` is a scrollbar's trough, `inactive` 12 % towards black, and
`theme.folder`, `#c9a85c`, a folder icon's face. `theme.urgent` is what wants
attention, as a desktop mwc marks urgent: `inactive` 70 % towards amber `#ffa000`,
with black or white text. A client draws with these and sets
no colours of its own.

On the window `theme.dim` is dimmed text and `theme.err` error text: a dark red, or a
light one where white reads better, moved towards black or white as far as 4.5:1
needs.


## Use

mtk is vendored, not installed: a client has it in `mtk/` with git subtree and
builds it in.

    git subtree add --prefix=mtk https://github.com/reygnn/mtk main --squash
    git subtree pull --prefix=mtk https://github.com/reygnn/mtk main --squash

In the client's Makefile:

    all: app
    MTK_DIR = mtk
    include $(MTK_DIR)/mtk.mk

    app: app.c $(MTK_DEPS)
    	$(CC) $(MTK_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ app.c \
    		$(MTK_SRC) $(MTK_LIBS) $(LDLIBS)

`MTK_HEADLESS` are the modules without Wayland, for a client's own unit tests. The
API is in `mtk.h`; `demo/mtkdemo.c` uses all of its Wayland side, `tests/unit` the
rest.

A client needs:

    sudo apt install build-essential pkg-config libwayland-dev wayland-protocols \
        libxkbcommon-dev libcairo2-dev libpango1.0-dev


## Tests

| Target | |
|---|---|
| `make` | `build/mtkdemo` |
| `make unit` | `tests/unit`: a program per module without Wayland, under ASan and UBSan |
| `make check` | the units, `tests/mk.sh` (a client built with `mtk.mk`), and the demo built under ASan and UBSan with `-Werror` |
| `make test` | `tests/ui.sh`: the demo in a headless mwc, under ASan, checked in screenshots |
| `make analyze` | `gcc -fanalyzer`, findings as errors |
| `make lint` | C lines over 100 columns |

`make test` needs a built mwc in `MWC_DIR` (default `../mwc`) with `build/vinput`,
`build/pngtool` and `build/xdgc`, grim, and the Adwaita cursor theme.


## License

MIT, see `LICENSE`.
