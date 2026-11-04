#!/bin/bash
# Black-box test of sc: a headless mwc with simulated mouse and keyboard, and a
# private PulseAudio server with two null sinks. Results are checked with pactl and
# in screenshots. It has its own sockets, and touches neither a running session nor
# the volume of the real sound card.
#
# Needs a built mwc with build/vinput and build/pngtool in MWC_DIR (by default
# ../mwc or ../mwm), grim, pactl and pulseaudio (PA_MODULES names its module directory,
# if it isn't in the usual place). Output goes to build/ui.log, sc's own to build/ui-sc.log.
SC_DIR=$(cd "$(dirname "$0")/.." && pwd)
SC=$SC_DIR/sc
LOG=$SC_DIR/build/ui.log
SC_LOG=$SC_DIR/build/ui-sc.log
if [ -z "${MWC_DIR:-}" ]; then
	MWC_DIR=$SC_DIR/../mwc
	[ -d "$MWC_DIR" ] || MWC_DIR=$SC_DIR/../mwm
fi
[ -x "$SC" ] || { echo "FAIL: $SC not built"; exit 1; }
[ -d "$MWC_DIR" ] || { echo "FAIL: no mwc in $MWC_DIR (set MWC_DIR)"; exit 1; }
cd "$MWC_DIR" || exit 1
. tests/lib.sh
require_tools grim pactl pulseaudio

export MWC_TEST_STATE=$SMOKE_DIR/state
export LC_ALL=C.UTF-8
export HOME=$SMOKE_DIR/home XDG_CONFIG_HOME=$SMOKE_DIR/home/.config
export PULSE_SERVER=unix:$SMOKE_DIR/pulse.sock
export DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent
unset MWC_INACTIVE MWC_ACTIVE SC_FONT
mkdir -p "$HOME"
: >"$SC_LOG"
SHOT=$SMOKE_DIR/ui.png
SEL=7387ad
TROUGH=999dac
ERR_SHOT=$SC_DIR/build/ui-last.png

pa_start() {
	pulseaudio -n --daemonize=no --exit-idle-time=-1 --disable-shm=yes \
		${PA_MODULES:+--dl-search-path="$PA_MODULES"} \
		-L "module-null-sink sink_name=first sink_properties=device.description=First" \
		-L "module-null-sink sink_name=second sink_properties=device.description=Second" \
		-L "module-native-protocol-unix socket=$SMOKE_DIR/pulse.sock auth-anonymous=1" \
		>>"$SC_LOG" 2>&1 &
	PA_PID=$!
	wait_for "the sound server is up" pactl info
	pactl set-default-sink first
}

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
	check "sc has a focused window" '[ "$(get sc kbfocus)" = 1 ]'
	CX=$(($(get sc x) + B))
	CY=$(($(get sc y) + T + B))
	W=$(get sc w)
	H=$(get sc h)
	BARH=$(((H - 16) / 2))
	# In the window: the scale's inner x range and a row through it, below the
	# middle so that the slider's groove doesn't matter.
	S0=$((6 + BARH + 4 + 2))
	S1=$(scale_end)
	ROW=$((6 + BARH / 2 + 2))
	echo "window ${W}x$H, scale x $S0..$S1"
}

# Where the inside of the scale ends, in the window. The percent text beside it depends
# on the font, so walk in from the right along the scale's top bevel, above the text.
scale_end() {
	local x y=$((CY + 6)) shot=$SMOKE_DIR/w.png
	WAYLAND_DISPLAY=wayland-0 grim "$shot" 2>/dev/null || fail "no screenshot"
	for ((x = CX + W - 1; x > CX; x--)); do
		[ "$(build/pngtool pixel "$shot" "$x" "$y")" != aeb2c3 ] && break
	done
	echo $((x - CX - 1))
}

# Where a percentage puts the middle of the slider, in the window.
thumb_mid() { echo $((S0 + (S1 - S0 - 30) * $1 / 100 + 15)); }

trap 'WAYLAND_DISPLAY=wayland-0 grim "$ERR_SHOT" 2>/dev/null; cleanup' EXIT

