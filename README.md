# pulseControl

pulseControl is a volume control for the default output of PulseAudio, in the look of
the Motif window manager. It is made for [mwc](https://github.com/reygnn/mwc), alone or
in its dock beside [deskControl](https://github.com/reygnn/deskControl) and
[digitalClock](https://github.com/reygnn/digitalClock), and runs on any Wayland
compositor with xdg-shell. It speaks the Pulse protocol, so it works with
`pipewire-pulse` too. C on libpulse and [mtk](https://github.com/reygnn/mtk), the
toolkit of mwc's clients, vendored in `mtk/`: libwayland, xkbcommon, Cairo and Pango.


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
it stays empty, and the volume is shown in brackets. Only mwm's thin border, in the
active colour while pulseControl is the active window. The colours are mwc's, see
Configuration.

It follows the default output and changes made by other programs. Without a sound
server the status line says so, and pulseControl tries again every second.


## Keys

| Keys | Action |
|---|---|
| Up, Right, +, wheel up | louder by 5 % |
| Down, Left, -, wheel down | quieter by 5 % |
| click or drag on the scale | set the volume |
| digits, or a click on the percentage | type the volume |
| m, Space, a click on the speaker | mute on and off |
| the keyboard's volume keys | louder, quieter, mute |
| Esc, q, Ctrl+Q, Ctrl+W | quit |

In the field Return takes the typed volume and Esc cancels it. With Alt, Super or Ctrl
held no key acts, except Ctrl+Q, Ctrl+W and the volume keys, which also act in the
field. The balance between the channels is kept.


## Configuration

| Variable | Default | |
|---|---|---|
| `MWC_INACTIVE`, `MWC_ACTIVE` | `#aaaaaa`, `#c6c1aa` | background; fill, focused border |
| `PULSE_SERVER` | | another sound server, as for every Pulse program |

Without the colour variables it reads `Mwm*background` and `Mwm*activeBackground` from
`~/.config/mwc/mwmrc`, else the Motif defaults. The text takes `Mwm*foreground` and
`Mwm*activeForeground` from there as mwc's titles do, else black or white as Motif
picks it.

The font is fixed as DejaVu Sans 10, the same in deskControl, pulseControl, osDisplay
and digitalClock, so that the four are as high as each other in the dock. Without it
pulseControl says which font fontconfig took instead.


## How this is built

pulseControl is developed with Claude (Anthropic) as a coding assistant. I set the
scope and the design and review the changes; Claude writes most of the code and the
tests. `make test` drives pulseControl in a headless mwc against a sound server of its
own and checks it with `pactl` and in screenshots. What is open is written down in
CAVEATS.


## Testing

    make check

tests the parts without Wayland: the colours, the mwmrc, the seats and the buffer's
scale, under ASan and UBSan.

    make test

runs pulseControl in a headless mwc with simulated mouse and keyboard, against a
private PulseAudio server with two null sinks, and checks the keys, the mouse, typing,
what other programs change, a lost and returning server, the border and focus, and the
colours. The real sound card is left alone. It needs a built mwc with `build/vinput`,
`build/pngtool`, `build/xdgc` and `build/layerc` in `MWC_DIR` (by default `../mwc`),
grim, pactl, pulseaudio and the Adwaita cursor theme; `PA_MODULES` names the module
directory if it is elsewhere.

On a host with `pipewire-pulse`, apt would remove `pipewire-alsa` for `pulseaudio`, so
the test belongs in mwc's development box:

    distrobox enter mwc-dev -- sudo apt install libpulse-dev pulseaudio pulseaudio-utils
    distrobox enter mwc-dev -- make test
