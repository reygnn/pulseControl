# sc – minimale Lautstärkeregelung für PulseAudio unter Wayland (passend zu mwc)

Eine C-Datei, nur libpulse, libwayland, xkbcommon, Cairo und Pango, kein Toolkit.
Motif-Look wie [mfm](https://github.com/reygnn/mfm), mit denselben Farben wie
[mwc](https://github.com/reygnn/mwc): `MWC_INACTIVE` / `MWC_ACTIVE` aus der Umgebung,
sonst `Mwm*background` / `Mwm*activeBackground` aus `~/.config/mwc/mwmrc`.

sc regelt den Standard-Ausgang. Wechselt der oder ändert ein anderes Programm die
Lautstärke, zieht das Fenster nach. Es spricht das Pulse-Protokoll, läuft also auch
mit `pipewire-pulse`.

## Bauen

    sudo apt install build-essential pkg-config libpulse-dev libwayland-dev \
        wayland-protocols libxkbcommon-dev libcairo2-dev libpango1.0-dev
    make
    sudo make install        # -> /usr/local/bin/sc

## Bedienung

| Eingabe | Aktion |
|---|---|
| ↑ → + / Mausrad hoch | lauter (5 %) |
| ↓ ← − / Mausrad runter | leiser (5 %) |
| Regler klicken oder ziehen | Lautstärke setzen |
| m / Leertaste / Klick auf den Lautsprecher | stumm ein/aus |
| Esc / q / Ctrl+Q / Ctrl+W | beenden |

Mit Alt, Super oder Ctrl gedrückt wirkt keine dieser Tasten, außer Ctrl+Q und Ctrl+W.
Die Lautstärketasten der Tastatur wirken, solange das Fenster den Fokus hat.

Der Regler endet bei 100 %. Hat ein anderes Programm mehr eingestellt, zeigt sc den
Wert an und kann ihn senken, hebt aber nichts über 100 % an: Der Wert bleibt, bis man
den Regler nach links zieht oder leiser stellt. Die Balance zwischen den
Kanälen bleibt erhalten, auch wenn man über 0 % zieht. Lautstärke ändern hebt die
Stummschaltung nicht auf.

Ist kein Server erreichbar, steht das in der Statuszeile. sc versucht es dann jede
Sekunde wieder, startet aber selbst keinen.

## Umgebung

| Variable | Standard | |
|---|---|---|
| `SC_FONT` | `Sans 10` | Pango-Name, z. B. `Sans Bold 11` |
| `MWC_INACTIVE`, `MWC_ACTIVE` | `#aeb2c3`, `#7387ad` | Hintergrund, Regler-Füllung |
| `PULSE_SERVER` | | anderer Sound-Server, wie bei allen Pulse-Programmen |

## Tests

    make test

fährt sc unter einem headless laufenden mwc mit simulierter Maus und Tastatur, gegen
einen eigenen PulseAudio-Server mit zwei Null-Sinks, und prüft mit `pactl` und
Screenshots. Die echte Lautstärke bleibt unberührt. Es braucht einen gebauten mwc samt
`build/vinput` und `build/pngtool` in `MWC_DIR` (sonst `../mwc` oder `../mwm`), `grim`,
`pactl` und `pulseaudio`; liegen dessen Module woanders, nennt `PA_MODULES` den Ordner.

Läuft auf dem Rechner `pipewire-pulse`, gehört `pulseaudio` nicht auf den Host, denn
apt würde dafür `pipewire-alsa` entfernen. Besser in der Entwicklungsbox von mwc:

    sudo apt install libpulse-dev pulseaudio pulseaudio-utils grim

## Lizenz

MIT, siehe [LICENSE](LICENSE).
