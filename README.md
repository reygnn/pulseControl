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

`-v` prints the version, which is set in `version.h`,
and the commit built, as `VERSION+DATE.TIME.HASH` with its commit time in UTC
and `.dirty` for changes not committed.


## Look

The scale on top, below it a mute button with a speaker, the volume in percent and an
i. The window is as wide as digitalClock, so that the two line up in the dock. The
scale fills in the active colour up to the slider; muted, it stays empty, and the
volume is shown in brackets. The i opens a popup with the name of the output, or what
is wrong; while something is wrong, the i is in the error colour. Only mwm's thin border, in the
active colour while pulseControl is the active window. The colours are mwc's, see
Configuration.

It follows the default output and changes made by other programs. Without a sound
server the i says so, and pulseControl tries again every second.


## Keys

| Keys | Action |
|---|---|
| Up, Right, +, wheel up | louder by 5 % |
| Down, Left, -, wheel down | quieter by 5 % |
| click or drag on the scale | set the volume |
| digits, or a click on the percentage | type the volume |
| m, Space, a click on the speaker | mute on and off |
| i, a click on the i | the output's name, or what is wrong |
| the keyboard's volume keys | louder, quieter, mute |
| Esc, q, Ctrl+Q, Ctrl+W | quit |

In the field Return takes the typed volume and Esc cancels it. The popup closes with
Esc, Return, Space, i or a click; while it is up, only the volume keys and Ctrl+Q and
Ctrl+W act besides. With Alt, Super or Ctrl
held no key acts, except Ctrl+Q, Ctrl+W and the volume keys, which also act in the
field. The balance between the channels is kept.


## Configuration

| Variable | Default | |
|---|---|---|
| `MWC_ACTIVE` | `#2e5a8c` | fill, selection, focused border |
| `PULSE_SERVER` | | another sound server, as for every Pulse program |

The colours are mtk's (`mtk/README.md`, Colours), and pulseControl sets none of its
own. Without `MWC_ACTIVE` the active colour is `Mwm*activeBackground` from
`~/.config/mwc/mwmrc`, its text `Mwm*activeForeground`. The face is
`Mwm*basicBackground`, else `#c4c4c4`, its text `Mwm*textForeground`, else black; a
`basicBackground` without `textForeground` gets black or white. Text with less than
4.5:1 on its face is moved towards black or white as far as that needs. The volume
field, its trough and the field to type in are mtk's `list`, `trough` and `field`.
`MWC_INACTIVE`, `Mwm*background` and `Mwm*foreground` colour mwc's frames only.

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
