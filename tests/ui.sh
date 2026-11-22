#!/bin/bash
# Black-box test of pulseControl: a headless mwc with simulated mouse and keyboard, and a
# private PulseAudio server with two null sinks. Results are checked with pactl and
# in screenshots. It has its own sockets, and touches neither a running session nor
# the volume of the real sound card.
#
# Needs a built mwc with build/vinput, build/pngtool, build/xdgc and build/layerc in
# MWC_DIR (by default ../mwc), grim, pactl, pulseaudio (PA_MODULES names its module
# directory, if it isn't in the usual place) and the Adwaita cursor theme. Output goes
# to build/ui.log, pulseControl's own to build/ui-pulseControl.log.
SC_DIR=$(cd "$(dirname "$0")/.." && pwd)
SC=${SC:-$SC_DIR/pulseControl}
LOG=$SC_DIR/build/ui.log
SC_LOG=$SC_DIR/build/ui-pulseControl.log
MWC_DIR=${MWC_DIR:-$SC_DIR/../mwc}
[ -x "$SC" ] || { echo "FAIL: $SC not built"; exit 1; }
[ -d "$MWC_DIR" ] || { echo "FAIL: no mwc in $MWC_DIR (set MWC_DIR)"; exit 1; }
cd "$MWC_DIR" || exit 1
. tests/lib.sh
require_tools grim pactl pulseaudio
for tool in build/vinput build/pngtool build/xdgc build/layerc; do
	[ -x "$tool" ] || { echo "FAIL: $MWC_DIR/$tool not built (make $tool)"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
export LC_ALL=C.UTF-8
export HOME=$SMOKE_DIR/home XDG_CONFIG_HOME=$SMOKE_DIR/home/.config
export PULSE_SERVER=unix:$SMOKE_DIR/pulse.sock
export DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent
unset MWC_INACTIVE MWC_ACTIVE PULSE_RUNTIME_PATH PULSE_STATE_PATH XCURSOR_PATH
# The font is fixed: a variable that once chose it must not change it.
export SC_FONT="Sans 20"
# The cursor checks count in Adwaita's sizes, whatever the developer uses. Without it
# wayland-cursor takes its built-in cursor, and they fail with no hint why: mwc's
# Podman image has no cursor theme. Where wayland-cursor looks, HOME being the test's.
export XCURSOR_THEME=Adwaita
for d in "$HOME/.local/share/icons" "$HOME/.icons" /usr/share/icons /usr/share/pixmaps; do
	[ -d "$d/Adwaita/cursors" ] && break
	d=
done
[ -n "$d" ] || { echo "FAIL: no Adwaita cursor theme (adwaita-icon-theme)"; exit 1; }
mkdir -p "$HOME"
: >"$SC_LOG"
SHOT=$SMOKE_DIR/ui.png
# mtk's defaults: the face, the active colour, the trough and the closed field
BG=c4c4c4
SEL=2e5a8c
TROUGH=acacac
LIST=d6d6d6
ERR_SHOT=$SC_DIR/build/ui-last.png
rm -f "$ERR_SHOT"

# Only -v, and it needs no compositor.
# The version from version.h, then the commit the build is from where git knows it.
want=$(sed -n 's/^#define PULSECONTROL_VERSION "\([^"]*\)".*/pulseControl \1/p' "$SC_DIR/version.h")
if commit=$(TZ=UTC0 git -C "$SC_DIR" log -1 --format=%cd.%h \
		--date=format-local:%Y%m%d.%H%M 2>/dev/null); then
	git -C "$SC_DIR" diff --quiet HEAD || commit=$commit.dirty
	want=$want+$commit
fi
[ "$("$SC" -v)" = "$want" ] || fail "-v prints '$("$SC" -v)', not '$want'"
echo "ok: -v prints the version"
"$SC" -h 2>/dev/null || fail "-h is a usage error"
"$SC" -h 2>/dev/null | grep -q '^usage:' || fail "-h prints no usage on stdout"
echo "ok: -h prints the usage on stdout"
for a in -v -h; do
	"$SC" $a >/dev/full 2>/dev/null && fail "$a: exit 0 on a full disk"
	"$SC" $a >&- 2>/dev/null && fail "$a: exit 0 without stdout"
	"$SC" $a 2>&1 >/dev/full | grep -q '^pulseControl: stdout: ' ||
		fail "$a: no message on a full disk"
	echo "ok: $a fails when stdout does"
done
[ -z "$("$SC" -x 2>/dev/null)" ] || fail "-x prints on stdout"
"$SC" -x 2>&1 >/dev/null | grep -q '^usage:' || fail "-x prints no usage on stderr"
echo "ok: a refused argument gets the usage on stderr"
for args in --help -x "-v -v" "-h -x"; do
	"$SC" $args >/dev/null 2>&1 && fail "'$args' is taken"
done
echo "ok: anything else is a usage error"

pa_start() {
	pulseaudio -n --daemonize=no --exit-idle-time=-1 --disable-shm=yes \
		${PA_MODULES:+--dl-search-path="$PA_MODULES"} \
		-L "module-null-sink sink_name=first sink_properties=device.description=First" \
		-L "module-null-sink sink_name=second sink_properties=device.description=Second" \
		-L "module-native-protocol-unix socket=$SMOKE_DIR/pulse.sock auth-anonymous=1" \
		>>"$SC_LOG" 2>&1 &
	PA_PID=$!
	wait_for "the sound server is up" pa_answers
	pactl set-default-sink first
}

pa_answers() { pactl info >/dev/null 2>&1; }

pa_stop() {
	kill "$PA_PID"
	wait "$PA_PID" 2>/dev/null
	rm -f "$SMOKE_DIR/pulse.sock"
}

# "55 55": the percent of each channel.
vol_of() {
	pactl get-sink-volume "$1" | sed -n 's/^Volume: //p' | grep -o '[0-9]*%' |
		tr -d % | xargs
}

expect_vol() { # <description> <sink> <percent ...>
	local what=$1 sink=$2 i got
	shift 2
	for ((i = 0; i < 50; i++)); do
		got=$(vol_of "$sink")
		[ "$got" = "$*" ] && { echo "ok: $what ($got)"; return 0; }
		sleep 0.05
	done
	fail "$what: $sink is at '$got', expected '$*'"
}

# rises_to <description> <sink> <percent>: the first channel gets there, within 3 s.
rises_to() {
	local i got
	for ((i = 0; i < 60; i++)); do
		got=$(vol_of "$2")
		[ "${got%% *}" -ge "$3" ] && { echo "ok: $1 ($got)"; return 0; }
		sleep 0.05
	done
	fail "$1: $2 is at '$got', not up to $3 %"
}

# stays <description> <sink> <percent>: once a step on its way has arrived, the volume
# doesn't move for a second, longer than the 40 ms a repeating key takes for a step,
# and is at most two steps above percent. Without that a repeat run on to 100 % would
# pass, standing still there.
stays() {
	local i got before=
	for ((i = 0; i < 20; i++)); do
		got=$(vol_of "$2")
		[ "$got" = "$before" ] && break
		before=$got
		sleep 0.1
	done
	for ((i = 0; i < 20; i++)); do
		sleep 0.05
		got=$(vol_of "$2")
		[ "$got" = "$before" ] || fail "$1: $2 went on from '$before' to '$got'"
	done
	[ "${before%% *}" -le $(($3 + 10)) ] || fail "$1: $2 went on from $3 % to '$before'"
	echo "ok: $1 ($before)"
}

# As expect_vol, within 1 %: a pixel of the scale need not be a whole percent.
expect_near() { # <description> <sink> <percent ...>
	local what=$1 sink=$2 i got ok v w
	shift 2
	for ((i = 0; i < 50; i++)); do
		got=$(vol_of "$sink")
		ok=1
		local -a have=($got) want=("$@")
		[ "${#have[@]}" = "${#want[@]}" ] || ok=0
		for ((v = 0; ok && v < ${#want[@]}; v++)); do
			w=$((have[v] - want[v]))
			[ "$w" -ge -1 ] && [ "$w" -le 1 ] || ok=0
		done
		[ "$ok" = 1 ] && { echo "ok: $what ($got)"; return 0; }
		sleep 0.05
	done
	fail "$what: $sink is at '$got', expected about '$*'"
}

expect_mute() { # <description> <sink> <yes|no>
	local i got
	for ((i = 0; i < 50; i++)); do
		got=$(pactl get-sink-mute "$2")
		[ "$got" = "Mute: $3" ] && { echo "ok: $1"; return 0; }
		sleep 0.05
	done
	fail "$1: $got, expected $3"
}

wait_pixel() { # <description> <x> <y> <rrggbb>
	pixel_is "$1" "$(($2 + CX))" "$(($3 + CY))" "$4"
}

sc_start() {
	WAYLAND_DISPLAY=wayland-0 "$SC" >>"$SC_LOG" 2>&1 &
	SC_PID=$!
	check "pulseControl has a focused window" '[ "$(get pulseControl kbfocus)" = 1 ]'
	# Everything below counts from inside its own frame of 3, which mwc doesn't add.
	WX=$(get pulseControl x) WY=$(get pulseControl y)
	CX=$((WX + 3)) CY=$((WY + 3))
	W=$(($(get pulseControl w) - 6))
	H=$(($(get pulseControl h) - 6))
	BARH=$(((H - 16) / 2))
	# In the window: the scale's inner x range and a row through it, below the
	# middle so that the slider's groove doesn't matter.
	S0=$((6 + BARH + 4 + 1))
	S1=$(scale_end)
	ROW=$((6 + BARH / 2 + 2))
	# Inside the volume field, right of its text and above it, and its left end and
	# middle: the text is centred.
	FIELD_X=$((W - 6 - 3))
	FIELD_Y=$((6 + 3))
	FIELD_L=$(field_left)
	FIELD_M=$(((FIELD_L + FIELD_X) / 2))
	echo "window ${W}x$H, scale x $S0..$S1, field x $FIELD_L..$FIELD_X"
}

# Where the inside of the volume field starts, in the window: along a row above the
# text, as far left as the face at its right end goes. Any theme.
field_left() {
	local x y=$((CY + FIELD_Y)) shot=$SMOKE_DIR/f.png c
	WAYLAND_DISPLAY=wayland-0 grim "$shot" 2>/dev/null || fail "no screenshot" >&2
	c=$(build/pngtool pixel "$shot" $((CX + FIELD_X)) "$y")
	for ((x = CX + FIELD_X; x > CX; x--)); do
		[ "$(build/pngtool pixel "$shot" "$x" "$y")" = "$c" ] || break
	done
	echo $((x - CX + 1))
}

# field_has <description> <rrggbb>: that colour in the field's row above the text, as
# a selection behind it is.
field_has() {
	local i
	for ((i = 0; i < 30; i++)); do
		WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null &&
			[ "$(build/pngtool count "$SHOT" $((CX + FIELD_L)) $((CY + FIELD_Y)) \
				$((FIELD_X - FIELD_L + 1)) 1 "$2" 0)" -gt 0 ] &&
			{ echo "ok: $1"; return 0; }
		sleep 0.1
	done
	cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "$1: no $2 in the field"
}

# Where the inside of the scale ends, in the window. The volume field beside it depends
# on the font, so walk in from the right along the top bevels: first the field, then the
# gap, then the scale.
scale_end() {
	local x y=$((CY + 6)) shot=$SMOKE_DIR/w.png
	# Stderr: the caller captures stdout, and would swallow the message.
	WAYLAND_DISPLAY=wayland-0 grim "$shot" 2>/dev/null || fail "no screenshot" >&2
	for ((x = CX + W - 1; x > CX; x--)); do
		[ "$(build/pngtool pixel "$shot" "$x" "$y")" != "$BG" ] && break
	done
	for (( ; x > CX; x--)); do
		[ "$(build/pngtool pixel "$shot" "$x" "$y")" = "$BG" ] && break
	done
	for (( ; x > CX; x--)); do
		[ "$(build/pngtool pixel "$shot" "$x" "$y")" != "$BG" ] && break
	done
	echo $((x - CX - 1))
}

# sc_exits <description>: it must end by itself, and with status 0.
sc_exits() {
	local i status
	for ((i = 0; i < 50; i++)); do
		kill -0 "$SC_PID" 2>/dev/null || break
		sleep 0.1
	done
	kill -0 "$SC_PID" 2>/dev/null && fail "$1: pulseControl is still running"
	wait "$SC_PID"
	status=$?
	[ "$status" = 0 ] || fail "$1: pulseControl exited with status $status"
}

sc_quit() {
	do_input key q
	sc_exits q
	check "q closes the window" '[ -z "$(get pulseControl w)" ]'
}

# status_has <description> <rrggbb> [tol]: text in that colour in the status line, for
# which any pixel fully inside a stroke will do; each channel off by at most tol, 0 if
# not given.
status_has() {
	local i shot=$SMOKE_DIR/s.png
	for ((i = 0; i < 30; i++)); do
		WAYLAND_DISPLAY=wayland-0 grim "$shot" 2>/dev/null || fail "no screenshot"
		[ "$(build/pngtool count "$shot" $((CX + 12)) $((CY + H - 6 - BARH + 4)) \
			98 $((BARH - 8)) "$2" "${3:-0}")" -gt 0 ] && { echo "ok: $1"; return 0; }
		sleep 0.1
	done
	fail "$1: no $2 in the status line"
}

sc_is_client() { pactl list short clients 2>/dev/null | grep -qw pulseControl; }

# Where a percentage puts the middle of the slider, in the window.
thumb_mid() { echo $((S0 + (S1 - S0 - 30) * $1 / 100 + 15)); }

# shows <description> <percent>: the window has the volume, filled up to the slider
# and empty behind it. Before input that depends on a volume another program set:
# the server may have it before pulseControl does. 3 pixels beside the slider, so that
# 96 and 100 differ.
shows() {
	wait_pixel "$1" $(($(thumb_mid "$2") - 18)) "$ROW" "$SEL"
	wait_pixel "$1" $(($(thumb_mid "$2") + 18)) "$ROW" "$TROUGH"
}

# A compositor left stopped would hang the screenshot and the cleanup.
# A test that kept its own screenshot keeps it.
trap 'kill -CONT "$pid" 2>/dev/null; [ -e "$ERR_SHOT" ] ||
	WAYLAND_DISPLAY=wayland-0 grim "$ERR_SHOT" 2>/dev/null; cleanup' EXIT

# The window that takes the keyboard away stays clear of pulseControl.
mkdir -p "$XDG_CONFIG_HOME/mwc"
printf 'Mwm*thief*geometry: -0-0\n' >"$XDG_CONFIG_HOME/mwc/mwmrc"
start_mwc 'touch "$SMOKE_DIR/up"; exec sleep 600'
wait_for "mwc is up" test -e "$SMOKE_DIR/up"
start_vinput
refresh
pa_start
pactl set-sink-volume first 50%
pactl set-sink-volume second 10%

sc_start
# Only its own thin frame: mwc adds no border and no title bar.
[ "$(get pulseControl bw)" = 0 ] && [ "$(get pulseControl th)" = 0 ] ||
	fail "mwc decorates it: border $(get pulseControl bw), title $(get pulseControl th)"
echo "ok: no server-side decoration"
# DejaVu Sans 10 makes it 70 high, as high as the other dock tools.
[ "$H" = 70 ] || fail "the window is $H high, not 70: the font is not DejaVu Sans 10"
echo "ok: the font is DejaVu Sans 10, whatever SC_FONT says"
grep -q 'not found' "$SC_LOG" && fail "it warns of a missing font: $(cat "$SC_LOG")"
echo "ok: no warning with DejaVu Sans there"
# Its frame: a bevel of 2, then a line of the face, active while it is the active window.
pixel_is "the frame is in the active colour" $((WX + 2)) $((CY + ROW)) "$SEL"
# Another window, in the far corner, takes the keyboard, and gives it back.
WAYLAND_DISPLAY=wayland-0 "$MWC_DIR/build/xdgc" app=thief size=200x100 \
	>"$SMOKE_DIR/thief.log" 2>&1 &
PID_thief=$!
check "another window takes the keyboard" \
	"[ \"\$(get thief kbfocus)\" = 1 ] && [ \"\$(get pulseControl kbfocus)\" = 0 ]"
pixel_is "without it the frame is inactive" $((WX + 2)) $((CY + ROW)) "$BG"
kill "$PID_thief" 2>/dev/null
wait "$PID_thief" 2>/dev/null
do_input move $((CX + 20)) $((CY + H - 10))
do_input click left
check "a click on the status line gives the keyboard back" \
	"[ \"\$(get pulseControl kbfocus)\" = 1 ]"
pixel_is "and the frame is active again" $((WX + 2)) $((CY + ROW)) "$SEL"
# A layer surface that takes the keyboard, as a launcher does, leaves the window active
# in mwc, and mwc's frames with it: so is this one.
WAYLAND_DISPLAY=wayland-0 "$MWC_DIR/build/layerc" keyboard=exclusive anchor=bottom,right \
	>"$SMOKE_DIR/layer.log" 2>&1 &
PID_layer=$!
check "a layer surface takes the keyboard, the window stays active" \
	"[ \"\$(get pulseControl kbfocus)\" = 0 ] && [ \"\$(get pulseControl active)\" = 1 ]"
pixel_is "and so does its frame" $((WX + 2)) $((CY + ROW)) "$SEL"
kill "$PID_layer" 2>/dev/null
wait "$PID_layer" 2>/dev/null
check "the keyboard comes back" "[ \"\$(get pulseControl kbfocus)\" = 1 ]"
wait_pixel "the scale is filled up to the slider" $(($(thumb_mid 50) - 20)) "$ROW" "$SEL"
wait_pixel "and empty behind it" $(($(thumb_mid 50) + 20)) "$ROW" "$TROUGH"
# The widgets have mwc's bevel of 1, the frame its 2: the fill starts right inside.
wait_pixel "the scale's bevel is 1" "$S0" "$ROW" "$SEL"

# ---- keyboard ----------------------------------------------------------------

do_input key Up
expect_vol "Up is louder" first 55 55
do_input key Right
expect_vol "Right is louder" first 60 60
do_input key Down
do_input key Left
expect_vol "Down and Left are quieter" first 50 50

# A held key repeats at mwc's 600 ms and 25 a second, until it is let go or the
# keyboard goes elsewhere. From 10 %, so that 100 % doesn't stop it instead.
pactl set-sink-volume first 10%
shows "at 10 before holding a key" 10
do_input press Up
rises_to "Up held repeats" first 30
do_input lift Up
at=$(vol_of first)
stays "and stops when let go" first "${at%% *}"
pactl set-sink-volume first 10%
shows "at 10 again" 10
do_input press Up
rises_to "held again, it repeats" first 30
WAYLAND_DISPLAY=wayland-0 "$MWC_DIR/build/layerc" keyboard=exclusive anchor=bottom,right \
	>"$SMOKE_DIR/layer.log" 2>&1 &
PID_layer=$!
check "a layer surface takes the keyboard" "[ \"\$(get pulseControl kbfocus)\" = 0 ]"
at=$(vol_of first)
stays "the repeat ends with the keyboard, though the key is down" first "${at%% *}"
kill "$PID_layer" 2>/dev/null
wait "$PID_layer" 2>/dev/null
check "the keyboard comes back" "[ \"\$(get pulseControl kbfocus)\" = 1 ]"
at=$(vol_of first)
do_input lift Up
stays "and the key coming up later does nothing" first "${at%% *}"
pactl set-sink-volume first 50%
shows "back at 50" 50
do_input key m
expect_mute "m mutes" first yes
wait_pixel "a muted scale has no fill" $((S0 + 2)) "$ROW" "$TROUGH"
do_input key space
expect_mute "Space unmutes" first no
wait_pixel "the fill is back" $((S0 + 2)) "$ROW" "$SEL"

# With Alt or Ctrl these keys are something else, and a plain w is nothing. Checked
# by the m after them: it comes later, so they are done when it has muted.
do_input key alt+q
do_input key ctrl+m
do_input key w
do_input key m
expect_mute "ctrl+m doesn't mute, m after it does" first yes
kill -0 "$SC_PID" 2>/dev/null || fail "alt+q or w ended pulseControl"

# Changing the volume leaves the muting alone.
do_input key Up
expect_vol "a muted sink takes a step" first 55 55
expect_mute "and stays muted" first yes
do_input key Down
do_input key m
expect_mute "unmuted" first no
expect_vol "back at the volume from before" first 50 50

# ---- typing the volume --------------------------------------------------------

do_input key 4
wait_pixel "a digit opens the field" "$FIELD_X" "$FIELD_Y" f6f6f6
do_input key 2
do_input key Return
expect_vol "digits and Return set the volume" first 42 42
wait_pixel "and close the field" "$FIELD_X" "$FIELD_Y" "$LIST"

do_input key 7
do_input key Escape
wait_pixel "Esc closes the field" "$FIELD_X" "$FIELD_Y" "$LIST"
kill -0 "$SC_PID" 2>/dev/null || fail "Esc in the field ended pulseControl"
expect_vol "and leaves the volume" first 42 42

# Keys in the field, checked by the same keys once it is closed.
do_input key 5
do_input key q
do_input key m
do_input key Up
do_input key Escape
do_input key m
expect_mute "m in the field doesn't mute, after it does" first yes
kill -0 "$SC_PID" 2>/dev/null || fail "q in the field ended pulseControl"
do_input key Up
expect_vol "Up in the field doesn't step, after it does" first 47 47
do_input key Down
do_input key m
expect_mute "unmuted" first no
expect_vol "back at 42" first 42 42

for k in 1 0 1; do do_input key "$k"; done
do_input key Return
expect_vol "a digit past 100 doesn't arrive" first 10 10
for k in 1 0 0; do do_input key "$k"; done
do_input key 7
do_input key Return
expect_vol "100 is the most, and has room for no more" first 100 100

do_input key 6
do_input key 7
do_input key BackSpace
do_input key Return
expect_vol "Backspace takes a digit" first 6 6

# A leading zero takes no place: with it 0100 would be 010, which is 10.
for k in 0 1 0 0; do do_input key "$k"; done
do_input key Return
expect_vol "a leading zero is replaced" first 100 100
do_input key 0
do_input key 5
do_input key Return
expect_vol "and so is a lone one" first 5 5

# ---- AZERTY -------------------------------------------------------------------------
# A plain key is what the layout gives. vinput names keys where the us layout has
# them: its 6 is '-' in fr, a step down, not the 6 on its Shift level, which would
# open the field; its semicolon is fr's m, its m fr's comma, its a fr's q.
stop_vinput
start_vinput 1280 720 fr
pactl set-sink-volume first 50%
shows "at 50 in fr" 50
do_input key 6
do_input key Return
expect_vol "'-' on AZERTY is quieter" first 45 45
do_input press 6
check "and repeats when held" '[ "$(vol_of first | cut -d" " -f1)" -le 30 ]'
do_input lift 6
pactl set-sink-volume first 50%
shows "at 50 again" 50
do_input key m
do_input key semicolon
expect_mute "m mutes, the comma on the us m doesn't" first yes
do_input key semicolon
expect_mute "unmuted" first no
do_input key a
sc_exits "q on the us a"
check "q closes the window" '[ -z "$(get pulseControl w)" ]'
stop_vinput
start_vinput
pactl set-sink-volume first 5%
sc_start

do_input move $((CX + FIELD_X)) $((CY + ROW))
do_input click left
wait_pixel "a click opens the field" "$FIELD_X" "$FIELD_Y" f6f6f6
do_input key Return
wait_pixel "Return without typing closes the field" "$FIELD_X" "$FIELD_Y" "$LIST"
expect_vol "and changes nothing" first 5 5

do_input click left
do_input key 3
do_input key 3
do_input key Return
expect_vol "typing after the click sets it" first 33 33

# Closed by a click on the status line, which sets nothing itself. A step after it
# counts from 33, not from a 9 the click might have taken.
do_input click left
do_input key 9
do_input move $((CX + 20)) $((CY + H - 10))
do_input click left
wait_pixel "a click beside the field closes it" "$FIELD_X" "$FIELD_Y" "$LIST"
do_input key Up
expect_vol "and takes nothing" first 38 38
do_input key Down
expect_vol "back at 33" first 33 33

do_input key 0
do_input key Return
expect_vol "0 is silence" first 0 0
expect_mute "without muting" first no

do_input key m
expect_mute "muted" first yes
do_input key 5
do_input key 0
do_input key Return
expect_vol "a typed volume arrives" first 50 50
expect_mute "and ends the muting" first no
do_input key m
expect_mute "muted again" first yes
do_input key 0
do_input key Return
expect_vol "a typed 0 arrives" first 0 0
expect_mute "and leaves the muting" first yes
do_input key m
expect_mute "unmuted" first no
do_input key 5
do_input key 0
do_input key Return
expect_vol "back at 50" first 50 50

# The text on the active colour, mwc's blue: white, as Motif picks it and mwc's titles
# have it. Counted in the middle of the selection, inside the field.
do_input move $((CX + FIELD_X)) $((CY + ROW))
do_input click left
field_has "a click selects the volume" "$SEL"
ink_ok=
for ((i = 0; i < 30; i++)); do
	if WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null; then
		set -- $((CX + FIELD_M - 5)) $((CY + 10)) 10 $((BARH - 8))
		black=$(build/pngtool count "$SHOT" "$@" 000000)
		white=$(build/pngtool count "$SHOT" "$@" ffffff)
		[ "$white" -gt 0 ] && [ "$black" = 0 ] && { ink_ok=1; break; }
	fi
	sleep 0.1
done
[ -n "$ink_ok" ] || { cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "the selected volume has $black pixels near black and $white near white"; }
echo "ok: the selected volume is white"
do_input key Escape
wait_pixel "Esc closes the field" "$FIELD_X" "$FIELD_Y" "$LIST"

# An open field with nothing typed shows the volume as it is now. Its ink differs
# between 50 % and 11 %: counted once each, then 11 % set while the field is open.
# field_ink [rrggbb [tol]]: black by default; the selected volume's is white, counted
# within 8 so that the field's #f6f6f6 beside the selection is not.
field_ink() {
	WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null &&
		build/pngtool count "$SHOT" $((CX + FIELD_L)) $((CY + 10)) \
			$((FIELD_X - FIELD_L + 1)) $((BARH - 8)) "${1:-000000}" ${2:+"$2"}
}
# The first and last column of the field with ink in them, "l r", in the window.
ink_span() {
	local x l= r=
	WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null || fail "no screenshot"
	for ((x = FIELD_L; x <= FIELD_X; x++)); do
		[ "$(build/pngtool count "$SHOT" $((CX + x)) $((CY + 10)) 1 $((BARH - 8)) \
			000000)" -gt 0 ] || continue
		l=${l:-$x}
		r=$x
	done
	echo "$l $r"
}
# span_ink <l> <r>: the ink between those columns.
span_ink() {
	WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null || fail "no screenshot"
	build/pngtool count "$SHOT" $((CX + $1)) $((CY + 10)) $(($2 - $1 + 1)) $((BARH - 8)) \
		000000
}
open_field() {
	do_input move $((CX + FIELD_X)) $((CY + ROW))
	do_input click left
	field_has "$1" "$SEL"
}
field_shows() { # <ink> [rrggbb [tol]]
	local i
	for ((i = 0; i < 30; i++)); do
		[ "$(field_ink "${2:-}" "${3:-}")" = "$1" ] && return 0
		sleep 0.1
	done
	return 1
}
pactl set-sink-volume first 11%
shows "11 % is shown" 11
open_field "the field opens at 11 %"
ink11=$(field_ink ffffff 8)
do_input key Escape
pactl set-sink-volume first 50%
shows "50 % is shown" 50
open_field "the field opens at 50 %"
ink50=$(field_ink ffffff 8)
[ "$ink11" != "$ink50" ] || fail "11 % and 50 % have the same ink ($ink11)"
pactl set-sink-volume first 11%
field_shows "$ink11" ffffff 8 || { cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "the open field doesn't follow the volume: ink $(field_ink ffffff 8), not $ink11"; }
echo "ok: the open field follows another program"
do_input key Escape
pactl set-sink-volume first 50%
shows "back at 50" 50

# ---- mouse -------------------------------------------------------------------

do_input move $((CX + S0 + 5)) $((CY + ROW))
do_input wheel -1
expect_vol "the wheel up is louder" first 55 55
do_input wheel 1
do_input wheel 1
expect_vol "the wheel down is quieter" first 45 45

BX=$((CX + 6 + BARH / 2))
do_input move "$BX" $((CY + 6 + BARH / 2))
do_input click left
expect_mute "the speaker mutes" first yes
do_input click left
expect_mute "and unmutes" first no

# Pressed, but released elsewhere: nothing happens.
do_input down left
do_input move $((CX + $(thumb_mid 45))) $((CY + ROW))
do_input up left
do_input move "$BX" $((CY + 6 + BARH / 2))
do_input click left
expect_mute "sliding off the speaker cancels the click, a click after it mutes" first yes
do_input click left
expect_mute "unmuted" first no
# Off and back before the release, as a Motif push button: it counts.
do_input down left
do_input move $((CX + $(thumb_mid 45))) $((CY + ROW))
do_input move "$BX" $((CY + 6 + BARH / 2))
do_input up left
expect_mute "off the speaker and back, the release mutes" first yes
do_input click left
expect_mute "unmuted again" first no

# A click beside the slider puts it there; 75 % is well away from 45 %.
do_input move $((CX + $(thumb_mid 75))) $((CY + ROW))
do_input click left
expect_near "a click on the scale sets the volume" first 75 75

# A drag beyond the left end, and back, with what is reported in between.
do_input down left
do_input move $((CX - 40)) $((CY + ROW))
expect_vol "dragging past the left end gives 0" first 0 0
do_input move $((CX + $(thumb_mid 50))) $((CY + ROW))
expect_near "dragging back to the middle" first 50 50
held=$(vol_of first)
held=${held%% *}
do_input up left
do_input key Up
expect_vol "the release leaves it there, a step goes on from it" first \
	$((held + 5)) $((held + 5))

# A press on the slider without moving changes nothing, wherever on it.
pactl set-sink-volume first 40%
expect_vol "changed by another program" first 40 40
shows "and shown" 40
do_input move $((CX + $(thumb_mid 40) + 12)) $((CY + ROW))
do_input click left
do_input key Up
expect_vol "a click on the slider leaves the volume alone, a step goes on" first 45 45
do_input key Down
shows "back at 40" 40

# Grabbed away from its middle, it moves from where it is.
do_input down left
do_input move $((CX + $(thumb_mid 40) + 13)) $((CY + ROW))
expect_near "a grabbed slider doesn't jump" first 40 40
do_input up left

# ---- what others do ----------------------------------------------------------

pactl set-sink-volume first 90%
wait_pixel "the scale follows another program" $(($(thumb_mid 90) - 20)) "$ROW" "$SEL"
wait_pixel "and only that far" $(($(thumb_mid 90) + 20)) "$ROW" "$TROUGH"

# During a drag the window ignores others, but catches up with the release.
do_input move $((CX + $(thumb_mid 90))) $((CY + ROW))
do_input down left
pactl set-sink-volume first 30%
# Not a wait for anything: without it the change mostly arrives during the drag too,
# and if not, the release still shows it. It only makes the drag case the likely one.
sleep 0.2
do_input up left
wait_pixel "a change during a drag shows after the release" $(thumb_mid 60) "$ROW" "$TROUGH"
pactl set-sink-mute first 1
wait_pixel "and its muting" $((S0 + 2)) "$ROW" "$TROUGH"
pactl set-sink-mute first 0

# The balance survives steps, and a volume of zero in a drag.
pactl set-sink-volume first 80% 40%
do_input move "$BX" $((CY + 6 + BARH / 2))
shows "an uneven volume shows its loudest channel" 80
do_input key Up
expect_near "a step keeps the balance" first 85 43
do_input move $((CX + $(thumb_mid 85))) $((CY + ROW))
do_input down left
do_input move $((CX - 40)) $((CY + ROW))
expect_vol "dragged to zero" first 0 0
do_input move $((CX + $(thumb_mid 60))) $((CY + ROW))
expect_near "the balance is back after zero" first 60 30
do_input up left

# Above 100 %: shown, can go down, doesn't go up.
pactl set-sink-volume first 130%
wait_pixel "the slider stays inside the scale above 100 %" $((S1 - 5)) "$ROW" "$BG"
# Up and a grab of the slider keep it; Down after them lowers it from 130.
do_input key Up
do_input move $((CX + $(thumb_mid 100))) $((CY + ROW))
do_input down left
do_input move $((CX + $(thumb_mid 100))) $((CY + ROW + 1))
do_input move $((CX + $(thumb_mid 100) + 1)) $((CY + ROW + 1))
do_input up left
do_input key Down
expect_vol "neither Up nor a grab raise it above 100 %, Down lowers it" first 125 125
pactl set-sink-volume first 96%
shows "96 % is shown" 96
do_input key Up
expect_vol "Up stops at 100 %" first 100 100
pactl set-sink-volume first 3%
shows "3 % is shown" 3
do_input key Down
expect_vol "Down stops at 0" first 0 0

# vinput waits for each key, so this needn't outrun the server's answers.
pactl set-sink-volume first 5%
shows "5 % is shown" 5
for _ in 1 2 3 4 5 6 7 8; do do_input key Up; done
expect_vol "steps in a row add up" first 45 45

# ---- the default output changes ----------------------------------------------

pactl set-default-sink second
wait_pixel "the scale shows the new default" $(($(thumb_mid 45) - 20)) "$ROW" "$TROUGH"
shows "at its volume" 10
do_input key Up
expect_vol "a step goes to the new default" second 15 15
expect_vol "and not to the old one" first 45 45
pactl set-default-sink first
shows "the old default is back" 45

# A right-to-left name starts at the left of the status line, as any other.
RTL=$(pactl load-module module-null-sink sink_name=rtl \
	sink_properties=device.description=שלום)
pactl set-sink-volume rtl 20%
pactl set-default-sink rtl
shows "a sink with a right-to-left name" 20
status_has "its name is at the left" 000000 32    # thin strokes, never quite black
pactl set-default-sink first
pactl unload-module "$RTL"
shows "the old default is back again" 45

# ---- no output at all --------------------------------------------------------

pactl unload-module module-null-sink
wait_pixel "without a sink the scale is empty" $((S0 + 2)) "$ROW" "$TROUGH"
status_has "the status line says so" 8a0303
# The speaker is dimmed then, but still at 4.5:1 on the face: 4e4e4e as in
# digitalClock. Drawn without antialiasing, so exact; the text has subpixel fringes.
dim_ok=
for ((i = 0; i < 30; i++)); do
	WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null &&
		[ "$(build/pngtool count "$SHOT" $((CX + 6)) $((CY + 6)) "$BARH" "$BARH" \
			4e4e4e 0)" -gt 8 ] && { dim_ok=1; break; }
	sleep 0.1
done
[ -n "$dim_ok" ] || { cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "the dimmed speaker is not 4e4e4e"; }
echo "ok: the dimmed speaker is 4e4e4e"
do_input key Up         # must not crash
pactl load-module module-null-sink sink_name=first \
	sink_properties=device.description=First >/dev/null
pactl set-default-sink first
pactl set-sink-volume first 50%
wait_pixel "with a sink back the scale fills" $((S0 + 2)) "$ROW" "$SEL"
shows "at the volume set" 50
do_input key Up
expect_vol "and it works" first 55 55

# ---- the server goes away and comes back -------------------------------------

pa_stop
wait_pixel "without a server the scale is empty" $((S0 + 2)) "$ROW" "$TROUGH"
do_input key Up         # must not crash
do_input move $((CX + $(thumb_mid 50))) $((CY + ROW))
do_input click left
# It retries by itself: a window that gets no events, as here with the compositor
# stopped, must still come back.
sleep 1.5
kill -STOP "$pid"
sleep 1.5
pa_start
wait_for "pulseControl reconnects without any Wayland event" sc_is_client
kill -CONT "$pid"
pactl set-sink-volume first 30%
wait_pixel "with a server back it comes back" $((S0 + 2)) "$ROW" "$SEL"
shows "at the volume set" 30
do_input key Up
expect_vol "and works" first 35 35

# ---- quitting ----------------------------------------------------------------

for combo in Escape q ctrl+q ctrl+w; do
	do_input key "$combo"
	sc_exits "$combo"
	check "$combo closes the window" '[ -z "$(get pulseControl w)" ]'
	[ "$combo" = ctrl+w ] || sc_start
done
# A step and a quit read in one batch: the step reaches the server before pulseControl
# goes. Stopped, it finds both waiting when it goes on.
sc_start
shows "at 35 before quitting" 35
kill -STOP "$SC_PID"
do_input key Up
do_input key q
kill -CONT "$SC_PID"
sc_exits "a step and q in one batch"
expect_vol "the step arrives before the end" first 40 40
# And a click after q in the same batch is not meant: the speaker stays as it is.
# Shown first: without its sink nothing would be sent anyway, and the check be empty.
sc_start
shows "at 40 before quitting" 40
kill -STOP "$SC_PID"
do_input key q
do_input move $((CX + 6 + BARH / 2)) $((CY + 6 + BARH / 2))
do_input click left
kill -CONT "$SC_PID"
sc_exits "q and a click in one batch"
expect_mute "the click after q mutes nothing" first no
# Nor does the wheel turned after q: nothing read after it reaches the server.
sc_start
shows "at 40 again before quitting" 40
kill -STOP "$SC_PID"
do_input key q
do_input move $((CX + S0 + 5)) $((CY + ROW))
do_input wheel -1
kill -CONT "$SC_PID"
sc_exits "q and the wheel in one batch"
expect_vol "the wheel after q changes nothing" first 40 40

# ---- size --------------------------------------------------------------------

# Taller by Alt+F8, then maximized and back: the height is the one from before. mwc
# gives it back itself; one that leaves the size to the window relies on float_h.
sc_start
H0=$(get pulseControl h)
do_input key alt+F8
refresh
do_input move $(global cursor x) $(($(global cursor y) + 40))
do_input click left
check "Alt+F8 makes it taller" "[ \"\$(get pulseControl h)\" -gt $H0 ]"
H1=$(get pulseControl h)
do_input key alt+F10
check "maximized" "[ \"\$(get pulseControl max)\" = 1 ]"
do_input key alt+F5
check "restored at the height from before" \
	"[ \"\$(get pulseControl max)\" != 1 ] && [ \"\$(get pulseControl h)\" = $H1 ]"
sc_quit

# ---- colours -----------------------------------------------------------------

mkdir -p "$XDG_CONFIG_HOME/mwc"
printf 'Mwm*activeBackground: #ff0000\n' >"$XDG_CONFIG_HOME/mwc/mwmrc"
sc_start
wait_pixel "the fill comes from mwmrc" $((S0 + 2)) "$ROW" ff0000
sc_quit
MWC_ACTIVE='#00ff00' sc_start
wait_pixel "MWC_ACTIVE goes before mwmrc" $((S0 + 2)) "$ROW" 00ff00
sc_quit
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

# rgb: takes hex digits only, as mwc does: no sign, no blank, no 0x.
for bad in rgb:-f/0/0 'rgb: f/0/0' rgb:0xf/0/0; do
	MWC_ACTIVE=$bad sc_start
	wait_pixel "MWC_ACTIVE=$bad is no colour" $((S0 + 2)) "$ROW" "$SEL"
	sc_quit
done
MWC_ACTIVE=rgb:0/ff/0 sc_start
wait_pixel "MWC_ACTIVE=rgb:0/ff/0 is one" $((S0 + 2)) "$ROW" 00ff00
sc_quit

# A long line is read whole: a comment's end is no setting, however long it is, and a
# setting is one, however many blanks it has.
{
	echo 'Mwm*activeBackground: #ff0000'
	printf '!%0510d' 0 | tr 0 x     # make check tests other lengths
	echo 'Mwm*activeBackground: #00ff00'
} >"$XDG_CONFIG_HOME/mwc/mwmrc"
sc_start
wait_pixel "the rest of a long line in mwmrc is no setting" $((S0 + 2)) "$ROW" ff0000
sc_quit
printf 'Mwm*activeBackground:%600s#00ff00\n' '' >"$XDG_CONFIG_HOME/mwc/mwmrc"
sc_start
wait_pixel "a long setting in mwmrc counts" $((S0 + 2)) "$ROW" 00ff00
sc_quit
# The last line wins, as in mwc, even when its value is no colour: one too long to keep
# gives the default, not the line before.
{
	echo 'Mwm*activeBackground: #ff0000'
	printf 'Mwm*activeBackground: #%0300d\n' 0
} >"$XDG_CONFIG_HOME/mwc/mwmrc"
sc_start
wait_pixel "a last value too long to keep gives the default" $((S0 + 2)) "$ROW" "$SEL"
sc_quit
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

# The client palette's text: textForeground on the face, in the field and as its caret,
# activeForeground on the active colour, as in the selected volume. Both read at 4.5:1
# as they are.
printf 'Mwm*textForeground: #00007f\nMwm*activeForeground: #ffccff\n' \
	>"$XDG_CONFIG_HOME/mwc/mwmrc"
sc_start
status_has "the text takes Mwm*textForeground" 00007f
do_input key 4
wait_pixel "a digit opens the field" "$FIELD_X" "$FIELD_Y" f6f6f6
check "the typed volume takes Mwm*textForeground" '[ "$(field_ink 00007f 0)" -gt 0 ]'
check "and none of it is black" '[ "$(field_ink)" = 0 ]'
do_input key Escape
wait_pixel "Esc closes the field" "$FIELD_X" "$FIELD_Y" "$LIST"
do_input move $((CX + FIELD_X)) $((CY + ROW))
do_input click left
field_has "a click selects the volume" "$SEL"
fg_ok=
for ((i = 0; i < 30; i++)); do
	WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null &&
		[ "$(build/pngtool count "$SHOT" $((CX + FIELD_M - 5)) $((CY + 10)) 10 \
			$((BARH - 8)) ffccff)" -gt 0 ] && { fg_ok=1; break; }
	sleep 0.1
done
[ -n "$fg_ok" ] || { cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "the selected volume is not in Mwm*activeForeground"; }
echo "ok: the selected volume takes Mwm*activeForeground"
do_input key Escape
sc_quit
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

# mwc's frames' colours leave the client alone: its face and text stay mtk's.
printf 'Mwm*background: #ff0000\nMwm*foreground: #00ff00\n' >"$XDG_CONFIG_HOME/mwc/mwmrc"
MWC_INACTIVE='#0000ff' sc_start
wait_pixel "Mwm*background and MWC_INACTIVE leave the face" 1 1 "$BG"
status_has "and Mwm*foreground the text" 000000 32
sc_quit
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

# On a mid grey the volume's text has no room to be dimmer and still read: muted, it is
# in brackets instead, which the ink at the field's right end shows.
echo 'Mwm*basicBackground: #505050' >"$XDG_CONFIG_HOME/mwc/mwmrc"
sc_start
pactl set-sink-mute first 0
check "the volume is shown on a mid grey" '[ "$(field_ink)" -gt 0 ]'
ink_on=$(field_ink)
span=$(ink_span)
digits=$(span_ink $span)
pactl set-sink-mute first 1
muted_ok=
for ((i = 0; i < 30; i++)); do
	[ "$(field_ink)" != "$ink_on" ] && { muted_ok=1; break; }
	sleep 0.1
done
[ -n "$muted_ok" ] || { cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "on a mid grey a muted volume looks as an unmuted one (ink $ink_on)"; }
echo "ok: on a mid grey a muted volume looks different"
# The brackets go around the volume, which stays where it was: centred without them.
[ "$(span_ink $span)" = "$digits" ] || { cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "the volume moves when the brackets come: ink $(span_ink $span), not $digits, in x $span"; }
echo "ok: the brackets don't move the volume (x $span)"
pactl set-sink-mute first 0
sc_quit

# An error stays readable on a dark background. No server here.
echo 'Mwm*basicBackground: #202020' >"$XDG_CONFIG_HOME/mwc/mwmrc"
PULSE_SERVER=unix:$SMOKE_DIR/none sc_start
wait_pixel "Mwm*basicBackground is the background" 1 1 202020
status_has "the error has a light colour there" ffa0a0
sc_quit
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

# ---- a scaled output ---------------------------------------------------------

# At scale 2 it draws twice the pixels: the first one of the frame's face beside the
# bevel is the active colour, not a blend of the two from a buffer scaled up.
echo 'Mwm*HEADLESS-1*outputScale: 2' >"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled" "[ \"\$(global output_HEADLESS-1 scale)\" = 200 ]"
sc_start
sharp=
for ((i = 0; i < 30; i++)); do
	WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null &&
		sharp=$(build/pngtool pixel "$SHOT" $((2 * (WX + 2))) $((2 * (CY + ROW))))
	[ "$sharp" = "$SEL" ] && break
	sleep 0.1
done
[ "$sharp" = "$SEL" ] || { cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "at scale 2 the frame beside the bevel is $sharp, not $SEL (screenshot in $ERR_SHOT)"; }
echo "ok: sharp at scale 2"
sc_quit

# A cursor theme need not have the size asked for, and the cursor surface's scale must
# divide the image. Adwaita here has 24, 30, 36, 48, 72 and 96: 8 at scale 4 asks for
# 32 and gets 30, which 4 doesn't divide. Then a viewport shows the buffer at 30 / 4,
# about 8, as large as mwc shows its own cursor. The size is checked too: an enter
# before the scale of 4 arrives would pass with a smaller scale's cursor. vinput counts
# in the 1280x720 it was started with, four times the scaled layout.
cursor_ok() { # the cursor is shown, and its scale divides its size
	local w h f
	w=$(global cursor cw) h=$(global cursor ch) f=$(global cursor cscale)
	[ "${w:-0}" -gt 0 ] && [ "${f:-0}" -gt 0 ] && [ $((w % f)) = 0 ] && [ $((h % f)) = 0 ]
}
# The last size the cursor's viewport was given, "w h"; -1 -1 is none. From the
# protocol log of the instance started with WAYLAND_DEBUG.
cursor_dest() {
	grep -o 'wp_viewport#[0-9]*\.set_destination(-\?[0-9]*, -\?[0-9]*)' "$SC_LOG" |
		tail -1 | sed 's/.*(\(.*\), \(.*\))/\1 \2/'
}
# Cursor buffers destroyed since line $1 of the log: those of a theme that went. Window
# buffers don't count; an id the window takes up later is not the cursor's any more.
cursor_gone() {
	awk -v from="$1" '
	match($0, /wl_pointer#[0-9]+\.set_cursor\([0-9]+, wl_surface#[0-9]+/) {
		split(substr($0, RSTART, RLENGTH), f, /[#(, ]+/); cur = f[5]
	}
	match($0, /wl_surface#[0-9]+\.attach\(wl_buffer#[0-9]+/) {
		split(substr($0, RSTART, RLENGTH), f, /[#.()]+/)
		if (f[2] == cur) cb[f[5]] = 1
	}
	match($0, /wl_buffer#[0-9]+\.destroy\(\)/) {
		split(substr($0, RSTART, RLENGTH), f, /[#.()]+/)
		if (cb[f[2]] && NR >= from) n++
		delete cb[f[2]]
	}
	END { print n + 0 }' "$SC_LOG"
}
# Buffers destroyed while the cursor surface still showed them: a theme gone too early.
cursor_kept() {
	awk '
	match($0, /wl_pointer#[0-9]+\.set_cursor\([0-9]+, wl_surface#[0-9]+/) {
		split(substr($0, RSTART, RLENGTH), f, /[#(, ]+/); cur = f[5]
	}
	match($0, /wl_surface#[0-9]+\.attach\((wl_buffer#[0-9]+|nil)/) {
		split(substr($0, RSTART, RLENGTH), f, /[#.()]+/)
		att[f[2]] = f[4] == "nil" ? "" : f[5]
	}
	match($0, /wl_buffer#[0-9]+\.destroy\(\)/) {
		split(substr($0, RSTART, RLENGTH), f, /[#.()]+/)
		if (cur != "" && att[cur] == f[2]) print f[2]
	}' "$SC_LOG"
}
# How often the cursor surface was left empty since line $1 of the log: with the pointer
# on the window it never should be, or the cursor flickers. From the scale change on, as
# an empty surface at the start is right: the scale often comes before the first enter.
# The whole log is read, for which surface is the cursor's.
cursor_blank() {
	awk -v from="$1" '
	match($0, /wl_pointer#[0-9]+\.set_cursor\([0-9]+, wl_surface#[0-9]+/) {
		split(substr($0, RSTART, RLENGTH), f, /[#(, ]+/); cur = f[5]
	}
	match($0, /wl_surface#[0-9]+\.attach\(nil/) {
		split(substr($0, RSTART, RLENGTH), f, /[#.()]+/)
		if (NR >= from && f[2] == cur) n++
	}
	END { print n + 0 }' "$SC_LOG"
}
echo 'Mwm*HEADLESS-1*outputScale: 4' >"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled by 4" "[ \"\$(global output_HEADLESS-1 scale)\" = 400 ]"
: >"$SC_LOG"
XCURSOR_SIZE=8 WAYLAND_DEBUG=client sc_start
do_input move $((4 * (CX + W / 2))) $((4 * (CY + H / 2)))
check "a cursor of 30 at scale 4 is set at scale 1" \
	"cursor_ok && [ \"\$(global cursor cw)\" = 30 ] && [ \"\$(global cursor cscale)\" = 1 ]"
check "and the viewport shows it at 8" "[ \"\$(cursor_dest)\" = '8 8' ]"
echo "cursor $(global cursor cw)x$(global cursor ch)@$(global cursor cscale) at $(cursor_dest)"
# Back to 2 with the pointer kept on the window: the cursor follows without an enter.
FROM=$(($(wc -l <"$SC_LOG") + 1))
echo 'Mwm*HEADLESS-1*outputScale: 2' >"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "a new scale loads the cursor again at once" \
	"cursor_ok && [ \"\$(global cursor cw)\" = 24 ] && [ \"\$(global cursor cscale)\" = 2 ]"
check "there the scale divides it, and the viewport is unset" \
	"[ \"\$(cursor_dest)\" = '-1 -1' ]"
echo "cursor $(global cursor cw)x$(global cursor ch)@$(global cursor cscale) at $(cursor_dest)"
# The new theme is shown before the old one goes, so no buffer goes while shown.
[ -z "$(cursor_kept)" ] || fail "with the pointer on it, the cursor showed destroyed buffers"
echo "ok: with the pointer on the window no cursor buffer goes while shown"
[ "$(cursor_blank "$FROM")" = 0 ] || fail "with the pointer on it, the cursor was left empty"
echo "ok: and the cursor is never empty"
# The pointer leaves, the scale changes: the surface lets go of the old theme's buffer
# before that goes.
do_input move 1279 719
check "the pointer leaves the window" "grep -q 'wl_pointer#[0-9]*\.leave(' \"$SC_LOG\""
FROM=$(($(wc -l <"$SC_LOG") + 1))
echo 'Mwm*HEADLESS-1*outputScale: 4' >"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled by 4 again" "[ \"\$(global output_HEADLESS-1 scale)\" = 400 ]"
check "the old theme is gone" "[ \"\$(cursor_gone $FROM)\" -gt 0 ]"
[ -z "$(cursor_kept)" ] || fail "with the pointer away, the cursor showed destroyed buffers"
echo "ok: with the pointer away no cursor buffer goes while shown"
echo 'Mwm*HEADLESS-1*outputScale: 2' >"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled by 2 again" "[ \"\$(global output_HEADLESS-1 scale)\" = 200 ]"
sc_quit
# Only a whole number is a size: "32x" is none, and gives the default 24, as 24 does.
XCURSOR_SIZE=32x sc_start
do_input move $((2 * (CX + W / 2))) $((2 * (CY + H / 2)))
check "XCURSOR_SIZE=32x gives the default" \
	"cursor_ok && [ \"\$(global cursor cw)\" = 48 ] && [ \"\$(global cursor cscale)\" = 2 ]"
sc_quit
# A size past any theme is the largest there is, not an overflow. Still at scale 2.
XCURSOR_SIZE=1000000000 sc_start
do_input move $((2 * (CX + W / 2))) $((2 * (CY + H / 2)))
check "a huge XCURSOR_SIZE gives the largest cursor" \
	"cursor_ok && [ \"\$(global cursor cw)\" = 96 ]"
echo "cursor $(global cursor cw)x$(global cursor ch)@$(global cursor cscale)"
sc_quit
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

# Without DejaVu Sans fontconfig takes another font, and it says so.
mkdir -p "$SMOKE_DIR/fonts"
# fc-match always names some font: it must be Mono, not DejaVu Sans itself.
mono=$(fc-match -f '%{family[0]}|%{file}' 'DejaVu Sans Mono')
[ "${mono%%|*}" = "DejaVu Sans Mono" ] || fail "no DejaVu Sans Mono, fc-match gives $mono"
cp "${mono#*|}" "$SMOKE_DIR/fonts/" || fail "cannot copy ${mono#*|}"
cat >"$SMOKE_DIR/fonts.conf" <<EOF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
	<dir>$SMOKE_DIR/fonts</dir>
	<cachedir>$SMOKE_DIR/fontcache</cachedir>
</fontconfig>
EOF
: >"$SC_LOG"
FONTCONFIG_FILE=$SMOKE_DIR/fonts.conf sc_start
grep -q 'font DejaVu Sans not found' "$SC_LOG" ||
	fail "no warning without DejaVu Sans: $(cat "$SC_LOG")"
echo "ok: without DejaVu Sans it warns"
sc_quit

# ---- a layout without Latin letters ------------------------------------------------
# q and Ctrl+Q are the key that is Q in the first Latin layout, as Qt reads it.
stop_vinput
start_vinput 1280 720 ru,us
sc_start
sc_quit
sc_start
do_input key ctrl+q
sc_exits ctrl+q
check "Ctrl+Q closes the window" '[ -z "$(get pulseControl w)" ]'

trap cleanup EXIT
pa_stop
stop_vinput
stop_mwc
echo "PASS: ui"
