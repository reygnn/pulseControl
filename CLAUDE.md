# pulseControl

Project rules. Everything else: see the global CLAUDE.md, section "C projects".


## Build and test

`make` and `make check` build and run on the host. `make test` needs a built mwc (by
default `../mwc`, else `MWC_DIR`) with `build/vinput`, `build/pngtool`, `build/xdgc` and
`build/layerc`, plus grim, pactl and pulseaudio; `PA_MODULES` names pulseaudio's module
directory if it is elsewhere. The host has pipewire-pulse and no pulseaudio, so run it
in mwc's box:
`distrobox enter mwc-dev -- bash -lc 'cd ~/data/C-Projects/pulseControl && make test'`.

| Target | |
|---|---|
| `make` | pulseControl |
| `make check` | the modules without Wayland (`theme.c`, `util.c`) under ASan and UBSan |
| `make test` | `tests/ui.sh`: a headless mwc and a private PulseAudio with two null sinks, log in `build/ui.log`, pulseControl's own in `build/ui-pulseControl.log`, a failing screenshot in `build/ui-last.png` |

To check that a test fails without a fix, build the old `sc.c` elsewhere and run
`SC=/path/to/old/pulseControl tests/ui.sh`.


## Rules

- `sc.c` holds the program, no toolkit. Logic worth testing without Wayland goes into a
  small module with a unit test in `tests/`, as `theme.c` and `util.c`.
- The look follows mwc: colours from `MWC_INACTIVE`, `MWC_ACTIVE` or mwc's mwmrc, the
  bevel rules of mwc's `draw.c`.
- The tests never touch the real sound card or a running session.
- A test client pulseControl needs belongs in mwc's `tests/`, not here.
- C lines at most 100 columns, tabs as 8, as in mwc.