start_mwc 'touch "$SMOKE_DIR/up"; exec sleep 600'
wait_for "mwc is up" test -e "$SMOKE_DIR/up"
start_vinput
refresh
B=$(global theme border)
T=$(global theme title)
pa_start
pactl set-sink-volume first 50%
pactl set-sink-volume second 10%

sc_start
check "mwc gave it a frame" '[ "$(get sc frame)" = 1 ]'
wait_pixel "the scale is filled up to the slider" $((S0 + 2)) "$ROW" "$SEL"
wait_pixel "and empty behind it" $((S1 - 3)) "$ROW" "$TROUGH"

# ---- keyboard ----------------------------------------------------------------

do_input key Up
expect_vol "Up is louder" first 55 55
do_input key Right
expect_vol "Right is louder" first 60 60
do_input key Down
do_input key Left
expect_vol "Down and Left are quieter" first 50 50
do_input key m
expect_mute "m mutes" first yes
wait_pixel "a muted scale has no fill" $((S0 + 2)) "$ROW" "$TROUGH"
do_input key space
expect_mute "Space unmutes" first no
wait_pixel "the fill is back" $((S0 + 2)) "$ROW" "$SEL"

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
do_input up left
sleep 0.2
expect_vol "the release leaves it there" first $held

# A press on the slider without moving changes nothing, wherever on it.
pactl set-sink-volume first 40%
expect_vol "changed by another program" first 40 40
do_input move $((CX + $(thumb_mid 40) + 12)) $((CY + ROW))
do_input click left
sleep 0.2
expect_vol "a click on the slider leaves the volume alone" first 40 40

# ---- what others do ----------------------------------------------------------

pactl set-sink-volume first 90%
wait_pixel "the scale follows another program" $(($(thumb_mid 90) - 20)) "$ROW" "$SEL"
pactl set-sink-mute first 1
wait_pixel "and its muting" $((S0 + 2)) "$ROW" "$TROUGH"
pactl set-sink-mute first 0

# The balance survives steps, and a volume of zero in a drag.
pactl set-sink-volume first 80% 40%
do_input move "$BX" $((CY + 6 + BARH / 2))
sleep 0.2
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
sleep 0.2
do_input key Up
sleep 0.2
expect_vol "Up doesn't raise a volume above 100 %" first 130 130
do_input key Down
expect_vol "Down lowers it" first 125 125
pactl set-sink-volume first 98%
sleep 0.2
do_input key Up
expect_vol "Up stops at 100 %" first 100 100
pactl set-sink-volume first 3%
sleep 0.2
do_input key Down
expect_vol "Down stops at 0" first 0 0

# Steps quicker than the server answers build on each other.
pactl set-sink-volume first 5%
sleep 0.2
for _ in 1 2 3 4 5 6 7 8; do do_input key Up; done
expect_vol "quick steps add up" first 45 45

# ---- the default output changes ----------------------------------------------

pactl set-default-sink second
wait_pixel "the scale shows the new default" $((S0 + 2)) "$ROW" "$SEL"
sleep 0.2
do_input key Up
expect_vol "a step goes to the new default" second 15 15
expect_vol "and not to the old one" first 45 45
pactl set-default-sink first
sleep 0.2

# ---- the server goes away and comes back -------------------------------------

pa_stop
wait_pixel "without a server the scale is empty" $((S0 + 2)) "$ROW" "$TROUGH"
do_input key Up         # must not crash
do_input move $((CX + $(thumb_mid 50))) $((CY + ROW))
do_input click left
pa_start
pactl set-sink-volume first 30%
wait_pixel "with a server back it comes back" $((S0 + 2)) "$ROW" "$SEL"
sleep 0.3
do_input key Up
expect_vol "and works" first 35 35

# ---- quitting ----------------------------------------------------------------

for combo in Escape q ctrl+w; do
	do_input key "$combo"
	wait "$SC_PID"
	status=$?
	[ "$status" = 0 ] || fail "$combo: sc exited with status $status"
	check "$combo closes the window" '[ -z "$(get sc w)" ]'
	[ "$combo" = ctrl+w ] || sc_start
done

trap cleanup EXIT
pa_stop
stop_vinput
stop_mwc
echo "PASS: ui"
