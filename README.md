# pulseControl

pulseControl is a volume control for the default output of PulseAudio, in the look of
the Motif window manager. It is made for [mwc](https://github.com/reygnn/mwc), alone or
in its dock beside [deskControl](https://github.com/reygnn/deskControl) and
[digitalClock](https://github.com/reygnn/digitalClock), and runs on any Wayland
compositor with xdg-shell. It speaks the Pulse protocol, so it works with
`pipewire-pulse` too. One C file on libpulse, libwayland, xkbcommon, Cairo and Pango,
no toolkit.


## Build

On Ubuntu 26.04:

    sudo apt install build-essential pkg-config libpulse-dev libwayland-dev \
        wayland-protocols libxkbcommon-dev libcairo2-dev libpango1.0-dev
    make
    sudo make install

`make install` puts `pulseControl` in `/usr/local/bin` and adds a desktop entry.


## Run

    pulseControl &

In mwc, a place of its own or the dock, in `~/.config/mwc/mwmrc`:

    Mwm*pulseControl*geometry:   -0-0
    Mwm*pulseControl*dock:       true

`-v` prints the version, which is set in `version.h`.


## Look

A mute button with a speaker, the scale, the volume in percent, and a status line with
the name of the output. The scale fills in the active colour up to the slider; muted,
it stays empty.

It draws only mwm's thin border, no title bar, buttons or resize handles. The border
takes the active colour while pulseControl has the keyboard. Alt and a drag move it, as
any window in mwc. The colours are mwc's: `MWC_INACTIVE` and `MWC_ACTIVE` from the
environment, else `Mwm*background` and `Mwm*activeBackground` from the mwmrc, else the
Motif defaults.

pulseControl follows the default output when it changes, and the volume when another
program changes it. Without a sound server the status line says so, and pulseControl
tries again every second; it starts none itself.


## Keys

| Keys | Action |
|---|---|
| Up, Right, +, wheel up | louder by 5 % |
| Down, Left, -, wheel down | quieter by 5 % |
| click or drag on the scale | set the volume |
| digits, or a click on the percentage | type the volume |
| m, Space, a click on the speaker | mute on and off |
| Esc, q, Ctrl+Q, Ctrl+W | quit |

With Alt, Super or Ctrl held no key acts, except Ctrl+Q and Ctrl+W. The keyboard's
volume keys work while the window has the keyboard.

The field takes 0 to 100; a digit that would go past that does not arrive. Return
takes the value, Backspace deletes a digit. Esc, a click beside the field or another
window taking the keyboard cancel it, so Esc does not quit while the field is open.
0 is silence; a typed value above 0 ends the muting.

The scale ends at 100 %. A volume above that, set by another program, is shown and can
be lowered, but pulseControl raises nothing past 100 %. The balance between the
channels is kept, also through 0 % in a drag. Changing the volume leaves the muting
alone.


## Configuration

| Variable | Default | |
|---|---|---|
| `SC_FONT` | `Sans 10` | Pango font name, e.g. `Sans Bold 11` |
| `MWC_INACTIVE`, `MWC_ACTIVE` | `#aeb2c3`, `#7387ad` | background; fill, focused border |
| `PULSE_SERVER` | | another sound server, as for every Pulse program |


## How this is built

pulseControl is developed with Claude (Anthropic) as a coding assistant. I set the
scope and review the changes; Claude writes most of the code and the tests. `make
test` drives pulseControl in a headless mwc against a sound server of its own and
checks it with `pactl` and in screenshots. What is open is written down in CAVEATS.


## Testing

    make test

runs pulseControl in a headless mwc with simulated mouse and keyboard, against a
private PulseAudio server with two null sinks, and checks the keys, the mouse, typing,
what other programs change, a lost and returning server, the border and focus, and the
colours. The real sound card is left alone. It needs a built mwc with `build/vinput`,
`build/pngtool` and `build/xdgc` in `MWC_DIR` (by default `../mwc` or `../mwm`), grim,
pactl and pulseaudio; `PA_MODULES` names the module directory if it is elsewhere.

On a host with `pipewire-pulse`, apt would remove `pipewire-alsa` for `pulseaudio`, so
the test belongs in mwc's development box:

    distrobox enter mwc-dev -- sudo apt install libpulse-dev pulseaudio pulseaudio-utils
    distrobox enter mwc-dev -- make test
